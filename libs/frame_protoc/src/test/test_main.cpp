#include <iostream>
#include <cstring>
#include <cassert>
#include <cmath>
#include <vector>
#include <random>
#include <algorithm>
#include <functional>

#include "sender.h"
#include "receiver.h"
#include "protocol.h"
#include "proxy_receiver.h"
#include "bitrate_controller.h"

using namespace fec_protocol;

// Simple test harness
static int tests_run = 0;
static int tests_passed = 0;

// Debug counters for out-of-order completion test
static int dbg_nacks = 0;
static int dbg_sender_nack_calls = 0;

#define TEST(name) \
    do { \
        tests_run++; \
        std::cout << "TEST: " << name << " ... "; \
    } while(0)

#define PASS() \
    do { \
        tests_passed++; \
        std::cout << "PASSED" << std::endl; \
    } while(0)

#define FAIL(msg) \
    do { \
        std::cout << "FAILED: " << msg << std::endl; \
    } while(0)

// Test 1: Basic loopback - send a frame, receive it without any loss
void test_basic_loopback() {
    TEST("Basic loopback (no loss)");

    SenderConfig sender_cfg;
    sender_cfg.mtu = 1400;
    sender_cfg.fec_ratio = 0.25f;

    ReceiverConfig receiver_cfg;
    receiver_cfg.frame_timeout_ms = 3000;

    FrameSender sender(sender_cfg);
    FrameReceiver receiver(receiver_cfg);

    // Wire them together: sender output -> receiver input
    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        receiver.onPacketReceived(data, len);
    });

    // Receiver output -> check result
    Frame received_frame;
    bool frame_received = false;

    receiver.setFrameReadyCallback([&](Frame&& frame) {
        received_frame = std::move(frame);
        frame_received = true;
    });

    // Receiver control packets -> sender
    receiver.setSendCallback([&](const uint8_t* data, size_t len) {
        sender.onPacketReceived(data, len);
    });

    // Create test frame
    std::vector<uint8_t> test_data(3000);
    for (size_t i = 0; i < test_data.size(); ++i) {
        test_data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    // Send frame
    bool ok = sender.sendFrame(test_data.data(), test_data.size(), frame_type::VIDEO_IDR);
    if (!ok) { FAIL("sendFrame returned false"); return; }

    // In this direct-loopback scenario, frame should be immediately assembled
    if (!frame_received) { FAIL("Frame not received"); return; }
    if (received_frame.size != test_data.size()) {
        FAIL("Frame size mismatch: expected " + std::to_string(test_data.size()) +
             " got " + std::to_string(received_frame.size));
        return;
    }
    if (received_frame.type != frame_type::VIDEO_IDR) {
        FAIL("Frame type mismatch");
        return;
    }
    if (std::memcmp(received_frame.data.data(), test_data.data(), test_data.size()) != 0) {
        FAIL("Frame data mismatch");
        return;
    }

    PASS();
}

// Test 2: Small frame (single packet)
void test_single_packet_frame() {
    TEST("Single packet frame");

    SenderConfig sender_cfg;
    sender_cfg.mtu = 1400;
    sender_cfg.fec_ratio = 0.5f;

    ReceiverConfig receiver_cfg;

    FrameSender sender(sender_cfg);
    FrameReceiver receiver(receiver_cfg);

    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        receiver.onPacketReceived(data, len);
    });

    Frame received_frame;
    bool frame_received = false;
    receiver.setFrameReadyCallback([&](Frame&& frame) {
        received_frame = std::move(frame);
        frame_received = true;
    });
    receiver.setSendCallback([](const uint8_t*, size_t) {});

    // Small frame that fits in one data block
    std::vector<uint8_t> test_data(100);
    for (size_t i = 0; i < test_data.size(); ++i) {
        test_data[i] = static_cast<uint8_t>(i * 3);
    }

    bool ok = sender.sendFrame(test_data.data(), test_data.size(), frame_type::AUDIO);
    if (!ok) { FAIL("sendFrame returned false"); return; }
    if (!frame_received) { FAIL("Frame not received"); return; }
    if (received_frame.size != test_data.size()) { FAIL("Size mismatch"); return; }
    if (std::memcmp(received_frame.data.data(), test_data.data(), test_data.size()) != 0) {
        FAIL("Data mismatch");
        return;
    }

    PASS();
}

// Test 3: FEC recovery - simulate packet loss
void test_fec_recovery() {
    TEST("FEC recovery (simulated loss)");

    SenderConfig sender_cfg;
    sender_cfg.mtu = 1400;
    sender_cfg.fec_ratio = 0.5f;  // 50% redundancy for better recovery

    ReceiverConfig receiver_cfg;

    FrameSender sender(sender_cfg);
    FrameReceiver receiver(receiver_cfg);

    // Drop some packets
    int packet_count = 0;
    int dropped_count = 0;
    std::mt19937 rng(42); // Fixed seed for reproducibility

    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        packet_count++;
        // Drop every 4th packet (25% loss, within FEC recovery capability of 50%)
        if (packet_count % 4 == 0) {
            dropped_count++;
            return; // Drop this packet
        }
        receiver.onPacketReceived(data, len);
    });

    Frame received_frame;
    bool frame_received = false;
    receiver.setFrameReadyCallback([&](Frame&& frame) {
        received_frame = std::move(frame);
        frame_received = true;
    });
    receiver.setSendCallback([](const uint8_t*, size_t) {});

    // Create test frame large enough to span multiple blocks
    std::vector<uint8_t> test_data(5000);
    for (size_t i = 0; i < test_data.size(); ++i) {
        test_data[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
    }

    bool ok = sender.sendFrame(test_data.data(), test_data.size(), frame_type::VIDEO);
    if (!ok) { FAIL("sendFrame returned false"); return; }

    std::cout << "(sent=" << packet_count << " dropped=" << dropped_count << ") ";

    if (!frame_received) { FAIL("Frame not recovered via FEC"); return; }
    if (received_frame.size != test_data.size()) {
        FAIL("Size mismatch: expected " + std::to_string(test_data.size()) +
             " got " + std::to_string(received_frame.size));
        return;
    }
    if (std::memcmp(received_frame.data.data(), test_data.data(), test_data.size()) != 0) {
        FAIL("Data mismatch after FEC recovery");
        return;
    }

    PASS();
}

// Test 4: Large frame spanning multiple FEC groups
void test_large_frame_multiple_groups() {
    TEST("Large frame (multiple FEC groups)");

    SenderConfig sender_cfg;
    sender_cfg.mtu = 200;           // Small MTU to force many blocks
    sender_cfg.fec_ratio = 0.5f;    // max_group_size = floor(255*0.5) = 127

    ReceiverConfig receiver_cfg;

    FrameSender sender(sender_cfg);
    FrameReceiver receiver(receiver_cfg);

    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        receiver.onPacketReceived(data, len);
    });

    Frame received_frame;
    bool frame_received = false;
    receiver.setFrameReadyCallback([&](Frame&& frame) {
        received_frame = std::move(frame);
        frame_received = true;
    });
    receiver.setSendCallback([](const uint8_t*, size_t) {});

    // 50KB frame with small MTU should create multiple groups
    // MTU=200, ~180 payload per block, 50000/180 ≈ 278 data blocks
    // max_group_size = floor(255*0.5) = 127, so splits into 3 groups
    std::vector<uint8_t> test_data(50000);
    for (size_t i = 0; i < test_data.size(); ++i) {
        test_data[i] = static_cast<uint8_t>((i * 11 + 37) & 0xFF);
    }

    bool ok = sender.sendFrame(test_data.data(), test_data.size(), frame_type::VIDEO_IDR);
    if (!ok) { FAIL("sendFrame returned false"); return; }
    if (!frame_received) { FAIL("Frame not received"); return; }
    if (received_frame.size != test_data.size()) { FAIL("Size mismatch"); return; }
    if (std::memcmp(received_frame.data.data(), test_data.data(), test_data.size()) != 0) {
        FAIL("Data mismatch");
        return;
    }

    PASS();
}

