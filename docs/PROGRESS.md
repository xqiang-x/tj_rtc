# OpenHD RTC 链路 — 进展与现场记录

> 最后更新：2026-10-10
> 用途：记录当前成果、现场环境与待办，供下次继续。协议/架构细节见 `openhd_rtc_link_design.md`（设计文档）。
> 规则：每到重大节点（环境切换、修复合并、联调结果）更新本文件。

> **目录重组（2026-10-10）**：仓库按「RTC 核心 vs 使用方」分离——
> `rtc/`（client_sdk / common / libs / server 核心）、`apps/`（demos + OpenHD/QOpenHD 子模块）、
> `docs/`（本文件、bug.md、设计文档）。旧路径映射：`demos/x` → `apps/demos/x`、
> `client_sdk|common|libs|server` → `rtc/` 下同名目录、`OpenHD|QOpenHD` → `apps/` 下。
> 本文件 §4 各条为历史记录，仍用旧路径；**pi4 / nano 上的 `~/tj_rtc` 副本仍是旧布局**
> （未重新同步，运行中的进程不受影响），下次同步后远端路径也变为 `apps/demos/…`。
> Android 构建树 `~/qopenhd_android_build` 因 QOpenHD 源码路径变化需重新 qmake 一次。

## 1. 一句话现状

手机端 QOpenHD 经 tj_rtc 链路（SFU 106.15.177.248）显示视频正常（Plan B YUV 渲染）；
上行 MAVLink 已打通（手机 → RTC ctrl 可靠 TCP → air）；Redmi Note 8 Pro 与另一台手机
均已实机验证出画。三个仓库全部提交到 `feature/wq` 分支（**未推送**）。

pi4 专用 air 程序 `apps/demos/pi4_air`（pi4 上副本仍在旧路径 `demos/pi4_air`）已完成并**切换上线**（2026-09-23）：本机 local air +
gst 源已停，**pi4_air 正在 pi4（192.168.50.114）运行并推 openhd0**，手机（Redmi）重启
app 后已出画（解码 31fps、`yuv upload glErr=0`、链路 loss 0%）。

## 2. 代码提交状态（feature/wq，均未推送 GitHub）

| 仓库 | HEAD | 内容 | 基线 |
|---|---|---|---|
| tj_rtc | `386002a` | 子模块登记 | — |
| tj_rtc | `68a68a2` | 链路设计文档（openhd_rtc_link_design.md） | — |
| tj_rtc | `c166344` | Subscriber：UDP 模式自愈 TCP 可靠 ctrl | main `4902d84` |
| tj_rtc | `ac79d2a` | RECV_REPORT 收包时间回报链路（带宽估计脚手架，算法待实现） | — |
| tj_rtc | `86d46cf` | 新增 demos/pi4_air（pi4 专用 air 端） | — |
| tj_rtc | `6c86652` | 新增 demos/jetson_camera_push（Jetson NVENC 推流） | — |
| tj_rtc | （本次重组提交） | 目录重组：rtc/ + apps/ + docs/ 分离（纯移动 + 路径适配） | — |
| OpenHD | `eca5ace2` | air 端 RTC 链路（RtcLink/RTP-ES/rtc 配置/探针） | `f07729b3` |
| QOpenHD | `3d97fea3d` | 客户端接入（app/rtc、Plan B YUV、上行 MAVLink） | `6dd753d73` |

- 子模块（tj_rtc `.gitmodules`，重组后位于 `apps/` 下）：
  - `apps/OpenHD` → https://github.com/xqiang-x/OpenHD.git @ `eca5ace2`（heads/feature/wq）
  - `apps/QOpenHD` → https://github.com/xqiang-x/QOpenHD.git @ `3d97fea3d`（heads/feature/wq）
- **推送顺序（重要）**：OpenHD → QOpenHD → tj_rtc。子模块 commit 未推的话，
  别人克隆 tj_rtc 时 `submodule update` 会找不到提交。
