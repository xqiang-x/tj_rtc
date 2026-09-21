#pragma once
#include "types.h"
#include <memory>

namespace fec_protocol {

struct ProxyNackConfig {
    uint32_t nack_delay_ms = 10;       // 缺块创建后到首次上游 NACK 的最小延迟（防乱序大量 NACK）
    uint32_t nack_interval_ms = 50;    // Interval between retries
    uint32_t max_nack_retries = 3;     // Max NACK attempts per missing seq
    uint32_t nack_lifetime_ms = 2000;  // Max lifetime of a missing-seq state（与下游 2s 帧超时对齐）
};

struct ProxyConfig {
    float    downstream_fec_ratio = 0.25f;  // FEC blocks to forward: ceil(dbc * ratio)
    uint32_t cache_timeout_ms = 5000;       // Cache eviction timeout (ms)
    uint32_t max_cache_entries = 10000;     // Hard cap on cache size
    uint32_t fec_group_timeout_ms = 3000;   // 转发决策 FEC 组状态保留时长（组块 2s 内未收全即放弃）
    uint32_t tick_interval_ms = 50;         // tick 最小间隔：NACK 生成 50ms 节流，避免每包全量扫描
    // 单次允许的最大序号间隙：超过则视为序号重置/异常包，直接重锚期望序号，
    // 不逐号创建 NACK 状态，防止单个坏包引发海量分配（曾导致 bad_alloc 崩溃）
    uint32_t max_gap_window = 1024;
    ProxyNackConfig nack;                   // Active NACK configuration
};

struct ProxyStats {
    uint64_t packets_received = 0;      // Total upstream packets received
    uint64_t packets_forwarded = 0;     // Total forwarded downstream
    uint64_t fec_packets_dropped = 0;   // FEC blocks filtered out
    uint64_t nack_requests_received = 0;// NACKs from downstream
    uint64_t nack_cache_hits = 0;       // Retransmissions served from cache
    uint64_t nack_cache_misses = 0;     // NACKs that couldn't be served
    uint64_t cache_size = 0;            // Current cache size
    uint64_t upstream_nacks_sent = 0;       // NACKs sent toward upstream
    uint64_t upstream_nack_suppressed = 0;  // NACKs suppressed (FEC recoverable)
    uint64_t upstream_nack_recovered = 0;   // Missing seqs recovered after NACK
    uint64_t stats_forwarded_upstream = 0;  // Downstream STATS relayed upstream
    uint64_t pli_forwarded_upstream = 0;    // Downstream PLI relayed upstream
    uint64_t pongs_sent = 0;                // PONG replies to upstream PING
};

class ProxyFrameReceiver {
public:
    explicit ProxyFrameReceiver(const ProxyConfig& config);
    ~ProxyFrameReceiver();

    ProxyFrameReceiver(const ProxyFrameReceiver&) = delete;
    ProxyFrameReceiver& operator=(const ProxyFrameReceiver&) = delete;

    // --- Callbacks ---

    // Set callback for forwarding packets to downstream client
    void setSendCallback(SendPacketCallback cb);

    // Set callback for sending NACKs toward upstream sender
    void setUpstreamSendCallback(UpstreamSendCallback cb);

    // Set callback for NACK entries not found in local cache
    void setNackMissCallback(NackMissCallback cb);

    // --- Core operations ---

    // Process a packet from upstream sender.
    // Parses header, applies FEC filtering, caches and forwards.
    // Returns true if packet was forwarded, false if dropped.
    bool onUpstreamPacket(const uint8_t* data, size_t len, uint64_t now_us);

    // Process a NACK from downstream client.
    // Responds from cache if available, invokes miss callback otherwise.
    void onDownstreamNack(const uint8_t* data, size_t len);

    // Process any command packet from downstream (NACK, STATS, etc.)
    void onDownstreamPacket(const uint8_t* data, size_t len);

    // Forward NACK entries toward the upstream sender (e.g. downstream cache misses)
    void requestUpstreamNack(const std::vector<NackEntry>& entries);

    // Send a PLI (key frame request) toward the upstream sender
    void requestKeyFrame();

    // Drive cache eviction and active NACK generation.
    void tick(uint64_t now_us);

    // --- Queries ---

    const ProxyStats& getStats() const;
    uint32_t maxCacheEntries() const;
    void setDownstreamFecRatio(float ratio);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fec_protocol