// Test 5: PING/PONG
void test_ping_pong() {
    TEST("PING/PONG RTT");

    SenderConfig sender_cfg;
    sender_cfg.ping_interval_ms = 100;

    ReceiverConfig receiver_cfg;

    FrameSender sender(sender_cfg);
    FrameReceiver receiver(receiver_cfg);

    // Bidirectional connection
    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        receiver.onPacketReceived(data, len);
    });
    receiver.setSendCallback([&](const uint8_t* data, size_t len) {
        sender.onPacketReceived(data, len);
    });
    receiver.setFrameReadyCallback([](Frame&&) {});

    // Simulate time progression
    uint64_t time_us = 1000000; // Start at 1 second
    sender.tick(time_us);

    // Advance time to trigger PING
    time_us += 200000; // +200ms
    sender.tick(time_us);

    // The receiver should have sent PONG, sender should have RTT
    // Note: In this synchronous test, PONG is sent immediately,
    // so RTT should be ~0 (or very small due to test setup)

    const ProtocolStats& stats = sender.getStats();
    // RTT might be 0 in synchronous loopback, which is acceptable
    std::cout << "(RTT=" << stats.rtt_ms << "ms) ";

    PASS();
}

// Test 6b: PING cadence with high-frequency ticks (regression)
// 回归测试：tick 每 100ms 调用一次（与 Publisher 每帧 tick 一致）时，
// PING 必须仍按 ping_interval_ms 周期发送。旧实现每次 tick 无条件刷新
// last_ping_time_us，导致间隔条件永不满足，PING 只在首 tick 发一次
void test_ping_interval_frequent_ticks() {
    TEST("PING cadence with frequent ticks");

    SenderConfig sender_cfg;
    sender_cfg.ping_interval_ms = 1000;

    FrameSender sender(sender_cfg);
    int ping_count = 0;
    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        CommonHeader hdr;
        size_t consumed = 0;
        if (CommonHeader::deserialize(data, len, hdr, consumed) &&
            hdr.msg_type == MsgType::PING) {
            ping_count++;
        }
    });

    // 起点非 0，模拟进程已运行一段时间后的稳态
    uint64_t now_us = 1'000'000;
    for (int i = 0; i < 35; ++i) {  // 3.5s，每 100ms 一次 tick
        sender.tick(now_us);
        now_us += 100'000;
    }

    // 期望：首 tick 1 次 + t=1s/2s/3s 各 1 次 = 4 次
    std::cout << "(ping_count=" << ping_count << ") ";
    if (ping_count != 4) {
        FAIL("Expected 4 PINGs over 3.5s of 100ms ticks, got " + std::to_string(ping_count));
        return;
    }
    PASS();
}

// Test 6: Protocol serialization roundtrip
void test_protocol_serialization() {
    TEST("Protocol header serialization");

    // Test CommonHeader
    CommonHeader hdr;
    hdr.version = 1;
    hdr.seq_len = 4;
    hdr.msg_type = MsgType::DATA;
    hdr.seq_num = 12345678;

    uint8_t buf[64];
    size_t written = hdr.serialize(buf, sizeof(buf));
    if (written != 6) { FAIL("CommonHeader size wrong"); return; }

    CommonHeader hdr2;
    size_t consumed;
    bool ok = CommonHeader::deserialize(buf, written, hdr2, consumed);
    if (!ok) { FAIL("CommonHeader deserialize failed"); return; }
    if (hdr2.version != 1 || hdr2.seq_len != 4 || hdr2.msg_type != MsgType::DATA ||
        hdr2.seq_num != 12345678) {
        FAIL("CommonHeader roundtrip mismatch");
        return;
    }

    // Test DataHeader
    DataHeader dhdr;
    dhdr.data_block_count = 5;
    dhdr.fec_block_count = 2;
    dhdr.block_index = 3;
    dhdr.fec_group_index = 1;
    dhdr.fec_group_count = 2;
    dhdr.frame_type = frame_type::VIDEO_IDR;
    dhdr.frame_size = 65535;
    dhdr.retry_count = 7;
    dhdr.payload_length = 1200;

    written = dhdr.serialize(buf, sizeof(buf));
    if (written != DataHeader::serializedSize()) { FAIL("DataHeader size wrong"); return; }

    DataHeader dhdr2;
    ok = DataHeader::deserialize(buf, written, dhdr2);
    if (!ok) { FAIL("DataHeader deserialize failed"); return; }
    if (dhdr2.data_block_count != 5 || dhdr2.fec_block_count != 2 ||
        dhdr2.block_index != 3 || dhdr2.fec_group_index != 1 ||
        dhdr2.fec_group_count != 2 || dhdr2.frame_type != frame_type::VIDEO_IDR ||
        dhdr2.frame_size != 65535 || dhdr2.retry_count != 7 ||
        dhdr2.payload_length != 1200) {
        FAIL("DataHeader roundtrip mismatch");
        return;
    }

    PASS();
}

// Test 7: Stats reporting
void test_stats() {
    TEST("Stats reporting");

    SenderConfig sender_cfg;
    sender_cfg.fec_ratio = 0.0f; // No FEC for simplicity

    ReceiverConfig receiver_cfg;
    receiver_cfg.stats_interval_ms = 100;

    FrameSender sender(sender_cfg);
    FrameReceiver receiver(receiver_cfg);

    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        receiver.onPacketReceived(data, len);
    });
    receiver.setSendCallback([&](const uint8_t* data, size_t len) {
        sender.onPacketReceived(data, len);
    });
    receiver.setFrameReadyCallback([](Frame&&) {});

    // Send some frames
    std::vector<uint8_t> test_data(500, 0xAB);
    sender.sendFrame(test_data.data(), test_data.size(), frame_type::AUDIO);
    sender.sendFrame(test_data.data(), test_data.size(), frame_type::AUDIO);

    const ProtocolStats& sender_stats = sender.getStats();
    if (sender_stats.frames_sent != 2) {
        FAIL("Expected 2 frames sent, got " + std::to_string(sender_stats.frames_sent));
        return;
    }

    const ProtocolStats& receiver_stats = receiver.getStats();
    if (receiver_stats.frames_completed != 2) {
        FAIL("Expected 2 frames completed, got " + std::to_string(receiver_stats.frames_completed));
        return;
    }

    PASS();
}

