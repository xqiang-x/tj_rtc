#include "receiver.h"
#include "protocol.h"
#include "cm256.h"
#include <unordered_map>
#include <vector>
#include <cstring>
#include <algorithm>
#include <array>
#include <bitset>

namespace fec_protocol {

// Internal: state of one FEC group within a pending frame
struct FecGroupState {
    uint8_t data_block_count = 0;
    uint8_t fec_block_count = 0;
    bool params_known = false;
    bool decoded = false;

    FecGroupState() { slot_by_index.fill(-1); }

    // Received block storage (each exactly block_bytes long)
    std::vector<std::vector<uint8_t>> block_storage;
    // Block descriptors for cm256
    std::vector<uint8_t> block_indices;  // Which block_index each slot holds
    // O(1) map: block_index -> storage slot (or -1)
    std::array<int16_t, 256> slot_by_index;
    uint8_t received_count = 0;
    std::bitset<256> received_mask;

    void init(uint8_t data_cnt, uint8_t fec_cnt, size_t block_bytes) {
        data_block_count = data_cnt;
        fec_block_count = fec_cnt;
        params_known = true;
        // Pre-allocate for max blocks we might receive
        block_storage.reserve(data_cnt + fec_cnt);
        block_indices.reserve(data_cnt + fec_cnt);
        slot_by_index.fill(-1);
    }

