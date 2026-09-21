#include "SfuConnManager.h"
#include "SfuServerBase.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <glog/logging.h>

namespace sfu {

ConnSession::~ConnSession() {
    // Stop watcher if still active
    if (loop && watcher.data) {
        ev_io_stop(loop, &watcher);
    }

    // Close fd
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }

    // Free receive buffer
    delete[] recvBuf;
    recvBuf = nullptr;

    // Clear send queue
    for (auto* item : sendQueue) {
        delete item;
    }
    sendQueue.clear();

    // Call cleanup callback
    if (endCallback && userData) {
        endCallback(userData);
    }
}

SfuConnManager::SfuConnManager(SfuServerBase* server)
    : m_server(server) {
}

SfuConnManager::~SfuConnManager() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& pair : m_sessionMap) {
        delete pair.second;
    }
    m_sessionMap.clear();
    m_addrFdMap.clear();
}

ConnSession* SfuConnManager::CreateSession(int fd, bool isTcp, const NetAddr& addr,
                                            struct ev_loop* loop) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Check if session already exists
    auto it = m_sessionMap.find(fd);
    if (it != m_sessionMap.end()) {
        return it->second;
    }

    // For UDP, check by address to avoid duplicates
    std::string addrKey = addr.ToKey();
    auto addrIt = m_addrFdMap.find(addrKey);
    if (!isTcp && addrIt != m_addrFdMap.end()) {
        auto existingIt = m_sessionMap.find(addrIt->second);
        if (existingIt != m_sessionMap.end()) {
            return existingIt->second;
        }
    }

    // Create new session
    ConnSession* session = new ConnSession;
    session->fd = fd;
    session->isTcp = isTcp;
    session->remoteAddr = addr;
    session->loop = loop;
    session->prefixBytesRecv = 0;
    session->recvLen = 0;
    session->allLen = 0;
    session->serverPtr = m_server;  // Back pointer for disconnect callback

    // Initialize watcher
    session->watcher.data = session;
    ev_io_init(&session->watcher, ReadCb, fd, EV_READ);

    if (!isTcp) {
        // UDP: start ReadCb — there is no separate receive handler for UDP
        ev_io_start(loop, &session->watcher);
    }
    // TCP: do NOT start ReadCb here.
    // SfuServerBase::TcpRecvCb already has a read watcher on the same fd.
    // Starting a second EV_READ watcher causes both callbacks to compete for bytes,
    // corrupting the length-prefixed frame stream (e.g. header byte consumed by
    // ReadCb, TcpRecvCb reads ctrlType byte as header → wrong msgType → disconnect).

    // Store in maps
    m_sessionMap[fd] = session;
    m_addrFdMap[addrKey] = fd;

    return session;
}

void SfuConnManager::RemoveSession(int fd) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_sessionMap.find(fd);
    if (it != m_sessionMap.end()) {
        ConnSession* session = it->second;

        // Remove from address map
        std::string addrKey = session->remoteAddr.ToKey();
        m_addrFdMap.erase(addrKey);

        // Delete session (calls destructor which stops watcher and closes fd)
        delete session;
        m_sessionMap.erase(it);
    }
}

void SfuConnManager::RemoveSessionByAddr(const std::string& addrKey) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_addrFdMap.find(addrKey);
    if (it != m_addrFdMap.end()) {
        int fd = it->second;
        m_addrFdMap.erase(it);

        auto sessionIt = m_sessionMap.find(fd);
        if (sessionIt != m_sessionMap.end()) {
            delete sessionIt->second;
            m_sessionMap.erase(sessionIt);
        }
    }
}

ConnSession* SfuConnManager::GetSession(int fd) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_sessionMap.find(fd);
    if (it != m_sessionMap.end()) {
        return it->second;
    }
    return nullptr;
}

ConnSession* SfuConnManager::GetSessionByAddr(const std::string& addrKey) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_addrFdMap.find(addrKey);
    if (it != m_addrFdMap.end()) {
        int fd = it->second;
        auto sessionIt = m_sessionMap.find(fd);
        if (sessionIt != m_sessionMap.end()) {
            return sessionIt->second;
        }
    }
    return nullptr;
}

