#ifndef SFU_SUBSCRIBE_PROTOCOL_H
#define SFU_SUBSCRIBE_PROTOCOL_H

#include "SfuProtocol.h"

#include <cstdint>
#include <string>
#include <vector>
#include <cstring>

namespace sfu {

// ===== Subscribe Protocol Message Types =====
// These extend the base MsgType enum (0x10-0x1F reserved for SFU control)

enum class SfuCtrlType : uint8_t {
    kSubscribeReq    = 0x10,  // Client -> Server: Subscribe request
    kSubscribeResp   = 0x11,  // Server -> Client: Subscribe response (with 32bit ID)
    kUnsubscribe     = 0x12,  // Client -> Server: Unsubscribe
    kStreamData      = 0x13,  // Client -> Server: Stream data (with session ID prefix)
    kForwardData     = 0x14,  // Server -> Client: Forwarded stream data (with source session ID)
    kRoomInfo        = 0x15,  // Server -> Client: Stream info (query result)
    kKick            = 0x16,  // Server -> Client: Kick (stream conflict / publisher offline / server admin)
    // P2P signaling (0x17-0x1C)
    kP2PRequest      = 0x17,  // Client -> Server: Request P2P with peer
    kP2PResponse     = 0x18,  // Server -> Client: Peer info + reflexive address
    kP2PCandidate    = 0x19,  // Bidirectional via server: Candidate address exchange
    kP2PConnectCheck = 0x1A,  // Bidirectional: Connectivity check result
    kP2PFallback     = 0x1B,  // Server -> Client: Fall back to server relay
    kP2PStatus       = 0x1C,  // Client -> Server: P2P link quality report

    // 遥控信令通道
    kCtrlReliable    = 0x20,  // 可靠信令（TCP 传输）
    kCtrlUnreliable  = 0x21,  // 不可靠信令（UDP 传输）
};

// 订阅角色：推流（注册流）与拉流（订阅流）统一走 kSubscribeReq，用 role 区分
enum class SubscribeRole : uint8_t {
    kSubscriber = 0,  // 拉流：订阅一条已存在的流（streamId = 目标流名）
    kPublisher  = 1,  // 推流：注册并发布一条新流（streamId = 本端流名，全局唯一）
};

// 订阅响应 reason 常用值（服务器 → 客户端，客户端据此区分失败原因）
inline constexpr const char* kReasonOk             = "OK";
inline constexpr const char* kReasonStreamExists   = "STREAM_EXISTS";    // 推流：流名已被占用
inline constexpr const char* kReasonStreamNotFound = "STREAM_NOT_FOUND"; // 拉流：目标流不存在
inline constexpr const char* kReasonInvalidFormat  = "INVALID_FORMAT";
inline constexpr const char* kReasonStreamEnd      = "STREAM_END";       // 推流端下线/流被注销

// ===== Subscribe Request Format =====
// 推拉统一使用 kSubscribeReq，role 决定语义：
//   role=kPublisher  ：注册并发布一条流（streamId 即流名，全服务唯一）
//   role=kSubscriber ：订阅一条已存在的流（streamId 为目标流名）
// [1 byte: version|msgType=kCmd] [1 byte: SfuCtrlType=kSubscribeReq]
// [1 byte: role (0=subscriber, 1=publisher)]
// [4 bytes: streamId length] [streamId bytes]
// [4 bytes: userId length] [userId bytes]
// [1 byte: flags] (audio|video|data)

struct SubscribeFlags {
    bool audio : 1;       // 订阅/发布音频通道
    bool video : 1;       // 订阅/发布视频通道
    bool data : 1;        // 订阅/发布消息通道（信令通道承载）
    uint8_t reserved : 5;

    uint8_t Encode() const {
        return (audio ? 1 : 0) |
               (video ? 2 : 0) |
               (data ? 4 : 0) |
               (reserved << 3);
    }

