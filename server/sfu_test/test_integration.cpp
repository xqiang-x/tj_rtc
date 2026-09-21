// SFU Server + RTC Client Integration Test
// Tests: KV store, subscribe protocol, session management, data forwarding

#include "SfuServerBase.h"
#include "SfuEvPool.h"
#include "SfuKvStore.h"
#include "SfuSubscribeProtocol.h"

#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

using namespace sfu;

// ===== Test Configuration =====
const int TEST_PORT = 9100;
const int NUM_CLIENTS = 3;
const int MSGS_PER_CLIENT = 5;

// ===== Global Test State =====
std::atomic<int> g_sessionsCreated{0};
std::atomic<int> g_subscribeOk{0};
std::atomic<int> g_totalStreamData{0};
std::atomic<int> g_streamReceived{0};
std::mutex g_testMutex;
std::vector<uint32_t> g_sessionIds;

// ===== Test Client =====
class TestRtcClient {
public:
    TestRtcClient(int id, const std::string& ip, int port)
        : m_id(id), m_ip(ip), m_port(port) {}

    ~TestRtcClient() { disconnect(); }

    bool connect() {
        m_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0) return false;

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(m_port);
        addr.sin_addr.s_addr = inet_addr(m_ip.c_str());

        if (::connect(m_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(m_fd);
            m_fd = -1;
            return false;
        }

        int flags = fcntl(m_fd, F_GETFL, 0);
        fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);

        m_connected = true;
        std::cout << "[CLIENT " << m_id << "] Connected" << std::endl;
        return true;
    }

    void disconnect() {
        if (m_fd >= 0) {
            close(m_fd);
            m_fd = -1;
        }
        m_connected = false;
    }

    bool subscribe(SubscribeRole role, const std::string& userId, const std::string& streamId) {
        SubscribeFlags flags;
        flags.audio = true;
        flags.video = true;
        flags.data = true;
        flags.reserved = 0;

        auto msg = SerializeSubscribeReq(role, streamId, userId, flags);
        return sendRaw(msg.data(), msg.size());
    }

    bool sendStreamData(const std::vector<uint8_t>& data) {
        if (!m_connected || m_sessionId == 0) return false;

        auto msg = SerializeStreamData(m_sessionId, data.data(), data.size());
        return sendRaw(msg.data(), msg.size());
    }

    bool waitForSubscribeResp(int timeoutMs = 2000) {
        uint8_t buf[4096];
        auto start = std::chrono::steady_clock::now();
        int recvCount = 0;

        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(timeoutMs)) {
            ssize_t n = recv(m_fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    std::cerr << "[CLIENT " << m_id << "] Recv error: " << strerror(errno) << std::endl;
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            recvCount++;
            std::cout << "[CLIENT " << m_id << "] Recv " << n << " bytes (call " << recvCount << ")" << std::endl;

            // Parse TCP frame: 4-byte length + payload
            if (n < 4) {
                std::cout << "[CLIENT " << m_id << "] Incomplete header" << std::endl;
                continue;
            }

            uint32_t payloadLen = (buf[0] << 24) | (buf[1] << 16) |
                                  (buf[2] << 8) | buf[3];

            std::cout << "[CLIENT " << m_id << "] Payload len: " << payloadLen << std::endl;

            if (n < 4 + static_cast<ssize_t>(payloadLen)) {
                std::cout << "[CLIENT " << m_id << "] Incomplete payload" << std::endl;
                continue;
            }

            auto resp = ParseSubscribeResp(buf + 4, payloadLen);
            std::cout << "[CLIENT " << m_id << "] Parsed resp: valid=" << resp.valid
                      << " status=" << (int)resp.status << std::endl;

            if (resp.valid && resp.status == 0) {
                m_sessionId = resp.sessionId;
                std::cout << "[CLIENT " << m_id << "] Subscribe OK, sessionId="
                          << m_sessionId << std::endl;
                return true;
            }
        }

        return false;
    }

    uint32_t sessionId() const { return m_sessionId; }
    int id() const { return m_id; }
    bool isConnected() const { return m_connected; }
    int fd() const { return m_fd; }

private:
    bool sendRaw(const uint8_t* data, size_t len) {
        if (!m_connected) return false;

        // TCP frame: 4-byte length prefix + payload
        uint8_t header[4];
        uint32_t totalLen = static_cast<uint32_t>(len);
        header[0] = (totalLen >> 24) & 0xFF;
        header[1] = (totalLen >> 16) & 0xFF;
        header[2] = (totalLen >> 8) & 0xFF;
        header[3] = totalLen & 0xFF;

        ssize_t n1 = ::send(m_fd, header, 4, MSG_NOSIGNAL);
        if (n1 != 4) return false;

        ssize_t n2 = ::send(m_fd, data, len, MSG_NOSIGNAL);
        return n2 == static_cast<ssize_t>(len);
    }

    int m_id;
    std::string m_ip;
    int m_port;
    int m_fd = -1;
    bool m_connected = false;
    uint32_t m_sessionId = 0;
};

// ===== Server Event Handler =====
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
            g_sessionsCreated++;

            auto resp = SerializeSubscribeResp(sessionId, 0, "OK");
            // TCP 帧格式：4 字节大端长度前缀 + payload
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

