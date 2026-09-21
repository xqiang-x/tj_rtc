# Android RTC 推拉流应用

基于 rtc_client_sdk 的 Android 音视频推拉流应用，使用 MediaCodec 硬解 H.264 视频流。

## 功能特性

- ✅ **配置界面** - 输入服务器地址、端口、流名（与推流端一致）、用户ID
- ✅ **本地存储** - 自动保存配置，下次启动自动加载
- ✅ **视频拉流** - 接收 H.264 视频流并硬解码显示
- ✅ **音频播放** - 可选开关，播放 PCM 音频（48kHz, 立体声, 16-bit）
- ✅ **FEC 抗丢包** - 基于 cm256 的前向纠错
- ✅ **UDP 传输** - 高效低延迟传输

## 项目结构

```
android_rtc_app/
├── app/
│   ├── src/main/
│   │   ├── java/com/example/rtcap/
│   │   │   ├── MainActivity.java          # 配置输入界面
│   │   │   ├── StreamActivity.java        # 视频显示界面
│   │   │   ├── RtcClient.java             # JNI 封装 + 编解码
│   │   │   └── ConfigManager.java         # 本地存储
│   │   ├── jni/
│   │   │   ├── RtcJniBridge.cpp           # JNI 桥接层
│   │   │   └── CMakeLists.txt             # NDK 编译配置
│   │   ├── res/
│   │   │   ├── layout/
│   │   │   │   ├── activity_main.xml      # 配置界面布局
│   │   │   │   └── activity_stream.xml    # 视频界面布局
│   │   │   └── values/
│   │   │       └── strings.xml            # 字符串资源
│   │   └── AndroidManifest.xml            # 权限声明
│   └── build.gradle                        # App 构建配置
├── build.gradle                            # 项目构建配置
├── settings.gradle                         # 项目设置
└── README.md                               # 本文档
```

## 编译要求

### 环境要求

- **Android Studio** Arctic Fox 或更高版本
- **NDK** 25+ (支持 C++17)
- **CMake** 3.22+
- **Android SDK** API 34 (Android 14)
- **最低支持** Android 7.0 (API 24)

### 依赖项目

确保以下项目在同一目录下：

```
qoder_libuv/
├── android_rtc_app/      # 本应用
├── rtc_client_sdk/       # RTC 通信 SDK
├── sfu_server/           # SFU 服务器库
├── frame_protoc/         # FEC 编解码库
└── pacer/                # 带宽控制库
```

## 编译步骤

### 方法 1：使用 Android Studio（推荐）

1. 打开 Android Studio
2. File -> Open -> 选择 `android_rtc_app` 目录
3. 等待 Gradle 同步完成
4. Build -> Make Project (Ctrl+F9 / Cmd+F9)
5. Run -> Run 'app' (Shift+F10)

### 方法 2：命令行编译

```bash
cd android_rtc_app

# Linux/macOS
./gradlew assembleDebug

# Windows
gradlew.bat assembleDebug

# 安装到设备
adb install app/build/outputs/apk/debug/app-debug.apk
```

## 使用方法

### 1. 启动 SFU 服务器

确保 SFU 服务器已启动并监听端口 9200：

```bash
cd sfu_server_full/build
./sfu_server_full --port 9200
```

### 2. 配置 Android 应用

1. 打开应用，进入配置界面
2. 输入服务器地址（例如：`192.168.1.100`）
3. 输入服务器端口（例如：`9200`）
4. 输入流名（例如：`stream1`，即推流端 uid）
5. 输入用户ID（例如：`android_user`）
6. 勾选"开启音频"（可选）
7. 点击"开始拉流"

### 3. 开始观看

- 视频会自动显示在屏幕上
- 如果勾选了音频，会同时播放声音
- 点击"停止"按钮返回配置界面

## 技术架构

### 数据流

