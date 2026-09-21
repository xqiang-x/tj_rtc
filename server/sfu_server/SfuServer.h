#ifndef SFU_SERVER_FULL_H
#define SFU_SERVER_FULL_H

#include "SfuServerBase.h"
#include "SfuConnManager.h"
#include "SfuProtocol.h"
#include "SfuSubscribeProtocol.h"
#include "SfuP2PProtocol.h"
#include "StunService.h"
#include "proxy_receiver.h"
#include "Pacer.h"
#include "MemPool.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <atomic>
#include <memory>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>

// Forward declarations
struct ev_loop;

namespace sfu {

class SfuServerFull;

// Session information stored in SFU
struct SfuSession {
    uint32_t session_id;
    int fd;                              // -1 for UDP sessions
    std::string user_id;
    std::string stream_id;               // 推流端：本端注册的流名（全局唯一）；控制会话与主会话一致
    std::string subscribe_stream_id;     // 拉流端：订阅的目标流名
    bool is_publisher;                   // Can send stream
    bool is_subscriber;                  // Can receive stream
    bool is_ctrl_only = false;           // 纯控制信令会话（UDP 推流端的 TCP 配对会话）：不注册流、不收媒体
    bool subscribe_audio;
    bool subscribe_video;
    bool subscribe_data;
    std::chrono::steady_clock::time_point create_time;
    std::chrono::steady_clock::time_point last_active_time;
    uint64_t bytes_sent;
    uint64_t bytes_recv;
    uint64_t frames_sent;
    uint64_t frames_recv;
    
    // Timeout configuration
    int timeout_sec = 3;  // 由 CreateSession 设定：UDP 会话 30s（活跃推流/拉流），TCP 会话 3600s
    // UDP session fields (fd=-1 for UDP sessions)
    int              udp_server_fd   = -1;    // server-side UDP socket fd (for sendto)
    struct sockaddr_in udp_client_addr = {};  // UDP client address
    socklen_t        udp_client_alen = 0;    // address length
    
    // ProxyReceiver for UDP publishers (FEC filtering + NACK + cache)
    std::unique_ptr<fec_protocol::ProxyFrameReceiver> proxy_receiver;
    float current_fec_ratio = 0.25f;  // Tracked downstream FEC ratio for dynamic adjustment

    // PLI (key-frame request) throttle: at most one forwarded upstream per 500ms
    std::chrono::steady_clock::time_point last_pli_forward_time{};  // epoch = never forwarded
    uint64_t pli_forwarded = 0;
    uint64_t pli_throttled = 0;
    
    // Pacer for rate limiting (per-session bandwidth control)
    std::unique_ptr<Pacer> pacer;

    // Pacing send queue: packets waiting for Pacer tokens (event-loop thread only)
    std::deque<std::vector<uint8_t>> pending_send;
    ev_timer pacing_timer;
    bool pacing_timer_init = false;
    SfuServerFull* owner = nullptr;   // 供 pacing 定时器回调使用

