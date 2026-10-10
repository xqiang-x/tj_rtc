# OpenHD 集成 tj_rtc 传输链路（RtcLink）详细设计

| 项 | 内容 |
|---|---|
| 版本 | v1.0（评审稿） |
| 日期 | 2026-09-21 |
| 范围 | OpenHD air/ground 增加一条基于 tj_rtc SDK 的独立链路（RtcLink） |
| v1 编码范围 | H.264 先行（H.265 结构同构，v2 补） |
| 传输模式 | SFU 中转（公网 VPS），supervised UDP 模式（FEC/NACK/自适应码率） |
| 不改动模块 | `ohd_video`、`ohd_telemetry`（零改动） |

---

## 1. 背景与目标

OpenHD 当前支持三种链路：WFB（monitor 模式 wifi 广播）、Ethernet、Microhard，通过业务无关的
`OHDLink` 抽象（`apps/OpenHD/OpenHD/ohd_common/inc/openhd_link.hpp:54`）对上层提供统一的
"视频单向 + 遥测/RC 双向 + 音频单向" 契约。

tj_rtc 是以 SFU 为中心的音视频传输 SDK（Publisher/Subscriber + 控制信令通道），适合在
**无 WFB 网卡、需借道 4G/5G/公网** 的场景下作为 OpenHD 的替代链路。

目标：新增 `RtcLink : public OHDLink`，作为**与 RTP(WFB) 传输并列的独立传输方式**——
air 端把编码输出以"完整访问单元（AU）"推给 Publisher，ground 端把收到的 AU 重新打成
RTP 转发给 QOpenHD。tj_rtc 侧不感知 RTP，RTP 侧不感知 tj_rtc。

### 1.1 部署拓扑

```
┌─────────────┐   编码器输出      ┌───────────────┐   tj_rtc(UDP+FEC)  ┌──────────────────┐
│ OpenHD air  │ ──RTP分片──▶ RtcLink.air  ──▶  Publisher  ──────────▶ │ VPS SFU          │
│ (cam→H.264) │                (RTP→AU)                               │ 106.15.177.248   │
└─────────────┘                                                       │ tcp:9200 udp:9201│
                                                                      │ stun:9202        │
┌─────────────┐   RTP 5600/5800   ┌───────────────┐   tj_rtc(UDP+FEC) │                  │
│  QOpenHD    │ ◀──────────────── │ RtcLink.gnd   │ ◀─────────────── │                  │
│ (ground pi4)│                   │ (AU→RTP)      │   Subscriber     │                  │
└─────────────┘                   └───────────────┘                  └──────────────────┘
                                        ▲ 遥测/RC：SendCtrlUpstream/Downstream + OnControlData
```

### 1.2 通道映射总表

| OHDLink 契约 | RtcLink 实现（air） | RtcLink 实现（ground） |
|---|---|---|
| `transmit_video_data(stream_index, FragmentedVideoFrame)` | `RtpEsAssembler`：RTP→AU → `Publisher::PushVideoFrame` | —（ground 不发送视频） |
| `on_receive_video_data(idx, data, len)` | — | `EsRtpPacketizer`：AU→RTP，逐包回调 |
| `transmit_telemetry_data(packet)` | `Publisher::SendCtrlDownstream`（RC→reliable，遥测→unreliable） | `Subscriber::SendCtrlUpstream`（同上） |
| `on_receive_telemetry_data` | `Publisher::SetOnControlData` | `Subscriber::SetOnControlData` |
| `transmit_audio_data(packet)` | `Publisher::PushAudioFrame`（RTP PCMA 包不透明透传） | — |
| `m_audio_data_rx_cb` | — | FrameType=0x10 分支 → `on_receive_audio_data` |
| PLI（`SetOnKeyFrameRequest`） | v1：仅记录日志（依赖 OpenHD 周期关键帧） | 由 SFU 自动触发（新订阅者/丢包） |

---

## 2. 范围

### 2.1 v1 目标

1. `RtcLink` 支持 air（Publisher）/ ground（Subscriber）两种角色，由 `OHDProfile` 自动决定。
2. 视频（H.264）、遥测/RC、音频三通道全通；单摄像头优先，双摄按 streamId 扩展（cam0/cam1）。
3. `/boot/rtc.txt` 配置启用（仿 `ethernet.txt` 模式），启用后优先级高于 WB/Microhard。
4. ground 端订阅看护（air 后上线/断线重连场景）。
5. 使用 VPS SFU（106.15.177.248:9200/9201）完成 L2–L4 测试。

### 2.2 非目标（v2+）

- H.265、MJPEG；P2P 直连（P2PManager/STUN）；载荷加密；WFB+RTC 双链路并发聚合；
  `OnBitrateAdvice` 反向驱动 OpenHD 编码器码率（v2 高价值项）；GetStats 上报 QOpenHD。

### 2.3 硬性约束

- **OpenHD 侧视频编码必须为 H.264**（v1）。Air 端装配器对非法 NALU 模式做弱校验告警。
- tj_rtc 使用 **UDP 模式**（`Start(..., useUdp=true)`）：视频走 FEC/UDP + 自适应码率，
  可靠信令走独立 TCP 连接，避免遥测被视频的 TCP 队头阻塞。TCP 模式仅供调试。

---

## 3. 关键事实与既有约定（实现依据）

| # | 事实 | 依据 |
|---|---|---|
| F1 | `OHDLink` 三通道契约稳定；视频 air 只调 `transmit_video_data`，ground 只注册回调 | `ohd_link.hpp:54`；`ohd_video_air.cpp:291`；`ohd_video_ground.cpp:48,87-97` |
| F2 | `EthernetLink` 是最小实现模板（~158 行），含 `ethernet.txt` 配置模式 | `ohd_interface/src/ethernet_link.cpp` |
| F3 | 链路装配点：ctor 短路创建 + `get_link_handle()` + `has_primary_link()` | `ohd_interface/src/ohd_interface.cpp:77-118, 557-570, 571` |
| F4 | air 的 `FragmentedVideoFrame.rtp_fragments` = RTP 包（含 12B 头），边界由 FU-E 启发式判定，**对单 NALU 包不返回 fu_end（边界是松散的，可能跨帧/含多帧）** | `rtp_eof_helper.cpp:98-107`；`gstreamerstream.cpp:645-682` |
| F5 | tj_rtc 链路帧约定：**一次 PushVideoFrame = 一个完整访问单元（Annex-B），IDR 帧内嵌 SPS/PPS，SEI/AUD 作边界丢弃** | `apps/demos/raspberry_pi_pusher/src/NaluSplitter.cpp:64-100`；`apps/demos/android_rtc_app/.../RtcClient.java:489-491` |
| F6 | 订阅端 `OnFrame` 的 payload 首字节 = FrameType（0x01 PARAMS /0x02 IDR /0x03 P /0x10 AUDIO），其后为裸码流 | `Subscriber.cpp:140-171` |
| F7 | ground 视频数据逐包转发到 `127.0.0.1:5600/5601`（+5800/5801、外部设备），QOpenHD 无感知 | `ohd_video_ground.cpp:39-46, 87-97` |
| F8 | 音频 = RTP PCMA（alaw, 8kHz）包 | `gstaudiostream.cpp:110-145, 217-225` |
| F9 | SFU 在新订阅者/丢包时向上游转发 PLI（500ms 节流）→ air 端 `SetOnKeyFrameRequest` | `SfuServer.cpp:919-924, 1059-1073` |
| F10 | ground 订阅失败（流不存在）会直接退出网络线程、置 `m_running=false`；断连则线程退出但 `m_running` 仍为 true | `Subscriber.cpp:513-519`（TCP/UDP 路径同构） |
| F11 | Publisher 自带断线检测与重连（1s 间隔），UDP 模式有 PONG/失败计数兜底 | `Publisher.h:174-192` |
| F12 | FEC 按 256 块/组自动切分多组，**大帧（数百 KB IDR）无硬上限** | `rtc/libs/frame_protoc/src/types.h:14`；`protocol.h:17-18` |
| F13 | SDK 仅依赖 Threads + 内嵌 cm256cc；aarch64 自动 NEON 分支 | `frame_protoc/CMakeLists.txt:30-38` |
| F14 | OpenHD 与 tj_rtc 同为 C++17；tj_rtc 为 Apache-2.0（与 GPLv3 兼容） | 各 CMakeLists；LICENSE |

---

## 4. 详细设计