```
┌─────────────────────────────────────────┐
│  Android UI (Java)                      │
│  ┌─────────────┐    ┌──────────────┐   │
│  │ MainActivity│    │StreamActivity│   │
│  └──────┬──────┘    └──────┬───────┘   │
│         │                  │            │
└─────────┼──────────────────┼────────────┘
          │                  │
          ▼                  ▼
┌─────────────────────────────────────────┐
│  RtcClient.java (JNI + 编解码)          │
│  ┌──────────────┐  ┌─────────────────┐ │
│  │ MediaCodec   │  │  AudioTrack     │ │
│  │ (H.264 解码) │  │  (PCM 播放)     │ │
│  └──────┬───────┘  └────────┬────────┘ │
│         │                   │           │
└─────────┼───────────────────┼───────────┘
          │                   │
          ▼                   ▼
┌─────────────────────────────────────────┐
│  RtcJniBridge.cpp (C++ JNI)             │
│  ┌──────────────────────────────────┐  │
│  │  pull::Subscriber (拉流端)      │  │
│  │  ┌──────────────┐               │  │
│  │  │ FrameReceiver│               │  │
│  │  └──────┬───────┘               │  │
│  └─────────┼────────────────────────┘  │
└────────────┼───────────────────────────┘
             │
             ▼
┌─────────────────────────────────────────┐
│  UDP/TCP 网络传输                       │
│  ┌──────────────┐                       │
│  │ FEC 重组     │                       │
│  └──────────────┘                       │
└─────────────────────────────────────────┘
```

### 视频解码流程

```
1. JNI 接收 H.264 NALU
   ↓
2. 调用 Java onVideoFrame()
   ↓
3. 解析帧类型（第1字节）
   - 0x01: VIDEO_PARAMS (SPS/PPS)
   - 0x02: VIDEO_IDR (关键帧)
   - 0x03: VIDEO_P (P帧)
   ↓
4. 如果是 SPS/PPS，配置 MediaCodec
   ↓
5. 将 payload 喂给 MediaCodec
   ↓
6. MediaCodec 硬解码到 Surface
   ↓
7. SurfaceView 显示视频
```

### 音频播放流程

```
1. JNI 接收 PCM 数据
   ↓
2. 调用 Java onAudioFrame()
   ↓
3. 检查 enableAudio 开关
   ↓
4. 跳过帧类型字节
   ↓
5. AudioTrack 播放（48kHz, 立体声, 16-bit）
```

## 关键实现细节

### JNI 线程安全

JNI 回调在 SDK 的工作线程中执行，需要 AttachCurrentThread：

```cpp
static JNIEnv* AttachCurrentThread() {
    JNIEnv* env = nullptr;
    if (g_java_vm) {
        g_java_vm->AttachCurrentThread(&env, nullptr);
    }
    return env;
}
```

### MediaCodec 配置

需要先收到 SPS/PPS 参数集才能初始化解码器：

```java
if (frameType == 0x01 && !mHasSPSPPS) {
    MediaFormat format = MediaFormat.createVideoFormat(
        MediaFormat.MIMETYPE_VIDEO_AVC, 1280, 720);
    format.setByteBuffer("csd-0", ByteBuffer.wrap(spsPps));
    mVideoDecoder.configure(format, mSurface, null, 0);
    mVideoDecoder.start();
    mHasSPSPPS = true;
}
```

### 本地存储

使用 SharedPreferences 保存配置：

```java
prefs.edit()
    .putString("server_ip", serverIp)
    .putInt("server_port", port)
    .apply();
```

## 调试技巧

### 查看日志

```bash
# 查看应用日志
adb logcat -s RtcJniBridge RtcClient StreamActivity

# 查看所有日志
adb logcat | grep -E "Rtc|Stream"
```

### 常见问题

**1. 编译失败：找不到 jni.h**
- 确保已安装 Android NDK
- 在 Android Studio 中配置 NDK 路径

**2. 运行时崩溃：UnsatisfiedLinkError**
- 检查 CMakeLists.txt 路径是否正确
- 确保所有源文件都存在

**3. 视频黑屏**
- 检查是否收到 SPS/PPS
- 查看日志中是否有 "视频解码器已配置"

**4. 连接失败**
- 确认服务器地址和端口正确
- 检查网络连接（WiFi/移动数据）
- 确认 SFU 服务器已启动

## 后续扩展

### 推流功能

添加摄像头推流需要：

1. 使用 Camera2 API 采集视频
2. 使用 MediaCodec 编码 H.264
3. 调用 `nativePushVideoFrame()` 推送

### 其他编解码器

- **VP8/VP9** - 修改 MediaCodec MIME 类型
- **AV1** - 需要 Android 10+ 支持
- **Opus** - 音频编码（需要第三方库）

## 许可证

本项目基于 rtc_client_sdk 构建，遵循相同的许可协议。
