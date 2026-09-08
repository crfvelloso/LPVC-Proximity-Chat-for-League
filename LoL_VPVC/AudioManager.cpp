#define MINIAUDIO_IMPLEMENTATION
#include "AudioManager.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "rnnoise.h"

namespace {

// Limitador suave: mantem o volume alto sem o estalo do corte duro (hard clip).
inline float SoftClip(float x) {
    const float knee = 22000.0f;
    const float head = 32767.0f - knee;
    if (x > knee)  return knee + head * std::tanh((x - knee) / head);
    if (x < -knee) return -knee - head * std::tanh((-x - knee) / head);
    return x;
}

inline float Rms(const int16_t* p, size_t n) {
    if (n == 0) return 0.0f;
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) acc += (double)p[i] * (double)p[i];
    return (float)std::sqrt(acc / (double)n);
}

constexpr int kPrebufferFrames = 2;    // 40 ms antes de comecar a tocar um peer
constexpr int kMaxJitterFrames = 12;   // 240 ms de teto (evita atraso acumulado)

} // namespace

AudioManager::AudioManager() {
    mixBuf_.resize(kMixChunk, 0.0f);
    tmpBuf_.resize(kMixChunk, 0);
    frameBuf_.resize(kFrameSize, 0);
    echoBuf_.resize(kFrameSize, 0);
    rnnIn_.resize(kRnnFrame, 0.0f);
    rnnOut_.resize(kRnnFrame, 0.0f);
    lastOutput_.resize(kMixChunk, 0);
    CreateDsp();
}

AudioManager::~AudioManager() {
    Shutdown();
    ClearPeers();
    DestroyDsp();
}

// ---------------------------------------------------------------------------
// DSP
// ---------------------------------------------------------------------------
bool AudioManager::CreateDsp() {
    std::lock_guard<std::mutex> lock(dspMutex_);
    int err = OPUS_OK;
    encoder_ = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !encoder_) {
        lastError_ = "Falha ao criar o encoder Opus";
        return false;
    }
    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(32000));
    opus_encoder_ctl(encoder_, OPUS_SET_VBR(1));
    opus_encoder_ctl(encoder_, OPUS_SET_VBR_CONSTRAINT(0));
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(6));       // qualidade/CPU equilibrados
    opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(1));       // recuperacao de perda
    opus_encoder_ctl(encoder_, OPUS_SET_PACKET_LOSS_PERC(10));
    opus_encoder_ctl(encoder_, OPUS_SET_LSB_DEPTH(16));
    opus_encoder_ctl(encoder_, OPUS_SET_DTX(0));

    rnn_ = rnnoise_create(nullptr);
    preprocess_ = speex_preprocess_state_init(kFrameSize, kSampleRate);
    echo_ = speex_echo_state_init(kFrameSize, kSampleRate / 10);  // cauda de 100 ms
    if (echo_) {
        int rate = kSampleRate;
        speex_echo_ctl(echo_, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    }
    dspDirty_.store(true);
    return true;
}

void AudioManager::DestroyDsp() {
    std::lock_guard<std::mutex> lock(dspMutex_);
    if (encoder_)    { opus_encoder_destroy(encoder_); encoder_ = nullptr; }
    if (rnn_)        { rnnoise_destroy(rnn_); rnn_ = nullptr; }
    if (preprocess_) { speex_preprocess_state_destroy(preprocess_); preprocess_ = nullptr; }
    if (echo_)       { speex_echo_state_destroy(echo_); echo_ = nullptr; }
}

// Chamado pela thread de captura quando algum ajuste da UI muda.
void AudioManager::ApplyPreprocessSettings() {
    if (!preprocess_) return;

    // O denoise fica por conta do rnnoise (rede neural), que preserva muito
    // mais a voz que o filtro espectral do Speex.
    spx_int32_t denoiseOff = 0;
    speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_SET_DENOISE, &denoiseOff);

    // AGC: o motivo numero 1 de microfone baixo demais. Normaliza a voz para um
    // alvo fixo, independente do ganho que o driver do Windows entrega.
    spx_int32_t agcOn = agc_.load() ? 1 : 0;
    speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_SET_AGC, &agcOn);
    spx_int32_t target = agcTarget_.load();
    speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_SET_AGC_TARGET, &target);
    spx_int32_t maxGain = 32;      // ate +32 dB para microfones fracos
    speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_SET_AGC_MAX_GAIN, &maxGain);
    spx_int32_t inc = 12, dec = -40;
    speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_SET_AGC_INCREMENT, &inc);
    speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_SET_AGC_DECREMENT, &dec);

    spx_int32_t vadOn = vad_.load() ? 1 : 0;
    speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_SET_VAD, &vadOn);
    // Histerese: exige 80% para abrir, mas segura em 55% para nao cortar palavras.
    spx_int32_t probStart = 80, probContinue = 55;
    speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_SET_PROB_START, &probStart);
    speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_SET_PROB_CONTINUE, &probContinue);

    spx_int32_t dereverb = 0;
    speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_SET_DEREVERB, &dereverb);

    SpeexEchoState* linked = echoCancel_.load() ? echo_ : nullptr;
    speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_SET_ECHO_STATE, linked);
    if (linked) {
        spx_int32_t echoSuppress = -40, echoSuppressActive = -15;
        speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &echoSuppress);
        speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &echoSuppressActive);
    }
}