    bool addBlock(uint8_t block_index, const uint8_t* payload, size_t block_bytes) {
        if (decoded) return false;
        if (received_mask.test(block_index)) return false; // Duplicate

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

// Internal: a frame being reassembled
struct PendingFrame {
    uint16_t frame_id = 0;
    FrameType frame_type = 0;
    uint32_t frame_size = 0;
    uint8_t fec_group_count = 0;
    size_t block_bytes = 0;

    std::vector<FecGroupState> groups;
    uint64_t first_block_time_us = 0;
    uint64_t last_block_time_us = 0;

    // Sequence tracking
    SeqNum first_seq = 0;
    SeqNum last_seq = 0;
    bool first_seq_known = false;

    bool complete = false;
    bool discarded = false;   // 超时丢弃标记：按序提交时跳过并释放

    bool allGroupsDecoded() const {
        if (groups.size() != fec_group_count) return false;
        for (const auto& g : groups) {
            if (!g.decoded) return false;
        }
        return true;
    }
};

// Internal: NACK tracking for a missing sequence
struct NackState {
    uint64_t first_detected_us = 0;
    uint64_t last_nack_sent_us = 0;
    uint32_t nack_count = 0;
};

struct FrameReceiver::Impl {
    ReceiverConfig config;
    FrameReadyCallback frame_ready_cb;
    SendPacketCallback send_cb;
    FrameDroppedCallback frame_dropped_cb;
    ProtocolStats stats;
    CM256 cm256;

    SeqNum cmd_seq = 0;

    // PLI (key-frame request) client-side throttle
    uint64_t last_pli_sent_us = 0;

    // Pending frames keyed by frame_id (O(1) lookup per packet)
    std::unordered_map<uint16_t, PendingFrame> pending_frames;

    // 按序提交游标：指向当前应提交的帧（也是唯一允许保留的“旧帧”）。
    // 只有游标帧收全才提交，其后先收全的帧必须等待；游标之前的帧一律视为陈旧
    uint16_t next_deliver_frame_id = 0;
    bool has_delivery_cursor = false;

    // Sequence tracking for gap detection
    // 默认 0：令 detectGaps 以首包 seq 建立连续性游标；若默认 1，首包
    // （block_index==0 时）会把 seq=1..首包 seq 的历史块全部误判缺失 → NACK 风暴
    SeqNum expected_data_seq = 0;
    SeqNum completed_seq = 0;

    // NACK state: seq -> state
    std::unordered_map<SeqNum, NackState> nack_states;

    // 链路 RTT（由 PING 携带的上游实测值更新）：NACK 重试按 1×RTT 间隔
    uint32_t rtt_ms = 0;

    // Stats tracking
    uint64_t last_stats_time_us = 0;
    uint64_t next_stats_send_us = 0;
    uint64_t total_blocks_received = 0;
    uint64_t total_blocks_expected = 0;

    // 按统计窗口（STATS 间隔）计的计数器，每窗口重置
    uint64_t window_blocks_received = 0;
    uint64_t window_blocks_lost = 0;
    uint64_t window_blocks_recovered_fec = 0;
    uint64_t window_blocks_recovered_nack = 0;
    uint32_t window_packets = 0;
    uint64_t goodput_window_bytes = 0;   // 窗口内完成帧的字节数
    uint64_t last_stats_send_us = 0;

    // FEC 组布局：group_key -> 组的 seq 范围与数据块数。
    // 用于区分"缺失的 seq 是数据块还是 FEC 冗余块"：FEC 块可被中间节点
    // （如 SFU 代理的冗余过滤）有意丢弃，其缺失不算丢包、不触发 NACK。
    struct GroupLayout {
        SeqNum start_seq;
        uint8_t data_count;
        uint8_t total_count;
    };
    std::unordered_map<uint32_t, GroupLayout> group_layouts;

    Impl(const ReceiverConfig& cfg) : config(cfg) {}

    SeqNum nextCmdSeq() {
        cmd_seq++;
        return cmd_seq;
    }

    size_t commonHeaderSize(uint8_t seq_len) const {
        return 2 + seq_len;
    }

    void sendPacket(const uint8_t* data, size_t len) {
        if (send_cb) send_cb(data, len);
    }

    void handleData(const CommonHeader& hdr, const uint8_t* payload, size_t payload_len,
                    uint64_t now_us) {
        DataHeader data_hdr;
        if (!DataHeader::deserialize(payload, payload_len, data_hdr)) return;

        // 已交付或已超时丢弃的旧帧：其迟到的分块不再需要。
        // 按序提交下，游标之前的帧不可能还处于未完成状态——未完成的旧帧必然
        // 就是游标帧自身（diff == 0，放行以等待补齐）
        if (has_delivery_cursor) {
            int16_t diff = static_cast<int16_t>(data_hdr.frame_id - next_deliver_frame_id);
            if (diff < 0) {
                return; // Stale packet from a delivered/discarded frame
            }
        }

        const uint8_t* block_payload = payload + DataHeader::serializedSize();
        size_t block_payload_len = payload_len - DataHeader::serializedSize();
        if (block_payload_len < data_hdr.payload_length) return;

        total_blocks_received++;
        window_blocks_received++;
        window_packets++;

        // Record the FEC group layout (once per group) so gaps can be classified
        // as data-block loss vs FEC-redundancy drop.
        bool first_of_group = false;
        SeqNum group_start_seq = hdr.seq_num - data_hdr.block_index;
        {
            uint32_t gkey = (static_cast<uint32_t>(data_hdr.frame_id) << 8) |
                            static_cast<uint32_t>(data_hdr.fec_group_index);
            if (group_layouts.find(gkey) == group_layouts.end()) {
                GroupLayout gl;
                gl.start_seq = group_start_seq;
                gl.data_count = data_hdr.data_block_count;
                gl.total_count = static_cast<uint8_t>(
                    data_hdr.data_block_count + data_hdr.fec_block_count);
                group_layouts[gkey] = gl;
                first_of_group = true;
            }
        }

        // Detect sequence gaps for NACK.
        // 组内首包 block_index>0 时（如订阅中途加入），组起始块本身也缺失：
        // 显式加入 NACK 跟踪（detectGaps 会把游标起始 seq 当作"已收到"），
        // 再由 detectGaps(当前 seq) 覆盖中间的缺失块
        if (first_of_group && data_hdr.block_index > 0 &&
            (expected_data_seq == 0 || group_start_seq >= expected_data_seq)) {
            if (expected_data_seq == 0) expected_data_seq = group_start_seq;
            if (nack_states.find(group_start_seq) == nack_states.end() &&
                !isFecSeq(group_start_seq)) {
                NackState state;
                state.first_detected_us = now_us;
                state.last_nack_sent_us = 0;
                state.nack_count = 0;
                nack_states[group_start_seq] = state;
                stats.blocks_lost++;
                window_blocks_lost++;
            }
            expected_data_seq = group_start_seq + 1;
        }
        detectGaps(hdr.seq_num, now_us);

        // Remove from NACK tracking if this was a missing block
        auto nack_it = nack_states.find(hdr.seq_num);
        if (nack_it != nack_states.end()) {
            stats.blocks_recovered_nack++;
            window_blocks_recovered_nack++;
            nack_states.erase(nack_it);
        }

        // Find or create the pending frame this block belongs to
        PendingFrame* frame = findOrCreateFrame(hdr.seq_num, data_hdr, now_us);
        if (!frame) return;

        // Determine block_bytes (all blocks in a frame must have same block_bytes)
        size_t block_bytes = data_hdr.payload_length;
        if (frame->block_bytes == 0) {
            frame->block_bytes = block_bytes;
        }

        // Ensure group vector is properly sized
        if (frame->groups.size() < frame->fec_group_count) {
            frame->groups.resize(frame->fec_group_count);
        }

        uint8_t group_idx = data_hdr.fec_group_index;
        if (group_idx >= frame->groups.size()) return;

        FecGroupState& group = frame->groups[group_idx];
        if (!group.params_known) {
            group.init(data_hdr.data_block_count, data_hdr.fec_block_count, block_bytes);
        }

        // 修正帧起始 seq：first_seq 用于估算 completed_seq（发送端据此清理重传缓存）。
        // 首个收到的块未必是帧内第 0 块，需减去帧内偏移（前面各组总块数 + 组内 index），
        // 否则 completed_seq 高估 → 发送端过早清掉重传缓存 → 上游 NACK 无法恢复
        {
            uint32_t prefix = 0;
            bool prefix_known = (group_idx == 0);
            for (uint8_t gi = 0; gi < group_idx && !prefix_known; ++gi) {
                auto lit = group_layouts.find(
                    (static_cast<uint32_t>(data_hdr.frame_id) << 8) | gi);
                if (lit == group_layouts.end()) break;
                prefix += lit->second.total_count;
                if (gi + 1 == group_idx) prefix_known = true;
            }
            if (prefix_known) {
                SeqNum true_first = hdr.seq_num - data_hdr.block_index - prefix;
                if (true_first < frame->first_seq) frame->first_seq = true_first;
            }
        }

        // Add block to group
        group.addBlock(data_hdr.block_index, block_payload, block_bytes);
        frame->last_block_time_us = now_us;

        // Try FEC decode if enough blocks
        if (group.canDecode()) {
            tryDecode(group, block_bytes, data_hdr.frame_id, data_hdr.fec_group_index);
        }

        // 收全后不直接提交：按帧序（游标）提交，避免后到先交造成乱序。
        // 已超时丢弃的帧即使后续补块凑齐也不交付（等待关键帧重建连续流）
        if (frame->allGroupsDecoded() && !frame->discarded) {
            frame->complete = true;
        }
        tryDeliverInOrder();
    }

    PendingFrame* findOrCreateFrame(SeqNum seq, const DataHeader& data_hdr, uint64_t now_us) {
        // Find existing frame by frame_id (O(1))
        auto existing = pending_frames.find(data_hdr.frame_id);
        if (existing != pending_frames.end() &&
            !existing->second.complete && !existing->second.discarded) {
            return &existing->second;
        }

        // Check max pending frames limit
        // 先清掉已超时丢弃的帧（它们在等游标跳过，可安全释放名额）
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
            // Drop the frame waiting longest
            auto oldest = pending_frames.begin();
            for (auto it = pending_frames.begin(); it != pending_frames.end(); ++it) {
                if (it->second.first_block_time_us < oldest->second.first_block_time_us) {
                    oldest = it;
                }
            }
            if (frame_dropped_cb) {
                frame_dropped_cb(oldest->second.frame_type, oldest->second.frame_size);
            }
            stats.frames_dropped++;
            maybeSendPli(now_us, oldest->second.frame_type);
            pending_frames.erase(oldest);
        }

        // Create new pending frame
        PendingFrame frame;
        frame.frame_id = data_hdr.frame_id;
        frame.frame_type = data_hdr.frame_type;
        frame.frame_size = data_hdr.frame_size;
        frame.fec_group_count = data_hdr.fec_group_count;
        frame.first_block_time_us = now_us;
        frame.last_block_time_us = now_us;
        frame.first_seq = seq;
        frame.first_seq_known = true;
        frame.groups.resize(data_hdr.fec_group_count);

        auto result = pending_frames.emplace(data_hdr.frame_id, std::move(frame));
        // 首帧到达即建立提交游标（发送端按帧序发包，首帧即最早的帧）；
        // 若游标帧已收全（组参数首包就齐全），立即尝试按序提交
        if (!has_delivery_cursor) {
            has_delivery_cursor = true;
            next_deliver_frame_id = data_hdr.frame_id;
            tryDeliverInOrder();
        }
        return &result.first->second;
    }

    void tryDecode(FecGroupState& group, size_t block_bytes,
                   uint16_t frame_id, uint8_t group_idx) {
        if (group.decoded) return;

        // Check if we have all original blocks (no FEC needed)
        bool have_all_originals = true;
        for (uint8_t i = 0; i < group.data_block_count; ++i) {
            if (!group.received_mask.test(i)) {
                have_all_originals = false;
                break;
            }
        }

        if (have_all_originals) {
            group.decoded = true;
            // 组已完整，无需再对该组范围内任一缺失 seq 发 NACK
            eraseGroupNacks(frame_id, group_idx);
            return;
        }

        // Need FEC decoding
        if (group.received_count < group.data_block_count) return;

        CM256::cm256_encoder_params params;
        params.OriginalCount = group.data_block_count;
        params.RecoveryCount = group.fec_block_count;
        params.BlockBytes = static_cast<int>(block_bytes);

        // Build cm256_block array with exactly data_block_count entries
        std::vector<CM256::cm256_block> blocks(group.data_block_count);
        for (uint8_t i = 0; i < group.data_block_count; ++i) {
            blocks[i].Block = group.block_storage[i].data();
            blocks[i].Index = group.block_indices[i];
        }

        // Count originals present now, before decode rewrites everything
        uint8_t originals_before = countOriginalBlocks(group);

        int ret = cm256.cm256_decode(params, blocks.data());
        if (ret == 0) {
            group.decoded = true;
            // Update block_indices after decode (cm256 updates Index for recovered blocks)
            for (uint8_t i = 0; i < group.data_block_count; ++i) {
                group.block_indices[i] = blocks[i].Index;
                group.slot_by_index[blocks[i].Index] = static_cast<int16_t>(i);
            }
            stats.blocks_recovered_fec += (group.data_block_count - originals_before);
            window_blocks_recovered_fec += (group.data_block_count - originals_before);

            // FEC 已恢复该组全部缺失块：整个组 seq 范围内的 NACK 全部清除，
            // 防止已恢复的数据块仍被反复请求重传（FEC 恢复 → 不再请求）
            eraseGroupNacks(frame_id, group_idx);
        }
    }

    // 组解码成功后，删除该组 seq 范围内所有 NACK 跟踪（缺失已被 FEC 补齐）
    void eraseGroupNacks(uint16_t frame_id, uint8_t group_idx) {
        uint32_t gkey = (static_cast<uint32_t>(frame_id) << 8) |
                        static_cast<uint32_t>(group_idx);
        auto lit = group_layouts.find(gkey);
        if (lit == group_layouts.end()) return;
        const GroupLayout& gl = lit->second;
        SeqNum begin_seq = gl.start_seq;
        SeqNum end_seq = gl.start_seq + static_cast<SeqNum>(gl.total_count);

        std::vector<SeqNum> to_erase;
        for (const auto& kv : nack_states) {
            if (kv.first >= begin_seq && kv.first < end_seq) {
                to_erase.push_back(kv.first);
            }
        }
        for (SeqNum seq : to_erase) {
            nack_states.erase(seq);
        }
    }

    uint8_t countOriginalBlocks(const FecGroupState& group) const {
        uint8_t count = 0;
        for (uint8_t i = 0; i < group.data_block_count && i < group.block_indices.size(); ++i) {
            if (group.block_indices[i] < group.data_block_count) {
                count++;
            }
        }
        return count;
    }

    // 按序提交：仅游标帧收全（或已被跳过）时才交付，确保内部帧按帧序上抛。
    // 后续帧即使先收全也必须等待游标帧；游标帧超时丢弃后自动前进，
    // 其后若已有收全帧则立即追缴提交
    void tryDeliverInOrder() {
        if (!has_delivery_cursor) return;

        // 清理游标之前的陈旧帧（订阅中途进入、重传风暴导致的落后帧）：
        // 游标不回退，这些帧永远不会被交付，滞留会挤占 max_pending_frames 触发丢帧
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
            if (it == pending_frames.end()) break;      // 游标帧尚未到达，等待
            PendingFrame& frame = it->second;

            if (frame.discarded) {
                // 游标帧已超时丢弃：释放并继续检查下一帧
                pending_frames.erase(it);
                next_deliver_frame_id++;
                continue;
            }
            if (!frame.complete) break;                 // 游标帧未收全，必须等待

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

            // Collect data blocks in order (index 0 .. data_block_count-1)
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

        stats.frames_completed++;
        goodput_window_bytes += frame.frame_size;

        // Update completed_seq, then deliver (caller removes from pending)
        updateCompletedSeq(frame);
        if (frame_ready_cb) {
            frame_ready_cb(std::move(output));
        }
    }

    void updateCompletedSeq(const PendingFrame& frame) {
        // Estimate the last sequence number of this frame
        uint32_t total_blocks = 0;
        for (const auto& g : frame.groups) {
            total_blocks += g.data_block_count + g.fec_block_count;
        }
        SeqNum last = frame.first_seq + total_blocks - 1;

        // ACK 不能越过仍在等待补块的旧帧：发送端按 completed_seq 清理重传缓存，
        // 若越过未完成帧，其缺失块的重传缓存会被提前清掉 → NACK 永远无法恢复；
        // 已超时丢弃的帧则不再阻塞（发送端须清理其重传缓存）
        for (const auto& pf : pending_frames) {
            if (pf.second.complete || pf.second.discarded) continue;
            SeqNum pf_first = pf.second.first_seq;
            if (pf_first == 0) continue;
            if (pf_first <= last) last = pf_first - 1;
        }

        if (last > completed_seq) {
            completed_seq = last;
        }
    }

    // Returns true if seq is known to be an FEC-redundancy block (droppable).
    // Unknown seqs are treated conservatively as data blocks.
    bool isFecSeq(SeqNum seq) const {
        for (const auto& kv : group_layouts) {
            const GroupLayout& gl = kv.second;
            if (seq >= gl.start_seq &&
                seq < gl.start_seq + static_cast<SeqNum>(gl.total_count)) {
                SeqNum off = seq - gl.start_seq;
                return off >= gl.data_count;
            }
        }
        return false;
    }

    // A layout is only needed until its whole seq range lies below the expected-seq
    // cursor; any gap inside it has already been detected and classified by then.
    // (Pruning by frame-completion is wrong: a frame can complete on its data blocks
    // while its trailing FEC block is still pending/dropped and not yet classified.)
    void pruneGroupLayouts() {
        for (auto it = group_layouts.begin(); it != group_layouts.end(); ) {
            const GroupLayout& gl = it->second;
            if (gl.start_seq + static_cast<SeqNum>(gl.total_count) <= expected_data_seq) {
                it = group_layouts.erase(it);
            } else {
                ++it;
            }
        }
    }

    void detectGaps(SeqNum received_seq, uint64_t now_us) {
        if (expected_data_seq == 0) {
            expected_data_seq = received_seq;
        }

        if (received_seq > expected_data_seq) {
            // 防御：异常大缺口（>512 块 ≈ 数百毫秒数据）说明流状态错乱
            // （启动 seq 基准错乱、长时间断流恢复等），缺口内的历史块已不可能
            // 存在于任何重传缓存，逐块 NACK 只会形成风暴；直接重置游标并等待
            // 帧超时丢弃 + PLI 关键帧重建
            if (received_seq - expected_data_seq > 512) {
                expected_data_seq = received_seq + 1;
                return;
            }

            // Gap detected: expected_data_seq .. received_seq-1 are missing
            SeqNum missing = expected_data_seq;
            while (missing < received_seq) {
                if (nack_states.find(missing) == nack_states.end()) {
                    if (!isFecSeq(missing)) {
                        NackState state;
                        state.first_detected_us = now_us;
                        state.last_nack_sent_us = 0;
                        state.nack_count = 0;
                        nack_states[missing] = state;
                        stats.blocks_lost++;
                        window_blocks_lost++;
                    }
                    // FEC-redundancy gaps are not counted as loss nor NACKed:
                    // intermediate nodes may intentionally drop FEC blocks.
                }
                missing++;
            }
        }

        if (received_seq >= expected_data_seq) {
            expected_data_seq = received_seq + 1;
        }
    }

    void handlePing(const CommonHeader& hdr, const uint8_t* payload, size_t payload_len) {
        std::vector<TlvItem> items;
        if (!deserializeTlvPayload(payload, payload_len, items)) return;

        // Find timestamp TLV
        Timestamp ping_ts = 0;
        for (const auto& item : items) {
            if (item.type == TlvType::TIMESTAMP && item.value.size() == 8) {
                ping_ts = readBE64(item.value.data());
                break;
            }
        }

        // PING 携带上游实测 RTT（2B BE ms）：用于 NACK 重试按 1×RTT 间隔
        for (const auto& item : items) {
            if (item.type == TlvType::RTT && item.value.size() == 2) {
                rtt_ms = readBE16(item.value.data());
                break;
            }
        }

        // Build PONG response
        SeqNum pong_seq = nextCmdSeq();
        uint8_t pong_seq_len = computeSeqLen(pong_seq);
        if (pong_seq_len == 0) pong_seq_len = 1;

        std::vector<uint8_t> buf(commonHeaderSize(pong_seq_len) + 2 + 10);

        CommonHeader pong_hdr;
        pong_hdr.version = kProtocolVersion;
        pong_hdr.seq_len = pong_seq_len;
        pong_hdr.msg_type = MsgType::PONG;
        pong_hdr.seq_num = pong_seq;

        size_t offset = pong_hdr.serialize(buf.data(), buf.size());

        // PayloadLength = 10
        writeBE16(buf.data() + offset, 10);
        offset += 2;

        // TLV: Echo Timestamp
        buf[offset++] = static_cast<uint8_t>(TlvType::ECHO_TIMESTAMP);
        buf[offset++] = 8;
        writeBE64(buf.data() + offset, ping_ts);
        offset += 8;

        sendPacket(buf.data(), offset);
    }

    void markNackSent(SeqNum seq, uint64_t now_us) {
        auto it = nack_states.find(seq);
        if (it != nack_states.end()) {
            it->second.last_nack_sent_us = now_us;
            it->second.nack_count++;
        }
    }

    void generateNacks(uint64_t now_us) {
        uint64_t delay_us = static_cast<uint64_t>(config.nack_delay_ms) * 1000;
        // 重试间隔：按 1×RTT（至少不高于配置节流值，防止 RTT 极小或未知时 NACK 风暴）
        uint64_t interval_us = (rtt_ms > 0)
            ? (static_cast<uint64_t>(rtt_ms) * 1000)
            : (static_cast<uint64_t>(config.nack_interval_ms) * 1000);
        if (interval_us < static_cast<uint64_t>(config.nack_interval_ms) * 1000) {
            interval_us = static_cast<uint64_t>(config.nack_interval_ms) * 1000;
        }

        // Drop entries that exhausted retries; collect seqs due for a NACK
        std::vector<SeqNum> due;
        uint64_t lifetime_us = static_cast<uint64_t>(config.frame_timeout_ms) * 1000;
        for (auto it = nack_states.begin(); it != nack_states.end(); ) {
            NackState& state = it->second;
            // 生命周期：缺块超过帧超时（默认 2s）后，对应帧已被按序提交丢弃，
            // 继续重试不会恢复任何帧，反而在丢包下形成 NACK 风暴，必须清除
            if (now_us - state.first_detected_us >= lifetime_us) {
                it = nack_states.erase(it);
                continue;
            }
            if (state.nack_count >= config.max_nack_retries) {
                it = nack_states.erase(it);
                continue;
            }
            bool should_send = (state.last_nack_sent_us == 0)
                ? (now_us - state.first_detected_us >= delay_us)
                : (now_us - state.last_nack_sent_us >= interval_us);
            if (should_send) due.push_back(it->first);
            ++it;
        }

        if (due.empty()) return;
        std::sort(due.begin(), due.end());

        // Batch: each entry covers lost_start_seq plus up to 32 later seqs via bitmask
        std::vector<NackEntry> entries;
        std::vector<bool> covered(due.size(), false);
        for (size_t i = 0; i < due.size(); ++i) {
            if (covered[i]) continue;

            NackEntry entry;
            entry.lost_start_seq = due[i];
            entry.bitmask = 0;
            covered[i] = true;
            markNackSent(due[i], now_us);

            for (size_t j = i + 1; j < due.size(); ++j) {
                if (covered[j]) continue;
                SeqNum off = due[j] - due[i];
                if (off > 32) break; // sorted: all later seqs are further away
                entry.bitmask |= (1u << (off - 1));
                covered[j] = true;
                markNackSent(due[j], now_us);
            }
            entries.push_back(entry);
        }

        sendNackPacket(entries);
    }

    void sendNackPacket(const std::vector<NackEntry>& entries) {
        // 分片发送：长时间停滞后积压的缺失块可能产生上千条目，
        // 单个超大 UDP 数据报会因 EMSGSIZE 发送失败，导致永远无法恢复
        constexpr size_t kMaxEntriesPerPacket = 100;
        for (size_t begin = 0; begin < entries.size(); begin += kMaxEntriesPerPacket) {
            size_t end = std::min(begin + kMaxEntriesPerPacket, entries.size());
            size_t count = end - begin;

            SeqNum seq = nextCmdSeq();
            uint8_t seq_len = computeSeqLen(seq);
            if (seq_len == 0) seq_len = 1;

            size_t payload_size = 2 + count * kNackEntrySize;
            std::vector<uint8_t> buf(commonHeaderSize(seq_len) + payload_size);

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
                sendPacket(buf.data(), offset + written);
            }
        }
    }

