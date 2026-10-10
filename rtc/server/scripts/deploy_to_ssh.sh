#!/bin/bash

# SFU 服务器 SSH 远程编译和部署脚本
# 使用方法: ./deploy_to_ssh.sh [服务器地址]

set -e

# 配置
SERVER_USER="root"
SERVER_HOST="${1:-}"
DEPLOY_DIR="~/qoder_libuv"
PROJECT_DIR="/Users/mac/Documents/qoder_libuv"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

echo_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

echo_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 检查参数
if [ -z "$SERVER_HOST" ]; then
    echo_error "请提供服务器地址"
    echo "使用方法: $0 <服务器IP或域名>"
    echo "示例: $0 192.168.1.100"
    exit 1
fi

echo_info "========================================="
echo_info "  SFU 服务器 SSH 远程部署"
echo_info "========================================="
echo_info "服务器: ${SERVER_USER}@${SERVER_HOST}"
echo_info "部署目录: ${DEPLOY_DIR}"
echo ""

# 步骤 1: 测试 SSH 连接
echo_info "步骤 1/5: 测试 SSH 连接..."
if ! ssh -o ConnectTimeout=5 -o BatchMode=yes ${SERVER_USER}@${SERVER_HOST} "echo 'SSH 连接成功'" 2>/dev/null; then
    echo_error "SSH 连接失败，请检查:"
    echo "  1. 服务器地址是否正确"
    echo "  2. 是否已设置免密登录"
    echo "  3. SSH 服务是否运行"
    exit 1
fi
echo_info "✓ SSH 连接正常"
echo ""

# 步骤 2: 在服务器上创建目录
echo_info "步骤 2/5: 在服务器上创建目录..."
ssh ${SERVER_USER}@${SERVER_HOST} << 'EOF'
mkdir -p ~/qoder_libuv/sfu_server_full
mkdir -p ~/qoder_libuv/sfu_server
mkdir -p ~/qoder_libuv/frame_protoc/src
mkdir -p ~/qoder_libuv/frame_protoc/cm256cc
mkdir -p ~/qoder_libuv/pacer
mkdir -p ~/qoder_libuv/sfu_server_full/build
EOF
echo_info "✓ 目录创建完成"
echo ""

# 步骤 3: 上传源文件
echo_info "步骤 3/5: 上传源文件到服务器..."

# sfu_server_full
echo_info "  上传 sfu_server_full..."
scp ${PROJECT_DIR}/sfu_server_full/CMakeLists.txt ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/sfu_server_full/
scp ${PROJECT_DIR}/sfu_server_full/SfuServer.cpp ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/sfu_server_full/
scp ${PROJECT_DIR}/sfu_server_full/SfuServer.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/sfu_server_full/
scp ${PROJECT_DIR}/sfu_server_full/main.cpp ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/sfu_server_full/

# sfu_server (库)
echo_info "  上传 sfu_server..."
scp ${PROJECT_DIR}/sfu_server/SfuProtocol.cpp ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/sfu_server/
scp ${PROJECT_DIR}/sfu_server/SfuProtocol.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/sfu_server/
scp ${PROJECT_DIR}/sfu_server/SfuKvStore.cpp ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/sfu_server/
scp ${PROJECT_DIR}/sfu_server/SfuKvStore.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/sfu_server/
scp ${PROJECT_DIR}/sfu_server/SfuEvPool.cpp ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/sfu_server/
scp ${PROJECT_DIR}/sfu_server/SfuEvPool.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/sfu_server/
scp ${PROJECT_DIR}/sfu_server/SfuServerBase.cpp ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/sfu_server/
scp ${PROJECT_DIR}/sfu_server/SfuServerBase.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/sfu_server/
scp ${PROJECT_DIR}/sfu_server/SfuConnManager.cpp ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/sfu_server/
scp ${PROJECT_DIR}/sfu_server/SfuConnManager.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/sfu_server/
scp ${PROJECT_DIR}/sfu_server/SfuFrameType.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/sfu_server/
scp ${PROJECT_DIR}/sfu_server/SfuSubscribeProtocol.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/sfu_server/

# frame_protoc (FEC)
echo_info "  上传 frame_protoc..."
scp ${PROJECT_DIR}/frame_protoc/src/protocol.cpp ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/frame_protoc/src/
scp ${PROJECT_DIR}/frame_protoc/src/protocol.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/frame_protoc/src/
scp ${PROJECT_DIR}/frame_protoc/src/sender.cpp ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/frame_protoc/src/
scp ${PROJECT_DIR}/frame_protoc/src/sender.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/frame_protoc/src/
scp ${PROJECT_DIR}/frame_protoc/src/receiver.cpp ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/frame_protoc/src/
scp ${PROJECT_DIR}/frame_protoc/src/receiver.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/frame_protoc/src/
scp ${PROJECT_DIR}/frame_protoc/src/proxy_receiver.cpp ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/frame_protoc/src/
scp ${PROJECT_DIR}/frame_protoc/src/proxy_receiver.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/frame_protoc/src/
scp ${PROJECT_DIR}/frame_protoc/src/config.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/frame_protoc/src/
scp ${PROJECT_DIR}/frame_protoc/src/types.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/frame_protoc/src/
scp ${PROJECT_DIR}/frame_protoc/src/stats.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/frame_protoc/src/
scp ${PROJECT_DIR}/frame_protoc/cm256cc/gf256.cpp ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/frame_protoc/cm256cc/
scp ${PROJECT_DIR}/frame_protoc/cm256cc/gf256.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/frame_protoc/cm256cc/
scp ${PROJECT_DIR}/frame_protoc/cm256cc/cm256.cpp ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/frame_protoc/cm256cc/
scp ${PROJECT_DIR}/frame_protoc/cm256cc/cm256.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/frame_protoc/cm256cc/
scp ${PROJECT_DIR}/frame_protoc/cm256cc/export.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/frame_protoc/cm256cc/
scp ${PROJECT_DIR}/frame_protoc/cm256cc/sse2neon.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/frame_protoc/cm256cc/

