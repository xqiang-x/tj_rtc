#include "P2PManager.h"
#include "SfuSockOpt.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <iostream>
#include <random>
#include <algorithm>

namespace p2p {

P2PManager::P2PManager() = default;

P2PManager::~P2PManager() {
    Stop();
}

void P2PManager::SetConfig(const P2PConfig& config) {
    m_config = config;
}

void P2PManager::SetSendSignalingCallback(SendSignalingCb cb) {
    m_sendSignalingCb = std::move(cb);
}

void P2PManager::SetOnPhaseChanged(OnPhaseChangedCb cb) {
    m_onPhaseChangedCb = std::move(cb);
}

void P2PManager::SetOnFallback(OnFallbackCb cb) {
    m_onFallbackCb = std::move(cb);
}

void P2PManager::SetOnHolePunchComplete(OnHolePunchCompleteCb cb) {
    m_onHolePunchCompleteCb = std::move(cb);
}

void P2PManager::Start() {
    if (m_running.load()) return;
    m_running.store(true);
    m_lastStatsTime = std::chrono::steady_clock::now();
    std::cout << "[P2P] Started" << std::endl;
}

void P2PManager::Stop() {
    m_running.store(false);
    StopRecvThread();
    StopQualityMonitor();
    CloseP2PSocket();

    if (m_stunThread.joinable()) m_stunThread.join();
    if (m_holePunchThread.joinable()) m_holePunchThread.join();

    std::cout << "[P2P] Stopped" << std::endl;
}

P2PStats P2PManager::GetStats() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

void P2PManager::SetPhase(P2PPhase newPhase) {
    if (m_phase == newPhase) return;
    P2PPhase oldPhase = m_phase;
    m_phase = newPhase;
    std::cout << "[P2P] Phase: " << static_cast<int>(oldPhase)
              << " -> " << static_cast<int>(newPhase) << std::endl;
    if (m_onPhaseChangedCb) {
        m_onPhaseChangedCb(oldPhase, newPhase);
    }
}

bool P2PManager::CreateP2PSocket() {
    if (m_p2pSocketFd >= 0) return true;

    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::cerr << "[P2P] socket() failed: " << strerror(errno) << std::endl;
        return false;
    }

    // 尽力把接收缓冲提到 8MB（非 root 时钳制到 rmem_max）
    int rcvbuf = sfu::SetUdpRecvBuffer(fd);
    std::cout << "[P2P] UDP RCVBUF target=8388608 actual=" << rcvbuf << " bytes" << std::endl;

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    m_p2pSocketFd = fd;
    return true;
}

void P2PManager::CloseP2PSocket() {
    if (m_p2pSocketFd >= 0) {
        ::close(m_p2pSocketFd);
        m_p2pSocketFd = -1;
    }
}

void P2PManager::StartStunDiscovery() {
    if (!m_running.load() || m_phase != P2PPhase::kIdle) return;
    SetPhase(P2PPhase::kStunDiscovery);

    if (m_stunThread.joinable()) m_stunThread.join();
    m_stunThread = std::thread(&P2PManager::StunDiscoveryFunc, this);
}

