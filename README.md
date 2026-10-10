# tj_rtc

RTC 音视频工程：SFU 服务器、客户端 SDK、各平台推流/拉流 Demo。

## 目录结构

```
tj_rtc/
├── rtc/                  # RTC 核心（自研栈）
│   ├── client_sdk/       # 客户端 SDK（Publisher/Subscriber/P2PManager）
│   ├── common/           # 共享协议头（sfu_protocol）
│   ├── libs/             # frame_protoc(FEC) / pacer / mem_pool
│   └── server/           # SFU 服务器
├── apps/                 # 使用 RTC 的应用端
│   ├── demos/            # 各平台推拉流 Demo
│   ├── OpenHD/           # OpenHD 子模块（RtcLink 集成）
│   └── QOpenHD/          # QOpenHD 子模块（地面站 UI）
└── docs/                 # 设计/进度/缺陷文档
```

## 部署环境

- **pi4**：本地树莓派 4 推流端（SSH 别名见本地 `~/.ssh/config`，凭据不入库）
- **sh**：SFU 服务器（SSH 别名见本地 `~/.ssh/config`，凭据不入库）

## 文档

- [SFU 内存泄漏 / std::bad_alloc 崩溃修复总结（2026-08-28）](rtc/server/docs/MEMORY_FIX_20260828.md)
- [Bug 记录（推流假活、SFU 崩溃根因与修复，截至 2026-09-07）](docs/bug.md)
- [OpenHD RtcLink 链路设计与验证记录](docs/openhd_rtc_link_design.md)
- [项目进度与联调记录](docs/PROGRESS.md)