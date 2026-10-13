# SFU 组帧 + xquic MoQ 网关 + WebTransport Web Demo 实施计划

> 版本：2026-10-12 v3（回归 xquic 方案；网关直接复用 `moq/demo` 作底座）
> 范围：`rtc/` 核心（SFU 组帧、IPC）+ `rtc/libs/xquic/moq/`（网关 + WebTransport 补齐）+ `apps/demos/`（Web 测试页）
> 参考：`docs/PROGRESS.md`（现行环境约束）、`rtc/libs/xquic/AGENTS.md`（xquic fork 开发规范）
> v2（moxygen 底座）作废原因：按用户决策回归 xquic——其 `moq/demo` 已提供即用的 server/client 底座，改造量远小于引入外部依赖链；xquic MoQ 为 pre-standard（`0xff000005`）这一事实记录在案（§9-8），标准对齐列入后续观察项。

---

## 1. 目标与现状

### 1.1 目标（四条验收线）

| # | 目标 | 验收判据 |
|---|------|---------|
| G1 | SFU 具备**组帧能力**：将上游 FEC 分片包按帧重组为完整媒体帧（H.264 AU） | SFU 1s 统计行出现 `[SFU-ASM]`：`assembled=30/s`、`pending≤2`、无持续 timeout（§6） |
| G2 | **xquic MoQ 网关**（独立进程）：从 SFU 获取完整帧，以 MoQ track 发布 | xquic `moq_demo_client` 订阅到网关的流，收到**真实帧**（长度/时序/KEY 与源一致） |
| G3 | xquic fork **补齐 WebTransport**：HTTP/3 extended CONNECT + Session 语义 | Chrome `new WebTransport(...)` 握手成功、`ready` resolve |
| G4 | **Web 测试 demo**：浏览器原生接收 MoQ 媒体并用 WebCodecs 解码播放 | Chrome 页面出画 720p@30，端到端延迟 < 1.5s |

### 1.2 现状（已核实）

**SFU / 组帧 / IPC 侧**

- SFU（`rtc/server/sfu_server/`，libev 单事件循环）：UDP 推流会话挂 `ProxyFrameReceiver`（FEC 过滤/NACK/缓存，**只透传分片包，不做帧重组**）；TCP 9200 / UDP 9201 / STUN 9202。
- frame_protoc：`FrameReceiver`（`rtc/libs/frame_protoc/src/receiver.cpp`）已具备完整组帧能力（FEC 解码、按序提交游标、`setFrameReadyCallback` 输出 `Frame{type,size,data}`，data 为 Annex-B AU），但**带 NACK/PING/STATS 回包副作用**，当前仅客户端订阅端使用。
- 帧类型映射：推流侧 `VIDEO_PARAMS=0x01 / VIDEO_IDR=0x02 / VIDEO_P=0x03 / AUDIO_PCM=0x10`；FEC 侧 `PARAMS=0x04 / IDR=0x03 / VIDEO=0x02 / AUDIO=0x01`。

**xquic MoQ demo（本轮核实，`rtc/libs/xquic/moq/demo/`，共 2546 行）**

| 文件 | 说明 |
|------|------|
| `xqc_moq_demo_server.c`（791 行） | MoQ 服务端 demo：libevent 循环 + xquic engine；`xqc_moq_init_alpn(...QUIC)` 注册 ALPN `moq-quic`（仅 raw QUIC）；session setup 建 `"namespace"/"video"`(av01, LOC) + `"audio"`(opus, LOC) 两 track；`on_subscribe`→`subscribe_ok`→启动 33ms 假帧定时器；**假帧发生器 `xqc_app_send_callback`**（payload={1..10}、帧长=target_bitrate/8/30×帧长系数表、每 30 帧或 `request_keyframe` 发 KEY）；`on_request_keyframe`→置标志→下一帧 KEY（**PLI 闭环已打通**）；`xqc_moq_configure_bitrate` + `on_bitrate_change` 回调齐备 |
| `xqc_moq_demo_client.c`（887 行） | 客户端 demo：`-a/-p` 连服务端、自签证书放行、0-RTT ticket/token/proxy 选项；`on_catalog`→`xqc_moq_subscribe_latest`；`on_video/on_audio` 打印 seq/len/延迟；seq==3 时演示 `xqc_moq_request_keyframe`；也可 `-r pub` 反向发布（同假帧发生器） |
| `xqc_moq_audiodemo_server.c`（595 行） | 音频版服务端（二期音频参考） |
| `xqc_moq_demo_comm.{h,c}` | 共享底座：libevent+engine 定时器对接、日志、socket 写回调、`user_conn_t`（含 moq_session/track/subscribe_id/ev_send_timer） |
| `moq/demo/CMakeLists.txt` | 构建 3 个 demo 目标；**需 `XQC_ENABLE_MOQ=ON` + `XQC_ENABLE_TESTING=ON`**（`moq/tests` 子目录依赖 CUnit，可只编 demo 目标规避） |