- **子模块内未提交（目录重组适配，2026-10-10）**：`apps/QOpenHD/app/rtc/rtc.pri`
  （加上本就有的 `RtcClient.cpp` 看门狗改动）、`apps/OpenHD` 的 `ohd_interface/CMakeLists.txt`
  注释、`test/rtc_probe/CMakeLists.txt`、`test/rtc_probe/smoke/build.sh`、
  `test/rtc_probe/l3_air_openhd/run_ohd_air.sh` —— 均留在各自工作区待随 feature/wq 提交。

## 3. 当前测试环境（改动前必读）

| 项 | 值 |
|---|---|
| SFU 服务器 | 106.15.177.248，TCP 9200（信令）/ UDP 9201（媒体） |
| 视频源 | **pi4 上的 `pi4_air`**（本机 local air 与 gst 源已于 2026-09-23 停止） |
| 手机（验证机） | Redmi Note 8 Pro（MTK Helio G90T），adb 序列号 `7ljbivaa4xlvrgxg`，包名 `com.openhd.qopenhd` |
| 另一台手机 | 装 `QOpenHD_v24_20260923_112757.apk` 后界面正常（型号未记录） |
| pi4 | `192.168.50.114`（用户 pi）**正在运行 pi4_air**（stream=openhd0 / user=pi4_air；pid 见 `~/tj_rtc/pi4_air.pid`，日志 `~/tj_rtc/pi4_air.log`）；ov5647 相机 720p@30 出画正常；Pixhawk 未插（`serial=无`，热插拔等待中）；`~/tj_rtc` 源码为**重组前布局**的同步副本（下次 rsync 后变为 `rtc/`+`apps/` 新布局）；旧 `raspberry_pi_pusher`(stream=pivideo) 曾连续跑 21h（见 `bug.md` Bug 1） |

`rtc_local_air`（本机替身 air 端，源码 `/tmp/rtc_local_air/rtc_local_air.cpp`）：

```
拓扑：gst(rtph264pay) --UDP:5600--> [RtpEsAssembler] --AU--> Publisher --SFU--> 手机
      /dev/ttyACM0(Pixhawk) --MAVLink--> ctrl 下行 --> 手机 --> ctrl 上行 --> 写回串口
构建：cmake -S /tmp/rtc_local_air -B /tmp/rtc_local_air/build && cmake --build /tmp/rtc_local_air/build -j
运行：/tmp/rtc_local_air/build/rtc_local_air --stream openhd0 --user local_air
```

> **⚠ 约束（下次继续务必遵守）**
> - 当前实时测试环境 = **pi4_air（pi4）+ 手机 QOpenHD**：未经用户确认，不得停止/重启
>   pi4_air、不得改流名。
> - 本机 `rtc_local_air` 与 gst 源**已停**（2026-09-23 用户确认切换）；需切回时先停 pi4_air。
> - 手机侧调试（adb 重启/安装 app）允许。

**air 切换操作要点（2026-09-23 实测）**：

1. 先停一端：`pkill -TERM -f rtc_local_air`（gst 源一并停；pi4 端停止用 `kill $(cat ~/tj_rtc/pi4_air.pid)`）。
2. 启动另一端后，SFU 释放旧 stream 注册需 ~15s：日志出现 `订阅失败: STREAM_EXISTS` 并
   自动 1s 重试，**属正常，勿手动干预**（实测重试 13 次后 `CONNECTED sessionId=3277`）。
3. **手机 app 必须重启**才能重订阅新 air（否则 `connected=true` 但
   `video_frames (+0/s)` 冻结在旧推流端）：
   `adb shell am force-stop com.openhd.qopenhd && adb shell monkey -p com.openhd.qopenhd -c android.intent.category.LAUNCHER 1`
4. 判据：pi4 日志 `pli` 出现且增长（订阅端请求关键帧）、kbps≈2500、drop=0；
   手机 `video_frames (+31/s)`、`yuv upload ... glErr=0`。

pi4_air 启动命令（pi4 上；pi 端副本仍是旧布局，重新同步后路径变为 `~/tj_rtc/apps/demos/pi4_air/build`）：

```bash
cd ~/tj_rtc/demos/pi4_air/build
nohup setsid ./pi4_air --stream openhd0 --user pi4_air > ~/tj_rtc/pi4_air.log 2>&1 < /dev/null &
```

