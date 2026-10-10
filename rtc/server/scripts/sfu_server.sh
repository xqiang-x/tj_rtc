#!/bin/bash
# SFU 服务器管理脚本：start / stop / restart / status / log
# 布局：二进制 /root/bin，配置 /root/conf，日志 /root/log

BIN_DIR=/root/bin
CONF_DIR=/root/conf
LOG_DIR=/root/log
SHELL_DIR=/root/shell

BIN=$BIN_DIR/sfu_server_full
CONF=$CONF_DIR/sfu.conf
PID_FILE=$LOG_DIR/sfu.pid
LOG_FILE=$LOG_DIR/sfu.log

# 默认参数
TCP_PORT=9200
UDP_PORT=9201
STUN_PORT=9202
MAX_CLIENTS=1024
SESSION_TIMEOUT=3
STATS_INTERVAL=10
VERBOSE=0

load_conf() {
    if [ -f "$CONF" ]; then
        # shellcheck disable=SC1090
        . "$CONF"
    fi
}

make_args() {
    ARGS=(-p "$TCP_PORT")
    if [ -n "$UDP_PORT" ] && [ "$UDP_PORT" != "0" ]; then
        ARGS+=(-u "$UDP_PORT")
        if [ -n "$STUN_PORT" ] && [ "$STUN_PORT" != "0" ]; then
            ARGS+=(--stun-port "$STUN_PORT")
        fi
    fi
    ARGS+=(-m "$MAX_CLIENTS" -s "$STATS_INTERVAL" -t "$SESSION_TIMEOUT")
    ARGS+=(--log-dir "$LOG_DIR")
    if [ "$VERBOSE" = "1" ]; then
        ARGS+=(-v)
    fi
}

# 最新 glog INFO 文件
latest_glog_file() {
    ls -t "$LOG_DIR"/sfu_server_full.*INFO.* 2>/dev/null | head -1
}

start() {
    load_conf
    if [ -f "$PID_FILE" ]; then
        local old_pid
        old_pid=$(cat "$PID_FILE")
        if kill -0 "$old_pid" 2>/dev/null; then
            echo "sfu_server_full 已在运行 (PID: $old_pid)"
            return 1
        fi
        rm -f "$PID_FILE"
    fi
    make_args
    # 开启 core dump（崩溃时在 /root/cores 生成 core，便于定位 bad_alloc 等崩溃根因）
    ulimit -c unlimited
    mkdir -p /root/cores
    echo "启动 sfu_server_full: ${ARGS[*]}"
    nohup "$BIN" "${ARGS[@]}" >> "$LOG_FILE" 2>&1 &
    echo $! > "$PID_FILE"
    sleep 2
    if kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
        echo "✓ sfu_server_full 已启动 (PID: $(cat "$PID_FILE"))"
        echo "glog 日志: $(latest_glog_file)"
        tail -5 "$LOG_FILE"
    else
        echo "✗ 启动失败，最后日志:"
        tail -20 "$LOG_FILE"
        rm -f "$PID_FILE"
        return 1
    fi
}

stop() {
    if [ ! -f "$PID_FILE" ]; then
        echo "未在运行 (无 PID 文件)"
        return 0
    fi
    local pid
    pid=$(cat "$PID_FILE")
    if kill -0 "$pid" 2>/dev/null; then
        echo "停止 sfu_server_full (PID: $pid)..."
        kill "$pid"
        for _ in $(seq 1 10); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 1
        done
        if kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid"
        fi
    fi
    rm -f "$PID_FILE"
    echo "已停止"
}

status() {
    if [ -f "$PID_FILE" ]; then
        local pid
        pid=$(cat "$PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            echo "运行中 (PID: $pid)"
            ps -o pid,etime,args -p "$pid" | tail -1
            return 0
        fi
        echo "PID 文件存在但进程不存在 (PID: $pid)"
        return 1
    fi
    echo "未运行"
    return 1
}

case "${1:-}" in
    start)   start ;;
    stop)    stop ;;
    restart) stop; sleep 1; start ;;
    status)  status ;;
    log)     tail -f "$(latest_glog_file)" ;;
    log-tail) tail -100 "$(latest_glog_file)" ;;
    *)
        echo "用法: $0 {start|stop|restart|status|log|log-tail}"
        exit 1
        ;;
esac