// ---------------------------------------------------------------------------
// Dispositivos
// ---------------------------------------------------------------------------
std::vector<AudioDeviceInfo> AudioManager::ListDevices(bool capture) {
    std::vector<AudioDeviceInfo> result;
    ma_context ctx;
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) return result;

    ma_device_info* playbackInfos = nullptr; ma_uint32 playbackCount = 0;
    ma_device_info* captureInfos  = nullptr; ma_uint32 captureCount  = 0;
    if (ma_context_get_devices(&ctx, &playbackInfos, &playbackCount, &captureInfos, &captureCount) == MA_SUCCESS) {
        ma_device_info* list = capture ? captureInfos : playbackInfos;
        ma_uint32 count = capture ? captureCount : playbackCount;
        result.reserve(count);
        for (ma_uint32 i = 0; i < count; ++i) {
            AudioDeviceInfo info;
            info.name = list[i].name;
            info.id = list[i].id;
            info.isDefault = (list[i].isDefault != 0);
            result.push_back(info);
        }
    }
    ma_context_uninit(&ctx);
    return result;
}

bool AudioManager::Init(const ma_device_id* captureId, const ma_device_id* playbackId) {
    if (isInitialized_) return true;
    if (!encoder_ && !CreateDsp()) return false;

    // ~1 s de folga: o suficiente para absorver qualquer travada do sistema.
    {
        std::lock_guard<std::mutex> lock(captureMutex_);
        captureRing_.Resize(kSampleRate);
        echoRing_.Resize(kSampleRate);
    }

    ma_device_config config = ma_device_config_init(ma_device_type_duplex);
    config.capture.format        = ma_format_s16;
    config.capture.channels      = 1;
    config.capture.shareMode     = ma_share_mode_shared;
    config.capture.pDeviceID     = captureId;
    config.playback.format       = ma_format_s16;
    config.playback.channels     = 1;
    config.playback.pDeviceID    = playbackId;
    config.sampleRate            = kSampleRate;
    config.dataCallback          = DataCallback;
    config.pUserData             = this;
    config.periodSizeInFrames    = 480;      // 10 ms -> baixa latencia
    config.periods               = 3;
    config.performanceProfile    = ma_performance_profile_low_latency;

    if (ma_device_init(NULL, &config, &device_) != MA_SUCCESS) {
        lastError_ = "Nao foi possivel abrir os dispositivos de audio";
        return false;
    }

    workerRunning_.store(true);
    worker_ = std::thread(&AudioManager::CaptureWorker, this);

    if (ma_device_start(&device_) != MA_SUCCESS) {
        workerRunning_.store(false);
        captureCv_.notify_all();
        if (worker_.joinable()) worker_.join();
        ma_device_uninit(&device_);
        lastError_ = "Nao foi possivel iniciar o dispositivo de audio";
        return false;
    }

    isInitialized_ = true;
    lastError_.clear();
    return true;
}

bool AudioManager::Restart(const ma_device_id* captureId, const ma_device_id* playbackId) {
    Shutdown();
    ClearPeers();
    return Init(captureId, playbackId);
}

void AudioManager::Shutdown() {
    if (!isInitialized_) return;
    isInitialized_ = false;

    ma_device_stop(&device_);
    workerRunning_.store(false);
    captureCv_.notify_all();
    if (worker_.joinable()) worker_.join();
    ma_device_uninit(&device_);

    std::lock_guard<std::mutex> lock(captureMutex_);
    captureRing_.Clear();
    echoRing_.Clear();
}

// ---------------------------------------------------------------------------
// Ajustes
// ---------------------------------------------------------------------------
void AudioManager::SetInputGain(float g)   { inputGain_.store(std::clamp(g, 0.0f, 16.0f)); }
void AudioManager::SetOutputGain(float g)  { outputGain_.store(std::clamp(g, 0.0f, 8.0f)); }