# pacer
echo_info "  上传 pacer..."
scp ${PROJECT_DIR}/pacer/Pacer.cpp ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/pacer/
scp ${PROJECT_DIR}/pacer/Pacer.h ${SERVER_USER}@${SERVER_HOST}:${DEPLOY_DIR}/pacer/

echo_info "✓ 文件上传完成"
echo ""

# 步骤 4: 远程编译
echo_info "步骤 4/5: 在服务器上编译..."
ssh ${SERVER_USER}@${SERVER_HOST} << 'REMOTE_SCRIPT'
cd ~/qoder_libuv/sfu_server_full/build

echo "检查依赖..."
# 检查是否有 cmake
if ! command -v cmake &> /dev/null; then
    echo "安装 cmake..."
    if command -v apt-get &> /dev/null; then
        sudo apt-get update && sudo apt-get install -y cmake build-essential libev-dev
    elif command -v yum &> /dev/null; then
        sudo yum install -y cmake gcc-c++ libev-devel
    elif command -v dnf &> /dev/null; then
        sudo dnf install -y cmake gcc-c++ libev-devel
    else
        echo "错误: 无法自动安装 cmake，请手动安装"
        exit 1
    fi
fi

# 检查 libev
if [ ! -f /usr/include/ev.h ] && [ ! -f /usr/local/include/ev.h ]; then
    echo "安装 libev..."
    if command -v apt-get &> /dev/null; then
        sudo apt-get install -y libev-dev
    elif command -v yum &> /dev/null; then
        sudo yum install -y libev-devel
    elif command -v dnf &> /dev/null; then
        sudo dnf install -y libev-devel
    else
        echo "错误: 无法自动安装 libev，请手动安装"
        exit 1
    fi
fi

echo "开始编译..."
cmake ..
make -j$(nproc)

if [ -f "sfu_server_full" ]; then
    echo "✓ 编译成功"
    ls -lh sfu_server_full
else
    echo "✗ 编译失败"
    exit 1
fi
REMOTE_SCRIPT

echo_info "✓ 远程编译完成"
echo ""

# 步骤 5: 启动服务器
echo_info "步骤 5/5: 启动 SFU 服务器..."
echo ""
echo_warn "服务器将在后台运行"
echo_warn "使用以下命令查看日志: ssh ${SERVER_USER}@${SERVER_HOST} 'tail -f ~/qoder_libuv/sfu_server_full/build/sfu.log'"
echo_warn "使用以下命令停止服务器: ssh ${SERVER_USER}@${SERVER_HOST} 'kill \$(cat ~/qoder_libuv/sfu_server_full/build/sfu.pid)'"
echo ""

ssh ${SERVER_USER}@${SERVER_HOST} << 'START_SCRIPT'
cd ~/qoder_libuv/sfu_server_full/build

# 检查是否已在运行
if [ -f sfu.pid ]; then
    OLD_PID=$(cat sfu.pid)
    if kill -0 $OLD_PID 2>/dev/null; then
        echo "停止旧服务器进程 (PID: $OLD_PID)..."
        kill $OLD_PID
        sleep 2
    fi
fi

# 启动服务器
echo "启动 SFU 服务器..."
nohup ./sfu_server_full --port 9200 > sfu.log 2>&1 &
echo $! > sfu.pid

sleep 2

# 检查是否启动成功
if kill -0 $(cat sfu.pid) 2>/dev/null; then
    echo "✓ SFU 服务器已启动 (PID: $(cat sfu.pid))"
    echo ""
    echo "服务器信息:"
    echo "  - TCP 端口: 9200"
    echo "  - UDP 端口: 9201"
    echo "  - 日志文件: ~/qoder_libuv/sfu_server_full/build/sfu.log"
    echo ""
    echo "最近日志:"
    tail -20 sfu.log
else
    echo "✗ 服务器启动失败，查看日志:"
    cat sfu.log
    exit 1
fi
START_SCRIPT

echo ""
echo_info "========================================="
echo_info "  ✅ 部署完成！"
echo_info "========================================="
echo ""
echo_info "服务器地址: ${SERVER_HOST}"
echo_info "TCP 端口: 9200"
echo_info "UDP 端口: 9201"
echo ""
echo_info "常用命令:"
echo_info "  查看日志:   ssh ${SERVER_USER}@${SERVER_HOST} 'tail -f ~/qoder_libuv/sfu_server_full/build/sfu.log'"
echo_info "  停止服务:   ssh ${SERVER_USER}@${SERVER_HOST} 'kill \$(cat ~/qoder_libuv/sfu_server_full/build/sfu.pid)'"
echo_info "  重启服务:   ssh ${SERVER_USER}@${SERVER_HOST} 'cd ~/qoder_libuv/sfu_server_full/build && ./restart.sh'"
echo_info "  查看进程:   ssh ${SERVER_USER}@${SERVER_HOST} 'ps aux | grep sfu_server'"
echo ""
