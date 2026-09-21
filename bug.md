# Bug 记录

## Bug 1：推流端在服务端会话超时后永不重连（盲发 3.5 小时）

**状态**：待修复 | **发现日期**：2026-09-01 | **影响范围**：所有使用 rtc_client_sdk UDP 推流的端（pi4 raspberry_pi_pusher 已实测复现）

### 现象

- 推流进程（pi4 `raspberry_pi_pusher`）进程存活、以 30fps 持续发包、TCP 连接 ESTAB，表面一切正常；
- 但服务端（sh 服务器 `sfu_server_full`）早已把该会话超时踢下线，收到的包全部丢弃，拉流端断流数小时；
- 服务端日志被 `[SFU-ERROR] Unknown session: 1002` 刷屏（实测 3,430,071 条，sfu.log 膨胀至 712MB+）；
- 拉流端反复订阅 `Stream 'pivideo' not found` 被拒。

### 实测时间线（2026-09-01）

```
18:21:44   pi4 与服务器间网络中断约 31s（session 1002 pi_pusher 与 1004 android_pull 同时进入 idle）
18:22:15   [INFO] Session 1002 timeout (user=pi_pusher, idle=31s, type=UDP)
18:22:15   [SFU-STREAM] Publisher session 1002 offline, stream 'pivideo' ends
18:22:15   [SFU-STREAM] Stream 'pivideo' removed (publisher offline)
18:22:15 ~ 21:54   pi4 持续 30fps 发包，服务端全部丢弃，刷 343 万条 Unknown session 错误
19:04 / 21:42 / 21:51   android_pull 订阅 pivideo 三次被拒（Stream not found）
21:54   手动 systemctl restart pivideo-pusher 后恢复（新 session 1008）
```

### 根因分析（断线检测链全线失效）

客户端断线判定在 `client_sdk/rtc_client_sdk/src/Publisher.cpp:270-277` 的 `linkDown()`：

```cpp
auto linkDown = [this, &nowUs]() -> bool {
    if (m_ctrlLinkDown.load()) return true;          // 条件1：控制信令 TCP 断开
    if (!m_useUdp) return false;
    if (m_udpSendFailCount.load() >= kUdpSendFailThreshold) return true;  // 条件2：sendto 连续失败
    if (m_frameSender && m_frameSender->hasReceivedPong() &&              // 条件3：PONG 超时
        nowUs() - m_frameSender->lastPongTimeUs() > kPongTimeoutUs) return true;  // 10s
    return false;
};
```

四个条件在本次故障中全部不触发：

1. **条件 3（PONG 超时）被前置条件永久禁用——主因**
   - 服务端 `libs/frame_protoc/src/proxy_receiver.cpp:480-497` `ProxyFrameReceiver::onUpstreamPacket()`：
     `MsgType::DATA` 走缓存；**非 DATA 包（含 PING）直接 `sendPacket` 原样转发给下游，从不回 PONG 给推流端**。
   - 推流端每 1s 发一次 PING（`config.h:14 ping_interval_ms=1000`，`sender.cpp:507`），但永远收不到 PONG，
     `hasReceivedPong()` 恒为 false → `Publisher.cpp:274` 的 PONG 超时检测（`kPongTimeoutUs = 10s`，`Publisher.h:185`）永不生效。
   - 注意：`FrameReceiver::onPacket()`（`receiver.cpp:947`）里有 `case MsgType::PING: handlePing(...)` 且 `receiver.cpp:626-650` 有完整的 PONG 构造逻辑，但 **ProxyFrameReceiver 的 onUpstreamPacket 路径没有接 PING→PONG**。
2. **条件 1（控制 TCP 断开）不触发**：服务端 `DestroySession` 超时踢会话时，不通知推流端、也不关闭其控制信令连接，`m_ctrlLinkDown` 恒为 false。
3. **条件 2（sendto 失败）不触发**：UDP 无连接，网络恢复后 sendto 永远成功。
4. 服务端超时后 `HandleStreamData`（`server/sfu_server/SfuServer.cpp:881-886`）查 `m_sessions` 失败 → 每包打印 `[SFU-ERROR] Unknown session` 并 return，静默丢弃，客户端无从感知。