## 4. 已完成修复与验证（附判据）

### 4.1 手机视频黑屏 → Plan B（QOpenHD `3d97fea3d`）

- 根因：MTK（及部分平台）硬解走 Surface→SurfaceTexture 外部纹理路径采样全黑/故障。
- 方案：`AMediaCodec` 配置 `nullptr` Surface（ByteBuffer 输出）→ 归一化紧致 NV12
  （colorFormat 19=I420）→ `YuvFramePtr` 回调 → `QSurfaceTexture` 互斥暂存最新帧 →
  `updatePaintNode` 上传 Y(`GL_LUMINANCE`)/UV(`GL_LUMINANCE_ALPHA`) 双纹理 → 片元着色器 BT.601。
- 判据：logcat `QSurfaceTexture: yuv textures created ... ok= true`、`yuv upload#N glErr= 0`；
  真机 1280×720@30fps 出画。
- 回退开关（诊断用）：`adb shell setprop debug.qopenhd.surfacevideo 1` → legacy Surface 模式
  （默认 YUV 模式；另有 `debug.qopenhd.videodiag 0-6` 渲染诊断模式）。

### 4.2 显示方向 = UPRIGHT（不翻转）

- 源轨迹 Lissajous 模型（RMS 0.23px）+ 视频矩形实测修正 **[210..2130]×[0..1080]**（缩放 1.5，
  球径实测 55-58px 印证）。
- 手机轨迹对源模型 τ 全局拟合：**UPRIGHT err=84.0px vs FLIPPED err=144.7px**；
  早期几何锚定法结论一致。**结论：保持不翻转，勿再折腾**。

### 4.3 上行 MAVLink（手机 → air）→ RTC ctrl 可靠通道

- 实测：对该推流端会话 UDP(不可靠) 上行 0 交付，TCP(reliable) 上行 15/15 全达
  （代码注释亦记录，见 Subscriber::EnsureCtrlTcp）。
- 修复：`EnsureCtrlTcp`（UDP 模式建立/重建自愈 TCP 信令连接，保活 tick 幂等重建）+
  发送回落 UDP（通道未就绪时不再静默丢包）+ 提前记录下游广播 session。
- 判据：air 端 `mav_rx` 计数持续增长（实测 28 → 40 → 52）。

### 4.4 另一台手机「界面全红 + 文字白块」（2026-09-23 修复）

- 根因 A：视频层全屏，且解码纹理未就绪时采样未定义纹理 → 部分驱动返回整块异常颜色（红屏）。
- 根因 B：材质 `updateState` 把 GL 活跃纹理单元留在 2/3 未恢复 → 后续 QML 文字/图片
  纹理绑错单元 → 文字变白色块（MTK 驱动宽容未暴露，换机暴露）。
- 修复：纹理未就绪/创建失败时**不绘制**（`m_yuvTexOk` 判定 + 失败日志）+ 恢复 `glActiveTexture(GL_TEXTURE0)`。
- 判据：用户在新包（`QOpenHD_v24_20260923_112757.apk`）上确认界面正常；MTK 回归无影响。

### 4.5 LowLagDecoder 解码错误恢复

- 解码器报错后无需重启 app 即可重建；代码完成并部署，现场未再触发错误。

### 4.6 已排除的「伪问题」（勿重复排查）

- 录屏中球周期性冻结/消失 = MIUI/MTK `screenrecord` 采集伪影。
  证据：用户目视实时画面正常流畅；app 遥测 frameSwapped 52-64Hz、paint 60/s、上传 30fps 健康；
  gst 源（`videotestsrc pattern=ball`，纯黑底白球）300 帧内球从未缺席。
- 192.168.2.1:5760 每 3 秒重试 Connection refused = air 端未监听该地址，无害。
- MTK codec 服务 `CCDst` 错误刷屏 = 系统级噪声，无害。
- 录屏中「4 行条块」= app 自带 OSD 元素（带淡入淡出），非视频内容。

### 4.7 pi4 专用 air 程序（`apps/demos/pi4_air`，2026-09-23 完成编译）