### 4.1 视频 air 方向：RTP → Annex-B AU（RtpEsAssembler）

**入口**：`RtcLink::transmit_video_data(idx, frame)`，仅使用 `frame.rtp_fragments`，**不使用**
`FragmentedVideoFrame` 的帧边界（F4），改为在包级自行成帧。

**处理算法（单线程：gst appsink 消费线程）**：

```
每收到一个 RTP 包 pkt：
  1. 解析 RTP 头：V/P/X/CC、marker、seq、timestamp；跳过 4*CC 与扩展头（4+4*ext_len）
  2. 取 payload；若 timestamp != cur_ts 且当前 AU 非空 → FinalizeAu()（兜底成帧）
  3. 按 H.264 RTP 载荷类型处理：
     - 单 NALU（1..23）：EmitNalu(payload)
     - FU-A（28）：
         S=1 → 若已有未完成 FU 则丢弃（容错并告警）；新建 nalu = [(ind&E0)|(fu&1F)] + body
         S=0 → 追加 body；E=1 → EmitNalu(nalu)
         无进行中 FU 且 S=0 → 丢弃并告警（失步）
     - STAP-A（24）：按 16bit 大端长度逐个 EmitNalu（防御性，gstreamer 默认不产出）
     - 其余（29/30/31）：丢弃并告警
  4. 若 marker=1 → FinalizeAu()

EmitNalu(nalu)：按 H.264 NALU 类型分流
  - 1..5（VCL） → cur_au.vcl 追加；记录 has_idr = (type==5)
  - 7/8（SPS/PPS） → cur_au.params 追加
  - 6/9/12（SEI/AUD/Filler） → 丢弃（边界）
  - 其他 → 丢弃

FinalizeAu()：
  - 若 cur_au.vcl 非空：
      out = annexb_pack(cur_au.params + cur_au.vcl)     # 00 00 00 01 拼接
      Publisher::PushVideoFrame(out, len, isParamSet=false, isKeyFrame=cur_au.has_idr)
  - 否则若 cur_au.params 非空（纯参数集 AU）：
      Publisher::PushVideoFrame(params, len, isParamSet=true, isKeyFrame=false)
  - 清空 cur_au

安全兜底：
  - AU 超时 500ms 无 marker/ts 变化 → 强制 FinalizeAu（防丢包卡死）
  - AU 累积 > 4MB → 丢弃 + 告警（防爆内存）
  - ts 变化成帧优先于 OpenHD 边界：与 F4 松边界天然兼容
```

**与既有约定的对齐**：输出与 `NaluSplitter`（F5）语义一致——参数集与 VCL 同属一个 AU 时合并
（gstreamer 的 SPS/PPS 与 IDR 通常同一 timestamp，天然落入同一 AU）；纯参数集 AU 单独以
`isParamSet=true` 抛出；SEI/AUD 丢弃。

**输入侧的可靠性假设**：air 端的 RTP 包来自本机 gstreamer appsink（进程内），无丢包/乱序，
因此装配器不做序号缺口容错，仅做超时兜底。

**H.265 检测（弱校验）**：若连续 N 包解析出的 NALU 类型非法（>23 且非 24/28），
输出一次告警"疑似 H.265/非 H.264 流，v1 不支持"并停止推流该路视频（保留遥测）。

### 4.2 视频 ground 方向：Annex-B AU → RTP（EsRtpPacketizer）

**入口**：`Subscriber::SetOnFrame`（SDK 网络线程回调）。

```
OnFrame(payload)：
  type = payload[0]; data = payload[1..]
  if type == 0x10 → on_receive_audio_data(data, len); return      # 音频
  if type ∉ (0x01,0x02,0x03) → 忽略
  nalus = SplitAnnexB(data)                     # 3/4 字节起始码，保留 1..5/7/8，防御性过滤
  ts = NextTs90k()                              # 见下
  for i, nalu in enumerate(nalus):
      last = (i == len(nalus)-1)
      if len(nalu) <= 1428:                     # 1440 - 12(RTP)
          SendPacket(SingleNalu(nalu), marker=last)
      else:
          FU-A 分片（每包 body ≤ 1426，2 字节 FU 头），末片 marker=last
  每个 RTP 包 → on_receive_video_data(stream_index, pkt, len)     # 逐包转发，等价 WFB 行为

RTP 头约定：V=2、P/X/CC=0、PT=96、SSRC 固定(0x4F504844)、seq 每流 uint16 递增
NextTs90k：base + (steady_clock_ns * 9) / 100_000                # 单调 90kHz，抗抖动
FU-A：indicator=(nal[0]&0xE0)|28；fu_header=(nal[0]&0x1F)|(S<<7)|(E<<6)
```

**转发链路**：`on_receive_video_data` → `OHDVideoGround::on_video_data`（`stream_index` 分流）
→ `UDPMultiForwarder` 同步 sendto 至 5600/5800/外部设备（F7）。QOpenHD 收到的字节流与
WFB 路径完全同类（RTP/H.264/PT96/MTU1440），预期零适配。

**AUD 插入**：v1 不插入；若 L2 实测 QOpenHD 的 `h264parse` 需要 AU 对齐信号，
再补 9 号 NALU（预留开关）。

**订阅标志**：`Subscriber::DoSubscribe` 自动以 `audio/video/data = true` 订阅
（`Subscriber.cpp:689-694`），RtcLink 无需额外声明媒体类型。

### 4.3 遥测 / RC（MAVLink）

MAVLink（含 RC）全部走 tj_rtc **信令通道（ctrl）**，与 FEC 媒体通道互不干扰；方向由角色决定
（air=Publisher → `SendCtrlDownstream`；ground=Subscriber → `SendCtrlUpstream`）。

**数据流（air→ground 遥测；ground→air 完全对称）**：

```
air:  OHDTelemetry → WBEndpoint（mavlink 聚合，≤1024B/包）→ transmit_telemetry_data
      → RtcLink → Publisher::SendCtrlDownstream(data, len, reliable)
      → SFU HandleCtrlData：direction=0x00 广播给该流所有订阅者
      → ground Subscriber::DispatchControlMessage → SetOnControlData（回调）
      → RtcLink → on_receive_telemetry_data → OHDTelemetry(MEndpoint::parseNewData)
ground→air：SendCtrlUpstream（direction=0x01，定向到 publisher session）
```

**线上格式与尺寸（已核实）**：
- 客户端↔SFU：`[ctrlType][direction][session(4)][payload]` = 6B 头；UDP 路径再加 1B SFU header。
  单包 = 7B + payload；OpenHD 聚合上限 1024B（`aggregate_pack_messages` 默认 max_mtu）
  → 最大 1031B < MTU 1500，**无 IP 分片**；TCP 路径帧上限 1MB（SfuProtocol.h:48）。
- downstream 为**广播**：同一流的多个订阅者同时收遥测（pi4 的 OpenHD ground 与 Android App
  可并存，多 GCS 天然支持）；一个流名只允许一个 publisher → 上行 RC 目标无歧义。
- 收发缓冲区与路由由 SFU 完成（`HandleCtrlData`，SfuServer.cpp:1994），无速率限制。

**可靠性决策（v1 默认：双向 unreliable + 兑现 n_injections）**：

| 方向 | 默认 | 发送行为 |
|---|---|---|
| air→ground 遥测 | `reliable=false`（UDP） | 按 `n_injections`（1/2/4）复制发送 |
| ground→air RC | `reliable=false`（UDP） | 同上 |
| 配置翻转 | `rc_reliable=1` / `telemetry_reliable=1` | 走 TCP，只发一次（TCP 重传兜底） |

理由：
1. **与 OpenHD 既有语义一致**：WFB 链路就是 `for (n_injections)` 重复发送
   （WBStreamTx.cpp:321，GroundTelemetry 对 RC 推荐 4 次注入）——复制是 OpenHD RF 链路的
   既定可靠性手段，行为对齐后调参经验可迁移。
2. **避开 TCP 的坑**：ctrl TCP 是共享单流，存在 HoL 阻塞；且连接断开后的静默失效对 RC 而言
   比丢包更糟。RC 是 20~50Hz 连续 setpoint 流，单包丢失无害；复制 ×2~4 后独立丢包率降为 pⁿ。
3. UDP ctrl 与媒体共用主 socket（订阅保活/心跳同路径），断连检测由已有机制覆盖。

发送实现（RtcLink::transmit_telemetry_data 内）：

