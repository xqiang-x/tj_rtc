# SFU 服务器使用指南

## ✅ 服务器信息

| 项目 | 值 |
|------|------|
| **公网 IP** | `106.15.177.248` |
| **SSH 别名** | `sh` |
| **TCP 端口** | `9200` (客户端连接) |
| **UDP 端口** | `9201` (音视频流) |
| **状态** | ✅ 运行中 |
| **PID** | 1008226 |
| **运行时间** | 5+ 分钟 |

## 🚀 快速开始

### 1. 加载快捷命令（每次打开终端）

```bash
cd /Users/mac/Documents/qoder_libuv
source sfu_aliases.sh
```

### 2. 常用管理命令

```bash
# 查看服务器状态
sfu-status

# 查看实时日志
sfu-log

# 查看最后 100 行日志
sfu-logtail

# 重启服务器
sfu-restart

# 停止服务器
sfu-stop

# 启动服务器
sfu-start

# SSH 到服务器
sfu-ssh

# 查看帮助
sfu-help
```

### 3. 测试服务器连接

```bash
./test_server_connection.sh
```

## 📱 客户端使用

### macOS 推流（摄像头）

```bash
# 1. 编译（如果还没编译）
cd mac_camera_push
mkdir -p build && cd build
cmake ..
make -j4

# 2. 运行
./mac_camera_push
```

**配置：**
- 服务器地址: `106.15.177.248`
- 服务器端口: `9200`
- 流名: `stream1`（即推流端 uid）
- 用户ID: `mac_publisher`

### macOS 拉流

```bash
# 1. 编译（如果还没编译）
cd mac_pull_test
mkdir -p build && cd build
cmake ..
make -j4

# 2. 运行
./mac_pull_test
```

**配置：**
- 服务器地址: `106.15.177.248`
- 服务器端口: `9200`
- 流名: `stream1`（与推流端一致）
- 用户ID: `mac_subscriber`

### Android 应用

在 Android 应用中配置：

```
服务器地址: 106.15.177.248
服务器端口: 9200
流名: stream1
用户ID: android_user
[✓] 开启音频
```

## 📊 监控和调试

### 查看实时日志

```bash
# 方法 1: 使用快捷命令
sfu-log

# 方法 2: 直接 SSH
ssh root@sh "tail -f ~/qoder_libuv/sfu_server_full/build/sfu.log"
```

### 查看连接数

```bash
ssh root@sh "netstat -an | grep 9200 | wc -l"
```

### 查看进程资源

```bash
ssh root@sh "ps aux | grep sfu_server"
```

### 重启服务

```bash
sfu-restart
```

## 🔄 更新代码后重新部署

### 方法 1: 完整重新部署（推荐）

```bash
./deploy_to_ssh.sh sh
```

这会自动：
1. 停止旧服务
2. 上传新代码
3. 远程编译
4. 启动新服务

### 方法 2: 仅重新编译

```bash
# 重新编译
ssh root@sh "~/qoder_libuv/sfu_server_full/build/sfu_manage.sh recompile"

# 重启服务
ssh root@sh "~/qoder_libuv/sfu_server_full/build/sfu_manage.sh restart"
```

## 📝 典型使用场景

### 场景 1: 本地测试

```bash
# 终端 1: 启动推流
cd mac_camera_push/build
./mac_camera_push

# 终端 2: 启动拉流
cd mac_pull_test/build
./mac_pull_test
```

### 场景 2: 多客户端测试

```bash
# 客户端 1: macOS 推流
mac_camera_push/build/mac_camera_push
  - server: 106.15.177.248
  - stream: stream1（即推流端 uid）
  - user: mac_pub_1

# 客户端 2: macOS 拉流
mac_pull_test/build/mac_pull_test
  - server: 106.15.177.248
  - stream: stream1（与推流端一致）
  - user: mac_sub_1

# 客户端 3: Android 拉流
Android App
  - server: 106.15.177.248
  - stream: stream1
  - user: android_sub_1
```

### 场景 3: 多流

```bash
# 流 1
mac_camera_push: stream1, user: pub1
mac_pull_test: stream1, user: sub1

# 流 2（另起一路推流+拉流）
mac_camera_push: stream2, user: pub2
mac_pull_test: stream2, user: sub2
```

## ⚙️ 服务器配置