// Test 7: NACK recovery - full feedback loop (loss → NACK → retransmit)
void test_nack_recovery() {
    TEST("NACK recovery (loss → NACK → retransmit)");

    SenderConfig sender_cfg;
    sender_cfg.mtu = 1400;
    sender_cfg.fec_ratio = 0.0f;   // 无 FEC，丢包只能靠 NACK 恢复

    ReceiverConfig receiver_cfg;
    receiver_cfg.nack_delay_ms = 20;
    receiver_cfg.nack_interval_ms = 50;
    receiver_cfg.stats_interval_ms = 100000;  // 避免 STATS 干扰

    FrameSender sender(sender_cfg);
    FrameReceiver receiver(receiver_cfg);

    uint64_t now_us = 1'000'000;  // 模拟时钟：1s

    // 首个 seq=1 的 DATA 包丢弃一次；重传包放行
    const SeqNum drop_seq = 1;
    bool dropped = false;
    int retransmits_delivered = 0;
    int nack_packets = 0;

    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        CommonHeader hdr;
        size_t consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;
        if (hdr.msg_type == MsgType::DATA) {
            if (hdr.seq_num == drop_seq && !dropped) {
                dropped = true;
                return;  // 首次传输丢弃
            }
            if (hdr.seq_num == drop_seq && dropped) {
                retransmits_delivered++;
            }
            receiver.onPacketReceived(data, len, now_us);
        }
    });

    receiver.setSendCallback([&](const uint8_t* data, size_t len) {
        CommonHeader hdr;
        size_t consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;
        if (hdr.msg_type == MsgType::NACK) nack_packets++;
        // 控制包回传给发送端（NACK → 触发重传）
        sender.onPacketReceived(data, len, now_us);
    });

    Frame received_frame;
    bool frame_received = false;
    receiver.setFrameReadyCallback([&](Frame&& frame) {
        received_frame = std::move(frame);
        frame_received = true;
    });

    // 5KB 帧 + MTU 1400 → 4 个数据块（seq 0..3），丢弃中间的 seq=1
    std::vector<uint8_t> test_data(5000);
    for (size_t i = 0; i < test_data.size(); ++i) {
        test_data[i] = static_cast<uint8_t>((i * 5 + 1) & 0xFF);
    }

    bool ok = sender.sendFrame(test_data.data(), test_data.size(),
                               frame_type::VIDEO_IDR, now_us);
    if (!ok) { FAIL("sendFrame returned false"); return; }
    if (!dropped) { FAIL("drop did not happen"); return; }
    if (frame_received) { FAIL("frame completed despite loss (should need NACK)"); return; }

    // 推进模拟时钟并 tick 接收端，直到 NACK 触发重传、帧补全
    for (int i = 0; i < 50 && !frame_received; ++i) {
        now_us += 10'000;  // +10ms
        receiver.tick(now_us);
    }

    if (!frame_received) {
        FAIL("frame not recovered via NACK (nack_packets=" + std::to_string(nack_packets) + ")");
        return;
    }
    if (nack_packets == 0) { FAIL("no NACK was sent"); return; }
    if (retransmits_delivered == 0) { FAIL("no retransmission delivered"); return; }
    if (received_frame.size != test_data.size()) { FAIL("size mismatch"); return; }
    if (std::memcmp(received_frame.data.data(), test_data.data(), test_data.size()) != 0) {
        FAIL("data mismatch after NACK recovery");
        return;
    }

    std::cout << "(nacks=" << nack_packets
              << " retrans=" << retransmits_delivered << ") ";
    PASS();
}

// Test: BitrateController state machine
// Verifies: steady state, loss-triggered decrease + FEC raise, recovery increase,
// sustained-loss hard cut, and FEC relaxation after stable windows.
void test_bitrate_controller() {
    TEST("BitrateController state machine");

    BitrateControllerConfig cfg;
    cfg.initial_bitrate_bps = 3000000;
    cfg.min_bitrate_bps = 200000;
    cfg.max_bitrate_bps = 50000000;
    cfg.window_ms = 300;
    cfg.loss_down_threshold = 0.02f;
    cfg.loss_up_threshold = 0.01f;
    cfg.increase_factor = 1.10f;
    cfg.decrease_factor = 0.80f;
    cfg.fec_initial = 0.25f;
    cfg.fec_max = 0.30f;
    cfg.cooldown_windows = 3;
    cfg.max_loss_rounds = 3;
    cfg.hold_ms = 1000;
    cfg.fec_relax_windows = 10;
    cfg.ewma_alpha = 0.30f;
    cfg.advice_change_min = 0.05f;

    BitrateController ctrl(cfg);
    uint64_t now_us = 0;
    const uint64_t window_us = static_cast<uint64_t>(cfg.window_ms) * 1000ULL;

    // --- Phase 1: 连续 2 个丢包窗口触发降码率 + FEC 跟随丢包率 ---
    // （单窗口丢包尖峰只确认不动作，防止订阅切换等误报导致码率崩塌）
    now_us += window_us;
    ctrl.onFeedback(now_us, 2200000, 0.05f);  // 第 1 个丢包窗口：仅确认
    if (ctrl.targetBitrate() != cfg.initial_bitrate_bps) {
        FAIL("single loss window should not act yet (got " +
             std::to_string(ctrl.targetBitrate()) + ")");
        return;
    }
    now_us += window_us;
    ctrl.onFeedback(now_us, 2200000, 0.05f);  // 第 2 个丢包窗口：确认生效
    uint32_t after_loss = ctrl.targetBitrate();
    float fec_after_loss = ctrl.fecRatio();
    if (after_loss >= cfg.initial_bitrate_bps) {
        FAIL("target did not drop on loss (got " + std::to_string(after_loss) + ")");
        return;
    }
    // FEC 按公式跟随丢包率：0.05 × 1.5 = 0.075（初值 0.25 与目标差 > 死区，已更新）
    if (std::fabs(fec_after_loss - 0.075f) > 1e-3f) {
        FAIL("FEC not following loss formula (got " +
             std::to_string(fec_after_loss) + ")");
        return;
    }

    // --- Phase 2: sustained loss rounds lead to hard cut ---
    uint32_t before_hard_cut = ctrl.targetBitrate();
    for (int i = 0; i < static_cast<int>(cfg.max_loss_rounds) + 2; ++i) {
        now_us += window_us;
        ctrl.onFeedback(now_us, 2000000, 0.06f);
    }
    uint32_t after_sustained = ctrl.targetBitrate();
    if (after_sustained > before_hard_cut) {
        FAIL("sustained loss did not reduce rate");
        return;
    }
    // FEC capped at max
    if (ctrl.fecRatio() > cfg.fec_max + 1e-6f) {
        FAIL("FEC exceeded max");
        return;
    }

    // --- Phase 3: recovery — cooldown then increase ---
    uint32_t before_recover = ctrl.targetBitrate();
    // Feed many clean windows; rate should eventually rise above pre-recovery.
    bool rose = false;
    for (int i = 0; i < 40; ++i) {
        now_us += window_us;
        ctrl.onFeedback(now_us, before_recover, 0.0f);
        if (ctrl.targetBitrate() > before_recover) { rose = true; break; }
    }
    if (!rose) {
        FAIL("rate did not recover after clean windows");
        return;
    }

    // --- Phase 4: FEC 冗余随丢包率回落 ---
    // 干净窗口下 fec = clamp(0 × 1.5, min, max) = fec_min（Phase 3 已回落）
    for (int i = 0; i < static_cast<int>(cfg.fec_relax_windows) * 3; ++i) {
        now_us += window_us;
        ctrl.onFeedback(now_us, ctrl.targetBitrate(), 0.0f);
    }
    if (std::fabs(ctrl.fecRatio() - cfg.fec_min) > 1e-3f) {
        FAIL("FEC did not relax to min on clean windows (got " +
             std::to_string(ctrl.fecRatio()) + ")");
        return;
    }

    // --- Phase 5: rate stays within bounds ---
    if (ctrl.targetBitrate() < cfg.min_bitrate_bps ||
        ctrl.targetBitrate() > cfg.max_bitrate_bps) {
        FAIL("rate out of bounds");
        return;
    }

    std::cout << "(final=" << ctrl.targetBitrate()
              << " fec=" << ctrl.fecRatio() << ") ";
    PASS();
}

