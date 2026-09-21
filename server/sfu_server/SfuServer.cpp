#include "SfuServer.h"
#include "SfuConnManager.h"
#include "SfuEvPool.h"
#include "SfuKvStore.h"
#include "receiver.h"
#include "sender.h"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <malloc.h>
#include <glog/logging.h>

// Debug logging: VLOG(1) is only emitted when FLAGS_v >= 1 (set by --verbose)
#define SFU_DEBUG_LOG(...) VLOG(1) << "[SFU-DEBUG] " << __VA_ARGS__

namespace sfu {

// ======================== Construction / Destruction ========================

SfuServerFull::SfuServerFull(const Config& config)
    : m_last_fec_adjust(std::chrono::steady_clock::now())
    , m_config(config)
    , m_next_session_id(1000)
    , m_stats{}
{
    m_stats.start_time = std::chrono::steady_clock::now();

    // Register ourselves as the event handler on SfuServerBase
    SetEventHandler(this);
}

SfuServerFull::~SfuServerFull() {
    Stop();
}

// ======================== Start / Stop ========================

int SfuServerFull::Start() {
    if (m_running.load()) {
        LogError("Server already running");
        return -1;
    }

    // Start TCP socket via SfuServerBase
    if (m_config.enable_tcp) {
        int ret = StartTcp(m_config.listen_ip, m_config.listen_port);
        if (ret < 0) {
            LogError("Failed to start TCP on %s:%d", m_config.listen_ip.c_str(), m_config.listen_port);
            return -1;
        }
        LogInfo("TCP listening on %s:%d", m_config.listen_ip.c_str(), m_config.listen_port);
    }

    // Start UDP socket via SfuServerBase
    if (m_config.enable_udp) {
        int udp_port = m_config.udp_port > 0 ? m_config.udp_port : m_config.listen_port + 1;
        int ret = StartUdp(m_config.listen_ip, udp_port);
        if (ret < 0) {
            LogError("Failed to start UDP on %s:%d", m_config.listen_ip.c_str(), udp_port);
            if (m_config.enable_tcp) {
                StopAll();
            }
            return -1;
        }
        LogInfo("UDP listening on %s:%d", m_config.listen_ip.c_str(), udp_port);
    }

    // Start STUN service on dedicated thread
    {
        int stunPort = m_config.stun_port > 0 ? m_config.stun_port :
                       (m_config.udp_port > 0 ? m_config.udp_port + 1 : m_config.listen_port + 2);
        m_stunThread = std::thread(&SfuServerFull::StunThreadFunc, this, stunPort);
        LogInfo("STUN service on %s:%d", m_config.listen_ip.c_str(), stunPort);
    }

    // Schedule start on SfuEvPool
    EvStart();

    m_running.store(true);
    LogInfo("SFU Server started (TCP=%s, UDP=%s)",
            m_config.enable_tcp ? "ON" : "OFF",
            m_config.enable_udp ? "ON" : "OFF");

    return 0;
}

void SfuServerFull::Stop() {
    if (!m_running.load()) return;

    m_running.store(false);

    // Stop STUN thread
    if (m_stunSocketFd.load() >= 0) {
        close(m_stunSocketFd.load());
        m_stunSocketFd.store(-1);
    }
    if (m_stunThread.joinable()) {
        m_stunThread.join();
    }

    // Schedule stop on SfuEvPool（sessions/rooms 的清理在 loop 线程的 OnEvStop 中完成）
    EvStop();

    LogInfo("Server stopped");
}

// ======================== Public Management APIs ========================

// 服务器停止回调：运行在 ev_loop 线程。基类已停掉全部 watchers/timers，
// 此后 loop 线程不会再进入本服务器的任何回调，可安全回收会话与流数据。
void SfuServerFull::OnEvStop() {
    SfuServerBase::OnEvStop();

    m_sessions.clear();
    m_fdToSession.clear();
    m_streams.clear();
    m_stats.active_sessions = 0;
    m_stats.active_streams = 0;
}

void SfuServerFull::PrintStats() const {
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - m_stats.start_time).count();

    // 统计快照作为整块输出，避免逐行刷日志
    std::string block;
    auto pf = [&block](const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        block += buf;
    };

    pf("\n========== SFU Server Statistics ==========\n");
    pf("  Uptime:              %lld seconds\n", (long long)uptime);
    pf("  Total connections:   %u\n", m_stats.total_connections);
    pf("  Active sessions:     %u\n", m_stats.active_sessions);
    pf("  Active streams:      %u\n", m_stats.active_streams);
    pf("  Total bytes in:      %llu (%.2f MB)\n",
           (unsigned long long)m_stats.total_bytes_in,
           (double)m_stats.total_bytes_in / (1024.0 * 1024.0));
    pf("  Total bytes out:     %llu (%.2f MB)\n",
           (unsigned long long)m_stats.total_bytes_out,
           (double)m_stats.total_bytes_out / (1024.0 * 1024.0));
    pf("  Total frames fwd:    %llu\n", (unsigned long long)m_stats.total_frames_forwarded);
    size_t connCount = GetConnManager() ? GetConnManager()->GetSessionCount() : 0;
    pf("  ConnManager sessions: %zu\n", connCount);
    pf("============================================\n");
    LOG(INFO) << "\n" << block;
    
    // Print ProxyReceiver stats
    PrintProxyStats();
}

void SfuServerFull::PrintProxyStats() const {
    
    std::string block;
    auto pf = [&block](const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        block += buf;
    };

    bool has_proxy = false;
    uint64_t total_received = 0, total_forwarded = 0, total_dropped = 0;
    uint64_t total_cache_hits = 0, total_cache_misses = 0;
    uint64_t total_upstream_nacks = 0, total_suppressed = 0, total_recovered = 0;
    uint64_t total_pongs = 0;
    
    for (const auto& pair : m_sessions) {
        const auto* session = pair.second.get();
        if (session->proxy_receiver) {
            has_proxy = true;
            const auto& stats = session->proxy_receiver->getStats();
            
            total_received += stats.packets_received;
            total_forwarded += stats.packets_forwarded;
            total_dropped += stats.fec_packets_dropped;
            (void)stats.nack_requests_received;  // Used for adjustment, not stats
            total_cache_hits += stats.nack_cache_hits;
            total_cache_misses += stats.nack_cache_misses;
            total_upstream_nacks += stats.upstream_nacks_sent;
            total_suppressed += stats.upstream_nack_suppressed;
            total_recovered += stats.upstream_nack_recovered;
            total_pongs += stats.pongs_sent;
            
            // Per-session stats
            float hit_rate = (stats.nack_cache_hits + stats.nack_cache_misses) > 0 
                ? (float)stats.nack_cache_hits / (stats.nack_cache_hits + stats.nack_cache_misses) * 100.0f 
                : 0.0f;
            
            pf("  [Session %u] Proxy Stats:\n", session->session_id);
            pf("    Received: %llu  Forwarded: %llu  Dropped FEC: %llu\n",
                   (unsigned long long)stats.packets_received,
                   (unsigned long long)stats.packets_forwarded,
                   (unsigned long long)stats.fec_packets_dropped);
            pf("    PONG sent: %llu  PLI forwarded: %llu\n",
                   (unsigned long long)stats.pongs_sent,
                   (unsigned long long)stats.pli_forwarded_upstream);
            pf("    NACK recv: %llu  Cache hits: %llu  Cache misses: %llu  Hit rate: %.1f%%\n",
                   (unsigned long long)stats.nack_requests_received,
                   (unsigned long long)stats.nack_cache_hits,
                   (unsigned long long)stats.nack_cache_misses,
                   hit_rate);
            pf("    Upstream NACKs: %llu  Suppressed: %llu  Recovered: %llu\n",
                   (unsigned long long)stats.upstream_nacks_sent,
                   (unsigned long long)stats.upstream_nack_suppressed,
                   (unsigned long long)stats.upstream_nack_recovered);
            pf("    Cache size: %llu / %u entries\n",
                   (unsigned long long)stats.cache_size, session->proxy_receiver->maxCacheEntries());
        }
    }
    
    if (has_proxy) {
        float overall_hit_rate = (total_cache_hits + total_cache_misses) > 0
            ? (float)total_cache_hits / (total_cache_hits + total_cache_misses) * 100.0f
            : 0.0f;
        
        pf("\n  [Proxy Summary] Total:\n");
        pf("    Received: %llu  Forwarded: %llu  Dropped: %llu\n",
               (unsigned long long)total_received,
               (unsigned long long)total_forwarded,
               (unsigned long long)total_dropped);
        pf("    PONG sent: %llu  Upstream NACKs: %llu  Suppressed: %llu  Recovered: %llu\n",
               (unsigned long long)total_pongs,
               (unsigned long long)total_upstream_nacks,
               (unsigned long long)total_suppressed,
               (unsigned long long)total_recovered);
        pf("    Cache hit rate: %.1f%% (%llu/%llu)\n",
               overall_hit_rate,
               (unsigned long long)total_cache_hits,
               (unsigned long long)(total_cache_hits + total_cache_misses));
        pf("============================================\n");
    }
    if (!block.empty()) {
        LOG(INFO) << "\n" << block;
    }
}

// ======================== Pacer Bandwidth Control ========================

void SfuServerFull::SetSessionBandwidth(uint32_t session_id, uint64_t bandwidth_bps) {
    
    
    auto it = m_sessions.find(session_id);
    if (it == m_sessions.end()) {
        LOG(ERROR) << "[SFU-PACER] Session " << session_id << " not found";
        return;
    }
    
    auto* session = it->second.get();
    if (!session->pacer) {
        LOG(ERROR) << "[SFU-PACER] Session " << session_id << " has no Pacer";
        return;
    }
    
    session->pacer->setBandwidth(bandwidth_bps);
    LOG(INFO) << "[SFU-PACER] Session " << session_id << " bandwidth set to "
              << (bandwidth_bps / 1'000'000) << " Mbps";
}

void SfuServerFull::SetStreamBandwidth(const std::string& stream_id, uint64_t bandwidth_bps) {
    auto sit = m_streams.find(stream_id);
    if (sit == m_streams.end()) {
        LOG(ERROR) << "[SFU-PACER] Stream " << stream_id << " not found";
        return;
    }

    auto* stream = sit->second.get();
    int updated_count = 0;

    for (auto sub_sid : stream->subscriber_ids) {
        auto it = m_sessions.find(sub_sid);
        if (it == m_sessions.end()) continue;

        auto* session = it->second.get();
        if (session->pacer) {
            session->pacer->setBandwidth(bandwidth_bps);
            updated_count++;
        }
    }

    LOG(INFO) << "[SFU-PACER] Stream " << stream_id << " bandwidth set to "
              << (bandwidth_bps / 1'000'000) << " Mbps (" << updated_count << " sessions)";
}

