#ifndef SFU_SERVER_BASE_H
#define SFU_SERVER_BASE_H

#include "SfuProtocol.h"
#include "SfuEvPool.h"

#include <netinet/in.h>
#include <string>
#include <unordered_map>
#include <cstdint>
#include <functional>
#include <thread>
#include <deque>

extern "C" {
#include <ev.h>
}

namespace sfu {

// Forward declarations
class SfuEventHandler;
class SfuConnManager;

// Per-server socket info
struct ServerSocketInfo {
    int fd = 0;
    std::string ip;
    int port = 0;
    bool isTcp = false;

    struct ev_io watcher;
    struct ev_loop* loop = nullptr;

    // Parent server reference for message dispatch
    class SfuServerBase* parentServer = nullptr;

    // TCP receive state machine
    uint8_t lengthPrefix[4];
    ssize_t prefixBytesRecv = 0;
    uint8_t* recvBuf = nullptr;
    ssize_t recvLen = 0;
    ssize_t allLen = 0;
    uint32_t msgsRecv = 0;  // 已收齐帧数：区分"首帧即非法"与"中途错位"
    
    // TCP send queue
    std::deque<std::pair<uint8_t*, size_t>> sendQueue;
    bool hasWriteWatcher = false;

    ~ServerSocketInfo();
};

// Business layer event handler interface
class SfuEventHandler {
public:
    virtual ~SfuEventHandler() = default;

    // Called when a connection message is received
    virtual bool OnConnect(uint8_t* data, size_t len, ConnInfo* conn) = 0;

    // Called when a data message is received
    virtual bool OnData(uint8_t* data, size_t len, ConnInfo* conn) = 0;

    // Called when a heartbeat message is received
    virtual bool OnHeartbeat(uint8_t* data, size_t len, ConnInfo* conn) = 0;

    // Called when a command message is received
    virtual bool OnCmd(uint8_t* data, size_t len, ConnInfo* conn) = 0;

    // Called when a disconnect message is received
    virtual void OnDisconnect(uint8_t* data, size_t len, ConnInfo* conn) = 0;

    // Called when a stream message is received (SFU-specific)
    virtual bool OnStream(uint8_t* data, size_t len, ConnInfo* conn) = 0;

    // Called when a stream control message is received (SFU-specific)
    virtual bool OnStreamCtrl(uint8_t* data, size_t len, ConnInfo* conn) = 0;

    // Called when a room control message is received (SFU-specific)
    virtual bool OnRoomCtrl(uint8_t* data, size_t len, ConnInfo* conn) = 0;

    // Called when any UDP packet is received (for session keepalive);
    // addr/addrLen is the current source address, so NAT rebinds can be followed
    virtual void OnUdpPacketReceived(uint32_t sessionId, const sockaddr_in& addr, socklen_t addrLen) {}

    // Periodic update callback
    virtual void OnUpdate(int intervalMs) {}
};

// SFU Server Base - manages listening sockets for UDP/TCP
class SfuServerBase {
public:
    SfuServerBase();
    virtual ~SfuServerBase();

    // Set the event handler for business logic callbacks
    void SetEventHandler(SfuEventHandler* handler);

    // Bind and start listening on a UDP socket
    int StartUdp(const std::string& ip, int port);

    // Bind and start listening on a TCP socket
    int StartTcp(const std::string& ip, int port);

    // Stop a specific socket
    void StopSocket(int fd);

    // Stop all sockets
    void StopAll();

    // Schedule this server to start on the event loop pool
    void EvStart();

    // Schedule this server to stop on the event loop pool
    void EvStop();

    // Called by SfuEvPool when it's time to start (runs in event loop thread)
    virtual void OnEvStart();

    // Called by SfuEvPool when it's time to stop (runs in event loop thread)
    virtual void OnEvStop();

    // Get the event loop (only valid after OnEvStart)
    struct ev_loop* GetLoop() const { return m_loop; }

    // Get connection manager
    SfuConnManager* GetConnManager() const { return m_connManager; }

    // Get event handler
    SfuEventHandler* GetEventHandler() const { return m_handler; }

protected:
    // libev callbacks (static, routed to instance)
    static void UdpRecvCb(struct ev_loop* loop, ev_io* w, int revents);
    static void TcpAcceptCb(struct ev_loop* loop, ev_io* w, int revents);
    static void TcpRecvCb(struct ev_loop* loop, ev_io* w, int revents);
    static void Timer500msCb(struct ev_loop* loop, ev_timer* w, int revents);
    static void Timer1000msCb(struct ev_loop* loop, ev_timer* w, int revents);

    // Internal methods
    void HandleUdpPacket(ServerSocketInfo* serverInfo);
    void HandleTcpAccept(ServerSocketInfo* listenInfo);
    void HandleTcpRecv(ServerSocketInfo* connInfo);
    
    // Virtual method for handling UDP fragments (overridden by SfuServerFull for FEC)
    virtual void HandleUdpFragment(ConnInfo* conn, uint8_t* buf, ssize_t len) { (void)conn; (void)buf; (void)len; }

    // Dispatch received message to event handler
    bool DispatchMessage(uint8_t* data, size_t len, ConnInfo* conn, uint8_t msgType);

    // Send data to a specific client
    void SendToClient(int fd, const uint8_t* data, size_t len);

    // Map session ID to client fd（仅由 ev_loop 线程访问，无锁）
    void RegisterSessionFd(uint32_t sessionId, int fd);
    void UnregisterSessionFd(uint32_t sessionId);
    int GetSessionFd(uint32_t sessionId);

    // Set non-blocking on a socket
    static int SetNonBlock(int fd);

    struct ev_loop* m_loop = nullptr;
    std::unordered_map<int, ServerSocketInfo*> m_socketMap;
    SfuEventHandler* m_handler = nullptr;
    SfuConnManager* m_connManager = nullptr;
    std::thread m_thread;

    // Timers
    ev_timer m_timer500ms;
    ev_timer m_timer1000ms;

    // Running flag
    std::atomic_bool m_running{false};

    // Key for event loop pool routing
    std::string m_key;

    // Session ID to client fd mapping（仅由 ev_loop 线程访问，无锁）
    std::unordered_map<uint32_t, int> m_sessionFdMap;
};

} // namespace sfu

#endif // SFU_SERVER_BASE_H
