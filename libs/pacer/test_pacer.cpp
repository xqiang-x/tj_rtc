#include "Pacer.h"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    // 创建一个 1 Mbps 的 Pacer（方便测试）
    Pacer pacer(1'000'000);  // 1 Mbps = 125,000 bytes/sec
    
    std::cout << "Pacer Test - Bandwidth: " << pacer.getBandwidth() / 1'000'000 << " Mbps" << std::endl;
    std::cout << "Bucket size: " << pacer.getAvailableTokens() << " bytes" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // 测试 1：小包发送（应该不需要等待）
    std::cout << "\n[Test 1] Small packets (100 bytes each):" << std::endl;
    for (int i = 0; i < 5; ++i) {
        int64_t wait_us = pacer.beforeSend(100);
        if (wait_us > 0) {
            std::cout << "  Packet " << i << ": wait " << wait_us << " us" << std::endl;
            std::this_thread::sleep_for(std::chrono::microseconds(wait_us));
        } else {
            std::cout << "  Packet " << i << ": sent immediately" << std::endl;
        }
        pacer.afterSend(100);
    }
    
    // 测试 2：大包发送（可能需要等待）
    std::cout << "\n[Test 2] Large packet (50 KB):" << std::endl;
    int64_t wait_us = pacer.beforeSend(50'000);
    if (wait_us > 0) {
        std::cout << "  Need to wait: " << wait_us << " us (" << wait_us / 1000.0 << " ms)" << std::endl;
        std::this_thread::sleep_for(std::chrono::microseconds(wait_us));
    } else {
        std::cout << "  Sent immediately" << std::endl;
    }
    pacer.afterSend(50'000);
    
    // 测试 3：动态调整带宽
    std::cout << "\n[Test 3] Adjust bandwidth to 10 Mbps:" << std::endl;
    pacer.setBandwidth(10'000'000);
    std::cout << "  New bandwidth: " << pacer.getBandwidth() / 1'000'000 << " Mbps" << std::endl;
    std::cout << "  Available tokens: " << pacer.getAvailableTokens() << " bytes" << std::endl;
    
    // 等待一些时间让 token 补充
    std::cout << "\n[Wait] Sleeping 100 ms to refill tokens..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::cout << "  Tokens after 100ms: " << pacer.getAvailableTokens() << " bytes" << std::endl;
    
    // 测试 4：连续发送模拟
    std::cout << "\n[Test 4] Continuous send simulation:" << std::endl;
    auto start = std::chrono::steady_clock::now();
    uint64_t total_bytes = 0;
    
    for (int i = 0; i < 10; ++i) {
        uint64_t packet_size = 10'000;  // 10 KB
        wait_us = pacer.beforeSend(packet_size);
        
        if (wait_us > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(wait_us));
        }
        
        pacer.afterSend(packet_size);
        total_bytes += packet_size;
    }
    
    auto end = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    double actual_bw_mbps = (total_bytes * 8.0) / (elapsed_ms / 1000.0) / 1'000'000.0;
    
    std::cout << "  Sent: " << total_bytes << " bytes in " << elapsed_ms << " ms" << std::endl;
    std::cout << "  Actual bandwidth: " << actual_bw_mbps << " Mbps" << std::endl;
    std::cout << "  Expected: 10 Mbps" << std::endl;
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test completed!" << std::endl;
    
    return 0;
}