- 结构 = `raspberry_pi_pusher` 视频管线（libcamera → V4L2 硬编 → NaluSplitter → Publisher，
  复用其 `src/` 组件不重复维护）+ `rtc_local_air` 的 MAVLink 桥（串口分帧/统计/上行写回）。
- 新增：**串口热插拔**（Pixhawk 未插照常推流，插入 ~1s 自动挂接，拔出自动摘除）；
  合并两套 CLI；1s 统计行（enc/aus/key/kbps/drop / mav_tx/mav_rx/up_write/up_drop/resync / pli / serial）。
- 默认与手机对齐：stream=openhd0、user=pi4_air、720p@30、2500kbps、gop=30、pacer 20Mbps。
- 判据（2026-09-23 首跑通过）：pi4 日志 `CONNECTED sessionId=3277`、enc 30fps、kbps≈2500、
  drop=0、`[Push-PeerStats] loss=0%`；手机重启 app 后 `video_frames (+31/s)`、`idr` 增长、
  `yuv upload# size=1280x720 glErr=0`；`pli=1`（订阅端加入请求关键帧）。**Pixhawk 串口桥待插上后验证**。
- 附带：pi4 `~/tj_rtc` 的 client_sdk/common/libs（重组后本地对应 `rtc/…`）已向前同步到 feature/wq 状态
  （含 TCP 帧长溢出防护、NACK 状态防爆、Subscriber 自愈 TCP 等修复；pi 端无本地改动被覆盖）。

### 4.8 多客户端同 user_id → SFU 会话劫持（2026-09-23 定位，Redmi 冻结根因）

- 现象：17:58:58 另一台手机的 app 启动订阅（默认 user=`qopenhd`、stream=openhd0、UDP）的同一刻，
  Redmi 画面当场冻结（`video_frames` 停增、`connected=true` 但永不恢复，重启 app 才恢复）。
- 根因（SFU `rtc/server/sfu_server/SfuServer.cpp`）：
  - `CreateSession` 对「同 user+stream 的既有 UDP 会话」做**幂等复用**（本意：订阅请求丢包重发时
    不误踢正在用的会话）→ 日志 `User %s re-subscribe ... reusing session %u`。
  - 订阅路径随后把该复用会话的 `udp_client_addr` **无校验改写**为新请求源地址（SfuServer.cpp:907-911）。
  - `OnUdpPacketReceived`（SfuServer.cpp:652）同样按**包内 sessionId** 定位会话并用源地址改写地址。
  - 结果：两台设备只要 user+stream 相同就被当成「同一客户端」→ 后加入者夺走会话与媒体目标地址，
    先加入者静默断流（无踢出/错误通知）；其保活心跳因 recv 阻塞收不到流而停发，无法抢回地址
    → **永久冻结，只能重启 app**。
- 实证（SFU 服务器 INFO 日志 + pi4/手机日志对齐，会话 3279）：
  - `17:44:56` 创建（Redmi，115.205.228.86:17878）→ `17:58:58 reusing session 3279`（另一台手机，
    140.205.11.21:10927；同刻 Redmi 冻结 + pi4 `pli=2`）→ `18:20:47` 本机同参数复测
    （20s 收 620 帧、IDR 1/s、丢包 0，证明**后加入者即刻可看**）→ `18:22:51` Redmi 重启 app
    复用同一会话后恢复（+31/s）。
- **规避（无需改 SFU）**：两台设备各用**不同 user id**（设置→视频设置 `qopenhd_rtc_user_id`，
  另一台改成如 `qopenhd2`）→ 各自独立会话，可同时观看。
- **待修（SFU 侧，部署需确认）**：`CreateSession` 复用加「源地址一致」校验；订阅路径地址改写仅限
  同一客户端；subscriber 不因同 user 互踢。修复后需重编 `sfu_server_full` 替换
  `106.15.177.248:/root/bin/` 并重启（会短时中断推拉流）。
- 复现工具：`/tmp/reuse_test`（本机最小订阅端，参数与 app 一致，**/tmp 易失可重写**）。
- 相关：SFU 服务器有免密 ssh（`ssh sh` ≡ root@106.15.177.248），活动日志
  `/root/log/sfu_server_full.*.INFO.*`（fd 3），`sfu_mon.log` 为进程内存监控。

