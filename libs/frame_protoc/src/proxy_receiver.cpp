#include "proxy_receiver.h"
#include "protocol.h"
#include <map>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace fec_protocol {

struct CachedPacket {
    std::vector<uint8_t> packet;
    uint64_t cache_time_us;
};

struct FecGroupForwardState {
    uint8_t data_block_count;
    uint8_t max_fec_to_forward;
    uint8_t fec_forwarded_count;
    uint8_t data_forwarded_count;   // Track received data blocks forwarded
    uint64_t first_seen_us;
    SeqNum min_seq;                 // First seq seen for this group
    SeqNum max_seq;                 // Last seq seen for this group
};

// Active NACK state for a missing sequence
struct ProxyNackState {
    uint64_t first_detected_us = 0;
    uint64_t last_nack_sent_us = 0;
    uint32_t nack_count = 0;
};

struct ProxyFrameReceiver::Impl {
    ProxyConfig config;
    SendPacketCallback send_cb;
    UpstreamSendCallback upstream_send_cb;
    NackMissCallback nack_miss_cb;
    ProxyStats stats;

    // Packet cache: seq_num -> cached packet (ordered by seq = insertion order)
    std::map<SeqNum, CachedPacket> cache;

    // FEC group forward tracking: key = (frame_id << 8) | fec_group_index
    std::unordered_map<uint32_t, FecGroupForwardState> fec_groups;

    // Active NACK: gap detection and state (ordered by seq)
    SeqNum expected_data_seq = 0;
    bool expected_seq_initialized = false;
    std::map<SeqNum, ProxyNackState> proxy_nack_states;

    // Downstream completed_seq for cleanup
    SeqNum downstream_completed_seq = 0;

    // Command seq for upstream NACK packets
    SeqNum upstream_cmd_seq = 0;

    uint64_t last_tick_us = 0;

    Impl(const ProxyConfig& cfg) : config(cfg) {}

    void sendPacket(const uint8_t* data, size_t len) {
        if (send_cb) send_cb(data, len);
    }

    void sendUpstream(const uint8_t* data, size_t len) {
        if (upstream_send_cb) upstream_send_cb(data, len);
    }

    void cachePacket(SeqNum seq, const uint8_t* data, size_t len, uint64_t now_us) {
        CachedPacket pkt;
        pkt.packet.assign(data, data + len);
        pkt.cache_time_us = now_us;
        cache[seq] = std::move(pkt);
    }

    void detectUpstreamGaps(SeqNum received_seq, uint64_t now_us) {
        if (!expected_seq_initialized) {
            expected_data_seq = received_seq;
            expected_seq_initialized = true;
        }

        if (received_seq > expected_data_seq) {
            SeqNum gap = received_seq - expected_data_seq;
            if (gap > config.max_gap_window) {
                // 巨大序号跳变（坏包/伪造包/推流端序号重置）：直接重锚，
                // 不逐号创建 NACK 状态，防止单个包引发海量分配
                expected_data_seq = received_seq + 1;
            } else {
                // Gap detected: expected_data_seq .. received_seq-1 are missing
                SeqNum missing = expected_data_seq;
                while (missing < received_seq) {
                    if (proxy_nack_states.find(missing) == proxy_nack_states.end()) {
                        ProxyNackState state;
                        state.first_detected_us = now_us;
                        state.last_nack_sent_us = 0;
                        state.nack_count = 0;
                        proxy_nack_states[missing] = state;
                    }
                    missing++;
                }
                expected_data_seq = received_seq + 1;
            }
        } else if (received_seq == expected_data_seq) {
            expected_data_seq = received_seq + 1;
        }

        // If this seq was previously missing, it's now recovered
        auto nack_it = proxy_nack_states.find(received_seq);
        if (nack_it != proxy_nack_states.end()) {
            proxy_nack_states.erase(nack_it);
            stats.upstream_nack_recovered++;
        }
    }

    bool isGroupRecoverable(SeqNum missing_seq) {
        // Try to find which group this missing seq belongs to
        // Check all active FEC groups' seq ranges
        for (const auto& kv : fec_groups) {
            const FecGroupForwardState& group = kv.second;
            if (missing_seq >= group.min_seq && missing_seq <= group.max_seq) {
                // Found the group - check if downstream has enough blocks
                uint8_t total_forwarded = group.data_forwarded_count + group.fec_forwarded_count;
                return total_forwarded >= group.data_block_count;
            }
        }
        // Can't determine group - conservative: not recoverable, send NACK
        return false;
    }

