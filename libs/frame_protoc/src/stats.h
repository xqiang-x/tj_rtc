#pragma once
#include <cstdint>

namespace fec_protocol {

struct ProtocolStats {
    uint32_t rtt_ms = 0;
    float    loss_rate = 0.0f;            // 0.0 ~ 1.0
    float    fec_recovery_rate = 0.0f;    // 0.0 ~ 1.0
    float    nack_recovery_rate = 0.0f;   // 0.0 ~ 1.0
    uint32_t bandwidth_bps = 0;
    uint32_t goodput_bps = 0;             // 接收端实测有效吞吐（完成帧字节/窗口）
    uint32_t stats_window_packets = 0;    // 最近一个统计窗口内收到的数据包数

    uint64_t frames_sent = 0;
    uint64_t frames_completed = 0;
    uint64_t frames_dropped = 0;
    uint64_t blocks_sent = 0;
    uint64_t blocks_lost = 0;
    uint64_t blocks_recovered_fec = 0;
    uint64_t blocks_recovered_nack = 0;
};

} // namespace fec_protocol