### 4.9 订阅端媒体路径 CGNAT 静默失效（2026-09-23）→ RtcClient 媒体失活看门狗

- 现象：Redmi 上 MAVLink（ctrl/TCP）完全正常（`ctrl_rx` +19/s）但视频永久卡死：
  `19:41:04` 起 `video_frames` 冻结于 73710（同步 `LowLagDecoder out-frames=71324`、
  `idr` 停增），此前有「卡死 ~2min → 短暂恢复 ~20s（+31/s）→ 再卡死」循环，
  最终连续冻结 **9m45s**（至 19:50:49 用户手动重启 app 才恢复）。
- 排除项：非 4.8 会话劫持（SFU 无任何 re-subscribe 事件）；非推流端（pi4 全程
  `enc=30fps kbps≈2500 drop=0`、`[Push-PeerStats] loss=0%`）；非 SFU 转发故障
  （`bytes_fwd` 持续增长，10s ≈3.2MB）；非解码器（收不到帧，非解不动）。
- 证据链（SFU 日志 + SFU 上 tcpdump + 手机 logcat）：
  - 订阅会话 3279 自 `18:22:51` 登记 `115.205.228.86:19150` 后，日志期内 SFU **无一条
    `[SFU-NAT]` 地址跟随记录**——登记地址从未变化；SFU 持续向 19150 发媒体。
  - tcpdump：入向只见 `:18235`（pi4 媒体）与 `:19150` 的零星 37B 小包（订阅端
    PLI/NACK 仍从**同一端口**上行）；出向持续写 `:19150`。
  - 订阅端 PLI ~2/s 持续到达并被转发（`[SFU-PLI] forwarded upstream total=1486→1773`，
    pi4 `pli=1766`）→ **出向（UDP）活、入向（UDP）死**，且 SFU 目标地址=上行源端口
    仍收不到媒体 → 判定为 CGNAT **入向映射静默黑洞**（非对称失效，运营商侧）。
  - 恢复：19:50:49 重启 app → 新订阅携带新端口 `:17670` → 立即 +31/s。
    心跳（6B）与 PLI 都在到达，故服务端无法感知、心跳保活类自愈无解：必须重新映射。
- 修复（客户端侧，无需改 SFU）：`apps/QOpenHD/app/rtc/RtcClient.cpp` supervisor_loop 增加
  **媒体失活看门狗**：`connected && video_frames>0 && 连续 5s 无新帧` →
  `m_needs_restart=true` → 重建订阅（换新本地 UDP 端口 → 刷新 NAT 映射）；日志
  `RtcClient: no video frames for 5s while connected, forcing re-subscribe to refresh NAT mapping`。
  最长卡死从「无限」收敛到 ~5-7s。对照：推流端有 PONG 超时（10s）重连自愈，订阅端此前没有。
- 产物：`QOpenHD_v24_20260923_195519.apk`（已装 Redmi）。**RtcClient.cpp 改动未提交**
  （随 QOpenHD 仓库待提交）。
- 看门狗实测（09-23 20:00-20:02，装包后）：环境侧入向黑洞**每 ~30-60s 复发一次**
  （20:00:19、20:01:33 等），看门狗**每次自动恢复**：`restarts` 1→6、重建即换新本地端口
  （18790→18879→18928→18947→19047…）、重订阅后立即 +31/s；20:01:33 后持续稳定
  （+31/s、`ctrl_rx` +50/s）。**验证通过：不再需要手动重启 app**。
- 环境侧建议（看门狗仅为兜底，根治在用户侧网络）：
  - Redmi 切 5G/其他网络观察 10 分钟：若不再复发 ⇒ 家用路由器/宽带侧 UDP 防护
    （UDP flood 保护 / ALG 等）嫌疑，查路由器设置；
  - 临时绕过：app 设置 `qopenhd_rtc_use_udp=false` → 媒体走 TCP 通道，完全绕开
    UDP 入向黑洞（代价：延迟略增）。
