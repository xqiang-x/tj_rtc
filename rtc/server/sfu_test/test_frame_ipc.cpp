// FrameDispatcher / FrameIpc 集成测试（单线程：测试线程自驱 ev_run(EVRUN_NOWAIT)）
// 覆盖：
//  1. 协议契约：serialize/parse 往返 + frame_type 数值与 frame_protoc 静态一致
//  2. 订阅→收帧→退订：SUB/SUB_OK(sid)、帧扇出（双订阅者）、UNSUB 生效、
//     isStreamActive 追踪
//  3. 背压：慢消费者积压丢非关键帧、关键帧保送
//  4. 断连重连：断连清引用、重连 SUB 后 sid 稳定
//  5. PLI 节流与转发回调
//  6. SUB_ERR（单连接订阅数上限）

#include <iostream>
#include <cstring>
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>

extern "C" {
#include <ev.h>
}

#include "FrameIpc.h"
#include "FrameDispatcher.h"
#include "types.h"

using namespace frame_ipc;

// ── 契约：帧类型数值必须与 frame_protoc 一致（防漂移）────────────────────────
static_assert(kAudio == fec_protocol::frame_type::AUDIO, "frame_type drift: AUDIO");
static_assert(kVideo == fec_protocol::frame_type::VIDEO, "frame_type drift: VIDEO");
static_assert(kVideoIdr == fec_protocol::frame_type::VIDEO_IDR, "frame_type drift: VIDEO_IDR");
static_assert(kVideoParams == fec_protocol::frame_type::VIDEO_PARAMS, "frame_type drift: VIDEO_PARAMS");
static_assert(kFrameData == fec_protocol::frame_type::DATA, "frame_type drift: DATA");

// ── 测试骨架 ─────────────────────────────────────────────────────────────────

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

static struct ev_loop* g_loop = nullptr;

static uint64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 单步驱动事件循环（非阻塞），并短暂休眠避免忙转
static void pump(int ms) {
    uint64_t deadline = nowUs() + static_cast<uint64_t>(ms) * 1000;
    do {
        ev_run(g_loop, EVRUN_NOWAIT);
        usleep(500);
    } while (nowUs() < deadline);
}

// ── 测试上下文（每用例独立 dispatcher 与 socket 路径）────────────────────────

struct TestCtx {
    struct ev_loop* loop = nullptr;
    std::string path;
    std::unique_ptr<sfu::FrameDispatcher> d;
    int pli_calls = 0;
    bool pli_result = true;
};

static std::unique_ptr<TestCtx> makeCtx(int idx,
                                        const sfu::FrameDispatcher::Options& opts = {}) {
    auto ctx = std::make_unique<TestCtx>();
    ctx->loop = ev_loop_new(EVFLAG_AUTO);
    g_loop = ctx->loop;
    ctx->path = "/tmp/tj_rtc_ipc_test_" + std::to_string(getpid()) + "_" +
                std::to_string(idx) + ".sock";
    TestCtx* raw = ctx.get();
    ctx->d = std::make_unique<sfu::FrameDispatcher>(
        ctx->loop, ctx->path,
        [raw](const std::string&) { raw->pli_calls++; return raw->pli_result; },
        opts);
    if (!ctx->d->start()) {
        g_loop = nullptr;
        ev_loop_destroy(ctx->loop);
        return nullptr;
    }
    return ctx;
}

static void destroyCtx(std::unique_ptr<TestCtx>& ctx) {
    if (!ctx) return;
    ctx->d->stop();
    ctx->d.reset();
    ev_loop_destroy(ctx->loop);
    g_loop = nullptr;
    ctx.reset();
}

// ── IPC 客户端 ───────────────────────────────────────────────────────────────

struct Client {
    int fd = -1;
    std::vector<uint8_t> in;  // 未消费的输入字节