    void generateUpstreamNacks(uint64_t now_us) {
        if (!upstream_send_cb) return;
        if (proxy_nack_states.empty()) return;

        uint64_t delay_us = static_cast<uint64_t>(config.nack.nack_delay_ms) * 1000;
        uint64_t interval_us = static_cast<uint64_t>(config.nack.nack_interval_ms) * 1000;
        uint64_t lifetime_us = static_cast<uint64_t>(config.nack.nack_lifetime_ms) * 1000;

        std::vector<NackEntry> entries;
        std::vector<SeqNum> to_erase;

        for (auto& kv : proxy_nack_states) {
            SeqNum seq = kv.first;
            ProxyNackState& state = kv.second;

            // 超过生命周期：下游帧已按 2s 超时丢弃，继续 NACK 无意义
            if (now_us - state.first_detected_us >= lifetime_us) {
                to_erase.push_back(seq);
                continue;
            }

            // FEC recoverability check - suppress if recoverable
            if (isGroupRecoverable(seq)) {
                to_erase.push_back(seq);
                stats.upstream_nack_suppressed++;
                continue;
            }

            // Exceeded max retries
            if (state.nack_count >= config.nack.max_nack_retries) {
                to_erase.push_back(seq);
                continue;
            }

            // Check timing
            bool should_send = false;
            if (state.last_nack_sent_us == 0) {
                // First NACK: wait for delay
                if (now_us - state.first_detected_us >= delay_us) {
                    should_send = true;
                }
            } else {
                // Subsequent: wait for interval
                if (now_us - state.last_nack_sent_us >= interval_us) {
                    should_send = true;
                }
            }

            if (should_send) {
                NackEntry entry;
                entry.lost_start_seq = seq;
                entry.bitmask = 0;
                entries.push_back(entry);

                state.last_nack_sent_us = now_us;
                state.nack_count++;
            }
        }

        // Erase suppressed/expired entries
        for (SeqNum seq : to_erase) {
            proxy_nack_states.erase(seq);
        }

        // Send batched NACK upstream
        if (!entries.empty()) {
            sendUpstreamNackPacket(entries);
            stats.upstream_nacks_sent += entries.size();
        }
    }

    void sendUpstreamNackPacket(const std::vector<NackEntry>& entries) {
        // 分片发送：积压时条目过多，单个超大 UDP 数据报会 EMSGSIZE 失败
        constexpr size_t kMaxEntriesPerPacket = 100;
        for (size_t begin = 0; begin < entries.size(); begin += kMaxEntriesPerPacket) {
            size_t end = std::min(begin + kMaxEntriesPerPacket, entries.size());
            size_t count = end - begin;

            upstream_cmd_seq++;
            SeqNum seq = upstream_cmd_seq;
            uint8_t seq_len = computeSeqLen(seq);
            if (seq_len == 0) seq_len = 1;

            size_t hdr_size = 2 + seq_len;
            size_t payload_size = 2 + count * kNackEntrySize;
            std::vector<uint8_t> buf(hdr_size + payload_size);

            CommonHeader hdr;
            hdr.version = kProtocolVersion;
            hdr.seq_len = seq_len;
            hdr.msg_type = MsgType::NACK;
            hdr.seq_num = seq;

            size_t offset = hdr.serialize(buf.data(), buf.size());

            std::vector<NackEntry> chunk(entries.begin() + begin, entries.begin() + end);
            size_t written = serializeNackPayload(chunk, buf.data() + offset,
                                                  buf.size() - offset, seq_len);
            if (written > 0) {
                sendUpstream(buf.data(), offset + written);
            }
        }
    }

    void sendUpstreamPli() {
        upstream_cmd_seq++;
        SeqNum seq = upstream_cmd_seq;
        uint8_t seq_len = computeSeqLen(seq);
        if (seq_len == 0) seq_len = 1;

        // PLI: CommonHeader + 2-byte PayloadLength=0 (matches receiver.cpp sendPliPacket)
        std::vector<uint8_t> buf(2 + seq_len + 2);

        CommonHeader hdr;
        hdr.version = kProtocolVersion;
        hdr.seq_len = seq_len;
        hdr.msg_type = MsgType::PLI;
        hdr.seq_num = seq;

        size_t offset = hdr.serialize(buf.data(), buf.size());
        buf[offset] = 0;
        buf[offset + 1] = 0;
        sendUpstream(buf.data(), offset + 2);
    }

