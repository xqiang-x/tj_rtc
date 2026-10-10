// mac_pull_test — SFU 拉流 + H.264 解码 + SDL2 实时显示
//
// 用法：
//   ./mac_pull_test [server_ip] [port] [stream_id] [user_id]
//   ./mac_pull_test --server 127.0.0.1 --port 9200 --stream stream1 --user subscriber1
//                     --udp  (使用 UDP 模式)
//
// 默认值：
//   server_ip  = 127.0.0.1
//   port       = 9200
//   stream_id  = room1
//   user_id    = subscriber1

#include "Subscriber.h"
#include "H264Decoder.h"
#include "SdlRenderer.h"
#include "SfuFrameType.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <string>
#include <vector>

static std::atomic<bool>     g_running{true};
static std::atomic<int>      g_frameCount{0};
static std::atomic<uint64_t> g_totalBytes{0};
static std::atomic<int>      g_videoFrames{0};
static std::atomic<int>      g_audioFrames{0};

// 解码前原始码流落盘（调试用）：--dump-h264 <path> 时启用
static FILE* g_dumpFile = nullptr;

void SignalHandler(int /*sig*/) {
    g_running.store(false);
    // 推一个 SDL_QUIT 事件唤醒主循环
    SDL_Event quit; quit.type = SDL_QUIT;
    SDL_PushEvent(&quit);
}

void PrintUsage(const char* prog) {
    std::cout << "\n用法：" << std::endl;
    std::cout << "  " << prog << " [server_ip] [port] [stream_id] [user_id]" << std::endl;
    std::cout << "  " << prog << " --server <ip> --port <p> --stream <s> --user <u>" << std::endl;
    std::cout << "\n参数说明：" << std::endl;
    std::cout << "  位置参数：" << std::endl;
    std::cout << "    server_ip    服务器地址 (默认: 127.0.0.1)" << std::endl;
    std::cout << "    port         服务器端口 (默认: 9200)" << std::endl;
    std::cout << "    stream_id     流名 (默认: room1)" << std::endl;
    std::cout << "    user_id      用户ID (默认: subscriber1)" << std::endl;
    std::cout << "\n  选项参数：" << std::endl;
    std::cout << "    --server IP  服务器地址" << std::endl;
    std::cout << "    --port PORT  服务器端口" << std::endl;
    std::cout << "    --stream STREAM  流名" << std::endl;
    std::cout << "    --user USER  用户ID" << std::endl;
    std::cout << "    --udp        使用 UDP 模式 (默认 TCP)" << std::endl;
    std::cout << "    --udp-port P UDP 端口 (默认: port+1)" << std::endl;
    std::cout << "    -h, --help   显示此帮助信息" << std::endl;
    std::cout << "    --dump-h264 <path>  将收到的视频帧原始码流写盘（调试用）" << std::endl;
    std::cout << "\n示例：" << std::endl;
    std::cout << "  # TCP 模式拉流本地服务器" << std::endl;
    std::cout << "  " << prog << " 127.0.0.1 9200 stream1 subscriber1" << std::endl;
    std::cout << "\n  # UDP 模式拉流远程服务器" << std::endl;
    std::cout << "  " << prog << " --server 106.15.177.248 --port 9200 --stream stream1 --user subscriber1 --udp" << std::endl;
    std::cout << "\n  # 简洁模式" << std::endl;
    std::cout << "  " << prog << " 106.15.177.248 9200 stream1 sub1" << std::endl;
    std::cout << "\n操作说明：" << std::endl;
    std::cout << "  按 ESC 或 Q 键关闭窗口退出" << std::endl;
    std::cout << "  按 Ctrl+C 强制退出" << std::endl;
    std::cout << std::endl;
}

