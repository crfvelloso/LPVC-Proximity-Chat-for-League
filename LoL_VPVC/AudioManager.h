#pragma once
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cstdint>
#include "miniaudio.h"
#include <opus.h>
#include <speex/speex_preprocess.h>
#include <speex/speex_echo.h>

struct DenoiseState;   // rnnoise (supressao de ruido por rede neural)

// ---------------------------------------------------------------------------
// Buffer circular contiguo, sem alocacao durante a reproducao.
// A protecao e feita por mutex externo. Ao encher, descarta o audio mais antigo.
// ---------------------------------------------------------------------------
class RingBufferS16 {
public:
    void Resize(size_t capacity) {
        buf_.assign(capacity, 0);
        head_ = tail_ = count_ = 0;
    }
    size_t Size() const { return count_; }
    size_t Capacity() const { return buf_.size(); }
    void Clear() { head_ = tail_ = count_ = 0; }

    void Write(const int16_t* src, size_t n) {
        if (buf_.empty() || n == 0) return;
        if (n >= buf_.size()) {            // pacote maior que o buffer inteiro
            src += (n - buf_.size());
            n = buf_.size();
        }
        if (count_ + n > buf_.size()) Skip(count_ + n - buf_.size());
        for (size_t i = 0; i < n; ++i) {
            buf_[tail_] = src[i];
            tail_ = (tail_ + 1 == buf_.size()) ? 0 : tail_ + 1;
        }
        count_ += n;
    }

    size_t Read(int16_t* dst, size_t n) {
        const size_t got = (n < count_) ? n : count_;
        for (size_t i = 0; i < got; ++i) {
            dst[i] = buf_[head_];
            head_ = (head_ + 1 == buf_.size()) ? 0 : head_ + 1;
        }
        count_ -= got;
        return got;
    }

    void Skip(size_t n) {
        if (buf_.empty()) return;
        const size_t s = (n < count_) ? n : count_;
        head_ = (head_ + s) % buf_.size();
        count_ -= s;
    }

private:
    std::vector<int16_t> buf_;
    size_t head_ = 0, tail_ = 0, count_ = 0;
};

struct AudioDeviceInfo {
    std::string name;
    ma_device_id id;
    bool isDefault = false;
};

class AudioManager {
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kFrameSize  = 960;    // 20 ms @ 48 kHz
    static constexpr int kRnnFrame   = 480;    // o rnnoise trabalha em blocos de 10 ms
    static constexpr int kMaxPacket  = 1275;   // maior pacote Opus possivel
    static constexpr int kMixChunk   = 2048;   // maximo de frames por passada de mixagem
    static constexpr uint32_t kLoopbackPeerId = 0xFFFFFFFFu;

    // Presets de supressao de ruido apresentados na interface.
    enum NoiseSuppression { kNoiseOff = 0, kNoiseLow = 1, kNoiseMedium = 2, kNoiseHigh = 3 };

    AudioManager();
    ~AudioManager();
    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    // ---- ciclo de vida -----------------------------------------------------
    bool Init(const ma_device_id* captureId = nullptr, const ma_device_id* playbackId = nullptr);
    bool Restart(const ma_device_id* captureId, const ma_device_id* playbackId);
    void Shutdown();
    bool IsRunning() const { return isInitialized_; }
    const std::string& LastError() const { return lastError_; }

    static std::vector<AudioDeviceInfo> ListDevices(bool capture);

    // ---- controles ---------------------------------------------------------
    void SetInputGain(float linear);        // ganho de microfone (antes do DSP)
    void SetOutputGain(float linear);       // volume mestre de saida
    // Controle unico de ruido: Desligado / Baixa / Media / Alta.
    void SetNoiseSuppression(int level);
    int  GetNoiseSuppression() const { return nsLevel_.load(); }
    float GetVoiceProbability() const { return voiceProb_.load(); }
    void SetAgc(bool on);
    void SetAgcTarget(int level);           // 0..32768 (padrao 24000)
    void SetEchoCancel(bool on);
    void SetVad(bool on);
    void SetMuted(bool m)    { muted_.store(m); }
    void SetDeafened(bool d) { deafened_.store(d); }
    void SetLoopback(bool l) { loopback_.store(l); }
    void SetBitrate(int bps);

    bool GetAgc() const         { return agc_.load(); }
    bool GetEchoCancel() const  { return echoCancel_.load(); }
    bool GetVad() const         { return vad_.load(); }