void P2PManager::StunDiscoveryFunc() {
    std::cout << "[P2P] Starting STUN discovery to " << m_config.serverIp
              << ":" << m_config.stunPort << std::endl;

    int stunSock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (stunSock < 0) {
        std::cerr << "[P2P] STUN socket failed" << std::endl;
        SetPhase(P2PPhase::kIdle);
        return;
    }

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(stunSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(static_cast<uint16_t>(m_config.stunPort));
    inet_pton(AF_INET, m_config.serverIp.c_str(), &serverAddr.sin_addr);

    std::vector<sfu::P2PAddress> results;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    for (int attempt = 0; attempt < 3 && m_running.load(); ++attempt) {
        uint8_t transactionId[12];
        for (int i = 0; i < 12; ++i) transactionId[i] = dis(gen);

        auto request = sfu::BuildStunBindingRequest(transactionId);
        ssize_t sent = ::sendto(stunSock, request.data(), request.size(), 0,
                                reinterpret_cast<struct sockaddr*>(&serverAddr),
                                sizeof(serverAddr));
        if (sent <= 0) {
            std::cerr << "[P2P] STUN send failed attempt " << attempt << std::endl;
            continue;
        }

        uint8_t resp[256];
        struct sockaddr_in fromAddr{};
        socklen_t fromLen = sizeof(fromAddr);
        ssize_t n = ::recvfrom(stunSock, resp, sizeof(resp), 0,
                               reinterpret_cast<struct sockaddr*>(&fromAddr), &fromLen);
        if (n <= 0) {
            std::cerr << "[P2P] STUN recv timeout attempt " << attempt << std::endl;
            continue;
        }

        auto hdr = sfu::ParseStunHeader(resp, static_cast<size_t>(n));
        if (!hdr.valid || hdr.messageType != sfu::STUN_BINDING_RESPONSE) {
            std::cerr << "[P2P] Invalid STUN response" << std::endl;
            continue;
        }

        auto mapped = sfu::ParseStunMappedAddress(resp, static_cast<size_t>(n), hdr.transactionId);
        if (!mapped.valid) {
            std::cerr << "[P2P] Failed to parse STUN mapped address" << std::endl;
            continue;
        }

        sfu::P2PAddress addr;
        addr.ip = mapped.ip;
        addr.port = mapped.port;
        results.push_back(addr);

        std::cout << "[P2P] STUN result " << attempt << ": " << addr.ip << ":" << addr.port << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    ::close(stunSock);

    if (results.empty()) {
        std::cerr << "[P2P] STUN discovery failed - no results" << std::endl;
        SetPhase(P2PPhase::kIdle);
        return;
    }

    m_reflexiveAddr = results.back();

    bool samePort = true;
    for (size_t i = 1; i < results.size(); ++i) {
        if (results[i].port != results[0].port) {
            samePort = false;
            break;
        }
    }
    m_natType = samePort ? 1 : 3;

    std::cout << "[P2P] Reflexive address: " << m_reflexiveAddr.ip << ":" << m_reflexiveAddr.port
              << ", NAT type: " << (m_natType == 1 ? "cone" : "symmetric") << std::endl;

    SetPhase(P2PPhase::kWaitingPeer);
}

void P2PManager::RequestP2P(const std::string& targetStreamId) {
    if (m_phase != P2PPhase::kWaitingPeer) {
        std::cerr << "[P2P] Cannot request P2P in phase " << static_cast<int>(m_phase) << std::endl;
        return;
    }

    m_peerStreamId = targetStreamId;
    auto msg = sfu::SerializeP2PRequest(targetStreamId, 0x03);
    if (m_sendSignalingCb) {
        m_sendSignalingCb(msg);
    }
    std::cout << "[P2P] Sent P2P request for stream: " << targetStreamId << std::endl;
}

void P2PManager::HandleP2PResponse(const uint8_t* data, size_t len) {
    auto resp = sfu::ParseP2PResponse(data, len);
    if (!resp.valid) {
        std::cerr << "[P2P] Invalid P2P response" << std::endl;
        return;
    }

    if (resp.status != 0) {
        std::cerr << "[P2P] P2P request failed, status=" << static_cast<int>(resp.status) << std::endl;
        return;
    }

    m_peerSessionId = resp.peerSessionId;
    m_peerReflexiveAddr = resp.peerAddr;
    m_reflexiveAddr = resp.yourAddr;
    m_natType = resp.natType;
    m_subscriberCount = resp.subscriberCount;

    std::cout << "[P2P] P2P response: peer=" << m_peerReflexiveAddr.ip << ":" << m_peerReflexiveAddr.port
              << " your=" << m_reflexiveAddr.ip << ":" << m_reflexiveAddr.port
              << " nat=" << static_cast<int>(m_natType)
              << " subs=" << static_cast<int>(m_subscriberCount) << std::endl;

    if (m_subscriberCount > 1) {
        std::cout << "[P2P] Multiple subscribers, skipping P2P" << std::endl;
        SetPhase(P2PPhase::kFallback);
        return;
    }

    if (m_natType == 3) {
        std::cout << "[P2P] Symmetric NAT, P2P unlikely" << std::endl;
    }

    if (m_holePunchThread.joinable()) m_holePunchThread.join();
    m_holePunchThread = std::thread(&P2PManager::HolePunchFunc, this);
}

void P2PManager::HandleP2PCandidate(const uint8_t* data, size_t len) {
    auto cand = sfu::ParseP2PCandidate(data, len);
    if (!cand.valid) {
        std::cerr << "[P2P] Invalid P2P candidate" << std::endl;
        return;
    }

    m_peerReflexiveAddr.ip = cand.ip;
    m_peerReflexiveAddr.port = cand.port;
    m_peerSessionId = cand.targetSessionId;

    std::cout << "[P2P] Received candidate: " << cand.ip << ":" << cand.port << std::endl;

    if (m_phase == P2PPhase::kWaitingPeer || m_phase == P2PPhase::kHolePunching) {
        if (m_holePunchThread.joinable()) m_holePunchThread.join();
        m_holePunchThread = std::thread(&P2PManager::HolePunchFunc, this);
    }
}

void P2PManager::HolePunchFunc() {
    SetPhase(P2PPhase::kHolePunching);

    if (!CreateP2PSocket()) {
        std::cerr << "[P2P] Failed to create P2P socket" << std::endl;
        SetPhase(P2PPhase::kFallback);
        return;
    }

    struct sockaddr_in peerAddr{};
    peerAddr.sin_family = AF_INET;
    peerAddr.sin_port = htons(m_peerReflexiveAddr.port);
    inet_pton(AF_INET, m_peerReflexiveAddr.ip.c_str(), &peerAddr.sin_addr);

    auto probeData = sfu::SerializeP2PProbe(nullptr, 0);

    auto startTime = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(m_config.holePunchTimeoutMs);
    bool peerReached = false;

    std::cout << "[P2P] Hole punching to " << m_peerReflexiveAddr.ip
              << ":" << m_peerReflexiveAddr.port << std::endl;

    for (int i = 0; i < m_config.probeCount && m_running.load(); ++i) {
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (elapsed > timeout) break;

        ssize_t sent = ::sendto(m_p2pSocketFd, probeData.data(), probeData.size(), 0,
                                reinterpret_cast<struct sockaddr*>(&peerAddr),
                                sizeof(peerAddr));
        if (sent > 0) {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.probesSent++;
        }

        uint8_t recvBuf[256];
        struct sockaddr_in fromAddr{};
        socklen_t fromLen = sizeof(fromAddr);
        ssize_t n = ::recvfrom(m_p2pSocketFd, recvBuf, sizeof(recvBuf), 0,
                               reinterpret_cast<struct sockaddr*>(&fromAddr), &fromLen);
        if (n > 0) {
            auto pkt = sfu::ParseP2PPacket(recvBuf, static_cast<size_t>(n));
            if (pkt.valid && pkt.type == sfu::P2PPacketType::kProbe) {
                std::cout << "[P2P] Received probe from peer!" << std::endl;
                peerReached = true;
                std::lock_guard<std::mutex> lock(m_statsMutex);
                m_stats.probesReceived++;
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(m_config.probeIntervalMs));
    }

    if (!peerReached) {
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (elapsed < timeout) {
            while (elapsed < timeout && m_running.load()) {
                uint8_t recvBuf[256];
                struct sockaddr_in fromAddr{};
                socklen_t fromLen = sizeof(fromAddr);
                ssize_t n = ::recvfrom(m_p2pSocketFd, recvBuf, sizeof(recvBuf), 0,
                                       reinterpret_cast<struct sockaddr*>(&fromAddr), &fromLen);
                if (n > 0) {
                    auto pkt = sfu::ParseP2PPacket(recvBuf, static_cast<size_t>(n));
                    if (pkt.valid) {
                        peerReached = true;
                        std::cout << "[P2P] Received late probe from peer!" << std::endl;
                        break;
                    }
                }
                elapsed = std::chrono::steady_clock::now() - startTime;
            }
        }
    }

    if (peerReached) {
        auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        SendConnectCheck(true, static_cast<uint16_t>(std::min<long>(rtt, 65535)));
        SetPhase(P2PPhase::kConnected);
        m_connectedTime = std::chrono::steady_clock::now();
        if (m_onHolePunchCompleteCb) {
            m_onHolePunchCompleteCb(true);
        }
    } else {
        std::cout << "[P2P] Hole punch failed - timeout" << std::endl;
        SendConnectCheck(false, 0);
        SetPhase(P2PPhase::kFallback);
        CloseP2PSocket();
        if (m_onHolePunchCompleteCb) {
            m_onHolePunchCompleteCb(false);
        }
    }
}

void P2PManager::HandleP2PFallback(const uint8_t* data, size_t len) {
    auto fb = sfu::ParseP2PFallback(data, len);
    if (!fb.valid) return;

    std::cout << "[P2P] Received fallback: reason=" << static_cast<int>(fb.reason) << std::endl;

    CloseP2PSocket();
    SetPhase(P2PPhase::kFallback);

    if (m_onFallbackCb) {
        m_onFallbackCb(fb.reason);
    }
}

void P2PManager::HandleP2PConnectCheck(const uint8_t* data, size_t len) {
    auto cc = sfu::ParseP2PConnectCheck(data, len);
    if (!cc.valid) return;

    if (cc.success) {
        std::cout << "[P2P] Peer confirmed connectivity, RTT=" << cc.rttMs << "ms" << std::endl;
        if (m_phase == P2PPhase::kHolePunching || m_phase == P2PPhase::kWaitingPeer) {
            SetPhase(P2PPhase::kConnected);
            m_connectedTime = std::chrono::steady_clock::now();
        }
    } else {
        std::cout << "[P2P] Peer connectivity check failed" << std::endl;
    }
}

bool P2PManager::SendP2PPacket(const uint8_t* data, size_t len) {
    if (m_p2pSocketFd < 0 || m_phase < P2PPhase::kConnected) return false;

    struct sockaddr_in peerAddr{};
    peerAddr.sin_family = AF_INET;
    peerAddr.sin_port = htons(m_peerReflexiveAddr.port);
    inet_pton(AF_INET, m_peerReflexiveAddr.ip.c_str(), &peerAddr.sin_addr);

    ssize_t sent = ::sendto(m_p2pSocketFd, data, len, 0,
                            reinterpret_cast<struct sockaddr*>(&peerAddr),
                            sizeof(peerAddr));
    if (sent > 0) {
        RecordPacketSent(static_cast<size_t>(sent));
        return true;
    }
    return false;
}

void P2PManager::RecordPacketSent(size_t bytes) {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    m_stats.packetsSent++;
    (void)bytes;
}

void P2PManager::RecordPacketReceived(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    m_stats.packetsReceived++;
    (void)data;
    (void)len;
}

void P2PManager::UpdateQuality() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_statsMutex);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastStatsTime).count();
    if (elapsed < 500) return;

    uint64_t sentDelta = m_stats.packetsSent - m_lastPacketsSent;
    uint64_t recvDelta = m_stats.packetsReceived - m_lastPacketsReceived;

    if (sentDelta > 0) {
        double loss = 1.0 - static_cast<double>(recvDelta) / static_cast<double>(sentDelta);
        m_stats.lossRate = std::max(0.0, std::min(1.0, loss));
    }

    m_stats.qualityScore = ComputeQualityScore();

    m_lastPacketsSent = m_stats.packetsSent;
    m_lastPacketsReceived = m_stats.packetsReceived;
    m_lastStatsTime = now;

    if (m_phase == P2PPhase::kConnected) {
        if (m_stats.qualityScore >= 80) {
            m_highQualityWindows++;
            m_lowQualityWindows = 0;
        } else if (m_stats.qualityScore < 50) {
            m_lowQualityWindows++;
            m_highQualityWindows = 0;
        } else {
            m_highQualityWindows = 0;
            m_lowQualityWindows = 0;
        }
    }
}

