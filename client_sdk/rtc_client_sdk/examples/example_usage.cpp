// example_usage.cpp
// RTC Client SDK 使用示例 - 流模型下的发布/订阅混合测试
// 同一进程内启动一个 Publisher（推一路模拟音视频流）+ 一个 Subscriber（拉回该流）
// 用法:
//   rtc_sdk_example [server_ip] [port] [stream_id]
//   默认: 127.0.0.1 9200 demo_stream (UDP 模式)

#include "Publisher.h"
#include "Subscriber.h"
#include "SfuFrameType.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cmath>

static std::atomic<bool> g_running{true};

void SignalHandler(int /*sig*/) {
    g_running.store(false);
}

int main(int argc, char* argv[]) {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    std::string serverIp = (argc > 1) ? argv[1] : "127.0.0.1";
    int         port     = (argc > 2) ? std::atoi(argv[2]) : 9200;
    std::string streamId = (argc > 3) ? argv[3] : "demo_stream";

    std::cout << "========================================\n"
              << "  RTC Client SDK - 发布/订阅混合测试\n"
              << "========================================\n"
              << "  音频: 50fps (20ms/帧) PCM 正弦波\n"
              << "  视频: 25fps (40ms/帧) 模拟 H.264\n"
              << "    关键帧 ~50-80KB, P帧 ~5-15KB\n"
              << "  流名: " << streamId << " (UDP 模式)\n"
              << "  按 Ctrl+C 退出\n"
              << "========================================\n" << std::endl;

    // ── 发布者（推流端）：注册并发布一条流 ──────────────────
    push::Publisher pub;
    pub.SetOnConnected([](uint32_t sessionId) {
        std::cout << "[Example] 推流连接成功，sessionId=" << sessionId << std::endl;
    });
    pub.SetOnDisconnected([]() {
        std::cout << "[Example] 推流连接断开" << std::endl;
    });
    // PLI 关键帧请求：回调在 UDP 接收线程触发，实际使用中应只置标志，
    // 由编码线程在下一帧强制输出 IDR（PushVideoFrame 时 isParamSet/isKeyFrame=true）
    pub.SetOnKeyFrameRequest([]() {
        std::cout << "[Example] 收到 PLI：请求编码关键帧" << std::endl;
    });

    // ── 订阅者（拉流端）：订阅同一流名 ──────────────────────
    pull::Subscriber sub;
    sub.SetOnConnected([](uint32_t sessionId) {
        std::cout << "[Example] 拉流连接成功，sessionId=" << sessionId << std::endl;
    });
    sub.SetOnDisconnected([]() {
        std::cout << "[Example] 拉流连接断开" << std::endl;
    });
    sub.SetOnFrame([](uint32_t sourceSessionId, std::vector<uint8_t>&& data) {
        static int videoCount = 0;
        static int audioCount = 0;
        if (data.empty()) return;
        // 首字节为 FrameType（0x01/0x02/0x03 视频，0x10 音频）
        auto ft = static_cast<sfu::FrameType>(data[0]);
        if (sfu::IsAudioFrame(ft)) {
            audioCount++;
            if (audioCount % 200 == 0) {
                std::cout << "[Example] 收到音频帧 #" << audioCount
                          << " size=" << data.size() << " bytes" << std::endl;
            }
            return;
        }
        videoCount++;
        if (videoCount <= 3 || videoCount % 50 == 0) {
            const char* frameType = "UNKNOWN";
            switch (ft) {
                case sfu::FrameType::VIDEO_PARAMS: frameType = "SPS/PPS"; break;
                case sfu::FrameType::VIDEO_IDR:    frameType = "IDR";     break;
                case sfu::FrameType::VIDEO_P:      frameType = "P-frame"; break;
                default: break;
            }
            std::cout << "[Example] 收到视频帧 #" << videoCount
                      << " type=" << frameType
                      << " size=" << data.size() << " bytes" << std::endl;
        }
    });

    std::cout << "\n[Example] 启动 Publisher..." << std::endl;
    if (!pub.Start(serverIp, port, streamId, "sdk_pub", true, 0)) {
        std::cerr << "[Example] Publisher 启动失败" << std::endl;
        return 1;
    }
    std::cout << "[Example] 启动 Subscriber..." << std::endl;
    if (!sub.Start(serverIp, port, streamId, "sdk_sub", true, 0)) {
        std::cerr << "[Example] Subscriber 启动失败" << std::endl;
        return 1;
    }

    std::cout << "[Example] 运行中... 按 Ctrl+C 停止\n" << std::endl;

    // ── 模拟音视频推流：视频 25fps，音频 50fps（20ms/帧 PCM） ──
    std::vector<uint8_t> paramsNal = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1E};
    std::vector<uint8_t> idrNal    = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84};
    std::vector<uint8_t> pNal      = {0x00, 0x00, 0x00, 0x01, 0x41, 0x9A};
    auto makeVideo = [&](size_t size, bool isParamSet, bool isKeyFrame) {
        std::vector<uint8_t> frame = isParamSet ? paramsNal : (isKeyFrame ? idrNal : pNal);
        frame.resize(size, 0xAB);
        pub.PushVideoFrame(frame.data(), frame.size(), isParamSet, isKeyFrame);
    };

    const size_t audioFrameSize = 48000 * 2 * 2 / 50;  // 48kHz 双声道 20ms = 3840B
    std::vector<int16_t> pcm(audioFrameSize / 2);
    auto mediaStart = std::chrono::steady_clock::now();
    uint64_t videoIdx = 0, audioIdx = 0;
    while (g_running.load()) {
        auto now = std::chrono::steady_clock::now();
        auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(now - mediaStart).count();

        // 视频：40ms 一帧；第 1 帧发 SPS/PPS + IDR，之后每 60 帧一个 IDR
        while (elapsedUs >= static_cast<int64_t>((videoIdx + 1) * 40'000LL)) {
            bool isIdr = (videoIdx % 60 == 0);
            bool isParams = (videoIdx == 0);
            size_t size = isIdr ? 60'000 : 10'000;
            if (isParams) size = 100;
            makeVideo(size, isParams, isIdr);
            videoIdx++;
        }

        // 音频：20ms 一帧 PCM 正弦波
        while (elapsedUs >= static_cast<int64_t>((audioIdx + 1) * 20'000LL)) {
            for (size_t i = 0; i < pcm.size(); ++i) {
                pcm[i] = static_cast<int16_t>(std::sin(2.0 * 3.14159265 * 440.0 * audioIdx * 20.0 / 1000.0) * 4000);
            }
            pub.PushAudioFrame(reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * 2);
            audioIdx++;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::cout << "\n[Example] 停止 Subscriber..." << std::endl;
    sub.Stop();
    std::cout << "[Example] 停止 Publisher..." << std::endl;
    pub.Stop();

    std::cout << "[Example] 退出" << std::endl;
    return 0;
}