# SFU 内存泄漏 / std::bad_alloc 崩溃修复总结（2026-08-28）

## 背景

sh 服务器（106.15.177.248，1.6GB 内存、无 swap）上的 `sfu_server_full` 连续发生两次崩溃，
表现为手机无法连接服务器（9200/9201 端口不再监听）。崩溃日志显示异常为 `std::bad_alloc`。

- **Run1**：高负载期间崩溃，内存持续增长直至分配失败。
- **Run2**：空闲运行约 44 分钟后崩溃，疑似系统内存压力叠加。

## 根因分析

### 1. `detectUpstreamGaps` 无上限间隙分配（主因）

`libs/frame_protoc/src/proxy_receiver.cpp` 中，收到上游数据包时若
`received_seq > expected_data_seq`，会为中间每一个"缺失序号"创建一个
`ProxyNackState` 并插入 `std::map`。`SeqNum` 为 `uint64_t`，一旦出现坏包 /
伪造包 / 推流端序号重置导致序号大幅跳变，单包即可触发数百万次分配，
直接耗尽内存抛出 `std::bad_alloc`。

### 2. NACK 重传缓存参数过大

`SfuServer.cpp` 中配置 `cache_timeout_ms = 300000`（300 秒）、
`max_cache_entries = 100000`。按 30fps、每帧 ~1500 字节计算，
单个推流会话稳态缓存约 120~140MB。实测进程 RSS 120MB，
`malloc_stats` 显示 `system ≈ in_use`，即内存占用是真实缓存数据而非碎片。

### 3. TCP 断开路径会话 / 缓冲区泄漏

- `SfuConnManager.cpp` 的 WriteCb 错误路径、ReadCb 半关闭（phase-1）与
  对端关闭（phase-2）路径只关闭 fd，未回调 `OnDisconnect`、未释放
  `ConnSession`（其 `recvBuf` 与 `sendQueue` 随之泄漏），会话条目残留在管理器中。
- `SfuServerBase.cpp` 的 `TcpRecvCb` 在协议解析失败 / 超长包等场景关闭连接时，
  未调用 `RemoveSession`，且存在重复 `close(fd)` 的风险。

### 4. glibc 碎片抖动（次要）

长时间运行下 RSS 以约 100KB/分钟缓慢上涨，但 `malloc_stats` 的 `in_use`
保持在 117.2MB 平稳，属于 arena 碎片，`malloc_trim(0)` 可回收。

### 5. 统计打印误导项（次要）

状态打印硬编码 `/ 10000` 作为缓存上限分母（实际为 100000），
导致 "83135 / 10000" 之类误导性输出，干扰了早期判断。

## 修复内容

### 修复 1：间隙上限 + 缓存参数收敛

- `libs/frame_protoc/src/proxy_receiver.h`
  - `ProxyConfig` 新增 `uint32_t max_gap_window = 1024;`
  - 新增接口 `uint32_t maxCacheEntries() const;`
- `libs/frame_protoc/src/proxy_receiver.cpp`
  - `detectUpstreamGaps`：`gap > max_gap_window` 时直接重锚
    `expected_data_seq = received_seq + 1`，不再逐号创建 NACK 状态；
    正常小间隙仍逐号登记。
- `server/sfu_server/SfuServer.cpp`
  - `proxy_cfg.cache_timeout_ms = 10 * 1000;`（300s → 10s）
  - `proxy_cfg.max_cache_entries = 20 * 1000;`（100000 → 20000）

### 修复 2：补全 TCP 断开路径的资源回收

- `server/sfu_server/SfuConnManager.cpp`：WriteCb 错误路径、ReadCb phase-1 /
  phase-2 关闭路径统一走完整清理——回调 `OnDisconnect` 后调用
  `server->GetConnManager()->RemoveSession(fd)`（`ConnSession` 析构负责
  关闭 fd、释放 `recvBuf`、清空 `sendQueue`）。
- `server/sfu_server/SfuServerBase.cpp`：`TcpRecvCb` 三处错误关闭路径
  （协议解析失败两段、>1MB 超长包保护）改为先 `RemoveSession(fd)`，
  再 `connInfo->fd = -1`（fd 已由析构关闭，避免重复 close），最后 `delete connInfo`。

### 修复 3：周期性 `malloc_trim` + 修正硬编码统计

- `server/sfu_server/SfuServer.cpp`
  - `#include <malloc.h>`，在 `OnUpdate` 的 1000ms 周期块中每 60 秒调用一次
    `malloc_trim(0)` 回收 glibc 碎片。
  - 状态打印改为动态上限：
    `printf("    Cache size: %llu / %u entries\n", ..., session->proxy_receiver->maxCacheEntries());`

## 构建与部署

```bash
# 本地编译（build_linux，x86-64 产物与 sh 一致）
cd build_linux && make sfu_server_full -j4

# 上传（旧二进制备份为 /root/bin/sfu_server_full.bak_0828）
scp bin/sfu_server_full sh:/root/bin/

# 重启
ssh sh /root/shell/sfu_server.sh restart
```

管理脚本：`/root/shell/sfu_server.sh {start|stop|restart|status|log}`，
pid 位于 `/root/log/sfu.pid`，日志位于 `/root/log/sfu.log`。

pi4 推流端同步重启脚本：`~/tj_rtc/restart_pusher.sh`
（720p30、2500kbps、stream=pivideo，输出 `~/tj_rtc/push_pivideo.log`）。

## 验证结果

| 指标 | 修复前 | 修复后 |
| --- | --- | --- |
| 进程 RSS（推流中） | ~120MB | ~9MB |
| NACK 缓存占用 | 83135 / 100000 | 2809 / 20000 |
| 堆碎片（system − in_use） | — | ~450KB |
| 推流 | 正常 | 30fps 稳定（pi4 PID 501305，sessionId=1000） |

## 遗留建议

1. 开启 core dump（`ulimit -c unlimited`），以便确认 Run2 空闲期崩溃的确切类型
   （当前无 core，推测为系统内存压力所致）。
2. sh 服务器内存仅 1.6GB 且无 swap，建议增加 1~2GB swap 作为缓冲。
3. 若需更长重传窗口，可上调 `cache_timeout_ms`，但应按
   `码率 × 时长` 评估单会话内存开销。