### 当前配置

```
TCP listen:        0.0.0.0:9200 (ON)
UDP listen:        0.0.0.0:9201
Max clients:       1024
Stats interval:    10s
Session timeout:   3s
Verbose logging:   disabled
```

### 修改配置

编辑源代码后重新编译部署：

```bash
# 修改 sfu_server_full/SfuServer.cpp 中的配置
# 然后重新部署
./deploy_to_ssh.sh sh
```

## 🔍 故障排除

### 问题 1: 客户端连接失败

**症状：** 客户端显示连接超时

**检查步骤：**

```bash
# 1. 检查服务器是否运行
sfu-status

# 2. 检查端口是否监听
ssh root@sh "netstat -tlnp | grep 9200"

# 3. 测试端口连通性
./test_server_connection.sh

# 4. 查看日志
sfu-log
```

**解决方法：**
- 确保服务器运行: `sfu-start`
- 检查安全组是否开放端口
- 重启服务器: `sfu-restart`

### 问题 2: 视频黑屏/卡顿

**症状：** 连接成功但没有视频

**检查步骤：**

```bash
# 1. 查看服务器日志
sfu-log

# 2. 检查推流客户端是否正常运行
# 查看推流客户端日志

# 3. 检查网络带宽
ssh root@sh "iftop -i eth0"
```

**可能原因：**
- 推流客户端未启动
- 网络丢包严重
- FEC 配置问题
- 编解码器问题

### 问题 3: 服务器崩溃

**症状：** 服务器进程消失

**解决方法：**

```bash
# 1. 查看最后日志
sfu-logtail

# 2. 重启服务器
sfu-start

# 3. 如果反复崩溃，查看系统日志
ssh root@sh "dmesg | tail -50"
```

### 问题 4: 端口被占用

**症状：** 启动失败，提示地址已在使用

**解决方法：**

```bash
# 1. 查看占用端口的进程
ssh root@sh "sudo lsof -i :9200"
ssh root@sh "sudo lsof -i :9201"

# 2. 停止所有 SFU 进程
ssh root@sh "pkill -9 sfu_server"

# 3. 重新启动
sfu-start
```

## 📈 性能优化

### 1. 调整 Session Timeout

如果客户端频繁断开，可以增加超时时间：

```cpp
// sfu_server_full/SfuServer.cpp
m_config.session_timeout_seconds = 10;  // 从 3s 改为 10s
```

### 2. 启用详细日志

调试问题时启用详细日志：

```cpp
// sfu_server_full/SfuServer.cpp
m_config.verbose_logging = true;
```

然后重新部署。

### 3. 增加最大客户端数

```cpp
// sfu_server_full/SfuServer.cpp
m_config.max_clients = 2048;  // 从 1024 增加到 2048
```

## 🔐 安全建议

### 1. 配置防火墙（可选）

只允许特定 IP 访问：

```bash
ssh root@sh << 'EOF'
# 只允许特定 IP
sudo ufw allow from <你的IP> to any port 9200 proto tcp
sudo ufw allow from <你的IP> to any port 9201 proto udp
sudo ufw enable
EOF
```

### 2. 修改默认端口（可选）

```cpp
// 修改为其他端口
int tcp_port = 19200;
int udp_port = 19201;
```

### 3. 添加认证（未来功能）

计划添加 Token 认证机制。

## 📞 获取帮助

### 查看文档

```bash
# 部署文档
cat DEPLOY_SSH.md

# 部署成功报告
cat DEPLOY_SUCCESS.md

# 项目 README
cat README.md
```

### 查看日志

```bash
# 实时日志
sfu-log

# 历史日志
ssh root@sh "cat ~/qoder_libuv/sfu_server_full/build/sfu.log"
```

### 联系支持

遇到问题请提供：
1. 服务器日志 (`sfu-logtail`)
2. 客户端日志
3. 操作步骤

---

## 🎯 快速参考

```bash
# 最常用命令
source sfu_aliases.sh      # 加载快捷命令
sfu-status                 # 查看状态
sfu-log                    # 查看日志
sfu-restart                # 重启服务

# 测试
./test_server_connection.sh  # 测试连接

# 部署
./deploy_to_ssh.sh sh        # 重新部署
```

**服务器地址: `106.15.177.248:9200`**

祝使用愉快！ 🚀