```cpp
const int n = reliable ? 1 : std::max(1, packet.n_injections);  // 实际取值 1/2/4
for (int i = 0; i < n; ++i) { /* SendCtrlDownstream / SendCtrlUpstream */ }
```

**上游窗口时序（重要前提）**：`Subscriber::SendCtrlUpstream` 在 `m_sourceSessionId==0` 时静默
丢弃（Subscriber.cpp:179）。该值原仅在收到**首个媒体帧**后才被设置（Subscriber.cpp:441/545）
→ 若 air 未推视频（或视频尚未到达），ground 的 RC/上行遥测将永远发不出。
→ 需 SDK 最小补丁 **P1**（见 7.4）：收到下行信令时同步记录 sourceSessionId，
使首条遥测信令即打开上游窗口（air 启动即发遥测，不再依赖视频先达）。

**2026-09-23 公网实测注记（Android App 上行）**：对 live air（openhd0，session 3216）做 A/B —
UDP(unreliable) 上行共 27 条**全部丢失**（air `mav_rx` 不增）；同探针改 TCP(reliable) 后
27/27 全部送达（`mav_rx` 13→28）。隔离实验：对新建/复用的测试 publisher 会话，SFU 转发
UDP 上行正常（9/10、7/8）——唯 live air 会话 UDP 上行不达，根因未定位（SFU 仅 INFO 日志）。
结论：**QOpenHD Android App 上行 ctrl 固定 reliable=true**（RtcClient.cpp send_ctrl_upstream）；
SDK 同步加固：UDP 模式下 reliable 发送在 ctrl TCP 未就绪时回退 UDP（不静默丢），
ctrl 接收线程退出置 `m_ctrlRunning=false`，由 1s 保活 tick 幂等 `EnsureCtrlTcp()` 重建自愈。
OpenHD ground 侧维持配置默认（unreliable+复制，L4 实测 ≥96.6% 交付）。

### 4.4 音频

- air：`transmit_audio_data(packet)` → `Publisher::PushAudioFrame(data, len)`
  载荷为 OpenHD 的 RTP PCMA 包（F8），**不透明透传**，不做 PCM 转换。
- ground：FrameType=0x10 → `on_receive_audio_data` → 5610（QOpenHD 正常解码）。
- 说明：与 SDK "AUDIO_PCM" 名义约定不符实（自家 App 拉 OpenHD 流时音频按 PCM 解释会异常），
  属已知取舍；OpenHD ground 路径完全正确。

### 4.5 关键帧请求（PLI）

- SFU 在新订阅者加入/下游丢包时已自动向上游发 PLI（F9），air 端 `SetOnKeyFrameRequest` 触发。
- v1：仅计数+日志（OpenHD 无 force-IDR 接口），依赖 `h26x_keyframe_interval` 周期关键帧恢复。
- v2：桥接到编码器（对 gst 编码器元素发 `GstForceKeyUnit` 事件，改动点在 `GStreamerStream`，
  与 `m_bitrate_ctrl_element` 同处）。

### 4.6 连接生命周期与看护

**air（Publisher）**：`Start()` 后 SDK 内部自愈（F11），RtcLink 仅注册 `OnConnected/
OnDisconnected` 用于日志与 `openhd::Reporter` 状态上报（可选）。

**ground（Subscriber）看护状态机**（独立 supervisor 线程，1s tick）：

```
状态：Idle → Starting → Streaming → Backoff → Starting ...
触发重启（置 needs_restart）：
  - SetOnSubscribeFailed（流不存在/被拒，F10：线程退出）
  - SetOnDisconnected（对端/SFU 断链）
  - IsRunning()==false 但期望运行
重启动作：sub->Stop()（join 网络线程）→ sleep(2000ms) → 重建 Subscriber → Start()
注意：回调运行于 SDK 网络线程内，只允许置原子标志；Stop()/重建仅在 supervisor 线程执行。
```

---

## 5. 接口设计（头文件草案）

### 5.1 类图

```
RtcLink : public OHDLink
 ├── RtcConfig                    # rtc.txt 解析
 ├── std::array<StreamState,2>    # cam0/cam1
 │     ├── air:   push::Publisher + RtpEsAssembler
 │     └── gnd:   pull::Subscriber + EsRtpPacketizer
 └── StreamSupervisor（仅 ground）  # 订阅重试/重建
```

### 5.2 RtcLink

```cpp
// ohd_interface/inc/rtc_link.h
class RtcLink : public OHDLink {
 public:
  explicit RtcLink(OHDProfile profile);
  ~RtcLink() override;

  void transmit_telemetry_data(TelemetryTxPacket packet) override;
  void transmit_video_data(int stream_index,
                           const openhd::FragmentedVideoFrame& frame) override;
  void transmit_audio_data(const openhd::AudioPacket& audio_packet) override;

 private:
  struct StreamState {
    int index = 0;
    std::unique_ptr<push::Publisher>  publisher;    // air
    std::unique_ptr<pull::Subscriber> subscriber;   // ground
    std::unique_ptr<RtpEsAssembler>   assembler;    // air
    std::unique_ptr<EsRtpPacketizer>  packetizer;   // ground
  };
  RtcConfig m_config;
  OHDProfile m_profile;
  std::array<StreamState, 2> m_streams;
  std::shared_ptr<spdlog::logger> m_console;

  void init_air_unit();
  void init_ground_unit();
  void on_control_data(int stream_index, const uint8_t* data, size_t len);
};
```

### 5.3 RtpEsAssembler（air，单线程）

```cpp
class RtpEsAssembler {
 public:
  using OutCb = std::function<void(std::vector<uint8_t>&& au, bool is_param, bool is_key)>;
  RtpEsAssembler(bool is_h265, OutCb cb);
  // 处理一个 RTP 包（含 12B RTP 头）
  void on_rtp_packet(const uint8_t* pkt, size_t len);
  // 由 RtcLink 在帧边界/超时时调用，强制收尾当前 AU
  void flush(uint64_t now_us);

 private:
  void emit_nalu(const uint8_t* nalu, size_t len);
  void finalize_au();
  struct Nalu { std::vector<uint8_t> data; };
  std::vector<std::vector<uint8_t>> m_params, m_vcl;
  bool m_has_idr = false, m_fu_in_progress = false;
  uint32_t m_cur_ts = 0; bool m_au_open = false;
  uint64_t m_au_start_us = 0; size_t m_au_bytes = 0;
  // 常量：kAuTimeoutUs=500ms、kMaxAuBytes=4MB、kMaxNaluTypes 弱校验计数
};
```

### 5.4 EsRtpPacketizer（ground）

```cpp
class EsRtpPacketizer {
 public:
  using SendCb = std::function<void(const uint8_t* rtp, size_t len)>;
  EsRtpPacketizer(bool is_h265, int mtu, SendCb cb);
  // 输入帧类型 0x01/0x02/0x03 + Annex-B AU
  void on_au(uint8_t frame_type, const uint8_t* data, size_t len);

 private:
  uint16_t m_seq = 0; uint32_t m_ssrc = 0x4F504844;
  std::vector<uint8_t> m_scratch;   // 复用缓冲（SendCb 同步发送，安全）
  // ts = base + steady_ns*9/100000；FU-A 分片；marker 语义见 4.2
};
```

### 5.5 RtcConfig

```cpp
struct RtcConfig {
  std::string server_ip;
  int  tcp_port = 9200;
  int  udp_port = 0;          // 0 → tcp_port+1
  bool use_udp = true;
  std::string stream_prefix = "openhd";   // streamId = prefix + index
  std::string user_id_air = "ohd_air", user_id_ground = "ohd_gnd";
  uint64_t pacer_mbps = 50;
  bool enable_cam[2] = {true, false};
  bool rc_reliable = false;           // §4.3：默认 UDP + n_injections 复制
  bool telemetry_reliable = false;    // 同上
  static RtcConfig load();                          // 解析 getConfigBasePath()+"rtc.txt"
  static RtcConfig load_from_file(const std::string& path);  // 供冒烟/单测指定路径
  bool is_valid() const;              // server_ip 非空
  int  effective_udp_port() const;    // udp_port>0 ? udp_port : tcp_port+1
};
```

---

## 6. 配置设计（/Config/rtc.txt）

存在即启用 RtcLink（仿 `ethernet.txt`，见 F2/F3）；缺失则完全不影响现有行为。