    void sendPliPacket() {
        SeqNum seq = nextCmdSeq();
        uint8_t seq_len = computeSeqLen(seq);
        if (seq_len == 0) seq_len = 1;

        // PLI: CommonHeader + 2-byte PayloadLength=0（无 payload，保留 TLV 扩展）
        std::vector<uint8_t> buf(commonHeaderSize(seq_len) + 2);

        CommonHeader hdr;
        hdr.version = kProtocolVersion;
        hdr.seq_len = seq_len;
        hdr.msg_type = MsgType::PLI;
        hdr.seq_num = seq;

        size_t offset = hdr.serialize(buf.data(), buf.size());
        buf[offset] = 0;
        buf[offset + 1] = 0;
        sendPacket(buf.data(), offset + 2);
    }

    // 丢帧时请求关键帧：仅视频帧触发，客户端侧按 pli_min_interval_ms 节流
    void maybeSendPli(uint64_t now_us, FrameType type) {
        if (type != frame_type::VIDEO && type != frame_type::VIDEO_IDR &&
            type != frame_type::VIDEO_PARAMS) {
            return;
        }
        uint64_t interval_us = static_cast<uint64_t>(config.pli_min_interval_ms) * 1000;
        if (last_pli_sent_us != 0 && now_us - last_pli_sent_us < interval_us) return;
        last_pli_sent_us = now_us;
        sendPliPacket();
    }