    // P2P state
    bool p2p_enabled = false;
    uint32_t p2p_peer_session_id = 0;
    uint8_t p2p_phase = 0;  // migration phase 0-4
    std::string reflexive_ip;
    uint16_t reflexive_port = 0;
    uint8_t nat_type = 0;
};

// 流（Stream）：推流 uid 即流名，全服务唯一。
// 一条流只有一个推流端（受限：1 视频 + 1 音频 + 1 消息），多个拉流端。
struct SfuStream {
    std::string stream_id;
    uint32_t publisher_session_id = 0;   // 推流端会话（0 = 无推流端在线）
    std::unordered_set<uint32_t> subscriber_ids;   // 订阅该流的会话
    bool video_on = false;               // 推流端声明启用的通道
    bool audio_on = false;
    bool data_on = false;
    std::chrono::steady_clock::time_point create_time;
    uint64_t total_bytes_forwarded;
    uint64_t total_frames_forwarded;
};

// Server statistics
struct SfuStats {
    uint32_t total_connections = 0;
    uint32_t active_sessions = 0;
    uint32_t active_streams = 0;
    uint64_t total_bytes_in = 0;
    uint64_t total_bytes_out = 0;
    uint64_t total_frames_forwarded = 0;
    std::chrono::steady_clock::time_point start_time;
};

class SfuServerFull : public SfuServerBase, public SfuEventHandler {
public:
    struct Config {
        std::string listen_ip = "0.0.0.0";
        int listen_port = 9200;
        uint32_t max_clients = 1024;
        uint32_t max_payload_size = 10 * 1024 * 1024; // 10MB
        bool enable_tcp = true;
        bool enable_udp = false;
        int udp_port = 0;  // 0 means listen_port + 1
        int stun_port = 0;  // 0 means udp_port + 1
        bool enable_stats_print = true;
        int stats_interval_sec = 10;
        int session_timeout_sec = 3; // 3 seconds for UDP, 3600 for TCP
        bool verbose = false;
    };

    explicit SfuServerFull(const Config& config);
    ~SfuServerFull() override;

    // Start/stop server (uses SfuEvPool)
    int Start();
    void Stop();

    // Public APIs for management
    void PrintStats() const;
    void PrintStreamInfo(const std::string& stream_id) const;
    void PrintSessionInfo(uint32_t session_id) const;
    void PrintAllStreams() const;
    void PrintAllSessions() const;
    void PrintProxyStats() const;
    void KickSession(uint32_t session_id);
    void KickAllFromStream(const std::string& stream_id);
    SfuStats GetStats() const;
    
    // Pacer bandwidth control
    void SetSessionBandwidth(uint32_t session_id, uint64_t bandwidth_bps);
    void SetStreamBandwidth(const std::string& stream_id, uint64_t bandwidth_bps);
    
    // Proxy dynamic FEC adjustment
    void AdjustProxyFecRatio();
    std::chrono::steady_clock::time_point m_last_fec_adjust;

    // SfuEventHandler interface implementation
    bool OnConnect(uint8_t* data, size_t len, ConnInfo* conn) override;
    bool OnData(uint8_t* data, size_t len, ConnInfo* conn) override;
    bool OnHeartbeat(uint8_t* data, size_t len, ConnInfo* conn) override;
    bool OnCmd(uint8_t* data, size_t len, ConnInfo* conn) override;
    void OnDisconnect(uint8_t* data, size_t len, ConnInfo* conn) override;
    bool OnStream(uint8_t* data, size_t len, ConnInfo* conn) override;
    bool OnStreamCtrl(uint8_t* data, size_t len, ConnInfo* conn) override;
    bool OnRoomCtrl(uint8_t* data, size_t len, ConnInfo* conn) override;
    void OnUdpPacketReceived(uint32_t sessionId, const sockaddr_in& addr, socklen_t addrLen) override;
    void OnUpdate(int intervalMs) override;
    
    // Override UDP fragment handling for FEC reassembly
    void HandleUdpFragment(ConnInfo* conn, uint8_t* buf, ssize_t len) override;

private:
    // Connection management (delegates to SfuConnManager)
    void OnConnEstablished(ConnSession* session);
    void OnConnClosed(int fd);

    // Message handling (SFU business logic)
    void HandleSubscribeReq(ConnInfo* conn, const uint8_t* data, size_t len);
    void HandleStreamData(ConnInfo* conn, const uint8_t* data, size_t len);
    void HandleUnsubscribe(ConnInfo* conn, const uint8_t* data, size_t len);
    
    // ProxyReceiver setup
    void setupProxyCallbacks(SfuSession* session, const std::string& stream_id);

    // P2P signaling handlers
    void HandleP2PRequest(ConnInfo* conn, const uint8_t* data, size_t len);
    void HandleP2PConnectCheck(ConnInfo* conn, const uint8_t* data, size_t len);
    void HandleP2PStatus(ConnInfo* conn, const uint8_t* data, size_t len);
    void SendP2PFallback(uint32_t session_id, uint8_t reason);

