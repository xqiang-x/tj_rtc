#include "Pacer.h"
#include <algorithm>
#include <iostream>

Pacer::Pacer(uint64_t bandwidth_bps, double bucket_size_sec)
    : bandwidth_bps_(bandwidth_bps)
    , bandwidth_bytes_per_sec_(bandwidth_bps / 8)
    , bucket_size_bytes_(static_cast<uint64_t>(bandwidth_bytes_per_sec_ * bucket_size_sec))
    , current_tokens_(bucket_size_bytes_)  // 初始时桶是满的
    , last_refill_time_(std::chrono::steady_clock::now())
    , total_bytes_sent_(0)
    , total_wait_count_(0)
    , total_wait_us_(0)
{
}

int64_t Pacer::beforeSend(uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 先补充 token
    refillTokens();
    
    // 计算需要等待的时间
    int64_t wait_us = 0;
    bool need_wait = consumeTokens(bytes, wait_us);
    
    if (need_wait) {
        total_wait_count_++;
        total_wait_us_ += wait_us;
    }
    
    return wait_us;
}

void Pacer::afterSend(uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    total_bytes_sent_ += bytes;
}

int64_t Pacer::timeUntilAvailable(uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    refillTokens();

    int64_t deficit = static_cast<int64_t>(bytes) - current_tokens_;
    if (deficit <= 0) return 0;
    if (bandwidth_bytes_per_sec_ == 0) return 0;
    return (deficit * 1'000'000) / static_cast<int64_t>(bandwidth_bytes_per_sec_);
}

bool Pacer::tryConsume(uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    refillTokens();

    int64_t bytes_i = static_cast<int64_t>(bytes);
    if (current_tokens_ < bytes_i) return false;
    current_tokens_ -= bytes_i;
    return true;
}

void Pacer::setBandwidth(uint64_t bandwidth_bps) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    bandwidth_bps_ = bandwidth_bps;
    bandwidth_bytes_per_sec_ = bandwidth_bps / 8;
    
    // 重新计算桶大小，保持当前的 token 比例
    double bucket_size_sec = static_cast<double>(bucket_size_bytes_) / 
                             (bandwidth_bytes_per_sec_ > 0 ? bandwidth_bytes_per_sec_ : 1);
    bucket_size_bytes_ = static_cast<uint64_t>(bandwidth_bytes_per_sec_ * bucket_size_sec);
    
    // 确保当前 token 不超过新的桶大小
    current_tokens_ = std::min<int64_t>(current_tokens_,
                                        static_cast<int64_t>(bucket_size_bytes_));
}

uint64_t Pacer::getBandwidth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bandwidth_bps_;
}

uint64_t Pacer::getAvailableTokens() const {
    std::lock_guard<std::mutex> lock(mutex_);
    refillTokens();
    return current_tokens_ > 0 ? static_cast<uint64_t>(current_tokens_) : 0;
}

void Pacer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    current_tokens_ = static_cast<int64_t>(bucket_size_bytes_);
    last_refill_time_ = std::chrono::steady_clock::now();
    total_bytes_sent_ = 0;
    total_wait_count_ = 0;
    total_wait_us_ = 0;
}

void Pacer::refillTokens() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        now - last_refill_time_).count();
    
    if (elapsed_us > 0) {
        // 计算这段时间应该添加多少 token
        // 公式：(bytes_per_sec * elapsed_us) / 1,000,000
        int64_t new_tokens = static_cast<int64_t>(
            (bandwidth_bytes_per_sec_ * elapsed_us) / 1'000'000);
        
        if (new_tokens > 0) {
            current_tokens_ = std::min<int64_t>(
                current_tokens_ + new_tokens,
                static_cast<int64_t>(bucket_size_bytes_));
            last_refill_time_ = now;
        }
    }
}

bool Pacer::consumeTokens(uint64_t bytes, int64_t& wait_us) {
    int64_t bytes_i = static_cast<int64_t>(bytes);

    if (current_tokens_ >= bytes_i) {
        // 桶中有足够的 token，立即消费
        current_tokens_ -= bytes_i;
        wait_us = 0;
        return false;
    }
    
    // token 不足：按当前余额计算需要等待多久
    int64_t deficit = bytes_i - current_tokens_;
    if (bandwidth_bytes_per_sec_ > 0) {
        wait_us = (deficit * 1'000'000) / static_cast<int64_t>(bandwidth_bytes_per_sec_);
    } else {
        wait_us = 0;
    }
    
    // 预扣全部字节，余额可为负：并发/后续的 beforeSend 会看到
    // 更深的透支，从而得到更长的等待时间，保证总发送速率受限
    current_tokens_ -= bytes_i;
    
    return true;
}
