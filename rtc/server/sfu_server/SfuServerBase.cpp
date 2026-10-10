#include "SfuServerBase.h"
#include "SfuConnManager.h"
#include "SfuKvStore.h"
#include "SfuSubscribeProtocol.h"
#include "SfuSockOpt.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <glog/logging.h>

#define MAX_UDP_PACKET 4096
#define TCP_LISTEN_BACKLOG 128

// Debug logging: VLOG(1) is only emitted when FLAGS_v >= 1 (set by --verbose)
#define SFU_DEBUG_LOG(...) VLOG(1) << "[SFU-DEBUG] " << __VA_ARGS__
#define SFU_DEBUG_TCP(...) VLOG(1) << "[TCP-DEBUG] " << __VA_ARGS__

namespace sfu {

ServerSocketInfo::~ServerSocketInfo() {
    if (loop && watcher.data) {
        ev_io_stop(loop, &watcher);
    }
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    delete[] recvBuf;
    recvBuf = nullptr;
}

SfuServerBase::SfuServerBase() {
    m_connManager = new SfuConnManager(this);
}

SfuServerBase::~SfuServerBase() {
    StopAll();
    delete m_connManager;
    m_connManager = nullptr;
}

void SfuServerBase::SetEventHandler(SfuEventHandler* handler) {
    m_handler = handler;
}

int SfuServerBase::SetNonBlock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int SfuServerBase::StartUdp(const std::string& ip, int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        LOG(ERROR) << "Failed to create UDP socket";
        return -1;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 尽力把接收缓冲提到 8MB（root 走 RCVBUFFORCE，否则钳制到 rmem_max）
    int rcvbuf = SetUdpRecvBuffer(fd);
    LOG(INFO) << "[SFU] UDP RCVBUF target=8388608 actual=" << rcvbuf << " bytes";

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG(ERROR) << "Failed to bind UDP socket to " << ip << ":" << port;
        close(fd);
        return -1;
    }

    SetNonBlock(fd);

    // Create server info
    ServerSocketInfo* serverInfo = new ServerSocketInfo;
    serverInfo->fd = fd;
    serverInfo->ip = ip;
    serverInfo->port = port;
    serverInfo->isTcp = false;
    serverInfo->parentServer = this;
    m_socketMap[fd] = serverInfo;

    m_key = "udp_" + ip + "_" + std::to_string(port);

    return fd;
}

int SfuServerBase::StartTcp(const std::string& ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG(ERROR) << "Failed to create TCP socket";
        return -1;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG(ERROR) << "Failed to bind TCP socket to " << ip << ":" << port;
        close(fd);
        return -1;
    }

    if (listen(fd, TCP_LISTEN_BACKLOG) < 0) {
        LOG(ERROR) << "Failed to listen on TCP socket";
        close(fd);
        return -1;
    }

    SetNonBlock(fd);

    // Create server info
    ServerSocketInfo* serverInfo = new ServerSocketInfo;
    serverInfo->fd = fd;
    serverInfo->ip = ip;
    serverInfo->port = port;
    serverInfo->isTcp = true;
    serverInfo->parentServer = this;
    m_socketMap[fd] = serverInfo;

    m_key = "tcp_" + ip + "_" + std::to_string(port);

    return fd;
}

void SfuServerBase::StopSocket(int fd) {
    auto it = m_socketMap.find(fd);
    if (it != m_socketMap.end()) {
        delete it->second;
        m_socketMap.erase(it);
    }
}

void SfuServerBase::StopAll() {
    // Only stop if we were actually running
    bool wasRunning = m_running.exchange(false);
    if (!wasRunning) {
        return;  // Already stopped or never started
    }

    // Stop timers - only if m_loop is valid
    if (m_loop) {
        // Check if timers are active before stopping them
        // ev_is_active returns true if the timer is currently active
        if (ev_is_active(&m_timer500ms)) {
            ev_timer_stop(m_loop, &m_timer500ms);
        }
        if (ev_is_active(&m_timer1000ms)) {
            ev_timer_stop(m_loop, &m_timer1000ms);
        }
        ev_break(m_loop, EVBREAK_ALL);
    }

    // Clean up all sockets
    for (auto& pair : m_socketMap) {
        delete pair.second;
    }
    m_socketMap.clear();

    // Wait for thread
    if (m_thread.joinable()) {
        m_thread.join();
    }

    m_loop = nullptr;
}

