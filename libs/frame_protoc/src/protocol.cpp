#include "protocol.h"
#include <cstring>
#include <algorithm>

namespace fec_protocol {

// --- Big-endian helpers ---

void writeBE16(uint8_t* buf, uint16_t val) {
    buf[0] = static_cast<uint8_t>(val >> 8);
    buf[1] = static_cast<uint8_t>(val);
}

void writeBE32(uint8_t* buf, uint32_t val) {
    buf[0] = static_cast<uint8_t>(val >> 24);
    buf[1] = static_cast<uint8_t>(val >> 16);
    buf[2] = static_cast<uint8_t>(val >> 8);
    buf[3] = static_cast<uint8_t>(val);
}

void writeBE64(uint8_t* buf, uint64_t val) {
    buf[0] = static_cast<uint8_t>(val >> 56);
    buf[1] = static_cast<uint8_t>(val >> 48);
    buf[2] = static_cast<uint8_t>(val >> 40);
    buf[3] = static_cast<uint8_t>(val >> 32);
    buf[4] = static_cast<uint8_t>(val >> 24);
    buf[5] = static_cast<uint8_t>(val >> 16);
    buf[6] = static_cast<uint8_t>(val >> 8);
    buf[7] = static_cast<uint8_t>(val);
}

uint16_t readBE16(const uint8_t* buf) {
    return (static_cast<uint16_t>(buf[0]) << 8) |
           static_cast<uint16_t>(buf[1]);
}

uint32_t readBE32(const uint8_t* buf) {
    return (static_cast<uint32_t>(buf[0]) << 24) |
           (static_cast<uint32_t>(buf[1]) << 16) |
           (static_cast<uint32_t>(buf[2]) << 8) |
           static_cast<uint32_t>(buf[3]);
}

uint64_t readBE64(const uint8_t* buf) {
    return (static_cast<uint64_t>(buf[0]) << 56) |
           (static_cast<uint64_t>(buf[1]) << 48) |
           (static_cast<uint64_t>(buf[2]) << 40) |
           (static_cast<uint64_t>(buf[3]) << 32) |
           (static_cast<uint64_t>(buf[4]) << 24) |
           (static_cast<uint64_t>(buf[5]) << 16) |
           (static_cast<uint64_t>(buf[6]) << 8) |
           static_cast<uint64_t>(buf[7]);
}

void writeSeqNum(uint8_t* buf, SeqNum val, uint8_t seq_len) {
    for (int i = seq_len - 1; i >= 0; --i) {
        buf[i] = static_cast<uint8_t>(val & 0xFF);
        val >>= 8;
    }
}

SeqNum readSeqNum(const uint8_t* buf, uint8_t seq_len) {
    SeqNum val = 0;
    for (uint8_t i = 0; i < seq_len; ++i) {
        val = (val << 8) | buf[i];
    }
    return val;
}

// --- Sequence number helpers ---

uint8_t computeSeqLen(SeqNum seq) {
    if (seq == 0) return 0;
    uint8_t len = 0;
    SeqNum tmp = seq;
    while (tmp > 0) {
        len++;
        tmp >>= 8;
    }
    return len;
}

SeqNum seqMask(uint8_t seq_len) {
    if (seq_len >= 8) return UINT64_MAX;
    return (static_cast<uint64_t>(1) << (seq_len * 8)) - 1;
}

int seqCompare(SeqNum a, SeqNum b, uint8_t seq_len) {
    if (a == b) return 0;
    SeqNum mask = seqMask(seq_len);
    SeqNum half = (mask >> 1) + 1;
    SeqNum diff = (a - b) & mask;
    if (diff == 0) return 0;
    return (diff < half) ? 1 : -1;
}

SeqNum seqAdd(SeqNum a, uint64_t offset, uint8_t seq_len) {
    return (a + offset) & seqMask(seq_len);
}

// --- CommonHeader ---

size_t CommonHeader::serialize(uint8_t* buf, size_t buf_len) const {
    size_t needed = serializedSize();
    if (buf_len < needed) return 0;

    // Byte 0: version(4bit) | seq_len(4bit)
    buf[0] = ((version & 0x0F) << 4) | (seq_len & 0x0F);
    // Byte 1: msg_type
    buf[1] = static_cast<uint8_t>(msg_type);
    // Bytes 2..2+seq_len-1: sequence number
    writeSeqNum(buf + 2, seq_num, seq_len);

    return needed;
}

bool CommonHeader::deserialize(const uint8_t* buf, size_t buf_len,
                               CommonHeader& out, size_t& bytes_consumed) {
    if (buf_len < 2) return false;

    out.version = (buf[0] >> 4) & 0x0F;
    out.seq_len = buf[0] & 0x0F;
    out.msg_type = static_cast<MsgType>(buf[1]);

    if (out.seq_len < 1 || out.seq_len > kMaxSeqLen) return false;

    size_t needed = 2 + out.seq_len;
    if (buf_len < needed) return false;

    out.seq_num = readSeqNum(buf + 2, out.seq_len);
    bytes_consumed = needed;
    return true;
}

// --- DataHeader ---

size_t DataHeader::serialize(uint8_t* buf, size_t buf_len) const {
    if (buf_len < serializedSize()) return 0;

    writeBE16(buf, frame_id);
    buf[2] = data_block_count;
    buf[3] = fec_block_count;
    buf[4] = block_index;
    buf[5] = fec_group_index;
    buf[6] = fec_group_count;
    buf[7] = frame_type;
    writeBE32(buf + 8, frame_size);
    // Byte 12: retry_count(4bit) | payload_length high 4 bits
    // Byte 13: payload_length low 8 bits
    buf[12] = ((retry_count & 0x0F) << 4) |
              ((payload_length >> 8) & 0x0F);
    buf[13] = static_cast<uint8_t>(payload_length & 0xFF);

    return serializedSize();
}

bool DataHeader::deserialize(const uint8_t* buf, size_t buf_len, DataHeader& out) {
    if (buf_len < serializedSize()) return false;

    out.frame_id = readBE16(buf);
    out.data_block_count = buf[2];
    out.fec_block_count = buf[3];
    out.block_index = buf[4];
    out.fec_group_index = buf[5];
    out.fec_group_count = buf[6];
    out.frame_type = buf[7];
    out.frame_size = readBE32(buf + 8);
    out.retry_count = (buf[12] >> 4) & 0x0F;
    out.payload_length = (static_cast<uint16_t>(buf[12] & 0x0F) << 8) | buf[13];

    return true;
}

// --- NACK payload ---

size_t serializeNackPayload(const std::vector<NackEntry>& entries,
                            uint8_t* buf, size_t buf_len, uint8_t seq_len) {
    (void)seq_len;
    // Payload format: 2 bytes payload_length + N * (8 bytes seq + 4 bytes bitmask)
    size_t payload_data_len = entries.size() * kNackEntrySize;
    size_t total = 2 + payload_data_len;
    if (buf_len < total) return 0;

    writeBE16(buf, static_cast<uint16_t>(payload_data_len));

    size_t offset = 2;
    for (const auto& entry : entries) {
        writeBE64(buf + offset, entry.lost_start_seq);
        offset += 8;
        writeBE32(buf + offset, entry.bitmask);
        offset += 4;
    }
    return total;
}

bool deserializeNackPayload(const uint8_t* buf, size_t buf_len,
                            std::vector<NackEntry>& out) {
    if (buf_len < 2) return false;

    uint16_t payload_len = readBE16(buf);
    if (buf_len < 2u + payload_len) return false;
    if (payload_len % kNackEntrySize != 0) return false;

    size_t count = payload_len / kNackEntrySize;
    out.clear();
    out.reserve(count);

    size_t offset = 2;
    for (size_t i = 0; i < count; ++i) {
        NackEntry entry;
        entry.lost_start_seq = readBE64(buf + offset);
        offset += 8;
        entry.bitmask = readBE32(buf + offset);
        offset += 4;
        out.push_back(entry);
    }
    return true;
}

// --- TLV payload ---

size_t serializeTlvPayload(const std::vector<TlvItem>& items,
                           uint8_t* buf, size_t buf_len) {
    // Calculate total TLV data size
    size_t tlv_data_len = 0;
    for (const auto& item : items) {
        tlv_data_len += 2 + item.value.size(); // type(1) + len(1) + value(N)
    }

    size_t total = 2 + tlv_data_len; // payload_length(2) + TLV data
    if (buf_len < total) return 0;

    writeBE16(buf, static_cast<uint16_t>(tlv_data_len));

    size_t offset = 2;
    for (const auto& item : items) {
        buf[offset++] = static_cast<uint8_t>(item.type);
        buf[offset++] = static_cast<uint8_t>(item.value.size());
        if (!item.value.empty()) {
            std::memcpy(buf + offset, item.value.data(), item.value.size());
            offset += item.value.size();
        }
    }
    return total;
}

bool deserializeTlvPayload(const uint8_t* buf, size_t buf_len,
                           std::vector<TlvItem>& out) {
    if (buf_len < 2) return false;

    uint16_t payload_len = readBE16(buf);
    if (buf_len < 2u + payload_len) return false;

    out.clear();
    size_t offset = 2;
    size_t end = 2 + payload_len;

    while (offset < end) {
        if (offset + 2 > end) return false;
        TlvItem item;
        item.type = static_cast<TlvType>(buf[offset++]);
        uint8_t len = buf[offset++];
        if (offset + len > end) return false;
        item.value.assign(buf + offset, buf + offset + len);
        offset += len;
        out.push_back(std::move(item));
    }
    return true;
}

} // namespace fec_protocol