    bool connect(const std::string& path) {
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return false;
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            fd = -1;
            return false;
        }
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        return true;
    }

    bool sendRaw(const uint8_t* data, size_t len) {
        size_t off = 0;
        uint64_t deadline = nowUs() + 1'000'000;
        while (off < len && nowUs() < deadline) {
            ssize_t n = ::write(fd, data + off, len - off);
            if (n > 0) { off += static_cast<size_t>(n); continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pump(1);
                continue;
            }
            return false;
        }
        return off == len;
    }

    // 拉取内核缓冲 → in；解析出一条完整消息给 out
    bool tryExtract(std::vector<uint8_t>& out) {
        uint8_t tmp[8192];
        for (;;) {
            ssize_t n = ::read(fd, tmp, sizeof(tmp));
            if (n > 0) { in.insert(in.end(), tmp, tmp + n); continue; }
            break;  // EAGAIN / EOF
        }
        if (in.size() < 4) return false;
        uint32_t len = getU32(in.data());
        if (len < 2 || len > kMaxMessageSize) { in.clear(); return false; }
        if (in.size() < 4u + len) return false;
        out.assign(in.begin() + 4, in.begin() + 4 + len);
        in.erase(in.begin(), in.begin() + 4 + len);
        return true;
    }

    // 等待一条消息（驱动事件循环）
    bool waitMessage(std::vector<uint8_t>& out, int timeout_ms) {
        uint64_t deadline = nowUs() + static_cast<uint64_t>(timeout_ms) * 1000;
        do {
            if (tryExtract(out)) return true;
            ev_run(g_loop, EVRUN_NOWAIT);
            usleep(500);
        } while (nowUs() < deadline);
        return tryExtract(out);
    }

    bool sendSub(const std::string& name) {
        uint8_t buf[16 + kMaxStreamName];
        size_t n = serializeSub(buf, sizeof(buf), name.c_str(),
                                static_cast<uint16_t>(name.size()));
        return n > 0 && sendRaw(buf, n);
    }
    bool sendUnsub(uint32_t sid) {
        uint8_t buf[16];
        size_t n = serializeUnsub(buf, sizeof(buf), sid);
        return n > 0 && sendRaw(buf, n);
    }
    bool sendPli(uint32_t sid) {
        uint8_t buf[16];
        size_t n = serializePli(buf, sizeof(buf), sid);
        return n > 0 && sendRaw(buf, n);
    }

    void close() {
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
};

static std::vector<uint8_t> makePayload(size_t n, uint8_t seed) {
    std::vector<uint8_t> data(n);
    for (size_t i = 0; i < n; ++i) {
        data[i] = static_cast<uint8_t>((i * 13 + seed) & 0xFF);
    }
    return data;
}

static fec_protocol::Frame makeFrame(uint8_t type, size_t size, uint8_t seed) {
    fec_protocol::Frame f;
    f.type = type;
    f.size = static_cast<uint32_t>(size);
    f.data = makePayload(size, seed);
    return f;
}

// 期待一条 DATA 并校验头部/负载
static bool expectData(Client& c, uint32_t expect_sid, uint8_t expect_type,
                       uint8_t expect_flags, uint64_t expect_seq,
                       const std::vector<uint8_t>& expect_payload,
                       const char* what, std::string& err) {
    std::vector<uint8_t> raw;
    if (!c.waitMessage(raw, 2000)) {
        err = std::string(what) + ": no message";
        return false;
    }
    Message m;
    DataView dv;
    if (!parseMessage(raw.data(), static_cast<uint32_t>(raw.size()), m) ||
        !parseData(m, dv)) {
        err = std::string(what) + ": not a DATA message";
        return false;
    }
    if (dv.sid != expect_sid) { err = std::string(what) + ": sid mismatch"; return false; }
    if (dv.frame_type != expect_type) { err = std::string(what) + ": type mismatch"; return false; }
    if (dv.flags != expect_flags) { err = std::string(what) + ": flags mismatch"; return false; }
    if (dv.seq != expect_seq) { err = std::string(what) + ": seq mismatch"; return false; }
    if (dv.payload_len != expect_payload.size() ||
        std::memcmp(dv.payload, expect_payload.data(), expect_payload.size()) != 0) {
        err = std::string(what) + ": payload mismatch";
        return false;
    }
    return true;
}

// ── Test 1: 协议往返 ─────────────────────────────────────────────────────────