void SfuServerFull::AdjustProxyFecRatio() {
    
    
    for (auto& pair : m_sessions) {
        auto* session = pair.second.get();
        if (!session->proxy_receiver) continue;
        
        const auto& stats = session->proxy_receiver->getStats();
        
        // Calculate NACK miss rate (cache miss / total NACKs)
        uint64_t total_nacks = stats.nack_cache_hits + stats.nack_cache_misses;
        if (total_nacks < 10) continue;  // Not enough data yet
        
        float miss_rate = (float)stats.nack_cache_misses / total_nacks;
        
        // Get current FEC ratio from session tracking
        float current_ratio = session->current_fec_ratio;

        // High miss rate (>30%) -> increase FEC ratio
        // Low miss rate (<5%) -> decrease FEC ratio
        if (miss_rate > 0.3f) {
            // Increase FEC: add 0.05, max 0.5
            float new_ratio = std::min(0.5f, current_ratio + 0.05f);
            session->proxy_receiver->setDownstreamFecRatio(new_ratio);
            session->current_fec_ratio = new_ratio;
            LOG(INFO) << "[SFU-PROXY] Session " << session->session_id
                      << " increased FEC ratio to " << new_ratio
                      << " (miss rate: " << (miss_rate * 100) << "%)";
        } else if (miss_rate < 0.05f && current_ratio > 0.1f) {
            // Decrease FEC: subtract 0.05, min 0.1
            float new_ratio = std::max(0.1f, current_ratio - 0.05f);
            session->proxy_receiver->setDownstreamFecRatio(new_ratio);
            session->current_fec_ratio = new_ratio;
            LOG(INFO) << "[SFU-PROXY] Session " << session->session_id
                      << " decreased FEC ratio to " << new_ratio
                      << " (miss rate: " << (miss_rate * 100) << "%)";
        }
    }
}

void SfuServerFull::PrintStreamInfo(const std::string& stream_id) const {
    auto it = m_streams.find(stream_id);
    if (it == m_streams.end()) {
        LOG(INFO) << "Stream '" << stream_id << "' not found";
        return;
    }

    std::string block;
    auto pf = [&block](const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        block += buf;
    };

    const auto& stream = *it->second;
    pf("\n========== Stream: %s ==========\n", stream_id.c_str());
    pf("  Publisher:    session=%u\n", stream.publisher_session_id);
    pf("  Channels:     audio=%s video=%s data=%s\n",
           stream.audio_on ? "on" : "off",
           stream.video_on ? "on" : "off",
           stream.data_on ? "on" : "off");
    pf("  Subscribers:  %zu\n", stream.subscriber_ids.size());
    pf("  Bytes fwd:    %llu\n", (unsigned long long)stream.total_bytes_forwarded);
    pf("  Frames fwd:   %llu\n", (unsigned long long)stream.total_frames_forwarded);

    pf("  Subscriber sessions: ");
    for (auto sid : stream.subscriber_ids) {
        auto sit = m_sessions.find(sid);
        if (sit != m_sessions.end()) {
            pf("[%u:%s] ", sid, sit->second->user_id.c_str());
        }
    }
    pf("\n=====================================\n");
    LOG(INFO) << "\n" << block;
}

void SfuServerFull::PrintSessionInfo(uint32_t session_id) const {
    
    auto it = m_sessions.find(session_id);
    if (it == m_sessions.end()) {
        LOG(INFO) << "Session " << session_id << " not found";
        return;
    }

    std::string block;
    auto pf = [&block](const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        block += buf;
    };

    const auto& s = *it->second;
    pf("\n========== Session: %u ==========\n", session_id);
    pf("  User ID:      %s\n", s.user_id.c_str());
    pf("  Stream:       %s\n", s.stream_id.c_str());
    pf("  Publisher:    %s\n", s.is_publisher ? "yes" : "no");
    pf("  Subscriber:   %s\n", s.is_subscriber ? "yes" : "no");
    if (s.is_publisher) {
        pf("  Publish:      audio=%s video=%s data=%s\n",
               s.subscribe_audio ? "yes" : "no",
               s.subscribe_video ? "yes" : "no",
               s.subscribe_data ? "yes" : "no");
    }
    if (s.is_subscriber) {
        pf("  Subscribe:    stream=%s audio=%s video=%s data=%s\n",
               s.subscribe_stream_id.c_str(),
               s.subscribe_audio ? "yes" : "no",
               s.subscribe_video ? "yes" : "no",
               s.subscribe_data ? "yes" : "no");
    }
    pf("  Bytes recv:   %llu\n", (unsigned long long)s.bytes_recv);
    pf("  Bytes sent:   %llu\n", (unsigned long long)s.bytes_sent);
    pf("  Frames recv:  %llu\n", (unsigned long long)s.frames_recv);
    pf("  Frames sent:  %llu\n", (unsigned long long)s.frames_sent);
    pf("=========================================\n");
    LOG(INFO) << "\n" << block;
}

void SfuServerFull::PrintAllStreams() const {
    std::string block;
    auto pf = [&block](const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        block += buf;
    };

    pf("\n========== All Streams (%zu) ==========\n", m_streams.size());
    for (const auto& pair : m_streams) {
        const auto& stream = *pair.second;
        pf("  Stream: %-20s pub=%-5u subs=%-3zu bytes_fwd=%llu frames_fwd=%llu\n",
               pair.first.c_str(),
               stream.publisher_session_id,
               stream.subscriber_ids.size(),
               (unsigned long long)stream.total_bytes_forwarded,
               (unsigned long long)stream.total_frames_forwarded);
    }
    pf("=====================================\n");
    LOG(INFO) << "\n" << block;
}

void SfuServerFull::PrintAllSessions() const {
    
    std::string block;
    auto pf = [&block](const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        block += buf;
    };

    pf("\n========== All Sessions (%zu) ==========\n", m_sessions.size());
    for (const auto& pair : m_sessions) {
        const auto& s = *pair.second;
        pf("  Session: %-5u user=%-15s stream=%-15s pub=%s sub=%s fd=%d\n",
               pair.first, s.user_id.c_str(), s.stream_id.c_str(),
               s.is_publisher ? "Y" : "N", s.is_subscriber ? "Y" : "N", s.fd);
    }
    pf("=========================================\n");
    LOG(INFO) << "\n" << block;
}

void SfuServerFull::KickSession(uint32_t session_id) {
    int fd = -1;

    {
        
        auto it = m_sessions.find(session_id);
        if (it == m_sessions.end()) {
            LogDebug("KickSession: session %u not found", session_id);
            return;
        }

        auto* session = it->second.get();
        fd = session->fd;

        LogInfo("Kicked session %u (user=%s)", session_id, session->user_id.c_str());

        // Send kick message if TCP
        if (fd >= 0) {
            auto msg = SerializeKick(session_id, "Kicked by admin");
            SendToClientQuick(fd, msg.data(), msg.size());
        }

        // Remove session
        DestroySession(session_id);
    }

    // Close connection via ConnManager
    if (fd >= 0 && GetConnManager()) {
        GetConnManager()->RemoveSession(fd);
    }
}

void SfuServerFull::KickAllFromStream(const std::string& stream_id) {
    auto sit = m_streams.find(stream_id);
    if (sit == m_streams.end()) {
        LogDebug("KickAllFromStream: stream %s not found", stream_id.c_str());
        return;
    }

    auto& stream = *sit->second;
    std::vector<uint32_t> all_sessions;
    if (stream.publisher_session_id != 0) {
        all_sessions.push_back(stream.publisher_session_id);
    }
    all_sessions.insert(all_sessions.end(), stream.subscriber_ids.begin(), stream.subscriber_ids.end());

    for (auto sid : all_sessions) {
        auto sit2 = m_sessions.find(sid);
        if (sit2 == m_sessions.end()) continue;
        auto* session = sit2->second.get();

        // Send kick message if TCP
        if (session->fd >= 0) {
            auto msg = SerializeKick(sid, "Stream cleared");
            SendToClientQuick(session->fd, msg.data(), msg.size());
        }
        LogInfo("Kicked session %u from stream %s", sid, stream_id.c_str());
    }

    // Clean up all sessions and the stream
    for (auto sid : all_sessions) {
        DestroySession(sid);
    }
    m_streams.erase(sit);
    LogInfo("Cleared stream %s, kicked %zu sessions", stream_id.c_str(), all_sessions.size());
}

SfuStats SfuServerFull::GetStats() const {
    
    return m_stats;
}

// ======================== SfuEventHandler Implementation ========================

bool SfuServerFull::OnConnect(uint8_t* data, size_t len, ConnInfo* conn) {
    LogDebug("OnConnect: fd=%d isTcp=%s", conn->fd, conn->isTcp ? "yes" : "no");
    conn->KeepAlive();  // Don't close the connection
    return true;
}

bool SfuServerFull::OnData(uint8_t* data, size_t len, ConnInfo* conn) {
    LogDebug("OnData: fd=%d len=%zu", conn->fd, len);
    return true;
}

bool SfuServerFull::OnHeartbeat(uint8_t* data, size_t len, ConnInfo* conn) {
    // Update last active time
    
    auto it = m_fdToSession.find(conn->fd);
    if (it != m_fdToSession.end()) {
        auto* session = m_sessions[it->second].get();
        if (session) {
            session->last_active_time = std::chrono::steady_clock::now();
        }
    }

    // Echo heartbeat back（data 已被 DispatchMessage 剥掉 header，这里要补回去）
    if (len > 0 && conn->isTcp) {
        std::vector<uint8_t> ack;
        ack.reserve(1 + len);
        ack.push_back(MakeHeader(PROTOCOL_VERSION, static_cast<uint8_t>(MsgType::kHeartbeat)));
        ack.insert(ack.end(), data, data + len);
        SendToClientQuick(conn->fd, ack.data(), ack.size());
    }
    return true;
}