int SfuConnManager::Send(ConnSession* session, uint8_t* data, size_t len, uint8_t msgType,
                         uint8_t version, bool queued) {
    if (!session || !data || len == 0) {
        return -1;
    }

    // Build header
    uint8_t header = MakeHeader(version, msgType);

    if (session->isTcp) {
        if (queued) {
            // Queue for TCP send
            SendBufItem* item = new SendBufItem;

            // Allocate buffer: 4 bytes length + 1 byte header + payload
            size_t totalLen = 4 + 1 + len;
            item->data = new uint8_t[totalLen];
            item->len = totalLen;

            // Write length prefix (network byte order)：只编码前缀之后的字节数，
            // 不含这 4 字节自身。本函数会自行 prepend header，调用方须传不含 header 的载荷。
            uint32_t frameLen = static_cast<uint32_t>(1 + len);
            item->data[0] = (frameLen >> 24) & 0xFF;
            item->data[1] = (frameLen >> 16) & 0xFF;
            item->data[2] = (frameLen >> 8) & 0xFF;
            item->data[3] = frameLen & 0xFF;

            // Write header and payload
            item->data[4] = header;
            memcpy(item->data + 5, data, len);

            session->sendQueue.push_back(item);
            EnableWriteWatcher(session);
            return 0;
        } else {
            // Immediate send for TCP
            size_t totalLen = 4 + 1 + len;
            uint8_t* buf = new uint8_t[totalLen];

            // Length prefix：只编码前缀之后的字节数（见 queued 分支同名注释）
            uint32_t frameLen = static_cast<uint32_t>(1 + len);
            buf[0] = (frameLen >> 24) & 0xFF;
            buf[1] = (frameLen >> 16) & 0xFF;
            buf[2] = (frameLen >> 8) & 0xFF;
            buf[3] = frameLen & 0xFF;

            // Header and payload
            buf[4] = header;
            memcpy(buf + 5, data, len);

            ssize_t n = ::send(session->fd, buf, totalLen, MSG_NOSIGNAL);
            delete[] buf;
            if (n > 0) {
                session->sendBytes += n;
            }
            return (n < 0) ? -1 : 0;
        }
    } else {
        // UDP: immediate send
        uint8_t* buf = new uint8_t[1 + len];
        buf[0] = header;
        memcpy(buf + 1, data, len);

        ssize_t n = ::sendto(session->fd, buf, 1 + len, MSG_DONTWAIT,
                             reinterpret_cast<const struct sockaddr*>(&session->remoteAddr.addr),
                             session->remoteAddr.addrLen);
        delete[] buf;
        if (n > 0) {
            session->sendBytes += n;
        }
        return (n < 0) ? -1 : 0;
    }
}

int SfuConnManager::SendQuick(ConnSession* session, uint8_t* data, size_t len, uint8_t msgType,
                              uint8_t version) {
    return Send(session, data, len, msgType, version, false);
}

uint8_t* SfuConnManager::AllocSendBuffer(size_t payloadSize, uint8_t version) {
    // Allocate buffer with header space
    // Returns pointer past the header so caller can write payload directly
    uint8_t* buf = new uint8_t[1 + payloadSize];
    buf[0] = MakeHeader(version, 0); // msgType will be filled in by Send
    return buf + 1; // Return pointer past header
}

void SfuConnManager::EnableWriteWatcher(ConnSession* session) {
    if (!session || !session->loop || session->hasWriteWatcher) {
        return;
    }

    ev_io_stop(session->loop, &session->watcher);
    ev_io_set(&session->watcher, session->fd, EV_READ | EV_WRITE);
    ev_io_start(session->loop, &session->watcher);
    session->hasWriteWatcher = true;
}

void SfuConnManager::DisableWriteWatcher(ConnSession* session) {
    if (!session || !session->loop || !session->hasWriteWatcher) {
        return;
    }

    ev_io_stop(session->loop, &session->watcher);
    ev_io_set(&session->watcher, session->fd, EV_READ);
    ev_io_start(session->loop, &session->watcher);
    session->hasWriteWatcher = false;
}

