# tj_rtc

RTC 音视频工程：SFU 服务器、客户端 SDK、各平台推流/拉流 Demo。

## 部署环境

- **pi4**：本地树莓派 4 推流端（SSH 别名见本地 `~/.ssh/config`，凭据不入库）
- **sh**：SFU 服务器（SSH 别名见本地 `~/.ssh/config`，凭据不入库）

## 文档

- [SFU 内存泄漏 / std::bad_alloc 崩溃修复总结（2026-08-28）](server/docs/MEMORY_FIX_20260828.md)
- [Bug 记录（推流假活、SFU 崩溃根因与修复，截至 2026-09-07）](bug.md)