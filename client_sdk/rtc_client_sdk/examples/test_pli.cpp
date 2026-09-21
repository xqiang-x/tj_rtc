// test_pli.cpp
// PLI 关键帧请求测试：Pub 统计收到的 PLI 回调，Sub 手动触发 RequestKeyFrame
// 用法：
//   test_pli pub <server_ip> <port> <duration_s>
//   test_pli sub <server_ip> <port> <count> <interval_ms>

#include "Publisher.h"
#include "Subscriber.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <signal.h>

static std::atomic<bool> g_running{true};
static std::atomic<int> g_pliCount{0};

void SignalHandler(int) {
    g_running.store(false);
}

void RunPub(const std::string& serverIp, int port, int durationS) {
    push::Publisher pub;
    pub.SetOnConnected([](uint32_t sessionId) {
        std::cout << "[Pub] connected, sessionId=" << sessionId << std::endl;
    });

    pub.SetOnKeyFrameRequest([]() {
        int n = ++g_pliCount;
        std::cout << "[Pub] KeyFrameRequest callback #" << n << std::endl;
    });

    if (!pub.Start(serverIp, port, "pli_test", "pli_pub", true, port + 1)) {
        std::cerr << "[Pub] Start failed" << std::endl;
        return;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(durationS);
    while (g_running.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "[Pub] TOTAL PLI callbacks=" << g_pliCount.load() << std::endl;
    pub.Stop();
}

void RunSub(const std::string& serverIp, int port, int count, int intervalMs) {
    pull::Subscriber sub;
    sub.SetOnConnected([](uint32_t sessionId) {
        std::cout << "[Sub] connected, sessionId=" << sessionId << std::endl;
    });

    if (!sub.Start(serverIp, port, "pli_test", "pli_sub", true, port + 1)) {
        std::cerr << "[Sub] Start failed" << std::endl;
        return;
    }

    // 等待建立媒体流（收到推流端 sessionId）
    std::this_thread::sleep_for(std::chrono::seconds(3));

    for (int i = 0; i < count && g_running.load(); ++i) {
        std::cout << "[Sub] RequestKeyFrame #" << (i + 1) << std::endl;
        sub.RequestKeyFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    // 尾部持续拉流一段时间，观察丢帧触发的自动 PLI
    std::cout << "[Sub] listening 20s for auto-PLI on drops..." << std::endl;
    for (int i = 0; i < 200 && g_running.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    sub.Stop();
}

int main(int argc, char* argv[]) {
    signal(SIGINT, SignalHandler);

    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <pub|sub> <server_ip> <port> [args...]" << std::endl;
        std::cerr << "  pub <server_ip> <port> <duration_s>" << std::endl;
        std::cerr << "  sub <server_ip> <port> <count> <interval_ms>" << std::endl;
        return 1;
    }

    std::string mode = argv[1];
    std::string serverIp = argv[2];
    int port = std::stoi(argv[3]);

    if (mode == "pub") {
        RunPub(serverIp, port, argc > 4 ? std::stoi(argv[4]) : 30);
    } else if (mode == "sub") {
        RunSub(serverIp, port, argc > 4 ? std::stoi(argv[4]) : 3,
               argc > 5 ? std::stoi(argv[5]) : 1000);
    } else {
        std::cerr << "Unknown mode: " << mode << std::endl;
        return 1;
    }

    return 0;
}