    void sendStats(uint64_t now_us) {
        std::vector<TlvItem> items;

        // Loss rate
        {
            TlvItem item;
            item.type = TlvType::LOSS_RATE;
            item.value.resize(1);
            item.value[0] = static_cast<uint8_t>(stats.loss_rate * 255.0f);
            items.push_back(std::move(item));
        }

        // FEC recovery rate
        {
            TlvItem item;
            item.type = TlvType::FEC_RECOVERY_RATE;
            item.value.resize(1);
            item.value[0] = static_cast<uint8_t>(stats.fec_recovery_rate * 255.0f);
            items.push_back(std::move(item));
        }

        // NACK recovery rate
        {
            TlvItem item;
            item.type = TlvType::NACK_RECOVERY_RATE;
            item.value.resize(1);
            item.value[0] = static_cast<uint8_t>(stats.nack_recovery_rate * 255.0f);
            items.push_back(std::move(item));
        }

        // Completed seq
        {
            TlvItem item;
            item.type = TlvType::COMPLETED_SEQ;
            item.value.resize(8);
            writeBE64(item.value.data(), completed_seq);
            items.push_back(std::move(item));
        }

        // Measured goodput (completed-frame bytes over the stats window)
        {
            TlvItem item;
            item.type = TlvType::GOODPUT;
            item.value.resize(4);
            writeBE32(item.value.data(), stats.goodput_bps);
            items.push_back(std::move(item));
        }

        // Serialize
        SeqNum stats_seq = nextCmdSeq();
        uint8_t stats_seq_len = computeSeqLen(stats_seq);
        if (stats_seq_len == 0) stats_seq_len = 1;

        size_t tlv_size = 0;
        for (const auto& item : items) {
            tlv_size += 2 + item.value.size();
        }

        std::vector<uint8_t> buf(commonHeaderSize(stats_seq_len) + 2 + tlv_size);

        CommonHeader hdr;
        hdr.version = kProtocolVersion;
        hdr.seq_len = stats_seq_len;
        hdr.msg_type = MsgType::STATS;
        hdr.seq_num = stats_seq;

        size_t offset = hdr.serialize(buf.data(), buf.size());
        size_t written = serializeTlvPayload(items, buf.data() + offset, buf.size() - offset);
        if (written > 0) {
            sendPacket(buf.data(), offset + written);
        }

        last_stats_time_us = now_us;
    }

