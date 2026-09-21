#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <functional>

namespace fec_protocol {

// Protocol version
constexpr uint8_t kProtocolVersion = 0x01;
constexpr uint16_t kMaxPayloadLength = 1450;
constexpr uint8_t kMaxSeqLen = 8;
constexpr uint8_t kMaxRetryCount = 15;
constexpr uint16_t kMaxBlocksPerGroup = 256;
constexpr uint8_t kNackEntrySize = 12;

// Sequence number type (internally always 64-bit)
using SeqNum = uint64_t;
using Timestamp = uint64_t;
using FrameType = uint8_t;

// Message type
enum class MsgType : uint8_t {
    DATA  = 0x01,
    NACK  = 0x02,
    STATS = 0x03,
    PING  = 0x04,
    PONG  = 0x05,
    PLI   = 0x06,   // Picture Loss Indication: request a key frame
};

// TLV type identifiers
enum class TlvType : uint8_t {
    RTT                = 0x01,
    LOSS_RATE          = 0x02,
    FEC_RECOVERY_RATE  = 0x03,
    NACK_RECOVERY_RATE = 0x04,
    BANDWIDTH_ESTIMATE = 0x05,
    COMPLETED_SEQ      = 0x06,
    GOODPUT            = 0x12,   // 接收端实测有效吞吐（bps，4 字节 BE）
    TIMESTAMP          = 0x10,
    ECHO_TIMESTAMP     = 0x11,
};

// Predefined frame types
namespace frame_type {
    constexpr FrameType AUDIO        = 0x01;
    constexpr FrameType VIDEO        = 0x02;
    constexpr FrameType VIDEO_IDR    = 0x03;
    constexpr FrameType VIDEO_PARAMS = 0x04;
    constexpr FrameType DATA         = 0x05;
}

// Complete frame (receiver output)
struct Frame {
    FrameType type;
    uint32_t size;
    std::vector<uint8_t> data;
};

// NACK entry
struct NackEntry {
    SeqNum lost_start_seq;
    uint32_t bitmask;
};

// TLV item
struct TlvItem {
    TlvType type;
    std::vector<uint8_t> value;
};

// Callback types
using SendPacketCallback = std::function<void(const uint8_t* data, size_t len)>;
using FrameReadyCallback = std::function<void(Frame&& frame)>;
using UpstreamSendCallback = std::function<void(const uint8_t* data, size_t len)>;
using NackMissCallback = std::function<void(const std::vector<NackEntry>& entries)>;

} // namespace fec_protocol