// Test: PLI sent when a video frame is dropped (timeout)
void test_pli_on_frame_drop() {
    TEST("PLI on frame drop");

    SenderConfig sender_cfg;
    sender_cfg.mtu = 1400;
    sender_cfg.fec_ratio = 0.0f;   // 无 FEC：丢块无法恢复，帧必然超时

    ReceiverConfig receiver_cfg;
    receiver_cfg.frame_timeout_ms = 100;
    receiver_cfg.stats_interval_ms = 1000000;  // 避免 STATS 干扰

    FrameSender sender(sender_cfg);
    FrameReceiver receiver(receiver_cfg);

    uint64_t now_us = 1'000'000;
    int pli_count = 0;

    // 通道：seq==1 永久丢弃（含重传）
    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        CommonHeader hdr;
        size_t consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;
        if (hdr.msg_type == MsgType::DATA && hdr.seq_num == 1) return;
        receiver.onPacketReceived(data, len, now_us);
    });

    receiver.setSendCallback([&](const uint8_t* data, size_t len) {
        CommonHeader hdr;
        size_t consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;
        if (hdr.msg_type == MsgType::PLI) pli_count++;
        // NACK 回传给 sender 触发重传（同样会被通道丢弃）
        sender.onPacketReceived(data, len, now_us);
    });
    receiver.setFrameReadyCallback([](Frame&&) {});

    std::vector<uint8_t> test_data(5000, 0x5A);
    bool ok = sender.sendFrame(test_data.data(), test_data.size(),
                               frame_type::VIDEO_IDR, now_us);
    if (!ok) { FAIL("sendFrame returned false"); return; }

    // 推进时钟直到帧超时（100ms）
    for (int i = 0; i < 30 && pli_count == 0; ++i) {
        now_us += 10'000;  // +10ms
        receiver.tick(now_us);
    }

    if (receiver.getStats().frames_dropped != 1) {
        FAIL("expected 1 dropped frame, got " +
             std::to_string(receiver.getStats().frames_dropped));
        return;
    }
    if (pli_count != 1) {
        FAIL("expected exactly 1 PLI, got " + std::to_string(pli_count));
        return;
    }

    PASS();
}

// Test: client-side PLI throttle (pli_min_interval_ms)
void test_pli_client_throttle() {
    TEST("PLI client throttle");

    SenderConfig sender_cfg;
    sender_cfg.mtu = 1400;
    sender_cfg.fec_ratio = 0.0f;

    ReceiverConfig receiver_cfg;
    receiver_cfg.frame_timeout_ms = 100;
    receiver_cfg.pli_min_interval_ms = 500;
    receiver_cfg.stats_interval_ms = 1000000;

    FrameSender sender(sender_cfg);
    FrameReceiver receiver(receiver_cfg);

    uint64_t now_us = 1'000'000;
    int pli_count = 0;

    // 每帧的首个数据块永久丢弃：帧 A(seq1..) B(seq5..) C(seq9..)
    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        CommonHeader hdr;
        size_t consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;
        if (hdr.msg_type == MsgType::DATA &&
            (hdr.seq_num == 1 || hdr.seq_num == 5 || hdr.seq_num == 9)) return;
        receiver.onPacketReceived(data, len, now_us);
    });
    receiver.setSendCallback([&](const uint8_t* data, size_t len) {
        CommonHeader hdr;
        size_t consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;
        if (hdr.msg_type == MsgType::PLI) pli_count++;
    });
    receiver.setFrameReadyCallback([](Frame&&) {});

    std::vector<uint8_t> test_data(5000, 0x33);

    auto runUntilTimeout = [&]() {
        for (int i = 0; i < 30; ++i) {
            now_us += 10'000;
            receiver.tick(now_us);
        }
    };

    // 帧 A：超时 → PLI #1
    sender.sendFrame(test_data.data(), test_data.size(), frame_type::VIDEO, now_us);
    runUntilTimeout();
    if (pli_count != 1) { FAIL("expected PLI #1 after first drop, got " +
        std::to_string(pli_count)); return; }

    // 帧 B：紧接着超时（< 500ms）→ 被节流
    sender.sendFrame(test_data.data(), test_data.size(), frame_type::VIDEO, now_us);
    runUntilTimeout();
    if (pli_count != 1) { FAIL("PLI not throttled within 500ms window, got " +
        std::to_string(pli_count)); return; }

    // 推进超过节流窗口后，帧 C 超时 → PLI #2
    now_us += 400'000;  // +400ms（累计已远超 500ms）
    receiver.tick(now_us);
    sender.sendFrame(test_data.data(), test_data.size(), frame_type::VIDEO, now_us);
    runUntilTimeout();
    if (pli_count != 2) { FAIL("expected PLI #2 after throttle window, got " +
        std::to_string(pli_count)); return; }

    PASS();
}

// Test: FrameSender invokes KeyFrameRequestCallback on PLI
void test_sender_pli_callback() {
    TEST("Sender PLI callback");

    SenderConfig sender_cfg;
    FrameSender sender(sender_cfg);

    int callback_count = 0;
    sender.setKeyFrameRequestCallback([&]() { callback_count++; });

    // 用 FrameReceiver 生成一个真实的 PLI 包
    ReceiverConfig receiver_cfg;
    FrameReceiver pli_gen(receiver_cfg);
    std::vector<uint8_t> pli_packet;
    pli_gen.setSendCallback([&](const uint8_t* data, size_t len) {
        pli_packet.assign(data, data + len);
    });
    pli_gen.requestKeyFrame();

    if (pli_packet.size() < 4 || pli_packet[1] != static_cast<uint8_t>(MsgType::PLI)) {
        FAIL("generated packet is not a PLI");
        return;
    }

    sender.onPacketReceived(pli_packet.data(), pli_packet.size());
    if (callback_count != 1) {
        FAIL("expected callback once, got " + std::to_string(callback_count));
        return;
    }

    PASS();
}

// Test: ProxyFrameReceiver forwards PLI upstream byte-for-byte
void test_proxy_pli_forward() {
    TEST("Proxy PLI forward");

    ProxyConfig proxy_cfg;
    ProxyFrameReceiver proxy(proxy_cfg);

    std::vector<uint8_t> upstream_packet;
    proxy.setUpstreamSendCallback([&](const uint8_t* data, size_t len) {
        upstream_packet.assign(data, data + len);
    });

    // 生成真实 PLI 包
    ReceiverConfig receiver_cfg;
    FrameReceiver pli_gen(receiver_cfg);
    std::vector<uint8_t> pli_packet;
    pli_gen.setSendCallback([&](const uint8_t* data, size_t len) {
        pli_packet.assign(data, data + len);
    });
    pli_gen.requestKeyFrame();
    if (pli_packet.empty()) { FAIL("no PLI generated"); return; }

    proxy.onDownstreamPacket(pli_packet.data(), pli_packet.size());

    if (upstream_packet != pli_packet) {
        FAIL("upstream packet differs from downstream PLI");
        return;
    }
    if (proxy.getStats().pli_forwarded_upstream != 1) {
        FAIL("pli_forwarded_upstream != 1");
        return;
    }

    PASS();
}

