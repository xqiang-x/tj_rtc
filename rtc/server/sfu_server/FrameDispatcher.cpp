#include "FrameDispatcher.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <chrono>

#include <glog/logging.h>

namespace sfu {

namespace {

constexpr size_t kReadChunk = 16 * 1024;
constexpr size_t kInBufCompactThreshold = 64 * 1024;
constexpr size_t kOutBufCompactThreshold = 64 * 1024;

uint64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

// ── 连接对象 ────────────────────────────────────────────────────────────────

struct FrameDispatcher::IpcConn {
    int fd = -1;
    FrameDispatcher* owner = nullptr;
    ev_io rd{};
    ev_io wr{};
    bool wr_active = false;

    std::vector<uint8_t> in_buf;
    size_t in_off = 0;

    std::vector<uint8_t> out_buf;
    size_t out_off = 0;

    std::unordered_set<uint32_t> sub_sids;

    size_t backlog() const { return out_buf.size() - out_off; }
};

// ── 生命周期 ────────────────────────────────────────────────────────────────

FrameDispatcher::FrameDispatcher(struct ev_loop* loop, const std::string& sock_path,
                                 PliHandler pli_handler)
    : FrameDispatcher(loop, sock_path, std::move(pli_handler), Options{}) {}

FrameDispatcher::FrameDispatcher(struct ev_loop* loop, const std::string& sock_path,
                                 PliHandler pli_handler, const Options& opts)
    : m_loop(loop), m_sock_path(sock_path), m_pli_handler(std::move(pli_handler)),
      m_opts(opts) {}

FrameDispatcher::~FrameDispatcher() {
    stop();
}

bool FrameDispatcher::start() {
    if (m_listen_fd >= 0) return true;
    if (m_sock_path.empty()) {
        LOG(ERROR) << "[SFU-IPC] empty socket path";
        return false;
    }

    // 父目录（如 /run/tj_rtc）不存在则创建
    auto slash = m_sock_path.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        std::string dir = m_sock_path.substr(0, slash);
        struct stat st;
        if (stat(dir.c_str(), &st) != 0) {
            if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
                LOG(ERROR) << "[SFU-IPC] mkdir " << dir << " failed: " << strerror(errno);
                return false;
            }
        }
    }