void SfuConnManager::WriteCb(struct ev_loop* loop, ev_io* w, int revents) {
    if (revents & EV_WRITE) {
        ConnSession* session = static_cast<ConnSession*>(w->data);
        if (!session) return;

        // Drain the send queue
        while (!session->sendQueue.empty()) {
            SendBufItem* item = session->sendQueue.front();

            ssize_t n = ::send(session->fd,
                               item->data + item->sentBytes,
                               item->len - item->sentBytes,
                               MSG_DONTWAIT | MSG_NOSIGNAL);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                // Connection error (EPIPE, ECONNRESET, etc.) - close and cleanup
                LOG(ERROR) << "[SFU-ERROR] Send failed on fd=" << session->fd 
                           << " errno=" << errno << " (" << strerror(errno) << ")";
                
                // Clear send queue
                while (!session->sendQueue.empty()) {
                    delete session->sendQueue.front();
                    session->sendQueue.pop_front();
                }
                
                // Close fd
                int fd = session->fd;
                session->fd = -1;
                close(fd);

                // Trigger disconnect callback and free the session
                // (closed fd won't deliver events to ReadCb)
                if (session->serverPtr) {
                    SfuServerBase* server = static_cast<SfuServerBase*>(session->serverPtr);
                    if (server->GetEventHandler()) {
                        ConnInfo conn;
                        conn.fd = fd;
                        conn.isTcp = session->isTcp;
                        server->GetEventHandler()->OnDisconnect(nullptr, 0, &conn);
                    }
                    if (server->GetConnManager()) {
                        server->GetConnManager()->RemoveSession(fd);
                    }
                }
                return;
            }

            session->sendBytes += n;
            item->sentBytes += n;

            if (item->sentBytes == item->len) {
                session->sendQueue.pop_front();
                delete item;
            } else {
                break;
            }
        }

        // Disable write watcher if queue is empty
        if (session->sendQueue.empty()) {
            if (session->loop) {
                ev_io_stop(session->loop, &session->watcher);
                ev_io_set(&session->watcher, session->fd, EV_READ);
                ev_io_start(session->loop, &session->watcher);
            }
            session->hasWriteWatcher = false;
        }
    }
}

// 统一拆连路径。注意 RemoveSession 内部会 delete session，故其后不得再碰 session。
static void CloseTcpSession(ConnSession* session) {
    if (!session) return;
    int fd = session->fd;

    close(fd);
    session->fd = -1;

    while (!session->sendQueue.empty()) {
        delete session->sendQueue.front();
        session->sendQueue.pop_front();
    }

    SfuServerBase* server = session->serverPtr
        ? static_cast<SfuServerBase*>(session->serverPtr) : nullptr;
    if (server && server->GetEventHandler()) {
        ConnInfo conn;
        conn.fd = fd;
        conn.isTcp = session->isTcp;
        server->GetEventHandler()->OnDisconnect(nullptr, 0, &conn);
    }

    // 释放 session 自身（fd 已关闭；析构负责 recvBuf/queue）
    if (server && server->GetConnManager()) {
        server->GetConnManager()->RemoveSession(fd);
    }
}