    static SubscribeFlags Decode(uint8_t flags) {
        SubscribeFlags f;
        f.audio = flags & 1;
        f.video = flags & 2;
        f.data = flags & 4;
        f.reserved = flags >> 3;
        return f;
    }
};

// Serialize subscribe request（推流/拉流统一入口）
inline std::vector<uint8_t> SerializeSubscribeReq(
    SubscribeRole role,
    const std::string& streamId,
    const std::string& userId,
    const SubscribeFlags& flags)
{
    std::vector<uint8_t> buf;

    // Header: version + msgType=kCmd
    buf.push_back(MakeHeader(PROTOCOL_VERSION, static_cast<uint8_t>(MsgType::kCmd)));

    // SfuCtrlType
    buf.push_back(static_cast<uint8_t>(SfuCtrlType::kSubscribeReq));

    // Role（1 byte：0=subscriber, 1=publisher）
    buf.push_back(static_cast<uint8_t>(role));

    // streamId length + data
    uint32_t streamIdLen = static_cast<uint32_t>(streamId.size());
    buf.push_back((streamIdLen >> 24) & 0xFF);
    buf.push_back((streamIdLen >> 16) & 0xFF);
    buf.push_back((streamIdLen >> 8) & 0xFF);
    buf.push_back(streamIdLen & 0xFF);
    buf.insert(buf.end(), streamId.begin(), streamId.end());

    // userId length + data
    uint32_t userIdLen = static_cast<uint32_t>(userId.size());
    buf.push_back((userIdLen >> 24) & 0xFF);
    buf.push_back((userIdLen >> 16) & 0xFF);
    buf.push_back((userIdLen >> 8) & 0xFF);
    buf.push_back(userIdLen & 0xFF);
    buf.insert(buf.end(), userId.begin(), userId.end());

    // Flags
    buf.push_back(flags.Encode());

    return buf;
}

// Parse subscribe request
struct SubscribeReq {
    SubscribeRole role = SubscribeRole::kSubscriber;
    std::string streamId;   // 推流：本端流名；拉流：目标流名
    std::string userId;
    SubscribeFlags flags;
    bool valid = false;
};

inline SubscribeReq ParseSubscribeReq(const uint8_t* data, size_t len) {
    SubscribeReq req;
    size_t offset = 0;

    if (len < 1 + 1 + 1 + 4) return req;  // min: ctrlType + role + streamIdLen

    uint8_t ctrlType = data[offset++];
    if (ctrlType != static_cast<uint8_t>(SfuCtrlType::kSubscribeReq)) return req;

    // Read role（0=subscriber, 1=publisher）
    req.role = static_cast<SubscribeRole>(data[offset++]);

    // Read streamId
    uint32_t streamIdLen = (data[offset] << 24) | (data[offset+1] << 16) |
                           (data[offset+2] << 8) | data[offset+3];
    offset += 4;
    if (offset + streamIdLen > len) return req;
    req.streamId.assign(reinterpret_cast<const char*>(data + offset), streamIdLen);
    offset += streamIdLen;

    // Read userId
    if (offset + 4 > len) return req;
    uint32_t userIdLen = (data[offset] << 24) | (data[offset+1] << 16) |
                         (data[offset+2] << 8) | data[offset+3];
    offset += 4;
    if (offset + userIdLen > len) return req;
    req.userId.assign(reinterpret_cast<const char*>(data + offset), userIdLen);
    offset += userIdLen;

    // Read flags
    if (offset >= len) return req;
    req.flags = SubscribeFlags::Decode(data[offset++]);

    req.valid = true;
    return req;
}

// ===== Subscribe Response Format =====
// [1 byte: version|msgType=kCmd] [1 byte: SfuCtrlType=kSubscribeResp]
// [4 bytes: sessionId (32bit)]
// [1 byte: status (0=success, 1=fail)]
// [4 bytes: reason length] [reason bytes]

inline std::vector<uint8_t> SerializeSubscribeResp(uint32_t sessionId, uint8_t status, const std::string& reason) {
    std::vector<uint8_t> buf;

    buf.push_back(MakeHeader(PROTOCOL_VERSION, static_cast<uint8_t>(MsgType::kCmd)));
    buf.push_back(static_cast<uint8_t>(SfuCtrlType::kSubscribeResp));

    // sessionId
    buf.push_back((sessionId >> 24) & 0xFF);
    buf.push_back((sessionId >> 16) & 0xFF);
    buf.push_back((sessionId >> 8) & 0xFF);
    buf.push_back(sessionId & 0xFF);

    // status
    buf.push_back(status);

    // reason
    uint32_t reasonLen = static_cast<uint32_t>(reason.size());
    buf.push_back((reasonLen >> 24) & 0xFF);
    buf.push_back((reasonLen >> 16) & 0xFF);
    buf.push_back((reasonLen >> 8) & 0xFF);
    buf.push_back(reasonLen & 0xFF);
    buf.insert(buf.end(), reason.begin(), reason.end());

    return buf;
}

struct SubscribeResp {
    uint32_t sessionId = 0;
    uint8_t status = 0;
    std::string reason;
    bool valid = false;
};

inline SubscribeResp ParseSubscribeResp(const uint8_t* data, size_t len) {
    SubscribeResp resp;
    size_t offset = 0;

    // Skip protocol header byte (version|msgType=kCmd)
    if (len < 1) return resp;
    offset++; // skip header

    if (len < 1 + 1 + 4 + 1) return resp;

    uint8_t ctrlType = data[offset++];
    if (ctrlType != static_cast<uint8_t>(SfuCtrlType::kSubscribeResp)) return resp;

    // sessionId
    resp.sessionId = (data[offset] << 24) | (data[offset+1] << 16) |
                     (data[offset+2] << 8) | data[offset+3];
    offset += 4;

    // status
    resp.status = data[offset++];

    // reason
    if (offset + 4 > len) return resp;
    uint32_t reasonLen = (data[offset] << 24) | (data[offset+1] << 16) |
                         (data[offset+2] << 8) | data[offset+3];
    offset += 4;
    if (offset + reasonLen > len) return resp;
    resp.reason.assign(reinterpret_cast<const char*>(data + offset), reasonLen);

    resp.valid = true;
    return resp;
}

// ===== Stream Data Format (Client -> Server) =====
// [1 byte: version|msgType=kCmd] [1 byte: SfuCtrlType=kStreamData]
// [4 bytes: sessionId]
// [N bytes: payload (frame_protoc encoded data)]

inline std::vector<uint8_t> SerializeStreamData(uint32_t sessionId, const uint8_t* payload, size_t payloadLen) {
    std::vector<uint8_t> buf;

    buf.push_back(MakeHeader(PROTOCOL_VERSION, static_cast<uint8_t>(MsgType::kCmd)));
    buf.push_back(static_cast<uint8_t>(SfuCtrlType::kStreamData));

    // sessionId
    buf.push_back((sessionId >> 24) & 0xFF);
    buf.push_back((sessionId >> 16) & 0xFF);
    buf.push_back((sessionId >> 8) & 0xFF);
    buf.push_back(sessionId & 0xFF);

    // payload
    buf.insert(buf.end(), payload, payload + payloadLen);

    return buf;
}

// 零拷贝版本：写入调用方提供的 buffer（适用于内存池）
// 返回实际写入字节数，0 表示 bufSize 不足
inline size_t SerializeStreamDataTo(uint8_t* buf, size_t bufSize, uint32_t sessionId, const uint8_t* payload, size_t payloadLen) {
    size_t needed = 6 + payloadLen; // 1 header + 1 ctrlType + 4 sessionId + payload
    if (bufSize < needed) return 0;

    buf[0] = MakeHeader(PROTOCOL_VERSION, static_cast<uint8_t>(MsgType::kCmd));
    buf[1] = static_cast<uint8_t>(SfuCtrlType::kStreamData);
    buf[2] = (sessionId >> 24) & 0xFF;
    buf[3] = (sessionId >> 16) & 0xFF;
    buf[4] = (sessionId >>  8) & 0xFF;
    buf[5] = sessionId & 0xFF;
    memcpy(buf + 6, payload, payloadLen);

    return needed;
}

struct StreamData {
    uint32_t sessionId = 0;
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;
    bool valid = false;
};

inline StreamData ParseStreamData(const uint8_t* data, size_t len) {
    StreamData sd;
    size_t offset = 0;

    if (len < 1 + 1 + 4) return sd;

    uint8_t ctrlType = data[offset++];
    if (ctrlType != static_cast<uint8_t>(SfuCtrlType::kStreamData)) return sd;

    // sessionId
    sd.sessionId = (data[offset] << 24) | (data[offset+1] << 16) |
                   (data[offset+2] << 8) | data[offset+3];
    offset += 4;

    // payload
    sd.payload = data + offset;
    sd.payloadLen = len - offset;
    sd.valid = true;

    return sd;
}

// ===== Forward Data Format (Server -> Client) =====
// [1 byte: version|msgType=kCmd] [1 byte: SfuCtrlType=kForwardData]
// [4 bytes: sourceSessionId]
// [N bytes: payload (frame_protoc encoded data)]

inline std::vector<uint8_t> SerializeForwardData(uint32_t sourceSessionId, const uint8_t* payload, size_t payloadLen) {
    std::vector<uint8_t> buf;

    buf.push_back(MakeHeader(PROTOCOL_VERSION, static_cast<uint8_t>(MsgType::kCmd)));
    buf.push_back(static_cast<uint8_t>(SfuCtrlType::kForwardData));

    // sourceSessionId
    buf.push_back((sourceSessionId >> 24) & 0xFF);
    buf.push_back((sourceSessionId >> 16) & 0xFF);
    buf.push_back((sourceSessionId >> 8) & 0xFF);
    buf.push_back(sourceSessionId & 0xFF);

    // payload
    buf.insert(buf.end(), payload, payload + payloadLen);

    return buf;
}

// 零拷贝版本：写入调用方提供的 buffer（适用于内存池）
inline size_t SerializeForwardDataTo(uint8_t* buf, size_t bufSize, uint32_t sourceSessionId, const uint8_t* payload, size_t payloadLen) {
    size_t needed = 6 + payloadLen; // 1 header + 1 ctrlType + 4 sessionId + payload
    if (bufSize < needed) return 0;

    buf[0] = MakeHeader(PROTOCOL_VERSION, static_cast<uint8_t>(MsgType::kCmd));
    buf[1] = static_cast<uint8_t>(SfuCtrlType::kForwardData);
    buf[2] = (sourceSessionId >> 24) & 0xFF;
    buf[3] = (sourceSessionId >> 16) & 0xFF;
    buf[4] = (sourceSessionId >>  8) & 0xFF;
    buf[5] = sourceSessionId & 0xFF;
    memcpy(buf + 6, payload, payloadLen);

    return needed;
}

struct ForwardData {
    uint32_t sourceSessionId = 0;
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;
    bool valid = false;
};

inline ForwardData ParseForwardData(const uint8_t* data, size_t len) {
    ForwardData fd;
    size_t offset = 0;

    if (len < 1 + 1 + 4) return fd;

    uint8_t ctrlType = data[offset++];
    if (ctrlType != static_cast<uint8_t>(SfuCtrlType::kForwardData)) return fd;

    // sourceSessionId
    fd.sourceSessionId = (data[offset] << 24) | (data[offset+1] << 16) |
                         (data[offset+2] << 8) | data[offset+3];
    offset += 4;

    // payload
    fd.payload = data + offset;
    fd.payloadLen = len - offset;
    fd.valid = true;

    return fd;
}

} // namespace sfu

#endif // SFU_SUBSCRIBE_PROTOCOL_H