**关键结论**：demo 的假帧发生器（`xqc_app_send_callback`）就是网关的接入点——**整体替换为 IPC 真帧扇出即可**，订阅协商、PLI→KEY、节拍器、统计打印全部保留。其余改造点：namespace 从写死 `"namespace"` 改为按流名动态、单流广播改为多连接扇出、加 IPC 客户端。

**xquic MoQ 传输现状（未变）**

- `xqc_moq_session.c`：`xqc_moq_init_alpn` 仅注册 `XQC_ALPN_MOQ_QUIC`；`XQC_MOQ_TRANSPORT_WEBTRANSPORT` 分支被注释（`//TODO: WEBTRANSPORT`）；client setup 版本 `XQC_MOQ_VERSION_5`（0xff000005）。
- `xqc_moq_stream_webtransport.c` 为空桩；HTTP/3 层有 `h3-ext`（ALPN "h3-ext"）与 bytestream/datagram 原语，但**无 extended CONNECT（`:protocol`）与 WebTransport Session 概念** → 浏览器不可直连，阶段 E 补齐。
- 构建环境：本机有 libev-dev、libevent-dev、libssl-dev；**无 boringssl**、xquic 从未编译过。

### 1.3 关键设计决策

1. **MoQ 网关 = 独立进程 + 本地 IPC**（已确认）：主构建零污染，网关崩溃不影响推拉流。
2. **网关基于 `xqc_moq_demo_server.c` 改造**（本轮新定，替代 v2 的 moxygen 底座），新目标 `moq_gateway`，与 demo 并列构建。
3. **组帧在 SFU 内完成**，双路并行：现有订阅者转发路径（分片包 + FEC 过滤）**零改动**；新增第二条组帧路径供网关消费。
4. **Web 端补齐 WebTransport**（xquic fork 内实现，沿用既定决策）。

---

## 2. 总体架构

```
推流端 (pi4_air 等)
   │ FEC 分片包 (UDP 9201, 保持现状)
   ▼
┌──────────────────────────────────────────────────────────┐
│ SFU (sfu_server_full, libev 单线程)                       │
│  SfuSession[publisher]                                    │
│   ├── ProxyFrameReceiver ──► 订阅者 (现状零改动)           │
│   └── FrameAssembler (新, 懒启动) ──► 完整帧               │
│            │                                              │
│            ▼                                              │
│      FrameDispatcher (新)                                 │
│         ├─ 统计 (assembled_fps/kbps/drops/timeouts)        │
│         ├─ PLI 汇聚 → 上游 requestKeyFrame                 │
│         └─ IPC: UDS /run/tj_rtc/frames.sock               │
│              SUB/UNSUB/PLI ⇄ 帧流出                        │
└──────────────────────────────────────────────────────────┘
                    │ 本地 IPC (UDS, 二进制帧协议)
                    ▼
┌──────────────────────────────────────────────────────────┐
│ moq_gateway (新进程, libevent + xquic engine)              │
│   基于 xqc_moq_demo_server.c 改造：                        │
│   ① IPC 客户端 (ev_io): 订阅流、收完整帧                   │
│   ② 假帧发生器 → 真帧扇出: 每 IPC 帧写所有订阅连接的        │
│      xqc_moq_write_video_frame (KEY/DELTA 映射)           │
│   ③ 订阅驱动: on_subscribe → IPC SUB (引用计数);           │
│      on_request_keyframe → IPC PLI → SFU → 推流端 IDR     │
│   ④ namespace "tj/<streamId>", track: video (audio 二期)   │
│   ⑤ 传输: ALPN "moq-quic" (raw QUIC, 本机调试)             │
│      [阶段 E 后] + ALPN "h3" WebTransport (浏览器)         │
└──────────────────────────────────────────────────────────┘
        ▲                        ▲
        │ moq-quic (调试)        │ WebTransport (h3, E 阶段后)
   moq_demo_client            Chrome / Edge 浏览器
                              (demo 页: JS MoQ client + WebCodecs)
```