### 建议修复方案（按优先级）

1. **服务端回 PONG（治本，改动最小）**
   - `libs/frame_protoc/src/proxy_receiver.cpp` `onUpstreamPacket()`：对 `MsgType::PING` 构造 PONG 经 `sendUpstream` 回给推流端（可参考 `receiver.cpp:626-650` 的 PONG 构造），其余非 DATA 包维持原样转发。
   - 效果：客户端 `hasReceivedPong()` 变为 true，之后服务端一旦踢会话（停止回 PONG），客户端 10s 内即可触发重连，自动恢复。
2. **客户端去掉 hasReceivedPong() 前置（防御）**
   - `Publisher.cpp:274`：改为"已收到过 PONG → 10s 超时；从未收到 → 推流开始 N 秒（如 15s）后仍无任何回包也判定断线"。防止服务端从未回 PONG 的版本上继续盲发。
3. **服务端 DestroySession 时通知/断开推流端控制连接**
   - 踢会话时对推流端控制连接发送错误通知（如 SubscribeResp error）或直接 `shutdown`，让客户端条件 1 触发。
4. **服务端日志限速（善后）**
   - `SfuServer.cpp:884` 的 `std::cout << "[SFU-ERROR] Unknown session: ..."` 改为每包静默丢弃（或 LogDebug + 周期汇总），避免死会话期间日志无限膨胀（本次 3.4M 行 / 712MB）。

### 涉及文件

| 文件 | 位置 | 说明 |
| --- | --- | --- |
| `libs/frame_protoc/src/proxy_receiver.cpp` | 480-497 | onUpstreamPacket：PING 不回 PONG（主因） |
| `libs/frame_protoc/src/receiver.cpp` | 626-650, 947 | 已有 PONG 构造与 PING 处理，可复用 |
| `client_sdk/rtc_client_sdk/src/Publisher.cpp` | 270-277, 368-379 | linkDown 断线判定；重连主循环 |
| `client_sdk/rtc_client_sdk/include/Publisher.h` | 185 | kPongTimeoutUs = 10s |
| `libs/frame_protoc/src/config.h` | 14 | ping_interval_ms = 1000 |
| `server/sfu_server/SfuServer.cpp` | 881-886, 900-903, 934-960 | Unknown session 丢弃/刷屏；首帧后 30s 超时；UDP 包路由 |

### 复现步骤

1. 部署本仓库 SDK 与 SFU，pi4 或任意 UDP 推流端推流（如 `raspberry_pi_pusher`，stream=pivideo）。
2. 用 `tcpdump -i any udp port 9200` 或 iptables 临时丢包 31s+（使服务端空闲超时踢会话，`timeout_sec=30`）。
3. 恢复网络，等待 >2 分钟。
4. 观察：推流端日志 `fps≈30` 持续增长（以为还在推）；服务端 `grep -c 'Unknown session' /root/log/sfu.log` 持续增长；拉流端黑屏/无流。
5. 期望（修复后）：推流端在服务端踢会话后 ≤10s 内打印 `[Push] 检测到连接断开，准备重连...` 并自动重新订阅，服务端出现新的 `Subscribe OK`，流自动恢复。

### 运维备忘

- 当前唯一恢复手段：重启推流端（pi4：`sudo systemctl restart pivideo-pusher`，服务定义见 `/etc/systemd/system/pivideo-pusher.service`，日志 `/home/pi/tj_rtc/push_pivideo.log`）。
- 快速判定"推流端假活"：`ssh sh "grep -c 'Unknown session' /root/log/sfu.log"` 是否在增长。

---

## Bug 2：SFU 服务端反复 std::bad_alloc 静默崩溃，无自动拉起（手机端拉流中断）

