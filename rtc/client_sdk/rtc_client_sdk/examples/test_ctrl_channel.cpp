// test_ctrl_channel.cpp
// 遥控信令通道测试：Pub（车端）和 Sub（遥控器端）
// 用法：
//   test_ctrl_channel pub <server_ip> <port>     # Pub 端（车）
//   test_ctrl_channel sub <server_ip> <port>     # Sub 端（遥控器）

#include "Publisher.h"
#include "Subscriber.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <atomic>
#include <signal.h>

static std::atomic<bool> g_running{true};

void SignalHandler(int) {
    g_running.store(false);
}

// ========== Pub 端（车）==========
void RunPub(const std::string& serverIp, int port) {
    push::Publisher pub;
    pub.SetOnConnected([](uint32_t sessionId) {
        std::cout << "[Pub] 已连接, sessionId=" << sessionId << std::endl;
    });

    // 接收控制信令（来自 Sub 的遥控指令）
    pub.SetOnControlData([](uint32_t srcSession, const uint8_t* data, size_t len) {
        std::string msg(reinterpret_cast<const char*>(data), len);
        std::cout << "[Pub] 收到信令 from session=" << srcSession
                  << " len=" << len << " data=\"" << msg << "\"" << std::endl;
    });

    if (!pub.Start(serverIp, port, "ctrl_test", "car_01", true, port + 1)) {
        std::cerr << "[Pub] 推流启动失败" << std::endl;
        return;
    }

    std::cout << "[Pub] 车端启动，开始广播状态 + 接收遥控指令" << std::endl;

    int reliableSeq = 0;
    int unreliableSeq = 0;

    while (g_running.load()) {
        // 每 2 秒发送一次可靠信令（电量状态）
        if (++reliableSeq % 20 == 0) {
            char buf[64];
            int n = snprintf(buf, sizeof(buf), "BATTERY:%d%%", 85 - (reliableSeq / 20) % 30);
            pub.SendCtrlDownstream(reinterpret_cast<const uint8_t*>(buf), n, true);
            std::cout << "[Pub] 发送可靠信令: " << buf << std::endl;
        }

        // 每 100ms 发送一次不可靠信令（姿态数据）
        {
            char buf[64];
            int n = snprintf(buf, sizeof(buf), "IMU:%d:%d:%d",
                             unreliableSeq % 360, (unreliableSeq * 7) % 360, (unreliableSeq * 13) % 360);
            pub.SendCtrlDownstream(reinterpret_cast<const uint8_t*>(buf), n, false);
        }
        unreliableSeq++;

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    pub.Stop();
}

// ========== Sub 端（遥控器）==========
void RunSub(const std::string& serverIp, int port) {
    pull::Subscriber sub;
    sub.SetOnConnected([](uint32_t sessionId) {
        std::cout << "[Sub] 已连接, sessionId=" << sessionId << std::endl;
    });

    // 接收车的状态广播
    sub.SetOnControlData([](uint32_t srcSession, const uint8_t* data, size_t len) {
        std::string msg(reinterpret_cast<const char*>(data), len);
        // 区分可靠/不可靠：以 BATTERY: 开头的是可靠信令，IMU: 开头的是不可靠
        if (msg.rfind("BATTERY:", 0) == 0) {
            std::cout << "[Sub] 可靠信令 from car: " << msg << std::endl;
        } else if (msg.rfind("IMU:", 0) == 0) {
            // 不可靠信令高频，每 10 条打印一次
            static int imuCount = 0;
            if (++imuCount % 10 == 0) {
                std::cout << "[Sub] 不可靠信令 #" << imuCount << ": " << msg << std::endl;
            }
        } else {
            std::cout << "[Sub] 信令 from session=" << srcSession
                      << " len=" << len << " data=\"" << msg << "\"" << std::endl;
        }
    });

    if (!sub.Start(serverIp, port, "ctrl_test", "remote_01", true, port + 1)) {
        std::cerr << "[Sub] 拉流启动失败" << std::endl;
        return;
    }

    std::cout << "[Sub] 遥控器启动，开始发送控制指令 + 接收车的状态" << std::endl;

    // 模拟遥控指令
    const char* commands[] = {
        "FORWARD:80",    // 前进 80%
        "TURN:15",       // 左转 15度
        "FORWARD:100",   // 全速前进
        "TURN:-20",      // 右转 20度
        "STOP",          // 停止
        "BACKWARD:50",   // 后退 50%
    };
    int cmdIdx = 0;

    int seq = 0;
    while (g_running.load()) {
        // 每 3 秒发送一条可靠控制指令
        if (++seq % 30 == 0) {
            const char* cmd = commands[cmdIdx % 6];
            sub.SendCtrlUpstream(reinterpret_cast<const uint8_t*>(cmd), strlen(cmd), true);
            std::cout << "[Sub] 发送可靠指令: " << cmd << std::endl;
            cmdIdx++;
        }

        // 每 50ms 发送不可靠摇杆数据
        {
            char buf[64];
            int throttle = 50 + (seq % 50);
            int steering = (seq % 100) - 50;
            int n = snprintf(buf, sizeof(buf), "JOY:%d:%d", throttle, steering);
            sub.SendCtrlUpstream(reinterpret_cast<const uint8_t*>(buf), n, false);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    sub.Stop();
}

// ========== main ==========
int main(int argc, char* argv[]) {
    signal(SIGINT, SignalHandler);

    if (argc < 4) {
        std::cerr << "用法: " << argv[0] << " <pub|sub> <server_ip> <port>" << std::endl;
        std::cerr << "  pub  - 车端（发布视频 + 广播状态 + 接收遥控指令）" << std::endl;
        std::cerr << "  sub  - 遥控器端（接收视频 + 发送遥控指令 + 接收状态）" << std::endl;
        return 1;
    }

    std::string mode = argv[1];
    std::string serverIp = argv[2];
    int port = std::stoi(argv[3]);

    std::cout << "=== 遥控信令通道测试 ===" << std::endl;
    std::cout << "模式: " << mode << ", 服务器: " << serverIp << ":" << port << std::endl;

    if (mode == "pub") {
        RunPub(serverIp, port);
    } else if (mode == "sub") {
        RunSub(serverIp, port);
    } else {
        std::cerr << "未知模式: " << mode << " (使用 pub 或 sub)" << std::endl;
        return 1;
    }

    return 0;
}