**帧数据格式**：组帧输出 `Frame{type,data}`，`data` 为 Annex-B H.264 AU（IDR 含或紧随 SPS/PPS PARAMS 帧）。IPC/网关/浏览器链路全程保持；关键帧判定 `type==VIDEO_IDR||VIDEO_PARAMS` → MoQ `XQC_MOQ_VIDEO_KEY`；`VIDEO_P` → `XQC_MOQ_VIDEO_DELTA`。PARAMS 帧作为 KEY 帧的载荷前缀合并发送（或直接发 KEY）。

**流选择**：网关 `--stream <id>[,<id2>...]` 白名单（v1 允许单流，多流验证后放开）；MoQ 客户端 setup 的 `path/extdata` 参数可携带 streamId 选择 namespace（缺省取白名单第一项）。

**订阅驱动**：无 MoQ 订阅者时网关不向 SFU 订阅、SFU 不启动组帧（零开销）；首个订阅者到达 → IPC `SUB <stream>`；最后一个断开 → `UNSUB`。

---

## 3. 分阶段工作分解

### 阶段 A：xquic 构建落地（前置，0.5 天，独立可验证）

| # | 内容 | 产出 |
|---|------|------|
| A1 | `scripts/build_boringssl.sh`：克隆 google/boringssl，`cmake -DBUILD_SHARED_LIBS=0 -DCMAKE_C_FLAGS=-fPIC` 编译 ssl/crypto → `rtc/libs/xquic/third_party/boringssl`（gitignore） | 静态库 + 脚本 |
| A2 | `rtc/libs/xquic/build.sh`：封装官方参数（`-DXQC_ENABLE_MOQ=ON -DXQC_ENABLE_TESTING=ON -DXQC_ENABLE_BBR2=ON -DSSL_TYPE=boringssl -DSSL_PATH=...`），产物 `build/lib/libxquic.a` + demo 可执行；CUnit 缺失时只编 demo 目标 | 一键构建脚本 |
| A3 | 验证：`moq_demo_server` + `moq_demo_client -r sub -n 300` 本机对跑（自签证书生成），client 持续收假帧、PLI 生效；`scripts/validate.sh build` 通过 | 阶段验收 + 命令备忘 |

**风险点**：boringssl 编译 ~10min；`moq/tests` 依赖 CUnit（`apt install libcunit1-dev` 或仅编 demo 目标，实测确认）。

### 阶段 B：SFU 服务器组帧（1.5 天）

| # | 文件 | 内容 |
|---|------|------|
| B1 | `rtc/libs/frame_protoc/src/frame_assembler.{h,cpp}` 新增 | 独立轻量组帧器：复用 `receiver.cpp` 的 FEC 解码与按序提交内核，**剥离 NACK/STATS/PONG 等一切回包副作用**（服务器代理层已负责）；接口 `onDataPacket(data,len,now_us)` + `setFrameReadyCallback` + `tick`；帧超时/乱序游标/FEC 组回收沿用现有实现。不复用 `FrameReceiver` 的原因：其 tick 会重复发 NACK/PING，与 ProxyFrameReceiver 冲突 |
| B2 | `rtc/server/sfu_server/SfuServer.{h,cpp}` | `SfuSession` 增 `frame_assembler` 成员（仅 UDP 推流会话，懒创建）；`OnUdpPacketReceived` 的 proxy 分支内并行投喂 DATA 包（只读消费，不改 proxy 行为） |
| B3 | `rtc/server/sfu_server/SfuServer.cpp` | `onFrameReady(stream_id, frame)` → `FrameDispatcher`（阶段 C 前先打统计日志）；`AdjustProxyFecRatio` 附近观测点同步扩展 |
| B4 | `SfuServer::PrintProxyStats()` 扩展 | 每流一行：`assembled_frames / assembled_kbps / pending / drops / timeouts` |
| B5 | `rtc/server/sfu_test/` | 单测：分片序列→组帧输出断言；FEC 恢复路径；超时丢弃路径（happy + abnormal） |

**关键不变量**：订阅者转发路径的包序、FEC 过滤比例、NACK 语义**零改动**；组帧器只读、不产生任何回包。

### 阶段 C：IPC + FrameDispatcher（1.5 天）

