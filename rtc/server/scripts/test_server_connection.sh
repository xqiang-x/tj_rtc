#!/bin/bash

# SFU 服务器连接测试脚本
# 测试服务器是否可以从外网正常访问

set -e

SERVER_IP="106.15.177.248"
TCP_PORT=9200
UDP_PORT=9201

# 颜色
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

echo_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

echo_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

echo_debug() {
    echo -e "${BLUE}[DEBUG]${NC} $1"
}

echo "========================================="
echo "  SFU 服务器连接测试"
echo "========================================="
echo "服务器: ${SERVER_IP}"
echo "TCP 端口: ${TCP_PORT}"
echo "UDP 端口: ${UDP_PORT}"
echo ""

# 测试 1: TCP 端口
echo_debug "测试 1: TCP 端口 ${TCP_PORT}..."
if nc -z -w 3 ${SERVER_IP} ${TCP_PORT} 2>/dev/null; then
    echo_info "✓ TCP ${TCP_PORT} 可达"
else
    echo_error "✗ TCP ${TCP_PORT} 不可达"
    echo_warn "请检查:"
    echo_warn "  1. 服务器是否运行"
    echo_warn "  2. 安全组是否开放 TCP ${TCP_PORT}"
    echo_warn "  3. 防火墙设置"
    exit 1
fi

# 测试 2: UDP 端口
echo_debug "测试 2: UDP 端口 ${UDP_PORT}..."
if nc -z -u -w 3 ${SERVER_IP} ${UDP_PORT} 2>/dev/null; then
    echo_info "✓ UDP ${UDP_PORT} 可达"
else
    echo_warn "⚠ UDP 端口测试完成（需要实际数据验证）"
fi

# 测试 3: TCP 连接握手
echo_debug "测试 3: TCP 连接握手..."
(
    echo -ne "\x01\x00\x00\x00" | nc -w 3 ${SERVER_IP} ${TCP_PORT} > /dev/null 2>&1
    echo $?
) | while read exit_code; do
    if [ "$exit_code" = "0" ]; then
        echo_info "✓ TCP 握手成功"
    else
        echo_warn "⚠ TCP 握手失败（可能正常，取决于协议）"
    fi
done

echo ""
echo_info "========================================="
echo_info "  ✅ 服务器连接测试完成"
echo_info "========================================="
echo ""
echo_info "服务器配置信息:"
echo_info "  公网 IP: ${SERVER_IP}"
echo_info "  TCP 端口: ${TCP_PORT} (客户端连接)"
echo_info "  UDP 端口: ${UDP_PORT} (音视频流)"
echo ""
echo_info "客户端配置示例:"
echo_info "  macOS 推流: server=${SERVER_IP}, port=${TCP_PORT}"
echo_info "  macOS 拉流: server=${SERVER_IP}, port=${TCP_PORT}"
echo_info "  Android 应用: ${SERVER_IP}:${TCP_PORT}"
echo ""
echo_warn "下一步:"
echo_warn "  1. 使用 mac_camera_push 推流测试"
echo_warn "  2. 使用 mac_pull_test 拉流测试"
echo_warn "  3. 使用 Android 应用测试"
echo ""
