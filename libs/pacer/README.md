# Pacer - 带宽控制库

基于 **Token Bucket（令牌桶）** 算法的发送速率控制库，用于精确控制网络带宽。

## 功能特性

- ✅ **精确带宽控制** - Token Bucket 算法实现
- ✅ **动态带宽调整** - 运行时可调整带宽
- ✅ **支持突发传输** - 允许短时间内的 burst
- ✅ **线程安全** - 使用 mutex 保护
- ✅ **默认 50 Mbps** - 开箱即用

## 核心 API

### 创建 Pacer

```cpp
#include "Pacer.h"

// 使用默认带宽 50 Mbps
Pacer pacer;

// 自定义带宽（10 Mbps）
Pacer pacer(10'000'000);

// 自定义带宽和桶大小（0.5 秒突发）
Pacer pacer(10'000'000, 0.5);
```

### 发送前调用

```cpp
uint64_t bytes_to_send = 1400;  // 要发送的字节数

// 计算需要等待的时间
int64_t wait_us = pacer.beforeSend(bytes_to_send);

if (wait_us > 0) {
    // 需要限速：等待指定时间
    std::this_thread::sleep_for(std::chrono::microseconds(wait_us));
}

// 发送数据
sendto(fd, data, bytes_to_send, ...);

// 记录发送完成
pacer.afterSend(bytes_to_send);
```

### 动态调整带宽

```cpp
// 调整到 20 Mbps
pacer.setBandwidth(20'000'000);

// 获取当前带宽
uint64_t bw = pacer.getBandwidth();  // 返回 bits per second

// 获取可用 token
uint64_t tokens = pacer.getAvailableTokens();  // 返回字节数
```

## 算法说明

### Token Bucket 原理

```
时间 ──────────────────────────────────────▶

桶容量：10,000 字节 (0.2s × 50 Mbps)
添加速率：6,250,000 字节/秒 (50 Mbps / 8)

添加 Token ──▶  [████████░░]  8,000/10,000 bytes
                    │
                    │ 消费 2,000 bytes
                    ▼
                [██████░░░░]  6,000/10,000 bytes
```

1. **添加 Token**：按带宽速率持续向桶中添加 token
2. **消费 Token**：发送数据时从桶中移除对应字节数的 token
3. **限速判断**：如果桶中 token 不足，计算需要等待的时间

### 等待时间计算

```
等待时间 (μs) = (deficit_bytes × 1,000,000) / bytes_per_second

例如：
- 需要发送：5,000 bytes
- 当前 token：2,000 bytes
- 带宽：50 Mbps = 6,250,000 bytes/sec
- deficit = 5,000 - 2,000 = 3,000 bytes
- wait_us = (3,000 × 1,000,000) / 6,250,000 = 480 μs
```

## SFU 集成示例

### 自动集成

Pacer 已集成到 SFU 服务端，每个 session 自动创建一个 Pacer 实例：

```cpp
// 在 SfuSession 中
struct SfuSession {
    std::unique_ptr<Pacer> pacer;  // 默认 50 Mbps
    // ...
};

// 创建 session 时自动初始化
session->pacer = std::make_unique<Pacer>(50'000'000);
```

### 动态调整带宽

```cpp
// 调整单个 session 的带宽
sfuServer.SetSessionBandwidth(session_id, 20'000'000);  // 20 Mbps

// 调整整个 room 的带宽
sfuServer.SetRoomBandwidth("room1", 30'000'000);  // 30 Mbps
```

### 发送流程

```cpp
void SfuServerFull::ForwardToSubscribers(...) {
    for (auto sub_session : subscribers) {
        // 1. 封装数据
        auto forward_msg = SerializeForwardData(...);
        
        // 2. Pacer 限速
        if (sub_session->pacer) {
            int64_t wait_us = sub_session->pacer->beforeSend(forward_msg.size());
            // 注意：在事件循环中不能 sleep，只能记录日志或使用定时器
        }
        
        // 3. 实际发送
        sendto(fd, forward_msg.data(), forward_msg.size(), ...);
        
        // 4. 记录发送完成
        if (sub_session->pacer) {
            sub_session->pacer->afterSend(forward_msg.size());
        }
    }
}
```

## 测试

### 编译测试程序

```bash
cd /Users/mac/Documents/qoder_libuv/pacer/build
cmake ..
make

# 运行测试
./test_pacer
```

### 运行 SFU 集成示例

```bash
cd /Users/mac/Documents/qoder_libuv/pacer
g++ -std=c++17 -I.. sfu_pacer_example.cpp ../pacer/Pacer.cpp -o test_sfu_pacer
./test_sfu_pacer
```

## 配置参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `bandwidth_bps` | 50,000,000 | 带宽（bits per second） |
| `bucket_size_sec` | 0.2 | 桶大小（秒数），允许突发 |

### 桶大小计算

```
bucket_size_bytes = bandwidth_bytes_per_sec × bucket_size_sec

例如 50 Mbps, 0.2s：
bucket_size = 6,250,000 × 0.2 = 1,250,000 bytes (约 1.2 MB)
```

这意味着在短时间内可以突发发送最多 1.2 MB 的数据。

## 性能影响

- **内存占用**：每个 Pacer 实例约 100 字节
- **CPU 开销**：每次 beforeSend/afterSend 约 100-200 纳秒
- **线程安全**：使用 mutex，多线程环境下有轻微开销

## 注意事项

1. **事件循环中不能 sleep**：在 libev 等事件循环中，`beforeSend` 返回的等待时间不能直接 sleep，应该：
   - 使用定时器延迟发送
   - 或者将数据加入队列，由事件循环控制发送节奏

2. **UDP vs TCP**：
   - UDP：发送后立即返回，限速效果是统计意义上的
   - TCP：可以使用发送队列实现更精确的限速

3. **带宽单位**：统一使用 **bits per second (bps)**，不是 bytes

## 许可证

MIT License