bool SfuServerFull::OnCmd(uint8_t* data, size_t len, ConnInfo* conn) {
    if (len < 1) return false;

    uint8_t ctrlType = data[0];
    
    SFU_DEBUG_LOG("OnCmd: ctrlType=" << static_cast<int>(ctrlType)
              << " (0x" << std::hex << static_cast<int>(ctrlType) << std::dec << ")"
              << " len=" << len << " fd=" << conn->fd);

    switch (static_cast<SfuCtrlType>(ctrlType)) {
        case SfuCtrlType::kSubscribeReq:
            // ParseSubscribeReq expects data to start with ctrlType
            HandleSubscribeReq(conn, data, len);
            break;

        case SfuCtrlType::kStreamData:
            // ParseStreamData expects data to start with ctrlType
            HandleStreamData(conn, data, len);
            break;

        case SfuCtrlType::kUnsubscribe:
            HandleUnsubscribe(conn, data, len);
            break;

        case SfuCtrlType::kP2PRequest:
            HandleP2PRequest(conn, data, len);
            break;

        case SfuCtrlType::kP2PConnectCheck:
            HandleP2PConnectCheck(conn, data, len);
            break;

        case SfuCtrlType::kP2PStatus:
            HandleP2PStatus(conn, data, len);
            break;

        case SfuCtrlType::kCtrlReliable:
        case SfuCtrlType::kCtrlUnreliable:
            HandleCtrlData(conn, ctrlType, data, len);
            break;

        default:
            LogDebug("Unknown SFU ctrl type: 0x%02x from fd=%d", ctrlType, conn->fd);
            SFU_DEBUG_LOG("Unknown ctrlType: 0x" << std::hex << static_cast<int>(ctrlType) << std::dec);
            return false;
    }

    return true;
}

void SfuServerFull::OnDisconnect(uint8_t* data, size_t len, ConnInfo* conn) {
    LogDebug("OnDisconnect: fd=%d", conn->fd);
    OnConnClosed(conn->fd);
}

bool SfuServerFull::OnStream(uint8_t* data, size_t len, ConnInfo* conn) {
    LogDebug("OnStream: fd=%d len=%zu", conn->fd, len);
    return true;
}

bool SfuServerFull::OnStreamCtrl(uint8_t* data, size_t len, ConnInfo* conn) {
    LogDebug("OnStreamCtrl: fd=%d len=%zu", conn->fd, len);
    return true;
}

bool SfuServerFull::OnRoomCtrl(uint8_t* data, size_t len, ConnInfo* conn) {
    LogDebug("OnRoomCtrl: fd=%d len=%zu", conn->fd, len);
    return true;
}

void SfuServerFull::OnUdpPacketReceived(uint32_t sessionId, const sockaddr_in& addr,
                                        socklen_t addrLen) {
    // Update last active time for UDP session (any packet keeps session alive)
    // This includes video data, FEC heartbeat (PING/PONG), etc.
    auto it = m_sessions.find(sessionId);
    if (it == m_sessions.end()) return;
    auto* session = it->second.get();
    session->last_active_time = std::chrono::steady_clock::now();

    // 运营商 CGNAT 可能在会话中途重绑 UDP 映射（公网端口变化）：若源地址与
    // 订阅时记录的地址不同，立即刷新，否则服务器回包（PONG/NACK）会发往
    // 失效映射被丢弃，推流端 10s 无 PONG 判定断线后重连，形成周期性断流
    if (session->udp_client_alen != addrLen ||
        session->udp_client_addr.sin_addr.s_addr != addr.sin_addr.s_addr ||
        session->udp_client_addr.sin_port != addr.sin_port) {
        session->udp_client_addr = addr;
        session->udp_client_alen = addrLen;
        LOG(INFO) << "[SFU-NAT] session=" << sessionId
                  << " source addr changed to " << inet_ntoa(addr.sin_addr)
                  << ":" << ntohs(addr.sin_port);
    }
}

void SfuServerFull::OnUpdate(int intervalMs) {
    if (intervalMs == 1000) {
        // 每 60s 归还一次热路径分配产生的 glibc 碎片，抑制 RSS 缓慢爬升
        static int trim_counter = 0;
        if (++trim_counter >= 60) {
            trim_counter = 0;
            malloc_trim(0);
        }

        CleanupTimeoutSessions();
        CleanupEmptyStreams();
        
        // Tick all ProxyReceivers (cache cleanup + NACK generation)
        // 单线程事件循环：sessions 无锁访问，tick 回调不会与其他线程竞争
        uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        
        // Collect sessions first（sessions 仅由 ev_loop 线程访问，无需加锁）
        std::vector<fec_protocol::ProxyFrameReceiver*> receivers;
        {
            
            for (auto& pair : m_sessions) {
                auto* session = pair.second.get();
                if (session->proxy_receiver) {
                    receivers.push_back(session->proxy_receiver.get());
                }
            }
        }
        
        // Tick all receivers
        for (auto* receiver : receivers) {
            receiver->tick(now_us);
        }
        
        // Dynamic FEC ratio adjustment (every 10 seconds)
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - m_last_fec_adjust).count();
        if (elapsed >= 10) {
            AdjustProxyFecRatio();
            m_last_fec_adjust = now;
        }
    }

    if (m_config.enable_stats_print && intervalMs == 1000) {
        static int counter = 0;
        counter++;
        if (counter >= m_config.stats_interval_sec) {
            PrintStats();
            PrintAllStreams();
            counter = 0;
        }
    }
}

// ======================== Connection Management ========================

void SfuServerFull::OnConnEstablished(ConnSession* session) {
    m_stats.total_connections++;
    LogDebug("Connection established: fd=%d isTcp=%s", session->fd, session->isTcp ? "yes" : "no");
}

void SfuServerFull::OnConnClosed(int fd) {
    
    auto it = m_fdToSession.find(fd);
    if (it != m_fdToSession.end()) {
        uint32_t session_id = it->second;
        LogInfo("Client fd=%d disconnected, destroying session %u", fd, session_id);
        DestroySession(session_id);
        m_fdToSession.erase(it);
    }
}

// ======================== Subscribe Handling ========================

void SfuServerFull::HandleSubscribeReq(ConnInfo* conn, const uint8_t* data, size_t len) {
    // Parse subscribe request (data starts after the ctrlType byte)
    auto req = ParseSubscribeReq(data, len);

    // 统一回包：TCP 走 send queue（带头），UDP 直接 sendto（无长度前缀）
    auto SendSubscribeResp = [this, conn](const std::vector<uint8_t>& resp) {
        if (conn->isTcp) {
            SendToClient(conn->fd, resp.data(), resp.size());
        } else {
            ::sendto(conn->fd, resp.data(), resp.size(), MSG_DONTWAIT,
                     reinterpret_cast<const struct sockaddr*>(&conn->remoteAddr.addr),
                     conn->remoteAddr.addrLen);
        }
    };

    if (!req.valid) {
        LogError("Invalid subscribe request from fd=%d", conn->fd);
        auto resp = SerializeSubscribeResp(0, 1, kReasonInvalidFormat);
        SendSubscribeResp(resp);
        return;
    }

    bool is_publisher = (req.role == SubscribeRole::kPublisher);
    // 纯控制会话：flags 三通道全 false（UDP 设备的独立 TCP 信令会话）——
    // 不注册流/不检查流存在/不入订阅者列表，仅用于信令配对
    bool ctrl_only = !(req.flags.audio || req.flags.video || req.flags.data);
    LogInfo("Subscribe request: role=%s stream=%s user=%s audio=%d video=%d data=%d ctrl_only=%d fd=%d",
            is_publisher ? "pub" : "sub", req.streamId.c_str(), req.userId.c_str(),
            req.flags.audio, req.flags.video, req.flags.data, ctrl_only, conn->fd);

    // Create session: fd=-1 for UDP (avoid polluting m_fdToSession with server UDP fd)
    int session_fd = conn->isTcp ? conn->fd : -1;
    uint32_t session_id = 0;

    if (is_publisher) {
        // ===== 推流端：注册并发布一条新流（流名全局唯一，冲突则拒绝） =====
        // 纯控制会话不占流名（流名由主会话注册），跳过冲突检查
        if (!ctrl_only) {
            auto sit = m_streams.find(req.streamId);
            if (sit != m_streams.end()) {
                SfuStream* stream = sit->second.get();
                uint32_t old_pub = stream->publisher_session_id;
                if (old_pub != 0) {
                    auto pit = m_sessions.find(old_pub);
                    // 同一 user+传输类型的推流端重发订阅（UDP 响应丢失的幂等重试）
                    // 或崩溃后重连：复用既有会话并刷新对端 UDP 地址，避免流名被
                    // 自己的僵尸会话占住 31s（idle 超时）导致假活/黑屏
                    if (pit != m_sessions.end() &&
                        pit->second->user_id == req.userId &&
                        (pit->second->fd < 0) == (session_fd < 0)) {
                        auto* old_session = pit->second.get();
                        if (!conn->isTcp) {
                            old_session->udp_server_fd   = conn->fd;
                            old_session->udp_client_addr = conn->remoteAddr.addr;
                            old_session->udp_client_alen = conn->remoteAddr.addrLen;
                        }
                        old_session->last_active_time = std::chrono::steady_clock::now();
                        LogInfo("Publisher %s re-subscribe: reuse session %u for stream '%s'",
                                req.userId.c_str(), old_pub, req.streamId.c_str());
                        auto resp = SerializeSubscribeResp(old_pub, 0, kReasonOk);
                        SendSubscribeResp(resp);
                        return;
                    }
                }
                LogInfo("Stream '%s' already exists, reject publisher %s",
                        req.streamId.c_str(), req.userId.c_str());
                auto resp = SerializeSubscribeResp(0, 1, kReasonStreamExists);
                SendSubscribeResp(resp);
                return;
            }
        }

        session_id = CreateSession(req.userId, req.streamId, session_fd);
        if (session_id == 0) {
            auto resp = SerializeSubscribeResp(0, 1, "Failed to create session");
            SendSubscribeResp(resp);
            return;
        }

        // Update session with publish info（单线程无需加锁）
        auto* session = m_sessions[session_id].get();
        if (session) {
            // 主会话才声明推流角色并注册流；纯控制会话仅占位（用于信令配对）
            if (!ctrl_only) {
                session->is_publisher = true;
                session->subscribe_audio = req.flags.audio;
                session->subscribe_video = req.flags.video;
                session->subscribe_data = req.flags.data;
            } else {
                session->is_ctrl_only = true;
            }

            // Store UDP address for UDP sessions
            if (!conn->isTcp) {
                session->udp_server_fd   = conn->fd;
                session->udp_client_addr = conn->remoteAddr.addr;
                session->udp_client_alen = conn->remoteAddr.addrLen;
            }
        }

        // 纯控制会话不注册流（流名由主会话持有）
        if (!ctrl_only) {
            // Register stream（上面已预检冲突，此处理论上必然成功）
            if (!RegisterStream(req.streamId, session_id)) {
                DestroySession(session_id);
                auto resp = SerializeSubscribeResp(0, 1, kReasonStreamExists);
                SendSubscribeResp(resp);
                return;
            }
            auto* stream = GetStream(req.streamId);
            if (stream) {
                stream->audio_on = req.flags.audio;
                stream->video_on = req.flags.video;
                stream->data_on = req.flags.data;
            }
            LogInfo("[SFU-STREAM] Stream '%s' registered by publisher %s (session=%u)",
                    req.streamId.c_str(), req.userId.c_str(), session_id);
        }
    } else {
        // ===== 拉流端：订阅一条已存在的流（纯控制会话不要求流存在） =====
        SfuStream* stream = nullptr;
        if (!ctrl_only) {
            stream = GetStream(req.streamId);
            if (!stream || stream->publisher_session_id == 0) {
                LogInfo("Stream '%s' not found, reject subscriber %s",
                        req.streamId.c_str(), req.userId.c_str());
                auto resp = SerializeSubscribeResp(0, 1, kReasonStreamNotFound);
                SendSubscribeResp(resp);
                return;
            }
        }

        session_id = CreateSession(req.userId, req.streamId, session_fd);
        if (session_id == 0) {
            auto resp = SerializeSubscribeResp(0, 1, "Failed to create session");
            SendSubscribeResp(resp);
            return;
        }

        // Update session with subscription info（单线程无需加锁）
        auto* session = m_sessions[session_id].get();
        if (session) {
            // 纯控制会话不声明订阅角色（不收媒体），仅维持会话用于信令配对
            if (!ctrl_only) {
                session->is_subscriber = true;
                session->subscribe_stream_id = req.streamId;
                session->subscribe_audio = req.flags.audio;
                session->subscribe_video = req.flags.video;
                session->subscribe_data = req.flags.data;

                // 加入流的订阅者列表（纯控制会话不入列表，不接收媒体转发）
                stream->subscriber_ids.insert(session_id);
            } else {
                session->is_ctrl_only = true;
            }

            // Store UDP address for UDP sessions
            if (!conn->isTcp) {
                session->udp_server_fd   = conn->fd;
                session->udp_client_addr = conn->remoteAddr.addr;
                session->udp_client_alen = conn->remoteAddr.addrLen;
            }

            // 新订阅者上线 → 向推流端请求关键帧，让新观众立即能解码（纯控制会话不触发）
            if (!ctrl_only) {
                auto* pub_session = GetSession(stream->publisher_session_id);
                if (pub_session && pub_session->proxy_receiver) {
                    constexpr auto kPliInterval = std::chrono::milliseconds(500);
                    auto now_tp = std::chrono::steady_clock::now();
                    if (pub_session->last_pli_forward_time.time_since_epoch().count() == 0 ||
                        now_tp - pub_session->last_pli_forward_time >= kPliInterval) {
                        pub_session->last_pli_forward_time = now_tp;
                        pub_session->pli_forwarded++;
                        pub_session->proxy_receiver->requestKeyFrame();
                        LogInfo("[SFU-PLI] new subscriber session=%u user=%s -> request keyframe from publisher session=%u",
                                session_id, req.userId.c_str(), stream->publisher_session_id);
                    }
                }

                // 多订阅者回退：当订阅者 > 1 时，通知已有 P2P 会话回退
                if (stream->subscriber_ids.size() > 1) {
                    for (auto sub_id : stream->subscriber_ids) {
                        if (sub_id == session_id) continue;
                        auto* sub_session = GetSession(sub_id);
                        if (sub_session && sub_session->p2p_enabled) {
                            LogInfo("Multi-subscriber fallback: sending kP2PFallback to session %u", sub_id);
                            SendP2PFallback(sub_id, 0);  // reason=0: new_subscriber
                            sub_session->p2p_enabled = false;
                        }
                    }
                }
            }
        }
    }

    // Send response
    auto resp = SerializeSubscribeResp(session_id, 0, kReasonOk);
    LOG(INFO) << "[SFU-SUBSCRIBE] Sending response: sessionId=" << session_id
              << " resp_size=" << resp.size() << " fd=" << conn->fd;
    
    if (!conn->isTcp) {
        // UDP: send directly via sendto (no length prefix)
        ::sendto(conn->fd, resp.data(), resp.size(), MSG_DONTWAIT,
                 reinterpret_cast<const struct sockaddr*>(&conn->remoteAddr.addr),
                 conn->remoteAddr.addrLen);
        LogInfo("Subscribe OK (UDP): role=%s session=%u user=%s stream=%s addr=%s:%u alen=%u",
                is_publisher ? "pub" : "sub",
                session_id, req.userId.c_str(), req.streamId.c_str(),
                inet_ntoa(conn->remoteAddr.addr.sin_addr), ntohs(conn->remoteAddr.addr.sin_port),
                conn->remoteAddr.addrLen);
    } else if (conn->isTcp) {
        int ret = SendToClient(conn->fd, resp.data(), resp.size());
        if (ret == 0) {
            LogInfo("Subscribe OK: role=%s session=%u user=%s stream=%s",
                    is_publisher ? "pub" : "sub",
                    session_id, req.userId.c_str(), req.streamId.c_str());
            SFU_DEBUG_LOG("Subscribe OK, registered fd=" << conn->fd 
                      << " for session=" << session_id);
        } else {
            LogError("Failed to send subscribe response to fd=%d (SendToClient ret=%d)",
                     conn->fd, ret);
            
            DestroySession(session_id);
        }
    }
}

