# RTC Client SDK

跨平台的音视频推拉流通信 SDK。基于**流（Stream）模型**：

- **流（streamId）**：推流端注册流时使用的唯一名字，全局唯一。一条流 = 1 个推流端（1 视频 + 1 音频 + 1 消息）+ 多个拉流端。
- **publisher（推流）**：`push::Publisher` 以流名注册并发布一路流。
- **subscriber（拉流）**：`pull::Subscriber` 以流名订阅一条已存在的流。

> 没有"房间"概念：推流 uid 就是流名，拉流时填同一个流名即可。

## 特性

- ✅ **推拉分离** - `push::Publisher` / `pull::Subscriber` 两个独立类
- ✅ **UDP/TCP 双模式** - 支持 UDP（带 FEC）和 TCP 传输
- ✅ **一路流三通道** - 1 视频 + 1 音频 + 1 消息（消息复用信令通道）
- ✅ **带宽控制** - 内置 Pacer 限速（Token Bucket 算法）
- ✅ **FEC 抗丢包** - 基于 cm256 的前向纠错
- ✅ **码率自适应** - UDP 模式内置 BitrateController 状态机
- ✅ **P2P 回退** - 多订阅者时经服务器协商 P2P 直连
- ✅ **轻量级** - 只包含通信核心，编解码由用户自行实现

## 目录结构

```
rtc_client_sdk/
├── include/              # 头文件
│   ├── Publisher.h      # 推流端（push::Publisher）
│   ├── Subscriber.h     # 拉流端（pull::Subscriber）
│   ├── P2PManager.h     # P2P 协商管理
│   ├── SimulatedVideoSource.h  # 模拟视频源（测试用）
│   ├── SimulatedAudioSource.h  # 模拟音频源（测试用）
│   └── SfuFrameType.h   # 帧类型枚举
├── src/                  # 源文件
│   ├── Publisher.cpp
│   ├── Subscriber.cpp
│   └── P2PManager.cpp
├── examples/             # 示例代码
│   ├── example_usage.cpp     # 推拉一体演示
│   ├── test_pli.cpp          # 关键帧请求（PLI）演示
│   └── test_ctrl_channel.cpp # 消息通道演示
├── CMakeLists.txt        # 编译配置
└── README.md             # 本文档
```

**注意**：本 SDK 只包含通信核心，不包含编解码器。用户需要根据平台自行实现：
- 视频采集（摄像头）
- 视频编码/解码（H.264/VP8/AV1 等）
- 音频采集（麦克风）
- 音频编码/解码（Opus/AAC 等）
- 视频渲染（SDL2/Metal/OpenGL 等）

## 快速开始

### 1. 编译

```bash
cd rtc_client_sdk
mkdir -p build && cd build
cmake ..
make -j4
```

### 2. 运行示例

```bash
# 确保 SFU 服务器已启动
./rtc_sdk_example
```

### 3. 集成到您的项目

```cmake
# 添加子目录
add_subdirectory(${CMAKE_SOURCE_DIR}/rtc_client_sdk)

# 链接 SDK
target_link_libraries(your_app rtc_client_sdk)
```

## API 使用

### 推流（push::Publisher）

```cpp
#include "Publisher.h"

// 创建推流端（以流名注册并发布一路流）
push::Publisher pub;

pub.SetOnConnected([](uint32_t sessionId) {
    std::cout << "连接成功: " << sessionId << std::endl;
});
pub.SetOnDisconnected([]() {
    std::cout << "连接断开" << std::endl;
});
// 关键帧请求（PLI）：订阅端丢帧时触发，编码线程应在下一帧强制 IDR
pub.SetOnKeyFrameRequest([]() {
    request_idr = true;
});

// 启动：连接服务器 → 注册流（streamId 即流名，全局唯一）→ 进入推流循环
// 参数：服务器IP, TCP端口, 流名, 用户ID, UDP模式, UDP端口(0=port+1)
if (!pub.Start("127.0.0.1", 9200, "stream1", "user1", true, 0)) {
    return -1;
}

// 推送视频帧（H.264 ES，含 SPS/PPS 参数集）
pub.PushVideoFrame(spsPps, len, true, false);   // 参数集帧
pub.PushVideoFrame(idr, len, false, true);      // IDR 关键帧
pub.PushVideoFrame(pFrame, len, false, false);  // 普通帧

// 推送音频帧（PCM 16-bit）
pub.PushAudioFrame(pcmData, pcmLen);

// 给所有订阅端发消息（复用信令通道）
uint8_t cmd[] = {0xAA};
pub.SendCtrlDownstream(cmd, sizeof(cmd), true);  // reliable=true 走可靠通道

pub.Stop();
```