// ===== Test Main =====
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "SFU Integration Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  Port: " << TEST_PORT << std::endl;
    std::cout << "  Clients: " << NUM_CLIENTS << std::endl;
    std::cout << "  Messages per client: " << MSGS_PER_CLIENT << std::endl;
    std::cout << "========================================" << std::endl;

    // Test 1: KV Store
    std::cout << "\n[Test 1] KV Store Test" << std::endl;
    {
        KvStore::Instance().Set("test:key", "test:value");
        auto val = KvStore::Instance().Get("test:key");
        if (val.has_value() && val.value() == "test:value") {
            std::cout << "  [PASS] KV Set/Get" << std::endl;
        } else {
            std::cout << "  [FAIL] KV Set/Get" << std::endl;
            return 1;
        }

        SessionInfo info;
        info.userId = "user_test";
        info.streamId = "stream_1";
        info.subscribeAudio = true;
        info.subscribeVideo = true;

        uint32_t sid = SessionManager::CreateSession(info);
        auto session = SessionManager::GetSession(sid);
        if (session.has_value() && session->userId == "user_test") {
            std::cout << "  [PASS] SessionManager Create/Get" << std::endl;
        } else {
            std::cout << "  [FAIL] SessionManager Create/Get" << std::endl;
            return 1;
        }

        auto streamSessions = SessionManager::GetStreamSessions("stream_1");
        if (streamSessions.size() == 1) {
            std::cout << "  [PASS] SessionManager GetStreamSessions" << std::endl;
        } else {
            std::cout << "  [FAIL] SessionManager GetStreamSessions" << std::endl;
            return 1;
        }
    }

    // Start event loop pool
    SfuEvPool::Instance().Start(2);

    // Create server
    SfuServerBase server;
    TestServerHandler serverHandler;
    server.SetEventHandler(&serverHandler);

    // Start TCP listener
    int tcpFd = server.StartTcp("0.0.0.0", TEST_PORT);
    if (tcpFd < 0) {
        std::cerr << "Failed to start TCP server" << std::endl;
        return 1;
    }
    std::cout << "\n[SERVER] TCP listener started on port " << TEST_PORT << std::endl;

    server.EvStart();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    std::cout << "[SERVER] Server ready" << std::endl;

    // Test 2: Subscribe Protocol
    std::cout << "\n[Test 2] Subscribe Protocol Test" << std::endl;

    std::vector<std::unique_ptr<TestRtcClient>> clients;
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        auto client = std::make_unique<TestRtcClient>(i, "127.0.0.1", TEST_PORT);
        if (!client->connect()) {
            std::cerr << "[FAIL] Client " << i << " connection failed" << std::endl;
            return 1;
        }
        clients.push_back(std::move(client));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Each client subscribes（流模型：0 号客户端推流注册流，其余拉流订阅同一流）
    for (auto& client : clients) {
        std::string userId = "user_" + std::to_string(client->id());
        SubscribeRole role = (client->id() == 0) ? SubscribeRole::kPublisher : SubscribeRole::kSubscriber;
        if (client->subscribe(role, userId, "stream_1")) {
            std::cout << "[CLIENT " << client->id() << "] Subscribe request sent" << std::endl;
        }
    }

    // Wait for responses
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    for (auto& client : clients) {
        if (client->waitForSubscribeResp()) {
            g_subscribeOk++;
            g_sessionIds.push_back(client->sessionId());
        } else {
            std::cerr << "[FAIL] Client " << client->id() << " subscribe timeout" << std::endl;
        }
    }

    if (g_subscribeOk == NUM_CLIENTS) {
        std::cout << "  [PASS] All clients subscribed (" << NUM_CLIENTS << ")" << std::endl;
    } else {
        std::cout << "  [FAIL] Only " << g_subscribeOk << "/" << NUM_CLIENTS << " subscribed" << std::endl;
    }

    // Test 3: Stream Data Forwarding
    std::cout << "\n[Test 3] Stream Data Test" << std::endl;

    for (int msgIdx = 0; msgIdx < MSGS_PER_CLIENT; ++msgIdx) {
        for (auto& client : clients) {
            // Create test frame data
            std::vector<uint8_t> frameData(256);
            memset(frameData.data(), client->id() + 'A', frameData.size());

            if (client->sendStreamData(frameData)) {
                g_totalStreamData++;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "  Total stream data sent: " << g_totalStreamData << std::endl;

    // Verify sessions exist in KV
    int verifiedSessions = 0;
    for (auto sid : g_sessionIds) {
        auto session = SessionManager::GetSession(sid);
        if (session.has_value()) {
            verifiedSessions++;
        }
    }

    if (verifiedSessions == static_cast<int>(g_sessionIds.size())) {
        std::cout << "  [PASS] All sessions verified in KV store" << std::endl;
    } else {
        std::cout << "  [FAIL] Only " << verifiedSessions << "/" << g_sessionIds.size()
                  << " sessions verified" << std::endl;
    }

    // Test Results
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Results:" << std::endl;
    std::cout << "========================================" << std::endl;

    bool allPass = true;

    std::cout << "  KV Store: [PASS]" << std::endl;
    std::cout << "  Session Management: [PASS]" << std::endl;

    if (g_subscribeOk == NUM_CLIENTS) {
        std::cout << "  Subscribe Protocol: [PASS]" << std::endl;
    } else {
        std::cout << "  Subscribe Protocol: [FAIL]" << std::endl;
        allPass = false;
    }

    if (g_totalStreamData == NUM_CLIENTS * MSGS_PER_CLIENT &&
        g_streamReceived == g_totalStreamData) {
        std::cout << "  Stream Data: [PASS] (" << g_totalStreamData << " msgs, "
                  << g_streamReceived.load() << " received by server)" << std::endl;
    } else {
        std::cout << "  Stream Data: [FAIL] (sent=" << g_totalStreamData
                  << " received=" << g_streamReceived.load() << ")" << std::endl;
        allPass = false;
    }

    std::cout << "  Overall: " << (allPass ? "[PASS]" : "[FAIL]") << std::endl;
    std::cout << "========================================" << std::endl;

    // Cleanup
    for (auto& client : clients) {
        client->disconnect();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    server.StopAll();
    SfuEvPool::Instance().Stop();

    return allPass ? 0 : 1;
}