// ======================== Stream Data Handling ========================

void SfuServerFull::HandleStreamData(ConnInfo* conn, const uint8_t* data, size_t len) {
    // Parse stream data (data starts after ctrlType byte)
    auto sd = ParseStreamData(data, len);
    if (!sd.valid) {
        LogDebug("Invalid stream data from fd=%d", conn->fd);
        return;
    }

    SFU_DEBUG_LOG("HandleStreamData: sessionId=" << sd.sessionId 
              << " payloadLen=" << sd.payloadLen << " from fd=" << conn->fd);

    // Verify session exists（单线程无需加锁）
    SfuSession* source_session = nullptr;
    auto it = m_sessions.find(sd.sessionId);
    if (it == m_sessions.end()) {
        LogDebug("Stream data from unknown session %u", sd.sessionId);
        // 限速打印：被超时踢出的死会话会持续发包，每包打印曾刷出 343 万行/712MB 日志。
        // 这里每 5s 汇总一次丢弃计数，详细日志仅 verbose 模式输出
        static uint64_t unknown_drop_count = 0;
        static uint64_t last_unknown_log_us = 0;
        unknown_drop_count++;
        uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now_us - last_unknown_log_us >= 5'000'000) {
            LOG(ERROR) << "[SFU-ERROR] Unknown session: " << sd.sessionId
                       << " (dropped " << unknown_drop_count
                       << " packets in last 5s)";
            unknown_drop_count = 0;
            last_unknown_log_us = now_us;
        }
        return;
    }
    source_session = it->second.get();

    // 新模型：仅推流端（流注册者）能发送媒体数据；拉流端只允许 NACK/PLI 反馈
    //（反馈包以推流端 sessionId 包装，故此处 source_session 仍为推流端会话）
    if (!source_session->is_publisher) {
        LogDebug("Reject stream data from non-publisher session %u", sd.sessionId);
        return;
    }
    const std::string& stream_id = source_session->stream_id;
    source_session->frames_recv++;
    source_session->bytes_recv += len;
    source_session->last_active_time = std::chrono::steady_clock::now();

    // UDP 会话空闲超时（30s）在 CreateSession 时已按传输类型设定，无需在此调整

    // Initialize ProxyReceiver for UDP publisher on first frame
    if (source_session->fd < 0 && !source_session->proxy_receiver) {
        fec_protocol::ProxyConfig proxy_cfg;
        proxy_cfg.downstream_fec_ratio = 0.25f;  // Forward 25% of FEC blocks
        // NACK 重传只需覆盖 RTT + 帧超时；小缓存把每推流端常驻内存从 ~140MB 降到 ~15MB
        proxy_cfg.cache_timeout_ms = 10 * 1000;
        proxy_cfg.max_cache_entries = 20 * 1000;
        proxy_cfg.nack.nack_delay_ms = 20;
        proxy_cfg.nack.nack_interval_ms = 50;
        // 上游 NACK 重试上限 40 ≈ 2s，与 nack_lifetime_ms=2000 生命周期对齐，
        // 丢失块超时后强制放弃，防止 proxy_nack_states 无限膨胀
        proxy_cfg.nack.max_nack_retries = 40;
        
        source_session->proxy_receiver = std::make_unique<fec_protocol::ProxyFrameReceiver>(proxy_cfg);
        
        // Setup callbacks
        setupProxyCallbacks(source_session, stream_id);
        
        LOG(INFO) << "[SFU-PROXY] ProxyReceiver initialized for session " << sd.sessionId;
    }
    
    // Process through ProxyReceiver for UDP publishers
    if (source_session->proxy_receiver) {
        uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        // Route by FEC message type (payload[1] = CommonHeader.msg_type):
        // NACK/STATS/PLI come from downstream pull clients (wrapped with publisher's
        // sessionId so they get routed here); DATA comes from the upstream publisher.
        uint8_t fec_msg_type = (sd.payloadLen >= 2) ? sd.payload[1] : 0;
        if (fec_msg_type == static_cast<uint8_t>(fec_protocol::MsgType::NACK) ||
            fec_msg_type == static_cast<uint8_t>(fec_protocol::MsgType::STATS)) {
            source_session->proxy_receiver->onDownstreamPacket(sd.payload, sd.payloadLen);
        } else if (fec_msg_type == static_cast<uint8_t>(fec_protocol::MsgType::PLI)) {
            // PLI 节流：每个推流端最多 500ms 向上游转发一个关键帧请求
            constexpr auto kPliInterval = std::chrono::milliseconds(500);
            auto now_tp = std::chrono::steady_clock::now();
            if (source_session->last_pli_forward_time.time_since_epoch().count() == 0 ||
                now_tp - source_session->last_pli_forward_time >= kPliInterval) {
                source_session->last_pli_forward_time = now_tp;
                source_session->pli_forwarded++;
                LogInfo("[SFU-PLI] forwarded upstream session=%u total=%llu",
                        sd.sessionId, (unsigned long long)source_session->pli_forwarded);
                source_session->proxy_receiver->onDownstreamPacket(sd.payload, sd.payloadLen);
            } else {
                source_session->pli_throttled++;
                LogDebug("[SFU-PLI] throttled session=%u throttled_count=%llu",
                         sd.sessionId, (unsigned long long)source_session->pli_throttled);
            }
        } else {
            bool forwarded = source_session->proxy_receiver->onUpstreamPacket(
                sd.payload, sd.payloadLen, now_us);
            if (!forwarded) {
                SFU_DEBUG_LOG("ProxyReceiver dropped packet for session " << sd.sessionId);
            }
        }

        // Drive cache eviction and active upstream NACK generation
        source_session->proxy_receiver->tick(now_us);

        return;  // ProxyReceiver handles forwarding
    }

    SFU_DEBUG_LOG("Session found: user=" << source_session->user_id
              << " stream=" << stream_id << " is_subscriber=" << source_session->is_subscriber);

    // UDP/TCP 模式：直接转发 payload（可能是 FEC 分片包，SFU 不解析）
    ForwardToSubscribers(sd.sessionId, stream_id, sd.payload, sd.payloadLen);

    if (m_config.verbose) {
        LogDebug("Forwarded stream: session=%u stream=%s payload=%zu bytes",
                 sd.sessionId, stream_id.c_str(), sd.payloadLen);
    }
}

