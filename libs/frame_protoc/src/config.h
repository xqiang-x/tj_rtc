#pragma once
#include <cstdint>

namespace fec_protocol {

struct SenderConfig {
    uint16_t mtu = 1400;                  // Maximum transmission unit (bytes)
    float    fec_ratio = 0.25f;           // FEC redundancy ratio (recovery/original)
    float    fec_ratio_min = 0.0f;        // 自适应冗余度下限（自动计算时 clamp 到这里）
    float    fec_ratio_max = 0.30f;       // 自适应冗余度上限
    float    fec_loss_factor = 1.5f;      // 自适应公式：冗余度 = 丢包率 × 因子
    uint32_t cache_timeout_ms = 5000;     // Sent block cache timeout (ms)
    uint8_t  max_retry_count = 3;         // Max retransmissions per block (-1 style: use int8_t below)
    uint32_t ping_interval_ms = 1000;     // PING interval (ms)
    float    nack_rtt_factor = 1.2f;      // NACK throttle: ignore if last retransmit < RTT * factor
    int32_t  max_nack_retransmit = -1;    // Max NACK retransmits per tick (-1 = unlimited)
    int32_t  max_block_retransmit = -1;   // Max retransmissions per single block (-1 = unlimited)
};

struct ReceiverConfig {
    uint32_t frame_timeout_ms = 2000;     // Frame assembly timeout (ms)；超时丢弃并请求关键帧
    uint32_t nack_delay_ms = 10;          // 缺块创建后到首次 NACK 的最小延迟（防乱序大量 NACK）
    uint32_t nack_interval_ms = 50;       // 无 RTT 时 NACK 重试间隔；有 RTT 时按 max(RTT, 本值)（1×RTT 下限节流）
    uint32_t max_nack_retries = 3;        // Max NACK requests per missing block
    uint32_t stats_interval_ms = 1000;    // STATS report interval (ms)
    uint16_t max_pending_frames = 64;     // Max concurrent incomplete frames
    uint32_t pli_min_interval_ms = 500;   // Min interval between automatic PLI sends
};

} // namespace fec_protocol