| # | 文件 | 内容 |
|---|------|------|
| C1 | `rtc/common/sfu_protocol/FrameIpc.h` 新增 | UDS 二进制协议：控制帧 `SUB/UNSUB/PLI stream_id`；数据帧 `[u32 len][u32 stream_id][u8 frame_type][u8 flags][u64 pts_us][u64 seq][payload...]`；本机域内，无鉴权需求；带版本号。**无依赖头文件**——网关侧（xquic 仓库内）持独立副本，配协议契约测试防漂移 |
| C2 | `rtc/server/sfu_server/FrameDispatcher.{h,cpp}` 新增 | UDS 服务端（`ev_io` 融入 libev 循环）：连接管理、订阅表（引用计数）、帧 fan-out、写缓冲背压（满则**保 IDR 丢 P**）、PLI 汇聚转发上游 `requestKeyFrame()`；订阅驱动懒启动/停止 assembler |
| C3 | `rtc/server/sfu_server/main.cpp` | 新 CLI：`--frame-ipc <path>`（默认 `/run/tj_rtc/frames.sock`，空=禁用） |
| C4 | `rtc/server/sfu_test/` | IPC 集成测试：订阅→收帧→退订；慢消费者背压；断连重连 |

### 阶段 D：MoQ 网关（基于 demo server 改造，2.5 天）

| # | 文件 | 内容 |
|---|------|------|
| D1 | `rtc/libs/xquic/moq/demo/xqc_moq_gateway.c` 新增（独立 target） | 复制 `xqc_moq_demo_server.c` 骨架（libevent + engine + `xqc_moq_demo_comm.c` 复用），删除假帧发生器与 countdown 逻辑，命名 `moq_gateway` |
| D2 | 网关 IPC 客户端（同文件或 `xqc_moq_gateway_ipc.{h,c}`） | UDS 连接（`ev_io` 融入 libevent 循环）：SUB/UNSUB/PLI 发送、数据帧接收→按 stream 分发的环形缓冲；断线重连（1s 间隔）；慢消费背压丢弃保 IDR |
| D3 | 帧扇出（替换 `xqc_app_send_callback`） | 每收到一个 IPC 帧：对**所有** `video_subscribe_id != -1` 的连接调用 `xqc_moq_write_video_frame`（KEY/DELTA 映射；`seq_num/pts` 透传）；`track` 由连接 session 创建；namespace `tj/<streamId>`、track `video` |
| D4 | 订阅驱动与 PLI | `on_subscribe`：回 `subscribe_ok` + IPC `SUB`（引用计数）；连接关闭/退订：引计数减，归零 → `UNSUB`；`on_request_keyframe` → IPC `PLI`（现有 SFU PLI 节流链路上游 IDR）；`on_bitrate_change` 记录日志（后续可映射为码率建议回传） |
| D5 | 多流与配置 | `--stream <id>`（可重复）；`on_session_setup` 按客户端 path/extdata 选择流并建 track/catalog；无订阅者不推送 |
| D6 | CMake + 单测 | `moq/demo/CMakeLists.txt` 增 `add_executable(moq_gateway ...)`；单测：frame_type→KEY/DELTA 映射、PARAMS 合并、扇出计数、IPC 断连（happy + abnormal） |

**可直接保留**：PLI→下一帧 KEY 的闭环、33ms 节拍（改为 IPC 到达节拍）、session/track/subscribe_ok 协商、统计打印、xquic 层 FEC 选项（`-f`，与 frame_protoc FEC 无关，调试用）。

### 阶段 E：xquic 补齐 WebTransport（3.5 天，最大风险项）

> 现状：`h3-ext`（ALPN "h3-ext"）有 bytestream/datagram 原语；缺 extended CONNECT、Session 管理、浏览器兼容的 SETTINGS/ALPN。浏览器要求：ALPN `h3`、`SETTINGS_ENABLE_CONNECT_PROTOCOL=1`、CONNECT 请求带 `:protocol=webtransport`、stream 4 字节 session_id 前缀、datagram `quarter stream id` 前缀、响应 200 于 CONNECT 流。

