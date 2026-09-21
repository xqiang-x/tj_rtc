# raspberry_pi_pusher

模仿 [`rpicam-apps`](https://github.com/raspberrypi/rpicam-apps) 的极简版**树莓派视频采集 + H.264 硬件编码 + 推流**程序，使用 [`rtc_client_sdk`](../../rtc_client_sdk) 把视频推到 SFU 服务器。

整个流水线只保留最核心的代码路径，方便阅读与裁剪：

```
 ┌────────────────────┐  YUV420   ┌────────────────────┐  H.264 ES   ┌────────────────┐  NAL  ┌────────────────┐
 │ libcamera 采集     │ ────────> │ V4L2 H.264 编码    │ ──────────> │ NaluSplitter   │ ────> │ rtc_client_sdk │
 │ (DMA-buf, /dev/v.) │  DMABUF   │ (/dev/video11)     │             │ (SPS/PPS/IDR)  │       │ (SFU 推流)      │
 └────────────────────┘           └────────────────────┘             └────────────────┘       └────────────────┘
```

## 模块对应关系

| 文件 | 对应 rpicam-apps |
| --- | --- |
| `src/main.cpp` | `apps/rpicam_vid.cpp`（去掉 preview / options 解析等） |
| `src/LibcameraVideoSource.{h,cpp}` | `core/rpicam_app.cpp` 的相机/缓冲管理子集 |
| `src/H264V4l2Encoder.{h,cpp}` | `encoder/h264_encoder.{hpp,cpp}` |
| `src/DmaHeap.{h,cpp}` | `core/dma_heaps.{hpp,cpp}` |
| `src/NaluSplitter.{h,cpp}` | 新增：把 H.264 ES 拆分为 SPS/PPS/IDR/P 推送 |

> 区别：rpicam-apps 把编码后的码流写到 `Output`（文件 / TCP / UDP），本程序把 NAL 拆分成 `VIDEO_PARAMS / VIDEO_IDR / VIDEO_P` 三类后，调用 `push::Publisher::PushVideoFrame()` 走 SDK 自带的 FEC + Pacer。

## 依赖

构建机要求 **Raspberry Pi OS（Bookworm 及以后）**，并安装：

- `libcamera`（含开发头）
- `cmake >= 3.14`，`g++ >= 9`，`pkg-config`

```bash
sudo apt install -y libcamera-dev libcamera-tools cmake g++ pkg-config
```

> 编码端要求 `/dev/video11` 可访问（即 V4L2 H.264 编码节点，Pi 4/5 默认存在）。

## 构建

```bash
cd /path/to/raspberry_pi_pusher
mkdir build && cd build
cmake ..
make -j4
```

会一并以子目录方式构建 `rtc_client_sdk` 静态库。

产物：

- `raspberry_pi_pusher`：主程序

## 运行

最简形式（默认 1280x720 @ 30fps，2 Mbps，UDP 推到 `127.0.0.1:9200`）：

```bash
./raspberry_pi_pusher
```

常用参数：

```bash
./raspberry_pi_pusher \
    --width 1280 --height 720 --fps 30 \
    --bitrate 2500 --gop 30 \
    --server 192.168.1.100 --port 9200 \
    --stream stream1 --user pi_pusher \
    --duration 0
```

`--help` 查看完整参数列表。`Ctrl+C` 优雅停止。

## 已做的简化

相对于 `rpicam-apps`，本程序刻意去掉了：

- Preview 预览窗口（DRM/EGL）
- Post-processing 流水线（OpenCV、HailoRT 等）
- 多 stream（lores / raw / still）
- ZSL、HDR、AF、AE/AWB 复杂控件
- 音频
- `Options` 解析框架（保留极少量命令行参数）
- libav / mjpeg / circular / net 等多路输出（统一只走 SDK）

## 注意事项

1. `LibcameraVideoSource` 直接用 `DmaHeap` 申请 FrameBuffer 是关键，这样 V4L2 编码器才能以 `V4L2_MEMORY_DMABUF` 接受它的 fd（与 rpicam-apps 思路一致）。
2. `H264V4l2Encoder` 强制开启 `V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER`，确保每个 IDR 之前都附带 SPS+PPS，便于拉流端在中途加入。
3. `NaluSplitter` 采用 Annex-B 起始码扫描；连续的 SPS/PPS 会合并为一个 `VIDEO_PARAMS` 帧推送，IDR 走 `VIDEO_IDR`，其余 slice 走 `VIDEO_P`。
4. `--bitrate` 单位是 **kbps**；`--pacer` 单位是 **Mbps**。

## 排错

- `无法打开 /dev/video11`：确认是 Pi 4/5 而不是 Pi 5 关掉硬编码节点的精简镜像；
- `DmaHeap` 失败：`/dev/dma_heap/vidbuf_cached` 不存在时，需要确保 libcamera 的内核驱动已加载；
- 推流连不上：确认 SFU 服务器在 `--server` 指定 IP/端口监听，并允许该 `room/user`。