// Regression: ProxyFrameReceiver 必须亲自回 PONG。曾因把 PING 原样转发下游、
// 从不回 PONG，导致服务端踢会话后推流端盲发 3.5 小时（343 万条 Unknown session）。
// 同时验证 PING 不再转发下游（避免下游误回 PONG 的转发链）。
void test_proxy_ping_pong() {
    TEST("Proxy PING -> PONG (liveness)");

    ProxyConfig proxy_cfg;
    ProxyFrameReceiver proxy(proxy_cfg);

    std::vector<uint8_t> upstream_packet;    // PONG 回推流端
    std::vector<uint8_t> downstream_packet;  // 应为空：PING 不转发下游
    proxy.setUpstreamSendCallback([&](const uint8_t* data, size_t len) {
        upstream_packet.assign(data, data + len);
    });
    proxy.setSendCallback([&](const uint8_t* data, size_t len) {
        downstream_packet.assign(data, data + len);
    });

    // 用真实 FrameSender 生成 PING 包
    SenderConfig sender_cfg;
    sender_cfg.ping_interval_ms = 100;
    FrameSender sender(sender_cfg);
    std::vector<uint8_t> ping_packet;
    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        ping_packet.assign(data, data + len);
    });
    sender.tick(1'000'000);
    if (ping_packet.empty()) { FAIL("no PING generated"); return; }

    CommonHeader hdr;
    size_t consumed = 0;
    if (!CommonHeader::deserialize(ping_packet.data(), ping_packet.size(), hdr, consumed)) {
        FAIL("PING parse failed"); return;
    }
    if (hdr.msg_type != MsgType::PING) { FAIL("expected PING"); return; }

    proxy.onUpstreamPacket(ping_packet.data(), ping_packet.size(), 1'000'000);

    if (upstream_packet.empty()) { FAIL("no PONG sent upstream"); return; }
    if (proxy.getStats().pongs_sent != 1) { FAIL("pongs_sent != 1"); return; }

    CommonHeader pong_hdr;
    size_t pong_consumed = 0;
    if (!CommonHeader::deserialize(upstream_packet.data(), upstream_packet.size(),
                                   pong_hdr, pong_consumed)) {
        FAIL("PONG parse failed"); return;
    }
    if (pong_hdr.msg_type != MsgType::PONG) { FAIL("expected PONG"); return; }

    // PONG 必须回显 PING 携带的时间戳（发送端据此计算 RTT）
    std::vector<TlvItem> items;
    if (!deserializeTlvPayload(upstream_packet.data() + pong_consumed,
                               upstream_packet.size() - pong_consumed, items)) {
        FAIL("PONG TLV parse failed"); return;
    }
    bool echo_ok = false;
    for (const auto& item : items) {
        if (item.type == TlvType::ECHO_TIMESTAMP && item.value.size() == 8) {
            if (readBE64(item.value.data()) == 1'000'000) echo_ok = true;
        }
    }
    if (!echo_ok) { FAIL("PONG echo timestamp mismatch"); return; }

    if (!downstream_packet.empty()) { FAIL("PING forwarded downstream"); return; }

    PASS();
}

// Regression 1: 旧帧因丢包未完成、新帧先完成（乱序完成）后，
// 旧帧的重传块不能被当作 stale 丢弃，否则旧帧永远无法补齐
void test_out_of_order_completion() {
    TEST("Out-of-order completion (late retransmit of older frame)");

    SenderConfig sender_cfg;
    sender_cfg.fec_ratio = 0.0f;
    ReceiverConfig receiver_cfg;
    receiver_cfg.nack_delay_ms = 20;
    receiver_cfg.nack_interval_ms = 50;
    receiver_cfg.stats_interval_ms = 100000;

    FrameSender sender(sender_cfg);
    FrameReceiver receiver(receiver_cfg);
    uint64_t now_us = 1'000'000;

    const SeqNum drop_seq = 2;  // 第一帧的中间块（seq 1..3 中的 seq=2）首次丢弃，重传时暂存
    bool dropped = false;
    std::vector<uint8_t> held_retransmit;

    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        CommonHeader hdr; size_t consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;
        if (hdr.msg_type != MsgType::DATA) return;
        if (hdr.seq_num == drop_seq && !dropped) { dropped = true; return; }
        if (hdr.seq_num == drop_seq && dropped) {
            held_retransmit.assign(data, data + len);
            return;
        }
        receiver.onPacketReceived(data, len, now_us);
    });
    receiver.setSendCallback([&](const uint8_t* data, size_t len) {
        CommonHeader hdr; size_t consumed = 0;
        if (CommonHeader::deserialize(data, len, hdr, consumed) &&
            hdr.msg_type == MsgType::NACK) dbg_nacks++;
        dbg_sender_nack_calls++;
        sender.onPacketReceived(data, len, now_us);
    });

    std::vector<Frame> frames;
    receiver.setFrameReadyCallback([&](Frame&& f) { frames.push_back(std::move(f)); });

    std::vector<uint8_t> f1(3000), f2(3000);
    for (size_t i = 0; i < f1.size(); ++i) { f1[i] = i & 0xFF; f2[i] = (i + 7) & 0xFF; }

    sender.sendFrame(f1.data(), f1.size(), frame_type::VIDEO, now_us);
    sender.sendFrame(f2.data(), f2.size(), frame_type::VIDEO, now_us);

    if (!dropped) { FAIL("drop did not happen"); return; }
    // 按序提交：旧帧未完成时，先收全的新帧必须等待（不得先提交）
    if (frames.size() != 0) {
        FAIL("frame2 delivered before frame1 completed (out-of-order delivery, got " +
             std::to_string(frames.size()) + ")");
        return;
    }

    // tick 触发 NACK → 发送端重传（被暂存，模拟重传晚于第二帧完成才到达）
    for (int i = 0; i < 10 && held_retransmit.empty(); ++i) {
        now_us += 10'000;
        receiver.tick(now_us);
    }
    if (held_retransmit.empty()) {
        FAIL("no retransmit generated (nacks=" + std::to_string(dbg_nacks) +
             " recv_send_calls=" + std::to_string(dbg_sender_nack_calls) + ")");
        return;
    }

    receiver.onPacketReceived(held_retransmit.data(), held_retransmit.size(), now_us);

    if (frames.size() != 2) {
        FAIL("older frame not recovered after late retransmit (got " +
             std::to_string(frames.size()) + ")");
        return;
    }
    // 按序提交：先补全的旧帧先交付，新帧随后
    if (frames[0].data != f1) { FAIL("frame1 data mismatch (delivered out of order)"); return; }
    if (frames[1].data != f2) { FAIL("frame2 data mismatch"); return; }

    PASS();
}

// Regression 2: 订阅中途加入时，首个收到包之前的缺失块必须被 NACK，
// 而不是被 expected 游标初始化静默跳过
void test_mid_join_gap_nack() {
    TEST("Mid-join gap NACK (blocks before first received seq)");

    SenderConfig sender_cfg;
    sender_cfg.fec_ratio = 0.0f;
    ReceiverConfig receiver_cfg;
    receiver_cfg.nack_delay_ms = 0;  // 立即触发
    receiver_cfg.stats_interval_ms = 100000;

    FrameSender sender(sender_cfg);
    FrameReceiver receiver(receiver_cfg);
    uint64_t now_us = 1'000'000;

    std::vector<std::vector<uint8_t>> packets;
    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        packets.emplace_back(data, data + len);
    });

    std::vector<SeqNum> nacked;
    receiver.setSendCallback([&](const uint8_t* data, size_t len) {
        CommonHeader hdr; size_t consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;
        if (hdr.msg_type != MsgType::NACK) return;
        std::vector<NackEntry> entries;
        if (deserializeNackPayload(data + consumed, len - consumed, entries)) {
            for (const auto& e : entries) {
                nacked.push_back(e.lost_start_seq);
                for (int b = 0; b < 32; ++b)
                    if (e.bitmask & (1u << b)) nacked.push_back(e.lost_start_seq + 1 + b);
            }
        }
    });

    std::vector<uint8_t> f1(5000);  // MTU 1400 → 4 个数据块 seq 1..4
    sender.sendFrame(f1.data(), f1.size(), frame_type::VIDEO, now_us);

    // 模拟中途加入：只投递 seq >= 3（block_index >= 2）的包
    for (const auto& p : packets) {
        CommonHeader hdr; size_t consumed = 0;
        if (!CommonHeader::deserialize(p.data(), p.size(), hdr, consumed)) continue;
        if (hdr.seq_num >= 3) receiver.onPacketReceived(p.data(), p.size(), now_us);
    }

    now_us += 30'000;
    receiver.tick(now_us);

    bool has1 = std::find(nacked.begin(), nacked.end(), SeqNum(1)) != nacked.end();
    bool has2 = std::find(nacked.begin(), nacked.end(), SeqNum(2)) != nacked.end();
    if (!has1 || !has2) {
        FAIL("missing blocks before join point not NACKed (nacked=" +
             std::to_string(nacked.size()) + ")");
        return;
    }
    PASS();
}

