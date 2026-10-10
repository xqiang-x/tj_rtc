// SPDX-License-Identifier: BSD-2-Clause
// jetson_camera_push —— Jetson Nano 专用推流 demo（摄像头采集 + H.264 硬编 + RTC 推流）
//
// 管线：
//   /dev/video0（CSI IMX219，Argus sensor-id 0）
//     → nvarguscamerasrc（ISP，NV12/NVMM）→ nvvidconv → nvv4l2h264enc（NVENC 硬编）
//     → h264parse → appsink → NaluSplitter → Publisher → SFU
//
// 构建（nano 需 cmake ≥ 3.14，构建方法见 README）：
//   cmake -S demos/jetson_camera_push -B demos/jetson_camera_push/build
//   cmake --build demos/jetson_camera_push/build -j2
// 运行：
//   ./jetson_camera_push                # 默认推 jetsonvideo → 106.15.177.248:9200
//   ./jetson_camera_push --tcp          # 走 TCP 媒体通道
//
// 注意：Encoder 的 idrinterval 仅 NULL/READY 可设，无法运行时强制 IDR；
// PLI（关键帧请求）由 GOP（--gop 帧 ≈ 1s）自然满足，与 pi4_air 行为一致。

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "Publisher.h"

#include "GstH264Source.h"
#include "NaluSplitter.h"