**状态**：已定位并修复（2026-09-07 上线，手机端拉流已验证恢复）| **发现日期**：2026-09-03 ~ 09-04 | **影响范围**：sh 服务器 `sfu_server_full`，每次崩溃期间所有拉流端不可用

### 现象

- 进程静默死亡：glog 无 FATAL/ERROR 前兆、无 OOM（崩溃时 memavail ≈ 1.1GB）、dmesg 无段错误、RSS 仅 ~8.6MB（与 8/28 内存修复后的正常水平一致）；
- `sfu.log` 末尾出现 `terminate called after throwing an instance of 'std::bad_alloc'`（libstdc++ 输出），写入时间与进程死亡时间吻合（可用 `/root/log/sfu_mon.log` 的 DEAD 时间点交叉验证）；
- 24 小时内崩溃 3 次：9/3 13:01（uptime 20.7h）、9/4 09:28（uptime 17h）、9/4 14:57（uptime 5h）；
- 服务为 nohup 启动、无托管，崩溃后不会自愈——9/4 这次挂了约 1 小时才被发现，手机端拉不到流。

### 已知规律

1. 三次崩溃时**都有订阅者在线**（崩溃前最后一次统计 `subs=1/2`；正常只有推流 pub+ctrl 时 subs=0）；
2. 其中两次紧贴订阅端 TCP 异常：
   - 9/4 09:28:44 `[SFU] TCP recv error (TcpRecvCb): fd=10 errno=104 (Connection reset by peer)` 是该进程最后一条日志，随后即死；
   - 9/3 03:02 `TCP recv error: fd=9 errno=9 (Bad file descriptor)`（10 小时后崩溃，可能不同触发）；
   - 两个进程的 ERROR 日志中还出现过 `Partial send`（fd=9，ctrl 连接写半截）。
3. 推测拉流端断开后的清理路径（`OnDisconnect` → `RemoveSession`）抛出 bad_alloc；低 RSS 下 bad_alloc 更符合"按损坏的尺寸值申请巨量内存"（use-after-free 读出垃圾长度）而非内存耗尽。

### 排查现状（2026-09-04）

- 部署二进制（9/2 16:18 构建，与本地 `build_server/bin/sfu_server_full` 一致，459576 字节）已包含：
  - 8/28 内存修复（gap 窗口上限 / 缓存参数收敛 / TCP 断开资源回收，见 `server/docs/MEMORY_FIX_20260828.md`）；
  - 9/2 watcher 修复（`SfuServerBase.cpp` TcpRecvCb 断开路径先 `ev_io_stop` 再 delete，修复 use-after-free 导致的偶发 bad_alloc）；
  - 崩溃仍在发生 → 是另一条新路径。
- 9/4 14:57 崩溃未生成 core：该进程 09:55 用旧版脚本启动（无 `ulimit -c`）；core dump 采集是当天 10:01 才加进脚本的。
- 16:02 已用新脚本重启（pid 210812，`Max core file size = unlimited` 已确认），**下次崩溃会在 `/root/cores/` 生成 core**。
- 注意：`kernel.core_pattern=/root/cores/core.%e.%p` 目前仅运行时生效，**重启机器即失效**，尚未持久化。
- 本次崩溃期间 pi4 推流端（手动进程，非 systemd 实例）表现正常：按 PONG 超时检测到断线并循环重试，服务端恢复后 ~3s 内自动重连成功（Bug 1 的修复链路验证有效）。

### 待办

1. ~~下次崩溃后用 core 定位~~ **已完成（2026-09-07，用 9/5 12:23 的 core）**：见下方"定位结论"。
2. 持久化 core_pattern：`echo 'kernel.core_pattern=/root/cores/core.%e.%p' > /etc/sysctl.d/99-coredump.conf && sysctl --system`（**仍未做**）。
3. 【决定：暂不做】不建 systemd 自动重启——当时以定位崩溃为优先，自动拉起会掩盖 crash 现场。**崩溃已定位，这一取舍的前提已不成立，可重新决定**（是否加自动拉起待用户确认）。
4. ~~重点审查订阅端（手机）断开的清理路径~~ **已复查**：根因在 `TcpRecvCb` 的长度前缀解析，与订阅端是否在线无关；`RemoveSession`/`OnDisconnect` 路径本身未发现问题。遗留的隐患见下方"仍未解决 / 后续"第 4 条。