void SfuServerBase::EvStart() {
    SfuEvPool::Instance().ScheduleStart(m_key, this);
}

void SfuServerBase::EvStop() {
    SfuEvPool::Instance().ScheduleStop(m_key, this);
}

void SfuServerBase::OnEvStart() {
    if (m_running.load()) {
        return;
    }

    m_loop = SfuEvPool::Instance().GetLoop(m_key);
    if (!m_loop) {
        LOG(ERROR) << "Failed to get event loop for key: " << m_key;
        return;
    }

    // Register watchers for all sockets
    for (auto& pair : m_socketMap) {
        ServerSocketInfo* serverInfo = pair.second;
        serverInfo->loop = m_loop;
        serverInfo->watcher.data = serverInfo;

        if (serverInfo->isTcp) {
            ev_io_init(&serverInfo->watcher, TcpAcceptCb, serverInfo->fd, EV_READ);
        } else {
            ev_io_init(&serverInfo->watcher, UdpRecvCb, serverInfo->fd, EV_READ);
        }
        ev_io_start(m_loop, &serverInfo->watcher);
    }

    // Start periodic timers
    m_timer500ms.data = this;
    ev_timer_init(&m_timer500ms, Timer500msCb, 0.5, 0.5);
    ev_timer_start(m_loop, &m_timer500ms);

    m_timer1000ms.data = this;
    ev_timer_init(&m_timer1000ms, Timer1000msCb, 1.0, 1.0);
    ev_timer_start(m_loop, &m_timer1000ms);

    m_running.store(true);

    // Do NOT start a new thread here - the pool's loop is already running
    // The server's watchers are registered on the pool's event loop
}

void SfuServerBase::OnEvStop() {
    // Only stop if we were actually running
    bool wasRunning = m_running.exchange(false);
    if (!wasRunning) {
        return;
    }

    // Stop all watchers
    for (auto& pair : m_socketMap) {
        ServerSocketInfo* serverInfo = pair.second;
        if (serverInfo && m_loop) {
            ev_io_stop(m_loop, &serverInfo->watcher);
        }
    }

    // Stop timers - only if m_loop is valid
    if (m_loop) {
        if (ev_is_active(&m_timer500ms)) {
            ev_timer_stop(m_loop, &m_timer500ms);
        }
        if (ev_is_active(&m_timer1000ms)) {
            ev_timer_stop(m_loop, &m_timer1000ms);
        }
    }
}

void SfuServerBase::UdpRecvCb(struct ev_loop* loop, ev_io* w, int revents) {
    if (!(revents & EV_READ)) return;

    ServerSocketInfo* serverInfo = static_cast<ServerSocketInfo*>(w->data);
    if (!serverInfo || !serverInfo->parentServer) return;
    serverInfo->loop = loop;

    uint8_t buf[MAX_UDP_PACKET];
    struct sockaddr_in clientAddr{};
    socklen_t addrLen = sizeof(clientAddr);

    ssize_t n = recvfrom(serverInfo->fd, buf, MAX_UDP_PACKET, 0,
                         reinterpret_cast<struct sockaddr*>(&clientAddr), &addrLen);
    if (n <= 0 || n < 1) return;

    uint8_t header  = buf[0];
    uint8_t version = GetHeaderVersion(header);
    uint8_t msgType = GetHeaderMsgType(header);

    // 构造连接信息，传递客户端 UDP 地址
    ConnInfo conn;
    conn.fd     = serverInfo->fd;   // 服务端 UDP socket fd（用于 sendto）
    conn.isTcp  = false;
    conn.remoteAddr.fd       = serverInfo->fd;
    conn.remoteAddr.addr     = clientAddr;
    conn.remoteAddr.addrLen  = addrLen;

    // 检查协议版本
    if (version == PROTOCOL_VERSION) {
        // Update last active time for UDP session (any packet keeps session alive)
        if (msgType == static_cast<uint8_t>(MsgType::kCmd) && n >= 6) {
            // For kStreamData: [ctrlType(1)][sessionId(4)][payload...]
            uint8_t ctrlType = buf[1];
            if (ctrlType == 0x13 || ctrlType == 0x14) {  // kStreamData or kForwardData
                uint32_t sessionId = 0;
                memcpy(&sessionId, buf + 2, 4);
                
                // Notify handler to update session's last_active_time
                if (serverInfo->parentServer && serverInfo->parentServer->GetEventHandler()) {
                    serverInfo->parentServer->GetEventHandler()->OnUdpPacketReceived(
                        sessionId, clientAddr, addrLen);
                }
            }
        }
        
        // SFU 标准协议帧
        serverInfo->parentServer->DispatchMessage(buf + 1, n - 1, &conn, msgType);
    } else {
        // 非 SFU 协议（可能是 FEC 分片），交给子类处理
        if (serverInfo->parentServer->m_handler) {
            serverInfo->parentServer->HandleUdpFragment(&conn, buf, n);
        }
    }
}

