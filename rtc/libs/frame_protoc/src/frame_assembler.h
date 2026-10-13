#pragma once
#include "types.h"
#include <memory>

namespace fec_protocol {

struct FrameAssemblerConfig {
    uint32_t frame_timeout_ms = 2000;     // 帧组包超时（与 ReceiverConfig 语义一致）
    uint16_t max_pending_frames = 64;     // 并发未收全帧上限（超出淘汰最早开始的帧）
    uint32_t tick_interval_ms = 50;       // 超时扫描节流：tick 被高频调用时内部限频
    uint32_t stats_interval_ms = 1000;    // 组装速率统计窗口
};

struct FrameAssemblerStats {
    uint64_t packets_received = 0;   // 投喂的 DATA 包总数（含陈旧包）
    uint64_t frames_assembled = 0;   // 成功组帧并交付
    uint64_t frames_timed_out = 0;   // 超时丢弃（含未收全与补齐后仍被丢弃）
    uint64_t frames_evicted = 0;     // pending 上限淘汰
    uint64_t cursor_gap_jumps = 0;   // 投喂缺口：游标帧从未出现时的跳过次数
    uint64_t bytes_assembled = 0;    // 累计交付字节
    uint64_t pending_frames = 0;     // 当前未完成帧数
    uint32_t assembled_kbps = 0;     // 最近统计窗口的组帧速率 (kbit/s)
};

// SFU 服务器侧只读组帧器：复用 receiver.cpp 的 FEC 解码与按序提交内核，
// 剥离 NACK/STATS/PONG/PLI 等一切回包副作用（代理层已负责这些信令）。
// 与 ProxyFrameReceiver 并行消费同一批 DATA 包，不改变任何转发行为。
// 仅供单线程（事件循环）使用。
class FrameAssembler {
public:
    explicit FrameAssembler(const FrameAssemblerConfig& config = {});
    ~FrameAssembler();

    FrameAssembler(const FrameAssembler&) = delete;
    FrameAssembler& operator=(const FrameAssembler&) = delete;

    // 完整组帧后回调（按帧序交付，参数为完成的帧）。
    void setFrameReadyCallback(FrameReadyCallback cb);

    // 投喂一个完整线上包（含 CommonHeader）；仅处理 MsgType::DATA，
    // 其余类型（PING/NACK/STATS/PLI/RECV_REPORT）直接忽略，不产生任何回包。
    // now_us: 调用方单调时钟（微秒）；0 = 沿用最近一次 tick 的时间。
    void onDataPacket(const uint8_t* data, size_t len, uint64_t now_us = 0);

    // 时间驱动：帧超时扫描（内部按 tick_interval_ms 节流）+ 速率统计窗口。
    // 高频调用开销恒定。
    void tick(uint64_t now_us);

    const FrameAssemblerStats& getStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fec_protocol