### 涉及文件

| 文件 | 位置 | 说明 |
| --- | --- | --- |
| `server/sfu_server/SfuServerBase.cpp` | 353-502 | TcpRecvCb 两阶段接收与断开清理（重点怀疑路径） |
| `server/sfu_server/SfuConnManager.cpp` | — | WriteCb/ReadCb 断开路径（8/28 修复，需复查） |
| `server/sfu_server/main.cpp` | 17-26, 186-189 | 信号处理；无 FATAL/自定义 terminate |
| `/root/shell/sfu_server.sh`（sh 服务器） | — | 管理脚本，9/4 10:01 增加 `ulimit -c unlimited` + `/root/cores` |
| `/root/log/sfu_mon.log`（sh 服务器） | — | 每分钟 RSS 监控（cron），可确定死亡时间窗口 |

### 定位结论（2026-09-07，用 9/5 12:23 的 core 确认）

**崩溃链路（非内存耗尽，而是畸形输入被当成 malloc 尺寸）** —— `SfuServerBase::TcpRecvCb`：

1. 长度前缀按 `int` 做 `<< 24`，首字节 ≥ 0x80 时结果为负，赋给 `ssize_t allLen` 后被符号扩展；
2. `if (allLen > 1MB)` 这道校验只能挡正数，**负值直接放行**；
3. `new uint8_t[allLen]` → size_t 约 18EB → 抛 `std::bad_alloc`；
4. libev 回调是 C 函数指针，异常一路抛穿 `ev_run`，外层无 try/catch → `std::terminate` → SIGABRT。

core 里捞出的实际值 `0xffffffffffff87e8`，即 `allLen = -30744`、原始前缀 4 字节 `FF FF 87 E8`；gdb 栈顶帧为 `TcpRecvCb+1376 call operator new[]`，与源码 432 行一致。RSS 仅 10MB、memavail 1.1GB 也印证不是 OOM。

**本地复现**：用同一 build（`build_server/bin/sfu_server_full`，9/2 16:18，459576 字节，与崩溃进程一致）起服务，对无鉴权的信令端口只发 `FF FF 87 E8` 这 4 个字节 → 立刻 `terminate called ... 'std::bad_alloc'`。即**任意 TCP 对端用 4 个字节即可打死整个 SFU**，而 `9200` 在公网裸奔（`iptables -P INPUT ACCEPT`、ufw inactive、无鉴权）。

**推翻的旧规律**：本次崩溃时 `subs=0`（只有推流 pub+ctrl 在线），所以"三次崩溃都有订阅者在线"不成立；触发条件与手机端是否在线无关，只取决于是否出现一次畸形长度前缀。这也解释了 8/28、9/2 两次按 use-after-free 思路做的修复为何没能止住。`SfuConnManager::ReadCb` 内是同一份代码的复制，同样有此漏洞（其非法分支还只 `return`，既不断连也不停 watcher）。

### 已修复并上线（2026-09-07，PID 408881）