### 拉流（pull::Subscriber）

```cpp
#include "Subscriber.h"
#include "SfuFrameType.h"

// 创建拉流端（以流名订阅一条已存在的流）
pull::Subscriber sub;

sub.SetOnConnected([](uint32_t sessionId) {
    std::cout << "订阅成功: " << sessionId << std::endl;
});
sub.SetOnDisconnected([]() {
    std::cout << "连接断开" << std::endl;
});

// 帧回调：所有帧走同一回调，按 data[0] 的 FrameType 区分
sub.SetOnFrame([](uint32_t sourceSessionId, std::vector<uint8_t>&& data) {
    auto type = static_cast<sfu::FrameType>(data[0]);
    if (type == sfu::FrameType::VIDEO_PARAMS ||
        type == sfu::FrameType::VIDEO_IDR ||
        type == sfu::FrameType::VIDEO_P) {
        // 视频帧（去掉帧头字节后送解码器）
        decoder.Decode(data.data() + 1, data.size() - 1);
    } else if (type == sfu::FrameType::AUDIO_PCM) {
        // 音频帧
        player.Play(data.data() + 1, data.size() - 1);
    }
});

// 启动：连接服务器 → 订阅流（streamId 与推流端一致）
if (!sub.Start("127.0.0.1", 9200, "stream1", "user2", true, 0)) {
    return -1;
}

// 帧数据通过回调接收...

// 给推流端发消息 / 请求关键帧
uint8_t cmd[] = {0xBB};
sub.SendCtrlUpstream(cmd, sizeof(cmd), true);
sub.RequestKeyFrame();  // 手动 PLI

sub.Stop();
```

### 帧类型

```cpp
namespace sfu {

enum class FrameType : uint8_t {
    VIDEO_PARAMS  = 0x01,  // 视频参数帧（SPS/PPS）
    VIDEO_IDR     = 0x02,  // 视频 IDR 帧（关键帧）
    VIDEO_P       = 0x03,  // 视频 P 帧（普通帧）
    AUDIO_PCM     = 0x10,  // PCM 音频帧（16-bit）
};

// 辅助函数
bool IsVideoFrame(FrameType type);
bool IsAudioFrame(FrameType type);
const char* FrameTypeName(FrameType type);

} // namespace sfu
```

## 依赖

- **C++17** 或更高
- **线程库** - pthread (Linux/macOS) 或原生线程 (Windows)
- **外部库**（通过相对路径引用，已包含在 SDK 中）：
  - `libs/frame_protoc` - FEC 编解码
  - `libs/pacer` - 带宽控制
  - `libs/mem_pool` - 内存池
  - `common/sfu_protocol` - 推流/订阅协议定义

**无平台特定依赖**，可在以下平台编译：
- ✅ Linux (GCC/Clang)
- ✅ macOS (Clang)
- ✅ Windows (MSVC/MinGW)

## 架构

```
┌─────────────────────────┐      ┌─────────────────────────┐
│  push::Publisher        │      │  pull::Subscriber       │
│  (推流端)               │      │  (拉流端)               │
├─────────────────────────┤      ├─────────────────────────┤
│  FrameSender(FEC)       │      │  FrameReceiver (FEC 重组)│
│  Pacer (带宽控制)       │      │  BitrateController 反馈 │
│  BitrateController      │      │  P2PManager             │
│  P2PManager             │      │                         │
├─────────────────────────┴──────┴─────────────────────────┤
│              SFU 协议层 (SfuProtocol / P2PProtocol)      │
├─────────────────────────────────────────────────────────┤
│              UDP/TCP 传输层                               │
└─────────────────────────────────────────────────────────┘

用户需要自行实现：
┌─────────────────────────────────────┐
│  [视频采集] → [编码器] → PushVideo  │
│  PullVideo  → [解码器] → [渲染器]   │
│  [音频采集] → PushAudio             │
│  PullAudio  → [解码器] → [播放器]   │
└─────────────────────────────────────┘
```

## License

MIT