void SfuServerBase::TcpAcceptCb(struct ev_loop* loop, ev_io* w, int revents) {
    if (revents & EV_READ) {
        ServerSocketInfo* listenInfo = static_cast<ServerSocketInfo*>(w->data);
        if (!listenInfo || !listenInfo->parentServer) return;

        struct sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);

        int connFd = accept(listenInfo->fd,
                            reinterpret_cast<struct sockaddr*>(&clientAddr), &addrLen);
        if (connFd < 0) {
            return;
        }

        SetNonBlock(connFd);

        // Create connection info
        ServerSocketInfo* connInfo = new ServerSocketInfo;
        connInfo->fd = connFd;
        connInfo->ip = inet_ntoa(clientAddr.sin_addr);
        connInfo->port = ntohs(clientAddr.sin_port);
        connInfo->isTcp = true;
        connInfo->loop = loop;
        connInfo->parentServer = listenInfo->parentServer;

        // Initialize receive state
        connInfo->prefixBytesRecv = 0;
        connInfo->recvLen = 0;
        connInfo->allLen = 0;

        // DON'T create ConnManager session here - watcher conflicts!
        // ConnManager session will be created on first data receive in TcpRecvCb

        // Register watcher (use TcpRecvCb for two-phase receive)
        connInfo->watcher.data = connInfo;
        ev_io_init(&connInfo->watcher, TcpRecvCb, connFd, EV_READ);
        ev_io_start(loop, &connInfo->watcher);
    }
}

// 统一拆连路径：通知业务层断开 → 释放 ConnManager 会话（其析构负责 close(fd)）→
// 必须 ev_io_stop 再 delete。不 stop 就 delete，fd 被复用后 libev 会回调已释放的
// watcher（use-after-free，曾表现为偶发 std::bad_alloc）。
static void CloseTcpConn(ServerSocketInfo* connInfo) {
    if (!connInfo) return;
    int fd = connInfo->fd;

    if (connInfo->parentServer && connInfo->parentServer->GetEventHandler()) {
        ConnInfo conn;
        conn.fd = fd;
        conn.isTcp = true;
        connInfo->parentServer->GetEventHandler()->OnDisconnect(nullptr, 0, &conn);
    }

    if (connInfo->parentServer && connInfo->parentServer->GetConnManager()) {
        connInfo->parentServer->GetConnManager()->RemoveSession(fd);
    }

    ev_io_stop(connInfo->loop, &connInfo->watcher);
    connInfo->fd = -1;  // fd 已由 ConnSession 析构关闭，避免重复 close
    delete connInfo;
}