    void checkFrameTimeouts(uint64_t now_us) {
        uint64_t timeout_us = static_cast<uint64_t>(config.frame_timeout_ms) * 1000;

        for (auto it = pending_frames.begin(); it != pending_frames.end(); ) {
            // 已标记丢弃的帧不再重复计时（由 tryDeliverInOrder 在游标经过时释放）
            if (!it->second.complete && !it->second.discarded &&
                (now_us - it->second.first_block_time_us) > timeout_us) {
                if (frame_dropped_cb) {
                    frame_dropped_cb(it->second.frame_type, it->second.frame_size);
                }
                stats.frames_dropped++;
                maybeSendPli(now_us, it->second.frame_type);
                it->second.discarded = true;
            } else {
                ++it;
            }
        }

        // 游标帧可能刚被标记丢弃：尝试推进游标（其后若有已收全帧则立即提交）
        tryDeliverInOrder();
    }

    void updateStats() {
        // Window-based rates reflect current network conditions
        uint64_t window_total = window_blocks_received + window_blocks_lost;
        if (window_total > 0) {
            stats.loss_rate = static_cast<float>(window_blocks_lost) /
                             static_cast<float>(window_total);
        } else {
            stats.loss_rate = 0.0f;
        }
        if (window_blocks_lost > 0) {
            stats.fec_recovery_rate = static_cast<float>(window_blocks_recovered_fec) /
                                     static_cast<float>(window_blocks_lost);
            stats.nack_recovery_rate = static_cast<float>(window_blocks_recovered_nack) /
                                      static_cast<float>(window_blocks_lost);
        } else {
            stats.fec_recovery_rate = 0.0f;
            stats.nack_recovery_rate = 0.0f;
        }
        stats.stats_window_packets = window_packets;
    }

