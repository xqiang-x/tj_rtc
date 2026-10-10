// SPDX-License-Identifier: BSD-2-Clause
// main.cpp - 树莓派视频采集 → H.264 编码 → rtc_client_sdk 推流
//
// 流水线：
//   LibcameraVideoSource ─[YUV420 DMABUF]→ H264V4l2Encoder
//                         ─[H.264 ES]    → NaluSplitter
//                         ─[NAL 单元]    → push::Publisher::PushVideoFrame
//
// 模仿 rpicam_vid 的极简版（去掉 preview / post-processing / 多输出 / options 解析）。

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "Publisher.h"

#include "H264V4l2Encoder.h"
#include "LibcameraVideoSource.h"
#include "NaluSplitter.h"

namespace {

std::atomic<bool> g_running{true};

void HandleSignal(int /*sig*/) {
    g_running.store(false);
}

struct AppOptions {
    // 视频参数
    unsigned int width = 1280;
    unsigned int height = 720;
    unsigned int fps = 30;
    unsigned int bitrate_bps = 2'000'000;
    unsigned int gop = 30;
    // 运行时长（秒），0 = 无限
    unsigned int duration_sec = 0;

    // 服务器
    std::string server_ip = "127.0.0.1";
    int server_port = 9200;
    bool use_udp = true;
    std::string stream_id = "pi_stream1";
    std::string user_id = "pi_pusher";
    uint64_t pacer_mbps = 50;
};

void PrintUsage(const char *prog) {
    std::fprintf(stderr,
                 "Usage: %s [options]\n"
                 "  --width W           视频宽 (默认 1280)\n"
                 "  --height H          视频高 (默认 720)\n"
                 "  --fps  N            帧率 (默认 30)\n"
                 "  --bitrate KBPS      码率 kbps (默认 2000)\n"
                 "  --gop  N            GOP 长度 (默认 30)\n"
                 "  --duration SEC      运行秒数, 0=无限 (默认 0)\n"
                 "  --server IP         SFU 服务器 IP (默认 127.0.0.1)\n"
                 "  --port  PORT        SFU 端口 (默认 9200)\n"
                 "  --tcp               使用 TCP 推流 (默认 UDP)\n"
                 "  --stream  ID          流名 (默认 pi_stream1)\n"
                 "  --user  ID          用户 ID (默认 pi_pusher)\n"
                 "  --pacer MBPS        UDP Pacer 带宽 Mbps (默认 50)\n"
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
        else if (a == "--bitrate")  { auto v = need(a.c_str()); if (!v) return false; opt.bitrate_bps = std::stoul(v) * 1000; }
        else if (a == "--gop")      { auto v = need(a.c_str()); if (!v) return false; opt.gop = std::stoul(v); }
        else if (a == "--duration") { auto v = need(a.c_str()); if (!v) return false; opt.duration_sec = std::stoul(v); }
        else if (a == "--server")   { auto v = need(a.c_str()); if (!v) return false; opt.server_ip = v; }
        else if (a == "--port")     { auto v = need(a.c_str()); if (!v) return false; opt.server_port = std::stoi(v); }
        else if (a == "--tcp")      { opt.use_udp = false; }
        else if (a == "--stream")  { auto v = need(a.c_str()); if (!v) return false; opt.stream_id = v; }
        else if (a == "--user")     { auto v = need(a.c_str()); if (!v) return false; opt.user_id = v; }
        else if (a == "--pacer")    { auto v = need(a.c_str()); if (!v) return false; opt.pacer_mbps = std::stoull(v); }
        else {
            std::fprintf(stderr, "未知参数: %s\n", a.c_str());
            PrintUsage(argv[0]);
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    AppOptions opt;
    if (!ParseArgs(argc, argv, opt)) return 1;

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    std::signal(SIGPIPE, SIG_IGN);  // SDK 内部 socket 写入断连时不被杀掉

    std::cout << "============================================\n"
              << " Simple Raspberry Pi H.264 Pusher\n"
              << "============================================\n"
              << " resolution : " << opt.width << "x" << opt.height << "\n"
              << " fps        : " << opt.fps << "\n"
              << " bitrate    : " << opt.bitrate_bps / 1000 << " kbps\n"
              << " GOP        : " << opt.gop << "\n"
              << " server     : " << opt.server_ip << ":" << opt.server_port
              << (opt.use_udp ? " (UDP)" : " (TCP)") << "\n"
              << " 流名/user  : " << opt.stream_id << " / " << opt.user_id << "\n"
              << "============================================\n";

    // ── 1. 初始化 SDK（发布者：以流名注册并发布一路流） ───────
    push::Publisher pub;
    pub.SetBandwidth(opt.pacer_mbps * 1'000'000);
    pub.SetOnConnected([](uint32_t sid) {
        std::cout << "[SDK] connected sessionId=" << sid << std::endl;
    });
    pub.SetOnDisconnected([]() {
        std::cout << "[SDK] disconnected" << std::endl;
    });

    if (!pub.Start(opt.server_ip, opt.server_port, opt.stream_id, opt.user_id, opt.use_udp, 0)) {
        std::cerr << "SDK Publisher Start 失败" << std::endl;
        return 1;
    }

    // ── 2. 启动 H.264 编码器 ────────────────────────────────
    H264V4l2Encoder encoder;
    NaluSplitter splitter;

    std::atomic<uint64_t> total_frames{0};
    std::atomic<uint64_t> total_bytes{0};

    splitter.SetOnNalu([&](NaluSplitter::Kind kind, const uint8_t *p, std::size_t n) {
        bool is_param = (kind == NaluSplitter::Kind::kParams);
        bool is_key = (kind == NaluSplitter::Kind::kIdr);
        pub.PushVideoFrame(p, n, is_param, is_key);
        total_bytes.fetch_add(n);
    });

    encoder.SetOnOutput([&](const uint8_t *data, std::size_t size,
                            int64_t /*ts_us*/, bool /*keyframe*/) {
        // 一个 V4L2 输出 buffer 内可能含 SPS+PPS+IDR 等多个 NAL，需要拆分
        splitter.Process(data, size);
        total_frames.fetch_add(1);
    });

    // ── 3. 启动相机采集 ─────────────────────────────────────
    LibcameraVideoSource source;
    source.SetOnFrame([&](const LibcameraVideoSource::Frame &f) {
        if (!encoder.EncodeBuffer(f.dmabuf_fd, f.size, f.timestamp_us)) {
            // 编码器输入队列满，丢弃
        }
    });

    LibcameraVideoSource::Config src_cfg;
    src_cfg.width = opt.width;
    src_cfg.height = opt.height;
    src_cfg.fps = opt.fps;
    src_cfg.buffer_count = 6;
    if (!source.Start(src_cfg)) {
        std::cerr << "相机启动失败" << std::endl;
        pub.Stop();
        return 1;
    }

    H264V4l2Encoder::Config enc_cfg;
    enc_cfg.width = source.Width();
    enc_cfg.height = source.Height();
    enc_cfg.stride = source.Stride();
    enc_cfg.fps = opt.fps;
    enc_cfg.bitrate_bps = opt.bitrate_bps;
    enc_cfg.intra_period = opt.gop;
    enc_cfg.rec709_colorspace = (opt.height >= 720);
    if (!encoder.Start(enc_cfg)) {
        std::cerr << "编码器启动失败" << std::endl;
        source.Stop();
        pub.Stop();
        return 1;
    }

    // ── 4. 主循环：等待终止 ─────────────────────────────────
    auto t0 = std::chrono::steady_clock::now();
    auto last_log = t0;
    uint64_t last_frames = 0;
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto now = std::chrono::steady_clock::now();
        if (opt.duration_sec > 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >=
                opt.duration_sec) {
            break;
        }
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log).count() >= 5) {
            uint64_t f = total_frames.load();
            uint64_t bytes = total_bytes.load();
            double sec = std::chrono::duration<double>(now - last_log).count();
            double fps = (f - last_frames) / sec;
            std::cout << "[stat] frames=" << f << " fps=" << fps
                      << " total_bytes=" << bytes << std::endl;
            last_log = now;
            last_frames = f;
        }
    }

    std::cout << "正在停止..." << std::endl;
    encoder.Stop();
    source.Stop();
    pub.Stop();
    std::cout << "退出" << std::endl;
    return 0;
}