void SfuServerBase::TcpRecvCb(struct ev_loop* loop, ev_io* w, int revents) {
    if (revents & EV_READ) {
        ServerSocketInfo* connInfo = static_cast<ServerSocketInfo*>(w->data);
        if (!connInfo || !connInfo->parentServer) return;

        // Create ConnManager session on first receive (to avoid watcher conflicts)
        auto* connManager = connInfo->parentServer->GetConnManager();
        if (connManager && !connManager->GetSession(connInfo->fd)) {
            NetAddr addr;
            addr.fd = connInfo->fd;
            // Fill sockaddr_in from the stored ip/port (simplified, not used for TCP)
            memset(&addr.addr, 0, sizeof(addr.addr));
            addr.addr.sin_family = AF_INET;
            addr.addr.sin_port = htons(connInfo->port);
            inet_pton(AF_INET, connInfo->ip.c_str(), &addr.addr.sin_addr);
            addr.addrLen = sizeof(addr.addr);
            
            connManager->CreateSession(connInfo->fd, true, addr, loop);
        }

        // Two-phase receive: first read 4-byte length prefix, then payload
        if (connInfo->prefixBytesRecv < 4) {
            ssize_t n = recv(connInfo->fd,
                             connInfo->lengthPrefix + connInfo->prefixBytesRecv,
                             4 - connInfo->prefixBytesRecv, 0);
            if (n <= 0) {
                // Connection closed or error
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    int fd = connInfo->fd;
                    
                    if (n == 0) {
                        LOG(INFO) << "[SFU] TCP connection closed by peer (TcpRecvCb): fd=" << fd;
                    } else {
                        LOG(ERROR) << "[SFU] TCP recv error (TcpRecvCb): fd=" << fd 
                                   << " errno=" << errno << " (" << strerror(errno) << ")";
                    }

                    CloseTcpConn(connInfo);
                }
                return;
            }
            connInfo->prefixBytesRecv += n;

            if (connInfo->prefixBytesRecv == 4) {
                uint32_t frameLen = ReadTcpFrameLen(connInfo->lengthPrefix);

                // 非法长度只断开这一条连接，绝不允许拿它去分配内存
                if (!IsValidTcpFrameLen(frameLen)) {
                    char raw[20];
                    snprintf(raw, sizeof(raw), "%02X %02X %02X %02X",
                             connInfo->lengthPrefix[0], connInfo->lengthPrefix[1],
                             connInfo->lengthPrefix[2], connInfo->lengthPrefix[3]);
                    LOG(ERROR) << "[SFU] TCP 帧长度非法，断开该连接: fd=" << connInfo->fd
                               << " peer=" << connInfo->ip << ":" << connInfo->port
                               << " len=" << frameLen << " prefix=[" << raw << "]"
                               << " msgsRecv=" << connInfo->msgsRecv
                               << (connInfo->msgsRecv == 0
                                       ? " (首帧即非法: 对端未按本协议发送)"
                                       : " (中途错位: 帧流已被带偏)");
                    CloseTcpConn(connInfo);
                    return;
                }

                connInfo->allLen = frameLen;
                connInfo->recvBuf = new uint8_t[connInfo->allLen];
                connInfo->recvLen = 0;
            }
        }

        if (connInfo->recvBuf && connInfo->recvLen < connInfo->allLen) {
            ssize_t n = recv(connInfo->fd,
                             connInfo->recvBuf + connInfo->recvLen,
                             connInfo->allLen - connInfo->recvLen, 0);
            if (n <= 0) {
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    int fd = connInfo->fd;
                    
                    if (n == 0) {
                        LOG(INFO) << "[SFU] TCP connection closed by peer (payload recv): fd=" << fd;
                    } else {
                        LOG(ERROR) << "[SFU] TCP recv error (payload recv): fd=" << fd 
                                   << " errno=" << errno << " (" << strerror(errno) << ")";
                    }

                    CloseTcpConn(connInfo);
                }
                return;
            }
            connInfo->recvLen += n;

            if (connInfo->recvLen == connInfo->allLen) {
                // Full message received, parse and dispatch
                if (connInfo->recvLen >= 1) {
                    uint8_t header = connInfo->recvBuf[0];
                    uint8_t version = GetHeaderVersion(header);
                    uint8_t msgType = GetHeaderMsgType(header);

                    SFU_DEBUG_TCP("Full message received: fd=" << connInfo->fd 
                              << " len=" << connInfo->recvLen 
                              << " msgType=" << static_cast<int>(msgType));

                    if (version == PROTOCOL_VERSION && connInfo->parentServer) {
                        ConnInfo conn;
                        conn.fd = connInfo->fd;
                        conn.isTcp = true;

                        connInfo->parentServer->DispatchMessage(
                            connInfo->recvBuf + 1, connInfo->recvLen - 1, &conn, msgType);
                    }
                }

                // Reset for next message
                connInfo->msgsRecv++;
                delete[] connInfo->recvBuf;
                connInfo->recvBuf = nullptr;
                connInfo->prefixBytesRecv = 0;
                connInfo->recvLen = 0;
                connInfo->allLen = 0;
            }
        }
    }
}

