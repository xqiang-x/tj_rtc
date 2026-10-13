// FrameAssembler 测试：服务器侧只读组帧器（frame_protoc/src/frame_assembler）
// 覆盖：无丢失组帧（happy）、FEC 恢复（happy）、超时丢弃+游标推进（abnormal）、
// 乱序到达按帧序交付（happy）、投喂缺口恢复（abnormal）。

#include <iostream>
#include <cstring>
#include <vector>

#include "sender.h"
#include "protocol.h"
#include "frame_assembler.h"

using namespace fec_protocol;

// Simple test harness (与 frame_protoc/src/test 同风格)
static int tests_run = 0;
static int tests_passed = 0;

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

static std::vector<uint8_t> makePattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> data(n);
    for (size_t i = 0; i < n; ++i) {
        data[i] = static_cast<uint8_t>((i * 7 + seed) & 0xFF);
    }
    return data;
}

// 收集交付帧的辅助结构
struct Delivery {
    std::vector<Frame> frames;
};

static void wireAssembler(FrameSender& sender, FrameAssembler& assembler,
                          uint64_t now_us) {
    sender.setSendCallback([&assembler, now_us](const uint8_t* data, size_t len) {
        assembler.onDataPacket(data, len, now_us);
    });
}

// Test 1: 无丢失 → 组帧输出与原始数据一致
static void test_basic_loopback() {
    TEST("Basic loopback (no loss)");

    SenderConfig sender_cfg;
    sender_cfg.mtu = 1400;
    sender_cfg.fec_ratio = 0.25f;

    FrameSender sender(sender_cfg);
    FrameAssembler assembler;

    Delivery delivery;
    assembler.setFrameReadyCallback([&](Frame&& frame) {
        delivery.frames.push_back(std::move(frame));
    });

    const uint64_t t = 1'000'000;
    wireAssembler(sender, assembler, t);

    auto test_data = makePattern(5000, 13);
    if (!sender.sendFrame(test_data.data(), test_data.size(),
                          frame_type::VIDEO_IDR, t)) {
        FAIL("sendFrame returned false");
        return;
    }
    assembler.tick(t);

    if (delivery.frames.size() != 1) {
        FAIL("expected 1 frame, got " + std::to_string(delivery.frames.size()));
        return;
    }
    const Frame& f = delivery.frames[0];
    if (f.type != frame_type::VIDEO_IDR) { FAIL("frame type mismatch"); return; }
    if (f.size != test_data.size()) { FAIL("size mismatch"); return; }
    if (std::memcmp(f.data.data(), test_data.data(), test_data.size()) != 0) {
        FAIL("data mismatch");
        return;
    }

    const auto& stats = assembler.getStats();
    if (stats.frames_assembled != 1) { FAIL("frames_assembled != 1"); return; }
    if (stats.pending_frames != 0) { FAIL("pending_frames != 0"); return; }
    if (stats.packets_received == 0) { FAIL("packets_received == 0"); return; }

    PASS();
}

// Test 2: 丢包由 FEC 恢复 → 帧仍完整交付
static void test_fec_recovery() {
    TEST("FEC recovery (simulated loss)");

    SenderConfig sender_cfg;
    sender_cfg.mtu = 1400;
    sender_cfg.fec_ratio = 0.5f;  // 50% 冗余，可容忍 25% 丢包（与 fec_test 一致）

    FrameSender sender(sender_cfg);
    FrameAssembler assembler;

    Delivery delivery;
    assembler.setFrameReadyCallback([&](Frame&& frame) {
        delivery.frames.push_back(std::move(frame));
    });

    const uint64_t t = 1'000'000;
    int packet_count = 0;
    int dropped_count = 0;
    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        packet_count++;
        if (packet_count % 4 == 0) {  // 每 4 包丢 1
            dropped_count++;
            return;
        }
        assembler.onDataPacket(data, len, t);
    });

    auto test_data = makePattern(5000, 29);
    if (!sender.sendFrame(test_data.data(), test_data.size(),
                          frame_type::VIDEO_IDR, t)) {
        FAIL("sendFrame returned false");
        return;
    }
    assembler.tick(t);

    std::cout << "(sent=" << packet_count << " dropped=" << dropped_count << ") ";

    if (delivery.frames.size() != 1) {
        FAIL("frame not recovered via FEC");
        return;
    }
    if (delivery.frames[0].size != test_data.size() ||
        std::memcmp(delivery.frames[0].data.data(), test_data.data(),
                    test_data.size()) != 0) {
        FAIL("data mismatch after FEC recovery");
        return;
    }
    PASS();
}