- 未解释：恢复前的「~230/s 追帧突发」段（累计数千帧）机制未明（部分退化期现象）；
  看门狗只收敛冻结时长。
- 隐患记录（顺手发现，未处理）：订阅端 PLI/NACK 以**推流端 sessionId** 包装上行
  （`Subscriber.cpp:131 SerializeStreamData(m_sourceSessionId,…)`）→ 命中 SFU
  `OnUdpPacketReceived(3277,…)`，可能把**推流会话的 UDP 地址改写为订阅端地址**；
  当前 PONG/NACK/PLI 回传均走 TCP（`Publisher.cpp:616` 注释）故无实际影响，
  但属 4.8 同一地址改写路径的潜在误伤，修 4.8 时应一并收紧。

### 4.10 Jetson Nano 专用推流 demo（`apps/demos/jetson_camera_push`，2026-10-09）

- 目标：nano（192.168.50.75，SSH 别名 `nano`）CSI 摄像头 → **NVENC 硬编 H.264** → 推流
  `jetsonvideo`（user=jetson_nano）→ SFU 106.15.177.248。
- 链路：`nvarguscamerasrc`(Argus，1080p30，NV12/NVMM) → `nvvidconv`(缩放 720p，I420/NVMM)
  → `nvv4l2h264enc`(2500kbps，insert-sps-pps，GOP 30) → `h264parse`(byte-stream/au)
  → `appsink` → 复用 `NaluSplitter` → `Publisher`。SDK/协议栈与 pi4_air 同源。
- 为什么走 Argus 而非 v4l2 直采：`/dev/video0` = CSI IMX219；本机 v4l2 直采
  （`nvv4l2camerasrc`/`v4l2src`）只有 4K@21 出帧，其余模式协商过但无帧；Argus 6 种模式全可用。
- PLI/关键帧：本版 `nvv4l2h264enc` 的 `idrinterval` 仅 NULL/READY 可设，**无运行时 force-idr**
  → PLI 只计数，IDR 由 GOP 周期（≤1s）满足（与 pi4_air 行为一致）。
- 环境改造：nano 系统 cmake 3.10.2 太旧（SDK 需 ≥3.14），装 `~/tools/cmake-3.28.3`
  （pypi manylinux2014_aarch64 wheel 解出，sha256 已验；官方 aarch64 二进制下载慢时的替代）。
