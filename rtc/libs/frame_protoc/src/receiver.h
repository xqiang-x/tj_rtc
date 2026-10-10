#pragma once
#include "types.h"
#include "config.h"
#include "stats.h"
#include <memory>

namespace fec_protocol {

// Frame dropped notification callback
using FrameDroppedCallback = std::function<void(FrameType type, uint32_t frame_size)>;

class FrameReceiver {
public:
    explicit FrameReceiver(const ReceiverConfig& config);
    ~FrameReceiver();

    FrameReceiver(const FrameReceiver&) = delete;
    FrameReceiver& operator=(const FrameReceiver&) = delete;

    // --- Callbacks ---

    void setFrameReadyCallback(FrameReadyCallback cb);
    void setSendCallback(SendPacketCallback cb);
    void setFrameDroppedCallback(FrameDroppedCallback cb);

    // --- Core operations ---

    // Feed all incoming raw packets.
    // DATA → reassemble + FEC decode; PING → reply PONG.
    // now_us: caller's monotonic clock in microseconds (0 = use last tick time).
    void onPacketReceived(const uint8_t* data, size_t len, uint64_t now_us = 0);

    // Drive time-based operations: frame timeout, NACK generation, STATS report.
    // Caller should invoke periodically (e.g., every 1~10ms).
    void tick(uint64_t now_us);

    // Manually send a PLI (key-frame request), bypassing the client-side throttle.
    // Called automatically when a video frame is dropped; the server still applies
    // its own rate limit.
    void requestKeyFrame(uint64_t now_us = 0);

    // --- Queries ---

    const ProtocolStats& getStats() const;
    SeqNum completedSeqNum() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fec_protocol