// Test: 随机丢包下多帧全链路恢复（FEC + NACK 联合工作）
// 模拟真实网络：固定种子随机丢 ~8% 数据包（含偶发连续突发丢 3 包），
// 控制包通道无损，双向反馈 + 虚拟时钟驱动，验证所有帧按序完整恢复、
// 统计计数（丢块/FEC 恢复/NACK 恢复）一致
void test_random_loss_full_loop() {
    TEST("Random loss: multi-frame FEC+NACK full loop");

    SenderConfig sender_cfg;
    sender_cfg.mtu = 1400;
    sender_cfg.fec_ratio = 0.35f;
    sender_cfg.ping_interval_ms = 1000;

    ReceiverConfig receiver_cfg;
    receiver_cfg.frame_timeout_ms = 3000;
    receiver_cfg.nack_delay_ms = 10;
    receiver_cfg.nack_interval_ms = 50;
    receiver_cfg.max_nack_retries = 6;
    receiver_cfg.stats_interval_ms = 500;

    FrameSender sender(sender_cfg);
    FrameReceiver receiver(receiver_cfg);

    uint64_t now_us = 1'000'000;

    // ---- 丢包信道（数据方向）：固定种子，可复现 ----
    std::mt19937 rng(20260808u);
    int data_packets = 0, dropped_packets = 0;
    int burst_left = 0;
    auto lossyChannel = [&](const uint8_t* data, size_t len) {
        data_packets++;
        bool drop;
        if (burst_left > 0) {
            burst_left--;
            drop = true;
        } else {
            drop = (rng() % 1000) < 80;           // 8% 随机丢包
            if (drop && (rng() % 100) < 30) burst_left = 2;  // 30% 概率突发再丢 2 包
        }
        if (drop) { dropped_packets++; return; }
        receiver.onPacketReceived(data, len, now_us);
    };

    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        CommonHeader hdr; size_t consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;
        if (hdr.msg_type == MsgType::DATA) { lossyChannel(data, len); return; }
        // PING 等控制包无损送达
        receiver.onPacketReceived(data, len, now_us);
    });

    int nack_packets = 0;
    receiver.setSendCallback([&](const uint8_t* data, size_t len) {
        CommonHeader hdr; size_t consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;
        if (hdr.msg_type == MsgType::NACK) nack_packets++;
        // 控制方向无损（NACK/STATS/PONG/PLI）
        sender.onPacketReceived(data, len, now_us);
    });

    // ---- 固定测试数据：20 帧（IDR ~40KB / P 帧 ~10KB），模式确定 ----
    const int kFrames = 20;
    std::vector<std::vector<uint8_t>> frames;
    std::vector<FrameType> frame_types;
    for (int f = 0; f < kFrames; ++f) {
        bool idr = (f % 5 == 0);
        size_t sz = idr ? 40000 : 10000;
        std::vector<uint8_t> d(sz);
        for (size_t i = 0; i < sz; ++i) d[i] = static_cast<uint8_t>((i * 31 + f * 7) & 0xFF);
        frames.push_back(std::move(d));
        frame_types.push_back(idr ? frame_type::VIDEO_IDR : frame_type::VIDEO);
    }

    std::vector<Frame> completed;
    receiver.setFrameReadyCallback([&](Frame&& frame) {
        completed.push_back(std::move(frame));
    });
    int dropped_frames = 0;
    receiver.setFrameDroppedCallback([&](FrameType, uint32_t) { dropped_frames++; });

    // ---- 驱动：按 33ms/帧发送，5ms 步长 tick 双端，直到全部完成或超时 ----
    uint64_t next_frame_us = now_us;
    int sent = 0;
    bool timed_out = false;
    for (int step = 0; step < 10000; ++step) {  // 上限 50s 模拟时间
        now_us += 5'000;
        while (sent < kFrames && now_us >= next_frame_us) {
            sender.sendFrame(frames[sent].data(), frames[sent].size(),
                             frame_types[sent], now_us);
            sent++;
            next_frame_us += 33'000;  // 30fps
        }
        sender.tick(now_us);
        receiver.tick(now_us);
        if (sent == kFrames && completed.size() == static_cast<size_t>(kFrames)) break;
        if (sent == kFrames &&
            now_us - next_frame_us > static_cast<uint64_t>(receiver_cfg.frame_timeout_ms) * 1000 + 500'000) {
            timed_out = true;
            break;
        }
    }

    std::cout << "(pkts=" << data_packets << " dropped=" << dropped_packets
              << " nacks=" << nack_packets << " done=" << completed.size() << "/"
              << kFrames << ") ";

    if (timed_out) { FAIL("simulation timed out waiting for frames"); return; }
    if (dropped_packets == 0) { FAIL("channel dropped nothing (loss model broken)"); return; }
    if (completed.size() != static_cast<size_t>(kFrames)) {
        FAIL("expected " + std::to_string(kFrames) + " frames, got " +
             std::to_string(completed.size()) + " (frame_drops=" +
             std::to_string(dropped_frames) + ")");
        return;
    }
    // 逐帧校验：顺序、类型、内容
    for (int f = 0; f < kFrames; ++f) {
        if (completed[f].type != frame_types[f]) {
            FAIL("frame " + std::to_string(f) + " type mismatch");
            return;
        }
        if (completed[f].size != frames[f].size() ||
            std::memcmp(completed[f].data.data(), frames[f].data(), frames[f].size()) != 0) {
            FAIL("frame " + std::to_string(f) + " data mismatch");
            return;
        }
    }

    // 统计一致性：有丢包就必须有恢复（FEC 和/或 NACK）
    const ProtocolStats& rs = receiver.getStats();
    if (rs.frames_completed != static_cast<uint64_t>(kFrames)) {
        FAIL("stats.frames_completed mismatch");
        return;
    }
    if (rs.frames_dropped != 0) {
        FAIL("unexpected frames_dropped=" + std::to_string(rs.frames_dropped));
        return;
    }
    if (rs.blocks_lost == 0) { FAIL("stats.blocks_lost == 0 despite loss"); return; }
    if (rs.blocks_recovered_fec == 0 && rs.blocks_recovered_nack == 0) {
        FAIL("no recovery recorded despite loss");
        return;
    }
    if (rs.blocks_recovered_fec + rs.blocks_recovered_nack < rs.blocks_lost) {
        FAIL("recovered(" + std::to_string(rs.blocks_recovered_fec + rs.blocks_recovered_nack) +
             ") < lost(" + std::to_string(rs.blocks_lost) + ")");
        return;
    }
    if (nack_packets == 0) { FAIL("no NACK sent despite 8% loss"); return; }
    if (sender.getStats().frames_sent != static_cast<uint64_t>(kFrames)) {
        FAIL("sender frames_sent mismatch");
        return;
    }

    PASS();
}

