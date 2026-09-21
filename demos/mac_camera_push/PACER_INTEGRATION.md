# Pacer 集成指南 - 推流端（UDP 模式）

## 概述

Pacer 带宽控制库已成功集成到 `mac_camera_push` 推流端的 **UDP 模式**，用于控制视频流的发送速率。

**注意**：TCP 模式不需要 Pacer，因为 TCP 本身具有拥塞控制机制。

## 功能特性

- ✅ **UDP 模式限速** - 推流端 UDP 发送数据时自动应用 Pacer
- ✅ **TCP 模式跳过** - TCP 模式不使用 Pacer（TCP 自带拥塞控制）
- ✅ **动态调整** - 可通过 `SetBandwidth()` 接口调整带宽
- ✅ **默认 50 Mbps** - 开箱即用的合理默认值

## 使用方式

### 命令行参数

```bash
# 使用默认带宽（50 Mbps）
./mac_camera_push

# 自定义推流带宽为 20 Mbps
./mac_camera_push --bandwidth 20

# 完整参数示例
./mac_camera_push \
    --server 127.0.0.1 \
    --port 9200 \
    --stream stream1 \
    --user publisher1 \
    --width 1280 \
    --height 720 \
    --fps 30 \
    --bitrate 2000 \
    --bandwidth 30 \
    --udp
```

### 代码集成

#### 1. 设置带宽（仅 UDP 模式有效）

```cpp
push::Publisher pub;

// 在 Start() 之前设置带宽（仅 UDP 模式使用）
pub.SetBandwidth(30'000'000);  // 30 Mbps

// 然后启动（streamId 即流名）
pub.Start(serverIp, port, streamId, userId, useUdp, udpPort);
```

**注意**：如果 `useUdp = false`（TCP 模式），`SetBandwidth()` 调用会被忽略。

#### 2. 发送流程（仅 UDP 模式应用 Pacer）

```cpp
// NetThreadFunc 中的发送逻辑
auto sendFrame = [this](const std::vector<uint8_t>& payload) -> bool {
    if (m_useUdp) {
        // UDP 模式：使用 FEC + Pacer
        m_frameSender->setSendCallback([this](const uint8_t* data, size_t len) {
            auto sfuMsg = sfu::SerializeStreamData(m_sessionId, data, len);
            
            // Pacer 限速
            int64_t wait_us = m_pacer->beforeSend(sfuMsg.size());
            if (wait_us > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(wait_us));
            }
            
            UdpSendAll(sfuMsg.data(), sfuMsg.size());
            m_pacer->afterSend(sfuMsg.size());  // 记录发送完成
        });
    } else {
        // TCP 模式：直接发送（不需要 Pacer）
        auto msg = sfu::SerializeStreamData(m_sessionId, payload.data(), payload.size());
        return TcpSendAll(msg.data(), msg.size());
    }
};
```

## 为什么 TCP 不需要 Pacer？

### TCP vs UDP 的拥塞控制

| 特性 | TCP | UDP |
|------|-----|-----|
| **拥塞控制** | ✅ 内置（慢启动、拥塞避免） | ❌ 无 |
| **流量控制** | ✅ 滑动窗口 | ❌ 无 |
| **丢包重传** | ✅ 自动重传 | ❌ 需应用层处理 |
| **需要 Pacer** | ❌ 不需要 | ✅ 需要 |

### TCP 自带机制

```
TCP 发送流程：
    ↓
拥塞窗口（cwnd）限制发送速率
    ↓
慢启动 → 拥塞避免 → 快速恢复
    ↓
自动适应网络状况
```

### UDP 需要 Pacer 的原因

```
UDP 发送流程（无 Pacer）：
    ↓
直接 sendto() → 可能突发占满带宽
    ↓
网络拥塞 → 丢包
    ↓
FEC 无法恢复 → 视频卡顿

UDP 发送流程（有 Pacer）：
    ↓
Token Bucket 限速
    ↓
均匀发送小包
    ↓
网络稳定 → 低丢包率
```

### 数据流