    // 心跳应答：把 PING 原样回给推流端会因中间节点不回 PONG 而让推流端
    // 永远收不到心跳回包（曾导致服务端踢会话后推流端盲发 3.5 小时）。
    // 本节点必须亲自构造 PONG（格式与 receiver.cpp handlePing 一致）。
    void handleUpstreamPing(const uint8_t* payload, size_t payload_len) {
        std::vector<TlvItem> items;
        Timestamp ping_ts = 0;
        if (deserializeTlvPayload(payload, payload_len, items)) {
            for (const auto& item : items) {
                if (item.type == TlvType::TIMESTAMP && item.value.size() == 8) {
                    ping_ts = readBE64(item.value.data());
                    break;
                }
            }
        }

        upstream_cmd_seq++;
        SeqNum pong_seq = upstream_cmd_seq;
        uint8_t pong_seq_len = computeSeqLen(pong_seq);
        if (pong_seq_len == 0) pong_seq_len = 1;

        // PONG: CommonHeader + PayloadLength(2) + TLV(ECHO_TIMESTAMP 10B)
        std::vector<uint8_t> buf(2 + pong_seq_len + 2 + 10);

        CommonHeader pong_hdr;
        pong_hdr.version = kProtocolVersion;
        pong_hdr.seq_len = pong_seq_len;
        pong_hdr.msg_type = MsgType::PONG;
        pong_hdr.seq_num = pong_seq;

        size_t offset = pong_hdr.serialize(buf.data(), buf.size());

        writeBE16(buf.data() + offset, 10);
        offset += 2;

        buf[offset++] = static_cast<uint8_t>(TlvType::ECHO_TIMESTAMP);
        buf[offset++] = 8;
        writeBE64(buf.data() + offset, ping_ts);
        offset += 8;

        sendUpstream(buf.data(), offset);
        stats.pongs_sent++;
    }