// ======================== Unsubscribe Handling ========================

void SfuServerFull::setupProxyCallbacks(SfuSession* session, const std::string& stream_id) {
    if (!session->proxy_receiver) return;
    
    // Callback 1: Forward packet to downstream subscribers
    session->proxy_receiver->setSendCallback(
        [this, stream_id, session](const uint8_t* data, size_t len) {
            // Forward to all subscribers in the stream
            // NOTE: No lock needed - SFU runs in single-threaded event loop
            // 'data' is FEC protocol packet, need to wrap with SFU header
            auto* stream = GetStream(stream_id);
            if (!stream) return;

            uint8_t* buf = static_cast<uint8_t*>(m_udpPool.allocate());
            // Use pool buffer when possible, fall back to vector for oversized payloads
            size_t msgLen = sfu::SerializeForwardDataTo(buf, mempool::FixedMemoryPool::kBlockSize,
                                                        session->session_id, data, len);
            std::vector<uint8_t> fallback_msg;
            const uint8_t* msg = buf;
            if (msgLen == 0) {
                // payload exceeds 1500-6=1494 bytes, fall back to vector
                fallback_msg = sfu::SerializeForwardData(session->session_id, data, len);
                msg = fallback_msg.data();
                msgLen = fallback_msg.size();
            }

            int fwd_count = 0;
            for (auto sub_sid : stream->subscriber_ids) {
                auto it = m_sessions.find(sub_sid);
                if (it == m_sessions.end()) continue;
                auto* sub_session = it->second.get();

                // Update subscriber's last_active_time when forwarding data
                // (subscriber only receives; this prevents UDP timeout)
                sub_session->last_active_time = std::chrono::steady_clock::now();

                if (QueueOrSend(sub_session, msg, msgLen)) {
                    fwd_count++;
                }
            }

            if (fwd_count > 0) {
                stream->total_bytes_forwarded += msgLen * fwd_count;
                stream->total_frames_forwarded += fwd_count;
                m_stats.total_bytes_out += msgLen * fwd_count;
                m_stats.total_frames_forwarded += fwd_count;
            }

            m_udpPool.deallocate(buf);
        }
    );
    
    // Callback 2: Send NACK to upstream publisher
    // PONG/NACK/STATS/PLI 等服务器→推流端反馈优先走其 TCP 控制信令通道：
    // 实测运营商 CGNAT 会周期性地丢弃 UDP 入向映射（约 2~4 分钟一轮，
    // 出向不受影响），导致推流端收不到 PONG 而周期性重连；TCP 无此问题。
    // 无可用控制会话时回退 UDP sendto（旧客户端/纯 UDP 路径）。
    session->proxy_receiver->setUpstreamSendCallback(
        [this, session](const uint8_t* data, size_t len) {
            SfuSession* ctrl = FindSiblingSessionLocked(session);
            if (ctrl && ctrl->fd >= 0 && ctrl->is_ctrl_only) {
                if (SendToClient(ctrl->fd, data, len) == 0) {
                    return;
                }
                // TCP 发送失败：继续走 UDP 兜底（控制会话即将被清理）
            }
            // Send feedback back to the UDP publisher
            if (session->udp_server_fd >= 0) {
                ssize_t sent = ::sendto(session->udp_server_fd,
                         data, len, MSG_DONTWAIT,
                         reinterpret_cast<const struct sockaddr*>(&session->udp_client_addr),
                         session->udp_client_alen);
                if (sent < 0) {
                    // 限速：上游不可达时 NACK 重试会周期性触发，避免刷屏
                    static uint64_t last_nack_fail_log_us = 0;
                    uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (now_us - last_nack_fail_log_us >= 5'000'000) {
                        LOG(ERROR) << "[SFU-PROXY] Failed to send NACK to upstream: errno="
                                   << errno << " session=" << session->session_id;
                        last_nack_fail_log_us = now_us;
                    }
                }
            }
        }
    );
    
    // Callback 3: NACK miss (cache doesn't have the packet)
    session->proxy_receiver->setNackMissCallback(
        [stream_id, session](const std::vector<fec_protocol::NackEntry>& entries) {
            // 限速：网络丢包严重时 miss 频繁，每 5s 汇总一条
            static uint64_t last_miss_log_us = 0;
            static uint64_t miss_count = 0;
            miss_count++;
            uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_us - last_miss_log_us >= 5'000'000) {
                LOG(INFO) << "[SFU-PROXY-NACK-MISS] Session " << session->session_id
                          << " stream=" << stream_id
                          << " recent_entries=" << entries.size()
                          << " total_misses=" << miss_count
                          << " -> requesting upstream retransmit";
                last_miss_log_us = now_us;
            }

            // Forward the miss upstream so the publisher retransmits
            if (session->proxy_receiver) {
                session->proxy_receiver->requestUpstreamNack(entries);
            }
        }
    );
}

// ======================== Unsubscribe Handling ========================

void SfuServerFull::HandleUnsubscribe(ConnInfo* conn, const uint8_t* data, size_t len) {
    uint32_t session_id = 0;
    if (len >= 4) {
        session_id = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
    }

    LogInfo("Unsubscribe: session=%u fd=%d", session_id, conn->fd);

    auto it = m_sessions.find(session_id);
    if (it != m_sessions.end()) {
        auto* session = it->second.get();
        std::string stream_id = session->stream_id;

        // 从流中摘除（推流端下线的通知订阅者由 DestroySession 统一处理）
        auto sit = m_streams.find(stream_id);
        if (sit != m_streams.end()) {
            auto* stream = sit->second.get();
            if (stream->publisher_session_id == session_id) {
                stream->publisher_session_id = 0;
            }
            stream->subscriber_ids.erase(session_id);
        }

        DestroySession(session_id);
    }
}

// ======================== Session & Stream Management ========================

uint32_t SfuServerFull::CreateSession(const std::string& user_id, const std::string& stream_id, int fd) {
    // 同一 user+stream 如果已存在旧 session，先销毁，不复用 ID
    // （session_id 保持严格单调递增，不回收不复用）
    // 注意：只销毁相同传输类型（UDP/TCP）的旧会话——同一客户端会同时建立
    // UDP 主会话和 TCP 控制信令会话（user_id+stream_id 相同），不能互相踢掉
    bool newIsUdp = (fd < 0);
    if (newIsUdp) {
        // UDP 订阅请求可能因丢包被客户端重发（幂等重试）：
        // 复用同 user+stream 的既有 UDP 会话，避免误踢客户端正在使用的会话
        for (const auto& pair : m_sessions) {
            if (pair.second->user_id == user_id && pair.second->stream_id == stream_id &&
                pair.second->fd < 0) {
                LogInfo("User %s re-subscribe for stream %s, reusing session %u",
                        user_id.c_str(), stream_id.c_str(), pair.first);
                return pair.first;
            }
        }
    }
    std::vector<uint32_t> stale_ids;
    for (const auto& pair : m_sessions) {
        if (pair.second->user_id == user_id && pair.second->stream_id == stream_id &&
            (pair.second->fd < 0) == newIsUdp) {
            stale_ids.push_back(pair.first);
        }
    }
    for (auto sid : stale_ids) {
        LogInfo("User %s reconnect for stream %s, destroying old session %u",
                user_id.c_str(), stream_id.c_str(), sid);
        DestroySession(sid);
    }

    uint32_t session_id = m_next_session_id++;

    auto session = std::make_unique<SfuSession>();
    session->session_id = session_id;
    session->fd = fd;
    session->owner = this;
    session->user_id = user_id;
    session->stream_id = stream_id;
    session->is_publisher = false;
    session->is_subscriber = false;
    session->subscribe_audio = false;
    session->subscribe_video = false;
    session->subscribe_data = false;
    session->create_time = std::chrono::steady_clock::now();
    session->last_active_time = session->create_time;
    session->bytes_sent = 0;
    session->bytes_recv = 0;
    session->frames_sent = 0;
    session->frames_recv = 0;

    // Set timeout based on connection type and role
    // UDP publisher: 30s (allow time for camera init + encoding)
    // UDP subscriber: 3s (should receive data continuously)
    // TCP: 3600s (1 hour)
    session->timeout_sec = (fd < 0) ? 30 : 3600;  // UDP: 30s, TCP: 1 hour

    // Initialize Pacer for all sessions (rate limiting)
    // Default bandwidth: 50 Mbps per session
    session->pacer = std::make_unique<Pacer>(50'000'000);  // 50 Mbps

    m_sessions[session_id] = std::move(session);

    if (fd >= 0) {
        m_fdToSession[fd] = session_id;
    }

    m_stats.active_sessions = static_cast<uint32_t>(m_sessions.size());

    LogDebug("Session created: %u (user=%s stream=%s fd=%d)",
             session_id, user_id.c_str(), stream_id.c_str(), fd);

    return session_id;
}

