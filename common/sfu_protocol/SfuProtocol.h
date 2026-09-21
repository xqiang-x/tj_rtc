#ifndef SFU_PROTOCOL_H
#define SFU_PROTOCOL_H

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string>

namespace sfu {

// Protocol version
constexpr uint8_t PROTOCOL_VERSION = 1;

// Message types (4 bits in header byte)
enum class MsgType : uint8_t {
    kData           = 0x01,
    kConnect        = 0x02,
    kHeartbeat      = 0x03,
    kDisconnect     = 0x04,
    kCmd            = 0x05,
    kConnectResult  = 0x06,
    kHeartbeatAck   = 0x07,
    kStream         = 0x08,
    kStreamCtrl     = 0x09,
    kRoomCtrl     = 0x0a,
};

// Header byte layout: 4 bits version | 4 bits msgType
inline uint8_t MakeHeader(uint8_t version, uint8_t msgType) {
    return ((version & 0x0F) << 4) | (msgType & 0x0F);
}

inline uint8_t GetHeaderVersion(uint8_t header) {
    return (header >> 4) & 0x0F;
}

inline uint8_t GetHeaderMsgType(uint8_t header) {
    return header & 0x0F;
}

// TCP 信令/媒体帧的权威帧格式（收发两端都必须遵守）：
//   [4 字节大端长度 N][N 字节载荷]，载荷第 0 字节就是协议 header 字节。
// 长度只编码"前缀之后的字节数"，绝不包含这 4 字节自身。
constexpr uint32_t kMinTcpFrameBytes = 1;
constexpr uint32_t kMaxTcpFrameBytes = 1 * 1024 * 1024;

// 无符号解析：必须用 uint32_t。曾按 int 做 <<24，首字节 >= 0x80 时结果为负，
// 赋给 ssize_t 后绕过 "> 1MB" 校验，new uint8_t[负数] 转成 ~18EB → bad_alloc
// 从 libev 回调抛到 ev_run 外 → std::terminate，任意对端 4 字节即可打死进程。
inline uint32_t ReadTcpFrameLen(const uint8_t* buf) {
    return (static_cast<uint32_t>(buf[0]) << 24) |
           (static_cast<uint32_t>(buf[1]) << 16) |
           (static_cast<uint32_t>(buf[2]) << 8) |
           static_cast<uint32_t>(buf[3]);
}

inline bool IsValidTcpFrameLen(uint32_t len) {
    return len >= kMinTcpFrameBytes && len <= kMaxTcpFrameBytes;
}

// Network address wrapper
struct NetAddr {
    int fd = 0;
    struct sockaddr_in addr{};
    socklen_t addrLen = sizeof(struct sockaddr_in);

    // Generate a string key from sockaddr for map lookups
    std::string ToKey() const {
        char buf[INET_ADDRSTRLEN + 6]; // ip:port\0
        const char* ip = inet_ntoa(addr.sin_addr);
        uint16_t port = ntohs(addr.sin_port);
        snprintf(buf, sizeof(buf), "%s:%u", ip, port);
        return std::string(buf);
    }

    bool operator==(const NetAddr& other) const {
        return addr.sin_addr.s_addr == other.addr.sin_addr.s_addr &&
               addr.sin_port == other.addr.sin_port;
    }
};

// Connection info passed to business layer callbacks
struct ConnInfo {
    int fd = 0;
    bool isTcp = false;
    NetAddr remoteAddr;
    void* userData = nullptr;

    // Call to prevent automatic fd closure after callback returns
    bool noClose = false;
    void KeepAlive() { noClose = true; }
};

// Utility: compute simple checksum
inline uint16_t ComputeChecksum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += data[i];
    }
    return static_cast<uint16_t>(sum & 0xFFFF);
}

} // namespace sfu

#endif // SFU_PROTOCOL_H
