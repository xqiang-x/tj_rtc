// sim_pusher - 本地生成模拟画面（飘动格子）→ x264 编码 → rtc_client_sdk 推流
//
// 用于在无相机的机器上向 SFU 推一路可解码的测试流。

#include <cstdint>

#include <x264.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "Publisher.h"

#include "NaluSplitter.h"

namespace {

std::atomic<bool> g_running{true};

void HandleSignal(int) { g_running.store(false); }

struct AppOptions {
    unsigned width = 1280;
    unsigned height = 720;
    unsigned fps = 30;
    unsigned bitrate_kbps = 1000;
    unsigned gop = 30;
    unsigned duration_sec = 0;

    std::string server_ip = "47.96.23.146";
    int server_port = 9200;
    bool use_udp = true;
    std::string stream_id = "stream1";
    std::string user_id = "sim_pusher";
    uint64_t pacer_mbps = 50;
    std::string dump_h264_path;  // 调试：AU 流（NaluSplitter 聚合后）落盘
    std::string dump_raw_path;   // 调试：x264 原始 NAL 流落盘（拼接前）
};

void PrintUsage(const char *prog) {
    std::fprintf(stderr,
                 "Usage: %s [options]\n"
                 "  --width W           视频宽 (默认 1280)\n"
                 "  --height H          视频高 (默认 720)\n"
                 "  --fps  N            帧率 (默认 30)\n"
                 "  --bitrate KBPS      码率 kbps (默认 1000)\n"
                 "  --gop  N            GOP 长度 (默认 30)\n"
                 "  --duration SEC      运行秒数, 0=无限 (默认 0)\n"
                 "  --server IP         SFU 服务器 IP (默认 47.96.23.146)\n"
                 "  --port  PORT        SFU 端口 (默认 9200)\n"
                 "  --tcp               使用 TCP 推流 (默认 UDP)\n"
                 "  --stream  ID          流名 (默认 stream1)\n"
                 "  --user  ID          用户 ID (默认 sim_pusher)\n"
                 "  --dump-h264 PATH     AU 流落盘（NaluSplitter 聚合后，调试用）\n"
                 "  --dump-raw PATH      x264 原始 NAL 流落盘（拼接前，调试用）\n"
                 "  -h, --help          显示本帮助\n",
                 prog);
}

bool ParseArgs(int argc, char **argv, AppOptions &opt) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "缺少 %s 的参数\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { PrintUsage(argv[0]); return false; }
        else if (a == "--width")    { auto v = need(a.c_str()); if (!v) return false; opt.width = std::stoul(v); }
        else if (a == "--height")   { auto v = need(a.c_str()); if (!v) return false; opt.height = std::stoul(v); }
        else if (a == "--fps")      { auto v = need(a.c_str()); if (!v) return false; opt.fps = std::stoul(v); }
        else if (a == "--bitrate")  { auto v = need(a.c_str()); if (!v) return false; opt.bitrate_kbps = std::stoul(v); }
        else if (a == "--gop")      { auto v = need(a.c_str()); if (!v) return false; opt.gop = std::stoul(v); }
        else if (a == "--duration") { auto v = need(a.c_str()); if (!v) return false; opt.duration_sec = std::stoul(v); }
        else if (a == "--server")   { auto v = need(a.c_str()); if (!v) return false; opt.server_ip = v; }
        else if (a == "--port")     { auto v = need(a.c_str()); if (!v) return false; opt.server_port = std::stoi(v); }
        else if (a == "--tcp")      { opt.use_udp = false; }
        else if (a == "--stream")   { auto v = need(a.c_str()); if (!v) return false; opt.stream_id = v; }
        else if (a == "--user")     { auto v = need(a.c_str()); if (!v) return false; opt.user_id = v; }
        else if (a == "--dump-h264") { auto v = need(a.c_str()); if (!v) return false; opt.dump_h264_path = v; }
        else if (a == "--dump-raw") { auto v = need(a.c_str()); if (!v) return false; opt.dump_raw_path = v; }
        else {
            std::fprintf(stderr, "未知参数: %s\n", a.c_str());
            PrintUsage(argv[0]);
            return false;
        }
    }
    return true;
}