uint8_t P2PManager::ComputeQualityScore() const {
    double loss = m_stats.lossRate;
    double rtt = m_stats.rttMs;
    double jitter = m_stats.jitterMs;

    double quality = 0.4 * (1.0 - loss)
                   + 0.3 * std::min(1.0, 100.0 / std::max(1.0, rtt))
                   + 0.3 * (1.0 - std::min(1.0, jitter / 100.0));

    return static_cast<uint8_t>(quality * 100);
}

void P2PManager::SendConnectCheck(bool success, uint16_t rttMs) {
    auto msg = sfu::SerializeP2PConnectCheck(m_peerSessionId, success, rttMs);
    if (m_sendSignalingCb) {
        m_sendSignalingCb(msg);
    }
}

void P2PManager::SendStatusReport() {
    if (m_peerSessionId == 0) return;

    auto msg = sfu::SerializeP2PStatus(
        m_peerSessionId,
        static_cast<uint8_t>(m_phase),
        static_cast<uint8_t>(m_stats.lossRate * 255),
        m_stats.rttMs,
        m_stats.qualityScore);

    if (m_sendSignalingCb) {
        m_sendSignalingCb(msg);
    }
}

void P2PManager::StartRecvThread() {
    if (m_recvThread.joinable()) return;

    m_recvThread = std::thread([this]() {
        std::cout << "[P2P] Recv thread started" << std::endl;
        uint8_t buf[65536];

        while (m_running.load()) {
            if (m_p2pSocketFd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            struct sockaddr_in fromAddr{};
            socklen_t fromLen = sizeof(fromAddr);
            ssize_t n = ::recvfrom(m_p2pSocketFd, buf, sizeof(buf), 0,
                                   reinterpret_cast<struct sockaddr*>(&fromAddr), &fromLen);
            if (n <= 0) continue;

            auto pkt = sfu::ParseP2PPacket(buf, static_cast<size_t>(n));
            if (!pkt.valid) continue;

            if (pkt.type == sfu::P2PPacketType::kData) {
                RecordPacketReceived(pkt.payload, pkt.payloadLen);
                if (m_onP2PDataCb && pkt.payloadLen > 0) {
                    m_onP2PDataCb(pkt.payload, pkt.payloadLen);
                }
            } else if (pkt.type == sfu::P2PPacketType::kHeartbeat) {
                // P2P heartbeat - keep alive
            }
            // kProbe packets are handled by HolePunchFunc
        }

        std::cout << "[P2P] Recv thread exited" << std::endl;
    });
}

void P2PManager::StopRecvThread() {
    if (m_recvThread.joinable()) {
        m_recvThread.join();
    }
}

void P2PManager::StartQualityMonitor() {
    if (m_qualityThread.joinable()) return;

    m_qualityThread = std::thread([this]() {
        std::cout << "[P2P] Quality monitor started" << std::endl;

        while (m_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!m_running.load()) break;

            if (m_phase >= P2PPhase::kConnected) {
                UpdateQuality();
                CheckPhaseMigration();
                SendStatusReport();
            }
        }

        std::cout << "[P2P] Quality monitor exited" << std::endl;
    });
}