> 路径说明（M2 实测）：OpenHD 的 `getConfigBasePath()` 当前硬编码返回 `"/Config/"`
> （`setConfigBasePath` 无效），因此设备上文件固定为 **`/Config/rtc.txt`**，与
> `ethernet.txt` 同目录。（文档旧稿写作 /boot/rtc.txt，以本节为准。）

| 键 | 默认 | 说明 |
|---|---|---|
| `server_ip` | （必填） | SFU 地址，如 `106.15.177.248` |
| `tcp_port` | 9200 | 信令端口 |
| `udp_port` | 0 | 0 → tcp_port+1（9201） |
| `use_udp` | 1 | 1=UDP 视频+FEC（推荐），0=TCP（调试） |
| `stream_prefix` | openhd | 流名 = prefix + 相机 index（openhd0/openhd1） |
| `user_id_air` / `user_id_ground` | ohd_air / ohd_gnd | 会话标识 |
| `pacer_mbps` | 50 | UDP 模式发送带宽上限 |
| `enable_cam0` / `enable_cam1` | 1 / 0 | 各相机是否走 RTC 链路 |
| `rc_reliable` | 0 | 0=UDP+按 n_injections 复制发送（默认，§4.3），1=可靠信令 TCP 单发 |
| `telemetry_reliable` | 0 | 同上，遥测通道 |

解析：简单 `key=value`，`#` 注释，去空白；错误值回退默认并告警。

---

## 7. 集成与构建

### 7.1 OHDInterface 改动（3 处，精确行为）

```cpp
// 1) ctor（ohd_interface.cpp，紧随 ethernet.txt 检查之后，同风格短路）
if (OHDFilesystemUtil::exists(std::string(getConfigBasePath()) + "rtc.txt")) {
  m_rtc_link = std::make_shared<RtcLink>(m_profile);
  m_console->warn("rtc found");           // 对齐 ethernet 的 "ethernet found"
  return;
}
// 2) get_link_handle(): 在 m_ethernet_link 分支后插入 m_rtc_link 分支
//    m_console->warn("Using alternative Link: RTC(tj_rtc)"); return m_rtc_link;
// 3) has_primary_link(): 追加 || m_rtc_link
// ohd_interface.h: 成员 std::shared_ptr<RtcLink> m_rtc_link;（声明不随宏变化，保证类布局一致）
```

实现注记（M2 定稿）：
- 包含与构造块整体包在 `#ifdef OHD_HAS_RTC_LINK` 内（CMake 由 SDK 是否 vendor 决定是否
  define）；`ohd_interface.h` 的成员声明与 `get_link_handle()`/`has_primary_link()` 分支
  **无条件**存在（前者防类布局不一致，后者对空指针天然短路，零成本）。
- 已验证：`sizeof(OHDInterface)` 在 define 开/关两变体下相同（576B）。

优先级：`ethernet.txt > rtc.txt > microhard > wb`（前两者为显式配置，短路互斥）。

### 7.2 源码 vendor 与 CMake

需要 vendor 的目录（不含 server/demos，SDK 仅依赖这些）。**源为仓库 `rtc/` 目录**
（即「SDK 根 = `rtc/`」：`rtc/{common,libs,client_sdk}` 的对应子树），vendor 目标布局：

```
ohd_interface/lib/tj_rtc/
├── common/sfu_protocol/            # header-only INTERFACE 库
├── libs/frame_protoc/              # 含 cm256cc（内嵌源码）
├── libs/pacer/
├── libs/mem_pool/
└── client_sdk/rtc_client_sdk/      # Publisher/Subscriber/P2PManager
```

`ohd_interface/CMakeLists.txt`：

```cmake
set(TJ_RTC_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/tj_rtc" CACHE PATH "tj_rtc SDK root")
add_subdirectory("${TJ_RTC_SDK_DIR}/client_sdk/rtc_client_sdk"
                 ${CMAKE_BINARY_DIR}/_deps/rtc_client_sdk)
target_link_libraries(OHDInterfaceLib PRIVATE rtc_client_sdk)
```

- SDK 的 CMake 自带守卫式依赖引入（`sfu_protocol/frame_protoc/pacer/mem_pool` 不存在则
  自动 add_subdirectory），且 `BUILD_DEMOS` 在非 standalone 时默认 OFF；
  `TJ_RTC_ROOT` 按 `client_sdk/rtc_client_sdk/../..` 解析 → vendor 布局必须保持上表结构
  （该约定使 SDK 根 = `rtc/`，开发期直接指向仓库 `rtc/` 即可）。
- 开发期：`-DTJ_RTC_SDK_DIR=/media/ubuntu/linux/tj_rtc/rtc` 直接指向仓库 `rtc/`，免拷贝；
  发布期：subtree/submodule 固定版本到 `lib/tj_rtc`。
- 交叉编译：frame_protoc 已按 `CMAKE_SYSTEM_PROCESSOR` 自动选择 `USE_NEON`（F13），
  无额外依赖；仅需 Threads（OpenHD 已链接）。

### 7.3 与上行编译选项的兼容

OpenHD 与 SDK 同为 C++17；SDK 目标自带 `-Wall`（PRIVATE），不污染 OpenHD 编译选项。
`NO_RESTRICT`/SIMD 宏为 PUBLIC 传播，仅影响链接目标自身。

### 7.4 对 SDK 的最小补丁（vendor 内生效）

| # | 位置 | 改动 | 目的 |
|---|---|---|---|
| P1 | `Subscriber::DispatchControlMessage`（Subscriber.cpp:224-242） | 解析出 `sourceSessionId != 0` 时同步 `m_sourceSessionId = sourceSessionId` | air 无视频流时 ground 上行（RC/遥测）仍可发出；上游窗口在首条下行信令即打开（F：原需等首个媒体帧） |

补丁原则：单行、值语义与既有 NACK/STATS 路由一致（同一 publisher session），无协议改动。
vendor 化后由 `lib/tj_rtc` 内的补丁分支维护，随版本升级需复核。

---

## 8. 线程模型与性能

| 路径 | 执行线程 | 备注 |
|---|---|---|
| air 视频：assembler → PushVideoFrame | gst appsink 消费线程 | 单线程访问，无锁；PushVideoFrame 内部入队（mutex+cv） |
| air 视频发送 | SDK 网络线程 | FEC 分片 + Pacer（pacer_mbps）+ 自适应码率 |
| air 遥测发送 | OpenHD 遥测线程 | SendCtrlDownstream 内部加锁，短临界区 |
| ground 视频：OnFrame → packetizer → sendto | SDK 网络线程 | 复用 scratch 缓冲；sendto 同步，等效 WFB 路径 |
| ground 看护 | supervisor 线程（1s tick） | 仅做状态检查与重建 |

拷贝开销：air 每 AU 一次入队拷贝（~几十 KB，可忽略）；ground 每包一次 sendto（与既有链路同量级）。
Publisher 队列有界（`kMaxVideoQueueSize=60`，满时丢"最近关键帧之前"的帧保 IDR 可解，
Publisher.cpp:112-127）；L4 观察实际深度确认参数是否需要调整。

---

## 9. 异常与边界处理清单

| 场景 | 行为 |
|---|---|
| SFU 不可达 / 断链 | air：Publisher 内部重连（F11）；ground：supervisor 重建 Subscriber |
| 订阅被拒（流不存在，air 未上线） | `OnSubscribeFailed` → 2s 退避重试，Reporter 告警 |
| PLI 无法强制 IDR | v1 记录；依赖周期关键帧（≤ keyframe interval 恢复） |
| RTP 时间戳回绕 | 以 marker 为主、ts 变化为兜底；AU 500ms 超时强制收尾 |
| 单 NALU 小帧（P/参数集） | ts 分组自然成帧，不依赖 OpenHD 松边界（F4） |
| 大帧（数百 KB IDR） | FEC 多组自动切分（F12）；AU>4MB 丢弃告警 |
| H.265/非 H.264 流 | 弱校验连续异常 → 告警并停推视频（遥测保留） |
| MTU | ground 打包上限 1440（与 WFB 一致）；tj_rtc 内部再按 1450 分片 |
| 双摄 | 两套 StreamState 独立 streamId；`enable_camN` 控制 |
| SDK 线程回调中调 Stop() | 禁止；回调只置原子标志，操作由 supervisor 执行 |
| ground 上游窗口（sourceSessionId==0） | 补丁 P1 后首条下行信令即开窗；air 完全无遥测时才持续丢弃（RC 本就无意义） |
| 音频名义约定（PCM vs PCMA） | 文档化取舍（4.4），OpenHD 路径无影响 |

