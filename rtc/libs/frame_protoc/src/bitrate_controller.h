#pragma once
#include <cstdint>

namespace fec_protocol {

struct BitrateControllerConfig {
    uint32_t initial_bitrate_bps = 3000000;   // 媒体目标码率初值
    uint32_t min_bitrate_bps     = 200000;
    uint32_t max_bitrate_bps     = 50000000;
    uint32_t window_ms           = 300;       // 反馈周期（与接收端 STATS 周期一致）

    float loss_down_threshold    = 0.02f;     // 丢包高于此值 → 降码率
    float loss_up_threshold      = 0.01f;     // 丢包低于此值才允许升码率
    float increase_factor        = 1.20f;     // 每阶段升幅
    float decrease_factor        = 0.80f;     // 持续丢包时的硬降幅

    float fec_initial            = 0.25f;
    float fec_min                = 0.05f;     // 自适应冗余度下限：fec = clamp(loss×factor, min, max)
    float fec_max                = 0.30f;
    float fec_loss_factor        = 1.5f;      // 冗余度 = 丢包率 × 因子（一般为 1.5 倍丢包率）
    float fec_dead_band          = 0.02f;     // 冗余度变化 < 2% 不更新（防丢包率抖动引起频繁变更）

    uint32_t cooldown_windows    = 3;         // 降级后禁止升码率的窗口数
    uint32_t max_loss_rounds     = 3;         // 观察期内丢包持续的轮数 → 硬降
    uint32_t hold_ms             = 1000;      // 两次升码率的最小间隔
    uint32_t fec_relax_windows   = 10;        // 连续好窗口数 → 逐步回收冗余

    float ewma_alpha             = 0.30f;     // goodput 平滑系数
    float advice_change_min      = 0.05f;     // 升码率建议变化 <5% 时不回调
    uint32_t loss_confirm_windows = 2;        // 连续 N 个窗口丢包超阈值才降码率（防订阅切换等瞬时误报）
};

struct BitrateAdvice {
    uint32_t target_bitrate_bps = 0;      // 媒体目标码率（不含 FEC）
    float fec_ratio = 0.25f;              // 建议 FEC 冗余比
    uint32_t measured_goodput_bps = 0;    // EWMA 平滑后的实测接收码率
    float loss_rate = 0.0f;
    bool rate_decreased = false;          // 本次建议是否为降码率
};

// 基于接收端反馈（goodput + 丢包率）的发送码率状态机：
//   INCREASE/（常态）→ 低丢包时按 hold_ms 节奏 ×increase_factor 升码率
//   LOSS_OBSERVE     → 丢包超阈值：降到实测接收码率并加 FEC，观察是否恶化
//   持续恶化 max_loss_rounds 轮 → 再 ×decrease_factor 硬降
class BitrateController {
public:
    explicit BitrateController(const BitrateControllerConfig& config = {});

    // 输入一个统计窗口的反馈，返回是否产生了新的 advice
    bool onFeedback(uint64_t now_us, uint32_t goodput_bps, float loss_rate);

    uint32_t targetBitrate() const { return target_bps_; }
    float fecRatio() const { return fec_ratio_; }
    const BitrateAdvice& lastAdvice() const { return last_advice_; }
    bool hasAdvice() const { return has_advice_; }

    void reset();

private:
    enum class State { NORMAL, LOSS_OBSERVE };

    uint32_t clampBitrate(uint32_t bps) const;
    float clampFec(float v) const;
    float applyAdaptiveFec(float loss_rate);
    bool emitAdvice(uint32_t smoothed_goodput, float loss_rate);

    BitrateControllerConfig cfg_;
    State state_ = State::NORMAL;
    uint32_t target_bps_;
    float fec_ratio_;

    float ewma_goodput_bps_ = 0.0f;
    bool has_goodput_ = false;

    uint32_t cooldown_remaining_ = 0;
    uint32_t loss_rounds_ = 0;
    uint32_t loss_streak_ = 0;
    uint32_t good_windows_ = 0;
    uint64_t next_increase_us_ = 0;

    BitrateAdvice last_advice_;
    bool has_advice_ = false;
};

} // namespace fec_protocol
