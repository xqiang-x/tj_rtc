#pragma once
#include "types.h"
#include "config.h"
#include "stats.h"
#include <memory>

namespace fec_protocol {

// Optional: dynamic FEC ratio adjustment callback
using FecRatioAdjustCallback = std::function<float(const ProtocolStats& stats)>;

// Invoked each time a STATS packet from the peer has been parsed
using StatsCallback = std::function<void(const ProtocolStats& stats)>;

// Invoked when a PLI (key-frame request) arrives from the peer
using KeyFrameRequestCallback = std::function<void()>;

class FrameSender {
public:
    explicit FrameSender(const SenderConfig& config);
    ~FrameSender();

    FrameSender(const FrameSender&) = delete;
    FrameSender& operator=(const FrameSender&) = delete;

    // --- Callbacks ---

    void setSendCallback(SendPacketCallback cb);
    void setFecRatioAdjustCallback(FecRatioAdjustCallback cb);
    void setStatsCallback(StatsCallback cb);
    void setKeyFrameRequestCallback(KeyFrameRequestCallback cb);

    // --- Core operations ---

    // Fragment a frame, FEC-encode, and send all blocks via callback.
    // now_us: caller's monotonic clock in microseconds (0 = use last tick time).
    // Returns false if frame is too large or parameters are invalid.
    bool sendFrame(const uint8_t* data, size_t len, FrameType type, uint64_t now_us = 0);

    // Feed received control packets (NACK/STATS/PONG/PLI) for processing.
    // NACK → auto retransmit; PONG → compute RTT; STATS → update peer stats;
    // PLI → invoke key-frame-request callback.
    // now_us: caller's monotonic clock in microseconds (0 = use last tick time).
    void onPacketReceived(const uint8_t* data, size_t len, uint64_t now_us = 0);

    // Drive time-based operations: cache cleanup, PING send, FEC adjustment.
    // Caller should invoke periodically (e.g., every 1~10ms).
    void tick(uint64_t now_us);

    // --- Queries ---

    const ProtocolStats& getStats() const;
    SeqNum currentDataSeqNum() const;
    void setFecRatio(float ratio);

    // 是否收到过至少一次 PONG（对端存活判定的前提）
    bool hasReceivedPong() const;
    // 最后一次收到 PONG 的单调时钟（微秒），未收到过返回 0
    uint64_t lastPongTimeUs() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fec_protocol