| 文件 | 改动 |
| --- | --- |
| `common/sfu_protocol/SfuProtocol.h` | 帧格式一处权威定义：`[4字节大端长度=前缀之后字节数][header][payload]`；`ReadTcpFrameLen`（无符号）、`IsValidTcpFrameLen`（1B~1MB 闭区间） |
| `SfuServerBase.cpp` `TcpRecvCb` | 无符号解析 + 区间校验；非法只断这一条连接并打印 `peer/prefix/len/msgsRecv`；抽出 `CloseTcpConn` 统一三处拆连（补齐拒绝分支漏掉的 `OnDisconnect`）；新增 `msgsRecv` 计数 |
| `SfuConnManager.cpp` `ReadCb` | 同上校验与日志；抽出 `CloseTcpSession` 替换裸 `return`（原来会把连接永久卡死） |
| `SfuEvPool.cpp` | `RunLoopWithBarrier` 包住两个事件循环线程的 `ev_run`，回调抛异常只记日志并重启循环，不再整机停摆 |
| `SfuServer.cpp` `SendToClientQuick` | 改走 `SendToClient`：原路径经 `ConnManager::SendQuick` 把长度写成 `4+1+len` 且额外塞一个 header，接收端会**固定多吃 4 字节、整条连接永久错位**（Kick 与 TCP 心跳回显走的就是它）；`OnHeartbeat` 回显改为自带 header |
| `SfuConnManager.cpp` `Send` | TCP 两个分支的长度字段统一为 `1+len` |

**验证**：本地 5 组载荷（`FF FF 87 E8` 首帧／合法帧后再接垃圾／长度 0／超 1MB）全部只断连不崩，`msgsRecv` 能正确区分"首帧即非法"与"中途错位"；生产上重放 `FF FF 87 E8` 后进程存活并留下一行防护日志，推流端会话与 UDP 媒体转发不受影响。

### 端到端确认与上线记录（2026-09-07）

- **手机端拉流成功**：Android app 订阅 `pivideo` 正常出流，本 Bug 的原始现象（"手机端拉不到流"）闭环。
- 支撑该结论的服务端链路：重启后 pi4 推流端 ~3s 内自动重连（session 1000 UDP / 1001 ctrl）、`Stream 'pivideo' registered`、proxy `Received` 计数持续增长 → 拉流端拿到的是实时流而非缓存。
- 崩溃载荷重放（`FF FF 87 E8` 打生产 9200）实际日志：
  `[SFU] TCP 帧长度非法，断开该连接: fd=10 peer=60.176.176.62:15432 len=4294936552 prefix=[FF FF 87 E8] msgsRecv=0 (首帧即非法: 对端未按本协议发送)`，进程存活、服务无感。
- 部署方式：`scp` 到 `.new` 后 `mv` 替换（避开运行中二进制的 `ETXTBSY`）。
  - 上线二进制 md5 `7033c3add0c0b3c1acbfddcbb901a85f`；
  - 回滚：`ssh sh "cp /root/bin/sfu_server_full.bak_20260907 /root/bin/sfu_server_full && /root/shell/sfu_server.sh restart"`；
  - 日常管理：`/root/shell/sfu_server.sh {start|stop|restart|status}`（脚本已带 `ulimit -c unlimited` + `/root/cores`）。

### 仍未解决 / 后续

1. **谁发来畸形前缀仍未定位**——之前 `TcpAcceptCb` 不记录 peer、崩溃那条连接零痕迹。现已在防护日志里带上 `peer=IP:port + 原始 4 字节 + msgsRecv`，下次出现即可判定是外部乱连（首帧即非法）还是服务端自己把帧流带偏（中途错位）。9/4 那次进程有 431 次 `closed by peer (TcpRecvCb)`、约每分钟 5 条，但本次重启后至今 0 条，说明该 churn 与当天某个客户端有关。
2. `9200` 公网裸端口是否加限制（防火墙/白名单/鉴权）——代码加固后已不致命，但仍可被用来随时断自己的连接。
3. `kernel.core_pattern=/root/cores/core.%e.%p` 仍只在运行时生效，重启机器即失效，未持久化。
4. 待查：`TcpRecvCb` 在 `DispatchMessage` 返回后仍继续使用 `connInfo`（若派发过程中该连接的拆连逻辑已 delete 过它，则 `delete[] connInfo->recvBuf` 是 use-after-free）；`SendToClient` 短写时只记日志、丢弃帧尾（同样会造成接收端永久错位）；`ConnSession::ReadCb` 的派发是空实现；`ConnManager::Send(queued=true)` 的发送队列无人 drain（`WriteCb` 从未注册，目前该路径已无调用者）。