    void resetWindow() {
        window_blocks_received = 0;
        window_blocks_lost = 0;
        window_blocks_recovered_fec = 0;
        window_blocks_recovered_nack = 0;
        window_packets = 0;
        goodput_window_bytes = 0;
    }
};

FrameReceiver::FrameReceiver(const ReceiverConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

FrameReceiver::~FrameReceiver() = default;

void FrameReceiver::setFrameReadyCallback(FrameReadyCallback cb) {
    impl_->frame_ready_cb = std::move(cb);
}

void FrameReceiver::setSendCallback(SendPacketCallback cb) {
    impl_->send_cb = std::move(cb);
}

void FrameReceiver::setFrameDroppedCallback(FrameDroppedCallback cb) {
    impl_->frame_dropped_cb = std::move(cb);
}

void FrameReceiver::onPacketReceived(const uint8_t* data, size_t len, uint64_t now_us) {
    CommonHeader hdr;
    size_t consumed = 0;
    if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;

    const uint8_t* payload = data + consumed;
    size_t payload_len = len - consumed;

    // Fall back to the last time provided by tick()
    if (now_us == 0) now_us = impl_->last_stats_time_us;

    switch (hdr.msg_type) {
        case MsgType::DATA:
            impl_->handleData(hdr, payload, payload_len, now_us);
            break;
        case MsgType::PING:
            impl_->handlePing(hdr, payload, payload_len);
            break;
        default:
            break;
    }
}

void FrameReceiver::tick(uint64_t now_us) {
    // Check frame timeouts
    impl_->checkFrameTimeouts(now_us);

    // Generate NACKs
    impl_->generateNacks(now_us);

    // Bound the FEC group-layout table
    impl_->pruneGroupLayouts();

    // Send STATS periodically
    uint64_t stats_interval_us = static_cast<uint64_t>(impl_->config.stats_interval_ms) * 1000;
    if (impl_->next_stats_send_us == 0 || now_us >= impl_->next_stats_send_us) {
        impl_->updateStats();

        uint64_t elapsed_us = (impl_->last_stats_send_us > 0 && now_us > impl_->last_stats_send_us)
                            ? (now_us - impl_->last_stats_send_us) : stats_interval_us;
        impl_->stats.goodput_bps = static_cast<uint32_t>(
            impl_->goodput_window_bytes * 8 * 1000000ULL / elapsed_us);
        impl_->last_stats_send_us = now_us;

        impl_->sendStats(now_us);
        impl_->resetWindow();
        impl_->next_stats_send_us = now_us + stats_interval_us;
    }

    impl_->last_stats_time_us = now_us;
}

const ProtocolStats& FrameReceiver::getStats() const {
    return impl_->stats;
}

SeqNum FrameReceiver::completedSeqNum() const {
    return impl_->completed_seq;
}

void FrameReceiver::requestKeyFrame(uint64_t now_us) {
    impl_->sendPliPacket();
    // 手动请求也重置节流窗口，避免紧随其后的丢帧再发一次
    if (now_us != 0) impl_->last_pli_sent_us = now_us;
}

} // namespace fec_protocol