static void test_protocol_roundtrip() {
    TEST("Protocol serialize/parse roundtrip");

    uint8_t buf[4 + kDataMsgSize + 512];
    std::vector<uint8_t> payload = makePayload(300, 7);

    // DATA
    size_t n = serializeData(buf, sizeof(buf), 42, kVideoIdr, kFlagKey,
                             1234567890123ULL, 777, payload.data(),
                             static_cast<uint32_t>(payload.size()));
    if (n == 0) { FAIL("serializeData returned 0"); return; }
    Message m; DataView dv;
    if (!parseMessage(buf + 4, static_cast<uint32_t>(n - 4), m) ||
        m.version != kVersion || !parseData(m, dv)) {
        FAIL("DATA parse failed");
        return;
    }
    if (dv.sid != 42 || dv.frame_type != kVideoIdr || dv.flags != kFlagKey ||
        dv.pts_us != 1234567890123ULL || dv.seq != 777 ||
        dv.payload_len != payload.size() ||
        std::memcmp(dv.payload, payload.data(), payload.size()) != 0) {
        FAIL("DATA roundtrip mismatch");
        return;
    }

    // SUB / SUB_OK
    n = serializeSub(buf, sizeof(buf), "stream-abc", 10);
    if (n == 0 || !parseMessage(buf + 4, static_cast<uint32_t>(n - 4), m)) {
        FAIL("SUB serialize/parse failed");
        return;
    }
    NameView nv;
    if (!parseSub(m, nv) || nv.name_len != 10 ||
        std::memcmp(nv.name, "stream-abc", 10) != 0) {
        FAIL("SUB roundtrip mismatch");
        return;
    }

    n = serializeSubOk(buf, sizeof(buf), 5, "stream-abc", 10);
    SubOkView okv;
    if (n == 0 || !parseMessage(buf + 4, static_cast<uint32_t>(n - 4), m) ||
        !parseSubOk(m, okv) || okv.sid != 5 || okv.name_len != 10 ||
        std::memcmp(okv.name, "stream-abc", 10) != 0) {
        FAIL("SUB_OK roundtrip mismatch");
        return;
    }

    n = serializeSubErr(buf, sizeof(buf), "stream-abc", 10, kErrTooManyStreams);
    SubErrView ekv;
    if (n == 0 || !parseMessage(buf + 4, static_cast<uint32_t>(n - 4), m) ||
        !parseSubErr(m, ekv) || ekv.name_len != 10 || ekv.reason != kErrTooManyStreams) {
        FAIL("SUB_ERR roundtrip mismatch");
        return;
    }

    // UNSUB / PLI
    n = serializeUnsub(buf, sizeof(buf), 9);
    uint32_t sid = 0;
    if (n == 0 || !parseMessage(buf + 4, static_cast<uint32_t>(n - 4), m) ||
        !parseSid(m, sid) || sid != 9) {
        FAIL("UNSUB roundtrip mismatch");
        return;
    }
    n = serializePli(buf, sizeof(buf), 11);
    if (n == 0 || !parseMessage(buf + 4, static_cast<uint32_t>(n - 4), m) ||
        !parseSid(m, sid) || sid != 11) {
        FAIL("PLI roundtrip mismatch");
        return;
    }

    // 边界：非法参数
    if (serializeSub(buf, sizeof(buf), nullptr, 0) != 0 ||
        serializeSub(buf, 8, "abcdef", 6) != 0 ||
        serializeData(buf, 10, 1, kVideo, 0, 0, 0, payload.data(), 300) != 0) {
        FAIL("capacity guards failed");
        return;
    }
    PASS();
}

// ── Test 2: 订阅 → 收帧 → 退订 ───────────────────────────────────────────────

