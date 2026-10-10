# SFU 服务器 SSH 部署指南

## 快速部署

### 1. 执行部署脚本

```bash
cd /Users/mac/Documents/qoder_libuv
./deploy_to_ssh.sh <服务器IP>
```

**示例：**
```bash
./deploy_to_ssh.sh 192.168.1.100
./deploy_to_ssh.sh your-domain.com
```

### 2. 部署过程

脚本会自动完成以下步骤：

1. ✅ **测试 SSH 连接** - 验证免密登录是否正常
2. ✅ **创建目录结构** - 在服务器上创建项目目录
3. ✅ **上传源文件** - 通过 SCP 上传所有必需的源文件
4. ✅ **远程编译** - SSH 到服务器执行 CMake 编译
5. ✅ **启动服务** - 在后台启动 SFU 服务器

## 服务器管理

部署完成后，在服务器上可以使用管理脚本：

```bash
ssh root@<服务器IP>
cd ~/qoder_libuv/sfu_server_full/build
./sfu_manage.sh <命令>
```

### 可用命令

| 命令 | 说明 | 示例 |
|------|------|------|
| `start` | 启动服务器 | `./sfu_manage.sh start` |
| `stop` | 停止服务器 | `./sfu_manage.sh stop` |
| `restart` | 重启服务器 | `./sfu_manage.sh restart` |
| `status` | 查看状态 | `./sfu_manage.sh status` |
| `log` | 实时查看日志 | `./sfu_manage.sh log` |
| `log-tail` | 查看最后 100 行 | `./sfu_manage.sh log-tail` |
| `recompile` | 重新编译 | `./sfu_manage.sh recompile` |

### 本地远程管理

也可以直接从本地执行管理命令：

```bash
# 查看状态
ssh root@<服务器IP> '~/qoder_libuv/sfu_server_full/build/sfu_manage.sh status'

# 查看日志
ssh root@<服务器IP> '~/qoder_libuv/sfu_server_full/build/sfu_manage.sh log-tail'

# 重启服务
ssh root@<服务器IP> '~/qoder_libuv/sfu_server_full/build/sfu_manage.sh restart'
```

## 服务器信息

部署后的服务器配置：

- **TCP 端口**: 9200（客户端连接）
- **UDP 端口**: 9201（音视频流传输）
- **部署目录**: `~/qoder_libuv`
- **可执行文件**: `~/qoder_libuv/sfu_server_full/build/sfu_server_full`
- **日志文件**: `~/qoder_libuv/sfu_server_full/build/sfu.log`
- **PID 文件**: `~/qoder_libuv/sfu_server_full/build/sfu.pid`

## 常见问题

### Q1: SSH 连接失败

**错误信息：**
```
[ERROR] SSH 连接失败，请检查:
  1. 服务器地址是否正确
  2. 是否已设置免密登录
  3. SSH 服务是否运行
```

**解决方法：**

1. **测试 SSH 连接：**
   ```bash
   ssh root@<服务器IP>
   ```

2. **设置免密登录（如果还没设置）：**
   ```bash
   # 生成本地密钥（如果没有）
   ssh-keygen -t rsa -b 4096
   
   # 复制公钥到服务器
   ssh-copy-id root@<服务器IP>
   
   # 测试免密登录
   ssh root@<服务器IP>
   ```

3. **检查 SSH 服务：**
   ```bash
   ssh root@<服务器IP> 'sudo systemctl status sshd'
   ```

### Q2: 编译失败

**错误信息：**
```
cmake not found
libev not found
```

**解决方法：**

脚本会自动安装依赖，如果失败可以手动安装：

**Ubuntu/Debian：**
```bash
ssh root@<服务器IP>
sudo apt-get update
sudo apt-get install -y cmake build-essential libev-dev
```

**CentOS/RHEL：**
```bash
ssh root@<服务器IP>
sudo yum install -y cmake gcc-c++ libev-devel
```

