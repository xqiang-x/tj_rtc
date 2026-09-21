#pragma once

#include "SfuP2PProtocol.h"

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdint>

namespace p2p {

enum class P2PPhase : uint8_t {
    kIdle            = 0,
    kStunDiscovery   = 1,
    kWaitingPeer     = 2,
    kHolePunching    = 3,
    kConnected       = 4,
    kFallback        = 5,
};

enum class P2PRole {
    kNone,
    kPublisher,
    kSubscriber,
};

struct P2PConfig {
    bool enabled = false;
    std::string serverIp;
    int stunPort = 0;
    int holePunchTimeoutMs = 5000;
    int probeIntervalMs = 50;
    int probeCount = 10;
};

struct P2PStats {
    uint64_t packetsSent = 0;
    uint64_t packetsReceived = 0;
    uint64_t probesSent = 0;
    uint64_t probesReceived = 0;
    double lossRate = 0.0;
    uint16_t rttMs = 0;
    uint16_t jitterMs = 0;
    uint8_t qualityScore = 0;
};

class P2PManager {
public:
    using SendSignalingCb = std::function<bool(const std::vector<uint8_t>& data)>;
    using OnPhaseChangedCb = std::function<void(P2PPhase oldPhase, P2PPhase newPhase)>;
    using OnFallbackCb = std::function<void(uint8_t reason)>;
    using OnHolePunchCompleteCb = std::function<void(bool success)>;
    using OnP2PDataCb = std::function<void(const uint8_t* fecData, size_t fecLen)>;

    P2PManager();
    ~P2PManager();

    P2PManager(const P2PManager&) = delete;
    P2PManager& operator=(const P2PManager&) = delete;

    void SetConfig(const P2PConfig& config);
    void SetSendSignalingCallback(SendSignalingCb cb);
    void SetOnPhaseChanged(OnPhaseChangedCb cb);
    void SetOnFallback(OnFallbackCb cb);
    void SetOnHolePunchComplete(OnHolePunchCompleteCb cb);

    void SetOnP2PData(OnP2PDataCb cb) { m_onP2PDataCb = std::move(cb); }
    void StartRecvThread();
    void StopRecvThread();
    void StartQualityMonitor();
    void StopQualityMonitor();
    void CheckPhaseMigration();

    void Start();
    void Stop();

    P2PPhase GetPhase() const { return m_phase; }
    const P2PConfig& GetConfig() const { return m_config; }
    P2PStats GetStats() const;

    void SetRole(P2PRole role) { m_role = role; }
    void SetPeerSessionId(uint32_t sid) { m_peerSessionId = sid; }
    uint32_t GetPeerSessionId() const { return m_peerSessionId; }

    void StartStunDiscovery();
    void RequestP2P(const std::string& targetStreamId);

    void HandleP2PResponse(const uint8_t* data, size_t len);
    void HandleP2PCandidate(const uint8_t* data, size_t len);
    void HandleP2PFallback(const uint8_t* data, size_t len);
    void HandleP2PConnectCheck(const uint8_t* data, size_t len);

    bool SendP2PPacket(const uint8_t* data, size_t len);
    int GetP2PSocketFd() const { return m_p2pSocketFd; }

    void UpdateQuality();
    void RecordPacketSent(size_t bytes);
    void RecordPacketReceived(const uint8_t* data, size_t len);

    void SendConnectCheck(bool success, uint16_t rttMs);
    void SendStatusReport();

private:
    void StunDiscoveryFunc();
    void HolePunchFunc();
    void SetPhase(P2PPhase newPhase);
    bool CreateP2PSocket();
    void CloseP2PSocket();
    uint8_t ComputeQualityScore() const;

    P2PConfig m_config;
    P2PRole m_role = P2PRole::kNone;
    P2PPhase m_phase = P2PPhase::kIdle;
    uint32_t m_peerSessionId = 0;
    std::string m_peerStreamId;

    sfu::P2PAddress m_reflexiveAddr;
    sfu::P2PAddress m_peerReflexiveAddr;
    uint8_t m_natType = 0;
    uint8_t m_subscriberCount = 0;

    int m_p2pSocketFd = -1;
    std::thread m_stunThread;
    std::thread m_holePunchThread;
    std::thread m_recvThread;
    std::thread m_qualityThread;

    mutable std::mutex m_mutex;
    std::atomic<bool> m_running{false};

    SendSignalingCb m_sendSignalingCb;
    OnPhaseChangedCb m_onPhaseChangedCb;
    OnFallbackCb m_onFallbackCb;
    OnHolePunchCompleteCb m_onHolePunchCompleteCb;
    OnP2PDataCb m_onP2PDataCb;

    mutable std::mutex m_statsMutex;
    P2PStats m_stats;
    std::chrono::steady_clock::time_point m_lastStatsTime;
    uint64_t m_lastPacketsSent = 0;
    uint64_t m_lastPacketsReceived = 0;

    std::chrono::steady_clock::time_point m_connectedTime;
    int m_highQualityWindows = 0;
    int m_lowQualityWindows = 0;
};

} // namespace p2p