    // 陈旧 socket 文件清理：先探测是否已有活着的 dispatcher 占用
    {
        int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (probe >= 0) {
            struct sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, m_sock_path.c_str(), sizeof(addr.sun_path) - 1);
            if (connect(probe, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
                close(probe);
                LOG(ERROR) << "[SFU-IPC] another dispatcher already listening on " << m_sock_path;
                return false;
            }
            close(probe);
            unlink(m_sock_path.c_str());
        }
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LOG(ERROR) << "[SFU-IPC] socket failed: " << strerror(errno);
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (m_sock_path.size() >= sizeof(addr.sun_path)) {
        LOG(ERROR) << "[SFU-IPC] socket path too long: " << m_sock_path;
        close(fd);
        return false;
    }
    std::strncpy(addr.sun_path, m_sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOG(ERROR) << "[SFU-IPC] bind " << m_sock_path << " failed: " << strerror(errno);
        close(fd);
        return false;
    }
    if (listen(fd, 16) != 0) {
        LOG(ERROR) << "[SFU-IPC] listen failed: " << strerror(errno);
        close(fd);
        unlink(m_sock_path.c_str());
        return false;
    }

    m_listen_fd = fd;
    ev_io_init(&m_accept_watcher, AcceptCb, fd, EV_READ);
    m_accept_watcher.data = this;
    ev_io_start(m_loop, &m_accept_watcher);
    m_accept_active = true;

    LOG(INFO) << "[SFU-IPC] FrameDispatcher listening on " << m_sock_path;
    return true;
}

void FrameDispatcher::stop() {
    std::vector<int> fds;
    fds.reserve(m_conns.size());
    for (auto& kv : m_conns) fds.push_back(kv.first);
    for (int fd : fds) closeConn(fd);

    if (m_accept_active) {
        ev_io_stop(m_loop, &m_accept_watcher);
        m_accept_active = false;
    }
    if (m_listen_fd >= 0) {
        close(m_listen_fd);
        m_listen_fd = -1;
    }
    if (!m_sock_path.empty()) {
        unlink(m_sock_path.c_str());
    }
    m_name_to_sid.clear();
    m_streams.clear();
}

// ── 查询 / 派发 ─────────────────────────────────────────────────────────────

bool FrameDispatcher::isStreamActive(const std::string& stream_id) const {
    auto it = m_name_to_sid.find(stream_id);
    if (it == m_name_to_sid.end()) return false;
    return m_streams.find(it->second) != m_streams.end();
}

void FrameDispatcher::dispatchFrame(const std::string& stream_id,
                                    fec_protocol::Frame&& frame) {
    auto nit = m_name_to_sid.find(stream_id);
    if (nit == m_name_to_sid.end()) {
        m_stats.frames_no_subscriber++;
        return;
    }
    auto sit = m_streams.find(nit->second);
    if (sit == m_streams.end() || sit->second.refcount == 0) {
        m_stats.frames_no_subscriber++;
        return;
    }
    StreamState& st = sit->second;

    const bool is_key = (frame.type == fec_protocol::frame_type::VIDEO_IDR ||
                         frame.type == fec_protocol::frame_type::VIDEO_PARAMS);
    uint8_t flags = is_key ? frame_ipc::kFlagKey : 0;
    uint64_t pts_us = nowUs();
    uint64_t seq = st.next_seq++;

    // 序列化一次，多订阅者复用
    size_t cap = 4 + frame_ipc::kDataMsgSize + frame.data.size();
    m_scratch.resize(cap);
    size_t mlen = frame_ipc::serializeData(
        m_scratch.data(), cap, st.sid, frame.type, flags, pts_us, seq,
        frame.data.data(), static_cast<uint32_t>(frame.data.size()));
    if (mlen == 0) {
        m_stats.frames_too_large++;
        LOG(WARNING) << "[SFU-IPC] frame too large to encode: stream=" << stream_id
                     << " size=" << frame.size;
        return;
    }

    bool queued_any = false;
    for (int fd : st.subscriber_fds) {
        auto cit = m_conns.find(fd);
        if (cit == m_conns.end()) continue;
        IpcConn* c = cit->second.get();
        size_t bl = c->backlog();
        if (bl >= m_opts.conn_hard_limit) {
            m_stats.frames_dropped_backpressure++;
            continue;
        }
        // 高水位保关键帧丢非关键帧（IDR/参数帧是网关侧恢复解码的锚点）
        if (bl >= m_opts.conn_high_watermark && !is_key) {
            m_stats.frames_dropped_backpressure++;
            continue;
        }
        queueBytes(c, m_scratch.data(), mlen);
        queued_any = true;
    }
    if (queued_any) m_stats.frames_dispatched++;
}

// ── 连接管理 ────────────────────────────────────────────────────────────────

void FrameDispatcher::AcceptCb(struct ev_loop* loop, ev_io* w, int revents) {
    (void)loop;
    (void)revents;
    static_cast<FrameDispatcher*>(w->data)->handleAccept();
}

void FrameDispatcher::ReadCb(struct ev_loop* loop, ev_io* w, int revents) {
    (void)loop;
    (void)revents;
    auto* conn = static_cast<IpcConn*>(w->data);
    conn->owner->handleRead(conn);
}

void FrameDispatcher::WriteCb(struct ev_loop* loop, ev_io* w, int revents) {
    (void)loop;
    (void)revents;
    auto* conn = static_cast<IpcConn*>(w->data);
    conn->owner->handleWrite(conn);
}

void FrameDispatcher::handleAccept() {
    for (;;) {
        int cfd = accept4(m_listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            LOG(ERROR) << "[SFU-IPC] accept failed: " << strerror(errno);
            break;
        }
        if (m_conns.size() >= m_opts.max_conns) {
            LOG(WARNING) << "[SFU-IPC] conn limit (" << m_opts.max_conns
                         << ") reached, rejecting fd=" << cfd;
            close(cfd);
            continue;
        }
        auto conn = std::make_unique<IpcConn>();
        conn->fd = cfd;
        conn->owner = this;
        conn->rd.data = conn.get();
        ev_io_init(&conn->rd, ReadCb, cfd, EV_READ);
        ev_io_start(m_loop, &conn->rd);
        conn->wr.data = conn.get();
        ev_io_init(&conn->wr, WriteCb, cfd, EV_WRITE);
        m_stats.conns_accepted++;
        LOG(INFO) << "[SFU-IPC] gateway connected fd=" << cfd
                  << " conns=" << (m_conns.size() + 1);
        m_conns.emplace(cfd, std::move(conn));
    }
}

void FrameDispatcher::handleRead(IpcConn* conn) {
    uint8_t buf[kReadChunk];
    for (;;) {
        ssize_t n = read(conn->fd, buf, sizeof(buf));
        if (n > 0) {
            conn->in_buf.insert(conn->in_buf.end(), buf, buf + n);
            continue;
        }
        if (n == 0) {
            closeConn(conn->fd);
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        LOG(WARNING) << "[SFU-IPC] read error fd=" << conn->fd << ": " << strerror(errno);
        closeConn(conn->fd);
        return;
    }

    for (;;) {
        size_t avail = conn->in_buf.size() - conn->in_off;
        if (avail < 4) break;
        const uint8_t* p = conn->in_buf.data() + conn->in_off;
        uint32_t msg_len = frame_ipc::getU32(p);
        if (msg_len < 2 || msg_len > frame_ipc::kMaxMessageSize) {
            m_stats.parse_errors++;
            LOG(WARNING) << "[SFU-IPC] invalid msg_len=" << msg_len
                         << " fd=" << conn->fd << ", closing";
            closeConn(conn->fd);
            return;
        }
        if (avail < 4u + msg_len) break;

        const int cfd = conn->fd;
        processMessage(conn, p + 4, msg_len);
        if (m_conns.find(cfd) == m_conns.end()) return;  // 处理中被关闭
        conn->in_off += 4u + msg_len;
    }

    if (conn->in_off > 0) {
        if (conn->in_off == conn->in_buf.size()) {
            conn->in_buf.clear();
            conn->in_off = 0;
        } else if (conn->in_off >= kInBufCompactThreshold) {
            conn->in_buf.erase(conn->in_buf.begin(), conn->in_buf.begin() + conn->in_off);
            conn->in_off = 0;
        }
    }
}

void FrameDispatcher::handleWrite(IpcConn* conn) {
    while (conn->out_off < conn->out_buf.size()) {
        ssize_t n = write(conn->fd, conn->out_buf.data() + conn->out_off,
                          conn->out_buf.size() - conn->out_off);
        if (n > 0) {
            conn->out_off += static_cast<size_t>(n);
            m_stats.bytes_written += static_cast<uint64_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        LOG(WARNING) << "[SFU-IPC] write error fd=" << conn->fd << ": " << strerror(errno);
        closeConn(conn->fd);
        return;
    }
    conn->out_buf.clear();
    conn->out_off = 0;
    if (conn->wr_active) {
        ev_io_stop(m_loop, &conn->wr);
        conn->wr_active = false;
    }
}

void FrameDispatcher::closeConn(int fd) {
    auto it = m_conns.find(fd);
    if (it == m_conns.end()) return;
    IpcConn* conn = it->second.get();

    if (conn->wr_active) {
        ev_io_stop(m_loop, &conn->wr);
        conn->wr_active = false;
    }
    ev_io_stop(m_loop, &conn->rd);
    ::close(fd);

    for (uint32_t sid : conn->sub_sids) {
        auto sit = m_streams.find(sid);
        if (sit == m_streams.end()) continue;
        StreamState& st = sit->second;
        if (st.refcount > 0) st.refcount--;
        st.subscriber_fds.erase(fd);
        if (st.refcount == 0) {
            LOG(INFO) << "[SFU-IPC] stream=" << st.name << " sid=" << sid
                      << " no subscribers, assembly source idle";
            m_streams.erase(sit);
        }
    }
    conn->sub_sids.clear();

    LOG(INFO) << "[SFU-IPC] gateway disconnected fd=" << fd
              << " conns=" << (m_conns.size() - 1);
    m_conns.erase(it);
    m_stats.conns_closed++;
}

// ── 消息处理 ────────────────────────────────────────────────────────────────

bool FrameDispatcher::processMessage(IpcConn* conn, const uint8_t* msg, size_t len) {
    frame_ipc::Message m;
    if (!frame_ipc::parseMessage(msg, static_cast<uint32_t>(len), m) ||
        m.version != frame_ipc::kVersion) {
        m_stats.parse_errors++;
        return false;
    }

    switch (m.kind) {
        case frame_ipc::kSub: {
            frame_ipc::NameView nv;
            if (!frame_ipc::parseSub(m, nv)) {
                m_stats.parse_errors++;
                return false;
            }
            handleSub(conn, nv.name, nv.name_len);
            return true;
        }
        case frame_ipc::kUnsub: {
            uint32_t sid = 0;
            if (!frame_ipc::parseSid(m, sid)) {
                m_stats.parse_errors++;
                return false;
            }
            handleUnsub(conn, sid);
            return true;
        }
        case frame_ipc::kPli: {
            uint32_t sid = 0;
            if (!frame_ipc::parseSid(m, sid)) {
                m_stats.parse_errors++;
                return false;
            }
            handlePli(conn, sid);
            return true;
        }
        default:
            m_stats.parse_errors++;
            return false;
    }
}

void FrameDispatcher::handleSub(IpcConn* conn, const char* name, uint16_t name_len) {
    std::string sname(name, name_len);

    auto known = m_name_to_sid.find(sname);
    bool already = (known != m_name_to_sid.end() &&
                    conn->sub_sids.count(known->second) > 0);
    if (!already && conn->sub_sids.size() >= m_opts.max_subs_per_conn) {
        uint8_t buf[16 + frame_ipc::kMaxStreamName];
        size_t n = frame_ipc::serializeSubErr(buf, sizeof(buf), name, name_len,
                                              frame_ipc::kErrTooManyStreams);
        if (n > 0) queueBytes(conn, buf, n);
        LOG(WARNING) << "[SFU-IPC] too many subs on fd=" << conn->fd
                     << ", rejected stream=" << sname;
        return;
    }

    uint32_t sid = sidForName(sname, true);
    StreamState& st = m_streams[sid];
    if (st.sid == 0) {
        st.name = sname;
        st.sid = sid;
    }
    if (conn->sub_sids.insert(sid).second) {
        st.refcount++;
        st.subscriber_fds.insert(conn->fd);
    }

    uint8_t buf[16 + frame_ipc::kMaxStreamName];
    size_t n = frame_ipc::serializeSubOk(buf, sizeof(buf), sid, name, name_len);
    if (n > 0) queueBytes(conn, buf, n);

    LOG(INFO) << "[SFU-IPC] SUB stream=" << sname << " sid=" << sid
              << " refcount=" << st.refcount << " fd=" << conn->fd;
}

void FrameDispatcher::handleUnsub(IpcConn* conn, uint32_t sid) {
    if (conn->sub_sids.erase(sid) == 0) return;
    auto it = m_streams.find(sid);
    if (it == m_streams.end()) return;
    StreamState& st = it->second;
    if (st.refcount > 0) st.refcount--;
    st.subscriber_fds.erase(conn->fd);
    LOG(INFO) << "[SFU-IPC] UNSUB stream=" << st.name << " sid=" << sid
              << " refcount=" << st.refcount << " fd=" << conn->fd;
    if (st.refcount == 0) m_streams.erase(it);
}

void FrameDispatcher::handlePli(IpcConn* conn, uint32_t sid) {
    (void)conn;
    m_stats.pli_received++;
    auto it = m_streams.find(sid);
    if (it == m_streams.end()) return;
    StreamState& st = it->second;

    uint64_t now = nowUs();
    uint64_t min_interval_us = static_cast<uint64_t>(m_opts.pli_min_interval_ms) * 1000;
    if (st.last_pli_us != 0 && now - st.last_pli_us < min_interval_us) {
        m_stats.pli_throttled++;
        return;
    }
    st.last_pli_us = now;
    if (m_pli_handler && m_pli_handler(st.name)) {
        m_stats.pli_forwarded++;
        LOG(INFO) << "[SFU-IPC] PLI stream=" << st.name << " forwarded upstream";
    }
}

// ── 工具 ────────────────────────────────────────────────────────────────────

uint32_t FrameDispatcher::sidForName(const std::string& name, bool create) {
    auto it = m_name_to_sid.find(name);
    if (it != m_name_to_sid.end()) return it->second;
    if (!create) return 0;
    uint32_t sid = m_next_sid++;
    m_name_to_sid.emplace(name, sid);
    return sid;
}

void FrameDispatcher::queueBytes(IpcConn* conn, const uint8_t* data, size_t len) {
    if (conn->out_off > 0 &&
        (conn->out_off >= kOutBufCompactThreshold ||
         conn->out_buf.size() + len > 4 * 1024 * 1024)) {
        conn->out_buf.erase(conn->out_buf.begin(),
                            conn->out_buf.begin() + conn->out_off);
        conn->out_off = 0;
    }
    conn->out_buf.insert(conn->out_buf.end(), data, data + len);
    scheduleWrite(conn);
}

void FrameDispatcher::scheduleWrite(IpcConn* conn) {
    if (conn->wr_active) return;
    ev_io_start(m_loop, &conn->wr);
    conn->wr_active = true;
}

} // namespace sfu