// Presets de supressao de ruido. Cada nivel ajusta de uma vez o quanto do sinal
// limpo entra na mistura, o limiar de voz do rnnoise e a porta de ruido.
void AudioManager::SetNoiseSuppression(int level) {
    level = std::clamp(level, (int)kNoiseOff, (int)kNoiseHigh);
    nsLevel_.store(level);
    switch (level) {
        case kNoiseOff:                                    // sem processamento
            rnnMix_.store(0.0f);  voiceProbGate_.store(0.0f); noiseGate_.store(60.0f);  break;
        case kNoiseLow:                                    // tira o chiado de fundo
            rnnMix_.store(0.60f); voiceProbGate_.store(0.30f); noiseGate_.store(80.0f);  break;
        case kNoiseMedium:                                 // teclado, ventilador
            rnnMix_.store(0.85f); voiceProbGate_.store(0.50f); noiseGate_.store(110.0f); break;
        case kNoiseHigh:                                   // ambiente barulhento
        default:
            rnnMix_.store(1.00f); voiceProbGate_.store(0.75f); noiseGate_.store(160.0f); break;
    }
    dspDirty_.store(true);
}
void AudioManager::SetAgc(bool on)         { agc_.store(on); dspDirty_.store(true); }
void AudioManager::SetAgcTarget(int level) { agcTarget_.store(std::clamp(level, 1000, 32000)); dspDirty_.store(true); }
void AudioManager::SetEchoCancel(bool on)  { echoCancel_.store(on); dspDirty_.store(true); }
void AudioManager::SetVad(bool on)         { vad_.store(on); dspDirty_.store(true); }

void AudioManager::SetBitrate(int bps) {
    std::lock_guard<std::mutex> lock(dspMutex_);
    if (encoder_) opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(std::clamp(bps, 8000, 128000)));
}

// ---------------------------------------------------------------------------
// Peers
// ---------------------------------------------------------------------------
void AudioManager::SetPeerGain(uint32_t peerId, float gain) {
    std::lock_guard<std::mutex> lock(peersMutex_);
    auto it = peers_.find(peerId);
    if (it != peers_.end()) it->second->gain.store(std::clamp(gain, 0.0f, 4.0f));
}

void AudioManager::RemovePeer(uint32_t peerId) {
    std::lock_guard<std::mutex> lock(peersMutex_);
    auto it = peers_.find(peerId);
    if (it != peers_.end()) {
        if (it->second->dec) opus_decoder_destroy(it->second->dec);
        peers_.erase(it);
    }
}

void AudioManager::ClearPeers() {
    std::lock_guard<std::mutex> lock(peersMutex_);
    for (auto& kv : peers_) if (kv.second->dec) opus_decoder_destroy(kv.second->dec);
    peers_.clear();
}

float AudioManager::GetPeerLevel(uint32_t peerId) {
    std::lock_guard<std::mutex> lock(peersMutex_);
    auto it = peers_.find(peerId);
    return (it != peers_.end()) ? it->second->level.load() : 0.0f;
}

void AudioManager::PushPacket(uint32_t peerId, uint32_t seq, const uint8_t* data, size_t len) {
    if (!data || len == 0 || deafened_.load()) return;

    std::lock_guard<std::mutex> lock(peersMutex_);
    auto it = peers_.find(peerId);
    if (it == peers_.end()) {
        int err = OPUS_OK;
        auto peer = std::make_unique<Peer>();
        peer->dec = opus_decoder_create(kSampleRate, 1, &err);
        if (err != OPUS_OK || !peer->dec) return;
        peer->pcm.Resize((size_t)kFrameSize * (kMaxJitterFrames + 4));
        it = peers_.emplace(peerId, std::move(peer)).first;
    }
    Peer& peer = *it->second;

    int16_t pcm[kFrameSize];

    // Perda de pacote: um buraco de 1 frame e reconstruido pelo FEC embutido no
    // pacote atual; buracos maiores caem na interpolacao (PLC) do proprio Opus.
    if (peer.hasSeq) {
        const uint32_t gap = seq - peer.lastSeq;
        if (gap == 0 || gap > 1000000u) return;   // duplicado ou fora de ordem
        if (gap == 2) {
            int n = opus_decode(peer.dec, data, (opus_int32)len, pcm, kFrameSize, 1);
            if (n > 0) peer.pcm.Write(pcm, (size_t)n);
        } else if (gap > 2 && gap < 32) {
            for (uint32_t i = 1; i < gap && i < 4; ++i) {
                int n = opus_decode(peer.dec, nullptr, 0, pcm, kFrameSize, 0);
                if (n > 0) peer.pcm.Write(pcm, (size_t)n);
            }
        }
    }
    peer.lastSeq = seq;
    peer.hasSeq = true;

    int decoded = opus_decode(peer.dec, data, (opus_int32)len, pcm, kFrameSize, 0);
    if (decoded > 0) peer.pcm.Write(pcm, (size_t)decoded);
}