| # | 文件 | 内容 |
|---|------|------|
| E1 | `include/xquic/xqc_http3.h` + `src/http3/xqc_h3_conn.{c,h}` | SETTINGS 扩展：`enable_connect_protocol`；本地/对端 settings 解析与校验 |
| E2 | `src/http3/xqc_h3_header.{c,h}` | `:protocol` 伪头部解析/生成（extended CONNECT）；`CONNECT` method 在 request 层的允许路径 |
| E3 | `src/http3/xqc_h3_request.{c,h}` | 服务端：收到带 `:protocol=webtransport` 的 CONNECT → 校验 settings → 回调应用层 → 200 响应；客户端：发起 extended CONNECT（demo 验证用） |
| E4 | `src/http3/xqc_h3_ext_bytestream.{c,h}` | WebTransport 变体：uni/bidi stream 首 4 字节 session_id 解析与发送前缀；与既有 bytestream 的差异点收敛（WebTransport 无 h3-ext 的 stream type 字节） |
| E5 | `src/http3/xqc_h3_ext_dgram.c` | WebTransport datagram：`quarter stream id` 前缀封装/解析 |
| E6 | 新 `include/xquic/xqc_webtransport.h` + `src/http3/xqc_webtransport.{c,h}` | WT Session 对象：`xqc_wt_session_t`（conn + session_id + 子流注册），对应用层暴露 connect 通知 / 流建立 / datagram 回调；会话关闭语义（CONNECT 流 FIN/错误码） |
| E7 | `moq/moq_transport/xqc_moq_stream_webtransport.c` + `xqc_moq_session.c` | 落地空桩：`XQC_MOQ_TRANSPORT_WEBTRANSPORT` 分支启用，`xqc_wt_*` 作为 MoQ 传输层；`xqc_moq_init_alpn` 支持 WebTransport 注册到 h3 层；浏览器场景子协议协商：`:protocol` + `Sec-WebTransport-Http3-Draft02` 头 + WT-Available-Protocols |
| E8 | `moq/demo/xqc_moq_gateway.c` | 接入 WT：ALPN 同时启 `moq-quic` 与 `h3`；自签证书 + sha256 摘要打印（浏览器 `serverCertificateHashes` 兼容） |
| E9 | xquic 内测试 | `xquic/tests/` 增 WebTransport/扩展 CONNECT 单测；`scripts/validate.sh build && scripts/validate.sh test` 全过（遵循 `harness/pipelines/dev-pipeline.md` 与 `CONTRIBUTING.md` 的 Nginx C 风格、80 列、单元测试双路径要求） |

**xquic fork 流程要求（AGENTS.md）**：开工前读 `harness/pipelines/dev-pipeline.md`；长任务先落 OpenSpec change（`openspec/`）；协议行为变更须在测试或实现注释引用 RFC/draft 章节（draft-ietf-webtrans-http3）；提交前跑 `xquic-pr-pre-review`。

**降级预案**：若 E3/E4 浏览器互操作受阻，备用通道 = h3-ext 专有路径（浏览器不可用）→ 本页 WebSocket 代理（记录为备选，不优先）。

### 阶段 F：Web 测试 demo（1.5 天）

| # | 文件 | 内容 |
|---|------|------|
| F1 | `apps/demos/moq_web/`（新目录） | 静态页：`index.html + js/moq-client.js + js/player.js`；极简 JS MoQ 客户端：WebTransport(h3) 连接 → client setup（版本 0xff000005、role/path 参数）→ 读 catalog track → subscribe video → 解析 LOC/groups/objects → WebCodecs `VideoDecoder`（`avc1`，annexb 输入）→ canvas 渲染；含 `serverCertificateHashes` 选项、连接/统计/错误面板 |
| F2 | `apps/demos/moq_web/README.md` + 启动脚本 | `bash run_demo.sh --stream openhd0`：起网关 → `python3 -m http.server`（或网关自带静态服务）→ 打印访问 URL 与证书指纹 |
| F3 | 证书生成脚本 | 自签 ECDSA P-256、有效期 ≤14 天（Chrome `serverCertificateHashes` 要求），网关启动时加载 |
| F4 | 端到端联调 | pi4_air(openhd0) 或本机测试源 → 浏览器出画；判据见 §6 |

**线格式对齐风险**：fork 的 MoQ 为私有格式（0xff000005 + LOC 变体），JS 端**按 fork 实际线格式实现**（先以 `moq_demo_client` 抓包核对），不依赖社区 moq-js 生态。

### 阶段 G：集成验证与文档（1 天）