    // 遥控信令处理
    void HandleCtrlData(ConnInfo* conn, uint8_t ctrlType, const uint8_t* data, size_t len);
    // 查找同一 user_id 且传输类型相反（UDP↔TCP）的配对会话（单线程，无锁）
    SfuSession* FindSiblingSessionLocked(SfuSession* s);

    // Session & stream management
    uint32_t CreateSession(const std::string& user_id, const std::string& stream_id, int fd);
    void DestroySession(uint32_t session_id);
    SfuSession* GetSession(uint32_t session_id);
    SfuSession* GetSessionByFd(int fd);
    bool RegisterStream(const std::string& stream_id, uint32_t session_id);  // 推流注册（流名冲突返回 false）
    SfuStream* GetStream(const std::string& stream_id);
    void RemoveStreamIfEmpty(const std::string& stream_id);

    // Stream forwarding
    void ForwardToSubscribers(uint32_t source_session_id, const std::string& stream_id,
                              const uint8_t* payload, size_t len);

    // Paced send: honors per-session Pacer by queueing + ev_timer deferred sends
    bool RawSendSession(SfuSession* s, const uint8_t* data, size_t len);
    bool QueueOrSend(SfuSession* s, const uint8_t* data, size_t len);

public:
    // Called from the per-session pacing ev_timer (via static trampoline)
    void DrainPendingSend(SfuSession* s);

    // 服务器停止回调（ev_loop 线程执行）：停 watchers/timers 后清理会话与流。
    // 单线程模型：该回调之后 loop 线程不会再进入本服务器的任何回调。
    void OnEvStop() override;

private:

    // Send helpers (uses SfuConnManager)
    int SendToClient(int fd, const uint8_t* data, size_t len);
    int SendToClientQuick(int fd, const uint8_t* data, size_t len);
    int SendToUdp(ConnSession* session, const uint8_t* data, size_t len);

    // Protocol serialization
    std::vector<uint8_t> SerializeSubscribeResp(uint32_t session_id, uint8_t status, const std::string& reason);
    std::vector<uint8_t> SerializeForwardData(uint32_t source_session_id, const uint8_t* payload, size_t len);
    std::vector<uint8_t> SerializeKick(uint32_t session_id, const std::string& reason);

    // Cleanup
    void CleanupTimeoutSessions();
    void CleanupEmptyStreams();

    // Logging
    void LogInfo(const char* fmt, ...);
    void LogDebug(const char* fmt, ...);
    void LogError(const char* fmt, ...);

    Config m_config;

    // Data structures (SFU-specific, on top of SfuConnManager)
    std::unordered_map<uint32_t, std::unique_ptr<SfuSession>> m_sessions;  // session_id -> session
    std::unordered_map<int, uint32_t> m_fdToSession;                        // fd -> session_id
    std::unordered_map<std::string, std::unique_ptr<SfuStream>> m_streams;  // stream_id -> stream

    // 单线程事件循环模型：sessions/streams/stats 仅由 ev_loop 线程访问，无需加锁
    //（Stop 时通过 OnEvStop 在同一线程内清理，避免跨线程回收）
    uint32_t m_next_session_id;
    SfuStats m_stats;
    std::atomic_bool m_running{false};   // 跨线程可见（main 控制线程 & STUN 线程）
    
    // UDP 发送包内存池（单线程事件循环中使用，无锁）
    mempool::FixedMemoryPool m_udpPool{512};  // 预分配 512 块 * 1500 字节

    // STUN service
    std::atomic<int> m_stunSocketFd{-1};   // STUN 线程与主线程共享
    StunService m_stunService;
    std::thread m_stunThread;
    void StunThreadFunc(int stunPort);
};

} // namespace sfu

#endif // SFU_SERVER_FULL_H