---

## 10. 测试计划（分层验证：先抽件、本机预演、后进 OpenHD）

原则：新代码中只有两个纯函数组件（`RtpEsAssembler` / `EsRtpPacketizer`）不依赖 SDK 与
OpenHD 主构建——**先抽出来在本机独立验证（L0），再做本机全链仿真（L1），最后才进
pi4/OpenHD/App（L2–L4）**。每级失败时排查范围限定于"本级胶水层"，避免全链联调。

| 级 | 抽出的部分 | 环境 | 内容 | 通过标准 | 估时 |
|---|---|---|---|---|---|
| **L0** 离线往返 | Assembler + Packetizer（纯代码） | 开发机，无网络 | 合成样本 + gst 真实回环夹具：RTP→AU→RTP→AU 逐字节比对；AU 落盘 ffprobe 校验 | 逐字节一致；ffprobe 帧数/分辨率正确 | 0.5d |
| **L1** 本机全链仿真 | 组件 + SDK 两个探针工具 | 开发机 + VPS | gst 源 → air 探针(assembler+Publisher) → VPS → ground 探针(Subscriber+packetizer) → gst 标准解码出画；两端 AU 落盘互比 | 连续出画；AU 与源逐字节一致 | 0.5d |
| **L2** ground 半程实机 | SDK + Packetizer | pi4 + VPS | 独立程序订阅现有 `pivideo` → packetizer → 5600；QOpenHD 出画 | 与 WFB 源画面一致、无花屏 | 0.5d |
| **L3** air 全 OpenHD | OpenHD air + rtc.txt | pi4 + VPS | OpenHD 视频走 RTC → VPS → App/mac_pull_test | 集成正确；帧率/码率正常 | 0.5d |
| **L4** 端到端 | 双端 OpenHD | 双设备 + VPS | 全通道（视频/遥测/RC/音频）+ 弱网（tc netem 丢包/限速） | §10.4 验收标准 | 0.5–1d |

> 开发机工具已就绪（gst-launch-1.0 + rtph264pay/x264enc/openh264enc + ffprobe + g++），
> L0/L1 无需任何硬件。现有 demos 直接复用：`sim_pusher`（x264 合成流，`--dump_h264_path`
> 可落盘 AU 对照）、`pull_probe`（帧统计）、Android App。pi4 已推 `pivideo` → SFU/公网/App
> 链路本身是已知可用项，分层验证里可作为固定对照组。

### 10.1 L0：离线往返（第一步就做）

- 编译（不碰 OpenHD 主构建，一条命令）：
  ```bash
  g++ -std=c++17 -O2 -Wall -Wextra -I apps/OpenHD/OpenHD/ohd_interface/inc \
      apps/OpenHD/OpenHD/ohd_interface/src/rtp_es_assembler.cpp \
      apps/OpenHD/OpenHD/ohd_interface/src/es_rtp_packetizer.cpp \
      apps/OpenHD/OpenHD/ohd_interface/test/test_rtp_es.cpp -o es_selftest
  ```
  前提：组件仅用 std 类型 + `std::function` 回调，不 include openhd::log / OHDLink
  （日志经回调暴露）——这是"能抽出来"的硬约束，代码评审时检查。
- 用例：
  1. 合成：单 NALU / FU-A 起中末 / STAP-A / 多 slice / marker 缺失 / ts 切换 /
     SEI/AUD 过滤 / NALU 长度 <与>1428B / 纯参数集 AU / 4MB 上限；
  2. 真实回环夹具：`gst-launch-1.0 videotestsrc … ! x264enc ! rtph264pay ! udpsink`
     本地抓包回放（覆盖真实 payloader 的分片/marker 行为）——自测程序内置夹具模式：
     ```bash
     ./es_selftest --capture-udp <port> <seconds> <out.h264>
     ```
     与 gst 组合示例（源分叉：一路 RTP 给夹具，一路 `h264parse` 转 Annex-B 落盘对照）：
     ```bash
     gst-launch-1.0 -q videotestsrc num-buffers=90 is-live=true ! \
       video/x-raw,width=640,height=360,framerate=30/1 ! \
       x264enc tune=zerolatency key-int-max=30 speed-preset=ultrafast ! tee name=t \
       t. ! queue ! rtph264pay pt=96 config-interval=1 ! udpsink host=127.0.0.1 port=15999 \
       t. ! queue ! h264parse ! video/x-h264,stream-format=byte-stream ! filesink location=src.h264
     ```
  3. 现场夹具（可选后补）：pi4 `tcpdump -w` 现有 5600 口的 RTP，作回归集。
- 交叉校验（独立实现互验，不比单一自测可信）：
  - assembler 产出的 AU 序列 → `ffprobe`/`ffplay` 直接解码；
  - packetizer 产出的 RTP → 回喂 assembler + gst `rtph264depay` 双验。
- **验证记录（M0 已完成）**：81 项合成用例全过（0 失败）；gst 真实夹具 976 RTP 包 →
  90 AU / 3 关键帧（匹配 `key-int-max=30`），`ffprobe` 解码 90 帧 640×360，`framemd5`
  与源逐帧比对 **90/90 全同**。夹具附带发现：本机 `x264enc` 默认输出 AVCC（`byte-stream`
  默认 false，`rtph264pay` 自动识别并转 NALU——与生产拓扑一致）；`config-interval=1` 注入的
  SPS/PPS 与 IDR 同 ts，天然落入同一 AU（params+VCL 合并路径得到实测覆盖）。

### 10.2 L1：本机全链仿真（无 pi4、无 OpenHD 构建）

```
gst 源(rtph264pay → 127.0.0.1:5600) → rtc_air_probe(assembler+Publisher) ──VPS(106.15.177.248)──▶
rtc_gnd_probe(Subscriber+packetizer) → 127.0.0.1:5610 → gst rtph264depay ! 解码 ! 出画
```

- 两个探针工具放 `ohd_interface/test/rtc_probe/`（独立迷你 CMake，链接 rtc_client_sdk，
  **不进 OpenHD 主构建**）；与 gst 之间用裸 UDP socket 对接（CLI gst-launch 起源/收端，
  工具不引入 gst 头文件依赖）。构建：
  ```bash
  cmake -S apps/OpenHD/OpenHD/ohd_interface/test/rtc_probe -B /tmp/rtc_probe_build
  cmake --build /tmp/rtc_probe_build -j
  ```
- 两端 AU 落盘 → `cmp` 逐字节比对（源=端到端，充要验证）。
- 本级一并验证：VPS 中转、SDK 推/拉、FEC 丢包恢复（可 `tc netem` 注入）、关键帧请求链路。
- 探针里的视频路径代码即 RtcLink 最终代码（同一个 Assembler/Packetizer 实例化），**零重写**；
  仅遥测/RC 与看护逻辑在 M5 补齐。
- **验证记录（M1 已完成，VPS 106.15.177.248 实跑）**：600 帧 640×360@30fps，
  `rtp_pkts=6145 → aus=600（key=20，匹配 key-int-max=30）`；**air/gnd 两端 AU 落盘
  `cmp` 逐字节全同**（5,179,003B）；gst 收端 `rtph264depay` 解码 600 帧，
  `framemd5` 源 == air == 收端 **600/600 逐帧全同**；FEC `lost=0 dropped=0`（干净网络）；
  PLI 链路通（gnd 首帧后请求一次，air 探针 `pli=1`）。丢包恢复注入（`tc netem` 需动宿主机
  网卡配置）顺延到 L4 弱网项一并做。
- 运行要点（harness 顺序，实测踩坑）：
  1. 先起两个探针，**等 air 探针打印 CONNECTED 后再启动 gst 源**——SDK 未注册时
     `PushVideoFrame` 直接丢弃（air 探针内部在 CONNECTED 前不消费 RTP，保持字节序列完整）；
  2. gst 收端 `h264parse` 默认输出 AVCC（length-prefixed），需显式
     `! video/x-h264,stream-format=byte-stream` 才是 Annex-B（源分叉落盘同理）；
  3. 收端 gst 用 `timeout -s INT ... gst-launch-1.0 -e` 结束，保证 filesink 收到 EOS 落盘完整。