    void handleDownstreamStats(const uint8_t* data, size_t len) {
        CommonHeader hdr;
        size_t hdr_consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, hdr_consumed)) return;
        if (hdr.msg_type != MsgType::STATS) return;

        // Forward raw STATS upstream so the publisher sees loss/goodput feedback
        sendUpstream(data, len);
        stats.stats_forwarded_upstream++;

        const uint8_t* payload = data + hdr_consumed;
        size_t payload_len = len - hdr_consumed;

        std::vector<TlvItem> items;
        if (!deserializeTlvPayload(payload, payload_len, items)) return;

        for (const auto& item : items) {
            if (item.type == TlvType::COMPLETED_SEQ && item.value.size() == 8) {
                SeqNum completed = readBE64(item.value.data());
                if (completed > downstream_completed_seq) {
                    downstream_completed_seq = completed;
                    cleanupByCompletedSeq(completed);
                }
                break;
            }
        }
    }

    void cleanupByCompletedSeq(SeqNum completed_seq) {
        // Remove nack_states for seqs <= completed_seq (ordered: range erase)
        proxy_nack_states.erase(proxy_nack_states.begin(),
                                proxy_nack_states.upper_bound(completed_seq));
    }

    bool handleDataPacket(const CommonHeader& hdr, const uint8_t* data, size_t len,
                          const uint8_t* payload, size_t payload_len, uint64_t now_us) {
        DataHeader data_hdr;
        if (!DataHeader::deserialize(payload, payload_len, data_hdr)) {
            return false;
        }

        // Gap detection for active NACK
        detectUpstreamGaps(hdr.seq_num, now_us);

        // Compute FEC group key
        uint32_t group_key = (static_cast<uint32_t>(data_hdr.frame_id) << 8) |
                             static_cast<uint32_t>(data_hdr.fec_group_index);

        // Find or create FEC group forward state
        auto it = fec_groups.find(group_key);
        if (it == fec_groups.end()) {
            FecGroupForwardState state;
            state.data_block_count = data_hdr.data_block_count;
            float fec_count = std::ceil(
                static_cast<float>(data_hdr.data_block_count) * config.downstream_fec_ratio);
            state.max_fec_to_forward = static_cast<uint8_t>(fec_count);
            // Cap at actual fec_block_count from upstream
            if (state.max_fec_to_forward > data_hdr.fec_block_count) {
                state.max_fec_to_forward = data_hdr.fec_block_count;
            }
            state.fec_forwarded_count = 0;
            state.data_forwarded_count = 0;
            state.first_seen_us = now_us;
            state.min_seq = hdr.seq_num;
            state.max_seq = hdr.seq_num;
            auto result = fec_groups.emplace(group_key, state);
            it = result.first;
        }

        FecGroupForwardState& state = it->second;

        // Update seq range for this group
        if (hdr.seq_num < state.min_seq) state.min_seq = hdr.seq_num;
        if (hdr.seq_num > state.max_seq) state.max_seq = hdr.seq_num;

        // Decision: data block or FEC block?
        if (data_hdr.block_index < state.data_block_count) {
            // Data block: always forward
            sendPacket(data, len);
            cachePacket(hdr.seq_num, data, len, now_us);
            state.data_forwarded_count++;
            stats.packets_forwarded++;
            return true;
        } else {
            // FEC block: check if we should forward
            if (state.fec_forwarded_count < state.max_fec_to_forward) {
                state.fec_forwarded_count++;
                sendPacket(data, len);
                cachePacket(hdr.seq_num, data, len, now_us);
                stats.packets_forwarded++;
                return true;
            } else {
                // Drop this FEC block
                stats.fec_packets_dropped++;
                return false;
            }
        }
    }

    void handleNack(const uint8_t* data, size_t len) {
        // Parse common header to get past it
        CommonHeader hdr;
        size_t hdr_consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, hdr_consumed)) return;
        if (hdr.msg_type != MsgType::NACK) return;

        const uint8_t* nack_payload = data + hdr_consumed;
        size_t nack_payload_len = len - hdr_consumed;

        std::vector<NackEntry> entries;
        if (!deserializeNackPayload(nack_payload, nack_payload_len, entries)) return;

        stats.nack_requests_received++;

        std::vector<NackEntry> missed_entries;

        for (const auto& entry : entries) {
            // Check start seq
            bool start_hit = tryResendFromCache(entry.lost_start_seq);

            // Build missed entry for start seq if not in cache
            NackEntry missed;
            missed.lost_start_seq = entry.lost_start_seq;
            missed.bitmask = 0;
            bool has_any_miss = !start_hit;

            // Check bitmask entries
            for (uint8_t bit = 0; bit < 32; ++bit) {
                if (entry.bitmask & (1u << bit)) {
                    SeqNum seq = entry.lost_start_seq + 1 + bit;
                    bool hit = tryResendFromCache(seq);
                    if (!hit) {
                        missed.bitmask |= (1u << bit);
                        has_any_miss = true;
                    }
                }
            }

            if (has_any_miss) {
                if (start_hit) {
                    if (missed.bitmask != 0) {
                        missed_entries.push_back(missed);
                    }
                } else {
                    missed_entries.push_back(missed);
                }
            }
        }

        if (!missed_entries.empty() && nack_miss_cb) {
            stats.nack_cache_misses += missed_entries.size();
            nack_miss_cb(missed_entries);
        }
    }

    bool tryResendFromCache(SeqNum seq) {
        auto it = cache.find(seq);
        if (it != cache.end()) {
            sendPacket(it->second.packet.data(), it->second.packet.size());
            stats.nack_cache_hits++;
            return true;
        }
        return false;
    }

    void cleanupExpired(uint64_t now_us) {
        uint64_t timeout_us = static_cast<uint64_t>(config.cache_timeout_ms) * 1000;

        // Clean cache (ordered by seq: expired entries sit at the front)
        while (!cache.empty()) {
            auto it = cache.begin();
            if (now_us - it->second.cache_time_us <= timeout_us) break;
            cache.erase(it);
        }

        // Enforce max cache size (front = oldest seq)
        while (cache.size() > config.max_cache_entries) {
            cache.erase(cache.begin());
        }

        // Clean FEC group tracker (bounded by concurrently active groups)
        // 组状态只用于转发决策与可恢复性判断，超过 fec_group_timeout 即失效
        uint64_t group_timeout_us = static_cast<uint64_t>(config.fec_group_timeout_ms) * 1000;
        for (auto it = fec_groups.begin(); it != fec_groups.end(); ) {
            if (now_us - it->second.first_seen_us > group_timeout_us) {
                it = fec_groups.erase(it);
            } else {
                ++it;
            }
        }

        // Clean expired proxy_nack_states (ordered: expired entries sit at the front)
        // 缺包状态生命周期独立于转发缓存：默认 2s，与下游帧超时对齐，避免
        // 长缓存配置（如 300s 无损模式）下大量缺包状态积压导致 generateUpstreamNacks 全表扫描卡顿
        uint64_t nack_lifetime_us = static_cast<uint64_t>(config.nack.nack_lifetime_ms) * 1000;
        while (!proxy_nack_states.empty()) {
            auto it = proxy_nack_states.begin();
            if (now_us - it->second.first_detected_us <= nack_lifetime_us) break;
            proxy_nack_states.erase(it);
        }

        stats.cache_size = cache.size();
    }
};