void P2PManager::StopQualityMonitor() {
    if (m_qualityThread.joinable()) {
        m_qualityThread.join();
    }
}

void P2PManager::CheckPhaseMigration() {
    P2PStats stats;
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        stats = m_stats;
    }

    // Phase migration logic with hysteresis
    switch (m_phase) {
        case P2PPhase::kConnected: {
            // Phase 1 (connected/probing): check if quality is good enough for Phase 2
            if (stats.lossRate < 0.05 && stats.rttMs < 200 && stats.jitterMs < 50) {
                m_highQualityWindows++;
                m_lowQualityWindows = 0;
                if (m_highQualityWindows >= 5) {
                    std::cout << "[P2P] Quality good, staying in Phase 3 (connected)" << std::endl;
                }
            } else if (stats.lossRate > 0.10) {
                m_lowQualityWindows++;
                m_highQualityWindows = 0;
                if (m_lowQualityWindows >= 3) {
                    std::cout << "[P2P] Quality degraded, falling back" << std::endl;
                    SetPhase(P2PPhase::kFallback);
                    CloseP2PSocket();
                    if (m_onFallbackCb) m_onFallbackCb(1);  // quality_degraded
                }
            } else {
                m_highQualityWindows = 0;
                m_lowQualityWindows = 0;
            }

            // Check for complete P2P failure
            if (stats.packetsSent > 100 && stats.packetsReceived == 0) {
                auto elapsed = std::chrono::steady_clock::now() - m_connectedTime;
                if (elapsed > std::chrono::seconds(2)) {
                    std::cout << "[P2P] No packets received, falling back" << std::endl;
                    SetPhase(P2PPhase::kFallback);
                    CloseP2PSocket();
                    if (m_onFallbackCb) m_onFallbackCb(1);
                }
            }
            break;
        }

        case P2PPhase::kFallback:
        case P2PPhase::kIdle:
        case P2PPhase::kStunDiscovery:
        case P2PPhase::kWaitingPeer:
        case P2PPhase::kHolePunching:
            break;
    }
}

} // namespace p2p
