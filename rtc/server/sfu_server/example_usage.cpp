// Example: How to use the SFU Server framework
// This file demonstrates the basic usage pattern

#include "SfuServerBase.h"
#include "SfuEvPool.h"
#include "SfuConnManager.h"
#include <iostream>

using namespace sfu;

// 1. Implement your business logic handler
class MySfuHandler : public SfuEventHandler {
public:
    bool OnConnect(uint8_t* data, size_t len, ConnInfo* conn) override {
        std::cout << "Client connected: fd=" << conn->fd << std::endl;
        // Accept the connection
        conn->KeepAlive();
        return true;
    }

    bool OnData(uint8_t* data, size_t len, ConnInfo* conn) override {
        std::cout << "Data received: fd=" << conn->fd << " len=" << len << std::endl;
        // Process data...
        return true;
    }

    bool OnHeartbeat(uint8_t* data, size_t len, ConnInfo* conn) override {
        // Send heartbeat response
        uint8_t resp[] = {0x01};
        // Send back using conn manager
        return true;
    }

    bool OnCmd(uint8_t* data, size_t len, ConnInfo* conn) override {
        std::cout << "Command received: fd=" << conn->fd << std::endl;
        return true;
    }

    void OnDisconnect(uint8_t* data, size_t len, ConnInfo* conn) override {
        std::cout << "Client disconnected: fd=" << conn->fd << std::endl;
    }

    bool OnStream(uint8_t* data, size_t len, ConnInfo* conn) override {
        // SFU-specific: handle media stream data
        std::cout << "Stream data: fd=" << conn->fd << " len=" << len << std::endl;
        return true;
    }

    bool OnStreamCtrl(uint8_t* data, size_t len, ConnInfo* conn) override {
        // SFU-specific: handle stream control (play, pause, etc.)
        return true;
    }

    bool OnRoomCtrl(uint8_t* data, size_t len, ConnInfo* conn) override {
        // SFU-specific: handle room control (join, leave, publish, subscribe)
        return true;
    }

    void OnUpdate(int intervalMs) override {
        // Periodic update callback (500ms or 1000ms)
        // Use for bandwidth stats, timeout detection, etc.
    }
};

// 2. Initialize and start the server
int main() {
    // Initialize event loop pool
    SfuEvPool::Instance().Start();

    // Create server
    SfuServerBase server;

    // Set event handler
    MySfuHandler handler;
    server.SetEventHandler(&handler);

    // Start listening
    server.StartUdp("0.0.0.0", 8000);
    server.StartTcp("0.0.0.0", 8001);

    // Schedule to start on event loop
    server.EvStart();

    // Wait for user input to stop
    std::cout << "SFU Server running. Press Enter to stop..." << std::endl;
    std::cin.get();

    // Stop server
    server.EvStop();
    server.StopAll();

    // Stop event loop pool
    SfuEvPool::Instance().Stop();

    return 0;
}