void SfuServerFull::DestroySession(uint32_t session_id) {
    // sessions 仅由 ev_loop 线程访问（单线程事件循环），无需持锁
    auto it = m_sessions.find(session_id);
    if (it == m_sessions.end()) return;

    auto* session = it->second.get();
    std::string stream_id = session->stream_id;
    std::string user_id = session->user_id;

    // Stop pacing timer before destroying the session
    if (session->pacing_timer_init) {
        ev_timer_stop(m_loop, &session->pacing_timer);
        session->pacing_timer_init = false;
    }

    // 从流中摘除
    auto sit = m_streams.find(stream_id);
    if (sit != m_streams.end()) {
        auto* stream = sit->second.get();

        if (stream->publisher_session_id == session_id) {
            // 推流端下线：通知所有订阅者流结束，并注销该流
            LogInfo("[SFU-STREAM] Publisher session %u offline, stream '%s' ends",
                    session_id, stream_id.c_str());
            auto end_msg = SerializeKick(session_id, kReasonStreamEnd);
            for (auto sub_id : stream->subscriber_ids) {
                auto sit2 = m_sessions.find(sub_id);
                if (sit2 == m_sessions.end()) continue;
                auto* sub = sit2->second.get();
                if (sub->fd >= 0) {
                    SendToClient(sub->fd, end_msg.data(), end_msg.size());
                } else if (sub->udp_server_fd >= 0) {
                    ::sendto(sub->udp_server_fd, end_msg.data(), end_msg.size(), MSG_DONTWAIT,
                             reinterpret_cast<const struct sockaddr*>(&sub->udp_client_addr),
                             sub->udp_client_alen);
                }
            }
            stream->subscriber_ids.clear();
            m_streams.erase(sit);
            m_stats.active_streams = static_cast<uint32_t>(m_streams.size());
            LogInfo("[SFU-STREAM] Stream '%s' removed (publisher offline)", stream_id.c_str());
        } else {
            stream->subscriber_ids.erase(session_id);
            // 无推流端且无订阅者时移除空流
            if (stream->publisher_session_id == 0 && stream->subscriber_ids.empty()) {
                m_streams.erase(sit);
                m_stats.active_streams = static_cast<uint32_t>(m_streams.size());
            }
        }
    }

    // UDP 推流端媒体会话被销毁（超时踢出/流删除）时，同步销毁并断开其 TCP
    // 控制信令会话：否则控制连接保持 ESTAB、服务端也不再回 PONG，
    // 推流端会继续盲发（曾实测 3.5 小时）；断开后客户端立即感知并自动重连
    // 注意：必须在 m_sessions.erase(it) 之前执行（erase 后 session 指针已失效）
    if (session->fd < 0 && session->is_publisher) {
        SfuSession* ctrl = FindSiblingSessionLocked(session);
        if (ctrl && ctrl->fd >= 0 && ctrl->is_ctrl_only) {
            int ctrl_fd = ctrl->fd;
            uint32_t ctrl_sid = ctrl->session_id;
            LogInfo("Publisher session %u offline, closing paired control session %u (fd=%d)",
                    session_id, ctrl_sid, ctrl_fd);
            DestroySession(ctrl_sid);
            if (GetConnManager()) {
                GetConnManager()->RemoveSession(ctrl_fd);
            }
        }
    }

    m_sessions.erase(it);
    m_stats.active_sessions = static_cast<uint32_t>(m_sessions.size());

    LogDebug("Session %u destroyed (user=%s stream=%s)", session_id,
             user_id.c_str(), stream_id.c_str());
}

SfuSession* SfuServerFull::GetSession(uint32_t session_id) {
    
    auto it = m_sessions.find(session_id);
    return (it != m_sessions.end()) ? it->second.get() : nullptr;
}

SfuSession* SfuServerFull::GetSessionByFd(int fd) {
    
    auto it = m_fdToSession.find(fd);
    if (it == m_fdToSession.end()) return nullptr;
    auto sit = m_sessions.find(it->second);
    return (sit != m_sessions.end()) ? sit->second.get() : nullptr;
}

bool SfuServerFull::RegisterStream(const std::string& stream_id, uint32_t session_id) {
    // 流名全局唯一：已存在则注册失败（调用方据此拒绝新推流端）
    if (m_streams.find(stream_id) != m_streams.end()) {
        return false;
    }

    auto stream = std::make_unique<SfuStream>();
    stream->stream_id = stream_id;
    stream->publisher_session_id = session_id;
    stream->create_time = std::chrono::steady_clock::now();
    stream->total_bytes_forwarded = 0;
    stream->total_frames_forwarded = 0;

    m_streams[stream_id] = std::move(stream);
    m_stats.active_streams = static_cast<uint32_t>(m_streams.size());

    LogInfo("Stream created: %s (publisher session=%u)", stream_id.c_str(), session_id);
    return true;
}

SfuStream* SfuServerFull::GetStream(const std::string& stream_id) {
    auto it = m_streams.find(stream_id);
    return (it != m_streams.end()) ? it->second.get() : nullptr;
}

void SfuServerFull::RemoveStreamIfEmpty(const std::string& stream_id) {
    auto it = m_streams.find(stream_id);
    if (it != m_streams.end()) {
        auto* stream = it->second.get();
        if (stream->publisher_session_id == 0 && stream->subscriber_ids.empty()) {
            m_streams.erase(it);
            m_stats.active_streams = static_cast<uint32_t>(m_streams.size());
            LogDebug("Stream removed (empty): %s", stream_id.c_str());
        }
    }
}

// ======================== Stream Forwarding ========================

void SfuServerFull::ForwardToSubscribers(uint32_t source_session_id, const std::string& stream_id,
                                         const uint8_t* payload, size_t len) {
    auto sit = m_streams.find(stream_id);
    if (sit == m_streams.end()) {
        SFU_DEBUG_LOG("ForwardToSubscribers: stream " << stream_id << " not found");
        return;
    }

    auto* stream = sit->second.get();

    // 仅用于 SFU_DEBUG_LOG（Release 下宏展开为空，需 maybe_unused 抑制警告）
    [[maybe_unused]] int subscriber_count = 0;

    SFU_DEBUG_LOG("ForwardToSubscribers: stream=" << stream_id
              << " subscribers=" << stream->subscriber_ids.size()
              << " source_session=" << source_session_id);

    // 在循环外一次性序列化 forward 包（所有 subscriber 发送相同内容）
    uint8_t* pool_buf = static_cast<uint8_t*>(m_udpPool.allocate());
    size_t forward_len = sfu::SerializeForwardDataTo(pool_buf, mempool::FixedMemoryPool::kBlockSize,
                                                     source_session_id, payload, len);
    const uint8_t* forward_data;
    size_t forward_size;
    std::vector<uint8_t> forward_msg_fallback;
    if (forward_len > 0) {
        forward_data = pool_buf;
        forward_size = forward_len;
    } else {
        m_udpPool.deallocate(pool_buf);
        pool_buf = nullptr;
        forward_msg_fallback = SerializeForwardData(source_session_id, payload, len);
        forward_data = forward_msg_fallback.data();
        forward_size = forward_msg_fallback.size();
    }

    // Forward to all subscribers of the stream（新模型：订阅即整流，无需按用户过滤）
    for (auto sub_id : stream->subscriber_ids) {
        if (sub_id == source_session_id) continue; // Skip sender

        auto it = m_sessions.find(sub_id);
        if (it == m_sessions.end()) {
            SFU_DEBUG_LOG("Subscriber session " << sub_id << " not found");
            continue;
        }

        auto* sub_session = it->second.get();

        SFU_DEBUG_LOG("Forwarding to: sub_session=" << sub_id
                  << " user=" << sub_session->user_id << " fd=" << sub_session->fd);

        // Pacer-aware send: immediate if tokens available, otherwise queued
        // and drained by the per-session pacing timer (keeps ordering)
        if (QueueOrSend(sub_session, forward_data, forward_size)) {
            sub_session->bytes_sent += forward_size;
            sub_session->frames_sent++;
            subscriber_count++;
            m_stats.total_bytes_out += forward_size;
            m_stats.total_frames_forwarded++;
            stream->total_bytes_forwarded += forward_size;
            stream->total_frames_forwarded++;
            SFU_DEBUG_LOG("Forwarded (or queued) to fd=" << sub_session->fd << " sub=" << sub_id);
        } else {
            SFU_DEBUG_LOG("Forward rejected for sub=" << sub_id << " (backpressure)");
        }
    }

    // 归还内存池
    if (pool_buf) {
        m_udpPool.deallocate(pool_buf);
    }

    SFU_DEBUG_LOG("ForwardToSubscribers completed: forwarded=" << subscriber_count);
}

// ======================== Pacing Send ========================

static void PacingTimerCb(struct ev_loop* loop, ev_timer* w, int revents) {
    auto* s = static_cast<SfuSession*>(w->data);
    if (s && s->owner) s->owner->DrainPendingSend(s);
}

// 实际上线发送：UDP 直接 sendto，TCP 走带长度前缀的 SendToClient
bool SfuServerFull::RawSendSession(SfuSession* s, const uint8_t* data, size_t len) {
    if (s->fd >= 0) {
        return SendToClient(s->fd, data, len) == 0;
    }
    if (s->udp_server_fd >= 0) {
        static std::atomic<uint64_t> udp_send_count{0};
        uint64_t n = ++udp_send_count;
        if (n % 500 == 1) {
            LogDebug("[UDP-SEND] session=%u -> %s:%u alen=%u errno_check",
                     s->session_id, inet_ntoa(s->udp_client_addr.sin_addr),
                     ntohs(s->udp_client_addr.sin_port), s->udp_client_alen);
        }
        ssize_t sent = ::sendto(s->udp_server_fd, data, len, MSG_DONTWAIT,
                                reinterpret_cast<const struct sockaddr*>(&s->udp_client_addr),
                                s->udp_client_alen);
        return sent == static_cast<ssize_t>(len);
    }
    return false;
}

static constexpr size_t kMaxPendingSendPackets = 1024;  // 订阅者过慢时的背压上限

// Pacer 感知发送：令牌足够立即发；不足则入队由定时器延迟发送（保持顺序）
// 排队中的包不扣 token，令牌在真正发送时扣除（每包恰好一次）
bool SfuServerFull::QueueOrSend(SfuSession* s, const uint8_t* data, size_t len) {
    if (s->pending_send.empty()) {
        bool can_send_now = true;
        if (s->pacer) {
            int64_t wait_us = s->pacer->timeUntilAvailable(len);
            if (wait_us > 0) {
                can_send_now = false;
                if (!s->pacing_timer_init) {
                    ev_timer_init(&s->pacing_timer, PacingTimerCb, 0.0, 0.0);
                    s->pacing_timer.data = s;
                    s->pacing_timer_init = true;
                }
                ev_timer_set(&s->pacing_timer, static_cast<double>(wait_us) / 1e6, 0.0);
                ev_timer_start(m_loop, &s->pacing_timer);
            }
        }
        if (can_send_now) {
            if (s->pacer) s->pacer->tryConsume(len);
            bool ok = RawSendSession(s, data, len);
            if (s->pacer) s->pacer->afterSend(len);
            return ok;
        }
    }

    // 排队（顺序保证 + 背压上限）
    if (s->pending_send.size() >= kMaxPendingSendPackets) return false;
    s->pending_send.emplace_back(data, data + len);
    return true;
}

// 定时器回调：按令牌可用情况逐个发送排队包
void SfuServerFull::DrainPendingSend(SfuSession* s) {
    while (!s->pending_send.empty()) {
        size_t sz = s->pending_send.front().size();
        if (s->pacer && !s->pacer->tryConsume(sz)) {
            // 令牌仍不足：按缺口令牌数重设定时器
            int64_t wait_us = s->pacer->timeUntilAvailable(sz);
            ev_timer_set(&s->pacing_timer,
                         static_cast<double>(wait_us > 0 ? wait_us : 1000) / 1e6, 0.0);
            ev_timer_start(m_loop, &s->pacing_timer);
            return;
        }
        std::vector<uint8_t> pkt = std::move(s->pending_send.front());
        s->pending_send.pop_front();
        RawSendSession(s, pkt.data(), pkt.size());
        if (s->pacer) s->pacer->afterSend(pkt.size());
    }
    if (s->pacing_timer_init) {
        ev_timer_stop(m_loop, &s->pacing_timer);
    }
}

