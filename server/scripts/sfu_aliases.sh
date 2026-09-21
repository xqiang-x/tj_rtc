#!/bin/bash

# SFU 服务器快捷命令参考
# 复制到你的终端配置文件中，或使用 source 命令加载

# ========================================
# 颜色定义
# ========================================
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# ========================================
# 配置（修改为你的服务器地址）
# ========================================
SFU_SERVER="sh"  # SSH 主机别名
SFU_SERVER_IP="106.15.177.248"  # 公网 IP
SFU_USER="root"

# ========================================
# 快捷命令
# ========================================

# 部署服务器
alias sfu-deploy='echo -e "${GREEN}部署 SFU 服务器到 ${SFU_SERVER}...${NC}" && cd /Users/mac/Documents/qoder_libuv && ./deploy_to_ssh.sh ${SFU_SERVER}'

# 查看服务器状态
alias sfu-status='echo -e "${GREEN}SFU 服务器状态:${NC}" && ssh ${SFU_USER}@${SFU_SERVER} "~/qoder_libuv/sfu_server_full/build/sfu_manage.sh status"'

# 启动服务器
alias sfu-start='echo -e "${GREEN}启动 SFU 服务器...${NC}" && ssh ${SFU_USER}@${SFU_SERVER} "~/qoder_libuv/sfu_server_full/build/sfu_manage.sh start"'

# 停止服务器
alias sfu-stop='echo -e "${YELLOW}停止 SFU 服务器...${NC}" && ssh ${SFU_USER}@${SFU_SERVER} "~/qoder_libuv/sfu_server_full/build/sfu_manage.sh stop"'

# 重启服务器
alias sfu-restart='echo -e "${GREEN}重启 SFU 服务器...${NC}" && ssh ${SFU_USER}@${SFU_SERVER} "~/qoder_libuv/sfu_server_full/build/sfu_manage.sh restart"'

# 查看实时日志
alias sfu-log='echo -e "${GREEN}查看 SFU 实时日志 (Ctrl+C 退出):${NC}" && ssh ${SFU_USER}@${SFU_SERVER} "~/qoder_libuv/sfu_server_full/build/sfu_manage.sh log"'

# 查看最后日志
alias sfu-logtail='echo -e "${GREEN}SFU 最后 100 行日志:${NC}" && ssh ${SFU_USER}@${SFU_SERVER} "~/qoder_libuv/sfu_server_full/build/sfu_manage.sh log-tail"'

# SSH 到服务器
alias sfu-ssh='echo -e "${BLUE}SSH 到 SFU 服务器...${NC}" && ssh ${SFU_USER}@${SFU_SERVER}'

# 查看服务器进程
alias sfu-ps='echo -e "${GREEN}SFU 服务器进程:${NC}" && ssh ${SFU_USER}@${SFU_SERVER} "ps aux | grep sfu_server | grep -v grep"'

# 查看端口占用
alias sfu-ports='echo -e "${GREEN}SFU 服务器端口:${NC}" && ssh ${SFU_USER}@${SFU_SERVER} "netstat -tlnp | grep -E \"9200|9201\""'

# 重新编译并部署
alias sfu-redeploy='echo -e "${GREEN}重新编译并部署 SFU 服务器...${NC}" && cd /Users/mac/Documents/qoder_libuv && ./deploy_to_ssh.sh ${SFU_SERVER}'

# 查看帮助
sfu-help() {
    echo -e "${GREEN}=========================================${NC}"
    echo -e "${GREEN}  SFU 服务器管理快捷命令${NC}"
    echo -e "${GREEN}=========================================${NC}"
    echo ""
    echo -e "${BLUE}部署:${NC}"
    echo "  sfu-deploy      部署服务器到 ${SFU_SERVER}"
    echo "  sfu-redeploy    重新编译并部署"
    echo ""
    echo -e "${BLUE}控制:${NC}"
    echo "  sfu-start       启动服务器"
    echo "  sfu-stop        停止服务器"
    echo "  sfu-restart     重启服务器"
    echo "  sfu-status      查看状态"
    echo ""
    echo -e "${BLUE}日志:${NC}"
    echo "  sfu-log         查看实时日志"
    echo "  sfu-logtail     查看最后 100 行"
    echo ""
    echo -e "${BLUE}调试:${NC}"
    echo "  sfu-ssh         SSH 到服务器"
    echo "  sfu-ps          查看进程"
    echo "  sfu-ports       查看端口"
    echo ""
    echo -e "${BLUE}配置:${NC}"
    echo "  SSH 别名: ${SFU_SERVER}"
    echo "  公网 IP: ${SFU_SERVER_IP}"
    echo "  TCP 端口: 9200"
    echo "  UDP 端口: 9201"
    echo ""
    echo -e "${YELLOW}提示: 编辑此文件修改 SFU_SERVER 变量${NC}"
}

# 显示帮助
sfu-help