// Test: 突发丢包仅靠 FEC 恢复（无 NACK 参与）
// 一个 FEC 组内连续丢 2 个数据块，冗余足够时组内即可解码，不应触发 NACK
void test_burst_loss_fec_only() {
    TEST("Burst loss recovered by FEC only (no NACK)");

    SenderConfig sender_cfg;
    sender_cfg.mtu = 1400;
    sender_cfg.fec_ratio = 0.5f;   // 冗余足够覆盖 2 块丢失

    ReceiverConfig receiver_cfg;
    receiver_cfg.nack_delay_ms = 20;
    receiver_cfg.stats_interval_ms = 100000;

    FrameSender sender(sender_cfg);
    FrameReceiver receiver(receiver_cfg);

    uint64_t now_us = 1'000'000;
    int nack_packets = 0;
    int packet_index = 0;

    // 首个 FEC 组的前 4 个包中连续丢第 2、3 个（block 1、2）
    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        packet_index++;
        if (packet_index == 2 || packet_index == 3) return;  // 突发丢 2 包
        receiver.onPacketReceived(data, len, now_us);
    });
    receiver.setSendCallback([&](const uint8_t* data, size_t len) {
        CommonHeader hdr; size_t consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;
        if (hdr.msg_type == MsgType::NACK) nack_packets++;
        sender.onPacketReceived(data, len, now_us);
    });

    Frame received_frame;
    bool frame_received = false;
    receiver.setFrameReadyCallback([&](Frame&& f) {
        received_frame = std::move(f);
        frame_received = true;
    });

    // 5000B + MTU 1400 → 4 个数据块，单组（max_group_size=127）
    std::vector<uint8_t> test_data(5000);
    for (size_t i = 0; i < test_data.size(); ++i)
        test_data[i] = static_cast<uint8_t>((i * 13 + 5) & 0xFF);

    bool ok = sender.sendFrame(test_data.data(), test_data.size(),
                               frame_type::VIDEO_IDR, now_us);
    if (!ok) { FAIL("sendFrame returned false"); return; }

    // 即使 FEC 当轮未解码，给少量时间也不应产生 NACK
    for (int i = 0; i < 10 && !frame_received; ++i) {
        now_us += 5'000;
        receiver.tick(now_us);
    }

    const ProtocolStats& rs = receiver.getStats();
    if (!frame_received) { FAIL("frame not recovered"); return; }
    if (nack_packets != 0) {
        FAIL("NACK was sent but FEC alone should suffice (nacks=" +
             std::to_string(nack_packets) + ")");
        return;
    }
    if (rs.blocks_recovered_fec < 2) {
        FAIL("expected >=2 FEC-recovered blocks, got " +
             std::to_string(rs.blocks_recovered_fec));
        return;
    }
    if (std::memcmp(received_frame.data.data(), test_data.data(), test_data.size()) != 0) {
        FAIL("data mismatch after FEC-only recovery");
        return;
    }

    PASS();
}

// Test: 按序提交下，游标帧超过缓存时限（默认 2s）丢弃并请求关键帧，
// 其后已收全的帧立即补交（丢弃不阻塞后续帧）
void test_in_order_drop_and_continue() {
    TEST("In-order: cursor frame timeout -> drop + PLI + continue");

    SenderConfig sender_cfg;
    sender_cfg.mtu = 1400;
    sender_cfg.fec_ratio = 0.0f;

    ReceiverConfig receiver_cfg;
    receiver_cfg.frame_timeout_ms = 2000;
    receiver_cfg.stats_interval_ms = 1000000;

    FrameSender sender(sender_cfg);
    FrameReceiver receiver(receiver_cfg);
    uint64_t now_us = 1'000'000;

    // 帧 A（游标帧）的首块永久丢弃（含重传）：A 永远无法补全
    const SeqNum drop_seq = 1;
    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        CommonHeader hdr; size_t consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;
        if (hdr.msg_type == MsgType::DATA && hdr.seq_num == drop_seq) return;
        receiver.onPacketReceived(data, len, now_us);
    });

    int pli_count = 0;
    receiver.setSendCallback([&](const uint8_t* data, size_t len) {
        CommonHeader hdr; size_t consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;
        if (hdr.msg_type == MsgType::PLI) pli_count++;
        sender.onPacketReceived(data, len, now_us);
    });

    std::vector<Frame> frames;
    receiver.setFrameReadyCallback([&](Frame&& f) { frames.push_back(std::move(f)); });

    std::vector<uint8_t> fa(3000, 0xAA), fb(3000, 0xBB);
    sender.sendFrame(fa.data(), fa.size(), frame_type::VIDEO, now_us);
    sender.sendFrame(fb.data(), fb.size(), frame_type::VIDEO, now_us);

    // 帧 B 已收全，但游标帧 A 未完成：必须等待，不得提前提交
    if (frames.size() != 0) {
        FAIL("frame B delivered before cursor frame A timed out (got " +
             std::to_string(frames.size()) + ")");
        return;
    }

    // 推进模拟时钟越过 2s 超时 → A 丢弃 + PLI + B 补交
    for (int i = 0; i < 30 && frames.empty(); ++i) {
        now_us += 100'000;  // +100ms
        receiver.tick(now_us);
    }

    const ProtocolStats& rs = receiver.getStats();
    if (rs.frames_dropped != 1) {
        FAIL("expected 1 dropped frame (cursor A), got " + std::to_string(rs.frames_dropped));
        return;
    }
    if (pli_count != 1) {
        FAIL("expected 1 PLI on frame drop, got " + std::to_string(pli_count));
        return;
    }
    if (frames.size() != 1 || frames[0].data != fb) {
        FAIL("frame B not delivered after cursor skip (got " +
             std::to_string(frames.size()) + ")");
        return;
    }

    PASS();
}

// Test: FEC 组恢复后，该组覆盖的缺失 seq 全部从 NACK map 清除
// 丢一个数据块，但 FEC 冗余足以恢复 → 帧完整交付，且此后不得再对该组发任何 NACK
void test_fec_recovery_clears_group_nacks() {
    TEST("FEC group recovery clears group NACKs");

    SenderConfig sender_cfg;
    sender_cfg.mtu = 1400;
    sender_cfg.fec_ratio = 0.5f;   // 3 数据块 → 2 FEC 块，可容忍 2 块丢失

    ReceiverConfig receiver_cfg;
    receiver_cfg.nack_delay_ms = 10;
    receiver_cfg.max_nack_retries = 40;  // 生命周期内不因重试上限停工

    FrameSender sender(sender_cfg);
    FrameReceiver receiver(receiver_cfg);
    uint64_t now_us = 1'000'000;

    int nack_packets = 0;
    receiver.setSendCallback([&](const uint8_t* data, size_t len) {
        CommonHeader hdr; size_t consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;
        if (hdr.msg_type == MsgType::NACK) nack_packets++;
    });

    bool dropped = false;
    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        CommonHeader hdr; size_t consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;
        if (hdr.msg_type == MsgType::DATA && hdr.seq_num == 2 && !dropped) {
            dropped = true;
            return;  // 首次传输丢弃 seq=2（块 1）
        }
        receiver.onPacketReceived(data, len, now_us);
    });

    Frame received_frame;
    bool frame_received = false;
    receiver.setFrameReadyCallback([&](Frame&& frame) {
        received_frame = std::move(frame);
        frame_received = true;
    });

    // 4000B / MTU 1400 → 3 数据块（seq 1..3）+ 2 FEC 块（seq 4..5）
    std::vector<uint8_t> test_data(4000);
    for (size_t i = 0; i < test_data.size(); ++i) {
        test_data[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    }

    sender.sendFrame(test_data.data(), test_data.size(),
                     frame_type::VIDEO, now_us);
    if (!dropped) { FAIL("drop did not happen"); return; }
    if (!frame_received) {
        FAIL("frame not recovered via FEC");
        return;
    }
    if (received_frame.data.size() != test_data.size() ||
        received_frame.data != test_data) {
        FAIL("frame data mismatch after FEC recovery");
        return;
    }
    const ProtocolStats& rs = receiver.getStats();
    if (rs.blocks_recovered_fec == 0) {
        FAIL("no FEC recovery recorded");
        return;
    }

    // 缺口 seq=2 已被 FEC 补齐：越过 delay/interval 后也不得再有该组 NACK
    for (int i = 0; i < 5; ++i) {
        now_us += 50'000;
        receiver.tick(now_us);
    }
    if (nack_packets != 0) {
        FAIL("NACK sent despite FEC group recovery (nack_packets=" +
             std::to_string(nack_packets) + ")");
        return;
    }

    std::cout << "(fec_recovered=" << rs.blocks_recovered_fec << ") ";
    PASS();
}

