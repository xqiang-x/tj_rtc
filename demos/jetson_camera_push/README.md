# jetson_camera_push

Jetson Nano 专用推流 demo：CSI 摄像头采集 → **H.264 硬件编码（NVENC）** → RTC 推流到 SFU。

```
/dev/video0（CSI IMX219，Argus sensor-id 0）
  → nvarguscamerasrc（ISP，NV12/NVMM）
  → nvvidconv（缩放 → I420/NVMM）
  → nvv4l2h264enc（Tegra NVENC 硬编）
  → h264parse（字节流/AU 对齐）
  → appsink → NaluSplitter → Publisher --(UDP/TCP)--> SFU 106.15.177.248
```

## 设备与模式说明（2026-10-09 实测，nano 192.168.50.75）

- `/dev/video0` = `vi-output, imx219 6-0010`，即 CSI 排线上的 **IMX219**（RPi Camera v2 同款）。
- **v4l2 直采路径（`nvv4l2camerasrc`/`v4l2src`）在本机只有 4K@21 出帧**，其余模式协商通过但无帧
  （`v4l2src` 枚举显示 RG10 raw Bayer）；因此本 demo 走 **NVIDIA Argus 官方路径**。
- `nvarguscamerasrc` 实测 6 种模式全部可用：

  | 模式 | 帧率 |
  |---|---|
  | 3264x2464 | 21 |
  | 3264x1848 | 28 |
  | 1920x1080 | 30 |
  | 1640x1232 | 30 |
  | 1280x720 | 60 |
  | 1280x720 | 120 |

- 默认参数：采集 1920x1080@30 → 缩放编码 1280x720@30 / 2500 kbps / GOP 30（≈1s），
  与 pi4_air + 手机 QOpenHD 链路一致的量级。
- **PLI（关键帧请求）**：本版编码器 `idrinterval`/`iframeinterval` 仅允许在 NULL/READY 状态设置，
  无运行时 force-idr；PLI 只计数，IDR 由 GOP 周期自然满足（与 pi4_air 行为一致）。

## 依赖

- JetPack 4.6（L4T R32.7.x）：`nvidia-l4t-camera`（nvargus-daemon 需在运行）、
  `nvidia-l4t-gstreamer`（nvv4l2h264enc 等插件）、`gstreamer1.0`/`libgstreamer-app1.0-dev`
- **cmake ≥ 3.14**（Ubuntu 18.04 自带 3.10.2 不够）。nano 上已装 `~/tools/cmake-3.28.3`
  （pypi `manylinux2014_aarch64` wheel 解出，官方 aarch64 二进制下载慢时的替代）：
  `~/tools/cmake-3.28.3/bin/cmake --build demos/jetson_camera_push/build -j2`

## 构建（在 jetson nano 上）

```bash
cd ~/tj_rtc
CMAKE=~/tools/cmake-3.28.3/bin/cmake   # 系统自带 cmake 3.10 太旧
$CMAKE -S demos/jetson_camera_push -B demos/jetson_camera_push/build
$CMAKE --build demos/jetson_camera_push/build -j2
```

## 运行

```bash
# 默认：推 jetsonvideo → 106.15.177.248:9200（UDP），采集 1080p30，编码 720p30
./demos/jetson_camera_push/build/jetson_camera_push

# 常用变体
./jetson_camera_push --tcp                      # 媒体走 TCP（绕开 UDP 入向黑洞场景）
./jetson_camera_push --bitrate 4000             # 提高码率
./jetson_camera_push --cap-w 1280 --cap-h 720 --cap-fps 60   # 采集 720p60（编码同尺寸）
./jetson_camera_push --user jetson2             # 换 user id（多客户端同流规避会话劫持）
```

后台常驻（建议）：

```bash
nohup setsid ./jetson_camera_push > ~/tj_rtc/jetson_push.log 2>&1 < /dev/null &
echo $! > ~/tj_rtc/jetson_push.pid
```

## 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--server` / `--port` | 106.15.177.248 / 9200 | SFU 地址（TCP 信令端口） |
| `--tcp` | 关（UDP） | UDP 媒体 + TCP 信令；`--tcp` 则全 TCP |
| `--stream` / `--user` | jetsonvideo / jetson_nano | 流名 / 用户 ID |
| `--bandwidth` | 20 | UDP Pacer 带宽（Mbps） |
| `--sensor-id` | 0 | Argus 传感器编号（0 = video0 的 IMX219） |
| `--cap-w/--cap-h/--cap-fps` | 1920/1080/30 | 采集模式（须匹配上表模式） |
| `--width/--height` | 1280/720 | 编码尺寸（nvvidconv 缩放） |
| `--bitrate` | 2500 | 编码码率 kbps |
| `--gop` | 30 | IDR 间隔（帧），30 帧 @30fps ≈ 1s |
| `--seconds` | 0（无限） | 运行时长 |

## 验证判据

- 启动后 `[push] CONNECTED sessionId=...`；
- 每秒统计 `fps≈30 kbps≈2500 key 按 GOP 递增`；
- 订阅端（手机 QOpenHD 或 `pull_probe`）收到 `jetsonvideo` 流并出画；
- 订阅端请求关键帧时本端打印 `收到 PLI`（IDR 在 ≤1s 内由 GOP 周期提供）。

## 排障

- `[gst] 缺少插件 ...`：确认 JetPack gstreamer 插件存在（`gst-inspect-1.0 nvv4l2h264enc`）。
- 相机打不开：`systemctl status nvargus-daemon`；先确认没有其他进程占用
  （`fuser -v /dev/video0`，Argus 与 v4l2 直采不能同时开）。
- 采集无帧：`--cap-w/--cap-h/--cap-fps` 必须为设备支持的模式（见上表），
  其它组合（如 720p@30）Argus 会退选最接近模式，实际帧率以统计为准。
- 服务器上同名流残留（换端重启时）：SFU 释放旧 stream 需 ~15s，日志出现
  `订阅失败: STREAM_EXISTS` 自动重试属正常。