    // ---- pares (peers) -----------------------------------------------------
    void SetPeerGain(uint32_t peerId, float gain);
    void RemovePeer(uint32_t peerId);
    void ClearPeers();
    void PushPacket(uint32_t peerId, uint32_t seq, const uint8_t* data, size_t len);

    // ---- telemetria para a UI ---------------------------------------------
    float GetInputLevel()  const { return inputLevel_.load(); }   // 0..1 pos-DSP
    float GetRawInputLevel() const { return rawLevel_.load(); }   // 0..1 cru, do driver
    float GetOutputLevel() const { return outputLevel_.load(); }
    float GetPeerLevel(uint32_t peerId);
    bool  IsTransmitting() const { return transmitting_.load(); }
    int   GetAgcGainPercent() const { return agcGain_.load(); }
    uint32_t GetLatencyMs() const { return latencyMs_.load(); }

    void SetNetworkSendCallback(std::function<void(const std::vector<uint8_t>&)> cb) {
        onAudioCaptured_ = std::move(cb);
    }

private:
    struct Peer {
        OpusDecoder* dec = nullptr;
        RingBufferS16 pcm;
        std::atomic<float> gain{1.0f};
        std::atomic<float> level{0.0f};
        uint32_t lastSeq = 0;
        bool hasSeq = false;
        bool playing = false;   // false enquanto acumula o pre-buffer
    };

    static void DataCallback(ma_device* dev, void* out, const void* in, ma_uint32 frameCount);
    void OnCapture(const int16_t* in, ma_uint32 frames);
    void OnPlayback(int16_t* out, ma_uint32 frames);
    void CaptureWorker();
    void ApplyPreprocessSettings();
    void DestroyDsp();
    bool CreateDsp();

    ma_device device_{};
    bool isInitialized_ = false;
    std::string lastError_;

    // filas entre o callback do device e a thread de processamento
    RingBufferS16 captureRing_;
    RingBufferS16 echoRing_;
    std::mutex captureMutex_;
    std::condition_variable captureCv_;
    std::thread worker_;
    std::atomic<bool> workerRunning_{false};

    std::mutex peersMutex_;
    std::unordered_map<uint32_t, std::unique_ptr<Peer>> peers_;

    // buffers pre-alocados (nada de malloc dentro do callback de audio)
    std::vector<float>   mixBuf_;
    std::vector<int16_t> tmpBuf_;
    std::vector<int16_t> frameBuf_;
    std::vector<int16_t> echoBuf_;
    std::vector<int16_t> lastOutput_;
    size_t lastOutputFrames_ = 0;

    OpusEncoder* encoder_ = nullptr;
    DenoiseState* rnn_ = nullptr;
    std::vector<float> rnnIn_;
    std::vector<float> rnnOut_;
    SpeexPreprocessState* preprocess_ = nullptr;
    SpeexEchoState* echo_ = nullptr;
    std::mutex dspMutex_;   // protege preprocess_/echo_/encoder_

    std::function<void(const std::vector<uint8_t>&)> onAudioCaptured_;

    std::atomic<float> inputGain_{1.0f};
    std::atomic<float> outputGain_{1.0f};
    std::atomic<float> noiseGate_{90.0f};
    std::atomic<int>   nsLevel_{kNoiseMedium};
    std::atomic<float> rnnMix_{0.85f};        // 0 = sem denoise, 1 = so o denoise
    std::atomic<float> voiceProbGate_{0.5f};  // limiar da probabilidade de voz
    std::atomic<float> voiceProb_{0.0f};
    std::atomic<bool>  agc_{true};
    std::atomic<int>   agcTarget_{24000};
    std::atomic<bool>  echoCancel_{true};
    std::atomic<bool>  vad_{true};
    std::atomic<bool>  muted_{false};
    std::atomic<bool>  deafened_{false};
    std::atomic<bool>  loopback_{false};
    std::atomic<bool>  dspDirty_{true};

    std::atomic<float> inputLevel_{0.0f};
    std::atomic<float> rawLevel_{0.0f};
    std::atomic<float> outputLevel_{0.0f};
    std::atomic<bool>  transmitting_{false};
    std::atomic<int>   agcGain_{100};
    std::atomic<uint32_t> latencyMs_{0};
    std::atomic<uint32_t> localSeq_{0};

    int consecutiveVoiceFrames_ = 0;
    int hangoverFrames_ = 0;
};