// ---------------------------------------------------------------------------
// Callback do dispositivo (thread de audio: nada de alocar ou bloquear aqui)
// ---------------------------------------------------------------------------
void AudioManager::DataCallback(ma_device* dev, void* out, const void* in, ma_uint32 frameCount) {
    AudioManager* self = (AudioManager*)dev->pUserData;
    if (!self) return;
    if (out) self->OnPlayback((int16_t*)out, frameCount);
    if (in)  self->OnCapture((const int16_t*)in, frameCount);
}

void AudioManager::OnCapture(const int16_t* in, ma_uint32 frames) {
    rawLevel_.store(std::min(1.0f, Rms(in, frames) / 8000.0f));

    std::unique_lock<std::mutex> lock(captureMutex_);
    captureRing_.Write(in, frames);
    // Referencia do cancelador de eco: exatamente o que acabamos de tocar.
    echoRing_.Write(lastOutput_.data(), std::min<size_t>(frames, lastOutputFrames_));
    const bool ready = captureRing_.Size() >= (size_t)kFrameSize;
    lock.unlock();
    if (ready) captureCv_.notify_one();
}

void AudioManager::OnPlayback(int16_t* out, ma_uint32 frames) {
    const float master = deafened_.load() ? 0.0f : outputGain_.load();
    ma_uint32 done = 0;
    float peak = 0.0f;
    size_t deepest = 0;

    while (done < frames) {
        const ma_uint32 chunk = std::min<ma_uint32>(frames - done, (ma_uint32)kMixChunk);
        std::fill(mixBuf_.begin(), mixBuf_.begin() + chunk, 0.0f);

        {
            std::lock_guard<std::mutex> lock(peersMutex_);
            for (auto& kv : peers_) {
                Peer& peer = *kv.second;
                deepest = std::max(deepest, peer.pcm.Size());

                // Teto de atraso: se acumulou demais (rede engasgou), pula na frente.
                if (peer.pcm.Size() > (size_t)(kFrameSize * kMaxJitterFrames))
                    peer.pcm.Skip(peer.pcm.Size() - (size_t)(kFrameSize * kPrebufferFrames));

                if (!peer.playing) {
                    if (peer.pcm.Size() >= (size_t)(kFrameSize * kPrebufferFrames)) peer.playing = true;
                    else { peer.level.store(peer.level.load() * 0.8f); continue; }
                }

                const size_t got = peer.pcm.Read(tmpBuf_.data(), chunk);
                if (got < chunk) peer.playing = false;   // underrun: reacumula
                if (got == 0) { peer.level.store(peer.level.load() * 0.8f); continue; }

                const float g = peer.gain.load();
                float acc = 0.0f;
                for (size_t i = 0; i < got; ++i) {
                    const float s = (float)tmpBuf_[i];
                    acc += s * s;
                    mixBuf_[i] += s * g;
                }
                const float rms = std::sqrt(acc / (float)got) / 8000.0f;
                const float prev = peer.level.load();
                peer.level.store(rms > prev ? std::min(1.0f, rms) : prev * 0.85f);
            }
        }

        int16_t* dst = out + done;
        for (ma_uint32 i = 0; i < chunk; ++i) {
            const float v = SoftClip(mixBuf_[i] * master);
            peak = std::max(peak, std::fabs(v));
            dst[i] = (int16_t)v;
        }
        done += chunk;
    }

    // Guarda a saida como referencia de eco para o proximo bloco de captura.
    const size_t keep = std::min<size_t>(frames, lastOutput_.size());
    if (keep > 0) std::memcpy(lastOutput_.data(), out + (frames - keep), keep * sizeof(int16_t));
    lastOutputFrames_ = keep;

    outputLevel_.store(std::min(1.0f, peak / 20000.0f));
    latencyMs_.store((uint32_t)(deepest * 1000 / kSampleRate));
}

