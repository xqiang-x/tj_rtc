#ifndef SFU_FRAME_DISPATCHER_H
#define SFU_FRAME_DISPATCHER_H

// FrameDispatcher：IPC 帧派发器（UDS 服务端，libev ev_io）
//
// 职责：
//  - 监听 Unix domain socket，接受 MoQ 网关连接（FrameIpc.h 协议）
//  - 订阅表（按流引用计数）：SUB/UNSUB/断连维护；供 SFU 查询"该流是否有网关订阅者"
//  - 帧 fan-out：FrameAssembler 输出的完整帧 → 序列化一次 → 写入所有订阅连接
//  - 写缓冲背压：缓冲超过高水位时丢弃非关键帧（保 IDR/参数帧），硬上限内可丢关键帧
//  - PLI 汇聚：网关关键帧请求按流节流后经 PliHandler 转上游（SFU 侧 requestKeyFrame）
//
// 线程语义：全部方法仅在 SFU ev_loop 线程调用（SFU 的组帧回调与 ev_io 回调同线程），
// 无锁；测试可用 ev_run(loop, EVRUN_NOWAIT) 单线程驱动。

#include "FrameIpc.h"
#include "types.h"

extern "C" {
#include <ev.h>
}

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sfu {

class FrameDispatcher {
public:
    // 向上游推流端请求关键帧（按流名）；返回是否已发出
    using PliHandler = std::function<bool(const std::string& stream_id)>;

    struct Options {
        uint32_t pli_min_interval_ms = 500;        // 每流 PLI 节流间隔（与 SFU 下行 PLI 链一致）
        uint32_t conn_high_watermark = 2 * 1024 * 1024;   // 超此积压丢非关键帧
        uint32_t conn_hard_limit     = 8 * 1024 * 1024;   // 超此积压关键帧也丢
        uint32_t max_conns           = 8;          // 网关连接数上限
        uint32_t max_subs_per_conn   = 64;         // 单连接订阅数上限
    };

    struct Stats {
        uint64_t conns_accepted = 0;
        uint64_t conns_closed = 0;
        uint64_t frames_dispatched = 0;          // 至少写入一个订阅者
        uint64_t frames_no_subscriber = 0;       // 无订阅者
        uint64_t frames_dropped_backpressure = 0;// 背压丢弃（按连接计）
        uint64_t frames_too_large = 0;           // 超 kMaxMessageSize 无法编码
        uint64_t bytes_written = 0;
        uint64_t pli_received = 0;
        uint64_t pli_forwarded = 0;
        uint64_t pli_throttled = 0;
        uint64_t parse_errors = 0;
    };

    // loop/sock_path 与 pli_handler 均由调用方保证在 dispatcher 生命周期内有效
    FrameDispatcher(struct ev_loop* loop, const std::string& sock_path,
                    PliHandler pli_handler);
    FrameDispatcher(struct ev_loop* loop, const std::string& sock_path,
                    PliHandler pli_handler, const Options& opts);
    ~FrameDispatcher();

    FrameDispatcher(const FrameDispatcher&) = delete;
    FrameDispatcher& operator=(const FrameDispatcher&) = delete;

    // 创建并监听 UDS（会尝试创建父目录、清理陈旧 socket）；失败返回 false
    bool start();
    // 关闭监听与所有连接，解除 sock 路径（幂等）
    void stop();

    // 该流是否至少有一个 IPC 订阅者（SFU 据此决定是否投喂组帧器）
    bool isStreamActive(const std::string& stream_id) const;

    // 组帧器输出的完整帧 → 扇出到该流全部订阅连接
    void dispatchFrame(const std::string& stream_id, fec_protocol::Frame&& frame);

    const Stats& getStats() const { return m_stats; }
    size_t connCount() const { return m_conns.size(); }
    size_t activeStreamCount() const { return m_streams.size(); }

private:
    struct IpcConn;
    struct StreamState {
        std::string name;
        uint32_t sid = 0;
        uint32_t refcount = 0;
        std::unordered_set<int> subscriber_fds;
        uint64_t next_seq = 0;
        uint64_t last_pli_us = 0;
    };

    static void AcceptCb(struct ev_loop* loop, ev_io* w, int revents);
    static void ReadCb(struct ev_loop* loop, ev_io* w, int revents);
    static void WriteCb(struct ev_loop* loop, ev_io* w, int revents);

    void handleAccept();
    void handleRead(IpcConn* conn);
    void handleWrite(IpcConn* conn);
    bool processMessage(IpcConn* conn, const uint8_t* msg, size_t len);
    void handleSub(IpcConn* conn, const char* name, uint16_t name_len);
    void handleUnsub(IpcConn* conn, uint32_t sid);
    void handlePli(IpcConn* conn, uint32_t sid);
    void closeConn(int fd);

    uint32_t sidForName(const std::string& name, bool create);
    void queueBytes(IpcConn* conn, const uint8_t* data, size_t len);
    void scheduleWrite(IpcConn* conn);

    struct ev_loop* m_loop = nullptr;
    std::string m_sock_path;
    PliHandler m_pli_handler;
    Options m_opts;

    int m_listen_fd = -1;
    ev_io m_accept_watcher{};
    bool m_accept_active = false;

    std::unordered_map<int, std::unique_ptr<IpcConn>> m_conns;

    // 流名 → sid（持久，保证 sid 稳定）；sid → 订阅状态（refcount==0 时清除）
    std::unordered_map<std::string, uint32_t> m_name_to_sid;
    std::unordered_map<uint32_t, StreamState> m_streams;
    uint32_t m_next_sid = 1;

    std::vector<uint8_t> m_scratch;  // DATA 消息序列化复用缓冲（单线程）

    Stats m_stats;
};

} // namespace sfu

#endif // SFU_FRAME_DISPATCHER_H