static void test_subscribe_deliver_unsubscribe() {
    TEST("Subscribe -> deliver -> fanout -> unsubscribe");

    auto ctx = makeCtx(2);
    if (!ctx) { FAIL("dispatcher start failed"); return; }

    Client a, b;
    if (!a.connect(ctx->path) || !b.connect(ctx->path)) {
        FAIL("client connect failed");
        destroyCtx(ctx);
        return;
    }
    pump(30);

    if (!ctx->d->isStreamActive("s1")) {
        // 预期：无订阅时 inactive
    } else {
        FAIL("stream active before subscribe");
        a.close(); b.close(); destroyCtx(ctx);
        return;
    }

    // A SUB → SUB_OK sid=1
    if (!a.sendSub("s1")) { FAIL("SUB send failed"); a.close(); b.close(); destroyCtx(ctx); return; }
    std::vector<uint8_t> raw;
    if (!a.waitMessage(raw, 2000)) { FAIL("no SUB_OK"); a.close(); b.close(); destroyCtx(ctx); return; }
    Message m; SubOkView okv;
    if (!parseMessage(raw.data(), static_cast<uint32_t>(raw.size()), m) ||
        !parseSubOk(m, okv) || okv.sid != 1) {
        FAIL("bad SUB_OK");
        a.close(); b.close(); destroyCtx(ctx);
        return;
    }
    if (!ctx->d->isStreamActive("s1")) {
        FAIL("stream not active after subscribe");
        a.close(); b.close(); destroyCtx(ctx);
        return;
    }

    // 帧 0：IDR，A 收到
    auto f0 = makeFrame(fec_protocol::frame_type::VIDEO_IDR, 1000, 3);
    std::vector<uint8_t> p0 = f0.data;
    ctx->d->dispatchFrame("s1", std::move(f0));
    std::string err;
    if (!expectData(a, 1, kVideoIdr, kFlagKey, 0, p0, "A frame0", err)) {
        FAIL(err); a.close(); b.close(); destroyCtx(ctx);
        return;
    }

    // B 加入 → 双方都收到帧 1
    if (!b.sendSub("s1")) { FAIL("B SUB send failed"); a.close(); b.close(); destroyCtx(ctx); return; }
    if (!b.waitMessage(raw, 2000)) { FAIL("B no SUB_OK"); a.close(); b.close(); destroyCtx(ctx); return; }
    if (!parseMessage(raw.data(), static_cast<uint32_t>(raw.size()), m) ||
        !parseSubOk(m, okv) || okv.sid != 1) {
        FAIL("B bad SUB_OK");
        a.close(); b.close(); destroyCtx(ctx);
        return;
    }
    auto f1 = makeFrame(fec_protocol::frame_type::VIDEO, 800, 5);
    std::vector<uint8_t> p1 = f1.data;
    ctx->d->dispatchFrame("s1", std::move(f1));
    if (!expectData(a, 1, kVideo, 0, 1, p1, "A frame1", err) ||
        !expectData(b, 1, kVideo, 0, 1, p1, "B frame1", err)) {
        FAIL(err); a.close(); b.close(); destroyCtx(ctx);
        return;
    }

    // A 退订 → 帧 2 仅 B 收到
    if (!a.sendUnsub(1)) { FAIL("UNSUB send failed"); a.close(); b.close(); destroyCtx(ctx); return; }
    pump(30);
    auto f2 = makeFrame(fec_protocol::frame_type::VIDEO, 700, 9);
    std::vector<uint8_t> p2 = f2.data;
    ctx->d->dispatchFrame("s1", std::move(f2));
    if (!expectData(b, 1, kVideo, 0, 2, p2, "B frame2", err)) {
        FAIL(err); a.close(); b.close(); destroyCtx(ctx);
        return;
    }
    {
        std::vector<uint8_t> extra;
        if (a.waitMessage(extra, 150)) {
            FAIL("A received after UNSUB");
            a.close(); b.close(); destroyCtx(ctx);
            return;
        }
    }
    if (!ctx->d->isStreamActive("s1")) {
        FAIL("stream inactive while B subscribed");
        a.close(); b.close(); destroyCtx(ctx);
        return;
    }

    // B 断连 → 流失活
    b.close();
    pump(50);
    if (ctx->d->isStreamActive("s1")) {
        FAIL("stream still active after B disconnect");
        a.close(); destroyCtx(ctx);
        return;
    }
    if (ctx->d->getStats().frames_no_subscriber == 0) {
        // 至少断连后此前的 dispatch 都已成功；这里不强断言
    }
    a.close();
    destroyCtx(ctx);
    PASS();
}

// ── Test 3: 背压（慢消费者丢非关键帧、保关键帧）─────────────────────────────

