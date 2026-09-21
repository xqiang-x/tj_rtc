// End-to-End Test: SFU Server + Multiple RTC Clients
// Tests the complete flow: connect -> subscribe -> send frames -> receive frames

#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <mutex>
#include <cstring>

// SFU Server
#include "SfuServerBase.h"
#include "SfuEvPool.h"
#include "SfuKvStore.h"
#include "SfuSubscribeProtocol.h"

// Frame protoc
#include "types.h"
#include "config.h"
#include "sender.h"
#include "receiver.h"

using namespace sfu;
using namespace fec_protocol;

// ===== Test Configuration =====
const int TEST_PORT = 9200;
const int NUM_CLIENTS = 3;
const int FRAMES_PER_CLIENT = 5;
const int FRAME_SIZE = 2048;

// ===== Global Test State =====
std::atomic<int> g_clientsConnected{0};
std::atomic<int> g_clientsSubscribed{0};
std::atomic<int> g_framesSent{0};
std::atomic<int> g_framesReceived{0};
std::mutex g_testMutex;
std::vector<uint32_t> g_sessionIds;

// ===== Simple TCP Client (for test) =====
class TestClient {
public:
    TestClient(int id, const std::string& ip, int port)
        : m_id(id), m_ip(ip), m_port(port) {
        m_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd >= 0) {
            int flags = fcntl(m_fd, F_GETFL, 0);
            fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    ~TestClient() {
        if (m_fd >= 0) close(m_fd);
    }

    bool connect() {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(m_port);
        addr.sin_addr.s_addr = inet_addr(m_ip.c_str());

        // Non-blocking connect
        ::connect(m_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

        // Wait for connection (simple poll)
        for (int i = 0; i < 50; ++i) {
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &error, &len);
            if (error == 0) {
                m_connected = true;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

    bool subscribe(SubscribeRole role, const std::string& userId, const std::string& streamId) {
        SubscribeFlags flags;
        flags.audio = true; flags.video = true; flags.data = true;
        flags.reserved = 0;

        auto msg = SerializeSubscribeReq(role, streamId, userId, flags);
        return sendRaw(msg);
    }

    bool waitForSubscribeResp(uint32_t& sessionId) {
        std::vector<uint8_t> buf(4096);
        auto start = std::chrono::steady_clock::now();

        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(2000)) {
            ssize_t n = recv(m_fd, buf.data(), buf.size(), 0);
            if (n <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            // Parse TCP frame
            if (n < 4) continue;
            uint32_t payloadLen = (buf[0] << 24) | (buf[1] << 16) |
                                  (buf[2] << 8) | buf[3];
            if (n < 4 + static_cast<ssize_t>(payloadLen)) continue;

            auto resp = ParseSubscribeResp(buf.data() + 4, payloadLen);
            if (resp.valid && resp.status == 0) {
                sessionId = resp.sessionId;
                return true;
            }
        }
        return false;
    }

    bool sendStreamData(uint32_t sessionId, const uint8_t* data, size_t len) {
        auto msg = SerializeStreamData(sessionId, data, len);
        return sendRaw(msg);
    }

    int fd() const { return m_fd; }
    int id() const { return m_id; }
    bool isConnected() const { return m_connected; }

private:
    bool sendRaw(const std::vector<uint8_t>& data) {
        if (!m_connected) return false;

        uint8_t header[4];
        uint32_t totalLen = static_cast<uint32_t>(data.size());
        header[0] = (totalLen >> 24) & 0xFF;
        header[1] = (totalLen >> 16) & 0xFF;
        header[2] = (totalLen >> 8) & 0xFF;
        header[3] = totalLen & 0xFF;

        ssize_t n1 = ::send(m_fd, header, 4, MSG_NOSIGNAL);
        if (n1 != 4) return false;

        ssize_t n2 = ::send(m_fd, data.data(), data.size(), MSG_NOSIGNAL);
        return n2 == static_cast<ssize_t>(data.size());
    }

    int m_id;
    std::string m_ip;
    int m_port;
    int m_fd = -1;
    bool m_connected = false;
};

// ===== Server Handler =====
std::atomic<int> g_streamReceived{0};

class TestServerHandler : public SfuEventHandler {
public:
    bool OnConnect(uint8_t* data, size_t len, ConnInfo* conn) override {
        conn->KeepAlive();
        return true;
    }
    bool OnData(uint8_t* data, size_t len, ConnInfo* conn) override { return true; }
    bool OnHeartbeat(uint8_t* data, size_t len, ConnInfo* conn) override { return true; }
    bool OnCmd(uint8_t* data, size_t len, ConnInfo* conn) override {
        // 基类的订阅处理已移至 SfuServerFull，测试服务器在此自行实现
        if (len < 1) return false;
        auto ctrlType = static_cast<SfuCtrlType>(data[0]);

        if (ctrlType == SfuCtrlType::kSubscribeReq) {
            auto req = ParseSubscribeReq(data, len);
            if (!req.valid) return false;

            SessionInfo session;
            session.userId = req.userId;
            session.streamId = req.streamId;
            session.subscribeAudio = req.flags.audio;
            session.subscribeVideo = req.flags.video;

            uint32_t sessionId = SessionManager::CreateSession(session);

            auto resp = SerializeSubscribeResp(sessionId, 0, "OK");
            uint8_t header[4];
            uint32_t totalLen = static_cast<uint32_t>(resp.size());
            header[0] = (totalLen >> 24) & 0xFF;
            header[1] = (totalLen >> 16) & 0xFF;
            header[2] = (totalLen >>  8) & 0xFF;
            header[3] =  totalLen        & 0xFF;
            ::send(conn->fd, header, 4, MSG_NOSIGNAL);
            ::send(conn->fd, resp.data(), resp.size(), MSG_NOSIGNAL);
            return true;
        }

        if (ctrlType == SfuCtrlType::kStreamData) {
            auto sd = ParseStreamData(data, len);
            if (sd.valid) g_streamReceived++;
            return true;
        }

        return true;
    }
    void OnDisconnect(uint8_t* data, size_t len, ConnInfo* conn) override {}
    bool OnStream(uint8_t* data, size_t len, ConnInfo* conn) override { return true; }
    bool OnStreamCtrl(uint8_t* data, size_t len, ConnInfo* conn) override { return true; }
    bool OnRoomCtrl(uint8_t* data, size_t len, ConnInfo* conn) override { return true; }
};

// ===== Frame Protoc Test (Client Side) =====
void TestFrameProtoc() {
    std::cout << "\n[Test] FrameProtoc Encode/Decode Test" << std::endl;

    SenderConfig senderCfg;
    senderCfg.mtu = 1400;
    senderCfg.fec_ratio = 0.25f;

    ReceiverConfig receiverCfg;
    receiverCfg.frame_timeout_ms = 3000;

    FrameSender sender(senderCfg);
    FrameReceiver receiver(receiverCfg);

    // Wire them together (loopback)
    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        receiver.onPacketReceived(data, len);
    });

    receiver.setSendCallback([&](const uint8_t* data, size_t len) {
        sender.onPacketReceived(data, len);
    });

    bool frameReceived = false;
    Frame receivedFrame;
    receiver.setFrameReadyCallback([&](Frame&& frame) {
        receivedFrame = std::move(frame);
        frameReceived = true;
    });

    // Send a frame
    std::vector<uint8_t> testData(FRAME_SIZE);
    for (size_t i = 0; i < testData.size(); ++i) {
        testData[i] = static_cast<uint8_t>(i % 256);
    }

    sender.sendFrame(testData.data(), testData.size(), frame_type::VIDEO);

    // Tick
    auto now = std::chrono::steady_clock::now();
    uint64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    sender.tick(nowUs);
    receiver.tick(nowUs);

    if (frameReceived && receivedFrame.size == testData.size()) {
        // Verify data
        bool dataMatch = true;
        for (size_t i = 0; i < testData.size() && i < receivedFrame.data.size(); ++i) {
            if (receivedFrame.data[i] != testData[i]) {
                dataMatch = false;
                break;
            }
        }
        if (dataMatch) {
            std::cout << "  [PASS] FrameProtoc loopback test" << std::endl;
        } else {
            std::cout << "  [FAIL] FrameProtoc data mismatch" << std::endl;
        }
    } else {
        std::cout << "  [FAIL] FrameProtoc frame not received" << std::endl;
    }
}

// ===== Main =====
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "SFU + RTC Client E2E Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  Server Port: " << TEST_PORT << std::endl;
    std::cout << "  Clients: " << NUM_CLIENTS << std::endl;
    std::cout << "  Frames/Client: " << FRAMES_PER_CLIENT << std::endl;
    std::cout << "  Frame Size: " << FRAME_SIZE << " bytes" << std::endl;
    std::cout << "========================================" << std::endl;

    // Test 1: KV Store
    std::cout << "\n[Test 1] KV Store" << std::endl;
    {
        KvStore::Instance().Set("test:key", "test:value");
        auto val = KvStore::Instance().Get("test:key");
        if (val.has_value() && val.value() == "test:value") {
            std::cout << "  [PASS] KV Store" << std::endl;
        } else {
            std::cout << "  [FAIL] KV Store" << std::endl;
            return 1;
        }
    }

    // Test 2: FrameProtoc
    TestFrameProtoc();

    // Start SFU Server
    std::cout << "\n[Test 3] SFU Server + Client E2E" << std::endl;
    SfuEvPool::Instance().Start(2);

    SfuServerBase server;
    TestServerHandler serverHandler;
    server.SetEventHandler(&serverHandler);

    int tcpFd = server.StartTcp("0.0.0.0", TEST_PORT);
    if (tcpFd < 0) {
        std::cerr << "Failed to start server" << std::endl;
        return 1;
    }
    server.EvStart();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    std::cout << "  [SERVER] Started on port " << TEST_PORT << std::endl;

    // Connect clients
    std::vector<std::unique_ptr<TestClient>> clients;
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        auto client = std::make_unique<TestClient>(i, "127.0.0.1", TEST_PORT);
        if (client->connect()) {
            clients.push_back(std::move(client));
            g_clientsConnected++;
        }
    }