namespace {

std::atomic<bool> g_stop{false};

void SignalHandler(int sig) {
    std::printf("\n[Main] 收到信号 %d，正在停止...\n", sig);
    g_stop.store(true);
}

struct Options {
    // SFU
    std::string server = "106.15.177.248";
    int server_port = 9200;
    std::string stream_id = "jetsonvideo";
    std::string user_id = "jetson_nano";
    bool use_udp = true;          // 默认 UDP（与 pi4_air / 手机端一致）
    uint64_t pacer_mbps = 20;
    // 视频
    int sensor_id = 0;            // Argus 传感器（0 = /dev/video0 的 IMX219）
    int cap_width = 1920;         // 采集模式（1920x1080@30 / 1280x720@60 等）
    int cap_height = 1080;
    int cap_fps = 30;
    int width = 1280;             // 编码尺寸
    int height = 720;
    int bitrate_kbps = 2500;
    int gop = 30;                 // IDR 间隔（帧）
    // 杂项
    int seconds = 0;              // 0 = 无限
    bool verbose = false;
};

void PrintUsage(const char* prog) {
    std::fprintf(stderr,
                 "用法: %s [options]\n"
                 " SFU:\n"
                 "  --server IP       SFU 服务器 (默认 106.15.177.248)\n"
                 "  --port  PORT      SFU 端口 (默认 9200)\n"
                 "  --tcp             使用 TCP 推流 (默认 UDP)\n"
                 "  --stream  ID      流名 (默认 jetsonvideo)\n"
                 "  --user  ID        用户 ID (默认 jetson_nano)\n"
                 "  --bandwidth MBPS  UDP Pacer 带宽 Mbps (默认 20)\n"
                 " 视频:\n"
                 "  --sensor-id N     Argus 传感器编号 (默认 0)\n"
                 "  --cap-w W         采集宽 (默认 1920)\n"
                 "  --cap-h H         采集高 (默认 1080)\n"
                 "  --cap-fps N       采集帧率 (默认 30)\n"
                 "  --width  W        编码宽 (默认 1280)\n"
                 "  --height H        编码高 (默认 720)\n"
                 "  --bitrate KBPS    编码码率 kbps (默认 2500)\n"
                 "  --gop  N          IDR 间隔帧数 (默认 30)\n"
                 " 杂项:\n"
                 "  --seconds N       运行秒数, 0=无限 (默认 0)\n"
                 "  --verbose         打印每帧统计\n"
                 "  -h, --help        显示本帮助\n",
                 prog);
}

bool ParseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "缺少 %s 的参数\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            PrintUsage(argv[0]);
            return false;
        } else if (a == "--server") {
            auto v = need(a.c_str()); if (v == nullptr) return false;
            opt.server = v;
        } else if (a == "--port") {
            auto v = need(a.c_str()); if (v == nullptr) return false;
            opt.server_port = std::atoi(v);
        } else if (a == "--tcp") {
            opt.use_udp = false;
        } else if (a == "--stream") {
            auto v = need(a.c_str()); if (v == nullptr) return false;
            opt.stream_id = v;
        } else if (a == "--user") {
            auto v = need(a.c_str()); if (v == nullptr) return false;
            opt.user_id = v;
        } else if (a == "--bandwidth") {
            auto v = need(a.c_str()); if (v == nullptr) return false;
            opt.pacer_mbps = std::strtoull(v, nullptr, 10);
        } else if (a == "--sensor-id") {
            auto v = need(a.c_str()); if (v == nullptr) return false;
            opt.sensor_id = std::atoi(v);
        } else if (a == "--cap-w") {
            auto v = need(a.c_str()); if (v == nullptr) return false;
            opt.cap_width = std::atoi(v);
        } else if (a == "--cap-h") {
            auto v = need(a.c_str()); if (v == nullptr) return false;
            opt.cap_height = std::atoi(v);
        } else if (a == "--cap-fps") {
            auto v = need(a.c_str()); if (v == nullptr) return false;
            opt.cap_fps = std::atoi(v);
        } else if (a == "--width") {
            auto v = need(a.c_str()); if (v == nullptr) return false;
            opt.width = std::atoi(v);
        } else if (a == "--height") {
            auto v = need(a.c_str()); if (v == nullptr) return false;
            opt.height = std::atoi(v);
        } else if (a == "--bitrate") {
            auto v = need(a.c_str()); if (v == nullptr) return false;
            opt.bitrate_kbps = std::atoi(v);
        } else if (a == "--gop") {
            auto v = need(a.c_str()); if (v == nullptr) return false;
            opt.gop = std::atoi(v);
        } else if (a == "--seconds") {
            auto v = need(a.c_str()); if (v == nullptr) return false;
            opt.seconds = std::atoi(v);
        } else if (a == "--verbose") {
            opt.verbose = true;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", a.c_str());
            PrintUsage(argv[0]);
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    gst_init(&argc, &argv);  // 必须：否则 gst_element_factory_make 断言失败

    Options opt;
    if (!ParseArgs(argc, argv, opt)) return 1;

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    signal(SIGPIPE, SIG_IGN);

    std::printf("========================================\n");
    std::printf("  Jetson Camera Push Demo (H.264 硬编)\n");
    std::printf("  Server : %s:%d (%s)\n", opt.server.c_str(), opt.server_port,
                opt.use_udp ? "UDP" : "TCP");
    std::printf("  Stream : %s\n", opt.stream_id.c_str());
    std::printf("  User   : %s\n", opt.user_id.c_str());
    std::printf("  Camera : Argus sensor-%d, 采集 %dx%d@%d\n", opt.sensor_id,
                opt.cap_width, opt.cap_height, opt.cap_fps);
    std::printf("  Encode : nvv4l2h264enc %dx%d %d kbps gop=%d\n", opt.width,
                opt.height, opt.bitrate_kbps, opt.gop);
    std::printf("========================================\n");

    // ── SDK Publisher ───────────────────────────────────────
    std::atomic<bool> connected{false};
    std::atomic<uint64_t> n_pli{0};

    push::Publisher pub;
    pub.SetBandwidth(opt.pacer_mbps * 1'000'000ULL);
    pub.SetOnConnected([&connected](uint32_t sid) {
        connected.store(true);
        std::printf("[push] CONNECTED sessionId=%u\n", sid);
        std::fflush(stdout);
    });
    pub.SetOnDisconnected([&connected] {
        connected.store(false);
        std::printf("[push] DISCONNECTED (SDK 将自动重连)\n");
        std::fflush(stdout);
    });
    // 编码器无运行时 force-idr：仅计数，IDR 由 GOP 周期自然满足
    pub.SetOnKeyFrameRequest([&n_pli] { n_pli.fetch_add(1); });

    if (!pub.Start(opt.server, opt.server_port, opt.stream_id, opt.user_id,
                   opt.use_udp, 0)) {
        std::fprintf(stderr, "[Main] Publisher Start 失败\n");
        return 1;
    }

    // ── 视频管线：Argus 采集 → NVENC 硬编 → NAL 拆分 → Publisher ──
    std::atomic<uint64_t> n_au{0};
    std::atomic<uint64_t> n_key{0};
    std::atomic<uint64_t> n_param{0};
    std::atomic<uint64_t> n_bytes{0};

    NaluSplitter splitter;
    splitter.SetOnNalu([&](NaluSplitter::Kind kind, const uint8_t* p, std::size_t n) {
        const bool is_param = (kind == NaluSplitter::Kind::kParams);
        const bool is_key = (kind == NaluSplitter::Kind::kIdr);
        n_au.fetch_add(1);
        n_bytes.fetch_add(n);
        if (is_key) n_key.fetch_add(1);
        if (is_param) n_param.fetch_add(1);
        if (opt.verbose) {
            std::printf("[vid] AU #%llu %zuB param=%d key=%d\n",
                        static_cast<unsigned long long>(n_au.load()), n,
                        is_param ? 1 : 0, is_key ? 1 : 0);
        }
        pub.PushVideoFrame(p, n, is_param, is_key);
    });

    GstH264Source source;
    source.SetOnAu([&](const uint8_t* data, std::size_t size) {
        splitter.Process(data, size);
    });

    GstH264Source::Config cfg;
    cfg.sensor_id = opt.sensor_id;
    cfg.cap_width = opt.cap_width;
    cfg.cap_height = opt.cap_height;
    cfg.cap_fps = opt.cap_fps;
    cfg.enc_width = opt.width;
    cfg.enc_height = opt.height;
    cfg.bitrate_kbps = opt.bitrate_kbps;
    cfg.gop = opt.gop;
    if (!source.Start(cfg)) {
        std::fprintf(stderr, "[Main] 视频管线启动失败\n");
        pub.Stop();
        return 1;
    }
    std::printf("[Main] 推流中... 按 Ctrl+C 停止\n\n");
    std::fflush(stdout);

    // ── 主循环：1s 统计 / 退出判定 ──────────────────────────
    const bool has_deadline = opt.seconds > 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(opt.seconds);
    auto last_print = std::chrono::steady_clock::now();
    uint64_t last_au = 0;
    uint64_t last_bytes = 0;
    uint64_t last_pli = 0;

    while (!g_stop.load() && source.IsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (has_deadline && std::chrono::steady_clock::now() >= deadline) break;

        const uint64_t pli = n_pli.load();
        if (pli != last_pli) {
            std::printf("[Main] 收到 PLI 关键帧请求 (累计 %llu) —— 由 GOP 周期(%.1fs)自然出 IDR\n",
                        static_cast<unsigned long long>(pli),
                        static_cast<double>(opt.gop) / opt.cap_fps);
            std::fflush(stdout);
            last_pli = pli;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_print >= std::chrono::seconds(1)) {
            const double dt = std::chrono::duration<double>(now - last_print).count();
            const uint64_t au = n_au.load();
            const uint64_t bytes = n_bytes.load();
            std::printf("[Main] 1s: au=%llu(+%llu) fps=%.1f key=%llu param=%llu "
                        "kbps=%.0f pli=%llu pub=%s\n",
                        static_cast<unsigned long long>(au),
                        static_cast<unsigned long long>(au - last_au),
                        static_cast<double>(au - last_au) / dt,
                        static_cast<unsigned long long>(n_key.load()),
                        static_cast<unsigned long long>(n_param.load()),
                        static_cast<double>(bytes - last_bytes) * 8.0 / dt / 1000.0,
                        static_cast<unsigned long long>(pli),
                        connected.load() ? "OK" : "等连接");
            std::fflush(stdout);
            last_print = now;
            last_au = au;
            last_bytes = bytes;
        }
    }

    if (!source.IsRunning() && !g_stop.load()) {
        std::fprintf(stderr, "[Main] 视频管线已停止（相机/编码器错误）\n");
    }

    // ── 收尾 ────────────────────────────────────────────────
    std::printf("[Main] 正在停止...\n");
    source.Stop();
    pub.Stop();
    std::printf("[Main] 结束: au=%llu key=%llu param=%llu bytes=%llu pli=%llu\n",
                static_cast<unsigned long long>(n_au.load()),
                static_cast<unsigned long long>(n_key.load()),
                static_cast<unsigned long long>(n_param.load()),
                static_cast<unsigned long long>(n_bytes.load()),
                static_cast<unsigned long long>(n_pli.load()));
    return 0;
}