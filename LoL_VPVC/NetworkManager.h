#pragma once
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>
#include <rtc/rtc.hpp>

// Estado que o cliente mantem sobre cada participante da sala.
struct PeerInfo {
    uint32_t id = 0;
    std::string name;
    int role = 0;          // 0 = azul, 1 = vermelho, 2 = organizador
    float x = 0.0f;        // posicao normalizada no minimapa (0..1)
    float y = 0.0f;
    bool hasPosition = false;
    double distance = -1.0;  // preenchido pelo main (unidades de mapa)
    int64_t lastSeenMs = 0;
};

class NetworkManager {
public:
    // Cabecalho binario: [magic][peerId LE][seq LE] + payload Opus
    static constexpr uint8_t kAudioMagic = 0xA1;
    static constexpr size_t  kHeaderSize = 9;

    NetworkManager();
    ~NetworkManager();
    NetworkManager(const NetworkManager&) = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;

    bool Init(const std::string& url, uint32_t localId);
    void Close();
    bool IsConnected() const { return connected_.load(); }
    const std::string& Url() const { return url_; }

    void SendAudio(const std::vector<uint8_t>& opusPayload);
    void SendPosition(float x, float y);          // 0..1 normalizado
    void SendInfo(const std::string& name, int role);
    void SendBye();
    // Mede o tempo de ida e volta ate o servidor (o relay devolve um PONG).
    void SendPing();
    int  GetPingMs() const { return pingMs_.load(); }   // -1 = sem resposta

    // (peerId, seq, dados, tamanho) - chamado na thread de rede.
    void SetAudioReceivedCallback(std::function<void(uint32_t, uint32_t, const uint8_t*, size_t)> cb) {
        onAudio_ = std::move(cb);
    }
    // Novo peer entrou (util para criar o slot de audio/UI).
    void SetPeerJoinedCallback(std::function<void(uint32_t)> cb) { onPeerJoined_ = std::move(cb); }
    void SetPeerLeftCallback(std::function<void(uint32_t)> cb)   { onPeerLeft_ = std::move(cb); }

    std::vector<PeerInfo> GetPeers() const;
    bool GetPeer(uint32_t id, PeerInfo& out) const;
    // Remove quem parou de mandar heartbeat; devolve os ids removidos.
    std::vector<uint32_t> PruneStale(int timeoutMs);

    uint64_t BytesSent() const { return bytesSent_.load(); }
    uint64_t BytesReceived() const { return bytesReceived_.load(); }

private:
    void OpenSocket();
    void HandleText(const std::string& msg);
    void HandleBinary(const std::byte* data, size_t size);
    static int64_t NowMs();

    std::shared_ptr<rtc::WebSocket> ws_;
    std::string url_;
    uint32_t localId_ = 0;

    std::function<void(uint32_t, uint32_t, const uint8_t*, size_t)> onAudio_;
    std::function<void(uint32_t)> onPeerJoined_;
    std::function<void(uint32_t)> onPeerLeft_;

    mutable std::mutex peersMutex_;
    std::map<uint32_t, PeerInfo> peers_;

    std::vector<std::byte> sendBuf_;
    std::mutex sendMutex_;
    std::atomic<uint32_t> seq_{0};

    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::thread reconnectThread_;
    std::atomic<int> pingMs_{-1};
    std::atomic<int64_t> lastPongMs_{0};
    std::atomic<uint64_t> bytesSent_{0};
    std::atomic<uint64_t> bytesReceived_{0};
};