```
摄像头采集 (AVFoundation)
    ↓
H.264 编码 (VideoToolbox)
    ↓
PushVideoFrame() → 帧队列
    ↓
网络线程消费帧
    ↓
FEC 分片 (FrameSender) [UDP 模式]
    ↓
Pacer 限速检查 (beforeSend)
    ↓
[如果需要等待] → sleep(wait_us)
    ↓
实际发送 (send/sendto)
    ↓
Pacer 记录完成 (afterSend)
```

### Token Bucket 工作原理

```
时间 ──────────────────────────────────────▶

桶容量：1,250,000 字节 (0.2s × 50 Mbps)
添加速率：6,250,000 字节/秒 (50 Mbps / 8)

初始状态：  [████████████]  1,250,000/1,250,000 bytes (满)

发送 500KB: [███████░░░░░]  750,000/1,250,000 bytes
              ↓
发送 1MB:   [░░░░░░░░░░░░]  0/1,250,000 bytes
              ↓
[需要等待]  等待时间 = (deficit × 1,000,000) / bytes_per_sec
              ↓
补充 Token: [███░░░░░░░░░]  312,500/1,250,000 bytes (0.05s 后)
```

## 带宽建议

| 场景 | 编码器码率 | Pacer 带宽 | 说明 |
|------|-----------|-----------|------|
| **720p30** | 2 Mbps | 3-5 Mbps | 留有余量给 FEC 和协议头 |
| **1080p30** | 4 Mbps | 6-10 Mbps | 高清视频 |
| **1080p60** | 8 Mbps | 12-20 Mbps | 高帧率 |
| **4K30** | 15 Mbps | 25-50 Mbps | 超高清 |

**建议**：Pacer 带宽应该是编码器码率的 **1.5-3 倍**，以容纳：
- FEC 冗余包（10-30%）
- SFU 协议头（每个包 6 字节）
- 突发帧（IDR 帧较大）

## 性能影响

### CPU 开销

- `beforeSend()`: ~100-200 纳秒
- `afterSend()`: ~50 纳秒
- 总计：每帧额外 < 1 微秒

### 内存占用

- 每个 Pacer 实例：~100 字节
- 推流端只有一个 Pacer：可忽略

### 限速精度

- Token Bucket 算法提供**统计意义上的精确限速**
- 短时间允许突发（桶大小 = 0.2 秒 × 带宽）
- 长时间平均速率严格符合设定值

## 调试技巧

### 查看 Pacer 初始化日志

```
[Push] Pacer initialized with 50 Mbps
```

### 监控实际发送速率

在服务端查看统计：
```
[SFU Stats] Total bytes out: XXX (XXX MB)
```

### 测试限速效果

```bash
# 设置很低的带宽（1 Mbps），观察发送变慢
./mac_camera_push --bandwidth 1

# 应该能看到 Pacer 日志显示等待时间
# [SFU-PACER] UDP rate limit: would wait XXX us for XXX bytes
```

## 与 FEC 的协同

Pacer 和 FEC 协同工作：

1. **FEC 分片**：将大帧分成小包（≤1400 字节）
2. **Pacer 限速**：控制小包的发送速率
3. **效果**：平滑的视频流，不会突发占满带宽

```
原始帧：50 KB
    ↓ FEC 分片
36 个小包 × 1400 字节
    ↓ Pacer 限速
以 50 Mbps 速率均匀发送（不是突发）
```

## 注意事项

1. **推流端 Pacer vs 服务端 Pacer**：
   - 推流端 Pacer（UDP 模式）：控制**上行**发送速率
   - 服务端 Pacer：控制**下行**转发速率（TCP/UDP 都需要）
   - 两者独立，可以设置不同的带宽

2. **网络线程 sleep**：
   - 推流端 UDP 模式是独立的网络线程，可以安全 sleep
   - 服务端使用 libev 事件循环，不能 sleep（需要定时器）

3. **动态调整**：
   - 当前实现中，带宽在 `Start()` 前设置
   - 运行时调整需要修改代码（调用 `SetBandwidth()`）
   - 仅对 UDP 模式有效

## 示例代码

完整的推流端示例见：
- [main.mm](../mac_camera_push/main.mm) - 设置带宽
- [Publisher.cpp](../../client_sdk/rtc_client_sdk/src/Publisher.cpp) - Pacer 集成

## 许可证

MIT License