- 验证判据（2026-10-09 通过）：nano 日志 `CONNECTED sessionId=3906`、`fps=31 kbps≈2450；
  `key` 按 GOP 递增；本机 `pull_probe 106.15.177.248 9200 jetsonvideo ws_probe 10` →
  309 帧/10s（≈31fps）、IDR=10、params=10、≈2.48Mbps、无异常小帧；定长 150s 收尾干净
  （au=4639 key=150 param=150 bytes≈46.7MB pli=2）。
- 当前状态：**nano 上已后台常驻**（`~/tj_rtc/jetson_push.pid`、日志 `~/tj_rtc/jetson_push.log`，
  pid 10232，普涨 au/fps=31）；pi4_air/openhd0 **未动**（遵守 §3 约束）。
- 已提交：`apps/demos/jetson_camera_push/`（tj_rtc `6c86652`；nano 端无 git 仅 rsync 部署，
  nano 副本仍在旧路径 `~/tj_rtc/demos/jetson_camera_push`，重新同步后变为 `apps/demos/…`）。

### 4.11 Jetson Nano GPIO / PWM 能力（2026-10-09 实测）

- **GPIO**：`/sys/class/gpio` 可用；pin 7（sysfs 216，GPIO09）实测 export→out→1→0→unexport 通过。
  `pi` 在 `gpio` 组（export/unexport 免 sudo），但导出后的 `direction/value` 为 root 属主 → 需
  sudo（`sudo -n` 免密已配置）；装了 jetson-gpio udev rules 后可全免 sudo。`Jetson.GPIO`
  python 库已安装（`import Jetson.GPIO` 可用）。40-pin 常用映射：pin7=216、pin29=149、pin31=200、
  pin32=168、pin33=38、pin15=194、pin11=50、pin13=14、pin18=15、pin22=13、pin26=20。
- **PWM**：40-pin 仅 pin 32 / pin 33 两路，对应 `pwmchip0`（7000a000.pwm）ch0 / ch2；
  实测 export→period 1ms→duty 0.5ms→enable 双通道同时 OK。jetson-io 官方函数表亦为
  `pwm0(32) / pwm2(33)`（其余可选：aud_mclk/i2s4/spi1/spi2/uartb）。
- **⚠ 占用（勿动）**：`pwmchip0` ch1 与 `pwmchip4`（70110000）ch0 = vdd-cpu / vdd-gpu
  调压器；`pwmchip0` ch3 = pwm-fan（风扇，PE7/hog pwm3）。可自由用仅 ch0/ch2。
- 用法要点：全部 root 属主需 sudo；servo 50Hz 例：period=20000000ns、duty=1000000~2000000ns；
  LED 呼吸/etc 任意。物理层未接示波器/负载验证（驱动层已验证），引脚功能如需重配走
  `jetson-io.py`（改后需重启）。其他 header 功能（i2c-1、spi、uart）未逐一实测。

### 4.12 nano 上既有软件勘察：DonkeyCar 小车框架（2026-10-09，只读勘察）

- `~/projects/donkeycar`（autorope/donkeycar git @ 97251b17 ≈v4.5.1，setup.py 有本地
  numpy pin 补丁）+ `~/env` venv（donkeycar 4.5.1 editable、**TensorFlow 2.5.0+nv21.8**）
  + `~/mycar` 车 app（manage/train/calibrate/config/myconfig，`donkey` CLI 可用）。
- **当前为初始状态**：myconfig.py 无任何有效配置（纯模板注释）；data/ / models/ / logs/
  全空；i2c-1 无 PCA9685、无 USB 外设（仅 USB hub）、无自启服务/进程 → 硬件未接、未采数据、未训练。
- 功能面（框架自带，已装）：web 遥控(:8887)/手柄(/dev/input/js0) + 边开边录 tub 数据、
  `donkey train` / `donkey ui` Keras 训练、local/local_angle 自动驾驶模式、
  `donkey calibrate` PWM 标定；驱动模板覆盖 PWM_STEERING_THROTTLE（config 默认生效）/
  L298N DC / H 桥 / VESC / 里程计 / LIDAR 等；paho-mqtt、jtop 已装。
- ⚠ 若日后跑车：DonkeyCar 的 CSIC 相机与 `jetson_camera_push` 争用同一 CSI（Argus 独占），
  二者不能同时开；PWM 驱动走 Jetson.GPIO/40-pin pin32/33（见 4.11）。
- 入口：`source ~/env/bin/activate && cd ~/mycar && python manage.py drive`。

## 5. 构建 / 安装 / 调试速查

- 构建脚本：`bash /tmp/rebuild_rtc.sh`（增量 make + install + androiddeployqt；
  `DEPLOY_RC=20` 属正常，为 post-gradle zipalign 返回值；全文见附录 B，**注意 /tmp 易失**）
- 构建树：`/home/ubuntu/qopenhd_android_build/`
- ⚠ 目录重组后（2026-10-10）：QOpenHD 源码在 `apps/QOpenHD`、SDK 在 `rtc/`，
  首次构建需重新 qmake（用新路径的 QOpenHD.pro），之后 `rebuild_rtc.sh` 增量 make 照常。
- APK 输出：`/home/ubuntu/qopenhd_android_build/android/build/outputs/apk/debug/`
  - **当前安装（Redmi，含 4.9 看门狗）**：`QOpenHD_v24_20260923_195519.apk`
  - 上一个：`QOpenHD_v24_20260923_112757.apk`（= QOpenHD @ `3d97fea3d`）
- adb：`export PATH=/home/ubuntu/Android/Sdk/platform-tools:$PATH`
- 安装/重启：`adb -s 7ljbivaa4xlvrgxg install -r <apk>`；启动：
  `adb shell monkey -p com.openhd.qopenhd -c android.intent.category.LAUNCHER 1`
- 关键日志：`adb logcat -d | grep -E "QSurfaceTexture|yuv|shader linked"`；
  错误面：`grep -iE "libEGL|AndroidRuntime|QOpenGLShader|failed"`
- 保持常亮（防锁屏中断测试）：`adb shell svc power stayon true`

## 6. 下次继续（待办）

1. **推送 GitHub**（顺序 OpenHD → QOpenHD → tj_rtc），推送后核对 `.gitmodules` URL 可克隆性。
   待推新增：tj_rtc 的 `ac79d2a`（RECV_REPORT）、`86d46cf`（pi4_air）、`6c86652`（jetson）、
   目录重组提交；子模块内目录适配改动建议先在各自 feature/wq 提交再推。
2. **pi4 air 已首跑出画（2026-09-23）**；剩余：**Pixhawk 插到 pi 后验证串口桥**
   （pi4 日志 `serial=OK`、`mav_tx/hb` 增长；手机上行 `up_write` 增长）；长稳观察。
3. **现场联调**：真实相机源（HDMI/CSI）+ 延迟/丢包观察；复核 `bug.md` Bug 1
   （推流端断线永不重连）在 RtcLink 下的行为。
4. **多客户端同 user 会话劫持（4.8）**：
   - 短期规避：另一台手机改 `qopenhd_rtc_user_id`（如 `qopenhd2`）后重启 app（两台可同时看）。
   - 根治：改 SFU `CreateSession` 复用校验（源地址一致才复用）+ 订阅路径地址改写收紧；
     重编 `sfu_server_full` 部署到 106.15.177.248（**需用户确认后执行**）。
   - 可考虑 QOpenHD 默认 user id 按设备唯一化（减少默认配置撞车）。
5. **/tmp 关键资产留档**：`/tmp/rtc_local_air/`（替身 air 源码）、`/tmp/rebuild_rtc.sh`
   —— 均在 /tmp，重启易失；建议归档进仓库或另行备份。
6. 真实环境长时间稳定性观察（解码错误恢复、RTC 重连自愈）。**4.9 看门狗已实测通过**
   （09-23 装包后 `restarts` 1→6、每次 ~5-7s 自动恢复，logcat 见
   `no video frames for 5s ... forcing re-subscribe`）。剩余：**环境侧排查入向 UDP 黑洞**
   （手机切 5G/其他网络验证，见 4.9；必要时评估改走 TCP 媒体 `qopenhd_rtc_use_udp=false`）。


## 附录 A：当前 gst 测试源命令（本机 127.0.0.1:5600）

```bash
gst-launch-1.0 -q videotestsrc is-live=true pattern=ball ! \
  video/x-raw,width=1280,height=720,framerate=30/1,format=I420 ! \
  x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 bitrate=4000 \
          threads=1 sliced-threads=false ! \
  rtph264pay pt=96 config-interval=1 ! \
  udpsink host=127.0.0.1 port=5600
