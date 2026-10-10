// pull_probe - 极简 SFU 拉流验证：连接流并打印收到帧统计
// 用法: ./pull_probe [server_ip] [port] [stream_id] [user_id] [duration_s]
//   默认: 127.0.0.1 9200 room1 sub_probe 10

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "Subscriber.h"
#include "SfuFrameType.h"

static std::atomic_bool g_stop{false};
static void SignalHandler(int) { g_stop.store(true); }

int main(int argc, char* argv[]) {
    std::string serverIp = "127.0.0.1";
    int         port     = 9200;
    std::string streamId  = "room1";
    std::string userId   = "sub_probe";
    int         duration = 10;
    bool        useUdp   = false;

    if (argc >= 2) serverIp = argv[1];
    if (argc >= 3) port     = std::atoi(argv[2]);
    if (argc >= 4) streamId  = argv[3];
    if (argc >= 5) userId   = argv[4];
    if (argc >= 6) duration = std::atoi(argv[5]);
    if (argc >= 7) useUdp   = (std::string(argv[6]) == "udp");

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    std::cout << "[Probe] 拉流 " << serverIp << ":" << port
              << " stream=" << streamId << " user=" << userId
              << " 时长=" << duration << "s"
              << " 模式=" << (useUdp ? "UDP" : "TCP") << std::endl;

    pull::Subscriber client;
    uint64_t videoFrames = 0, audioFrames = 0, bytes = 0;
    FILE* dumpFp = nullptr;
    if (const char* dumpPath = std::getenv("PULL_DUMP")) {
        dumpFp = fopen(dumpPath, "wb");
        std::cout << "[Probe] 接收帧 dump → " << dumpPath
                  << (dumpFp ? "" : " (打开失败!)") << std::endl;
    }
    uint64_t idrFrames = 0, paramFrames = 0, tinyFrames = 0;
    auto t0 = std::chrono::steady_clock::now();

    client.SetOnConnected([](uint32_t sessionId) {
        std::cout << "[Probe] 连接成功，sessionId=" << sessionId << std::endl;
    });
    client.SetOnDisconnected([&]() {
        std::cout << "[Probe] 连接断开" << std::endl;
        g_stop.store(true);
    });
    client.SetOnFrame([&](uint32_t, std::vector<uint8_t>&& data) {
        if (data.empty()) return;
        bytes += data.size();
        if (!sfu::IsVideoFrame(static_cast<sfu::FrameType>(data[0]))) {
            if (sfu::IsAudioFrame(static_cast<sfu::FrameType>(data[0]))) ++audioFrames;
            return;
        }
        ++videoFrames;
        uint8_t ft = data[0];
        if (ft == 0x02) ++idrFrames;
        if (ft == 0x01) ++paramFrames;
        if (data.size() < 200 && ft != 0x01) ++tinyFrames;
        if (dumpFp) {
            uint32_t len = static_cast<uint32_t>(data.size() - 1);
            fwrite(&len, 4, 1, dumpFp);
            fwrite(&ft, 1, 1, dumpFp);
            fwrite(data.data() + 1, 1, len, dumpFp);
        }

        // 解析首个 NALU 类型（跳过 type 字节 + 起始码）
        int nalType = -1;
        for (size_t i = 1; i + 3 < data.size(); ++i) {
            if (data[i] == 0 && data[i + 1] == 0 &&
                (data[i + 2] == 1 || (data[i + 2] == 0 && i + 3 < data.size() && data[i + 3] == 1))) {
                size_t off = (data[i + 2] == 1) ? i + 3 : i + 4;
                if (off < data.size()) nalType = data[off] & 0x1F;
                break;
            }
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        std::cout << "[Frame] #" << videoFrames << " t=" << ms << "ms type=0x"
                  << std::hex << (int)ft << std::dec
                  << " size=" << data.size()
                  << " nal=" << nalType << std::endl;
    });

    if (!client.Start(serverIp, port, streamId, userId, useUdp, 0)) {
        std::cerr << "[Probe] 启动拉流失败" << std::endl;
        return 1;
    }

    auto start = std::chrono::steady_clock::now();
    auto lastStatsTime = start;
    auto prevStats = client.GetStats();
    while (!g_stop.load() && client.IsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() >= duration) break;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStatsTime).count() >= 2000) {
            lastStatsTime = now;
            auto s = client.GetStats();
            std::cout << "[Stats] completed=+" << (s.frames_completed - prevStats.frames_completed)
                      << " dropped=+" << (s.frames_dropped - prevStats.frames_dropped)
                      << " fecBlk=+" << (s.blocks_recovered_fec - prevStats.blocks_recovered_fec)
                      << " nackBlk=+" << (s.blocks_recovered_nack - prevStats.blocks_recovered_nack)
                      << " loss=" << (s.loss_rate * 100.0f) << "%"
                      << " total(dropped)=" << s.frames_dropped << std::endl;
            prevStats = s;
        }
    }

    std::cout << "[Probe] 结果: 视频帧=" << videoFrames
              << " (IDR=" << idrFrames << " params=" << paramFrames
              << " 异常小帧=" << tinyFrames << ")"
              << " 音频帧=" << audioFrames
              << " 总字节=" << bytes << std::endl;
    client.Stop();
    if (dumpFp) fclose(dumpFp);
    return 0;
}
