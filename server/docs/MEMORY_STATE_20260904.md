# SFU 服务当前状态与排障记忆（2026-09-04 快照）

> 目的：让下一次会话（无论 AI 还是人工）无需重新考古即可接手。
> 时间线事件与崩溃史见文末；紧急恢复步骤见第一段。

## 0. 系统拓扑

- **pi4**（`~/.ssh/config` 别名，192.168.50.114，用户 pi）：树莓派推流端。
  - 运行方式：**手动 nohup**（非 systemd，systemd 服务 pivideo-pusher 已停用，9/2 14:17 后改手动）。
  - 进程：`~$ ./raspberry_pi_pusher --width 1280 --height 720 --fps 30 --bitrate 2500 --gop 30 --server 106.15.177.248 --port 9200 --stream pivideo --user pi_pusher --duration 0`
  - 实时日志：`/home/pi/pi_pusher.log`（stderr/stdout 重定向到此）
- **sh**（106.15.177.248，root）：SFU 服务器，1.6GB 内存、无 swap，云主机（阿里云 iZ 开头）。
  - 二进制：`/root/bin/sfu_server_full`（当前版日期 9/2 16:19，459KB；备份 `.bak_0828` / `.bak_0902_glog` / `.bak_0902_reuse`）
  - 管理脚本：`/root/shell/sfu_server.sh {start|stop|restart|status|log}`，pid `/root/log/sfu.pid`，stderr `/root/log/sfu.log`，glog 在 `/root/log/sfu_server_full.*.INFO.*`
  - 端口：TCP 9200（信令+媒体）、UDP 9201（媒体）、STUN 9202
  - 本地对应脚本：`server/scripts/sfu_server.sh`（已同步修改：ulimit core）
- **手机**：Android APK `rtc-app-debug.apk`（8/31 版），走 UDP 媒体 + TCP 信令。
- **本机**（ubuntu）：编译机；`demos/linux_camera_push/build/pull_probe` 是拉流验证工具。

## 1. 当前状态（2026-09-04 10:18）

- 服务器**运行中**：PID 192841（9/4 09:55 由脚本拉起），9200/9201 监听正常，RSS ~8.6MB。
- pi4 推流端自动重连成功：session=1000（pub 媒体，UDP）+ 1001（pub ctrl，TCP），30fps 稳定。
- 拉流验证：`pull_probe 106.15.177.248 9200 pivideo sub_probe 6 udp` → 184 帧/6s、IDR 正常、0 丢包。
- 手机若拉不到流：服务器重启过会清会话，App 需重新进入拉流页面/重连（无自动重订阅）。

## 2. 已部署的监控与调试设施（本次新增，勿删）

1. **RSS/内存分钟级监控**：sh crontab 每分钟写 `pid/RSS/MemFree/MemAvailable` 到 `/root/log/sfu_mon.log`。
   - 注意 crontab 中 `date +\%FT\%T` 的 `%` 必须转义（否则 cron 截断命令）。
   - 历史行中 `memfree_avail=` 字段是旧格式（第一次安装时 awk 写错，10/17 前），新格式为 `memfree_kb/ memavail_kb`。
2. **core dump**：
   - `kernel.core_pattern` 已从 apport 改为 `/root/cores/core.%e.%p`（sysctl -w，重启后会恢复默认，需重设）。
   - `/root/shell/sfu_server.sh` 已加 `ulimit -c unlimited` + `mkdir -p /root/cores`（仅下次 start 生效）。
   - **下次崩溃后**：到 /root/cores 找 core，`gdb /root/bin/sfu_server_full /root/cores/core.*` → `bt` 看 bad_alloc 抛出处。

## 3. 崩溃史（关键！）