static void test_backpressure() {
    TEST("Backpressure drop non-key, keep keyframe");

    sfu::FrameDispatcher::Options opts;
    opts.conn_high_watermark = 256 * 1024;
    opts.conn_hard_limit = 4 * 1024 * 1024;
    auto ctx = makeCtx(3, opts);
    if (!ctx) { FAIL("dispatcher start failed"); return; }

    Client slow;
    if (!slow.connect(ctx->path)) { FAIL("client connect failed"); destroyCtx(ctx); return; }
    pump(30);

    std::vector<uint8_t> raw;
    if (!slow.sendSub("bp") || !slow.waitMessage(raw, 2000)) {
        FAIL("SUB/SUB_OK failed"); slow.close(); destroyCtx(ctx); return;
    }

    // 慢消费者不读：连续派发 30 个 64KB 非关键帧 → 应触发背压丢弃
    for (int i = 0; i < 30; ++i) {
        auto f = makeFrame(fec_protocol::frame_type::VIDEO, 64 * 1024, static_cast<uint8_t>(i));
        ctx->d->dispatchFrame("bp", std::move(f));
    }
    // 随后一个关键帧（IDR）必须能排入
    auto key = makeFrame(fec_protocol::frame_type::VIDEO_IDR, 32 * 1024, 0xEE);
    ctx->d->dispatchFrame("bp", std::move(key));

    if (ctx->d->getStats().frames_dropped_backpressure == 0) {
        FAIL("no backpressure drops observed");
        slow.close(); destroyCtx(ctx);
        return;
    }

    // 开始消费：排空直到读到关键帧（seq 未知，凡是 kFlagKey 即命中）
    bool seen_key = false;
    for (int i = 0; i < 100 && !seen_key; ++i) {
        if (!slow.waitMessage(raw, 2000)) break;
        Message m; DataView dv;
        if (parseMessage(raw.data(), static_cast<uint32_t>(raw.size()), m) &&
            parseData(m, dv) && (dv.flags & kFlagKey)) {
            seen_key = true;
        }
    }
    if (!seen_key) {
        FAIL("keyframe not delivered after drain");
        slow.close(); destroyCtx(ctx);
        return;
    }
    std::cout << "(bp_drops=" << ctx->d->getStats().frames_dropped_backpressure << ") ";

    slow.close();
    destroyCtx(ctx);
    PASS();
}

// ── Test 4: 断连重连 ─────────────────────────────────────────────────────────

static void test_reconnect() {
    TEST("Disconnect and reconnect, stable sid");

    auto ctx = makeCtx(4);
    if (!ctx) { FAIL("dispatcher start failed"); return; }

    std::vector<uint8_t> raw;
    Message m; SubOkView okv;

    Client c1;
    if (!c1.connect(ctx->path)) { FAIL("connect failed"); destroyCtx(ctx); return; }
    pump(30);
    if (!c1.sendSub("rc") || !c1.waitMessage(raw, 2000) ||
        !parseMessage(raw.data(), static_cast<uint32_t>(raw.size()), m) ||
        !parseSubOk(m, okv) || okv.sid != 1) {
        FAIL("first SUB failed"); c1.close(); destroyCtx(ctx); return;
    }
    c1.close();
    pump(50);
    if (ctx->d->isStreamActive("rc")) {
        FAIL("stream active after disconnect");
        destroyCtx(ctx);
        return;
    }

    // 重连：sid 稳定（仍为 1），帧正常送达
    Client c2;
    if (!c2.connect(ctx->path)) { FAIL("reconnect failed"); destroyCtx(ctx); return; }
    pump(30);
    if (!c2.sendSub("rc") || !c2.waitMessage(raw, 2000) ||
        !parseMessage(raw.data(), static_cast<uint32_t>(raw.size()), m) ||
        !parseSubOk(m, okv) || okv.sid != 1) {
        FAIL("re-SUB sid not stable");
        c2.close(); destroyCtx(ctx);
        return;
    }
    auto f = makeFrame(fec_protocol::frame_type::VIDEO_IDR, 500, 1);
    std::vector<uint8_t> pf = f.data;
    ctx->d->dispatchFrame("rc", std::move(f));
    std::string err;
    if (!expectData(c2, 1, kVideoIdr, kFlagKey, 0, pf, "reconnect frame", err)) {
        FAIL(err); c2.close(); destroyCtx(ctx);
        return;
    }
    c2.close();
    destroyCtx(ctx);
    PASS();
}

// ── Test 5: PLI 节流 ─────────────────────────────────────────────────────────

