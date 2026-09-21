#!/bin/bash

# SFU 服务器管理脚本（部署到服务器后使用）
# 使用方法: ./sfu_manage.sh [start|stop|restart|status|log]

set -e

DEPLOY_DIR="$HOME/qoder_libuv/sfu_server_full/build"
PID_FILE="$DEPLOY_DIR/sfu.pid"
LOG_FILE="$DEPLOY_DIR/sfu.log"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

echo_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

echo_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

echo_debug() {
    echo -e "${BLUE}[DEBUG]${NC} $1"
}

usage() {
    echo "SFU 服务器管理工具"
    echo ""
    echo "使用方法: $0 <命令>"
    echo ""
    echo "命令:"
    echo "  start       启动服务器"
    echo "  stop        停止服务器"
    echo "  restart     重启服务器"
    echo "  status      查看状态"
    echo "  log         查看日志（实时）"
    echo "  log-tail    查看最后 100 行日志"
    echo "  recompile   重新编译"
    echo ""
    echo "示例:"
    echo "  $0 start"
    echo "  $0 log"
    echo "  $0 status"
}

# 检查服务器是否运行
is_running() {
    if [ -f "$PID_FILE" ]; then
        PID=$(cat "$PID_FILE")
        if kill -0 $PID 2>/dev/null; then
            return 0
        fi
    fi
    return 1
}

# 获取进程状态
get_status() {
    if is_running; then
        PID=$(cat "$PID_FILE")
        echo_info "服务器运行中"
        echo_info "PID: $PID"
        echo_info "端口: TCP 9200, UDP 9201"
        echo ""
        echo_debug "进程信息:"
        ps -p $PID -o pid,ppid,cmd,%mem,%cpu,etime
    else
        echo_warn "服务器未运行"
        if [ -f "$PID_FILE" ]; then
            echo_warn "PID 文件存在但进程不存在（可能异常退出）"
            rm -f "$PID_FILE"
        fi
    fi
}

# 启动服务器
start_server() {
    if is_running; then
        echo_warn "服务器已在运行 (PID: $(cat $PID_FILE))"
        return 0
    fi

    cd "$DEPLOY_DIR"

    if [ ! -f "sfu_server_full" ]; then
        echo_error "可执行文件不存在，请先编译"
        echo_info "运行: $0 recompile"
        return 1
    fi

    echo_info "启动 SFU 服务器..."
    nohup ./sfu_server_full --port 9200 > "$LOG_FILE" 2>&1 &
    echo $! > "$PID_FILE"

    sleep 2

    if is_running; then
        echo_info "✓ 服务器已启动 (PID: $(cat $PID_FILE))"
        echo_info "日志: $LOG_FILE"
        echo ""
        echo_debug "最近日志:"
        tail -10 "$LOG_FILE"
    else
        echo_error "✗ 服务器启动失败"
        echo_error "日志:"
        cat "$LOG_FILE"
        return 1
    fi
}

# 停止服务器
stop_server() {
    if ! is_running; then
        echo_warn "服务器未运行"
        rm -f "$PID_FILE"
        return 0
    fi

    PID=$(cat "$PID_FILE")
    echo_info "停止服务器 (PID: $PID)..."
    
    kill $PID
    
    # 等待进程退出
    for i in {1..10}; do
        if ! kill -0 $PID 2>/dev/null; then
            echo_info "✓ 服务器已停止"
            rm -f "$PID_FILE"
            return 0
        fi
        sleep 1
    done
    
    # 强制终止
    echo_warn "进程未响应，强制终止..."
    kill -9 $PID
    sleep 1
    rm -f "$PID_FILE"
    echo_info "✓ 服务器已强制停止"
}

# 重启服务器
restart_server() {
    echo_info "重启服务器..."
    stop_server
    sleep 1
    start_server
}

# 查看实时日志
view_log() {
    if [ ! -f "$LOG_FILE" ]; then
        echo_error "日志文件不存在: $LOG_FILE"
        return 1
    fi
    
    echo_info "查看实时日志 (Ctrl+C 退出)..."
    echo ""
    tail -f "$LOG_FILE"
}

# 查看最后日志
view_log_tail() {
    if [ ! -f "$LOG_FILE" ]; then
        echo_error "日志文件不存在: $LOG_FILE"
        return 1
    fi
    
    echo_info "最后 100 行日志:"
    echo ""
    tail -100 "$LOG_FILE"
}

# 重新编译
recompile() {
    cd "$HOME/qoder_libuv/sfu_server_full"
    
    echo_info "重新编译 SFU 服务器..."
    
    # 停止服务
    if is_running; then
        echo_info "停止当前服务..."
        stop_server
    fi
    
    # 编译
    cd build
    cmake ..
    make -j$(nproc)
    
    if [ -f "sfu_server_full" ]; then
        echo_info "✓ 编译成功"
        ls -lh sfu_server_full
    else
        echo_error "✗ 编译失败"
        return 1
    fi
}

# 主逻辑
case "${1:-}" in
    start)
        start_server
        ;;
    stop)
        stop_server
        ;;
    restart)
        restart_server
        ;;
    status)
        get_status
        ;;
    log)
        view_log
        ;;
    log-tail)
        view_log_tail
        ;;
    recompile)
        recompile
        ;;
    *)
        usage
        ;;
esac
