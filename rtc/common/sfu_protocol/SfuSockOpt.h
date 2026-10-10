#pragma once

#include <sys/socket.h>
#include <cstdint>

namespace sfu {

// 尽力把 UDP 接收缓冲设为 targetBytes（默认 8MiB）。
// 特权进程走 SO_RCVBUFFORCE（精确值，不受 net.core.rmem_max 限制）；否则回退到
// SO_RCVBUF（内核会把请求值翻倍后钳制到 rmem_max，即“可设置的最大值”）。
// 返回实际生效的缓冲字节数（getsockopt 回读）。
inline int SetUdpRecvBuffer(int fd, int targetBytes = 8 * 1024 * 1024) {
#ifdef SO_RCVBUFFORCE
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &targetBytes, sizeof(targetBytes)) != 0)
#endif
    {
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &targetBytes, sizeof(targetBytes));
    }
    int actual = 0;
    socklen_t len = sizeof(actual);
    ::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &len);
    return actual;
}

} // namespace sfu
