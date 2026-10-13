#include "frame_assembler.h"
#include "protocol.h"
#include "cm256.h"
#include <unordered_map>
#include <vector>
#include <cstring>
#include <algorithm>
#include <array>
#include <bitset>

namespace fec_protocol {

// 内部：pending 帧内一个 FEC 组的状态（与 receiver.cpp 同构）
struct FecGroupState {
    uint8_t data_block_count = 0;
    uint8_t fec_block_count = 0;
    bool params_known = false;
    bool decoded = false;

    FecGroupState() { slot_by_index.fill(-1); }

    std::vector<std::vector<uint8_t>> block_storage;
    std::vector<uint8_t> block_indices;
    std::array<int16_t, 256> slot_by_index;
    uint8_t received_count = 0;
    std::bitset<256> received_mask;

    void init(uint8_t data_cnt, uint8_t fec_cnt, size_t block_bytes) {
        (void)block_bytes;
        data_block_count = data_cnt;
        fec_block_count = fec_cnt;
        params_known = true;
        block_storage.reserve(data_cnt + fec_cnt);
        block_indices.reserve(data_cnt + fec_cnt);
        slot_by_index.fill(-1);
    }

    bool addBlock(uint8_t block_index, const uint8_t* payload, size_t block_bytes) {
        if (decoded) return false;
        if (received_mask.test(block_index)) return false;

        received_mask.set(block_index);
        slot_by_index[block_index] = static_cast<int16_t>(block_storage.size());
        received_count++;

        block_storage.emplace_back(payload, payload + block_bytes);
        block_indices.push_back(block_index);
        return true;
    }

    bool canDecode() const {
        return params_known && !decoded && received_count >= data_block_count;
    }
};

// 内部：一个正在组装的帧
struct PendingFrame {
    uint16_t frame_id = 0;
    FrameType frame_type = 0;
    uint32_t frame_size = 0;
    uint8_t fec_group_count = 0;
    size_t block_bytes = 0;

    std::vector<FecGroupState> groups;
    uint64_t first_block_time_us = 0;
    uint64_t last_block_time_us = 0;

    bool complete = false;
    bool discarded = false;   // 超时标记：按序提交时跳过并释放

    bool allGroupsDecoded() const {
        if (groups.size() != fec_group_count) return false;
        for (const auto& g : groups) {
            if (!g.decoded) return false;
        }
        return true;
    }
};

struct FrameAssembler::Impl {
    FrameAssemblerConfig config;
    FrameReadyCallback frame_ready_cb;
    FrameAssemblerStats stats;
    CM256 cm256;

    // Pending frames keyed by frame_id (O(1) lookup per packet)
    std::unordered_map<uint16_t, PendingFrame> pending_frames;

    // 按序提交游标（与 receiver.cpp 相同语义）
    uint16_t next_deliver_frame_id = 0;
    bool has_delivery_cursor = false;
    uint64_t last_deliver_us = 0;   // 最近一次成功交付的帧时间（缺口检测用）

    // tick 节流与统计窗口
    uint64_t next_tick_us = 0;
    uint64_t last_tick_now_us = 0;
    uint64_t last_stats_time_us = 0;
    uint64_t next_stats_time_us = 0;
    uint64_t window_bytes = 0;

    Impl(const FrameAssemblerConfig& cfg) : config(cfg) {}

    void handleData(const CommonHeader& hdr, const uint8_t* payload, size_t payload_len,
                    uint64_t now_us) {
        (void)hdr;
        DataHeader data_hdr;
        if (!DataHeader::deserialize(payload, payload_len, data_hdr)) return;

        // 已交付或已丢弃旧帧的迟到分块不再需要（与 receiver.cpp 相同）
        if (has_delivery_cursor) {
            int16_t diff = static_cast<int16_t>(data_hdr.frame_id - next_deliver_frame_id);
            if (diff < 0) return;
        }

        const uint8_t* block_payload = payload + DataHeader::serializedSize();
        size_t block_payload_len = payload_len - DataHeader::serializedSize();
        if (block_payload_len < data_hdr.payload_length) return;

        PendingFrame* frame = findOrCreateFrame(data_hdr, now_us);
        if (!frame) return;

        size_t block_bytes = data_hdr.payload_length;
        if (frame->block_bytes == 0) {
            frame->block_bytes = block_bytes;
        }

        if (frame->groups.size() < frame->fec_group_count) {
            frame->groups.resize(frame->fec_group_count);
        }

        uint8_t group_idx = data_hdr.fec_group_index;
        if (group_idx >= frame->groups.size()) return;

        FecGroupState& group = frame->groups[group_idx];
        if (!group.params_known) {
            group.init(data_hdr.data_block_count, data_hdr.fec_block_count, block_bytes);
        }

        group.addBlock(data_hdr.block_index, block_payload, block_bytes);
        frame->last_block_time_us = now_us;

        if (group.canDecode()) {
            tryDecode(group, block_bytes);
        }

        // 收全后不直接提交：按帧序（游标）提交，避免后到先交造成乱序
        if (frame->allGroupsDecoded() && !frame->discarded) {
            frame->complete = true;
        }
        tryDeliverInOrder();
    }

