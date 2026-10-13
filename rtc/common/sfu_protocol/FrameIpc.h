#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// tj_rtc Frame IPC 协议 v1：SFU ↔ MoQ 网关（Unix domain socket，SOCK_STREAM）
//
// 拓扑：SFU 为服务端（FrameDispatcher），MoQ 网关为客户端。
// 本文件必须保持零依赖（仅 <cstdint> <cstddef> <cstring>）：
// 网关侧（rtc/libs/xquic 内 moq_gateway）持有本文件的一份独立副本，
// 序列化行为由契约测试锁定（rtc/server/sfu_test/test_frame_ipc.cpp：
// 静态断言 frame_type 数值与 frame_protoc::frame_type 一致）。
//
// 线格式（全部整数大端）：
//   传输分帧 = [u32 msg_len][msg]，msg_len 为 msg 的字节数（不含自身 4 字节）
//   msg      = [u8 version][u8 kind][body]
//
//   kind=kData   body = [u32 sid][u8 frame_type][u8 flags][u64 pts_us][u64 seq][payload]
//   kind=kSub    body = [u16 name_len][name]                      (网关 → SFU)
//   kind=kUnsub  body = [u32 sid]                                 (网关 → SFU)
//   kind=kPli    body = [u32 sid]                                 (网关 → SFU)
//   kind=kSubOk  body = [u32 sid][u16 name_len][name]             (SFU → 网关)
//   kind=kSubErr body = [u32 sid=0][u16 name_len][name][u8 reason](SFU → 网关)
//
//   sid       ：SFU 收到 SUB 后为该流分配的索引（u32，非 0，按名字稳定）；
//               之后 DATA/UNSUB/PLI 一律使用 sid，不再携带流名。
//   frame_type：与 frame_protoc::frame_type 数值一致；
//               视频 IDR / 参数帧应视为独立可解码起点（与 flags.kFlagKey 同义）。
//   flags     ：bit0 = kFlagKey（关键帧/参数帧），其余保留为 0。
//   pts_us    ：SFU 组帧完成时刻（单调时钟，µs），仅用于网关侧排序/时间戳。
//   seq       ：该流内的帧序号，从 0 起严格递增（丢弃不回退）。
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace frame_ipc {

constexpr uint8_t  kVersion        = 1;
constexpr uint32_t kMaxMessageSize = 2 * 1024 * 1024;  // 单条消息上限（含一帧视频负载）
constexpr uint16_t kMaxStreamName  = 255;              // 流名字节数上限

// 消息类型
enum Kind : uint8_t {
    kData   = 0x01,
    kSub    = 0x02,
    kUnsub  = 0x03,
    kPli    = 0x04,
    kSubOk  = 0x05,
    kSubErr = 0x06,
};

// 帧类型（数值与 frame_protoc::frame_type 对齐，契约测试静态断言）
enum FrameType : uint8_t {
    kAudio       = 0x01,
    kVideo       = 0x02,
    kVideoIdr    = 0x03,
    kVideoParams = 0x04,
    kFrameData   = 0x05,
};

// DATA.flags 位定义
constexpr uint8_t kFlagKey = 0x01;

// kSubErr reason
enum SubErrReason : uint8_t {
    kErrTooManyStreams = 1,
    kErrBadRequest     = 2,
};

// DATA 定长部分：sid(4) + frame_type(1) + flags(1) + pts_us(8) + seq(8)
constexpr uint32_t kDataFixedSize = 22;
constexpr uint32_t kDataMsgSize   = 2 + kDataFixedSize;  // 无负载时的 msg 长度

// ── 大端读写 ─────────────────────────────────────────────────────────────────

inline void putU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}
inline void putU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}
inline void putU64(uint8_t* p, uint64_t v) {
    putU32(p, static_cast<uint32_t>(v >> 32));
    putU32(p + 4, static_cast<uint32_t>(v));
}
inline uint16_t getU16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
inline uint32_t getU32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
inline uint64_t getU64(const uint8_t* p) {
    return (static_cast<uint64_t>(getU32(p)) << 32) | getU32(p + 4);
}

// ── 序列化（含外层 4 字节长度前缀）───────────────────────────────────────────
// 缓冲容量不足（含超过 kMaxMessageSize）时返回 0，不写入。

inline size_t serializeData(uint8_t* out, size_t cap, uint32_t sid, uint8_t frame_type,
                            uint8_t flags, uint64_t pts_us, uint64_t seq,
                            const uint8_t* payload, uint32_t payload_len) {
    uint64_t msg_len = static_cast<uint64_t>(kDataMsgSize) + payload_len;
    if (msg_len > kMaxMessageSize || cap < 4 + msg_len) return 0;
    putU32(out, static_cast<uint32_t>(msg_len));
    out[4] = kVersion;
    out[5] = kData;
    putU32(out + 6, sid);
    out[10] = frame_type;
    out[11] = flags;
    putU64(out + 12, pts_us);
    putU64(out + 20, seq);
    if (payload_len > 0 && payload != nullptr) {
        std::memcpy(out + 28, payload, payload_len);
    }
    return static_cast<size_t>(4 + msg_len);
}