static void test_pli_throttle() {
    TEST("PLI forward and throttle");

    sfu::FrameDispatcher::Options opts;
    opts.pli_min_interval_ms = 120;
    auto ctx = makeCtx(5, opts);
    if (!ctx) { FAIL("dispatcher start failed"); return; }

    Client c;
    if (!c.connect(ctx->path)) { FAIL("connect failed"); destroyCtx(ctx); return; }
    pump(30);
    std::vector<uint8_t> raw;
    Message m; SubOkView okv;
    if (!c.sendSub("pli") || !c.waitMessage(raw, 2000) ||
        !parseMessage(raw.data(), static_cast<uint32_t>(raw.size()), m) ||
        !parseSubOk(m, okv)) {
        FAIL("SUB failed"); c.close(); destroyCtx(ctx); return;
    }
    uint32_t sid = okv.sid;

    // 第一次：转发
    if (!c.sendPli(sid)) { FAIL("PLI send failed"); c.close(); destroyCtx(ctx); return; }
    pump(50);
    if (ctx->pli_calls != 1 || ctx->d->getStats().pli_forwarded != 1) {
        FAIL("first PLI not forwarded");
        c.close(); destroyCtx(ctx);
        return;
    }

    // 窗口内第二次：节流
    if (!c.sendPli(sid)) { FAIL("PLI send failed"); c.close(); destroyCtx(ctx); return; }
    pump(50);
    if (ctx->pli_calls != 1 || ctx->d->getStats().pli_throttled != 1) {
        FAIL("second PLI not throttled");
        c.close(); destroyCtx(ctx);
        return;
    }

    // 超窗口：再次转发
    pump(120);
    if (!c.sendPli(sid)) { FAIL("PLI send failed"); c.close(); destroyCtx(ctx); return; }
    pump(50);
    if (ctx->pli_calls != 2 || ctx->d->getStats().pli_forwarded != 2) {
        FAIL("PLI not forwarded after throttle window");
        c.close(); destroyCtx(ctx);
        return;
    }

    c.close();
    destroyCtx(ctx);
    PASS();
}

// ── Test 6: SUB_ERR（订阅数上限）─────────────────────────────────────────────

static void test_sub_limit() {
    TEST("Sub error on per-conn limit");

    sfu::FrameDispatcher::Options opts;
    opts.max_subs_per_conn = 2;
    auto ctx = makeCtx(6, opts);
    if (!ctx) { FAIL("dispatcher start failed"); return; }

    Client c;
    if (!c.connect(ctx->path)) { FAIL("connect failed"); destroyCtx(ctx); return; }
    pump(30);

    std::vector<uint8_t> raw;
    Message m;
    if (!c.sendSub("sa") || !c.waitMessage(raw, 2000)) {
        FAIL("SUB sa failed"); c.close(); destroyCtx(ctx); return;
    }
    if (!c.sendSub("sb") || !c.waitMessage(raw, 2000)) {
        FAIL("SUB sb failed"); c.close(); destroyCtx(ctx); return;
    }
    // 第 3 个流：超限 → SUB_ERR
    if (!c.sendSub("sc") || !c.waitMessage(raw, 2000) ||
        !parseMessage(raw.data(), static_cast<uint32_t>(raw.size()), m)) {
        FAIL("SUB sc no reply"); c.close(); destroyCtx(ctx); return;
    }
    SubErrView ekv;
    if (!parseSubErr(m, ekv) || ekv.reason != kErrTooManyStreams ||
        ekv.name_len != 2 || std::memcmp(ekv.name, "sc", 2) != 0) {
        FAIL("expected SUB_ERR too_many_streams");
        c.close(); destroyCtx(ctx);
        return;
    }

    c.close();
    destroyCtx(ctx);
    PASS();
}

int main() {
    std::cout << "=== FrameDispatcher / FrameIpc Tests ===" << std::endl;

    test_protocol_roundtrip();
    test_subscribe_deliver_unsubscribe();
    test_backpressure();
    test_reconnect();
    test_pli_throttle();
    test_sub_limit();

    std::cout << "\n" << tests_passed << "/" << tests_run << " tests passed"
              << std::endl;
    return (tests_passed == tests_run) ? 0 : 1;
}