// --- Public interface ---

ProxyFrameReceiver::ProxyFrameReceiver(const ProxyConfig& config)
    : impl_(new Impl(config)) {}

ProxyFrameReceiver::~ProxyFrameReceiver() = default;

void ProxyFrameReceiver::setSendCallback(SendPacketCallback cb) {
    impl_->send_cb = std::move(cb);
}

void ProxyFrameReceiver::setUpstreamSendCallback(UpstreamSendCallback cb) {
    impl_->upstream_send_cb = std::move(cb);
}

void ProxyFrameReceiver::setNackMissCallback(NackMissCallback cb) {
    impl_->nack_miss_cb = std::move(cb);
}

bool ProxyFrameReceiver::onUpstreamPacket(const uint8_t* data, size_t len, uint64_t now_us) {
    if (!data || len == 0) return false;

    impl_->stats.packets_received++;

    // Parse common header
    CommonHeader hdr;
    size_t hdr_consumed = 0;
    if (!CommonHeader::deserialize(data, len, hdr, hdr_consumed)) return false;

    if (hdr.msg_type == MsgType::DATA) {
        const uint8_t* payload = data + hdr_consumed;
        size_t payload_len = len - hdr_consumed;
        return impl_->handleDataPacket(hdr, data, len, payload, payload_len, now_us);
    }

    if (hdr.msg_type == MsgType::PING) {
        // 心跳由本节点应答（PONG 回上游），不再转发下游：
        // 下游 FrameReceiver 会误把 PING 当作对自身的心跳而回 PONG，
        // 产生无意义的 PONG 转发链；且上游的存活判定只能由本节点维持
        const uint8_t* payload = data + hdr_consumed;
        size_t payload_len = len - hdr_consumed;
        impl_->handleUpstreamPing(payload, payload_len);
        return true;
    }

    // Other non-DATA packets (STATS, etc.): forward as-is, no need to cache
    impl_->sendPacket(data, len);
    impl_->stats.packets_forwarded++;
    return true;
}

void ProxyFrameReceiver::onDownstreamNack(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    impl_->handleNack(data, len);
}

void ProxyFrameReceiver::onDownstreamPacket(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;

    // Parse header to determine type
    CommonHeader hdr;
    size_t hdr_consumed = 0;
    if (!CommonHeader::deserialize(data, len, hdr, hdr_consumed)) return;

    switch (hdr.msg_type) {
        case MsgType::NACK:
            impl_->handleNack(data, len);
            break;
        case MsgType::STATS:
            impl_->handleDownstreamStats(data, len);
            break;
        case MsgType::PLI:
            // 原样转发关键帧请求到推流端（服务器已做 500ms 节流）
            impl_->sendUpstream(data, len);
            impl_->stats.pli_forwarded_upstream++;
            break;
        default:
            break;
    }
}

void ProxyFrameReceiver::requestUpstreamNack(const std::vector<NackEntry>& entries) {
    if (entries.empty()) return;
    if (!impl_->upstream_send_cb) return;
    impl_->sendUpstreamNackPacket(entries);
    impl_->stats.upstream_nacks_sent += entries.size();
}

void ProxyFrameReceiver::requestKeyFrame() {
    if (!impl_->upstream_send_cb) return;
    impl_->sendUpstreamPli();
    impl_->stats.pli_forwarded_upstream++;
}

void ProxyFrameReceiver::tick(uint64_t now_us) {
    // 50ms 节流：tick 含全表扫描（cache/NACK state 清理 + FEC 可恢复性判断），
    // 若每收到一个上游包都触发，丢包积压时会拖垮事件循环
    uint64_t min_interval = static_cast<uint64_t>(impl_->config.tick_interval_ms) * 1000;
    if (now_us - impl_->last_tick_us < min_interval) {
        return;  // 未到间隔：不刷新基准，否则每包刷新导致清理/补缺永远不执行
    }
    impl_->last_tick_us = now_us;
    impl_->cleanupExpired(now_us);
    impl_->generateUpstreamNacks(now_us);
}

const ProxyStats& ProxyFrameReceiver::getStats() const {
    return impl_->stats;
}

uint32_t ProxyFrameReceiver::maxCacheEntries() const {
    return impl_->config.max_cache_entries;
}

void ProxyFrameReceiver::setDownstreamFecRatio(float ratio) {
    impl_->config.downstream_fec_ratio = ratio;
}

} // namespace fec_protocol