// Test: NACK 重试间隔跟随 RTT（1×RTT，下限为配置节流值）
// 注入 RTT=120ms → 重试间隔应为 120ms；无 RTT 时回退到配置 50ms
void test_nack_rtt_interval() {
    TEST("NACK interval follows RTT");

    // 注入 RTT=120ms 的接收端
    SenderConfig sender_cfg;
    sender_cfg.mtu = 1400;
    sender_cfg.fec_ratio = 0.0f;
    ReceiverConfig receiver_cfg;
    receiver_cfg.nack_delay_ms = 10;
    receiver_cfg.nack_interval_ms = 50;
    receiver_cfg.max_nack_retries = 40;

    FrameReceiver receiver(receiver_cfg);
    uint64_t now_us = 1'000'000;

    std::vector<uint64_t> nack_offsets_us;  // 相对 t0 的 NACK 时刻
    receiver.setSendCallback([&](const uint8_t* data, size_t len) {
        CommonHeader hdr; size_t consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;
        if (hdr.msg_type == MsgType::NACK) {
            nack_offsets_us.push_back(now_us - 1'000'000);
        }
    });
    receiver.setFrameReadyCallback([](Frame&&) {});

    // 1) 手工 PING：携带 RTT TLV = 120ms → 接收端学到链路 RTT
    {
        uint8_t ping[64];
        CommonHeader hdr;
        hdr.version = kProtocolVersion;
        hdr.seq_len = 1;
        hdr.msg_type = MsgType::PING;
        hdr.seq_num = 1;
        size_t off = hdr.serialize(ping, sizeof(ping));
        writeBE16(ping + off, 14); off += 2;  // TIMESTAMP(10) + RTT(4)
        ping[off++] = static_cast<uint8_t>(TlvType::TIMESTAMP);
        ping[off++] = 8;
        writeBE64(ping + off, now_us); off += 8;
        ping[off++] = static_cast<uint8_t>(TlvType::RTT);
        ping[off++] = 2;
        writeBE16(ping + off, 120); off += 2;
        receiver.onPacketReceived(ping, off, now_us);
    }

    // 2) 发一帧（seq 1..3），丢 seq=2 → 无 FEC，缺口入 NACK map
    bool dropped = false;
    auto deliver = [&](const uint8_t* data, size_t len) {
        CommonHeader hdr; size_t consumed = 0;
        if (!CommonHeader::deserialize(data, len, hdr, consumed)) return;
        if (hdr.msg_type == MsgType::DATA && hdr.seq_num == 2 && !dropped) {
            dropped = true;
            return;
        }
        receiver.onPacketReceived(data, len, now_us);
    };
    {
        FrameSender sender(sender_cfg);
        sender.setSendCallback(deliver);
        std::vector<uint8_t> payload(3000);
        sender.sendFrame(payload.data(), payload.size(),
                         frame_type::VIDEO, now_us);
    }
    if (!dropped) { FAIL("drop did not happen"); return; }

    // 3) 时间线：10ms 首次 NACK；之后按 120ms（而非固定 50ms）重试
    auto tickAt = [&](uint64_t us) {
        now_us = 1'000'000 + us;
        receiver.tick(now_us);
    };
    tickAt(5'000);
    if (!nack_offsets_us.empty()) { FAIL("NACK before 10ms delay"); return; }
    tickAt(10'000);
    if (nack_offsets_us.size() != 1) { FAIL("first NACK not at 10ms"); return; }
    tickAt(60'000);    // 50ms 间隔若生效会在此出现：120ms 未到 → 不应有
    if (nack_offsets_us.size() != 1) { FAIL("NACK before 1×RTT interval"); return; }
    tickAt(130'000);   // 距首次 120ms → 第二次
    if (nack_offsets_us.size() != 2) { FAIL("second NACK not at 1×RTT"); return; }
    if (nack_offsets_us[1] - nack_offsets_us[0] != 120'000) {
        FAIL("NACK interval != RTT (got " +
             std::to_string(nack_offsets_us[1] - nack_offsets_us[0]) + "us)");
        return;
    }

    PASS();
}

// Test: FEC 冗余自适应公式 fec = clamp(loss × factor, fec_min, fec_max)
// 覆盖上限 clamp、下限 clamp、死区防抖三个行为
void test_adaptive_fec_formula() {
    TEST("Adaptive FEC formula");

    BitrateControllerConfig cfg;
    cfg.initial_bitrate_bps = 1'000'000;
    cfg.max_bitrate_bps = 5'000'000;
    cfg.min_bitrate_bps = 200'000;
    cfg.window_ms = 300;
    cfg.fec_initial = 0.25f;
    cfg.fec_min = 0.05f;
    cfg.fec_max = 0.30f;
    cfg.fec_loss_factor = 1.5f;
    cfg.advice_change_min = 0.05f;
    cfg.loss_down_threshold = 0.05f;  // 使各阶段都停留在观察/确认路径，聚焦 FEC 公式

    BitrateController ctrl(cfg);
    uint64_t now_us = 0;
    const uint64_t win = static_cast<uint64_t>(cfg.window_ms) * 1000ULL;
    auto feed = [&](float loss) {
        now_us += win;
        ctrl.onFeedback(now_us, 1'000'000, loss);
    };
    auto near = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };

    if (!near(ctrl.fecRatio(), 0.25f)) {
        FAIL("initial not clamped");
        return;
    }
    feed(0.0f);            // 0×1.5=0 < min → 0.05
    if (!near(ctrl.fecRatio(), cfg.fec_min)) { FAIL("floor not clamped"); return; }
    feed(0.10f);           // 0.15
    if (!near(ctrl.fecRatio(), 0.15f)) { FAIL("formula 1.5×loss wrong"); return; }
    feed(0.20f);           // 0.30 = fec_max
    if (!near(ctrl.fecRatio(), cfg.fec_max)) { FAIL("cap not clamped"); return; }
    feed(0.40f);           // 0.60 → clamp 0.30
    if (!near(ctrl.fecRatio(), cfg.fec_max)) { FAIL("high loss not capped"); return; }
    feed(0.0f);            // 回落 → 0.05
    if (!near(ctrl.fecRatio(), cfg.fec_min)) { FAIL("not relaxed to floor"); return; }
    feed(0.034f);          // 0.051 与当前 0.05 差 < 死区 0.02 → 保持
    if (!near(ctrl.fecRatio(), cfg.fec_min)) { FAIL("dead band not honored"); return; }
    feed(0.10f);           // 0.15 差 0.1 ≥ 死区 → 更新
    if (!near(ctrl.fecRatio(), 0.15f)) { FAIL("dead band stuck after update"); return; }

    PASS();
}

int main() {
    std::cout << "=== FEC Protocol Tests ===" << std::endl;
    std::cout << std::endl;

    test_protocol_serialization();
    test_basic_loopback();
    test_single_packet_frame();
    test_large_frame_multiple_groups();
    test_fec_recovery();
    test_burst_loss_fec_only();
    test_random_loss_full_loop();
    test_nack_recovery();
    test_out_of_order_completion();
    test_in_order_drop_and_continue();
    test_mid_join_gap_nack();
    test_fec_recovery_clears_group_nacks();
    test_nack_rtt_interval();
    test_adaptive_fec_formula();
    test_ping_pong();
    test_ping_interval_frequent_ticks();
    test_stats();
    test_bitrate_controller();
    test_pli_on_frame_drop();
    test_pli_client_throttle();
    test_sender_pli_callback();
    test_proxy_pli_forward();
    test_proxy_ping_pong();

    std::cout << std::endl;
    std::cout << "=== Results: " << tests_passed << "/" << tests_run << " passed ===" << std::endl;

    return (tests_passed == tests_run) ? 0 : 1;
}