// ======================== Send Helpers ========================

int SfuServerFull::SendToClient(int fd, const uint8_t* data, size_t len) {
    if (fd < 0 || !data || len == 0) return -1;

    // Find the ServerSocketInfo for this fd
    auto* connManager = GetConnManager();
    if (!connManager) {
        LOG(ERROR) << "[SFU-ERROR] ConnManager not available";
        return -1;
    }

    auto* session = connManager->GetSession(fd);
    if (!session) {
        LOG(ERROR) << "[SFU-ERROR] Session not found for fd=" << fd;
        return -1;
    }

    // Get the ServerSocketInfo from the session
    // The session->watcher.data points to ConnSession, but we need ServerSocketInfo
    // We'll store the send data in the session's sendQueue and handle it in WriteCb

    // Build the frame: [4-byte length prefix = len][len bytes payload]
    // data already contains the full SFU protocol message (header + content)
    // Length prefix encodes payload size only (NOT including the 4-byte prefix itself)
    size_t totalLen = 4 + len;
    static thread_local std::vector<uint8_t> buf;
    if (buf.size() < totalLen) buf.resize(totalLen);

    buf[0] = (len >> 24) & 0xFF;
    buf[1] = (len >> 16) & 0xFF;
    buf[2] = (len >>  8) & 0xFF;
    buf[3] =  len        & 0xFF;
    memcpy(buf.data() + 4, data, len);

    ssize_t n = ::send(fd, buf.data(), totalLen, MSG_NOSIGNAL);
    if (n > 0) {
        session->sendBytes += n;
    }

    if (n < static_cast<ssize_t>(totalLen)) {
        // 限速：持续 send 失败（对端关闭）时每包一条会刷屏
        static uint64_t last_partial_log_us = 0;
        uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now_us - last_partial_log_us >= 5'000'000) {
            LOG(ERROR) << "[SFU-ERROR] Partial send: sent=" << n
                       << " expected=" << totalLen << " fd=" << fd;
            last_partial_log_us = now_us;
        }
    }

    return (n > 0) ? 0 : -1;
}

int SfuServerFull::SendToClientQuick(int fd, const uint8_t* data, size_t len) {
    // 必须与 SendToClient 同一套帧格式：[4 字节长度 = 前缀之后的字节数][header][payload]。
    // 之前走 ConnManager::SendQuick：它把自身那 4 字节也算进长度字段，并额外再 prepend
    // 一个 header（而调用方传进来的 Serialize* 数据已自带 header），接收端按声明长度去读
    // 会固定多吃 4 字节 —— 整条连接的帧流从此永久错位。
    return SendToClient(fd, data, len);
}

int SfuServerFull::SendToUdp(ConnSession* session, const uint8_t* data, size_t len) {
    if (!session || !GetConnManager()) return -1;

    return GetConnManager()->Send(session, const_cast<uint8_t*>(data), len,
                                 static_cast<uint8_t>(MsgType::kCmd));
}

// ======================== Protocol Serialization ========================

std::vector<uint8_t> SfuServerFull::SerializeSubscribeResp(uint32_t session_id, uint8_t status, const std::string& reason) {
    return ::sfu::SerializeSubscribeResp(session_id, status, reason);
}

std::vector<uint8_t> SfuServerFull::SerializeForwardData(uint32_t source_session_id, const uint8_t* payload, size_t len) {
    return ::sfu::SerializeForwardData(source_session_id, payload, len);
}

std::vector<uint8_t> SfuServerFull::SerializeKick(uint32_t session_id, const std::string& reason) {
    std::vector<uint8_t> buf;
    buf.push_back(MakeHeader(PROTOCOL_VERSION, static_cast<uint8_t>(MsgType::kCmd)));
    buf.push_back(static_cast<uint8_t>(SfuCtrlType::kKick));

    // session_id
    buf.push_back((session_id >> 24) & 0xFF);
    buf.push_back((session_id >> 16) & 0xFF);
    buf.push_back((session_id >> 8) & 0xFF);
    buf.push_back(session_id & 0xFF);

    // reason
    uint32_t reasonLen = static_cast<uint32_t>(reason.size());
    buf.push_back((reasonLen >> 24) & 0xFF);
    buf.push_back((reasonLen >> 16) & 0xFF);
    buf.push_back((reasonLen >> 8) & 0xFF);
    buf.push_back(reasonLen & 0xFF);
    buf.insert(buf.end(), reason.begin(), reason.end());

    return buf;
}

// ======================== Timer & Cleanup ========================

void SfuServerFull::CleanupTimeoutSessions() {
    
    auto now = std::chrono::steady_clock::now();
    std::vector<uint32_t> to_remove;

    for (const auto& pair : m_sessions) {
        auto* session = pair.second.get();
        auto idle = std::chrono::duration_cast<std::chrono::seconds>(
            now - session->last_active_time).count();

        // 纯控制会话没有自己的数据流（订阅握手后即空闲），其存活完全由配对的
        // 媒体会话决定：媒体会话销毁时 DestroySession 会一并断开它，
        // 因此不按空闲超时踢出（否则每小时误踢一次导致推流端整条流重连）
        if (session->is_ctrl_only) continue;

        // Use session-specific timeout (UDP: 3s, TCP: 3600s)
        if (idle > session->timeout_sec) {
            to_remove.push_back(pair.first);
        }
    }

    for (auto sid : to_remove) {
        auto it = m_sessions.find(sid);
        if (it != m_sessions.end()) {
            LogInfo("Session %u timeout (user=%s, idle=%ds, type=%s)", 
                    sid, it->second->user_id.c_str(),
                    (int)std::chrono::duration_cast<std::chrono::seconds>(
                        now - it->second->last_active_time).count(),
                    it->second->fd < 0 ? "UDP" : "TCP");
            int fd = it->second->fd;
            DestroySession(sid);

            // Close connection via ConnManager
            if (fd >= 0 && GetConnManager()) {
                GetConnManager()->RemoveSession(fd);
            }
        }
    }
}

void SfuServerFull::CleanupEmptyStreams() {
    std::vector<std::string> to_remove;

    for (const auto& pair : m_streams) {
        if (pair.second->publisher_session_id == 0 && pair.second->subscriber_ids.empty()) {
            to_remove.push_back(pair.first);
        }
    }

    for (const auto& stream_id : to_remove) {
        m_streams.erase(stream_id);
    }

    if (!to_remove.empty()) {
        m_stats.active_streams = static_cast<uint32_t>(m_streams.size());
    }
}

// ======================== Logging ========================
// LogInfo/LogError/LogDebug 保留成员函数签名（74+ 调用点不变），内部转发到 glog。

void SfuServerFull::LogInfo(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LOG(INFO) << buf;
}

void SfuServerFull::LogDebug(const char* fmt, ...) {
    if (!m_config.verbose) return;

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    VLOG(1) << buf;
}

void SfuServerFull::LogError(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LOG(ERROR) << buf;
}

// ========== UDP FEC Handling（已废弃，SFU 不处理 FEC） ==========

// SFU 只负责转发 FEC 分片包，不解析、不重组
// FEC 重组是端侧（推流端/拉流端）的职责

void SfuServerFull::HandleUdpFragment(ConnInfo* conn, uint8_t* buf, ssize_t len) {
    (void)conn; (void)buf; (void)len;
}

// ======================== STUN Service ========================

void SfuServerFull::StunThreadFunc(int stunPort) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        LogError("STUN: failed to create socket");
        return;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(stunPort);
    addr.sin_addr.s_addr = inet_addr(m_config.listen_ip.c_str());

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LogError("STUN: failed to bind to %s:%d", m_config.listen_ip.c_str(), stunPort);
        close(fd);
        return;
    }

    m_stunSocketFd.store(fd);
    struct timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[512];
    uint8_t resp[64];

    while (m_running.load()) {
        struct sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                             reinterpret_cast<struct sockaddr*>(&clientAddr), &addrLen);
        if (n <= 0) continue;

        size_t respLen = m_stunService.HandlePacket(buf, static_cast<size_t>(n), clientAddr, resp, sizeof(resp));
        if (respLen > 0) {
            sendto(fd, resp, respLen, MSG_DONTWAIT,
                   reinterpret_cast<const struct sockaddr*>(&clientAddr), addrLen);
        }
    }

    close(fd);
    m_stunSocketFd.store(-1);
}

// ======================== P2P Signaling ========================

void SfuServerFull::HandleP2PRequest(ConnInfo* conn, const uint8_t* data, size_t len) {
    auto req = ParseP2PRequest(data, len);
    if (!req.valid) {
        LogDebug("P2P: invalid request from fd=%d", conn->fd);
        return;
    }

    

    // Find the requesting session
    SfuSession* requester = nullptr;
    for (auto& [sid, s] : m_sessions) {
        if (s->fd == conn->fd || (s->udp_client_addr.sin_port == conn->remoteAddr.addr.sin_port &&
                                   s->udp_client_addr.sin_addr.s_addr == conn->remoteAddr.addr.sin_addr.s_addr)) {
            requester = s.get();
            break;
        }
    }
    if (!requester) {
        LogDebug("P2P: session not found for fd=%d", conn->fd);
        return;
    }

    // Find the target by stream name: P2P peer is the publisher of the target stream
    SfuSession* target = nullptr;
    auto sit = m_streams.find(req.targetStreamId);
    if (sit != m_streams.end()) {
        target = GetSession(sit->second->publisher_session_id);
    }
    if (target && target == requester) {
        target = nullptr;  // 不能与自己打洞
    }

    if (!target) {
        LogInfo("P2P: stream '%s' has no publisher", req.targetStreamId.c_str());
        auto resp = SerializeP2PResponse(1, 0, {}, requester->reflexive_ip.empty() ?
            sfu::P2PAddress{inet_ntoa(requester->udp_client_addr.sin_addr), ntohs(requester->udp_client_addr.sin_port)} :
            sfu::P2PAddress{requester->reflexive_ip, requester->reflexive_port},
            0, static_cast<uint8_t>(m_streams.count(req.targetStreamId) ? m_streams.at(req.targetStreamId)->subscriber_ids.size() : 0));
        SendToClient(conn->fd, resp.data(), resp.size());
        return;
    }

    // Record P2P pairing
    requester->p2p_enabled = true;
    requester->p2p_peer_session_id = target->session_id;
    target->p2p_enabled = true;
    target->p2p_peer_session_id = requester->session_id;

    // Get subscriber count
    uint8_t subCount = 0;
    auto sit2 = m_streams.find(req.targetStreamId);
    if (sit2 != m_streams.end()) {
        subCount = static_cast<uint8_t>(sit2->second->subscriber_ids.size());
    }

    // Build addresses
    P2PAddress targetAddr;
    if (!target->reflexive_ip.empty()) {
        targetAddr = {target->reflexive_ip, target->reflexive_port};
    } else {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &target->udp_client_addr.sin_addr, ip, sizeof(ip));
        targetAddr = {ip, ntohs(target->udp_client_addr.sin_port)};
    }

    P2PAddress yourAddr;
    if (!requester->reflexive_ip.empty()) {
        yourAddr = {requester->reflexive_ip, requester->reflexive_port};
    } else {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &requester->udp_client_addr.sin_addr, ip, sizeof(ip));
        yourAddr = {ip, ntohs(requester->udp_client_addr.sin_port)};
    }

    // Send response to requester
    auto respToRequester = SerializeP2PResponse(0, target->session_id, targetAddr, yourAddr,
                                                 target->nat_type, subCount);
    SendToClient(conn->fd, respToRequester.data(), respToRequester.size());

    // Send candidate to target
    auto candToTarget = SerializeP2PCandidate(target->session_id, yourAddr.ip, yourAddr.port, 1, 100);
    if (target->fd >= 0) {
        SendToClient(target->fd, candToTarget.data(), candToTarget.size());
    }

    LogInfo("P2P: pairing session %u (%s) <-> session %u (%s) stream=%s subs=%u",
            requester->session_id, requester->user_id.c_str(),
            target->session_id, target->user_id.c_str(),
            req.targetStreamId.c_str(), subCount);
}

