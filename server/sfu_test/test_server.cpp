// SFU Server + Multi-Client Test Program
// Tests: server startup, multi-client connect, data send/recv, data integrity verification

// System headers first
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#include "SfuServerBase.h"
#include "SfuEvPool.h"
#include "SfuConnManager.h"
#include "SfuProtocol.h"

#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>

using namespace sfu;

// ===== Test Configuration =====
const int TEST_PORT_UDP = 9000;
const int TEST_PORT_TCP = 9001;
const int NUM_CLIENTS = 5;
const int MSGS_PER_CLIENT = 10;
const int MSG_SIZE = 256;

// ===== Global Test State =====
std::atomic<int> g_connectedClients{0};
std::atomic<int> g_disconnectedClients{0};
std::atomic<int> g_totalMsgsReceived{0};
std::atomic<int> g_totalBytesReceived{0};
std::atomic<bool> g_serverReady{false};
std::mutex g_testMutex;
std::set<std::string> g_receivedMsgs;  // Track received messages for verification

// ===== Server Event Handler =====
class TestServerHandler : public SfuEventHandler {
public:
    bool OnConnect(uint8_t* data, size_t len, ConnInfo* conn) override {
        std::lock_guard<std::mutex> lock(g_testMutex);
        int count = ++g_connectedClients;
        std::cout << "[SERVER] Client connected: fd=" << conn->fd
                  << " (total: " << count << ")" << std::endl;
        conn->KeepAlive();
        return true;
    }

    bool OnData(uint8_t* data, size_t len, ConnInfo* conn) override {
        g_totalMsgsReceived++;
        g_totalBytesReceived += static_cast<int>(len);

        // Verify data integrity
        std::string msgStr(reinterpret_cast<char*>(data), len);
        {
            std::lock_guard<std::mutex> lock(g_testMutex);
            if (g_receivedMsgs.count(msgStr)) {
                std::cout << "[SERVER] WARNING: Duplicate message received!" << std::endl;
            }
            g_receivedMsgs.insert(msgStr);
        }

        // Echo back to client
        // (In real scenario, we'd use ConnManager to send)

        return true;
    }

    bool OnHeartbeat(uint8_t* data, size_t len, ConnInfo* conn) override {
        return true;
    }

    bool OnCmd(uint8_t* data, size_t len, ConnInfo* conn) override {
        return true;
    }

    void OnDisconnect(uint8_t* data, size_t len, ConnInfo* conn) override {
        std::lock_guard<std::mutex> lock(g_testMutex);
        int count = ++g_disconnectedClients;
        std::cout << "[SERVER] Client disconnected: fd=" << conn->fd
                  << " (total: " << count << ")" << std::endl;
    }

    bool OnStream(uint8_t* data, size_t len, ConnInfo* conn) override {
        return OnData(data, len, conn);
    }

    bool OnStreamCtrl(uint8_t* data, size_t len, ConnInfo* conn) override {
        return true;
    }

    bool OnRoomCtrl(uint8_t* data, size_t len, ConnInfo* conn) override {
        return true;
    }
};

// ===== Simple TCP Client for Testing =====
class TestClient {
public:
    TestClient(int id, const std::string& ip, int port)
        : m_id(id), m_ip(ip), m_port(port) {}

    ~TestClient() {
        disconnect();
    }

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

        // Set non-blocking
        int flags = fcntl(m_fd, F_GETFL, 0);
        fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);

        m_connected = true;
        std::cout << "[CLIENT " << m_id << "] Connected to " << m_ip << ":" << m_port << std::endl;
        return true;
    }

    void disconnect() {
        if (m_fd >= 0) {
            close(m_fd);
            m_fd = -1;
        }
        m_connected = false;
    }

    bool sendTestData(int msgIndex) {
        if (!m_connected) return false;

        // Create test message with predictable pattern
        std::string msg = "client_" + std::to_string(m_id) +
                         "_msg_" + std::to_string(msgIndex) +
                         "_data_";

        // Fill with pattern
        while (msg.size() < MSG_SIZE) {
            msg += static_cast<char>('A' + (msg.size() % 26));
        }

        // Build TCP frame: 4-byte length prefix + 1-byte header + payload
        uint32_t payloadLen = 1 + msg.size();
        uint8_t frame[4 + 1 + MSG_SIZE];

        frame[0] = (payloadLen >> 24) & 0xFF;
        frame[1] = (payloadLen >> 16) & 0xFF;
        frame[2] = (payloadLen >> 8) & 0xFF;
        frame[3] = payloadLen & 0xFF;
        frame[4] = MakeHeader(PROTOCOL_VERSION, static_cast<uint8_t>(MsgType::kData));
        memcpy(frame + 5, msg.data(), msg.size());

        ssize_t n = ::send(m_fd, frame, 4 + payloadLen, MSG_NOSIGNAL);
        if (n > 0) {
            std::cout << "[CLIENT " << m_id << "] Sent msg " << msgIndex
                      << " (" << n << " bytes)" << std::endl;
            return true;
        }

        std::cerr << "[CLIENT " << m_id << "] Send failed: " << strerror(errno) << std::endl;
        return false;
    }

    int id() const { return m_id; }
    bool isConnected() const { return m_connected; }

