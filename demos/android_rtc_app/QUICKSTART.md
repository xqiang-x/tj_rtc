# Android RTC App 快速开始

## 前置条件

1. **安装 Android Studio**
   - 下载: https://developer.android.com/studio
   - 版本: Arctic Fox 或更高

2. **安装 NDK**
   - Android Studio -> Tools -> SDK Manager -> SDK Tools
   - 勾选 "NDK (Side by side)"
   - 版本: 25 或更高

3. **确保项目结构正确**
   ```
   qoder_libuv/
   ├── android_rtc_app/      ← 本应用
   ├── rtc_client_sdk/       ← 必须有
   ├── sfu_server/           ← 必须有
   ├── frame_protoc/         ← 必须有
   └── pacer/                ← 必须有
   ```

## 编译方法（3选1）

### 方法 1: Android Studio（最简单）

```bash
1. 打开 Android Studio
2. File -> Open
3. 选择 android_rtc_app 目录
4. 等待 Gradle 同步
5. 点击 Run 按钮 (绿色三角)
```

### 方法 2: 命令行

```bash
cd android_rtc_app

# macOS/Linux
./gradlew assembleDebug

# Windows
gradlew.bat assembleDebug

# 安装
adb install app/build/outputs/apk/debug/app-debug.apk
```

### 方法 3: 使用构建脚本

```bash
cd android_rtc_app
./build.sh
```

## 使用步骤

### 1. 启动 SFU 服务器

```bash
cd ../sfu_server_full/build
./sfu_server_full --port 9200
```

### 2. 启动另一个推流客户端

可以使用 mac_camera_push 或其他推流工具向服务器推流。

### 3. 打开 Android 应用

1. 启动应用，看到配置界面
2. 输入服务器信息：
   - 服务器地址: `192.168.1.100` (你的服务器IP)
   - 服务器端口: `9200`
   - 流名: `stream1`（与推流端 uid 一致）
   - 用户ID: `android_user`
3. 勾选"开启音频"（可选）
4. 点击"开始拉流"

### 4. 观看视频

- 视频会自动显示
- 如果开启了音频，会听到声音
- 点击"停止"返回

## 配置自动保存

第一次输入的配置会自动保存，下次启动应用时自动加载，无需重复输入。

## 调试

### 查看日志

```bash
# 只看关键日志
adb logcat -s RtcJniBridge:V RtcClient:V StreamActivity:V

# 看所有日志
adb logcat | grep -E "Rtc|Stream"
```

### 常见问题

**Q: 编译失败 "jni.h not found"**
```
解决: 确保已安装 NDK，并在 Android Studio 中配置
```

**Q: 运行时崩溃 "UnsatisfiedLinkError"**
```
解决: 检查项目结构，确保 rtc_client_sdk 等目录存在
```

**Q: 视频黑屏**
```
解决: 
1. 确认 SFU 服务器已启动
2. 确认有推流客户端在推流
3. 查看日志是否有 "视频解码器已配置"
```

**Q: 连接失败**
```
解决:
1. 检查服务器IP和端口
2. 确保手机和服务器在同一网络
3. 检查防火墙设置
```

## 项目文件说明

```
app/src/main/
├── java/com/example/rtcap/
│   ├── MainActivity.java          # 配置输入界面
│   ├── StreamActivity.java        # 视频显示界面
│   ├── RtcClient.java             # JNI封装+编解码
│   └── ConfigManager.java         # 本地存储
├── jni/
│   ├── RtcJniBridge.cpp           # JNI桥接(C++)
│   └── CMakeLists.txt             # NDK编译
├── res/layout/
│   ├── activity_main.xml          # 配置界面
│   └── activity_stream.xml        # 视频界面
└── AndroidManifest.xml            # 权限声明
```

## 下一步

- [ ] 添加推流功能（摄像头采集）
- [ ] 支持切换流
- [ ] 添加聊天功能
- [ ] 支持多人视频

## 技术支持

遇到问题请查看 README.md 中的详细文档。