void SfuConnManager::ReadCb(struct ev_loop* loop, ev_io* w, int revents) {
    if (revents & EV_READ) {
        ConnSession* session = static_cast<ConnSession*>(w->data);
        if (!session) return;

        // Two-phase receive for TCP
        if (session->isTcp) {
            // Read length prefix
            if (session->prefixBytesRecv < 4) {
                ssize_t n = recv(session->fd,
                                 session->lengthPrefix + session->prefixBytesRecv,
                                 4 - session->prefixBytesRecv, 0);
                if (n <= 0) {
                    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                        // Connection closed or error
                        int fd = session->fd;
                        
                        if (n == 0) {
                            LOG(INFO) << "[SFU] TCP connection closed by peer: fd=" << fd;
                        } else {
                            LOG(ERROR) << "[SFU] TCP recv error: fd=" << fd 
                                       << " errno=" << errno << " (" << strerror(errno) << ")";
                        }

                        CloseTcpSession(session);
                    }
                    return;
                }
                session->prefixBytesRecv += n;

                if (session->prefixBytesRecv == 4) {
                    uint32_t frameLen = ReadTcpFrameLen(session->lengthPrefix);

                    // 非法长度只断开这一条连接，绝不允许拿它去分配内存
                    if (!IsValidTcpFrameLen(frameLen)) {
                        char raw[20];
                        snprintf(raw, sizeof(raw), "%02X %02X %02X %02X",
                                 session->lengthPrefix[0], session->lengthPrefix[1],
                                 session->lengthPrefix[2], session->lengthPrefix[3]);
                        LOG(ERROR) << "[SFU] TCP 帧长度非法，断开该连接: fd=" << session->fd
                                   << " peer=" << session->remoteAddr.ToKey()
                                   << " len=" << frameLen << " prefix=[" << raw << "]"
                                   << " msgsRecv=" << session->msgsRecv
                                   << (session->msgsRecv == 0
                                           ? " (首帧即非法: 对端未按本协议发送)"
                                           : " (中途错位: 帧流已被带偏)");
                        CloseTcpSession(session);
                        return;
                    }

                    session->allLen = frameLen;
                    session->recvBuf = new uint8_t[session->allLen];
                    session->recvLen = 0;
                }
            }

            // Read payload
            if (session->recvBuf && session->recvLen < session->allLen) {
                ssize_t n = recv(session->fd,
                                 session->recvBuf + session->recvLen,
                                 session->allLen - session->recvLen, 0);
                if (n <= 0) {
                    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                        CloseTcpSession(session);
                    }
                    return;
                }
                session->recvLen += n;
                session->recvBytes += n;

                if (session->recvLen == session->allLen) {
                    // Full message received - dispatch
                    if (session->recvLen >= 1) {
                        uint8_t header = session->recvBuf[0];
                        uint8_t version = GetHeaderVersion(header);

                        if (version == PROTOCOL_VERSION) {
                            ConnInfo conn;
                            conn.fd = session->fd;
                            conn.isTcp = true;
                            conn.userData = session->userData;

                            // Dispatch to server's handler
                            // (Need server reference)
                        }
                    }

                    // Reset for next message
                    session->msgsRecv++;
                    delete[] session->recvBuf;
                    session->recvBuf = nullptr;
                    session->prefixBytesRecv = 0;
                    session->recvLen = 0;
                    session->allLen = 0;
                }
            }
        } else {
            // UDP receive
            uint8_t buf[4096];
            struct sockaddr_in clientAddr{};
            socklen_t addrLen = sizeof(clientAddr);

            ssize_t n = recvfrom(session->fd, buf, sizeof(buf), 0,
                                 reinterpret_cast<struct sockaddr*>(&clientAddr), &addrLen);
            if (n <= 0) {
                return;
            }
            session->recvBytes += n;
            
            // Update last active time for UDP session (any packet keeps session alive)
            // Need to find the session's SfuSession to update last_active_time
            // This will be done in the higher layer (HandleStreamData/OnCmd)

            if (n < 1) return;

            uint8_t header = buf[0];
            uint8_t version = GetHeaderVersion(header);

            if (version != PROTOCOL_VERSION) {
                return;
            }

            // Dispatch
            ConnInfo conn;
            conn.fd = session->fd;
            conn.isTcp = false;
            conn.userData = session->userData;
        }
    }
}

void SfuConnManager::DrainSendQueue(ConnSession* session) {
    while (!session->sendQueue.empty()) {
        SendBufItem* item = session->sendQueue.front();

        ssize_t n = ::send(session->fd,
                           item->data + item->sentBytes,
                           item->len - item->sentBytes,
                           MSG_DONTWAIT | MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket buffer full, will retry on next write event
                break;
            }
            // Connection error (EPIPE, ECONNRESET, etc.) - close and cleanup
            LOG(ERROR) << "[SFU-ERROR] Send failed in DrainSendQueue on fd=" << session->fd 
                       << " errno=" << errno << " (" << strerror(errno) << ")";
            
            // Clear send queue
            while (!session->sendQueue.empty()) {
                delete session->sendQueue.front();
                session->sendQueue.pop_front();
            }
            
            // Close fd - ReadCb will detect and trigger OnDisconnect
            int fd = session->fd;
            session->fd = -1;
            close(fd);
            
            return;
        }

        session->sendBytes += n;
        item->sentBytes += n;

        if (item->sentBytes == item->len) {
            // Fully sent
            session->sendQueue.pop_front();
            delete item;
        } else {
            // Partial send
            break;
        }
    }

    // Disable write watcher if queue is empty
    if (session->sendQueue.empty()) {
        DisableWriteWatcher(session);
    }
}

void SfuConnManager::HandleRecvData(ConnSession* session) {
    // Data handling is done in ReadCb
}

} // namespace sfu
