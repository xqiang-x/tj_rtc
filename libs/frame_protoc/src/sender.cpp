#include "sender.h"
#include "protocol.h"
#include "cm256.h"
#include <map>
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace fec_protocol {

struct CachedBlock {
    std::vector<uint8_t> packet;   // Full serialized packet
    uint64_t send_time_us;
    uint64_t last_retransmit_us;   // Last retransmit time (0 = never retransmitted)
    uint8_t retry_count;
};

struct FrameSender::Impl {
    SenderConfig config;
    SendPacketCallback send_cb;
    FecRatioAdjustCallback fec_adjust_cb;
    StatsCallback stats_cb;
    KeyFrameRequestCallback key_frame_request_cb;
    ProtocolStats stats;
    CM256 cm256;

    SeqNum data_seq = 0;    // Current data sequence number (last used)
    SeqNum cmd_seq = 0;     // Current command sequence number (last used)
    uint16_t frame_id = 0;  // Current frame ID (wraps at 65535)
    float current_fec_ratio;

    // 自适应 FEC 仅在收到过接收端 STATS（实测丢包率）后启用，
    // 避免建立期（loss=0 的假象）把冗余瞬间清零
    bool fec_adaptive_active_ = false;
    uint64_t last_fec_adjust_us = 0;  // 上次自适应调整时刻（时间门控）

    // Retransmission cache: data_seq -> cached block (ordered by seq)
    std::map<SeqNum, CachedBlock> cache;

    // PING state
    uint64_t last_ping_time_us = 0;
    Timestamp last_ping_timestamp = 0;

    // 最后收到 PONG 的时刻（0 = 从未收到），用于对端存活判定
    uint64_t last_pong_time_us = 0;

    // Peer-reported completed seq for cache cleanup
    SeqNum peer_completed_seq = 0;

    Impl(const SenderConfig& cfg)
        : config(cfg), current_fec_ratio(cfg.fec_ratio) {}

    SeqNum nextDataSeq() {
        data_seq++;
        return data_seq;
    }

    SeqNum nextCmdSeq() {
        cmd_seq++;
        return cmd_seq;
    }

    // Compute header size for a given seq_len
    static size_t commonHeaderSize(uint8_t seq_len) {
        return 2 + seq_len;
    }

    static size_t dataPacketHeaderSize(uint8_t seq_len) {
        return commonHeaderSize(seq_len) + DataHeader::serializedSize();
    }

    // Compute max payload per block for given seq_len
    size_t maxPayloadPerBlock(uint8_t seq_len) const {
        size_t hdr = dataPacketHeaderSize(seq_len);
        if (config.mtu <= hdr) return 0;
        size_t max_pl = config.mtu - hdr;
        return std::min(max_pl, static_cast<size_t>(kMaxPayloadLength));
    }

    void sendPacket(const uint8_t* data, size_t len) {
        if (send_cb) send_cb(data, len);
    }

    void sendPing(uint64_t now_us) {
        SeqNum seq = nextCmdSeq();
        uint8_t seq_len = computeSeqLen(seq);

        // Build PING packet: CommonHeader + PayloadLength(2) + TLV(TIMESTAMP 10B [ + RTT 4B])
        // 携带 RTT（上次 PONG 测得）：接收端据此按 1×RTT 安排 NACK 重试间隔
        bool has_rtt = stats.rtt_ms > 0;
        size_t tlv_size = has_rtt ? (10 + 4) : 10;
        size_t buf_size = commonHeaderSize(seq_len) + 2 + tlv_size;
        std::vector<uint8_t> buf(buf_size);

        CommonHeader hdr;
        hdr.version = kProtocolVersion;
        hdr.seq_len = seq_len;
        hdr.msg_type = MsgType::PING;
        hdr.seq_num = seq;

        size_t offset = hdr.serialize(buf.data(), buf.size());

        // PayloadLength
        writeBE16(buf.data() + offset, static_cast<uint16_t>(tlv_size));
        offset += 2;

        // TLV: Timestamp
        buf[offset++] = static_cast<uint8_t>(TlvType::TIMESTAMP);
        buf[offset++] = 8;
        writeBE64(buf.data() + offset, now_us);
        offset += 8;

        // TLV: RTT（发送端实测 RTT，BE16 ms）
        if (has_rtt) {
            buf[offset++] = static_cast<uint8_t>(TlvType::RTT);
            buf[offset++] = 2;
            writeBE16(buf.data() + offset, static_cast<uint16_t>(stats.rtt_ms));
            offset += 2;
        }

        last_ping_time_us = now_us;
        last_ping_timestamp = now_us;

        sendPacket(buf.data(), offset);
    }

    void handleNack(const uint8_t* payload, size_t payload_len, uint64_t now_us) {
        std::vector<NackEntry> entries;
        if (!deserializeNackPayload(payload, payload_len, entries)) return;

        int32_t retransmit_budget = config.max_nack_retransmit; // -1 = unlimited

        for (const auto& entry : entries) {
            // Retransmit the start seq itself
            if (retransmit_budget == 0) break;
            if (retransmitBlock(entry.lost_start_seq, now_us) && retransmit_budget > 0) {
                retransmit_budget--;
            }

            // Retransmit blocks indicated by bitmask
            for (int bit = 0; bit < 32; ++bit) {
                if (retransmit_budget == 0) break;
                if (entry.bitmask & (1u << bit)) {
                    SeqNum lost_seq = entry.lost_start_seq + 1 + bit;
                    if (retransmitBlock(lost_seq, now_us) && retransmit_budget > 0) {
                        retransmit_budget--;
                    }
                }
            }
        }
    }

    // Returns true if a packet was actually retransmitted
    bool retransmitBlock(SeqNum seq, uint64_t now_us) {
        auto it = cache.find(seq);
        if (it == cache.end()) return false;

        CachedBlock& block = it->second;

        // Check per-block retransmit limit
        if (config.max_block_retransmit >= 0 &&
            block.retry_count >= static_cast<uint8_t>(config.max_block_retransmit)) {
            return false;
        }

        // Throttle: skip if last retransmit was less than RTT * factor ago
        uint64_t min_interval_us = static_cast<uint64_t>(stats.rtt_ms) * 1000;
        min_interval_us = static_cast<uint64_t>(min_interval_us * config.nack_rtt_factor);
        uint64_t last_send = (block.last_retransmit_us > 0) ? block.last_retransmit_us : block.send_time_us;
        if (now_us - last_send < min_interval_us) {
            return false; // Too soon, skip this retransmit
        }

        block.retry_count++;
        block.last_retransmit_us = now_us;

        // Update retry count in the serialized packet.
        // retry_count 位于 DataHeader 内偏移 12（frame_id 2B + 计数/类型 6B + frame_size 4B 之后）；
        // 偏移写成 10 会破坏 frame_size 第 3 字节，导致重传帧被接收端按错误大小重组
        uint8_t cached_seq_len = block.packet[0] & 0x0F;
        size_t retry_offset = commonHeaderSize(cached_seq_len) + 12;
        if (block.packet.size() > retry_offset) {
            uint8_t& byte = block.packet[retry_offset];
            byte = (byte & 0x0F) | ((block.retry_count & 0x0F) << 4);
        }

        sendPacket(block.packet.data(), block.packet.size());
        stats.blocks_sent++;
        return true;
    }

    void handlePong(const uint8_t* payload, size_t payload_len, uint64_t now_us) {
        // 收到 PONG 本身即证明对端存活，先记录时间再解析 RTT
        last_pong_time_us = now_us;

        std::vector<TlvItem> items;
        if (!deserializeTlvPayload(payload, payload_len, items)) return;

        for (const auto& item : items) {
            if (item.type == TlvType::ECHO_TIMESTAMP && item.value.size() == 8) {
                Timestamp echo_ts = readBE64(item.value.data());
                if (now_us > echo_ts) {
                    uint64_t rtt_us = now_us - echo_ts;
                    stats.rtt_ms = static_cast<uint32_t>(rtt_us / 1000);
                }
            }
        }
    }

    void handleStats(const uint8_t* payload, size_t payload_len) {
        std::vector<TlvItem> items;
        if (!deserializeTlvPayload(payload, payload_len, items)) return;

        for (const auto& item : items) {
            switch (item.type) {
                case TlvType::LOSS_RATE:
                    if (item.value.size() == 1)
                        stats.loss_rate = item.value[0] / 255.0f;
                    fec_adaptive_active_ = true;  // 收到实测丢包率 → 启用自适应冗余
                    break;
                case TlvType::FEC_RECOVERY_RATE:
                    if (item.value.size() == 1)
                        stats.fec_recovery_rate = item.value[0] / 255.0f;
                    break;
                case TlvType::NACK_RECOVERY_RATE:
                    if (item.value.size() == 1)
                        stats.nack_recovery_rate = item.value[0] / 255.0f;
                    break;
                case TlvType::BANDWIDTH_ESTIMATE:
                    if (item.value.size() == 4)
                        stats.bandwidth_bps = readBE32(item.value.data());
                    break;
                case TlvType::GOODPUT:
                    if (item.value.size() == 4)
                        stats.goodput_bps = readBE32(item.value.data());
                    break;
                case TlvType::COMPLETED_SEQ:
                    if (item.value.size() == 8) {
                        SeqNum completed = readBE64(item.value.data());
                        if (completed > peer_completed_seq) {
                            peer_completed_seq = completed;
                            cleanupCache(completed);
                        }
                    }
                    break;
                default:
                    break;
            }
        }

        if (stats_cb) {
            stats_cb(stats);
        }
    }

    void cleanupCache(SeqNum up_to_seq) {
        cache.erase(cache.begin(), cache.upper_bound(up_to_seq));
    }

    void cleanupExpiredCache(uint64_t now_us) {
        uint64_t timeout_us = static_cast<uint64_t>(config.cache_timeout_ms) * 1000;
        // Cache is ordered by seq (= send order), so expiry starts at the front
        while (!cache.empty()) {
            auto it = cache.begin();
            if (now_us - it->second.send_time_us <= timeout_us) break;
            cache.erase(it);
        }
    }
};

FrameSender::FrameSender(const SenderConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

FrameSender::~FrameSender() = default;

void FrameSender::setSendCallback(SendPacketCallback cb) {
    impl_->send_cb = std::move(cb);
}

void FrameSender::setFecRatioAdjustCallback(FecRatioAdjustCallback cb) {
    impl_->fec_adjust_cb = std::move(cb);
}

void FrameSender::setStatsCallback(StatsCallback cb) {
    impl_->stats_cb = std::move(cb);
}

void FrameSender::setKeyFrameRequestCallback(KeyFrameRequestCallback cb) {
    impl_->key_frame_request_cb = std::move(cb);
}

bool FrameSender::sendFrame(const uint8_t* data, size_t len, FrameType type, uint64_t now_us) {
    if (!data || len == 0) return false;
    if (!impl_->cm256.isInitialized()) return false;
    if (now_us == 0) now_us = impl_->last_ping_time_us;

    // Assign frame ID for this frame
    impl_->frame_id++;

    // Pre-compute: estimate how many total blocks we'll send to determine the
    // max seq_len needed. Use a conservative estimate with the smallest possible
    // seq_len first, then iterate if needed.
    // Start with current seq to estimate the seq_len for this frame's blocks.
    // We need to know block count, but block count depends on seq_len (header size).
    // Use iterative approach: assume seq_len based on final seq after all blocks.

    // First pass: estimate with current seq_len
    uint8_t est_seq_len = computeSeqLen(impl_->data_seq + 1);
    if (est_seq_len == 0) est_seq_len = 1;

    size_t max_payload = impl_->maxPayloadPerBlock(est_seq_len);
    if (max_payload == 0) return false;

    // Compute number of data blocks and optimal block_bytes
    // If frame fits in one block, block_bytes = len (no waste)
    // Otherwise, split evenly: block_bytes = ceil(len / num_blocks)
    size_t total_data_blocks;
    size_t block_bytes;
    if (len <= max_payload) {
        total_data_blocks = 1;
        block_bytes = len;
    } else {
        total_data_blocks = (len + max_payload - 1) / max_payload;
        block_bytes = (len + total_data_blocks - 1) / total_data_blocks;
    }

    // Calculate total blocks including FEC
    // max_group_size is computed so that data + fec blocks per group <= 255
    // data_blocks_per_group <= 255 * (1 - fec_ratio)
    uint8_t max_group_size = static_cast<uint8_t>(255.0f * (1.0f - impl_->current_fec_ratio));
    if (max_group_size == 0) max_group_size = 1;
    uint8_t fec_group_count = static_cast<uint8_t>(
        (total_data_blocks + max_group_size - 1) / max_group_size);
    if (fec_group_count == 0) fec_group_count = 1;

    size_t est_total_blocks = 0;
    for (uint8_t g = 0; g < fec_group_count; ++g) {
        size_t remaining = total_data_blocks - (g * max_group_size);
        uint8_t dbc = static_cast<uint8_t>(std::min(static_cast<size_t>(max_group_size), remaining));
        uint8_t fbc = static_cast<uint8_t>(std::ceil(dbc * impl_->current_fec_ratio));
        if (fbc == 0 && impl_->current_fec_ratio > 0) fbc = 1;
        if (static_cast<uint16_t>(dbc) + fbc > 255) fbc = 255 - dbc;
        est_total_blocks += dbc + fbc;
    }

    // Determine the max seq that will be used
    SeqNum max_seq = impl_->data_seq + est_total_blocks;
    uint8_t frame_seq_len = computeSeqLen(max_seq);
    if (frame_seq_len == 0) frame_seq_len = 1;

    // If seq_len changed, recalculate block_bytes
    if (frame_seq_len != est_seq_len) {
        max_payload = impl_->maxPayloadPerBlock(frame_seq_len);
        if (max_payload == 0) return false;

        if (len <= max_payload) {
            total_data_blocks = 1;
            block_bytes = len;
        } else {
            total_data_blocks = (len + max_payload - 1) / max_payload;
            block_bytes = (len + total_data_blocks - 1) / total_data_blocks;
        }

        fec_group_count = static_cast<uint8_t>(
            (total_data_blocks + max_group_size - 1) / max_group_size);
        if (fec_group_count == 0) fec_group_count = 1;
    }

    size_t data_offset = 0;

    for (uint8_t group_idx = 0; group_idx < fec_group_count; ++group_idx) {
        size_t remaining_blocks = total_data_blocks - (group_idx * max_group_size);
        uint8_t data_block_count = static_cast<uint8_t>(
            std::min(static_cast<size_t>(max_group_size), remaining_blocks));

        uint8_t fec_block_count = static_cast<uint8_t>(
            std::ceil(data_block_count * impl_->current_fec_ratio));
        if (fec_block_count == 0 && impl_->current_fec_ratio > 0) {
            fec_block_count = 1;
        }
        if (static_cast<uint16_t>(data_block_count) + fec_block_count > 255) {
            fec_block_count = 255 - data_block_count;
        }

        // Prepare data blocks (padded to block_bytes)
        std::vector<std::vector<uint8_t>> block_data(data_block_count);
        std::vector<CM256::cm256_block> cm_blocks(data_block_count);

        for (uint8_t i = 0; i < data_block_count; ++i) {
            block_data[i].resize(block_bytes, 0);
            size_t copy_len = std::min(block_bytes, len - data_offset);
            if (data_offset < len) {
                std::memcpy(block_data[i].data(), data + data_offset, copy_len);
                data_offset += copy_len;
            }
            cm_blocks[i].Block = block_data[i].data();
            cm_blocks[i].Index = CM256::cm256_get_original_block_index(
                {data_block_count, fec_block_count, static_cast<int>(block_bytes)}, i);
        }

        // FEC encode
        std::vector<uint8_t> recovery_buf(fec_block_count * block_bytes, 0);
        if (fec_block_count > 0) {
            CM256::cm256_encoder_params params;
            params.OriginalCount = data_block_count;
            params.RecoveryCount = fec_block_count;
            params.BlockBytes = static_cast<int>(block_bytes);

            int ret = impl_->cm256.cm256_encode(params, cm_blocks.data(), recovery_buf.data());
            if (ret != 0) return false;
        }

        // Send all blocks in this group
        uint8_t total_blocks = data_block_count + fec_block_count;
        for (uint8_t blk_idx = 0; blk_idx < total_blocks; ++blk_idx) {
            SeqNum seq = impl_->nextDataSeq();
            uint8_t seq_len = computeSeqLen(seq);
            if (seq_len == 0) seq_len = 1;

            const uint8_t* payload_ptr;
            uint16_t payload_len = static_cast<uint16_t>(block_bytes);

            if (blk_idx < data_block_count) {
                payload_ptr = block_data[blk_idx].data();
            } else {
                size_t fec_idx = blk_idx - data_block_count;
                payload_ptr = recovery_buf.data() + fec_idx * block_bytes;
            }

            // Build the packet directly inside the retransmission cache entry
            size_t packet_size = Impl::dataPacketHeaderSize(seq_len) + payload_len;
            CachedBlock& cached = impl_->cache[seq];
            cached.packet.resize(packet_size);
            cached.send_time_us = now_us;
            cached.last_retransmit_us = 0;
            cached.retry_count = 0;
            uint8_t* packet = cached.packet.data();

            // Common header
            CommonHeader common_hdr;
            common_hdr.version = kProtocolVersion;
            common_hdr.seq_len = seq_len;
            common_hdr.msg_type = MsgType::DATA;
            common_hdr.seq_num = seq;
            size_t off = common_hdr.serialize(packet, packet_size);

            // Data header
            DataHeader data_hdr;
            data_hdr.frame_id = impl_->frame_id;
            data_hdr.data_block_count = data_block_count;
            data_hdr.fec_block_count = fec_block_count;
            data_hdr.block_index = blk_idx;
            data_hdr.fec_group_index = group_idx;
            data_hdr.fec_group_count = fec_group_count;
            data_hdr.frame_type = type;
            data_hdr.frame_size = static_cast<uint32_t>(len);
            data_hdr.retry_count = 0;
            data_hdr.payload_length = payload_len;
            off += data_hdr.serialize(packet + off, packet_size - off);

            // Payload
            std::memcpy(packet + off, payload_ptr, payload_len);

            // Send from the cached buffer
            impl_->sendPacket(packet, packet_size);
            impl_->stats.blocks_sent++;
        }
    }

    impl_->stats.frames_sent++;
    return true;
}

void FrameSender::onPacketReceived(const uint8_t* data, size_t len, uint64_t now_us) {
    CommonHeader hdr;
    size_t consumed = 0;
    if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;

    const uint8_t* payload = data + consumed;
    size_t payload_len = len - consumed;

    if (now_us == 0) now_us = impl_->last_ping_time_us;

    switch (hdr.msg_type) {
        case MsgType::NACK:
            impl_->handleNack(payload, payload_len, now_us);
            break;
        case MsgType::PONG:
            impl_->handlePong(payload, payload_len, now_us);
            break;
        case MsgType::STATS:
            impl_->handleStats(payload, payload_len);
            break;
        case MsgType::PLI:
            if (impl_->key_frame_request_cb) {
                impl_->key_frame_request_cb();
            }
            break;
        default:
            break;
    }
}

void FrameSender::tick(uint64_t now_us) {
    // Send PING periodically
    // 注意：last_ping_time_us 只在 sendPing() 内部刷新。若在每次 tick 都刷新，
    // 则高频率 tick（每帧一次）会不断重置计时基准，间隔条件永远不满足，
    // PING 只在首次 tick（last_ping_timestamp==0）发送一次 —— 心跳就此失效
    uint64_t ping_interval_us = static_cast<uint64_t>(impl_->config.ping_interval_ms) * 1000;
    if (now_us - impl_->last_ping_time_us >= ping_interval_us ||
        impl_->last_ping_timestamp == 0) {
        impl_->sendPing(now_us);
    }

    // Cleanup expired cache
    impl_->cleanupExpiredCache(now_us);

    // FEC ratio adjustment：优先用外部回调，否则自动按丢包率计算（1.5 倍丢包率），
    // 最终一律 clamp 到 [fec_ratio_min, fec_ratio_max]；自动模式仅在收到过
    // 接收端丢包率反馈后启用，且按时间节奏渐变（每 250ms 至多一步 ±0.02），
    // 收敛平滑：不会因 tick 频率不同导致冗余骤变、组数抖动
    float new_ratio = -1.0f;
    if (impl_->fec_adjust_cb) {
        float cb_ratio = impl_->fec_adjust_cb(impl_->stats);
        if (cb_ratio >= 0.0f && cb_ratio <= 1.0f) new_ratio = cb_ratio;
    } else if (impl_->fec_adaptive_active_) {
        const uint64_t kFecAdjIntervalUs = 250'000;  // 250ms
        if (now_us - impl_->last_fec_adjust_us >= kFecAdjIntervalUs) {
            new_ratio = impl_->stats.loss_rate * impl_->config.fec_loss_factor;
            impl_->last_fec_adjust_us = now_us;
        }
    }
    if (new_ratio >= 0.0f) {
        float clamped = new_ratio;
        if (clamped < impl_->config.fec_ratio_min) clamped = impl_->config.fec_ratio_min;
        if (clamped > impl_->config.fec_ratio_max) clamped = impl_->config.fec_ratio_max;
        float cur = impl_->current_fec_ratio;
        const float kMaxStep = 0.02f;  // 每步最大移动量（一个死区量级）
        if (clamped > cur + kMaxStep) clamped = cur + kMaxStep;
        if (clamped < cur - kMaxStep) clamped = cur - kMaxStep;
        impl_->current_fec_ratio = clamped;
    }
}

const ProtocolStats& FrameSender::getStats() const {
    return impl_->stats;
}

SeqNum FrameSender::currentDataSeqNum() const {
    return impl_->data_seq;
}

void FrameSender::setFecRatio(float ratio) {
    if (ratio >= 0.0f && ratio <= 1.0f) {
        impl_->current_fec_ratio = ratio;
    }
}

bool FrameSender::hasReceivedPong() const {
    return impl_ && impl_->last_pong_time_us != 0;
}

uint64_t FrameSender::lastPongTimeUs() const {
    return impl_ ? impl_->last_pong_time_us : 0;
}

} // namespace fec_protocol