**Fedora：**
```bash
ssh root@<服务器IP>
sudo dnf install -y cmake gcc-c++ libev-devel
```

### Q3: 服务器启动失败

**检查日志：**
```bash
ssh root@<服务器IP> 'tail -f ~/qoder_libuv/sfu_server_full/build/sfu.log'
```

**常见原因：**
1. 端口被占用
2. 权限不足
3. 编译失败

**解决方法：**
```bash
# 检查端口占用
ssh root@<服务器IP> 'sudo netstat -tlnp | grep 9200'

# 停止旧进程
ssh root@<服务器IP> '~/qoder_libuv/sfu_server_full/build/sfu_manage.sh stop'

# 重新启动
ssh root@<服务器IP> '~/qoder_libuv/sfu_server_full/build/sfu_manage.sh start'
```

### Q4: 客户端连接失败

**检查服务器状态：**
```bash
ssh root@<服务器IP> '~/qoder_libuv/sfu_server_full/build/sfu_manage.sh status'
```

**检查防火墙：**
```bash
# Ubuntu/Debian
ssh root@<服务器IP> 'sudo ufw allow 9200/tcp'
ssh root@<服务器IP> 'sudo ufw allow 9201/udp'

# CentOS/RHEL
ssh root@<服务器IP> 'sudo firewall-cmd --permanent --add-port=9200/tcp'
ssh root@<服务器IP> 'sudo firewall-cmd --permanent --add-port=9201/udp'
ssh root@<服务器IP> 'sudo firewall-cmd --reload'
```

**检查云服务商安全组：**
- 阿里云：添加安全组规则，开放 9200 (TCP) 和 9201 (UDP)
- 腾讯云：添加安全组规则，开放 9200 (TCP) 和 9201 (UDP)
- AWS：修改安全组，添加入站规则

## 更新部署

如果修改了代码，需要重新部署：

### 方法 1: 重新运行部署脚本

```bash
./deploy_to_ssh.sh <服务器IP>
```

脚本会自动停止旧服务并启动新服务。

### 方法 2: 仅重新编译（代码已上传）

```bash
ssh root@<服务器IP> '~/qoder_libuv/sfu_server_full/build/sfu_manage.sh recompile'
ssh root@<服务器IP> '~/qoder_libuv/sfu_server_full/build/sfu_manage.sh restart'
```

## 性能优化

### 1. 修改编译优化级别

编辑 `sfu_server_full/CMakeLists.txt`，修改优化级别：

```cmake
# 开发模式（调试）
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -g")

# 发布模式（优化）
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -DNDEBUG")
```

### 2. 使用 systemd 管理（推荐）

创建 systemd 服务文件：

```bash
ssh root@<服务器IP>

cat > /etc/systemd/system/sfu-server.service << 'EOF'
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
EOF

# 启用服务
systemctl daemon-reload
systemctl enable sfu-server
systemctl start sfu-server

# 查看状态
systemctl status sfu-server

# 查看日志
journalctl -u sfu-server -f
```

## 监控和日志

### 查看实时日志

```bash
ssh root@<服务器IP> 'tail -f ~/qoder_libuv/sfu_server_full/build/sfu.log'
```

### 查看连接数

```bash
ssh root@<服务器IP> 'netstat -an | grep 9200 | wc -l'
```

### 查看带宽使用

```bash
ssh root@<服务器IP> 'iftop -i eth0'
```

## 安全建议

1. **修改默认端口** - 避免使用常见端口
2. **配置防火墙** - 只允许特定 IP 访问
3. **使用 TLS** - 加密传输数据（需要修改代码）
4. **定期更新** - 保持代码和依赖库最新
5. **监控日志** - 定期检查异常连接

## 技术支持

遇到问题请查看：
- 项目 README: `/Users/mac/Documents/qoder_libuv/README.md`
- 服务器文档: `/Users/mac/Documents/qoder_libuv/sfu_server_full/README.md`
