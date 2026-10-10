#include "bitrate_controller.h"
#include <algorithm>
#include <cmath>

namespace fec_protocol {

BitrateController::BitrateController(const BitrateControllerConfig& config)
    : cfg_(config),
      target_bps_(config.initial_bitrate_bps),
      fec_ratio_(clampFec(config.fec_initial)) {}

void BitrateController::reset() {
    state_ = State::NORMAL;
    target_bps_ = cfg_.initial_bitrate_bps;
    fec_ratio_ = clampFec(cfg_.fec_initial);
    ewma_goodput_bps_ = 0.0f;
    has_goodput_ = false;
    cooldown_remaining_ = 0;
    loss_rounds_ = 0;
    good_windows_ = 0;
    next_increase_us_ = 0;
    has_advice_ = false;
}

uint32_t BitrateController::clampBitrate(uint32_t bps) const {
    if (bps < cfg_.min_bitrate_bps) return cfg_.min_bitrate_bps;
    if (bps > cfg_.max_bitrate_bps) return cfg_.max_bitrate_bps;
    return bps;
}

float BitrateController::clampFec(float v) const {
    if (v < cfg_.fec_min) return cfg_.fec_min;
    if (v > cfg_.fec_max) return cfg_.fec_max;
    return v;
}

// 自适应冗余：fec = clamp(loss × fec_loss_factor, fec_min, fec_max)（默认 1.5 倍丢包率），
// 与当前值之差小于死区（防丢包率抖动引起频繁变更）时保持当前值
float BitrateController::applyAdaptiveFec(float loss_rate) {
    float target = clampFec(loss_rate * cfg_.fec_loss_factor);
    if (std::fabs(target - fec_ratio_) < cfg_.fec_dead_band) {
        return fec_ratio_;
    }
    return target;
}

// Build advice and decide whether to notify the caller.
bool BitrateController::emitAdvice(uint32_t smoothed_goodput, float loss_rate) {
    BitrateAdvice adv;
    adv.target_bitrate_bps = target_bps_;
    adv.fec_ratio = fec_ratio_;
    adv.measured_goodput_bps = smoothed_goodput;
    adv.loss_rate = loss_rate;
    adv.rate_decreased = has_advice_ && target_bps_ < last_advice_.target_bitrate_bps;

    bool notify = false;
    if (!has_advice_) {
        notify = true;                       // first advice always emitted
    } else if (adv.rate_decreased) {
        notify = true;                       // 降码率必须立即通知
    } else if (adv.fec_ratio != last_advice_.fec_ratio) {
        notify = true;                       // FEC 冗余变化
    } else {
        uint32_t prev = last_advice_.target_bitrate_bps;
        if (prev > 0) {
            float rel = static_cast<float>(target_bps_ > prev ? target_bps_ - prev
                                                              : prev - target_bps_) /
                        static_cast<float>(prev);
            if (rel >= cfg_.advice_change_min) notify = true;
        } else if (target_bps_ != prev) {
            notify = true;
        }
    }

    last_advice_ = adv;
    has_advice_ = true;
    return notify;
}

bool BitrateController::onFeedback(uint64_t now_us, uint32_t goodput_bps, float loss_rate) {
    // EWMA 平滑实测接收码率
    if (!has_goodput_) {
        ewma_goodput_bps_ = static_cast<float>(goodput_bps);
        has_goodput_ = true;
        // 建立升码率节奏基线：首个窗口后 hold_ms 才允许首次升档
        if (next_increase_us_ == 0) {
            next_increase_us_ = now_us + static_cast<uint64_t>(cfg_.hold_ms) * 1000ULL;
        }
    } else {
        ewma_goodput_bps_ = cfg_.ewma_alpha * static_cast<float>(goodput_bps) +
                            (1.0f - cfg_.ewma_alpha) * ewma_goodput_bps_;
    }
    uint32_t smoothed = static_cast<uint32_t>(ewma_goodput_bps_);

    if (cooldown_remaining_ > 0) cooldown_remaining_--;

    if (loss_rate > cfg_.loss_down_threshold) {
        good_windows_ = 0;
        loss_streak_++;
        loss_rounds_++;

        // 单窗口丢包尖峰（订阅切换等瞬时误报）不动作，连续超阈值才降级
        if (loss_streak_ < cfg_.loss_confirm_windows) {
            // 冗余是保护性措施：观测到丢包即跟随（即使尚未触发降码率）
            fec_ratio_ = applyAdaptiveFec(loss_rate);
            return emitAdvice(smoothed, loss_rate);
        }

        if (state_ == State::NORMAL) {
            // 确认丢包：降到实测接收码率，并增加冗余进入观察
            state_ = State::LOSS_OBSERVE;
            if (smoothed > 0 && smoothed < target_bps_) {
                target_bps_ = clampBitrate(smoothed);
            } else {
                target_bps_ = clampBitrate(
                    static_cast<uint32_t>(target_bps_ * cfg_.decrease_factor));
            }
            loss_rounds_ = 1;
            cooldown_remaining_ = cfg_.cooldown_windows;
        } else if (loss_rounds_ >= cfg_.max_loss_rounds) {
            // 观察期内丢包持续：再硬降一档
            target_bps_ = clampBitrate(
                static_cast<uint32_t>(target_bps_ * cfg_.decrease_factor));
            loss_rounds_ = 0;
            cooldown_remaining_ = cfg_.cooldown_windows;
        }

        // FEC 冗余跟随丢包率（1.5×丢包率，clamp 到 [min, max]，死区内不更新）
        fec_ratio_ = applyAdaptiveFec(loss_rate);
        return emitAdvice(smoothed, loss_rate);
    }

    // 丢包恢复到阈值以下
    loss_streak_ = 0;
    if (state_ == State::LOSS_OBSERVE) {
        state_ = State::NORMAL;
    }
    good_windows_++;

    // 丢包率已低于阈值：冗余跟随丢包率回落（1.5×丢包率，clamp+死区）
    fec_ratio_ = applyAdaptiveFec(loss_rate);

    // 升码率：冷却结束 + 到达升档节奏 + 低丢包
    if (cooldown_remaining_ == 0 && loss_rate <= cfg_.loss_up_threshold &&
        now_us >= next_increase_us_) {
        uint32_t next = clampBitrate(
            static_cast<uint32_t>(target_bps_ * cfg_.increase_factor));
        if (next > target_bps_) {
            target_bps_ = next;
            next_increase_us_ = now_us + static_cast<uint64_t>(cfg_.hold_ms) * 1000ULL;
        }
    }

    return emitAdvice(smoothed, loss_rate);
}

} // namespace fec_protocol