| 时间 | 二进制 | 运行时长 | 表现 | 备注 |
| --- | --- | --- | --- | --- |
| 8/28 前 | 旧版 | — | 2 次 bad_alloc 崩溃 | 修复 1：detectUpstreamGaps 间隙上限 + 缓存参数收敛（见 MEMORY_FIX_20260828.md）；修复 2：TCP 断开路径资源回收；修复 3：malloc_trim |
| 9/2 15:08 / 15:38 | glog 迭代版 | 9min / 1min | bad_alloc | 当天迭代期，多备份共存 |
| 9/2 16:19 ~ 9/3 13:01 | 459KB 版（92533） | ~20.7h | **静默死**（无崩溃日志、无 OOM、无信号记录） | 用户决定不查（疑似外部干预） |
| 9/3 16:19 ~ 9/4 09:28 | 同一 459KB 版（151918） | ~17.2h | **bad_alloc**（崩溃前最后日志：`TCP recv error fd=10 errno=104`） | 崩溃前状态健康：Cache 2447/20000、4 会话、1 流；仅 3 条 ERROR（16:21 Invalid subscribe/23:28 Partial send fd=9/09:28 reset） |

**关键观察**：崩溃前 RSS 无缓慢增长迹象（现运行中仅 ~8.8MB，8/28 修复后缓存上限 2 万条也从未触顶），
**不像泄漏**，更像**瞬间大分配失败或系统内存被其它进程占满**（1.6GB 无 swap 小机器）。
该进程的 bad_alloc 也可能是**堆损坏**（use-after-free 后 bad_alloc 偶发），9/2 代码里曾修过一个
"watcher 未 stop 就 delete → use-after-free → 表现为 std::bad_alloc" 的路径（SfuServerBase.cpp:402-404 有注释）。
**若 core 分析指向堆损坏，优先查 SfuServerBase::TcpRecvCb 与 SfuConnManager::WriteCb 的双清理路径**（两套对象：ServerSocketInfo 与 ConnSession，断开时 OnDisconnect→DestroySession→RemoveSession 顺序敏感）。

## 4. 已完成的代码改动（9/3，未重新部署服务器）

**修好 Subscriber SDK 的 TCP 拉流 bug**（`client_sdk/rtc_client_sdk/src/Subscriber.cpp` + `include/Subscriber.h`）：
- 原 TCP 媒体路径把 kForwardData 分片直接喂 OnFrame，跳过 FEC 重组 → TCP 模式永远 0 完整帧。
- 抽出 `InitFrameReceiver()`（UDP/TCP 共用，NACK/STATS 回传按 m_useUdp 分流），TCP 循环改为 onPacketReceived + PLI + tick。
- 验证：TCP 拉流 309 帧/10s（修复前 0 帧），UDP 回归正常。
- 影响：Android APK 需重新编译才带上此修复（手机走 UDP，不受影响）。

## 5. 命令速查

```bash
# 服务器
ssh sh /root/shell/sfu_server.sh status        # 运行中 (PID xxx)
ssh sh "tail -5 /root/log/sfu_mon.log"          # 内存监控
ssh sh "ls -t /root/log/sfu_server_full.*.INFO.* | head -1"  # 最新 glog
ssh sh "grep -E 'Subscribe request|pivideo' \$(ls -t /root/log/sfu_server_full.*.INFO.* | head -1) | tail -3"

# pi4 推流端
ssh pi4 "ps aux | grep raspberry_pi_pusher | grep -v grep"   # 进程是否在
ssh pi4 "tail -5 /home/pi/pi_pusher.log"                     # 推流日志（[stat] frames=... fps=30）

# 本机拉流验证（必须用 UDP 模式！TCP 媒体模式详见上文 bug）
cd /home/ubuntu/tj_rtc/demos/linux_camera_push/build && ./pull_probe 106.15.177.248 9200 pivideo sub_probe 6 udp

# 崩溃后现场
ssh sh "ls -la /root/cores/"
ssh sh "gdb /root/bin/sfu_server_full /root/cores/core.* -ex bt -batch"
```

## 6. 遗留事项

1. 服务器**未配置开机自启/守护**（nohup 手动起），崩溃后不会自动恢复——考虑 systemd Timer 或脚本守护。
2. 8/28 遗留：sh 无 swap，建议加 1~2GB swapfile 缓冲内存尖峰。
3. core_pattern 重启后恢复 apport，需持久化（`/etc/sysctl.d/`）。
4. 9/4 09:28 崩溃尚无 core（本次部署 core dump 之后的下一次崩溃才有）。
5. 监控日志 10:16:17 前有两条手测畸形行（pid 为空 / memfree_avail 旧格式），不影响后续分析。