| # | 内容 |
|---|------|
| G1 | 端到端回归：现有订阅路径（手机 QOpenHD / `pull_probe`）**零回归**（判据同 PROGRESS.md §4.10） |
| G2 | 自愈：网关/SFU 任一侧重启后 IPC 重连、track 重新发布；浏览器自动重订阅 |
| G3 | 性能：组帧路径 CPU（目标 <3% 单核@2.5Mbps）、IPC 延迟（<0.5ms/帧）、浏览器延迟实测 |
| G4 | 文档：`docs/PROGRESS.md` 新增章节；部署说明（systemd：sfu + gateway 双单元示例）；`apps/demos/moq_web/README.md` |
| G5 | 提交策略：SFU/组帧/IPC 一个提交；xquic 仓库独立提交（含 OpenSpec change）；demo 一个提交 |

---

## 4. 文件清单汇总

**新增**
- `rtc/libs/frame_protoc/src/frame_assembler.{h,cpp}`
- `rtc/server/sfu_server/FrameDispatcher.{h,cpp}`
- `rtc/common/sfu_protocol/FrameIpc.h`
- `rtc/libs/xquic/moq/demo/xqc_moq_gateway.c`（+ 可选 `xqc_moq_gateway_ipc.{h,c}`）
- `rtc/libs/xquic/include/xquic/xqc_webtransport.h`、`src/http3/xqc_webtransport.{c,h}`
- `rtc/libs/xquic/third_party/boringssl/`（gitignore）、`scripts/build_boringssl.sh`、`build.sh`
- `apps/demos/moq_web/`（index.html、js/、run_demo.sh、README.md、gen_cert.sh）

**修改**
- `rtc/server/sfu_server/SfuServer.{h,cpp}`（assembler 成员/回调/统计）
- `rtc/server/sfu_server/main.cpp`（`--frame-ipc`）
- `rtc/libs/xquic/src/http3/{xqc_h3_conn,xqc_h3_header,xqc_h3_request,xqc_h3_ext_bytestream,xqc_h3_ext_dgram}.{c,h}`（WT 支撑）
- `rtc/libs/xquic/moq/moq_transport/{xqc_moq_stream_webtransport.c,xqc_moq_session.c}`
- `rtc/libs/xquic/moq/demo/CMakeLists.txt`（`moq_gateway` 目标）
- `docs/PROGRESS.md`

**不动（红线）**
- 订阅者转发路径全部语义（`ForwardToSubscribers`、proxy FEC 过滤、NACK/PLI 节流）
- `xqc_moq_demo_server.c` / `xqc_moq_demo_client.c` 原样保留（网关为新文件，demo 保持可对跑回归）
- pi4_air / nano 运行中环境（依 PROGRESS.md §3 约束：未经确认不得停/重启、不得改流名）

---

## 5. 里程碑与顺序

```
A(0.5d) ──► B(1.5d) ──► C(1.5d) ──► D(2.5d) ──► E(3.5d) ──► F(1.5d) ──► G(1d)
                 │                        │            │
                 └─ C 出口: IPC 手测收帧    └─ D 出口:   └─ E 出口:
                                              demo_client    浏览器 WT 握手
                                              收真实帧
```

- A 可与 B/C 并行（demo 对跑不依赖 SFU 改动）。
- **D 结束即可锁定「moq-quic 客户端收真实帧」成果**（不依赖 WebTransport）。
- E 为最大不确定项，开工前做 15min 探针：抓 Chrome WebTransport 握手包核对 fork 差距清单。

---

## 6. 验收判据（可执行）

| 验收点 | 命令/方法 | 通过标准 |
|--------|----------|---------|
| A: demo 对跑 | `moq_demo_server` + `moq_demo_client -r sub -n 300`（自签证书） | client 持续收帧，PLI 后下一帧 KEY |
| B: 组帧 | SFU 日志 `[SFU-ASM]` 1s 行 | `assembled=30/s`（720p30）、`kbps≈2500`、`pending≤2`、无持续 timeout |
| B: 不影响订阅 | `pull_probe 127.0.0.1 9200 <stream> ws_probe 10` | 帧率/IDR/码率与改动前一致（309 帧/10s、IDR=10 基线） |
| C: IPC | `socat - UNIX-CONNECT:/run/tj_rtc/frames.sock` + 手发 `SUB` | 收到帧流：stream_id/frame_type/pts 正确、退订后停止 |
| D: MoQ 发布 | 网关 + `moq_demo_client -r sub` 订阅 | 收到**真实帧**：`video_len` 与源帧一致、`seq_num` 递增、KEY 对应源 IDR、PLI（client seq==3）触发上游 IDR 且 ~1s 内到达 |
| E: WT 握手 | Chrome console: `new WebTransport('https://127.0.0.1:<port>/moq', {serverCertificateHashes:[...]})` | `transport.ready` resolve，`datagrams` 可用 |
| E: WT 双向流 | demo 页 JS 读 catalog | 收到 catalog track 数据 |
| E: xquic 自检 | `cd rtc/libs/xquic && ./scripts/validate.sh build && ./scripts/validate.sh test` | 全绿 |
| F: 浏览器出画 | Chrome 打开 demo 页（openhd0 源） | 出画 30fps，`VideoDecoder` 无报错，延迟 < 1.5s（页面角标） |
| G: 回归 | 手机 QOpenHD 实机 + pi4_air 日志 | `video_frames +31/s`、`drop=0`，与 §4.10 判据一致 |

