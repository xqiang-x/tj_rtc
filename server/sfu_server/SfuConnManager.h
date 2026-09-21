#ifndef SFU_CONN_MANAGER_H
#define SFU_CONN_MANAGER_H

#include "SfuProtocol.h"

#include <string>
#include <unordered_map>
#include <list>
#include <mutex>
#include <functional>
#include <cstdint>

extern "C" {
#include <ev.h>
}

namespace sfu {

// Forward declaration
class SfuServerBase;

// Send buffer item
struct SendBufItem {
    uint8_t* data = nullptr;
    size_t len = 0;
    size_t sentBytes = 0;
    bool isTcp = false;

    ~SendBufItem() {
        delete[] data;
    }
};

// Per-connection info for active connections
struct ConnSession {
    int fd = 0;
    bool isTcp = false;
    NetAddr remoteAddr;
    void* userData = nullptr;
    
    // Back pointer to server (for disconnect callback)
    void* serverPtr = nullptr;

    // Send queue
    std::list<SendBufItem*> sendQueue;
    bool hasWriteWatcher = false;

    // TCP receive state machine
    uint8_t lengthPrefix[4];
    ssize_t prefixBytesRecv = 0;
    uint8_t* recvBuf = nullptr;
    ssize_t recvLen = 0;
    ssize_t allLen = 0;
    uint32_t msgsRecv = 0;  // 已收齐帧数：区分"首帧即非法"与"中途错位"

    // Watcher
    struct ev_io watcher;
    struct ev_loop* loop = nullptr;

    // Cleanup callback
    std::function<void(void*)> endCallback;

    // Bandwidth stats
    uint64_t sendBytes = 0;
    uint64_t recvBytes = 0;
    uint64_t lastSendBytes = 0;
    uint64_t lastRecvBytes = 0;

    ~ConnSession();
};

// Connection Manager - manages active client connections
class SfuConnManager {
public:
    explicit SfuConnManager(SfuServerBase* server);
    ~SfuConnManager();

    // Create a new connection session
    ConnSession* CreateSession(int fd, bool isTcp, const NetAddr& addr, struct ev_loop* loop);

    // Remove and destroy a session
    void RemoveSession(int fd);

    // Remove session by address (for UDP dedup)
    void RemoveSessionByAddr(const std::string& addrKey);

    // Get session by fd
    ConnSession* GetSession(int fd);

    // Get session by address key
    ConnSession* GetSessionByAddr(const std::string& addrKey);

    // Send data (queued for TCP, immediate for UDP)
    int Send(ConnSession* session, uint8_t* data, size_t len, uint8_t msgType,
             uint8_t version = PROTOCOL_VERSION, bool queued = true);

    // Quick send (immediate, no queuing)
    int SendQuick(ConnSession* session, uint8_t* data, size_t len, uint8_t msgType,
                  uint8_t version = PROTOCOL_VERSION);

    // Allocate send buffer with header space
    uint8_t* AllocSendBuffer(size_t payloadSize, uint8_t version = PROTOCOL_VERSION);

    // Enable write watcher for TCP (when data is queued)
    void EnableWriteWatcher(ConnSession* session);

    // Disable write watcher for TCP (when queue is empty)
    void DisableWriteWatcher(ConnSession* session);

    // Get session count
    size_t GetSessionCount() const { return m_sessionMap.size(); }

    // Get server reference
    SfuServerBase* GetServer() const { return m_server; }

    // libev write callback (static)
    static void WriteCb(struct ev_loop* loop, ev_io* w, int revents);

    // libev read callback (static)
    static void ReadCb(struct ev_loop* loop, ev_io* w, int revents);

private:
    // Drain send queue for a session
    void DrainSendQueue(ConnSession* session);

    // Handle received data
    void HandleRecvData(ConnSession* session);

    SfuServerBase* m_server;
    std::unordered_map<int, ConnSession*> m_sessionMap;      // fd -> session
    std::unordered_map<std::string, int> m_addrFdMap;        // addr key -> fd
    std::mutex m_mutex;
};

} // namespace sfu

#endif // SFU_CONN_MANAGER_H