```

## 附录 B：rebuild_rtc.sh 全文（用于 /tmp 丢失后重建）

```bash
#!/bin/bash
set -e
BUILD=/home/ubuntu/qopenhd_android_build
cd "$BUILD"
export ANDROID_NDK_ROOT=/home/ubuntu/Android/Sdk/ndk/21.4.7075529
export ANDROID_NDK_HOME=$ANDROID_NDK_ROOT
export ANDROID_SDK_ROOT=/home/ubuntu/Android/Sdk
export JAVA_HOME=/usr/lib/jvm/java-11-openjdk-amd64
export PATH=/home/ubuntu/Qt/5.15.2/android/bin:$PATH

echo "=== make (incremental) ==="
make -j3
echo "=== make install ==="
make -j3 install INSTALL_ROOT="$BUILD/android"
echo "=== androiddeployqt (post-gradle zipalign may rc!=0, expected) ==="
set +e
androiddeployqt --output android --input android-QOpenHD-deployment-settings.json \
  --android-platform android-34 --gradle --apk --release
echo "DEPLOY_RC=$?"
set -e
echo "=== outputs ==="
find "$BUILD/android/build/outputs/apk" -name "*.apk" -newermt "-10 minutes" -exec ls -la {} \;
echo "REBUILD_DONE"
```