---

## 7. 风险与对策

| 风险 | 等级 | 对策 |
|------|------|------|
| xquic WebTransport 工作量/兼容性超预期 | 高 | D 结束即锁定「moq-quic 客户端可用」成果；E 失败则降级 WebSocket 代理（备选 F-alt，记录在案）；E 内部分步验收（SETTINGS → CONNECT → 流 → MoQ 层） |
| 网关改造触碰 demo 既有链路 | 中 | 网关为新文件，demo 原样保留作回归基准；D6 单测覆盖映射/扇出/断连 |
| LOC 容器与 JS 端解码对齐 | 中 | 以 fork 自身 `moq_demo_client` 抓包/解码为准实现 JS 端；pages 内做 hex dump 核对 |
| BoringSSL 构建环境问题（无网/慢） | 中 | 预取 tarball 缓存；失败可换 Tongsuo(babassl) 路径（xquic 原生支持） |
| 组帧双路径 CPU/内存增长 | 低 | 帧组装仅 UDP 推流会话启用；`max_pending_frames` 与 proxy 同量级；G3 量化验收 |
| 修改 SFU 触碰稳定路径 | 中 | B2 只「并行投喂」不改 proxy 行为；G1 回归门禁必须通过后才合并 |
| xquic fork 规范（OpenSpec/单测/PR 流程）额外开销 | 低 | 开工前读 `harness/pipelines/dev-pipeline.md`，把 E9 测试纳入 E 工时 |

---

## 8. 工期总览

| 阶段 | 内容 | 人日 |
|------|------|------|
| A | xquic 构建落地 + demo 对跑 | 0.5 |
| B | SFU 组帧 | 1.5 |
| C | IPC + FrameDispatcher | 1.5 |
| D | MoQ 网关（demo 改造） | 2.5 |
| E | xquic WebTransport | 3.5 |
| F | Web demo | 1.5 |
| G | 集成验证与文档 | 1.0 |
| **合计** | | **12** |

（单人全职估算；A+B+C+D 约 6 天可交付首个「真实帧 → MoQ 客户端收流」验证链路。）

---

## 9. 待决事项

1. ~~MoQ 集成形态~~ → 已定：独立进程网关 + IPC。
2. ~~网关底座~~ → 已定：复用 `xqc_moq_demo_server.c` 改造为新目标 `moq_gateway`。
3. ~~Web 传输方式~~ → 已定：先补齐 WebTransport。
4. 音频：v1 仅视频；demo 的 audio track 标注 opus，而我方源为 PCM——二期选定「PCM 直通+WebAudio（改 mime 标注）」或「网关转 Opus」。
5. WebTransport demo 证书分发：`serverCertificateHashes`（无 CA，浏览器专用）已纳入；真域名部署再评估 Let's Encrypt。
6. 多流 v1 范围：`--stream` 白名单单流先行，多流在 D5 验证后放开。
7. MoQ 命名规范确认：namespace `tj/<streamId>`、track `video`（audio 二期）。
8. **标准差距记录**：fork MoQ 为 pre-standard（`0xff000005`，不兼容 IETF draft-ietf-moq-transport），本管线为自闭环（JS 端随 fork 线格式实现）；若后续需社区互操作/标准播放器生态，再单独立项对齐（工作量参考 v2 §0 评估：moq/ 模块整体重写级）。
9. IPC 安全：本机 UDS 无鉴权，默认路径 `/run/tj_rtc/frames.sock` 权限 700；如后续需要跨机分离部署再补鉴权。