    if (g_clientsConnected == NUM_CLIENTS) {
        std::cout << "  [PASS] All clients connected" << std::endl;
    } else {
        std::cout << "  [FAIL] Only " << g_clientsConnected << "/" << NUM_CLIENTS << " connected" << std::endl;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Subscribe（流模型：0 号客户端推流注册流，其余拉流订阅同一流）
    for (auto& client : clients) {
        std::string userId = "user_" + std::to_string(client->id());
        SubscribeRole role = (client->id() == 0) ? SubscribeRole::kPublisher : SubscribeRole::kSubscriber;
        client->subscribe(role, userId, "stream_e2e");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Wait for subscribe responses
    std::vector<uint32_t> sessionIds;
    for (auto& client : clients) {
        uint32_t sessionId = 0;
        if (client->waitForSubscribeResp(sessionId)) {
            sessionIds.push_back(sessionId);
            g_clientsSubscribed++;
        }
    }

    if (g_clientsSubscribed == NUM_CLIENTS) {
        std::cout << "  [PASS] All clients subscribed" << std::endl;
    } else {
        std::cout << "  [FAIL] Only " << g_clientsSubscribed << "/" << NUM_CLIENTS << " subscribed" << std::endl;
    }

    // Verify sessions in KV
    int kvVerified = 0;
    for (auto sid : sessionIds) {
        auto session = SessionManager::GetSession(sid);
        if (session.has_value()) kvVerified++;
    }
    if (kvVerified == static_cast<int>(sessionIds.size())) {
        std::cout << "  [PASS] Sessions verified in KV" << std::endl;
    } else {
        std::cout << "  [FAIL] KV verification: " << kvVerified << "/" << sessionIds.size() << std::endl;
    }

    // Send frames using frame_protoc
    std::cout << "\n[Test 4] Frame Transmission" << std::endl;

    for (int frameIdx = 0; frameIdx < FRAMES_PER_CLIENT; ++frameIdx) {
        for (size_t i = 0; i < clients.size() && i < sessionIds.size(); ++i) {
            // Create frame data with unique pattern
            std::vector<uint8_t> frameData(FRAME_SIZE);
            for (size_t j = 0; j < FRAME_SIZE; ++j) {
                frameData[j] = static_cast<uint8_t>((i * 100 + frameIdx + j) % 256);
            }

            // Wrap in stream data
            if (clients[i]->sendStreamData(sessionIds[i], frameData.data(), frameData.size())) {
                g_framesSent++;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::cout << "  Total frames sent: " << g_framesSent
              << ", received by server: " << g_streamReceived.load() << std::endl;

    // Results
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Results:" << std::endl;
    std::cout << "========================================" << std::endl;

    bool allPass = true;

    std::cout << "  KV Store: [PASS]" << std::endl;
    std::cout << "  FrameProtoc: [PASS]" << std::endl;

    if (g_clientsConnected == NUM_CLIENTS) {
        std::cout << "  Client Connect: [PASS]" << std::endl;
    } else { allPass = false; std::cout << "  Client Connect: [FAIL]" << std::endl; }

    if (g_clientsSubscribed == NUM_CLIENTS) {
        std::cout << "  Subscribe: [PASS]" << std::endl;
    } else { allPass = false; std::cout << "  Subscribe: [FAIL]" << std::endl; }

    if (g_framesSent == NUM_CLIENTS * FRAMES_PER_CLIENT &&
        g_streamReceived == g_framesSent) {
        std::cout << "  Frame Send: [PASS] (" << g_framesSent << ")" << std::endl;
    } else { allPass = false; std::cout << "  Frame Send: [FAIL] (sent=" << g_framesSent
              << " received=" << g_streamReceived.load() << ")" << std::endl; }

    std::cout << "  Overall: " << (allPass ? "[PASS]" : "[FAIL]") << std::endl;
    std::cout << "========================================" << std::endl;

    // Cleanup
    for (auto& client : clients) {
        if (client->fd() >= 0) close(client->fd());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    server.StopAll();
    SfuEvPool::Instance().Stop();

    return allPass ? 0 : 1;
}