void SfuServerBase::Timer500msCb(struct ev_loop* loop, ev_timer* w, int revents) {
    SfuServerBase* server = static_cast<SfuServerBase*>(w->data);
    if (server && server->m_handler) {
        server->m_handler->OnUpdate(500);
    }
}

void SfuServerBase::Timer1000msCb(struct ev_loop* loop, ev_timer* w, int revents) {
    SfuServerBase* server = static_cast<SfuServerBase*>(w->data);
    if (server && server->m_handler) {
        server->m_handler->OnUpdate(1000);
    }
}

void SfuServerBase::HandleUdpPacket(ServerSocketInfo* serverInfo) {
    // Implemented in UdpRecvCb for simplicity
}

void SfuServerBase::HandleTcpAccept(ServerSocketInfo* listenInfo) {
    // Implemented in TcpAcceptCb for simplicity
}

void SfuServerBase::HandleTcpRecv(ServerSocketInfo* connInfo) {
    // Implemented in TcpRecvCb for simplicity
}

bool SfuServerBase::DispatchMessage(uint8_t* data, size_t len, ConnInfo* conn, uint8_t msgType) {
    if (!m_handler) {
        SFU_DEBUG_LOG("No handler registered");
        return false;
    }

    // Handle SFU-specific control messages first
    // NOTE: This is now handled by SfuServerFull's OnCmd handler
    // We skip the base class handling to avoid double-processing

    // Fall back to standard message handling
    switch (static_cast<MsgType>(msgType)) {
        case MsgType::kConnect:
            return m_handler->OnConnect(data, len, conn);
        case MsgType::kData:
            return m_handler->OnData(data, len, conn);
        case MsgType::kHeartbeat:
            return m_handler->OnHeartbeat(data, len, conn);
        case MsgType::kCmd:
            return m_handler->OnCmd(data, len, conn);
        case MsgType::kDisconnect:
            m_handler->OnDisconnect(data, len, conn);
            return false;
        case MsgType::kStream:
            return m_handler->OnStream(data, len, conn);
        case MsgType::kStreamCtrl:
            return m_handler->OnStreamCtrl(data, len, conn);
        case MsgType::kRoomCtrl:
            return m_handler->OnRoomCtrl(data, len, conn);
        default:
            return false;
    }
}

void SfuServerBase::SendToClient(int fd, const uint8_t* data, size_t len) {
    if (fd < 0 || !data || len == 0) {
        LOG(ERROR) << "[SFU] SendToClient: invalid params fd=" << fd;
        return;
    }

    SFU_DEBUG_LOG("SendToClient fd=" << fd << " len=" << len);

    // Send length prefix + data
    uint8_t header[4];
    uint32_t totalLen = static_cast<uint32_t>(len);
    header[0] = (totalLen >> 24) & 0xFF;
    header[1] = (totalLen >> 16) & 0xFF;
    header[2] = (totalLen >> 8) & 0xFF;
    header[3] = totalLen & 0xFF;

    ssize_t n1 = write(fd, header, 4);
    if (n1 != 4) {
        LOG(ERROR) << "[SFU] Failed to send header to fd=" << fd << " n1=" << n1;
        return;
    }

    ssize_t n2 = write(fd, data, len);
    if (n2 != static_cast<ssize_t>(len)) {
        LOG(ERROR) << "[SFU] Failed to send data to fd=" << fd << " n2=" << n2;
    } else {
        SFU_DEBUG_LOG("Sent " << len << " bytes to fd=" << fd);
    }
}

// 会话 fd 映射仅由 ev_loop 线程访问（单线程事件循环），无需加锁
void SfuServerBase::RegisterSessionFd(uint32_t sessionId, int fd) {
    m_sessionFdMap[sessionId] = fd;
}

void SfuServerBase::UnregisterSessionFd(uint32_t sessionId) {
    m_sessionFdMap.erase(sessionId);
}

int SfuServerBase::GetSessionFd(uint32_t sessionId) {
    auto it = m_sessionFdMap.find(sessionId);
    if (it != m_sessionFdMap.end()) {
        return it->second;
    }
    return -1;
}

} // namespace sfu
