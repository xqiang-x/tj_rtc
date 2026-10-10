#ifndef SFU_P2P_PROTOCOL_H
#define SFU_P2P_PROTOCOL_H

#include "SfuProtocol.h"
#include "SfuSubscribeProtocol.h"

#include <cstdint>
#include <string>
#include <vector>
#include <cstring>
#include <arpa/inet.h>

namespace sfu {

// ===== P2P Data Packet Header =====
// P2P 直连数据包的极简头部（区别于 SFU 中继包）
// [1 byte: version=0x02] [1 byte: pkt_type] [N bytes: raw FEC packet]

enum class P2PPacketType : uint8_t {
    kData      = 0x01,  // FEC 数据包
    kProbe     = 0x02,  // 打洞探测包
    kHeartbeat = 0x03,  // P2P 心跳保活
};

static constexpr uint8_t P2P_PROTOCOL_VERSION = 0x02;

inline std::vector<uint8_t> SerializeP2PDataPacket(const uint8_t* fecPayload, size_t len) {
    std::vector<uint8_t> buf(2 + len);
    buf[0] = P2P_PROTOCOL_VERSION;
    buf[1] = static_cast<uint8_t>(P2PPacketType::kData);
    if (len > 0) {
        memcpy(buf.data() + 2, fecPayload, len);
    }
    return buf;
}

inline std::vector<uint8_t> SerializeP2PProbe(const uint8_t* payload, size_t len) {
    std::vector<uint8_t> buf(2 + len);
    buf[0] = P2P_PROTOCOL_VERSION;
    buf[1] = static_cast<uint8_t>(P2PPacketType::kProbe);
    if (len > 0) {
        memcpy(buf.data() + 2, payload, len);
    }
    return buf;
}

inline std::vector<uint8_t> SerializeP2PHeartbeat() {
    return {P2P_PROTOCOL_VERSION, static_cast<uint8_t>(P2PPacketType::kHeartbeat)};
}

struct P2PPacket {
    P2PPacketType type = P2PPacketType::kData;
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;
    bool valid = false;
};

inline P2PPacket ParseP2PPacket(const uint8_t* data, size_t len) {
    P2PPacket pkt;
    if (len < 2 || data[0] != P2P_PROTOCOL_VERSION) return pkt;
    pkt.type = static_cast<P2PPacketType>(data[1]);
    pkt.payload = data + 2;
    pkt.payloadLen = len - 2;
    pkt.valid = true;
    return pkt;
}

// ===== kP2PRequest: Client -> Server =====
// [1 byte: ctrlType=0x17]
// [4 bytes: targetStreamId length] [N bytes: targetStreamId]（按流名寻址推流端）
// [1 byte: flags]  bit0=supports_stun, bit1=supports_holepunch

struct P2PRequest {
    std::string targetStreamId;
    uint8_t flags = 0x03;  // default: supports both
    bool valid = false;
};

inline std::vector<uint8_t> SerializeP2PRequest(const std::string& targetStreamId, uint8_t flags = 0x03) {
    std::vector<uint8_t> buf;
    buf.push_back(MakeHeader(PROTOCOL_VERSION, static_cast<uint8_t>(MsgType::kCmd)));
    buf.push_back(static_cast<uint8_t>(SfuCtrlType::kP2PRequest));

    uint32_t idLen = static_cast<uint32_t>(targetStreamId.size());
    buf.push_back((idLen >> 24) & 0xFF);
    buf.push_back((idLen >> 16) & 0xFF);
    buf.push_back((idLen >> 8) & 0xFF);
    buf.push_back(idLen & 0xFF);
    buf.insert(buf.end(), targetStreamId.begin(), targetStreamId.end());

    buf.push_back(flags);
    return buf;
}

inline P2PRequest ParseP2PRequest(const uint8_t* data, size_t len) {
    P2PRequest req;
    size_t offset = 0;
    if (len < 1 + 4) return req;

    uint8_t ctrlType = data[offset++];
    if (ctrlType != static_cast<uint8_t>(SfuCtrlType::kP2PRequest)) return req;

    uint32_t idLen = (data[offset] << 24) | (data[offset+1] << 16) |
                     (data[offset+2] << 8) | data[offset+3];
    offset += 4;
    if (offset + idLen + 1 > len) return req;
    req.targetStreamId.assign(reinterpret_cast<const char*>(data + offset), idLen);
    offset += idLen;
    req.flags = data[offset++];
    req.valid = true;
    return req;
}

// ===== kP2PResponse: Server -> Client =====
// [1 byte: ctrlType=0x18]
// [1 byte: status]  0=ok, 1=peer_not_found, 2=peer_not_p2p_capable
// [4 bytes: peer_session_id]
// [4 bytes: peer_ip_len] [N bytes: peer_ip]
// [2 bytes: peer_port]
// [4 bytes: your_ip_len] [N bytes: your_ip]
// [2 bytes: your_port]
// [1 byte: nat_type]  0=unknown, 1=full_cone, 2=restricted, 3=symmetric
// [1 byte: subscriber_count]

struct P2PAddress {
    std::string ip;
    uint16_t port = 0;
};

struct P2PResponse {
    uint8_t status = 0;
    uint32_t peerSessionId = 0;
    P2PAddress peerAddr;
    P2PAddress yourAddr;
    uint8_t natType = 0;
    uint8_t subscriberCount = 0;
    bool valid = false;
};

inline std::vector<uint8_t> SerializeP2PResponse(
    uint8_t status, uint32_t peerSessionId,
    const P2PAddress& peerAddr, const P2PAddress& yourAddr,
    uint8_t natType, uint8_t subscriberCount)
{
    std::vector<uint8_t> buf;
    buf.push_back(MakeHeader(PROTOCOL_VERSION, static_cast<uint8_t>(MsgType::kCmd)));
    buf.push_back(static_cast<uint8_t>(SfuCtrlType::kP2PResponse));

    buf.push_back(status);

    buf.push_back((peerSessionId >> 24) & 0xFF);
    buf.push_back((peerSessionId >> 16) & 0xFF);
    buf.push_back((peerSessionId >> 8) & 0xFF);
    buf.push_back(peerSessionId & 0xFF);

    auto writeAddr = [&](const P2PAddress& addr) {
        uint32_t ipLen = static_cast<uint32_t>(addr.ip.size());
        buf.push_back((ipLen >> 24) & 0xFF);
        buf.push_back((ipLen >> 16) & 0xFF);
        buf.push_back((ipLen >> 8) & 0xFF);
        buf.push_back(ipLen & 0xFF);
        buf.insert(buf.end(), addr.ip.begin(), addr.ip.end());
        buf.push_back((addr.port >> 8) & 0xFF);
        buf.push_back(addr.port & 0xFF);
    };

    writeAddr(peerAddr);
    writeAddr(yourAddr);

    buf.push_back(natType);
    buf.push_back(subscriberCount);
    return buf;
}

inline P2PResponse ParseP2PResponse(const uint8_t* data, size_t len) {
    P2PResponse resp;
    size_t offset = 0;
    if (len < 1) return resp;
    offset++;  // skip header byte

    if (len < offset + 1 + 4) return resp;
    resp.status = data[offset++];

    resp.peerSessionId = (data[offset] << 24) | (data[offset+1] << 16) |
                         (data[offset+2] << 8) | data[offset+3];
    offset += 4;

    auto readAddr = [&](P2PAddress& addr) -> bool {
        if (offset + 4 > len) return false;
        uint32_t ipLen = (data[offset] << 24) | (data[offset+1] << 16) |
                         (data[offset+2] << 8) | data[offset+3];
        offset += 4;
        if (offset + ipLen + 2 > len) return false;
        addr.ip.assign(reinterpret_cast<const char*>(data + offset), ipLen);
        offset += ipLen;
        addr.port = (data[offset] << 8) | data[offset+1];
        offset += 2;
        return true;
    };

    if (!readAddr(resp.peerAddr)) return resp;
    if (!readAddr(resp.yourAddr)) return resp;

    if (offset + 2 > len) return resp;
    resp.natType = data[offset++];
    resp.subscriberCount = data[offset++];
    resp.valid = true;
    return resp;
}

// ===== kP2PCandidate: Bidirectional via server =====
// [1 byte: ctrlType=0x19]
// [4 bytes: target_session_id]
// [4 bytes: ip_len] [N bytes: ip]
// [2 bytes: port]
// [1 byte: candidate_type]  0=host, 1=server_reflexive, 2=peer_reflexive, 3=relay
// [4 bytes: priority]

struct P2PCandidate {
    uint32_t targetSessionId = 0;
    std::string ip;
    uint16_t port = 0;
    uint8_t candidateType = 1;  // default: server-reflexive
    uint32_t priority = 0;
    bool valid = false;
};

inline std::vector<uint8_t> SerializeP2PCandidate(
    uint32_t targetSessionId, const std::string& ip, uint16_t port,
    uint8_t candidateType, uint32_t priority)
{
    std::vector<uint8_t> buf;
    buf.push_back(MakeHeader(PROTOCOL_VERSION, static_cast<uint8_t>(MsgType::kCmd)));
    buf.push_back(static_cast<uint8_t>(SfuCtrlType::kP2PCandidate));

    buf.push_back((targetSessionId >> 24) & 0xFF);
    buf.push_back((targetSessionId >> 16) & 0xFF);
    buf.push_back((targetSessionId >> 8) & 0xFF);
    buf.push_back(targetSessionId & 0xFF);

    uint32_t ipLen = static_cast<uint32_t>(ip.size());
    buf.push_back((ipLen >> 24) & 0xFF);
    buf.push_back((ipLen >> 16) & 0xFF);
    buf.push_back((ipLen >> 8) & 0xFF);
    buf.push_back(ipLen & 0xFF);
    buf.insert(buf.end(), ip.begin(), ip.end());

    buf.push_back((port >> 8) & 0xFF);
    buf.push_back(port & 0xFF);
    buf.push_back(candidateType);

    buf.push_back((priority >> 24) & 0xFF);
    buf.push_back((priority >> 16) & 0xFF);
    buf.push_back((priority >> 8) & 0xFF);
    buf.push_back(priority & 0xFF);

    return buf;
}

inline P2PCandidate ParseP2PCandidate(const uint8_t* data, size_t len) {
    P2PCandidate cand;
    size_t offset = 0;
    if (len < 1) return cand;
    offset++;  // skip header

    if (len < offset + 4) return cand;
    cand.targetSessionId = (data[offset] << 24) | (data[offset+1] << 16) |
                           (data[offset+2] << 8) | data[offset+3];
    offset += 4;

    if (offset + 4 > len) return cand;
    uint32_t ipLen = (data[offset] << 24) | (data[offset+1] << 16) |
                     (data[offset+2] << 8) | data[offset+3];
    offset += 4;
    if (offset + ipLen + 2 + 1 + 4 > len) return cand;

    cand.ip.assign(reinterpret_cast<const char*>(data + offset), ipLen);
    offset += ipLen;

    cand.port = (data[offset] << 8) | data[offset+1];
    offset += 2;
    cand.candidateType = data[offset++];
    cand.priority = (data[offset] << 24) | (data[offset+1] << 16) |
                    (data[offset+2] << 8) | data[offset+3];
    cand.valid = true;
    return cand;
}

// ===== kP2PFallback: Server -> Client =====
// [1 byte: ctrlType=0x1B]
// [1 byte: reason]  0=new_subscriber, 1=quality_degraded, 2=admin_override
// [4 bytes: publisher_session_id]

struct P2PFallback {
    uint8_t reason = 0;
    uint32_t publisherSessionId = 0;
    bool valid = false;
};

inline std::vector<uint8_t> SerializeP2PFallback(uint8_t reason, uint32_t publisherSessionId) {
    std::vector<uint8_t> buf;
    buf.push_back(MakeHeader(PROTOCOL_VERSION, static_cast<uint8_t>(MsgType::kCmd)));
    buf.push_back(static_cast<uint8_t>(SfuCtrlType::kP2PFallback));
    buf.push_back(reason);
    buf.push_back((publisherSessionId >> 24) & 0xFF);
    buf.push_back((publisherSessionId >> 16) & 0xFF);
    buf.push_back((publisherSessionId >> 8) & 0xFF);
    buf.push_back(publisherSessionId & 0xFF);
    return buf;
}

inline P2PFallback ParseP2PFallback(const uint8_t* data, size_t len) {
    P2PFallback fb;
    size_t offset = 0;
    if (len < 1) return fb;
    offset++;  // skip header

    if (len < offset + 1 + 4) return fb;
    fb.reason = data[offset++];
    fb.publisherSessionId = (data[offset] << 24) | (data[offset+1] << 16) |
                            (data[offset+2] << 8) | data[offset+3];
    fb.valid = true;
    return fb;
}

// ===== kP2PConnectCheck =====
// [1 byte: ctrlType=0x1A]
// [4 bytes: peer_session_id]
// [1 byte: success]  0=fail, 1=success
// [2 bytes: rtt_ms]

struct P2PConnectCheck {
    uint32_t peerSessionId = 0;
    bool success = false;
    uint16_t rttMs = 0;
    bool valid = false;
};

inline std::vector<uint8_t> SerializeP2PConnectCheck(uint32_t peerSessionId, bool success, uint16_t rttMs) {
    std::vector<uint8_t> buf;
    buf.push_back(MakeHeader(PROTOCOL_VERSION, static_cast<uint8_t>(MsgType::kCmd)));
    buf.push_back(static_cast<uint8_t>(SfuCtrlType::kP2PConnectCheck));
    buf.push_back((peerSessionId >> 24) & 0xFF);
    buf.push_back((peerSessionId >> 16) & 0xFF);
    buf.push_back((peerSessionId >> 8) & 0xFF);
    buf.push_back(peerSessionId & 0xFF);
    buf.push_back(success ? 1 : 0);
    buf.push_back((rttMs >> 8) & 0xFF);
    buf.push_back(rttMs & 0xFF);
    return buf;
}

inline P2PConnectCheck ParseP2PConnectCheck(const uint8_t* data, size_t len) {
    P2PConnectCheck cc;
    size_t offset = 0;
    if (len < 1) return cc;
    offset++;  // skip header

    if (len < offset + 4 + 1 + 2) return cc;
    cc.peerSessionId = (data[offset] << 24) | (data[offset+1] << 16) |
                       (data[offset+2] << 8) | data[offset+3];
    offset += 4;
    cc.success = data[offset++] != 0;
    cc.rttMs = (data[offset] << 8) | data[offset+1];
    cc.valid = true;
    return cc;
}

// ===== kP2PStatus: Client -> Server quality report =====
// [1 byte: ctrlType=0x1C]
// [4 bytes: peer_session_id]
// [1 byte: phase]  current migration phase (0-4)
// [1 byte: loss_rate]  0-255 mapped to 0.0-1.0
// [2 bytes: rtt_ms]
// [1 byte: quality_score]  0-100

struct P2PStatusReport {
    uint32_t peerSessionId = 0;
    uint8_t phase = 0;
    uint8_t lossRate = 0;
    uint16_t rttMs = 0;
    uint8_t qualityScore = 0;
    bool valid = false;
};

inline std::vector<uint8_t> SerializeP2PStatus(
    uint32_t peerSessionId, uint8_t phase,
    uint8_t lossRate, uint16_t rttMs, uint8_t qualityScore)
{
    std::vector<uint8_t> buf;
    buf.push_back(MakeHeader(PROTOCOL_VERSION, static_cast<uint8_t>(MsgType::kCmd)));
    buf.push_back(static_cast<uint8_t>(SfuCtrlType::kP2PStatus));
    buf.push_back((peerSessionId >> 24) & 0xFF);
    buf.push_back((peerSessionId >> 16) & 0xFF);
    buf.push_back((peerSessionId >> 8) & 0xFF);
    buf.push_back(peerSessionId & 0xFF);
    buf.push_back(phase);
    buf.push_back(lossRate);
    buf.push_back((rttMs >> 8) & 0xFF);
    buf.push_back(rttMs & 0xFF);
    buf.push_back(qualityScore);
    return buf;
}

inline P2PStatusReport ParseP2PStatus(const uint8_t* data, size_t len) {
    P2PStatusReport rpt;
    size_t offset = 0;
    if (len < 1) return rpt;
    offset++;  // skip header

    if (len < offset + 4 + 1 + 1 + 2 + 1) return rpt;
    rpt.peerSessionId = (data[offset] << 24) | (data[offset+1] << 16) |
                        (data[offset+2] << 8) | data[offset+3];
    offset += 4;
    rpt.phase = data[offset++];
    rpt.lossRate = data[offset++];
    rpt.rttMs = (data[offset] << 8) | data[offset+1];
    offset += 2;
    rpt.qualityScore = data[offset++];
    rpt.valid = true;
    return rpt;
}

// ===== STUN Wire Format (RFC 5389 subset) =====

static constexpr uint32_t STUN_MAGIC_COOKIE = 0x2112A442;
static constexpr uint16_t STUN_BINDING_REQUEST  = 0x0001;
static constexpr uint16_t STUN_BINDING_RESPONSE = 0x0101;
static constexpr uint16_t STUN_ATTR_XOR_MAPPED_ADDRESS = 0x0020;

// 构造 STUN Binding Request（20 字节）
inline std::vector<uint8_t> BuildStunBindingRequest(const uint8_t transactionId[12]) {
    std::vector<uint8_t> pkt(20);
    // Message Type
    pkt[0] = (STUN_BINDING_REQUEST >> 8) & 0xFF;
    pkt[1] = STUN_BINDING_REQUEST & 0xFF;
    // Message Length (0, no attributes)
    pkt[2] = 0;
    pkt[3] = 0;
    // Magic Cookie
    pkt[4] = (STUN_MAGIC_COOKIE >> 24) & 0xFF;
    pkt[5] = (STUN_MAGIC_COOKIE >> 16) & 0xFF;
    pkt[6] = (STUN_MAGIC_COOKIE >> 8) & 0xFF;
    pkt[7] = STUN_MAGIC_COOKIE & 0xFF;
    // Transaction ID
    memcpy(pkt.data() + 8, transactionId, 12);
    return pkt;
}

// 构造 STUN Binding Response（含 XOR-MAPPED-ADDRESS）
inline std::vector<uint8_t> BuildStunBindingResponse(
    const uint8_t transactionId[12],
    const std::string& mappedIp, uint16_t mappedPort)
{
    // XOR the address with magic cookie
    uint32_t ip = inet_addr(mappedIp.c_str());
    uint32_t xorIp = ip ^ htonl(STUN_MAGIC_COOKIE);  // network byte order XOR
    uint16_t xorPort = mappedPort ^ (STUN_MAGIC_COOKIE >> 16);

    std::vector<uint8_t> pkt(32);
    // Message Type = Binding Response
    pkt[0] = (STUN_BINDING_RESPONSE >> 8) & 0xFF;
    pkt[1] = STUN_BINDING_RESPONSE & 0xFF;
    // Message Length = 12 (XOR-MAPPED-ADDRESS attribute)
    pkt[2] = 0;
    pkt[3] = 12;
    // Magic Cookie
    pkt[4] = (STUN_MAGIC_COOKIE >> 24) & 0xFF;
    pkt[5] = (STUN_MAGIC_COOKIE >> 16) & 0xFF;
    pkt[6] = (STUN_MAGIC_COOKIE >> 8) & 0xFF;
    pkt[7] = STUN_MAGIC_COOKIE & 0xFF;
    // Transaction ID
    memcpy(pkt.data() + 8, transactionId, 12);

    // XOR-MAPPED-ADDRESS attribute
    size_t off = 20;
    // Attribute Type
    pkt[off++] = (STUN_ATTR_XOR_MAPPED_ADDRESS >> 8) & 0xFF;
    pkt[off++] = STUN_ATTR_XOR_MAPPED_ADDRESS & 0xFF;
    // Attribute Length
    pkt[off++] = 0;
    pkt[off++] = 8;
    // Reserved
    pkt[off++] = 0;
    // Family: IPv4
    pkt[off++] = 0x01;
    // XOR Port
    pkt[off++] = (xorPort >> 8) & 0xFF;
    pkt[off++] = xorPort & 0xFF;
    // XOR Address
    pkt[off++] = (xorIp >> 24) & 0xFF;
    pkt[off++] = (xorIp >> 16) & 0xFF;
    pkt[off++] = (xorIp >> 8) & 0xFF;
    pkt[off++] = xorIp & 0xFF;

    return pkt;
}

// 解析 STUN 消息头部，返回 message_type 和 transaction_id
struct StunHeader {
    uint16_t messageType = 0;
    uint16_t messageLength = 0;
    uint8_t transactionId[12] = {};
    bool valid = false;
};

inline StunHeader ParseStunHeader(const uint8_t* data, size_t len) {
    StunHeader hdr;
    if (len < 20) return hdr;
    hdr.messageType = (data[0] << 8) | data[1];
    hdr.messageLength = (data[2] << 8) | data[3];
    // Verify magic cookie
    uint32_t cookie = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
    if (cookie != STUN_MAGIC_COOKIE) return hdr;
    memcpy(hdr.transactionId, data + 8, 12);
    hdr.valid = true;
    return hdr;
}

// 从 STUN Binding Response 中提取 XOR-MAPPED-ADDRESS
struct StunMappedAddress {
    std::string ip;
    uint16_t port = 0;
    bool valid = false;
};

inline StunMappedAddress ParseStunMappedAddress(const uint8_t* data, size_t len, const uint8_t transactionId[12]) {
    StunMappedAddress addr;
    // Attributes start at offset 20
    size_t off = 20;
    while (off + 4 <= len) {
        uint16_t attrType = (data[off] << 8) | data[off+1];
        uint16_t attrLen = (data[off+2] << 8) | data[off+3];
        off += 4;
        if (off + attrLen > len) break;

        if (attrType == STUN_ATTR_XOR_MAPPED_ADDRESS && attrLen >= 8) {
            // data[off] = reserved, data[off+1] = family
            uint16_t xorPort = (data[off+2] << 8) | data[off+3];
            uint32_t xorIp = (data[off+4] << 24) | (data[off+5] << 16) |
                             (data[off+6] << 8) | data[off+7];

            addr.port = xorPort ^ (STUN_MAGIC_COOKIE >> 16);
            uint32_t ip = xorIp ^ htonl(STUN_MAGIC_COOKIE);

            struct in_addr in;
            in.s_addr = ip;
            addr.ip = inet_ntoa(in);
            addr.valid = true;
            return addr;
        }

        // Attributes are padded to 4-byte boundary
        off += (attrLen + 3) & ~3;
    }
    return addr;
}

} // namespace sfu

#endif // SFU_P2P_PROTOCOL_H