// Test 3: 帧超时丢弃 + 游标推进（后续完整帧正常交付）
static void test_timeout_drop_and_cursor_advance() {
    TEST("Frame timeout drop + cursor advance");

    SenderConfig sender_cfg;
    sender_cfg.mtu = 1400;
    sender_cfg.fec_ratio = 0.25f;

    FrameAssemblerConfig cfg;
    cfg.frame_timeout_ms = 2000;

    FrameSender sender(sender_cfg);
    FrameAssembler assembler(cfg);

    Delivery delivery;
    assembler.setFrameReadyCallback([&](Frame&& frame) {
        delivery.frames.push_back(std::move(frame));
    });

    const uint64_t t = 1'000'000;

    // 帧 A：只放行第 1 包 → 永远无法收全
    int pkt_idx = 0;
    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        pkt_idx++;
        if (pkt_idx != 1) return;
        assembler.onDataPacket(data, len, t);
    });

    auto data_a = makePattern(5000, 41);
    if (!sender.sendFrame(data_a.data(), data_a.size(), frame_type::VIDEO, t)) {
        FAIL("sendFrame A returned false");
        return;
    }

    // 超时前：不交付
    assembler.tick(t + 1'000'000);  // +1s < 2s 超时
    if (!delivery.frames.empty()) { FAIL("frame delivered before timeout"); return; }

    // 越过帧超时：丢弃 A，且不交付
    assembler.tick(t + 3'000'000);  // +3s > 2s
    if (!delivery.frames.empty()) { FAIL("timed-out frame delivered"); return; }
    if (assembler.getStats().frames_timed_out != 1) {
        FAIL("frames_timed_out != 1");
        return;
    }

    // 帧 B：完整送达 → 游标已跳过 A，B 正常交付
    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        assembler.onDataPacket(data, len, t + 4'000'000);
    });
    auto data_b = makePattern(4000, 53);
    if (!sender.sendFrame(data_b.data(), data_b.size(),
                          frame_type::VIDEO_IDR, t + 4'000'000)) {
        FAIL("sendFrame B returned false");
        return;
    }
    assembler.tick(t + 4'000'000);

    if (delivery.frames.size() != 1) {
        FAIL("frame B not delivered after cursor advance");
        return;
    }
    if (delivery.frames[0].size != data_b.size() ||
        std::memcmp(delivery.frames[0].data.data(), data_b.data(),
                    data_b.size()) != 0) {
        FAIL("frame B data mismatch");
        return;
    }
    PASS();
}

// Test 4: 后帧先收全 → 仍按帧序交付
static void test_out_of_order_commit() {
    TEST("Out-of-order arrival, ordered delivery");

    SenderConfig sender_cfg;
    sender_cfg.mtu = 1400;
    sender_cfg.fec_ratio = 0.25f;

    FrameSender sender(sender_cfg);
    FrameAssembler assembler;

    Delivery delivery;
    assembler.setFrameReadyCallback([&](Frame&& frame) {
        delivery.frames.push_back(std::move(frame));
    });

    const uint64_t t = 1'000'000;

    std::vector<std::vector<std::vector<uint8_t>>> pkts;
    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        pkts.back().emplace_back(data, data + len);
    });

    auto data_a = makePattern(5000, 67);
    auto data_b = makePattern(5000, 89);

    pkts.emplace_back();
    if (!sender.sendFrame(data_a.data(), data_a.size(), frame_type::VIDEO, t)) {
        FAIL("sendFrame A returned false");
        return;
    }
    pkts.emplace_back();
    if (!sender.sendFrame(data_b.data(), data_b.size(),
                          frame_type::VIDEO_IDR, t)) {
        FAIL("sendFrame B returned false");
        return;
    }
    const auto& pkts_a = pkts[0];
    const auto& pkts_b = pkts[1];
    if (pkts_a.empty() || pkts_b.empty()) { FAIL("no packets emitted"); return; }

    // A 的首包建立游标，随后 B 全部到达（先收全但不许先交付）
    assembler.onDataPacket(pkts_a[0].data(), pkts_a[0].size(), t);
    for (const auto& p : pkts_b) {
        assembler.onDataPacket(p.data(), p.size(), t);
    }
    if (!delivery.frames.empty()) {
        FAIL("frame B delivered before frame A");
        return;
    }

    // 补上 A 的其余包 → A、B 按序交付
    for (size_t i = 1; i < pkts_a.size(); ++i) {
        assembler.onDataPacket(pkts_a[i].data(), pkts_a[i].size(), t);
    }
    assembler.tick(t);

    if (delivery.frames.size() != 2) {
        FAIL("expected 2 frames, got " + std::to_string(delivery.frames.size()));
        return;
    }
    if (delivery.frames[0].type != frame_type::VIDEO ||
        delivery.frames[1].type != frame_type::VIDEO_IDR) {
        FAIL("delivery order not preserved (A then B)");
        return;
    }
    if (std::memcmp(delivery.frames[0].data.data(), data_a.data(), data_a.size()) != 0 ||
        std::memcmp(delivery.frames[1].data.data(), data_b.data(), data_b.size()) != 0) {
        FAIL("delivered data mismatch");
        return;
    }
    PASS();
}