// ---------------------------------------------------------------------------
// Thread de processamento de voz (fora do callback de audio)
// ---------------------------------------------------------------------------
void AudioManager::CaptureWorker() {
    std::vector<uint8_t> packet(kMaxPacket);
    std::vector<int16_t> aec(kFrameSize);

    while (workerRunning_.load()) {
        {
            std::unique_lock<std::mutex> lock(captureMutex_);
            captureCv_.wait(lock, [this] {
                return !workerRunning_.load() || captureRing_.Size() >= (size_t)kFrameSize;
            });
            if (!workerRunning_.load()) break;
            captureRing_.Read(frameBuf_.data(), kFrameSize);
            if (echoRing_.Size() >= (size_t)kFrameSize) echoRing_.Read(echoBuf_.data(), kFrameSize);
            else std::fill(echoBuf_.begin(), echoBuf_.end(), (int16_t)0);
        }

        // 1) Ganho de entrada (com limitador suave, para nao distorcer).
        const float gain = inputGain_.load();
        if (gain != 1.0f) {
            for (int i = 0; i < kFrameSize; ++i)
                frameBuf_[i] = (int16_t)SoftClip((float)frameBuf_[i] * gain);
        }

        int isSpeech = 1;
        {
            std::lock_guard<std::mutex> lock(dspMutex_);
            if (dspDirty_.exchange(false)) ApplyPreprocessSettings();

            // 2) Cancelamento de eco (evita captar o audio dos outros na caixa).
            if (echo_ && echoCancel_.load()) {
                speex_echo_cancellation(echo_, frameBuf_.data(), echoBuf_.data(), aec.data());
                std::memcpy(frameBuf_.data(), aec.data(), (size_t)kFrameSize * sizeof(int16_t));
            }

            // 3) Supressao de ruido neural (rnnoise), em blocos de 10 ms.
            //    A saida e misturada com o sinal original conforme o preset:
            //    "Baixa" deixa passar um pouco do original e soa mais natural.
            const float mix = rnnMix_.load();
            if (rnn_ && mix > 0.0f) {
                float prob = 0.0f;
                for (int block = 0; block < kFrameSize / kRnnFrame; ++block) {
                    int16_t* chunk = frameBuf_.data() + block * kRnnFrame;
                    for (int i = 0; i < kRnnFrame; ++i) rnnIn_[i] = (float)chunk[i];
                    prob += rnnoise_process_frame(rnn_, rnnOut_.data(), rnnIn_.data());
                    for (int i = 0; i < kRnnFrame; ++i)
                        chunk[i] = (int16_t)SoftClip(rnnOut_[i] * mix + rnnIn_[i] * (1.0f - mix));
                }
                voiceProb_.store(prob / (float)(kFrameSize / kRnnFrame));
            } else {
                voiceProb_.store(0.0f);
            }

            // 4) AGC + VAD do Speex (o denoise dele fica desligado).
            if (preprocess_) {
                isSpeech = speex_preprocess_run(preprocess_, frameBuf_.data());
                spx_int32_t g = 100;
                speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_GET_AGC_GAIN, &g);
                agcGain_.store((int)g);
            }
        }

        const float rms = Rms(frameBuf_.data(), kFrameSize);
        inputLevel_.store(std::min(1.0f, rms / 8000.0f));

        // 5) Porta de ruido com hangover (nao corta o fim das frases). Com o
        //    rnnoise ligado quem decide se e voz e a rede neural.
        //    Os dois detectores precisam concordar: a rede neural (que ja viu o
        //    sinal limpo) e o VAD do Speex. Sozinha, a rede abre demais em ruido.
        const bool speexSaysVoice = (!vad_.load() || isSpeech);
        const bool speechDetected = (rnnMix_.load() > 0.0f)
                                        ? (voiceProb_.load() >= voiceProbGate_.load() && speexSaysVoice)
                                        : speexSaysVoice;
        const bool voice = speechDetected && rms > noiseGate_.load();
        if (voice) consecutiveVoiceFrames_++;
        else       consecutiveVoiceFrames_ = 0;

        bool transmit = false;
        if (consecutiveVoiceFrames_ >= 2) { hangoverFrames_ = 20; transmit = true; }
        else if (hangoverFrames_ > 0)     { hangoverFrames_--;    transmit = true; }
        transmitting_.store(transmit);

        if (!transmit || muted_.load() || !onAudioCaptured_) continue;

        int bytes = 0;
        {
            std::lock_guard<std::mutex> lock(dspMutex_);
            if (!encoder_) continue;
            bytes = opus_encode(encoder_, frameBuf_.data(), kFrameSize, packet.data(), (opus_int32)packet.size());
        }
        if (bytes > 1) {
            std::vector<uint8_t> outPacket(packet.begin(), packet.begin() + bytes);
            const uint32_t seq = localSeq_.fetch_add(1);
            onAudioCaptured_(outPacket);
            if (loopback_.load()) PushPacket(kLoopbackPeerId, seq, outPacket.data(), outPacket.size());
        }
    }
}