- **验证记录（M2 已完成：RtcLink 冒烟，工具已收入仓库）**：
  工具位于 `ohd_interface/test/rtc_probe/smoke/`（`build.sh` 编译 `air_smoke`/`gnd_smoke`，
  两者直接用真实 `RtcLink` 类 + 仓库 `rtc_config.cpp`；`config_paths_stub.cpp` 替换
  `/Config/` 路径以便开发机运行；`run_smoke.sh` 一键编排 air/gnd/gst/探针 + framemd5 比对，
  用法见脚本头注释；`rtc.txt.example` 为配置模板）。
  - 编译/链接：`rtc_config.cpp`、`rtc_link.cpp` 过 `-std=c++17 -Wall -Wextra` 零告警；
    冒烟二进制成功链接真实 SDK 静态库（`rtc_client_sdk`/`frame_protoc`/`pacer`/`mem_pool`），
    所调 SDK 符号全部存在。
  - 类布局：`sizeof(OHDInterface)` 在 `OHD_HAS_RTC_LINK` 开/关下均 576B（无 ABI 漂移）。
  - 冒烟 run1（探针先起，实测订阅失败路径）：`STREAM_NOT_FOUND` → SDK 网络线程退出 →
    RtcLink ground 看护 2s 退避后重建订阅 → `已订阅 SFU 流 (session 3110)` → 812 AU，
    `bad=0`；**PLI 链路通**（gnd `RequestKeyFrame` → air `cam0 收到关键帧请求(PLI) 累计 1`）。
  - 冒烟 run2（air 先起）：air 推 837 帧（4256 RTP 包）；ground 收 837 AU / 3414 RTP 包，
    `bad=0`（RTP V/PT 全合规）；VPS 侧探针帧统计 idr=27 / p=780，`fec lost=0 rtt=0`；
    **framemd5 源==ground 重打包==探针收流 807/807 逐帧全同**（帧数差为探针 28s 采集窗
    与 gst 源 1s 起播偏移所致，计数完全收敛可解释）。
  - 冒烟 run3（仓库脚本复跑复核，`build.sh` + `run_smoke.sh` 原样）：air 835 帧 →
    ground 835 AU / 3406 包 `bad=0`，探针 framemd5 **804/804 全同**（offset=0），PLI 往返再次命中。

### 10.3 集成测试（L2–L4 拓扑）

| 级 | 拓扑 | 步骤 | 通过标准 |
|---|---|---|---|
| L2（≈原 Phase 0） | 现有 pi4 pusher 推 `pivideo` → VPS → pi4 运行 ground 探针(订阅 pivideo) | QOpenHD 接 5600 出画；观察 fps/花屏/卡顿 | 视频连续、无花屏；AU→RTP 正确 |
| L3（≈原 Phase 1） | OpenHD air(RtcLink, stream=openhd0) → VPS → Android App / mac_pull_test | App 解码出画；PLI 后恢复 | [SPS][PPS][IDR] 可解码；丢包后 ≤1 关键帧周期恢复 |
| L4（≈原 Phase 2） | OpenHD air ↔ VPS ↔ OpenHD ground | 全通道（视频/遥测/RC/音频）+ 弱网 | 遥测双向通、视频端到端可用、延迟与码率达标 |

L4 观测项：端到端延迟（秒表/帧号法）、`Subscriber::GetStats()`（丢包/RTT）、
CPU/内存（pi4）、air 端队列深度。

**验证记录（L2 已完成，2026-09-21，pi4 实机）**：
- 拓扑：pi4 `raspberry_pi_pusher`（1280×720@30、`--gop 30`、stream=`pivideo`，一直在跑）
  → VPS SFU → **pi4 本机** `rtc_gnd_probe`（订阅 pivideo → `EsRtpPacketizer` → RTP/UDP 转发）
  → 开发机 gst `rtp264depay` 收流落盘。
- pi4 侧构建：探针源码部署 `~/rtc_l2/`（新增目录，未动设备现有 `~/tj_rtc` 与 pusher），
  CMake 以 `-DTJ_RTC_ROOT=~/rtc_l2/tj_rtc` 使用**当前版 SDK 的 vendor 子集**
  （`rtc/{client_sdk,common,libs}`，~0.9MB，布局即 §7.2）——顺带验证了 vendor 子集在 ARM64
  自洽可构建（F13 NEON 自动选择生效）。**发现**：pi4 原有 `~/tj_rtc` 是旧版 SDK
  （`Subscriber` 无 `SetOnSubscribeFailed` 等新 API），故不复用它。
- 结果：30s 订阅 `frames=930`（31fps 实速，`params=30 idr=30 p=870` 与 gop=30 吻合），
  `rtp_out=2783`，FEC `done=930 dropped=0 lost=0 rtt=0ms`；两端 dump
  **framemd5 879/879 逐帧全同（mismatches=0）**，`ffprobe` 1280×720 h264 可解码。
  （879<930 为起始半 GOP 内无参数集参考所致，两端计数一致，非缺陷。）
- **QOpenHD 出画确认（同日补齐，开发机）**：官方 QOpenHD 的 Linux x86 预编译包来自
  OpenHD cloudsmith apt 仓库（jammy/amd64 deb，v2.6.4）；开发机无 root，用
  `apt-get download + dpkg-deb -x` 装到本地前缀（脚本入库：
  `test/rtc_probe/l2_qopenhd/{install_qopenhd_noroot.sh,run_qopenhd.sh}`）。
  关键点：需补 `libqt5waylandclient5`（否则 qmlglsink 插件加载失败）与
  `qml-module-qtquick-templates2`（QtQuick.Templates）；装完后需清 GStreamer registry
  缓存（`~/.cache/gstreamer-1.0`）再启动。QOpenHD 的 avcodec(custom_rtp) 接收路径
  bind 0.0.0.0:5600（源码 `rtpreceiver.cpp` 用 `UDPReceiver{nullopt, port}`），
  故 pi4 探针 `--out-ip <开发机IP> --out-port 5600` 可直灌出画，无需本机转发。
  结果：QOpenHD EVO 窗口正常渲染 HUD；视频区画面与源流参考帧逐像素**中心区均值
  RGB Δ≈5/255**（QOpenHD 侧 (206,197,201) vs 源 (212,201,207)，差异为 HUD 半透明叠加
  与缩放所致）——出画确认。注意：pivideo 源画面本身为静态渐变（相机对准墙面），
  运动画面/花屏评估留待 L3/L4 有真实运动源时进行。
- 复现命令（路径已按重组后布局适配）：
  ```bash
  # pi4 部署（开发机执行；新增 ~/rtc_l2，不动设备现有 ~/tj_rtc）
  tar -C apps/OpenHD -cf - OpenHD/ohd_interface/{inc/{rtp_es_assembler,es_rtp_packetizer}.h,src/{rtp_es_assembler,es_rtp_packetizer}.cpp,test/rtc_probe/{CMakeLists.txt,rtc_gnd_probe.cpp,rtc_air_probe.cpp}} \
    | ssh pi4 'mkdir -p ~/rtc_l2 && tar -C ~/rtc_l2 -xf -'
  tar -C rtc -cf - client_sdk common libs | ssh pi4 'mkdir -p ~/rtc_l2/tj_rtc && tar -C ~/rtc_l2/tj_rtc -xf -'
  ssh pi4 'cd ~/rtc_l2 && cmake -S OpenHD/ohd_interface/test/rtc_probe -B build \
      -DTJ_RTC_ROOT=$HOME/rtc_l2/tj_rtc && cmake --build build -j4 --target rtc_gnd_probe'
  # 运行（地面机先起 gst 监听 <PORT>，再把 --out-ip 指向地面机 IP）
  ssh pi4 './build/rtc_gnd_probe --server 106.15.177.248 --port 9200 --stream pivideo \
      --user ohd_gnd_l2 --out-ip <地面机IP> --out-port <PORT> --dump-au ~/rtc_l2/l2_pivideo.h264 --seconds 30'
  # 地面机：gst-launch-1.0 -e udpsrc port=<PORT> caps="application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000" \
  #   ! rtph264depay ! h264parse ! video/x-h264,stream-format=byte-stream ! filesink location=rx.h264
  # 对照：ffmpeg -i 两侧文件 -f framemd5 -  后窗口比对（同 L1 口径）
  ```

**验证记录（L3 已完成，2026-09-21，pi4 实机 air → 开发机 pull）**：
- 拓扑：pi4 **本仓构建的 OpenHD `--air`**（真实 ov5647，1920×1080@30，Constrained
  Baseline L4.0）→ GStreamer 编码 → **RtcLink**（stream=`openhd0`，air 会话 3128）
  → VPS SFU → 开发机 `rtc_gnd_probe` 订阅（30s）→ AU dump 离线校验 + QOpenHD 出画。
