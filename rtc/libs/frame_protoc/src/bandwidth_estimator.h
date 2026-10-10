#pragma once
#include "types.h"
#include <functional>
#include <vector>

namespace fec_protocol {

// 一个带宽估计样本：某包的序号、发送端向网络发包的时间、接收端收到该包的时间。
// send_time_us 与 recv_time_ms 分属两端各自的单调时钟，无共同基准，
// 估计时只能使用批内的差值（发送间隔 vs 接收间隔）
struct BwSample {
    SeqNum   seq = 0;
    uint64_t send_time_us = 0;   // 发送端单调时钟（微秒）；重传包为最近一次重传时间
    uint64_t recv_time_ms = 0;   // 接收端单调时钟（毫秒），来自 RECV_REPORT
    uint16_t packet_size = 0;    // 整包长度（字节，含协议头）
};

// 可注入的带宽估计函数类型（FrameSender::setBandwidthEstimator）
using BandwidthEstimator = std::function<uint32_t(const std::vector<BwSample>&)>;

// 带宽估计：输入一批包的样本（序号 + 发送时间 + 接收时间 + 包长），
// 返回估计带宽（bps）；无法估计（样本不足等）时返回 0。
// TODO: 算法待实现（如基于单向时延梯度 + 包到达速率的估计），当前恒返回 0
uint32_t estimateBandwidth(const std::vector<BwSample>& samples);

} // namespace fec_protocol
