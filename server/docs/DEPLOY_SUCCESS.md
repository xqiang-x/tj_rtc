# SFU 服务器部署成功报告

## ✅ 部署信息

**部署时间**: 2026-05-31 13:03:26  
**服务器**: root@sh (阿里云 Ubuntu 24.04)  
**服务器IP**: 172.24.6.184 (内网)  
**进程PID**: 1008226  

## 📊 服务器状态

```
✅ 服务器运行中
✅ TCP 端口: 9200 (监听中)
✅ UDP 端口: 9201 (监听中)
✅ 进程稳定运行
```

## 🔧 服务器配置

```
TCP listen:        0.0.0.0:9200 (ON)
UDP listen:        0.0.0.0:9201
Max clients:       1024
Stats interval:    10s
Session timeout:   3s
Verbose logging:   disabled
```

## 📁 部署位置

- **项目目录**: `/root/qoder_libuv`
- **可执行文件**: `/root/qoder_libuv/sfu_server_full/build/sfu_server_full`
- **日志文件**: `/root/qoder_libuv/sfu_server_full/build/sfu.log`
- **PID 文件**: `/root/qoder_libuv/sfu_server_full/build/sfu.pid`
- **管理脚本**: `/root/qoder_libuv/sfu_server_full/build/sfu_manage.sh`

## 🚀 快速使用

### 本地快捷命令

```bash
# 加载快捷命令
source /Users/mac/Documents/qoder_libuv/sfu_aliases.sh

# 查看状态
sfu-status

# 查看实时日志
sfu-log

# 重启服务器
sfu-restart

# 停止服务器
sfu-stop
```

### SSH 远程管理

```bash
# SSH 到服务器
ssh root@sh

# 使用管理脚本
cd ~/qoder_libuv/sfu_server_full/build
./sfu_manage.sh status      # 查看状态
./sfu_manage.sh log         # 实时日志
./sfu_manage.sh restart     # 重启
./sfu_manage.sh stop        # 停止
./sfu_manage.sh start       # 启动
```

## 🧪 测试连接

### 从 macOS 推流测试

```bash
cd /Users/mac/Documents/qoder_libuv/mac_camera_push/build
./mac_camera_push
```

配置中使用：
- 服务器地址: sh (或服务器公网IP)
- 服务器端口: 9200

### 从 macOS 拉流测试

```bash
cd /Users/mac/Documents/qoder_libuv/mac_pull_test/build
./mac_pull_test
```

### 从 Android 应用测试

在 Android 应用中配置：
- 服务器地址: 服务器公网IP
- 服务器端口: 9200
- 流名: stream1（即推流端 uid）
- 用户ID: android_user

## ⚠️ 重要提示

### 1. 防火墙/安全组配置

**如果是阿里云/腾讯云服务器，需要配置安全组：**

添加入站规则：
- TCP 9200 (客户端连接)
- UDP 9201 (音视频流传输)

**操作步骤：**
1. 登录阿里云控制台
2. 进入 ECS 实例
3. 安全组 -> 配置规则
4. 添加入方向规则：
   - 协议类型: TCP，端口: 9200，授权对象: 0.0.0.0/0
   - 协议类型: UDP，端口: 9201，授权对象: 0.0.0.0/0

### 2. 获取公网IP

```bash
# 查看公网IP
curl ifconfig.me

# 或
curl ip.sb
```

### 3. 服务器重启后自动启动

如果服务器重启，需要手动启动服务：

```bash
ssh root@sh
cd ~/qoder_libuv/sfu_server_full/build
./sfu_manage.sh start
```

**或者配置 systemd 自动启动（推荐）：**

```bash
ssh root@sh << 'EOF'
cat > /etc/systemd/system/sfu-server.service << 'SERVICE'
[Unit]
Description=SFU Media Server
After=network.target

[Service]
Type=forking
User=root
WorkingDirectory=/root/qoder_libuv/sfu_server_full/build
ExecStart=/root/qoder_libuv/sfu_server_full/build/sfu_server_full --port 9200
PIDFile=/root/qoder_libuv/sfu_server_full/build/sfu.pid
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
SERVICE

systemctl daemon-reload
systemctl enable sfu-server
systemctl start sfu-server
EOF
```

## 📝 更新代码后重新部署

```bash
# 方法 1: 完整重新部署
cd /Users/mac/Documents/qoder_libuv
./deploy_to_ssh.sh sh

# 方法 2: 仅重新编译（代码已上传）
ssh root@sh "~/qoder_libuv/sfu_server_full/build/sfu_manage.sh recompile"
ssh root@sh "~/qoder_libuv/sfu_server_full/build/sfu_manage.sh restart"
```

## 🔍 监控和调试

### 查看实时日志

```bash
ssh root@sh "tail -f ~/qoder_libuv/sfu_server_full/build/sfu.log"
```

### 查看连接数

```bash
ssh root@sh "netstat -an | grep 9200 | wc -l"
```

### 查看进程资源使用

```bash
ssh root@sh "ps aux | grep sfu_server"
```

### 查看网络带宽

```bash
ssh root@sh "iftop -i eth0"
```

## 📞 故障排除

### 问题 1: 客户端连接失败

**检查项：**
1. 服务器是否运行: `sfu-status`
2. 防火墙是否开放端口
3. 安全组是否配置
4. 客户端配置是否正确

### 问题 2: 服务器无响应

**解决方法：**
```bash
# 查看日志
sfu-log

# 重启服务
sfu-restart

# 检查系统资源
ssh root@sh "free -h && df -h && top -bn1 | head -20"
```

### 问题 3: 端口被占用

**解决方法：**
```bash
# 查看占用端口的进程
ssh root@sh "sudo lsof -i :9200"
ssh root@sh "sudo lsof -i :9201"

# 停止旧进程
sfu-stop

# 重新启动
sfu-start
```

## 🎯 下一步

1. **配置安全组** - 开放 9200 (TCP) 和 9201 (UDP)
2. **获取公网IP** - 使用 `curl ifconfig.me`
3. **测试连接** - 使用 mac_camera_push 或 Android 应用
4. **监控日志** - 实时查看服务器运行状态

---

**服务器已成功部署并运行！** 🎉