private:
    int m_id;
    std::string m_ip;
    int m_port;
    int m_fd = -1;
    bool m_connected = false;
};

// ===== Test Main =====
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "SFU Server Multi-Client Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Configuration:" << std::endl;
    std::cout << "  UDP Port: " << TEST_PORT_UDP << std::endl;
    std::cout << "  TCP Port: " << TEST_PORT_TCP << std::endl;
    std::cout << "  Clients: " << NUM_CLIENTS << std::endl;
    std::cout << "  Messages per client: " << MSGS_PER_CLIENT << std::endl;
    std::cout << "  Message size: " << MSG_SIZE << " bytes" << std::endl;
    std::cout << "  Expected total messages: " << NUM_CLIENTS * MSGS_PER_CLIENT << std::endl;
    std::cout << "========================================" << std::endl;

    // Start event loop pool
    SfuEvPool::Instance().Start(2);

    // Create server
    SfuServerBase server;
    TestServerHandler serverHandler;
    server.SetEventHandler(&serverHandler);

    // Start TCP listener
    int tcpFd = server.StartTcp("0.0.0.0", TEST_PORT_TCP);
    if (tcpFd < 0) {
        std::cerr << "Failed to start TCP server on port " << TEST_PORT_TCP << std::endl;
        return 1;
    }
    std::cout << "[SERVER] TCP listener started on port " << TEST_PORT_TCP << std::endl;

    // Start event loop
    server.EvStart();

    // Give the pool threads time to process the start request
    std::this_thread::sleep_for(std::chrono::milliseconds(700));

    g_serverReady.store(true);
    std::cout << "[SERVER] Server ready" << std::endl;

    // Connect clients
    std::vector<std::unique_ptr<TestClient>> clients;
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        auto client = std::make_unique<TestClient>(i, "127.0.0.1", TEST_PORT_TCP);
        if (client->connect()) {
            clients.push_back(std::move(client));
        } else {
            std::cerr << "[TEST] Failed to connect client " << i << std::endl;
        }
    }

    // Wait for connections to be accepted
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "========================================" << std::endl;
    std::cout << "Starting data transmission test..." << std::endl;
    std::cout << "========================================" << std::endl;

    // Each client sends multiple messages
    auto sendThread = [&]() {
        for (auto& client : clients) {
            for (int j = 0; j < MSGS_PER_CLIENT; ++j) {
                client->sendTestData(j);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < clients.size(); ++i) {
        threads.emplace_back([&, i]() {
            auto& client = clients[i];
            for (int j = 0; j < MSGS_PER_CLIENT; ++j) {
                client->sendTestData(j);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }

    // Wait for all send threads to complete
    for (auto& t : threads) {
        t.join();
    }

    // Wait for server to process all messages
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // ===== Verification =====
    std::cout << "========================================" << std::endl;
    std::cout << "Test Results:" << std::endl;
    std::cout << "========================================" << std::endl;

    int expectedMsgs = NUM_CLIENTS * MSGS_PER_CLIENT;
    int receivedMsgs = g_totalMsgsReceived.load();
    int receivedBytes = g_totalBytesReceived.load();

    std::cout << "  Messages sent: " << expectedMsgs << std::endl;
    std::cout << "  Messages received: " << receivedMsgs << std::endl;
    std::cout << "  Bytes received: " << receivedBytes << std::endl;
    std::cout << "  Unique messages: " << g_receivedMsgs.size() << std::endl;

    bool pass = true;

    if (receivedMsgs != expectedMsgs) {
        std::cout << "  [FAIL] Message count mismatch!" << std::endl;
        pass = false;
    } else {
        std::cout << "  [PASS] Message count correct" << std::endl;
    }

    if (g_receivedMsgs.size() != static_cast<size_t>(receivedMsgs)) {
        std::cout << "  [FAIL] Duplicate messages detected!" << std::endl;
        pass = false;
    } else {
        std::cout << "  [PASS] No duplicate messages" << std::endl;
    }

    // Verify each message content
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        for (int j = 0; j < MSGS_PER_CLIENT; ++j) {
            std::string expectedMsg = "client_" + std::to_string(i) +
                                     "_msg_" + std::to_string(j) +
                                     "_data_";
            while (expectedMsg.size() < MSG_SIZE) {
                expectedMsg += static_cast<char>('A' + (expectedMsg.size() % 26));
            }

            if (!g_receivedMsgs.count(expectedMsg)) {
                std::cout << "  [FAIL] Missing message from client " << i
                          << " msg " << j << std::endl;
                pass = false;
            }
        }
    }

    if (pass) {
        std::cout << "  [PASS] All messages verified!" << std::endl;
    }

    std::cout << "========================================" << std::endl;

    // Cleanup
    for (auto& client : clients) {
        client->disconnect();
    }

    // Allow time for cleanup
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    server.StopAll();
    SfuEvPool::Instance().Stop();

    return pass ? 0 : 1;
}
