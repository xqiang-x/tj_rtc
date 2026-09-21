/**
 * Pacer 集成示例 - SFU 服务端
 * 
 * 展示如何在 SFU 服务端集成 Pacer 进行带宽控制
 */

#include "Pacer.h"
#include <iostream>
#include <thread>
#include <chrono>

// 模拟 SFU 发送场景
class SfuSender {
public:
    SfuSender() : pacer_(50'000'000) {  // 默认 50 Mbps
        std::cout << "[SFU] Pacer initialized with 50 Mbps" << std::endl;
    }
    
    // 发送数据（带限速）
    void sendData(const uint8_t* data, size_t len) {
        // 1. 检查是否需要等待
        int64_t wait_us = pacer_.beforeSend(len);
        
        if (wait_us > 0) {
            // 需要限速：等待指定时间
            std::this_thread::sleep_for(std::chrono::microseconds(wait_us));
            
            static int log_count = 0;
            if (++log_count % 50 == 0) {
                std::cout << "[SFU-PACER] Rate limit: wait " << wait_us << " us for " << len << " bytes" << std::endl;
            }
        }
        
        // 2. 实际发送数据（这里只是模拟）
        // sendto(fd, data, len, ...);
        simulatedSend(data, len);
        
        // 3. 记录发送完成
        pacer_.afterSend(len);
        
        total_bytes_sent_ += len;
        total_packets_sent_++;
    }
    
    // 动态调整带宽
    void adjustBandwidth(uint64_t new_bps) {
        pacer_.setBandwidth(new_bps);
        std::cout << "[SFU] Bandwidth adjusted to " << new_bps / 1'000'000 << " Mbps" << std::endl;
    }
    
    // 打印统计信息
    void printStats() const {
        std::cout << "[SFU Stats] Sent: " << total_packets_sent_ << " packets, "
                  << total_bytes_sent_ << " bytes ("
                  << (total_bytes_sent_ * 8.0 / 1'000'000) << " Mbits)" << std::endl;
        std::cout << "[SFU Stats] Current bandwidth limit: " 
                  << pacer_.getBandwidth() / 1'000'000 << " Mbps" << std::endl;
        std::cout << "[SFU Stats] Available tokens: " 
                  << pacer_.getAvailableTokens() << " bytes" << std::endl;
    }
    
private:
    void simulatedSend(const uint8_t* /*data*/, size_t len) {
        // 模拟网络发送延迟
        // 实际代码中这里调用 sendto() 或 send()
        (void)len;
    }
    
    Pacer pacer_;
    uint64_t total_bytes_sent_ = 0;
    uint64_t total_packets_sent_ = 0;
};

int main() {
    SfuSender sender;
    
    // 模拟发送视频流
    std::cout << "\n=== Simulating Video Stream ===" << std::endl;
    
    // 假设视频帧大小：1080p @ 30fps ≈ 2 Mbps
    size_t frame_size = 250'000;  // 250 KB per frame (压缩后的 H.264)
    int fps = 30;
    double frame_interval_ms = 1000.0 / fps;
    
    auto start = std::chrono::steady_clock::now();
    
    // 发送 100 帧（约 3.3 秒）
    for (int i = 0; i < 100; ++i) {
        // 模拟 FEC 分片：每帧分成多个小包
        int packets_per_frame = frame_size / 1400;  // 每个 FEC 包最大 1400 字节
        
        for (int p = 0; p < packets_per_frame; ++p) {
            size_t packet_size = std::min(size_t(1400), frame_size - p * 1400);
            
            // 发送数据包（带 Pacer 限速）
            uint8_t dummy_data[1400];
            sender.sendData(dummy_data, packet_size);
        }
        
        // 帧间等待
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        
        // 每 30 帧打印一次统计
        if ((i + 1) % 30 == 0) {
            std::cout << "\n--- Frame " << (i + 1) << " ---" << std::endl;
            sender.printStats();
            std::cout << std::endl;
        }
    }
    
    auto end = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    std::cout << "\n=== Final Statistics ===" << std::endl;
    std::cout << "Duration: " << elapsed_ms << " ms" << std::endl;
    sender.printStats();
    
    // 测试动态带宽调整
    std::cout << "\n=== Testing Dynamic Bandwidth Adjustment ===" << std::endl;
    
    std::cout << "\nReducing bandwidth to 10 Mbps..." << std::endl;
    sender.adjustBandwidth(10'000'000);
    
    // 发送一些数据观察限速效果
    for (int i = 0; i < 10; ++i) {
        uint8_t dummy_data[5000];
        sender.sendData(dummy_data, 5000);
    }
    
    std::cout << "\nIncreasing bandwidth to 100 Mbps..." << std::endl;
    sender.adjustBandwidth(100'000'000);
    
    for (int i = 0; i < 10; ++i) {
        uint8_t dummy_data[5000];
        sender.sendData(dummy_data, 5000);
    }
    
    std::cout << "\n=== Test Completed ===" << std::endl;
    
    return 0;
}