int main(int argc, char* argv[]) {
    std::string serverIp   = "127.0.0.1";
    int         serverPort = 9200;
    std::string streamId   = "room1";
    std::string userId     = "subscriber1";
    bool        useUdp     = false;
    int         udpPort    = 0;  // 0 → port+1

    // 位置参数（兼容旧调用方式）
    if (argc >= 2 && argv[1][0] != '-') serverIp   = argv[1];
    if (argc >= 3 && argv[2][0] != '-') serverPort = std::stoi(argv[2]);
    if (argc >= 4 && argv[3][0] != '-') streamId    = argv[3];
    if (argc >= 5 && argv[4][0] != '-') userId     = argv[4];

    // --xxx 参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "--server" && i+1 < argc) serverIp   = argv[++i];
        else if (arg == "--port"   && i+1 < argc) serverPort = std::stoi(argv[++i]);
        else if (arg == "--stream" && i+1 < argc) streamId    = argv[++i];
        else if (arg == "--user"   && i+1 < argc) userId     = argv[++i];
        else if (arg == "--udp")                   useUdp     = true;
        else if (arg == "--udp-port" && i+1 < argc) udpPort   = std::stoi(argv[++i]);
        else if (arg == "--dump-h264" && i+1 < argc) {
            g_dumpFile = fopen(argv[++i], "wb");
            if (!g_dumpFile) std::cerr << "[Main] 无法打开 dump 文件: " << argv[i] << std::endl;
        }
        else if (arg == "-h" || arg == "--help")  { PrintUsage(argv[0]); return 0; }
    }

    // ── SDL 必须在主线程最早初始化 ────────────────────────────────────────────
    SdlRenderer renderer;
    if (!renderer.Init("SFU Pull — H.264 Live (" + streamId + ")")) {
        std::cerr << "[Main] SDL 初始化失败" << std::endl;
        return 1;
    }

    // ── VideoToolbox H.264 解码器 ─────────────────────────────────────────────
    H264Decoder decoder;
    decoder.SetFrameCallback([&](CVImageBufferRef buf, int w, int h) {
        renderer.EnqueueFrame(buf, w, h);
    });

    // ── 信号处理（SDL 已初始化后才能 SDL_PushEvent） ──────────────────────────
    signal(SIGINT,  SignalHandler);
    signal(SIGTERM, SignalHandler);

    std::cout << "========================================\n"
              << "  Mac Pull Test — H.264 Decoder + SDL2\n"
              << "========================================\n"
              << "  Server : " << serverIp << ":" << serverPort << "\n"
              << "  Stream : " << streamId   << "\n"
              << "  User   : " << userId   << "\n"
              << "  按 ESC / Q 关闭窗口退出\n"
              << "========================================" << std::endl;

    // ── 创建拉流客户端 ────────────────────────────────────────────────────────
    pull::Subscriber client;

    client.SetOnConnected([](uint32_t sessionId) {
        std::cout << "[Main] 拉流连接成功，sessionId=" << sessionId << std::endl;
    });
    client.SetOnDisconnected([&]() {
        std::cout << "[Main] 拉流连接断开" << std::endl;
        g_running.store(false);
        SDL_Event quit; quit.type = SDL_QUIT;
        SDL_PushEvent(&quit);
    });
    client.SetOnFrame([&](uint32_t sourceSessionId, std::vector<uint8_t>&& data) {
        int frameNum = ++g_frameCount;
        g_totalBytes += data.size();
        
        if (data.empty()) return;
        
        // 解析 frameType
        auto frameType = static_cast<sfu::FrameType>(data[0]);
        
        if (sfu::IsAudioFrame(frameType)) {
            // 音频帧（PCM 16-bit）
            ++g_audioFrames;
            // TODO: 音频处理（解码/播放）
            // 目前只统计，不处理
            if (g_audioFrames.load() % 500 == 0) {
                std::cout << "[Audio] 已接收 " << g_audioFrames.load() << " 帧 "
                          << sfu::FrameTypeName(frameType)
                          << " 大小=" << (data.size() - 1) << " bytes" << std::endl;
            }
        } else if (sfu::IsVideoFrame(frameType)) {
            // 视频帧（H.264）
            ++g_videoFrames;
            // 调试：原始码流落盘
            if (g_dumpFile) {
                fwrite(data.data() + 1, 1, data.size() - 1, g_dumpFile);
            }
            // 在网络线程解码；VideoToolbox 同步回调 → EnqueueFrame（加锁）
            decoder.FeedPayload(data.data(), data.size());
        } else {
            std::cerr << "[Main] 未知帧类型: 0x" << std::hex << (int)data[0] << std::dec << std::endl;
        }

        if (frameNum % 150 == 0) {
            std::cout << "[Main] 已接收 " << frameNum << " 帧"
                      << " (视频=" << g_videoFrames.load() 
                      << ", 音频=" << g_audioFrames.load() << ")"
                      << "  总字节=" << g_totalBytes.load()
                      << "  src=" << sourceSessionId << std::endl;
        }
    });

    if (!client.Start(serverIp, serverPort, streamId, userId, useUdp, udpPort)) {
        std::cerr << "[Main] 启动拉流客户端失败" << std::endl;
        return 1;
    }

    std::cout << "[Main] 等待连接..." << std::endl;

    // ── 主循环（SDL 事件 + 渲染，必须在主线程运行） ──────────────────────────
    auto lastStat = std::chrono::steady_clock::now();
    int  elapsed  = 0;

    while (g_running.load() && client.IsRunning()) {
        if (!renderer.PollAndRender()) {
            g_running.store(false);
            break;
        }
        // 启用了 VSYNC，无帧时主动让出 1ms 避免空转
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastStat).count() >= 10) {
            elapsed += 10;
            std::cout << "[Main] 已运行 " << elapsed << " 秒"
                      << "  解码帧=" << g_frameCount.load()
                      << "  总字节=" << g_totalBytes.load() << std::endl;
            lastStat = now;
        }
    }

    std::cout << "\n[Main] 停止..." << std::endl;
    client.Stop();
    if (g_dumpFile) { fclose(g_dumpFile); g_dumpFile = nullptr; }
    std::cout << "[Main] 最终统计: 总帧数=" << g_frameCount.load()
              << " (视频=" << g_videoFrames.load()
              << ", 音频=" << g_audioFrames.load() << ")"
              << ", 总字节=" << g_totalBytes.load() << std::endl;
    return 0;
}