inline size_t serializeSub(uint8_t* out, size_t cap, const char* name, uint16_t name_len) {
    if (name == nullptr || name_len == 0 || name_len > kMaxStreamName) return 0;
    size_t msg_len = 4 + name_len;
    if (cap < 4 + msg_len) return 0;
    putU32(out, static_cast<uint32_t>(msg_len));
    out[4] = kVersion;
    out[5] = kSub;
    putU16(out + 6, name_len);
    std::memcpy(out + 8, name, name_len);
    return 4 + msg_len;
}

inline size_t serializeUnsub(uint8_t* out, size_t cap, uint32_t sid) {
    if (cap < 10) return 0;
    putU32(out, 6);
    out[4] = kVersion;
    out[5] = kUnsub;
    putU32(out + 6, sid);
    return 10;
}

inline size_t serializePli(uint8_t* out, size_t cap, uint32_t sid) {
    if (cap < 10) return 0;
    putU32(out, 6);
    out[4] = kVersion;
    out[5] = kPli;
    putU32(out + 6, sid);
    return 10;
}

inline size_t serializeSubOk(uint8_t* out, size_t cap, uint32_t sid,
                             const char* name, uint16_t name_len) {
    if (name == nullptr || name_len == 0 || name_len > kMaxStreamName) return 0;
    size_t msg_len = 8 + name_len;  // ver+kind(2) + sid(4) + name_len(2) + name
    if (cap < 4 + msg_len) return 0;
    putU32(out, static_cast<uint32_t>(msg_len));
    out[4] = kVersion;
    out[5] = kSubOk;
    putU32(out + 6, sid);
    putU16(out + 10, name_len);
    std::memcpy(out + 12, name, name_len);
    return 4 + msg_len;
}

inline size_t serializeSubErr(uint8_t* out, size_t cap, const char* name,
                              uint16_t name_len, uint8_t reason) {
    if (name == nullptr || name_len == 0 || name_len > kMaxStreamName) return 0;
    size_t msg_len = 9 + name_len;  // ver+kind(2) + sid(4) + name_len(2) + name + reason
    if (cap < 4 + msg_len) return 0;
    putU32(out, static_cast<uint32_t>(msg_len));
    out[4] = kVersion;
    out[5] = kSubErr;
    putU32(out + 6, 0);
    putU16(out + 10, name_len);
    std::memcpy(out + 12, name, name_len);
    out[12 + name_len] = reason;
    return 4 + msg_len;
}

// ── 解析 ─────────────────────────────────────────────────────────────────────

// 解析后的 msg（body 指向调用方缓冲，不持有数据）
struct Message {
    uint8_t version = 0;
    uint8_t kind = 0;
    const uint8_t* body = nullptr;
    uint32_t body_len = 0;
};

inline bool parseMessage(const uint8_t* msg, uint32_t len, Message& out) {
    if (msg == nullptr || len < 2) return false;
    out.version = msg[0];
    out.kind = msg[1];
    out.body = msg + 2;
    out.body_len = len - 2;
    return true;
}

struct DataView {
    uint32_t sid = 0;
    uint8_t frame_type = 0;
    uint8_t flags = 0;
    uint64_t pts_us = 0;
    uint64_t seq = 0;
    const uint8_t* payload = nullptr;
    uint32_t payload_len = 0;
};

inline bool parseData(const Message& m, DataView& out) {
    if (m.kind != kData || m.body_len < kDataFixedSize) return false;
    out.sid = getU32(m.body);
    out.frame_type = m.body[4];
    out.flags = m.body[5];
    out.pts_us = getU64(m.body + 6);
    out.seq = getU64(m.body + 14);
    out.payload = m.body + kDataFixedSize;
    out.payload_len = m.body_len - kDataFixedSize;
    return true;
}

struct NameView {
    const char* name = nullptr;
    uint16_t name_len = 0;
};

inline bool parseSub(const Message& m, NameView& out) {
    if (m.kind != kSub || m.body_len < 2) return false;
    uint16_t n = getU16(m.body);
    if (n == 0 || n > kMaxStreamName || m.body_len < 2u + n) return false;
    out.name = reinterpret_cast<const char*>(m.body + 2);
    out.name_len = n;
    return true;
}

inline bool parseSid(const Message& m, uint32_t& sid) {
    if ((m.kind != kUnsub && m.kind != kPli) || m.body_len < 4) return false;
    sid = getU32(m.body);
    return sid != 0;
}

struct SubOkView {
    uint32_t sid = 0;
    const char* name = nullptr;
    uint16_t name_len = 0;
};

inline bool parseSubOk(const Message& m, SubOkView& out) {
    if (m.kind != kSubOk || m.body_len < 6) return false;
    out.sid = getU32(m.body);
    uint16_t n = getU16(m.body + 4);
    if (out.sid == 0 || n == 0 || n > kMaxStreamName || m.body_len < 6u + n) return false;
    out.name = reinterpret_cast<const char*>(m.body + 6);
    out.name_len = n;
    return true;
}

struct SubErrView {
    const char* name = nullptr;
    uint16_t name_len = 0;
    uint8_t reason = 0;
};

inline bool parseSubErr(const Message& m, SubErrView& out) {
    if (m.kind != kSubErr || m.body_len < 7) return false;
    uint16_t n = getU16(m.body + 4);
    if (n == 0 || n > kMaxStreamName || m.body_len < 7u + n) return false;
    out.name = reinterpret_cast<const char*>(m.body + 6);
    out.name_len = n;
    out.reason = m.body[6 + n];
    return true;
}

} // namespace frame_ipc