// ── 画面生成 ────────────────────────────────────────────────
inline void Rgb2Yuv(int r, int g, int b, uint8_t &y, uint8_t &u, uint8_t &v) {
    y = static_cast<uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
    u = static_cast<uint8_t>(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
    v = static_cast<uint8_t>(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}

struct Square { int r, g, b; double wx, wy, px, py; int half; };

// 生成第 frame 帧 I420 画面：滚动棋盘格 + 三个飘动彩色方块 + 扫描条
void GenerateFrame(uint8_t *y_plane, uint8_t *u_plane, uint8_t *v_plane,
                   unsigned W, unsigned H, uint64_t frame, unsigned fps) {
    double t = static_cast<double>(frame) / fps;
    const unsigned cell = 64;
    unsigned ox = static_cast<unsigned>(t * 40) % (2 * cell);
    unsigned oy = static_cast<unsigned>(t * 25) % (2 * cell);

    Square sq[3] = {
        {220, 40, 40, 0.9, 1.3, 0.0, 0.0, 60},
        {40, 200, 60, 1.1, 0.7, 2.1, 1.0, 45},
        {50, 90, 230, 0.6, 1.7, 4.2, 2.5, 35},
    };
    double bar_x = (t * 120);
    while (bar_x > W + 40) bar_x -= (W + 40);

    for (unsigned y = 0; y < H; ++y) {
        uint8_t *yrow = y_plane + y * W;
        for (unsigned x = 0; x < W; ++x) {
            int r, g, b;
            bool checker = (((x + ox) / cell) + ((y + oy) / cell)) % 2 == 0;
            int base = checker ? 170 : 60;
            r = g = b = base;

            // 飘动方块
            for (auto &s : sq) {
                double cx = W / 2.0 + std::sin(t * s.wx + s.px) * (W / 2.0 - 100);
                double cy = H / 2.0 + std::cos(t * s.wy + s.py) * (H / 2.0 - 90);
                if (std::abs(static_cast<double>(x) - cx) < s.half &&
                    std::abs(static_cast<double>(y) - cy) < s.half) {
                    r = s.r; g = s.g; b = s.b;
                }
            }
            // 竖向扫描条
            double dx = static_cast<double>(x) - bar_x;
            if (dx >= 0 && dx < 10) { r = 240; g = 240; b = 240; }

            uint8_t yy, uu, vv;
            Rgb2Yuv(r, g, b, yy, uu, vv);
            yrow[x] = yy;
            if ((x % 2) == 0 && (y % 2) == 0) {
                u_plane[(y / 2) * (W / 2) + x / 2] = uu;
                v_plane[(y / 2) * (W / 2) + x / 2] = vv;
            }
        }
    }
}

}  // namespace

int main(int argc, char **argv) {
    AppOptions opt;
    if (!ParseArgs(argc, argv, opt)) return 1;

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    std::signal(SIGPIPE, SIG_IGN);

    unsigned W = opt.width & ~1u, H = opt.height & ~1u;

    std::cout << "============================================\n"
              << " Sim H.264 Pusher (x264, 生成画面)\n"
              << "============================================\n"
              << " resolution : " << W << "x" << H << "\n"
              << " fps        : " << opt.fps << "\n"
              << " bitrate    : " << opt.bitrate_kbps << " kbps\n"
              << " GOP        : " << opt.gop << "\n"
              << " server     : " << opt.server_ip << ":" << opt.server_port
              << (opt.use_udp ? " (UDP)" : " (TCP)") << "\n"
              << " 流名/user  : " << opt.stream_id << " / " << opt.user_id << "\n"
              << "============================================\n";

    // ── SDK（发布者：注册并发布一条流） ──────────────────────
    push::Publisher pub;
    pub.SetBandwidth(opt.pacer_mbps * 1'000'000);
    pub.SetOnConnected([](uint32_t sid) {
        std::cout << "[SDK] connected sessionId=" << sid << std::endl;
    });
    pub.SetOnDisconnected([]() { std::cout << "[SDK] disconnected" << std::endl; });
    if (!pub.Start(opt.server_ip, opt.server_port, opt.stream_id, opt.user_id, opt.use_udp, 0)) {
        std::cerr << "SDK Publisher Start 失败" << std::endl;
        return 1;
    }

    // ── x264 ────────────────────────────────────────────────
    x264_param_t param;
    if (x264_param_default_preset(&param, "ultrafast", "zerolatency") < 0) {
        std::cerr << "x264 preset 失败" << std::endl;
        return 1;
    }
    // 传输粒度：完整视频帧（访问单元 AU）为单位：x264 一次 encode 输出的
    // 全部 NAL（关键帧 = SPS+PPS+IDR）拼接为一个 AU 整体推送，NaluSplitter
    // 聚合归类为单一帧。链路已完整支持多 slice（接收端帧内参数集 + slice
    // 合并），但 brew 版 libx264 实测 b_sliced_threads 多 slice 会偶发产出
    // 语法级坏帧（~1/5000 帧，位置随机，ffmpeg 内置同参数 0 错误）；单 slice
    // 零错误，故关闭多 slice，保证码流纯净。
    param.b_sliced_threads = 0;  // brew libx264 多 slice 偶发坏帧（见上）
    param.i_threads = 1;         // 单编码线程，避免线程竞争
    param.i_csp = X264_CSP_I420;
    param.i_width = static_cast<int>(W);
    param.i_height = static_cast<int>(H);
    param.i_fps_num = static_cast<int>(opt.fps);
    param.i_fps_den = 1;
    param.i_keyint_max = static_cast<int>(opt.gop);
    param.b_repeat_headers = 1;  // 每个 IDR 前带 SPS/PPS
    param.b_annexb = 1;
    param.rc.i_rc_method = X264_RC_ABR;
    param.rc.i_bitrate = static_cast<int>(opt.bitrate_kbps);
    if (x264_param_apply_profile(&param, "baseline") < 0) {
        std::cerr << "x264 profile 失败" << std::endl;
        return 1;
    }
    x264_t *enc = x264_encoder_open(&param);
    if (!enc) {
        std::cerr << "x264_encoder_open 失败" << std::endl;
        return 1;
    }

    std::vector<uint8_t> y_buf(W * H), u_buf(W / 2 * H / 2), v_buf(W / 2 * H / 2);

    NaluSplitter splitter;
    std::atomic<uint64_t> total_frames{0}, total_bytes{0};
    // 调试：x264 原始输出落盘（--dump-h264 <path>）
    FILE* dump_file = nullptr;
    if (!opt.dump_h264_path.empty()) {
        dump_file = fopen(opt.dump_h264_path.c_str(), "wb");
        if (!dump_file) std::cerr << "无法打开 dump 文件: " << opt.dump_h264_path << std::endl;
    }
    // 调试：x264 原始 NAL 流落盘（--dump-raw <path>，拼接前逐 NAL 写入）
    FILE* raw_file = nullptr;
    if (!opt.dump_raw_path.empty()) {
        raw_file = fopen(opt.dump_raw_path.c_str(), "wb");
        if (!raw_file) std::cerr << "无法打开 raw dump 文件: " << opt.dump_raw_path << std::endl;
    }
    splitter.SetOnNalu([&](NaluSplitter::Kind kind, const uint8_t *p, std::size_t n) {
        bool is_param = (kind == NaluSplitter::Kind::kParams);
        bool is_key = (kind == NaluSplitter::Kind::kIdr);
        pub.PushVideoFrame(p, n, is_param, is_key);
        total_bytes.fetch_add(n);
        if (dump_file) { fwrite(p, 1, n, dump_file); }
    });

    auto t0 = std::chrono::steady_clock::now();
    auto last_log = t0;
    uint64_t last_frames = 0;
    uint64_t frame_idx = 0;

    x264_picture_t pic_in, pic_out;
    x264_picture_init(&pic_in);
    pic_in.img.i_csp = X264_CSP_I420;
    pic_in.img.i_plane = 3;
    pic_in.img.plane[0] = y_buf.data();
    pic_in.img.plane[1] = u_buf.data();
    pic_in.img.plane[2] = v_buf.data();
    pic_in.img.i_stride[0] = static_cast<int>(W);
    pic_in.img.i_stride[1] = static_cast<int>(W / 2);
    pic_in.img.i_stride[2] = static_cast<int>(W / 2);

    while (g_running.load()) {
        // 按帧率配速
        auto target = t0 + std::chrono::microseconds(1'000'000LL * frame_idx / opt.fps);
        auto now = std::chrono::steady_clock::now();
        if (now < target) std::this_thread::sleep_for(target - now);

        if (opt.duration_sec > 0 && frame_idx >= static_cast<uint64_t>(opt.duration_sec) * opt.fps)
            break;

        GenerateFrame(y_buf.data(), u_buf.data(), v_buf.data(), W, H, frame_idx, opt.fps);
        pic_in.i_pts = static_cast<int64_t>(frame_idx);

        x264_nal_t *nals = nullptr;
        int nnal = 0;
        int ret = x264_encoder_encode(enc, &nals, &nnal, &pic_in, &pic_out);
        if (ret < 0) {
            std::cerr << "x264 encode 失败" << std::endl;
            break;
        }
        // 一次 encode 的全部 NAL 拼接为一个访问单元（一帧），整体交给
        // NaluSplitter 聚合归类后以帧为单位推送；切勿逐 NAL 推帧
        if (nnal > 0) {
            std::vector<uint8_t> au;
            size_t au_size = 0;
            for (int i = 0; i < nnal; ++i) au_size += nals[i].i_payload;
            au.resize(au_size);
            size_t off = 0;
            for (int i = 0; i < nnal; ++i) {
                memcpy(au.data() + off, nals[i].p_payload, nals[i].i_payload);
                off += nals[i].i_payload;
                if (raw_file) { fwrite(nals[i].p_payload, 1, nals[i].i_payload, raw_file); }
            }
            splitter.Process(au.data(), au.size());
        }
        if (ret > 0) total_frames.fetch_add(1);
        ++frame_idx;

        now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log).count() >= 5) {
            uint64_t f = total_frames.load();
            double sec = std::chrono::duration<double>(now - last_log).count();
            std::cout << "[stat] frames=" << f << " fps=" << (f - last_frames) / sec
                      << " total_bytes=" << total_bytes.load() << std::endl;
            last_log = now;
            last_frames = f;
        }
    }

    std::cout << "正在停止..." << std::endl;
    if (dump_file) { fflush(dump_file); fclose(dump_file); }
if (raw_file) { fflush(raw_file); fclose(raw_file); }
    x264_encoder_close(enc);
    pub.Stop();
    std::cout << "退出" << std::endl;
    return 0;
}
