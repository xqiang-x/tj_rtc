# pi4_air

树莓派 4 专用 **air 端**：摄像头采集 + H.264 硬件编码 → tj_rtc 推流，同时桥接 Pixhawk 串口 MAVLink。

由 `demos/raspberry_pi_pusher`（视频管线）与 `rtc_local_air`（MAVLink 桥）合并而来：

```
 ┌───────────────────┐ YUV420 ┌──────────────────┐ H.264 ES ┌──────────────┐ NAL ┌──────────────┐
 │ libcamera 采集    │ ─────> │ V4L2 硬编        │ ───────> │ NaluSplitter │ ──> │ rtc_client_sdk│──> SFU ──> 手机
 │ (CSI/USB 摄像头)  │ DMABUF │ (/dev/video11)   │          │(SPS/PPS|IDR) │     │ (Publisher)   │
 └───────────────────┘        └──────────────────┘          └──────────────┘     └──────────────┘
 /dev/ttyACM0(Pixhawk) --MAVLink--> SendCtrlDownstream --> 手机
 手机 --SendCtrlUpstream--> SetOnControlData --> 写回 /dev/ttyACM0
```

**串口热插拔**：Pixhawk 未插入时照常推视频；插入后约 1s 自动挂接；拔出自动摘除并等待重插。

## 依赖与构建

构建机（Pi4）要求 **Raspberry Pi OS Bookworm 及以后**：

```bash
sudo apt install -y libcamera-dev libcamera-tools cmake g++ pkg-config
```

在 tj_rtc 仓库内（有 libcamera 时顶层构建会自动带上本程序）：

```bash
cmake -S demos/pi4_air -B demos/pi4_air/build && cmake --build demos/pi4_air/build -j4
```

> 采集/编码代码直接复用 `../raspberry_pi_pusher/src`（DmaHeap / LibcameraVideoSource /
  H264V4l2Encoder / NaluSplitter），不在本目录重复维护。
> 需可访问 `/dev/video11`（Pi 4/5 的 V4L2 H.264 编码节点）与 `/dev/dma_heap/vidbuf_cached`。

## 运行

```bash
cd demos/pi4_air/build
./pi4_air --stream openhd0 --user pi4_air
```

`--help` 查看全部参数。默认值：

| 参数 | 默认 | 说明 |
|---|---|---|
| `--width/--height/--fps` | 1280x720@30 | libcamera 实际模式由硬件协商（ov5647 实测可用） |
| `--bitrate` | 2500 (kbps) | H.264 编码码率 |
| `--gop` | 30 | GOP≈1s；订阅端 PLI 只计数，等下一个 IDR（同 local air 行为） |
| `--server/--port` | 106.15.177.248:9200 | SFU（TCP 信令 / UDP 媒体） |
| `--stream/--user` | openhd0 / pi4_air | **openhd0 与手机订阅一致** |
| `--bandwidth` | 20 (Mbps) | UDP Pacer 带宽 |
| `--serial/--baud` | /dev/ttyACM0 @57600 | Pixhawk USB CDC（波特率被忽略）；热插拔 |
| `--no-upstream` | off | 只记录手机上行 ctrl，不写串口（保守测试） |

`Ctrl+C` 优雅停止；`--seconds N` 可限定运行时长。

## 部署到 Pi4（当前实测路径）

开发机 rsync（pi4 = 192.168.50.114，用户 pi）：

```bash
cd <tj_rtc 仓库根>
for d in client_sdk common libs; do rsync -rc --exclude 'build/' $d/ pi4:~/tj_rtc/$d/; done
rsync -rc --exclude 'build/' demos/raspberry_pi_pusher/ pi4:~/tj_rtc/demos/raspberry_pi_pusher/
rsync -rc demos/pi4_air/ pi4:~/tj_rtc/demos/pi4_air/
```

Pi 上构建（已验证通过）：

```bash
ssh pi4
cd ~/tj_rtc/demos/pi4_air
cmake -S . -B build && cmake --build build -j4
./build/pi4_air --help
```

可选 systemd 常驻（自启）：

```ini
# /etc/systemd/system/pi4_air.service
[Unit]
Description=pi4_air RTC air link
After=network-online.target
[Service]
User=pi
WorkingDirectory=/home/pi/tj_rtc/demos/pi4_air/build
ExecStart=/home/pi/tj_rtc/demos/pi4_air/build/pi4_air --stream openhd0 --user pi4_air
Restart=always
[Install]
WantedBy=multi-user.target
```

## 注意事项

1. **与本地 `rtc_local_air` 不能同时运行**——同一 stream（openhd0）重复注册会互相顶掉。
   切换时先停一端。旧 `raspberry_pi_pusher`（stream=pivideo）与本程序可并存，但无 MAVLink 桥。
2. 订阅端中途加入：等编码器下一个 IDR（GOP=30 → ≤1s），收到 PLI 仅打印计数。
3. Pixhawk 未插、串口不存在都不影响视频；上行 ctrl 在串口就绪前会丢弃并计数（`up_drop`）。
4. 1s 周期统计行含：`enc/aus/key/kbps/drop`（视频）、`mav_tx/mav_rx/up_write/up_drop/resync`
   （MAVLink）、`pli`、`serial=OK|无`。

## 排错

- `相机启动失败`：`rpicam-hello --list-cameras` 确认相机被 libcamera 识别；CSI 排线方向。
- `编码器启动失败 (/dev/video11)`：确认非精简镜像；`ls /dev/video11 /dev/dma_heap/vidbuf_cached`。
- 手机看不到流：确认 stream 名（默认 openhd0）与手机端订阅一致、SFU 可达。
- 串口写不进去：`ls -l /dev/ttyACM0` 确认存在；用户需在 `dialout` 组（pi 默认满足）。