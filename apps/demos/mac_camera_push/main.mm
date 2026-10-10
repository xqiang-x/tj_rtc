// main.mm
// Mac 摄像头采集 → H.264 编码 → SFU 推流 Demo
//
// 用法：
//   ./mac_camera_push [server_ip] [port] [stream_id] [user_id] [width] [height] [fps] [bitrate_kbps]
//   ./mac_camera_push --server 127.0.0.1 --port 9200 --stream stream1 --user publisher1
//                     --width 1280 --height 720 --fps 30 --bitrate 2000
//                     --bandwidth 50  (UDP 推流带宽 Mbps，默认 50，TCP 模式无效)
//                     --udp           (使用 UDP 模式)
//
// 默认值：
//   server_ip    = 127.0.0.1
//   port         = 9200
//   stream_id    = stream1
//   user_id      = publisher1
//   width        = 1280
//   height       = 720
//   fps          = 30
//   bitrate_kbps = 2000

#import <Foundation/Foundation.h>
#include <csignal>
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "MacCameraCapture.h"
#include "H264Encoder.h"
#include "Publisher.h"
#include "SimulatedAudioSource.h"

// ─────────────────────────────────────────────────────────────
// 全局停止标志（信号处理）
// ─────────────────────────────────────────────────────────────
static std::atomic_bool g_stop{false};

static void SignalHandler(int sig) {
    std::cout << "\n[Main] 收到信号 " << sig << "，正在停止..." << std::endl;
    g_stop.store(true);
}