    PendingFrame* findOrCreateFrame(const DataHeader& data_hdr, uint64_t now_us) {
        auto existing = pending_frames.find(data_hdr.frame_id);
        if (existing != pending_frames.end() &&
            !existing->second.complete && !existing->second.discarded) {
            return &existing->second;
        }

        // 名额腾挪：先清掉已超时丢弃的帧（它们在等游标跳过，可安全释放名额）
        if (pending_frames.size() >= config.max_pending_frames) {
            for (auto it = pending_frames.begin(); it != pending_frames.end(); ) {
                if (it->second.discarded) {
                    it = pending_frames.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (pending_frames.size() >= config.max_pending_frames && !pending_frames.empty()) {
            // 淘汰最早开始的帧
            auto oldest = pending_frames.begin();
            for (auto it = pending_frames.begin(); it != pending_frames.end(); ++it) {
                if (it->second.first_block_time_us < oldest->second.first_block_time_us) {
                    oldest = it;
                }
            }
            stats.frames_evicted++;
            pending_frames.erase(oldest);
        }

        PendingFrame frame;
        frame.frame_id = data_hdr.frame_id;
        frame.frame_type = data_hdr.frame_type;
        frame.frame_size = data_hdr.frame_size;
        frame.fec_group_count = data_hdr.fec_group_count;
        frame.first_block_time_us = now_us;
        frame.last_block_time_us = now_us;
        frame.groups.resize(data_hdr.fec_group_count);

        auto result = pending_frames.emplace(data_hdr.frame_id, std::move(frame));
        // 首帧到达即建立提交游标
        if (!has_delivery_cursor) {
            has_delivery_cursor = true;
            next_deliver_frame_id = data_hdr.frame_id;
            tryDeliverInOrder();
        }
        return &result.first->second;
    }

    void tryDecode(FecGroupState& group, size_t block_bytes) {
        if (group.decoded) return;

        bool have_all_originals = true;
        for (uint8_t i = 0; i < group.data_block_count; ++i) {
            if (!group.received_mask.test(i)) {
                have_all_originals = false;
                break;
            }
        }
        if (have_all_originals) {
            group.decoded = true;
            return;
        }

        if (group.received_count < group.data_block_count) return;

        CM256::cm256_encoder_params params;
        params.OriginalCount = group.data_block_count;
        params.RecoveryCount = group.fec_block_count;
        params.BlockBytes = static_cast<int>(block_bytes);

        std::vector<CM256::cm256_block> blocks(group.data_block_count);
        for (uint8_t i = 0; i < group.data_block_count; ++i) {
            blocks[i].Block = group.block_storage[i].data();
            blocks[i].Index = group.block_indices[i];
        }

        int ret = cm256.cm256_decode(params, blocks.data());
        if (ret == 0) {
            group.decoded = true;
            // cm256 恢复后更新 index 映射
            for (uint8_t i = 0; i < group.data_block_count; ++i) {
                group.block_indices[i] = blocks[i].Index;
                group.slot_by_index[blocks[i].Index] = static_cast<int16_t>(i);
            }
        }
    }

    // 按序提交：仅游标帧收全（或已被跳过）时才交付；后续帧先收全也必须等待
    void tryDeliverInOrder() {
        if (!has_delivery_cursor) return;

        // 游标之前的陈旧帧永远不会被交付，直接释放
        for (auto it = pending_frames.begin(); it != pending_frames.end(); ) {
            int16_t diff = static_cast<int16_t>(it->first - next_deliver_frame_id);
            if (diff < 0) {
                it = pending_frames.erase(it);
            } else {
                ++it;
            }
        }

        for (;;) {
            auto it = pending_frames.find(next_deliver_frame_id);
            if (it == pending_frames.end()) break;

            PendingFrame& frame = it->second;
            if (frame.discarded) {
                pending_frames.erase(it);
                next_deliver_frame_id++;
                continue;
            }
            if (!frame.complete) break;

            deliverFrame(frame);
            pending_frames.erase(it);
            next_deliver_frame_id++;
        }
    }

    void deliverFrame(PendingFrame& frame) {
        Frame output;
        output.type = frame.frame_type;
        output.size = frame.frame_size;
        output.data.resize(frame.frame_size);

        size_t write_offset = 0;
        size_t remaining = frame.frame_size;

        for (uint8_t g = 0; g < frame.fec_group_count && remaining > 0; ++g) {
            if (g >= frame.groups.size()) break;
            const FecGroupState& group = frame.groups[g];

            for (uint8_t blk = 0; blk < group.data_block_count && remaining > 0; ++blk) {
                int16_t slot = group.slot_by_index[blk];
                if (slot < 0 || slot >= static_cast<int16_t>(group.block_storage.size())) break;
                const uint8_t* src = group.block_storage[slot].data();

                size_t copy_len = std::min(remaining, frame.block_bytes);
                std::memcpy(output.data.data() + write_offset, src, copy_len);
                write_offset += copy_len;
                remaining -= copy_len;
            }
        }

        stats.frames_assembled++;
        stats.bytes_assembled += frame.frame_size;
        window_bytes += frame.frame_size;
        last_deliver_us = frame.last_block_time_us;

        if (frame_ready_cb) {
            frame_ready_cb(std::move(output));
        }
    }

    void checkFrameTimeouts(uint64_t now_us) {
        uint64_t timeout_us = static_cast<uint64_t>(config.frame_timeout_ms) * 1000;

        for (auto& kv : pending_frames) {
            PendingFrame& frame = kv.second;
            if (!frame.complete && !frame.discarded &&
                (now_us - frame.first_block_time_us) > timeout_us) {
                frame.discarded = true;
                stats.frames_timed_out++;
            }
        }

        // 游标帧可能刚被标记丢弃：尝试推进游标
        tryDeliverInOrder();

        // 投喂缺口恢复：整段帧未投喂（如无订阅者期间跳过组帧）时，游标帧
        // 不在 pending 中，"超时丢弃"无法推进游标。若自上次交付已超过
        // frame_timeout 且存在更晚的 pending 帧，则跳过节段缺口。
        if (has_delivery_cursor && last_deliver_us != 0 &&
            (now_us - last_deliver_us) > timeout_us && !pending_frames.empty()) {
            uint16_t best_id = 0;
            int16_t best_diff = 0;
            for (const auto& kv : pending_frames) {
                int16_t d = static_cast<int16_t>(kv.first - next_deliver_frame_id);
                if (d <= 0) continue;
                if (best_diff == 0 || d < best_diff) {
                    best_diff = d;
                    best_id = kv.first;
                }
            }
            if (best_diff > 0) {
                stats.cursor_gap_jumps++;
                next_deliver_frame_id = best_id;
                tryDeliverInOrder();
            }
        }
    }
};

FrameAssembler::FrameAssembler(const FrameAssemblerConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

FrameAssembler::~FrameAssembler() = default;

void FrameAssembler::setFrameReadyCallback(FrameReadyCallback cb) {
    impl_->frame_ready_cb = std::move(cb);
}

void FrameAssembler::onDataPacket(const uint8_t* data, size_t len, uint64_t now_us) {
    CommonHeader hdr;
    size_t consumed = 0;
    if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;

    // 只消费 DATA；其余信令由代理层负责，这里一概忽略（零回包）
    if (hdr.msg_type != MsgType::DATA) return;

    if (now_us == 0) now_us = impl_->last_tick_now_us;

    impl_->stats.packets_received++;
    impl_->handleData(hdr, data + consumed, len - consumed, now_us);
    impl_->stats.pending_frames = impl_->pending_frames.size();
}

void FrameAssembler::tick(uint64_t now_us) {
    impl_->last_tick_now_us = now_us;

    // 超时扫描按 tick_interval 节流（高频调用安全）
    if (impl_->next_tick_us == 0 || now_us >= impl_->next_tick_us) {
        impl_->checkFrameTimeouts(now_us);
        impl_->next_tick_us = now_us +
            static_cast<uint64_t>(impl_->config.tick_interval_ms) * 1000;
    }

    // 速率统计窗口
    if (impl_->next_stats_time_us == 0 || now_us >= impl_->next_stats_time_us) {
        if (impl_->last_stats_time_us > 0 && now_us > impl_->last_stats_time_us) {
            uint64_t elapsed_us = now_us - impl_->last_stats_time_us;
            impl_->stats.assembled_kbps = static_cast<uint32_t>(
                impl_->window_bytes * 8 * 1000 / elapsed_us);
        }
        impl_->window_bytes = 0;
        impl_->last_stats_time_us = now_us;
        impl_->next_stats_time_us = now_us +
            static_cast<uint64_t>(impl_->config.stats_interval_ms) * 1000;
    }

    impl_->stats.pending_frames = impl_->pending_frames.size();
}

const FrameAssemblerStats& FrameAssembler::getStats() const {
    return impl_->stats;
}

} // namespace fec_protocol