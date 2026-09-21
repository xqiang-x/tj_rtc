#ifndef SFU_EV_POOL_H
#define SFU_EV_POOL_H

#include <vector>
#include <string>
#include <list>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>

extern "C" {
#include <ev.h>
}

namespace sfu {

// Forward declaration
class SfuServerBase;

// Message for cross-thread task dispatching
struct EvPoolMsg {
    int type = 0;
    void* data = nullptr;
};

// Per-thread event loop info
struct ThreadInfo {
    int cpuNum = 0;
    struct ev_loop* loop = nullptr;
    std::thread::id threadId;
    std::thread worker;  // Owned thread handle for graceful join

    // Cross-thread task queues, processed within the target thread's context
    // 控制线程（main）⇄ ev_loop 线程是真实跨线程点，必须加锁
    std::list<std::pair<EvPoolMsg, SfuServerBase*>> willStartList;
    std::list<SfuServerBase*> willStopList;
    std::mutex listMx;

    // 5ms timer for processing cross-thread queues
    ev_timer evHoldTimer;
    std::atomic_bool running{false};

    // Watchdog heartbeat flag
    std::atomic_bool watchDogFlag{true};
};

// Event loop thread pool - one ev_loop per CPU core
class SfuEvPool {
public:
    static SfuEvPool& Instance();

    // Start the event loop pool with specified number of threads
    // If cpuCount is 0, uses all available CPU cores
    void Start(int cpuCount = 0);

    // Stop all event loops and join threads
    void Stop();

    // Get event loop for a given key (hash-based distribution)
    struct ev_loop* GetLoop(const std::string& key);

    // Get thread info for a given key
    ThreadInfo* GetThreadInfo(const std::string& key);

    // Schedule a server to start on the appropriate thread
    void ScheduleStart(const std::string& key, SfuServerBase* server);

    // Schedule a server to stop on the appropriate thread
    void ScheduleStop(const std::string& key, SfuServerBase* server);

    // Get total number of threads
    int GetThreadCount() const { return static_cast<int>(cpuInfos.size()); }

    // Check if the pool is running
    bool IsRunning() const { return running.load(); }

private:
    SfuEvPool() = default;
    ~SfuEvPool();

    // Hash function for key-based thread distribution
    uint32_t HashKey(const std::string& key);

    // Watchdog thread function
    static void WatchdogFun(SfuEvPool* pool);

    std::vector<ThreadInfo*> cpuInfos;
    std::thread watchdogThread;
    std::atomic_bool running{false};
};

} // namespace sfu

#endif // SFU_EV_POOL_H