- pi4 部署（全程增量，未动设备现有 `~/tj_rtc` 与 pusher）：新增 `~/OpenHD`（源码 tar
  部署 + `cmake -DTJ_RTC_SDK_DIR=$HOME/rtc_l2/tj_rtc` 构建）；补装依赖
  `libpoco-dev libpcap-dev libsodium-dev libv4l-dev gstreamer1.0-tools`。
- **发现/修复（构建期缺陷，M2 遗留）**：`ohd_interface/CMakeLists.txt` 的
  `target_sources` 漏掉 `src/rtp_es_assembler.cpp`、`src/es_rtp_packetizer.cpp`
  （此前仅冒烟 build.sh 单独编译，M2 冒烟未暴露）→ pi4 链接 undefined reference；
  补入后构建 100% 通过。已修复入库。
- **发现（libcamera 双栈 ABI 冲突）**：设备上发行版 gst 插件低层
  `libcamera.so.0.5` 需 `libpisp 1.2.1` 的 `compute_optimal_stride(..., bool)`
  符号，但 ld.so 优先命中源码构建的 `/usr/local/libpisp 1.0.7`（配套 libcamera
  0.4.0）→ 发行版插件加载失败。解法：改用 0.4 配套的
  `/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0/libgstlibcamera.so`，运行脚本固定
  `GST_PLUGIN_PATH`（已入 `run_ohd_air.sh`），成功枚举 ov5647 出流。
- **发现（本 checkout 无 CSI 自动探测）**：rpi 默认相机类型 20（MMAL HDMI）；
  写 `/usr/local/share/openhd/video/air_camera_generic.json`
  `primary_camera_type=30`（RPIF_V1_OV5647）后 libcamera 正常配置 1080p NV12。
- 配置：`/Config/rtc.txt`（stream_prefix=openhd、user_id_air=ohd_pi4_air、
  pacer_mbps=20、enable_cam0=1、enable_cam1=0）。
- 结果（探针 30s）：`frames=844（params=0 idr=169 p=675）audio=0 unknown=0`，
  `rtp_out=20459 rx_bytes=28143324`，FEC `done=844 dropped=0 lost=2 fec_rec=2
  nack_rec=0` → **丢包全部由 FEC 恢复，应用层零缺口**；air 侧 70s 发送 2000 帧
  ≈28fps，正常终止。
- AU dump 离线校验：全量解码 `ffmpeg -v error` 退出 0（仅起始半 GOP 无参考的
  12 行预期告警）；`ffprobe` h264 Constrained Baseline 1920×1080 level=40 30fps；
  `check_au.py`：SPS=169 PPS=169 IDR=169 P=675——**每个 IDR 均带 in-band
  SPS/PPS**（OpenHD 管线 `h264parse config-interval=-1`，故 `params=0` 属预期），
  IDR 间隔 4–5 帧（平均 5.0，≈167ms@30fps）→ **丢包恢复窗口远小于 1 关键帧周期**。
- QOpenHD 开发机出画：HUD + 实时画面（`/tmp/l3/l3_shot1.png`）；末帧均值 RGB 与
  pivideo 参考场景差 <1/255（场景一致）。
- 已知限制：相机对准静态场景，「运动花屏」评估留待 L4；开发机无 root，tc netem
  丢包注入未做（留 L4，需用户同意改主机网络配置）；订阅须在 air 开始推流之后
  （探针无重试，重试/看护留 M5）。
- 脚本入库：`test/rtc_probe/l3_air_openhd/{run_ohd_air.sh, run_l3_gnd.sh,
  check_au.py}`。

**验证记录（L4 已完成，2026-09-21，pi4 OpenHD air 全通道 ↔ 开发机 ground 探针）**：
- 拓扑：pi4 本仓 OpenHD `--air`（ov5647 1080p，`enable_audio=100` audiotestsrc）
  → RtcLink（stream=`openhd0`，air 会话 101）↔ VPS SFU ↔ 开发机 `rtc_gnd_probe`
  （`--ctrl-hz 20`，60s，AU 落盘 + QOpenHD 出画）。四通道同测：视频 / 下行遥测 /
  上行 RC / 音频。
- 部署（增量）：P1 补丁 `Subscriber.cpp` + M5 版 `rtc_link.{h,cpp}` 同步到 pi4
  `~/rtc_l2/tj_rtc`、`~/OpenHD/ohd_interface`，`make -j3` 重建通过（含
  `librtc_client_sdk.a`）；pi4 探针源码同步并重建。
- **结果（probe 60s 终值）**：视频 `frames=1726（idr=348 p=1378）≈28.8fps`、
  `rx_bytes=57.6MB`、`rtp_out=41946`；音频 `audio=3140`（≈52.3/s，PCMA 50pps
  标称 ✓）；FEC `done=4866 dropped=0 lost=1 fec_rec=1` → **零缺口**。
  ctrl 下行：`rx_pkt=247 / msgs=1029 / hb=240 / other=789`（msgid=1219 等 OpenHD
  自定义消息），air 侧日志 `遥测 ctrl 已发送 400 包`（每包 392B 聚合、21B 心跳
  交替）→ **air 心跳 2Hz 全程连续下行**。ctrl 上行：probe `tx=1242`
  （RC 20Hz×60s + 心跳 1Hz），air 侧 `遥测/RC ctrl 已收到 ≥1200 包（最近 30 字节
  = RC_CHANNELS_OVERRIDE 定长 30B，逐字节吻合）`→ 上行交付 ≥96.6%（差额为
  air 收尾期最后数秒；日志 1/200 粒度，精确终值未列印）。
- **P1 实测生效**：订阅后第一秒内 air 即收到上行 ctrl（视频首帧未到不构成前置），
  与 L1.5 冒烟（首个上行 552ms、视频 5s 才开）互为印证；air 侧 `已收到 200/400…`
  计数与 probe 发送计数同步增长。
- 视频内容校验：`ffmpeg -v error` 全量解码 0 错误；`check_au.py` SPS=348 PPS=348
  IDR=348 P=1378，IDR 间隔 1–5 帧（avg 5.0）——与 L3 一致（in-band SPS/PPS）。
  抽帧统计：均灰 6.6/σ1.72（夜间暗场）；同一时刻 pusher `pivideo` 同相机流
  均灰 6.55/σ2.38 → **暗场属真实场景（黑夜），非链路故障**。QOpenHD 解码推进与
  L3 完全同型（SPS/PPS/IDR → reinit 1920×1088 yuv420p）。
- **踩坑与修复（均已入库）**：
  1. `run_l4_full.sh` 首版用远端 `nohup … &` 启 air，ssh 会阻塞到远端进程所有
     fd 关闭（实测阻塞 85s 至 air 退出才返回）→ 改为**本地后台 ssh**（脚本内
     记录 pid，末尾 `wait`），并新增“等 air 在 SFU 注册（grep 日志）”同步点。
  2. 探针主循环 100ms 粒度把 `--ctrl-hz` 上限压到 ~10Hz（首轮 tx=658 暴露）→
     循环步长按速率减半（20Hz → 25ms 步长），次轮 tx=1242 达标。
  3. 探针心跳未受 `ctrl_hz` 门控（`--ctrl-hz 0` 仍 1Hz 上行）→ 收进
     `ctrl_hz>0` 分支；1s 统计行补 `fflush`（重定向文件时块缓冲导致日志不可读）。
- 环境副作用（已还原）：测试期停 pusher 释放相机，结束后以其自带
  `~/tj_rtc/restart_pusher.sh` 恢复（pivideo 已复流）；air `enable_audio` 由脚本
  临时置 100，trap 还原为 1；开发机 QOpenHD 退出。
- 脚本入库：`test/rtc_probe/l3_air_openhd/run_l4_full.sh`。
- 已知限制（同 L3 顺延）：暗场下「运动花屏」评估仍不充分；tc netem 弱网注入
  需用户同意改主机网络配置，未做（`rc_reliable` 一键切换机制已就位）；RC 交付率
  受 air 日志粒度限制只能给出下界（≥96.6%，实际更高）。

### 10.4 验收标准（v1）

