#ifndef PACER_H
#define PACER_H

#include <chrono>
#include <cstdint>
#include <mutex>

/**
 * @brief Token Bucket Pacer - 用于控制发送速率
 * 
 * 使用 Token Bucket 算法实现精确的带宽控制：
 * - 每个时钟周期向桶中添加固定数量的 token
 * - 发送数据时消耗 token（1 byte = 1 token）
 * - 如果桶中 token 不足，计算需要等待的时间
 * 
 * 特性：
 * - 支持动态调整带宽
 * - 支持突发传输（burst）
 * - 线程安全
 */
class Pacer {
public:
    /**
     * @brief 构造函数
     * @param bandwidth_bps 带宽（bits per second），默认 50 Mbps
     * @param bucket_size_sec 桶大小（秒数），默认 0.2 秒（允许短时间突发）
     */
    explicit Pacer(uint64_t bandwidth_bps = 50'000'000, double bucket_size_sec = 0.2);
    
    /**
     * @brief 发送前调用，计算需要等待的时间
     * @param bytes 要发送的字节数
     * @return 需要等待的微秒数（0 表示可以立即发送）
     *
     * 注意：本方法会预扣 token（余额可为负）。若只需要查询而不消费，
     * 请使用 timeUntilAvailable()。
     */
    int64_t beforeSend(uint64_t bytes);

    /**
     * @brief 查询发送 bytes 字节还需等待多久（不消费 token）
     * @return 需要等待的微秒数（0 表示当前即可发送）
     */
    int64_t timeUntilAvailable(uint64_t bytes);

    /**
     * @brief 尝试消费 token（不预扣）
     * @return true 余额足够并已扣除；false 余额不足，状态不变
     */
    bool tryConsume(uint64_t bytes);
    
    /**
     * @brief 发送后调用，记录已发送的字节数
     * @param bytes 实际发送的字节数
     */
    void afterSend(uint64_t bytes);
    
    /**
     * @brief 设置带宽
     * @param bandwidth_bps 新的带宽（bits per second）
     */
    void setBandwidth(uint64_t bandwidth_bps);
    
    /**
     * @brief 获取当前带宽
     * @return 当前带宽（bits per second）
     */
    uint64_t getBandwidth() const;
    
    /**
     * @brief 获取桶中当前可用 token 数（字节）
     * @return 可用字节数
     */
    uint64_t getAvailableTokens() const;
    
    /**
     * @brief 重置 Pacer 状态
     */
    void reset();

private:
    /**
     * @brief 补充 token 到桶中（mutable 状态，可在 const 上下文调用）
     */
    void refillTokens() const;
    
    /**
     * @brief 消费 token
     * @return 是否需要等待
     */
    bool consumeTokens(uint64_t bytes, int64_t& wait_us);

    // 配置参数
    uint64_t bandwidth_bps_;          // 带宽（bits per second）
    uint64_t bandwidth_bytes_per_sec_; // 带宽（bytes per second）
    uint64_t bucket_size_bytes_;       // 桶大小（字节）
    
    // 运行状态
    mutable std::mutex mutex_;
    // 允许为负：beforeSend 预扣 token，负余额表示已透支、后续请求需排队
    mutable int64_t current_tokens_;
    mutable std::chrono::steady_clock::time_point last_refill_time_;
    
    // 统计信息
    uint64_t total_bytes_sent_;
    uint64_t total_wait_count_;
    int64_t total_wait_us_;
};

#endif // PACER_H