// ─────────────────────────────────────────────────────────────
// 主函数
// ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    // ── 解析命令行参数 ────────────────────────────────────────
    std::string serverIp   = "127.0.0.1";
    int         port       = 9200;
    std::string streamId   = "stream1";
    std::string userId     = "publisher1";
    int         width      = 1280;
    int         height     = 720;
    int         fps        = 30;
    int         bitrateKbps = 2000;
    bool        useUdp     = false;
    int         udpPort    = 0;
    uint64_t    bandwidthMbps = 50;  // UDP 推流带宽（Mbps），TCP 模式无效

    // 先解析 --xxx 参数（避免位置参数把 --xxx 当成值）
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "--server"   && i+1 < argc) serverIp    = argv[++i];
        else if (arg == "--port"     && i+1 < argc) port        = std::atoi(argv[++i]);
        else if (arg == "--stream"   && i+1 < argc) streamId    = argv[++i];
        else if (arg == "--user"     && i+1 < argc) userId      = argv[++i];
        else if (arg == "--width"    && i+1 < argc) width       = std::atoi(argv[++i]);
        else if (arg == "--height"   && i+1 < argc) height      = std::atoi(argv[++i]);
        else if (arg == "--fps"      && i+1 < argc) fps         = std::atoi(argv[++i]);
        else if (arg == "--bitrate"  && i+1 < argc) bitrateKbps = std::atoi(argv[++i]);
        else if (arg == "--udp")                      useUdp     = true;
        else if (arg == "--udp-port" && i+1 < argc)  udpPort    = std::atoi(argv[++i]);
        else if (arg == "--bandwidth"&& i+1 < argc)  bandwidthMbps = std::atoi(argv[++i]);
        else if (arg == "-h" || arg == "--help")     {
            std::cout << "\n用法：" << std::endl;
            std::cout << "  " << argv[0] << " [server_ip] [port] [stream_id] [user_id] [width] [height] [fps] [bitrate_kbps]" << std::endl;
            std::cout << "  " << argv[0] << " --server 127.0.0.1 --port 9200 --stream stream1 --user publisher1" << std::endl;
            std::cout << "                     --width 1280 --height 720 --fps 30 --bitrate 2000" << std::endl;
            std::cout << "                     --bandwidth 50 --udp" << std::endl;
            std::cout << "\n参数说明：" << std::endl;
            std::cout << "  位置参数：" << std::endl;
            std::cout << "    server_ip      服务器地址 (默认: 127.0.0.1)" << std::endl;
            std::cout << "    port           服务器端口 (默认: 9200)" << std::endl;
            std::cout << "    stream_id      流名 (默认: stream1)" << std::endl;
            std::cout << "    user_id        用户ID (默认: publisher1)" << std::endl;
            std::cout << "    width          视频宽度 (默认: 1280)" << std::endl;
            std::cout << "    height         视频高度 (默认: 720)" << std::endl;
            std::cout << "    fps            帧率 (默认: 30)" << std::endl;
            std::cout << "    bitrate_kbps   比特率 kbps (默认: 2000)" << std::endl;
            std::cout << "\n  选项参数：" << std::endl;
            std::cout << "    --server IP    服务器地址" << std::endl;
            std::cout << "    --port PORT    服务器端口" << std::endl;
            std::cout << "    --stream STREAM  流名" << std::endl;
            std::cout << "    --user USER    用户ID" << std::endl;
            std::cout << "    --width W      视频宽度" << std::endl;
            std::cout << "    --height H     视频高度" << std::endl;
            std::cout << "    --fps FPS      帧率" << std::endl;
            std::cout << "    --bitrate Kbps 比特率 (kbps)" << std::endl;
            std::cout << "    --udp          使用 UDP 模式 (默认 TCP)" << std::endl;
            std::cout << "    --udp-port P   UDP 端口 (默认: port+1)" << std::endl;
            std::cout << "    --bandwidth M  UDP 推流带宽 Mbps (默认: 50, TCP 模式无效)" << std::endl;
            std::cout << "    -h, --help     显示此帮助信息" << std::endl;
            std::cout << "\n示例：" << std::endl;
            std::cout << "  # TCP 模式推流到本地服务器" << std::endl;
            std::cout << "  " << argv[0] << " 127.0.0.1 9200 stream1 publisher1" << std::endl;
            std::cout << "\n  # UDP 模式推流到远程服务器" << std::endl;
            std::cout << "  " << argv[0] << " --server 106.15.177.248 --port 9200 --stream stream1 --user publisher1 --udp --bandwidth 50" << std::endl;
            std::cout << "\n  # 自定义分辨率和帧率" << std::endl;
            std::cout << "  " << argv[0] << " --server 106.15.177.248 --width 1920 --height 1080 --fps 30 --bitrate 4000 --udp" << std::endl;
            std::cout << std::endl;
            return 0;
        }
    }

    // 再解析位置参数（跳过已识别的 --xxx）
    std::vector<bool> used(argc, false);
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg[0] == '-') { used[i] = true; continue; }  // 跳过 --xxx
        if (i-1 >= 0 && argv[i-1][0] == '-') { used[i] = true; continue; }  // 跳过参数值（含最后一个参数：原 i+1<argc 条件会让末位选项值漏判）
        if (!used[i]) {
            // 位置参数：serverIp port streamId userId width height fps bitrate
            static int posIdx = 0;
            switch (posIdx++) {
                case 0: serverIp    = argv[i]; break;
                case 1: port        = std::atoi(argv[i]); break;
                case 2: streamId    = argv[i]; break;
                case 3: userId      = argv[i]; break;
                case 4: width       = std::atoi(argv[i]); break;
                case 5: height      = std::atoi(argv[i]); break;
                case 6: fps         = std::atoi(argv[i]); break;
                case 7: bitrateKbps = std::atoi(argv[i]); break;
            }
        }
    }

    std::cout << "========================================" << std::endl;
    std::cout << "  Mac Camera Push Demo" << std::endl;
    std::cout << "  Server : " << serverIp << ":" << port << std::endl;
    std::cout << "  Stream : " << streamId   << std::endl;
    std::cout << "  User   : " << userId   << std::endl;
    std::cout << "  Video  : " << width << "x" << height
              << " @" << fps << "fps  " << bitrateKbps << "kbps" << std::endl;
    std::cout << "  Pacer  : " << bandwidthMbps << " Mbps (UDP only)" << std::endl;
    std::cout << "  Audio  : 48kHz Stereo PCM (simulated)" << std::endl;
    int actualUdpPort = (udpPort > 0) ? udpPort : port + 1;
    std::cout << "  UDP    : " << (useUdp ? "ON (port " + std::to_string(actualUdpPort) + ")" : "OFF") << std::endl;
    std::cout << "========================================" << std::endl;

    // ── 信号处理 ──────────────────────────────────────────────
    signal(SIGINT,  SignalHandler);
    signal(SIGTERM, SignalHandler);
    signal(SIGPIPE, SIG_IGN);  // 忽略 broken pipe

    // ── 列出可用摄像头 ────────────────────────────────────────
    auto devices = camera::MacCameraCapture::ListDevices();
    std::cout << "\n[Main] 可用摄像头：" << std::endl;
    for (int i = 0; i < (int)devices.size(); ++i) {
        std::cout << "  [" << i << "] " << devices[i] << std::endl;
    }
    if (devices.empty()) {
        std::cerr << "[Main] 未检测到摄像头，退出" << std::endl;
        return 1;
    }

    // ── 创建组件 ──────────────────────────────────────────────
    camera::MacCameraCapture capture;
    h264::H264Encoder         encoder;
    push::Publisher       pushClient;
    audio::SimulatedAudioSource audioSource(48000, 2);  // 48kHz 双声道

    // ── 配置推流客户端回调 ────────────────────────────────────
    pushClient.SetOnConnected([](uint32_t sessionId) {
        std::cout << "[Main] 推流连接成功，sessionId=" << sessionId << std::endl;
    });
    pushClient.SetOnDisconnected([]() {
        std::cout << "[Main] 推流连接已断开" << std::endl;
        g_stop.store(true);
    });
    
    // 设置推流带宽
    pushClient.SetBandwidth(bandwidthMbps * 1'000'000);

    // ── 配置编码器输出回调 → 推流 ─────────────────────────────
    encoder.SetPacketCallback([&pushClient](const h264::EncodedPacket& pkt) {
        // 此回调在 VideoToolbox 内部线程调用
        // PushVideoFrame 内部是线程安全的（mutex + queue）
        pushClient.PushVideoFrame(
            pkt.data, pkt.size,
            pkt.isParamSet, pkt.isKeyFrame);
    });

    // ── 配置摄像头帧回调 → 编码 ──────────────────────────────
    capture.SetFrameCallback([&encoder](CVPixelBufferRef pixelBuffer, CMTime pts) {
        // 此回调在 AVFoundation 采集线程调用（串行队列）
        encoder.EncodeFrame(pixelBuffer, pts);
    });

    // ── 初始化 H.264 编码器 ───────────────────────────────────
    if (!encoder.Init(width, height, fps, bitrateKbps)) {
        std::cerr << "[Main] H.264 编码器初始化失败" << std::endl;
        return 1;
    }

    // ── 启动推流客户端（后台网络线程） ───────────────────────
    if (!pushClient.Start(serverIp, port, streamId, userId, useUdp, udpPort)) {
        std::cerr << "[Main] 推流客户端启动失败" << std::endl;
        return 1;
    }

    // ── 等待网络连接建立（最多 10 秒） ───────────────────────
    std::cout << "[Main] 等待连接服务器..." << std::endl;
    for (int i = 0; i < 100 && !g_stop.load(); ++i) {
        if (pushClient.IsRunning()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 连接建立后再启动摄像头，避免数据发送到未准备好的连接
    // 等待 sessionId 分配完成（最多 5 秒）
    std::cout << "[Main] 等待订阅握手..." << std::endl;
    auto waitStart = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - waitStart).count();
        if (elapsed > 5) {
            std::cerr << "[Main] 订阅超时，摄像头仍将启动（帧将被丢弃直到握手完成）" << std::endl;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // PushVideoFrame 内部会在 sessionId==0 时丢弃帧，
        // 等握手完成后 sessionId 会被设置，不需要额外同步
        if (!pushClient.IsRunning()) {
            std::cerr << "[Main] 连接失败，退出" << std::endl;
            encoder.Release();
            return 1;
        }
    }

    // ── 启动摄像头采集 ────────────────────────────────────────
    if (!capture.Start(width, height, fps, 0)) {
        std::cerr << "[Main] 摄像头启动失败" << std::endl;
        pushClient.Stop();
        encoder.Release();
        return 1;
    }

    // 编码器分辨率与实际摄像头分辨率可能不同，重新初始化
    int actualW = capture.GetWidth();
    int actualH = capture.GetHeight();
    if (actualW != width || actualH != height) {
        std::cout << "[Main] 摄像头实际分辨率 " << actualW << "x" << actualH
                  << "，重新初始化编码器..." << std::endl;
        encoder.Release();
        if (!encoder.Init(actualW, actualH, fps, bitrateKbps)) {
            std::cerr << "[Main] 重新初始化编码器失败" << std::endl;
            capture.Stop();
            pushClient.Stop();
            return 1;
        }
    }

    std::cout << "\n[Main] 推流中... 按 Ctrl+C 停止\n" << std::endl;

    // ── 启动音频发送线程 ──────────────────────────────────────
    std::thread audioThread([&pushClient, &audioSource]() {
        std::cout << "[Audio] 音频线程启动" << std::endl;
        
        // 每 20ms 发送一帧音频（48000/50 = 960 frames）
        auto frameInterval = std::chrono::milliseconds(20);
        auto nextFrameTime = std::chrono::steady_clock::now();
        
        while (!::g_stop.load()) {
            // 生成音频帧
            auto audioData = audioSource.GeneratePacket();
            
            // 推送到发送队列
            pushClient.PushAudioFrame(audioData.data(), audioData.size());
            
            // 等待下一帧
            nextFrameTime += frameInterval;
            std::this_thread::sleep_until(nextFrameTime);
        }
        
        std::cout << "[Audio] 音频线程退出" << std::endl;
    });

    // ── 主循环：打印统计信息 ──────────────────────────────────
    uint64_t loopCount = 0;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ++loopCount;

        // 每 5 秒强制一个关键帧（应对新加入的订阅者）
        if (loopCount % 5 == 0) {
            encoder.ForceKeyFrame();
        }

        // 每 10 秒打印运行状态
        if (loopCount % 10 == 0) {
            std::cout << "[Main] 已运行 " << loopCount << " 秒"
                      << "  推流=" << (pushClient.IsRunning() ? "OK" : "断开")
                      << "  摄像头=" << (capture.IsRunning() ? "OK" : "停止")
                      << std::endl;
        }
    }

    // ── 清理 ──────────────────────────────────────────────────
    std::cout << "[Main] 正在清理..." << std::endl;
    capture.Stop();
    pushClient.Stop();
    encoder.Release();
    
    // 等待音频线程退出
    if (audioThread.joinable()) {
        audioThread.join();
    }

    std::cout << "[Main] 退出" << std::endl;
    return 0;
}
