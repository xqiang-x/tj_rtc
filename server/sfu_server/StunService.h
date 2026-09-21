#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <netinet/in.h>

namespace sfu {

// 轻量 STUN 服务 — 仅实现 Binding Request/Response（RFC 5389 子集）
// 无状态：收到请求 → 反射源地址 → 回复
class StunService {
public:
    // 处理一个 STUN 包
    // 返回要回复的字节数，0 表示不是有效 STUN 请求（不回复）
    // response 缓冲区调用方提供，至少 64 字节
    size_t HandlePacket(const uint8_t* data, size_t len,
                        const struct sockaddr_in& clientAddr,
                        uint8_t* response, size_t responseCapacity);

    uint64_t GetRequestCount() const { return m_requestCount; }
    uint64_t GetResponseCount() const { return m_responseCount; }

private:
    uint64_t m_requestCount = 0;
    uint64_t m_responseCount = 0;
};

} // namespace sfu