// Test 5: 非 DATA 包与陈旧包不产生副作用
static void test_non_data_ignored() {
    TEST("Non-DATA and stale packets ignored");

    SenderConfig sender_cfg;
    sender_cfg.mtu = 1400;

    FrameSender sender(sender_cfg);
    FrameAssembler assembler;

    Delivery delivery;
    assembler.setFrameReadyCallback([&](Frame&& frame) {
        delivery.frames.push_back(std::move(frame));
    });

    const uint64_t t = 1'000'000;
    std::vector<std::vector<std::vector<uint8_t>>> pkts;
    pkts.emplace_back();
    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        pkts.back().emplace_back(data, data + len);
    });

    auto data_a = makePattern(3000, 97);
    if (!sender.sendFrame(data_a.data(), data_a.size(), frame_type::VIDEO, t)) {
        FAIL("sendFrame returned false");
        return;
    }
    const auto& pa = pkts[0];

    // 篡改一个包为 PING 类型 → 组帧器必须忽略（无回包、无计数增长）
    std::vector<uint8_t> fake = pa[0];
    fake[1] = static_cast<uint8_t>(MsgType::PING);
    assembler.onDataPacket(fake.data(), fake.size(), t);
    if (assembler.getStats().packets_received != 0) {
        FAIL("non-DATA packet counted");
        return;
    }

    // 正常收全 → 交付
    for (const auto& p : pa) {
        assembler.onDataPacket(p.data(), p.size(), t);
    }
    if (delivery.frames.size() != 1) { FAIL("frame not delivered"); return; }

    // 交付后重复投喂陈旧包 → 无副作用（不重复交付）
    for (const auto& p : pa) {
        assembler.onDataPacket(p.data(), p.size(), t);
    }
    assembler.tick(t);
    if (delivery.frames.size() != 1) {
        FAIL("stale packet caused duplicate delivery");
        return;
    }
    PASS();
}

// Test 6: 投喂缺口恢复 — 游标帧从未出现（整段未投喂）时超时跳过节段
static void test_feed_gap_recovery() {
    TEST("Feed gap recovery (cursor frame never arrives)");

    SenderConfig sender_cfg;
    sender_cfg.mtu = 1400;
    sender_cfg.fec_ratio = 0.25f;

    FrameAssemblerConfig cfg;
    cfg.frame_timeout_ms = 2000;

    FrameSender sender(sender_cfg);
    FrameAssembler assembler(cfg);

    Delivery delivery;
    assembler.setFrameReadyCallback([&](Frame&& frame) {
        delivery.frames.push_back(std::move(frame));
    });

    const uint64_t t = 1'000'000;

    // 帧 0：完整投喂并交付，游标推进到 1
    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        assembler.onDataPacket(data, len, t);
    });
    auto data0 = makePattern(3000, 11);
    if (!sender.sendFrame(data0.data(), data0.size(), frame_type::VIDEO_IDR, t)) {
        FAIL("sendFrame 0 returned false");
        return;
    }
    assembler.tick(t);
    if (delivery.frames.size() != 1) { FAIL("frame 0 not delivered"); return; }

    // 帧 1..3：发送但整段不投喂（模拟无订阅者期间跳过组帧）→ 游标帧 1 从未出现
    sender.setSendCallback([](const uint8_t*, size_t) {});
    for (int i = 0; i < 3; ++i) {
        auto d = makePattern(3000, static_cast<uint8_t>(20 + i));
        if (!sender.sendFrame(d.data(), d.size(), frame_type::VIDEO, t)) {
            FAIL("sendFrame gap returned false");
            return;
        }
    }

    // 帧 4：缺口后恢复投喂（+3s > frame_timeout）→ 超时扫描跳过节段并交付
    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        assembler.onDataPacket(data, len, t + 3'000'000);
    });
    auto data4 = makePattern(3500, 31);
    if (!sender.sendFrame(data4.data(), data4.size(), frame_type::VIDEO,
                          t + 3'000'000)) {
        FAIL("sendFrame 4 returned false");
        return;
    }
    assembler.tick(t + 3'000'000);

    if (delivery.frames.size() != 2) {
        FAIL("frame after gap not delivered (got " +
             std::to_string(delivery.frames.size()) + ")");
        return;
    }
    if (delivery.frames[1].size != data4.size() ||
        std::memcmp(delivery.frames[1].data.data(), data4.data(),
                    data4.size()) != 0) {
        FAIL("frame after gap data mismatch");
        return;
    }
    const auto& stats = assembler.getStats();
    if (stats.cursor_gap_jumps != 1) {
        FAIL("cursor_gap_jumps != 1 (got " +
             std::to_string(stats.cursor_gap_jumps) + ")");
        return;
    }
    if (stats.pending_frames != 0) { FAIL("pending_frames != 0"); return; }
    PASS();
}

int main() {
    std::cout << "=== FrameAssembler Tests ===" << std::endl;

    test_basic_loopback();
    test_fec_recovery();
    test_timeout_drop_and_cursor_advance();
    test_out_of_order_commit();
    test_non_data_ignored();
    test_feed_gap_recovery();

    std::cout << "\n" << tests_passed << "/" << tests_run << " tests passed"
              << std::endl;
    return (tests_passed == tests_run) ? 0 : 1;
}