- 1080p@30fps/8-20Mbps 经 VPS 中转，QOpenHD 出画连续，无持续花屏；
- 遥测双向 ≤ 50ms 量级抖动可接受，RC 可用；
- air 掉线重连、ground 先启后连（air 后上线）两个场景自愈；
- 现有 WFB/Ethernet 链路行为零回归（rtc.txt 不存在时不加载任何 RtcLink 代码路径）。

---

## 11. 里程碑与工作量（估）

| 里程碑 | 内容 | 估时 | 状态 |
|---|---|---|---|
| M0 | **抽件 + L0**：RtpEsAssembler/EsRtpPacketizer 实现 + 离线往返自测（本机，零外部依赖） | 0.5d | ✅ 已完成（81 项用例 + gst 真实夹具 90/90） |
| M1 | **L1 本机仿真**：air/ground 探针工具 + VPS 全链预演 + RtcConfig/CMake 骨架 | 0.5–1d | ✅ 探针+VPS 全链已通（AU 全同、framemd5 600/600）；RtcConfig/CMake 骨架已在 M2 落地 |
| M2 | RtcLink 骨架 + OHDInterface 三处挂接（为 L2 准备） | 0.5d | ✅ RtcLink/Config 落地，冒烟全绿（framemd5 807/807，看护重建+PLI 实测通过） |
| M3 | ground 方向接入 + L2（pi4 订阅 pivideo，QOpenHD 出画） | 0.5d | ✅ 完成：pi4 探针 930 帧/FEC lost=0（framemd5 879/879 全同）+ QOpenHD 开发机出画（画面均值 Δ≈5/255） |
| M4 | air 方向接入 + L3（OpenHD air → App/pull_test） | 0.5d | ✅ 完成：pi4 OpenHD air（ov5647 1080p@30）→ VPS → 开发机探针 844 帧；FEC lost=2 全恢复、AU 全量解码+QOpenHD 出画；顺带修复 CMake `target_sources` 缺陷 |
| M5 | 遥测/RC/音频/看护（含补丁 P1）+ L4 端到端 | 0.5–1d | ✅ 完成：P1 入 SDK；遥测/RC 走首路相机 ctrl（ac=_ctrl_downstream/ground=_ctrl_upstream，UDP 按 n_injections 复制）；音频 air=PushAudioFrame(PCMA)/ground=0x10→on_receive_audio_data；L1.5 冒烟四通道全绿（视频 framemd5 802/802、遥测 292/292、音频 292/292、RC 280/280、首个上行 ctrl 552ms 早于视频）；L4 真机 60s：视频 1726 帧解码零错、音频 3140、下行 msgs=1029、上行 RC 20Hz 交付 ≥96.6%、FEC 零缺口 |
| 合计 | 每级验证通过才进下一级 | **约 3.5–4.5 个工作日** | |

---

## 12. 风险与开放问题

| # | 项 | 影响 | 应对 |
|---|---|---|---|
| R1 | AU 成组（ts/marker）在真实 gst 输出上的健壮性 | 花屏/帧错位 | L0 往返单测 + L1/L2 真实夹具；超时兜底 |
| R2 | QOpenHD 对无 AUD 流的兼容性 | 不出画 | L2 决定是否补 AUD（开关预留） |
| R3 | VPS 中转延迟（视频 UDP 已定；RC 默认 UDP+复制，TCP 备选） | RC 手感 | L4 弱网实测；`rc_reliable` 一键切换 |
| R4 | 无加密（WFB 有 txrx.key） | 安全 | v2 载荷层加密或 SFU 侧方案 |
| R5 | Publisher 队列积压 | 内存积压 | 已确认有界：`kMaxVideoQueueSize=60`，满时丢"最近关键帧之前"的帧（Publisher.cpp:112-127），保 IDR 可解；L4 观察实际深度 |
| R6 | H.265 用户误配 | 链路不可用 | 弱校验告警 + 文档；v2 补齐 |
| R7 | 单链路模型（一次只有一种 link） | 无法 WFB+RTC 并发 | v1 明确不支持；并发需多链路聚合（另立项） |

**开放问题（需实现前确认）**：
1. RC 可靠性：已定默认 unreliable+复制（§4.3，对齐 WFB 语义）；L4 公网实测交付
   ≥96.6%（无弱网注入）；`rc_reliable=1` 一键翻 TCP（只发一次）机制已就位，
   弱网实测需用户同意改主机网络配置后补做；**2026-09-23 补：Android App 上行对 live air
   实测 UDP 全丢、TCP 全达 → App 固定 reliable=true（§4.3 实测注记）；**
2. 补丁 P1 是否上游化（回馈 tj_rtc 仓库，减少 vendor 补丁面）；
3. 是否需要把 Link 状态/GetStats 通过 OpenHD 设置上报给 QOpenHD（v2 候选）；
4. ~~QOpenHD 出画确认环境~~（已解决）：QOpenHD v2.6.4 jammy/amd64 deb 已无 root 装到
   开发机并出画（§10.3 L2 记录；脚本 `test/rtc_probe/l2_qopenhd/`）；pi4 作为 L2 ground
   侧产生 RTP，UI 侧由开发机承担。后续 L3/L4 若需设备端 UI 再议。

---

## 附录 A：关键代码位置索引

| 用途 | 位置 |
|---|---|
| OHDLink 契约 | `apps/OpenHD/OpenHD/ohd_common/inc/openhd_link.hpp:54` |
| 链路实现模板 | `apps/OpenHD/OpenHD/ohd_interface/src/ethernet_link.cpp` |
| 链路装配 | `apps/OpenHD/OpenHD/ohd_interface/src/ohd_interface.cpp:77-118, 557-571` |
| RtcLink 实现（M2） | `apps/OpenHD/OpenHD/ohd_interface/src/rtc_link.cpp`；头 `inc/rtc_link.h` |
| RtcConfig（M2） | `apps/OpenHD/OpenHD/ohd_interface/src/rtc_config.cpp`；头 `inc/rtc_config.h` |
| RtcLink 冒烟工具（M2） | `apps/OpenHD/OpenHD/ohd_interface/test/rtc_probe/smoke/`（build.sh + run_smoke.sh + rtc.txt.example） |
| L2 QOpenHD 无 root 安装/启动（M3） | `apps/OpenHD/OpenHD/ohd_interface/test/rtc_probe/l2_qopenhd/`（install_qopenhd_noroot.sh + run_qopenhd.sh） |
| air 视频发送点 | `apps/OpenHD/OpenHD/ohd_video/src/ohd_video_air.cpp:291` |
| ground 视频转发 | `apps/OpenHD/OpenHD/ohd_video/src/ohd_video_ground.cpp:48, 87-97` |
| RTP 边界松散证据 | `apps/OpenHD/OpenHD/ohd_video/src/rtp_eof_helper.cpp:98-107` |
| AU 约定（pusher） | `apps/demos/raspberry_pi_pusher/src/NaluSplitter.cpp:64-100` |
| AU 约定（Android） | `apps/demos/android_rtc_app/app/src/main/java/com/example/rtcap/RtcClient.java:489-491` |
| SDK API | `rtc/client_sdk/rtc_client_sdk/include/Publisher.h, Subscriber.h` |
| FrameType 映射 | `rtc/client_sdk/rtc_client_sdk/src/Subscriber.cpp:148-171` |
| SFU PLI | `rtc/server/sfu_server/SfuServer.cpp:919-924, 1059-1073` |
| 订阅失败退出 | `rtc/client_sdk/rtc_client_sdk/src/Subscriber.cpp:513-519` |
| ctrl 下行发送/广播 | `Publisher.cpp:427-467`；`SfuServer.cpp:1994-2131` |
| ctrl 上游发送/窗口 | `Subscriber.cpp:178-219`（sourceSessionId==0 丢弃）；设置点 `:441, :545` |
| ctrl 收发分发 | `Subscriber.cpp:224-242`；`Publisher.cpp:472-490, 728-736` |
| 遥测聚合与注入数 | `apps/OpenHD/.../mav_include.h:80-116`（≤1024B）；注入复制 `WBStreamTx.cpp:321` |
| FEC 分组 | `rtc/libs/frame_protoc/src/types.h:14`；`protocol.h:17-18` |
| 构建/NEON | `rtc/libs/frame_protoc/CMakeLists.txt:30-38` |
| SFU 端口约定 | `rtc/server/sfu_server/SfuServer.h:115-121`（9200/9201/9202） |
| 音频管线 | `apps/OpenHD/OpenHD/ohd_video/src/gstaudiostream.cpp:110-145` |