#include "StunService.h"
#include "SfuP2PProtocol.h"

#include <arpa/inet.h>
#include <cstring>
#include <chrono>
#include <glog/logging.h>

namespace sfu {

size_t StunService::HandlePacket(const uint8_t* data, size_t len,
                                 const struct sockaddr_in& clientAddr,
                                 uint8_t* response, size_t responseCapacity)
{
    m_requestCount++;

    auto hdr = ParseStunHeader(data, len);
    if (!hdr.valid) return 0;

    if (hdr.messageType != STUN_BINDING_REQUEST) {
        // 限速：STUN 端口收到畸形包时每包一条会刷屏
        static uint64_t last_unknown_log_us = 0;
        uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now_us - last_unknown_log_us >= 5'000'000) {
            LOG(ERROR) << "[STUN] Unknown message type: 0x" << std::hex << hdr.messageType << std::dec;
            last_unknown_log_us = now_us;
        }
        return 0;
    }

    char ipBuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));
    uint16_t mappedPort = ntohs(clientAddr.sin_port);

    auto resp = BuildStunBindingResponse(hdr.transactionId, ipBuf, mappedPort);
    if (resp.size() > responseCapacity) return 0;

    memcpy(response, resp.data(), resp.size());
    m_responseCount++;
    return resp.size();
}

} // namespace sfu