void SfuServerFull::HandleP2PConnectCheck(ConnInfo* conn, const uint8_t* data, size_t len) {
    auto cc = ParseP2PConnectCheck(data, len);
    if (!cc.valid) return;

    
    auto* session = GetSession(cc.peerSessionId);
    if (!session) return;

    LogInfo("P2P: connect check from session %u -> %u, success=%d rtt=%ums",
            session->session_id, cc.peerSessionId, cc.success, cc.rttMs);
}

void SfuServerFull::HandleP2PStatus(ConnInfo* conn, const uint8_t* data, size_t len) {
    auto rpt = ParseP2PStatus(data, len);
    if (!rpt.valid) return;

    
    auto* session = GetSession(rpt.peerSessionId);
    if (session) {
        session->p2p_phase = rpt.phase;
    }

    LogDebug("P2P: status from fd=%d peer=%u phase=%u loss=%u%% rtt=%ums quality=%u%%",
             conn->fd, rpt.peerSessionId, rpt.phase,
             static_cast<unsigned>(rpt.lossRate * 100.0 / 255.0),
             rpt.rttMs, rpt.qualityScore);
}

void SfuServerFull::SendP2PFallback(uint32_t session_id, uint8_t reason) {
    auto* session = GetSession(session_id);
    if (!session) return;

    auto msg = SerializeP2PFallback(reason, session_id);
    if (session->fd >= 0) {
        SendToClient(session->fd, msg.data(), msg.size());
    }
    LogInfo("P2P: fallback sent to session %u, reason=%u", session_id, reason);
}

// ======================== 遥控信令处理 ========================

void SfuServerFull::HandleCtrlData(ConnInfo* conn, uint8_t ctrlType, const uint8_t* data, size_t len) {
    // data[0]=ctrlType, data[1]=direction, data[2..5]=session_id, data[6..]=payload
    if (len < 6) {
        LogDebug("CtrlData: too short len=%zu from fd=%d", len, conn->fd);
        return;
    }

    uint8_t direction = data[1];
    uint32_t target_session_id = (data[2] << 24) | (data[3] << 16) | (data[4] << 8) | data[5];
    const uint8_t* payload = data + 6;
    size_t payload_len = len - 6;

    

    // 第一轮：精确匹配（TCP 按 fd，UDP 按完整 IP+端口）
    SfuSession* sender = nullptr;
    for (auto& [sid, s] : m_sessions) {
        bool fdMatch = (!conn->isTcp && s->fd == -1) ? false : (s->fd == conn->fd);
        bool addrMatch = !conn->isTcp &&
                         s->udp_client_addr.sin_port == conn->remoteAddr.addr.sin_port &&
                         s->udp_client_addr.sin_addr.s_addr == conn->remoteAddr.addr.sin_addr.s_addr;
        if (fdMatch || addrMatch) {
            sender = s.get();
            break;
        }
    }
    // 第二轮：NAT 端口漂移兜底 —— 仅当该 IP 只有一个 UDP 会话时按 IP 匹配
    if (!sender && !conn->isTcp) {
        SfuSession* ipCandidate = nullptr;
        int ipCount = 0;
        for (auto& [sid, s] : m_sessions) {
            if (s->fd == -1 &&
                s->udp_client_addr.sin_addr.s_addr == conn->remoteAddr.addr.sin_addr.s_addr) {
                ipCandidate = s.get();
                ipCount++;
            }
        }
        if (ipCount == 1) sender = ipCandidate;
    }
    if (!sender) {
        LogDebug("CtrlData: sender not found for fd=%d", conn->fd);
        return;
    }
    // 发送者规范化：TCP 控制专用会话（非订阅者）归一化为其 UDP 主会话，
    // 保证转发的 source_session_id 与媒体流一致（订阅端据此回传 upstream）
    if (sender->fd >= 0 && !sender->is_subscriber) {
        if (auto* sib = FindSiblingSessionLocked(sender)) {
            sender = sib;
        }
    }
    LogDebug("CtrlData: sender session=%u user=%s stream=%s direction=0x%02x target_session=%u",
            sender->session_id, sender->user_id.c_str(), sender->stream_id.c_str(),
            direction, target_session_id);

    // Build forwarded message: [header][ctrlType][direction][sender_session_id][payload]
    size_t msg_len = 1 + 1 + 1 + 4 + payload_len;
    std::vector<uint8_t> msg(msg_len);
    msg[0] = MakeHeader(PROTOCOL_VERSION, static_cast<uint8_t>(MsgType::kCmd));
    msg[1] = ctrlType;
    msg[2] = direction;
    msg[3] = (sender->session_id >> 24) & 0xFF;
    msg[4] = (sender->session_id >> 16) & 0xFF;
    msg[5] = (sender->session_id >> 8)  & 0xFF;
    msg[6] =  sender->session_id        & 0xFF;
    if (payload_len > 0) {
        memcpy(msg.data() + 7, payload, payload_len);
    }

    bool unreliable = (ctrlType == static_cast<uint8_t>(SfuCtrlType::kCtrlUnreliable));

    if (direction == 0x00) {
        // Downstream: pub → broadcast to all subs of the stream
        auto sit = m_streams.find(sender->stream_id);
        if (sit == m_streams.end() || !sit->second) {
            LogDebug("CtrlData: stream '%s' not found", sender->stream_id.c_str());
            return;
        }
        auto* stream = sit->second.get();
        int fwd_count = 0;
        for (auto sub_sid : stream->subscriber_ids) {
            if (sub_sid == sender->session_id) continue;
            auto it = m_sessions.find(sub_sid);
            if (it == m_sessions.end()) continue;
            auto* sub = it->second.get();
            sub->last_active_time = std::chrono::steady_clock::now();

            if (!unreliable && sub->fd < 0) {
                // 可靠消息优先走该订阅者的 TCP 控制会话（UDP 无法保证可靠）
                if (auto* tcpSib = FindSiblingSessionLocked(sub);
                    tcpSib && tcpSib->fd >= 0) {
                    tcpSib->last_active_time = sub->last_active_time;
                    SendToClient(tcpSib->fd, msg.data(), msg.size());
                    fwd_count++;
                    continue;
                }
            }
            if (unreliable && sub->udp_server_fd >= 0) {
                ::sendto(sub->udp_server_fd, msg.data(), msg.size(), MSG_DONTWAIT,
                         reinterpret_cast<const struct sockaddr*>(&sub->udp_client_addr),
                         sub->udp_client_alen);
            } else {
                RawSendSession(sub, msg.data(), msg.size());
            }
            fwd_count++;
        }
        LogDebug("CtrlData: downstream broadcast to %d subs, ctrlType=0x%02x payloadLen=%zu",
                 fwd_count, ctrlType, payload_len);
    } else {
        // Upstream: sub → specific pub
        auto it = m_sessions.find(target_session_id);
        if (it == m_sessions.end()) {
            LogDebug("CtrlData: upstream target session %u not found", target_session_id);
            return;
        }
        auto* target = it->second.get();
        target->last_active_time = std::chrono::steady_clock::now();

        if (!unreliable && target->fd < 0) {
            // 可靠消息优先走目标端的 TCP 控制会话
            if (auto* tcpSib = FindSiblingSessionLocked(target);
                tcpSib && tcpSib->fd >= 0) {
                tcpSib->last_active_time = target->last_active_time;
                SendToClient(tcpSib->fd, msg.data(), msg.size());
                LogDebug("CtrlData: upstream reliable → TCP sibling of session %u", target_session_id);
                return;
            }
        }
        if (unreliable && target->udp_server_fd >= 0) {
            ::sendto(target->udp_server_fd, msg.data(), msg.size(), MSG_DONTWAIT,
                     reinterpret_cast<const struct sockaddr*>(&target->udp_client_addr),
                     target->udp_client_alen);
        } else {
            RawSendSession(target, msg.data(), msg.size());
        }
        LogDebug("CtrlData: upstream to session %u, ctrlType=0x%02x payloadLen=%zu",
                 target_session_id, ctrlType, payload_len);
    }
}

SfuSession* SfuServerFull::FindSiblingSessionLocked(SfuSession* s) {
    bool sIsUdp = (s->fd < 0);
    // 优先配对相同 stream_id 的会话（推流端 UDP 主会话 ↔ 其 TCP 控制会话流名一致）
    for (auto& [sid, other] : m_sessions) {
        if (sid != s->session_id &&
            other->user_id == s->user_id && other->stream_id == s->stream_id &&
            (other->fd < 0) == !sIsUdp) {
            return other.get();
        }
    }
    // 退化为按 user_id 配对（同一用户只有一条流时等价）
    for (auto& [sid, other] : m_sessions) {
        if (sid != s->session_id &&
            other->user_id == s->user_id &&
            (other->fd < 0) == !sIsUdp) {
            return other.get();
        }
    }
    return nullptr;
}

} // namespace sfu
