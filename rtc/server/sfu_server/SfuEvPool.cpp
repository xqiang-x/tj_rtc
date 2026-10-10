#include "SfuEvPool.h"
#include "SfuServerBase.h"

#include <thread>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <glog/logging.h>

#ifndef __APPLE__
#include <pthread.h>
#endif

namespace sfu {

// 事件循环异常屏障。libev 的回调是 C 函数指针，回调里抛出的 C++ 异常会一路抛穿
// ev_run → std::terminate → abort，一条畸形连接就能带走整个进程（生产上表现为
// "terminate called after throwing an instance of 'std::bad_alloc'"）。
// 这里兜住并重新进入循环：宁可丢一条连接，也不要整机静默停摆。
static void RunLoopWithBarrier(struct ev_loop* loop) {
    unsigned int thrown = 0;
    for (;;) {
        try {
            ev_run(loop, 0);
            return;  // 循环被正常 stop
        } catch (const std::exception& e) {
            LOG(ERROR) << "[SFU] ev_run 捕获异常，重启事件循环: #" << ++thrown
                       << " what=" << e.what();
        } catch (...) {
            LOG(ERROR) << "[SFU] ev_run 捕获未知异常，重启事件循环: #" << ++thrown;
        }
        // 同一个挂起事件反复抛异常时，避免空转刷满 CPU 和日志
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

SfuEvPool& SfuEvPool::Instance() {
    static SfuEvPool instance;
    return instance;
}

SfuEvPool::~SfuEvPool() {
    Stop();
    for (auto* info : cpuInfos) {
        delete info;
    }
    cpuInfos.clear();
}

uint32_t SfuEvPool::HashKey(const std::string& key) {
    uint32_t hash = 0;
    for (char c : key) {
        hash = hash * 31 + static_cast<uint32_t>(c);
    }
    return hash;
}

void SfuEvPool::Start(int cpuCount) {
    if (running.load()) {
        return;
    }

    if (cpuCount <= 0) {
        cpuCount = static_cast<int>(std::thread::hardware_concurrency());
        if (cpuCount == 0) {
            cpuCount = 1;
        }
    }

    // Create one event loop per CPU core
    for (int i = 0; i < cpuCount; ++i) {
        ThreadInfo* pInfo = new ThreadInfo;
        pInfo->cpuNum = i;
        pInfo->loop = ev_loop_new(EVFLAG_AUTO);
        if (!pInfo->loop) {
            LOG(ERROR) << "Failed to create ev_loop for CPU " << i;
            delete pInfo;
            continue;
        }
        pInfo->running = true;

        // Initialize 5ms hold timer for processing cross-thread queues
        pInfo->evHoldTimer.data = pInfo;
        ev_timer_init(&pInfo->evHoldTimer, [](struct ev_loop* loop, ev_timer* timer, int revents) {
            ThreadInfo* info = static_cast<ThreadInfo*>(timer->data);
            if (!info->running.load()) {
                ev_break(info->loop, EVBREAK_ALL);
                return;
            }
            info->watchDogFlag.store(true);

            std::lock_guard<std::mutex> lock(info->listMx);

            // Process start requests
            while (!info->willStartList.empty()) {
                auto pair = std::move(info->willStartList.front());
                info->willStartList.pop_front();
                if (pair.second) {
                    pair.second->OnEvStart();
                }
            }

            // Process stop requests - defer actual stop to avoid recursion
            // Just mark them for cleanup
            if (!info->willStopList.empty()) {
                // Process at most one stop request per timer cycle
                auto* server = info->willStopList.front();
                info->willStopList.pop_front();
                if (server) {
                    server->OnEvStop();
                }
            }
        }, 0, 0.005);
        ev_timer_start(pInfo->loop, &pInfo->evHoldTimer);

        cpuInfos.push_back(pInfo);
    }

    // Start event loop threads
    for (int i = 0; i < static_cast<int>(cpuInfos.size()); ++i) {
        ThreadInfo* pInfo = cpuInfos[i];

#ifdef __APPLE__
        // macOS: just create the thread
        pInfo->worker = std::thread([pInfo]() {
            pInfo->threadId = std::this_thread::get_id();
            RunLoopWithBarrier(pInfo->loop);
        });
#else
        // Linux: 不做 CPU 绑核——共享主机上绑定的核可能被其他负载占满，
        // 导致事件循环线程被饿死数秒（看门狗告警、会话超时）
        pInfo->worker = std::thread([pInfo]() {
            pInfo->threadId = std::this_thread::get_id();
            RunLoopWithBarrier(pInfo->loop);
        });
#endif
    }

    // Start watchdog thread
    running.store(true);
    watchdogThread = std::thread(&SfuEvPool::WatchdogFun, this);
}

void SfuEvPool::Stop() {
    if (!running.load()) {
        return;
    }

    running.store(false);

    // Break all event loops
    for (auto* info : cpuInfos) {
        info->running = false;
    }

    // Wait for watchdog thread
    if (watchdogThread.joinable()) {
        watchdogThread.join();
    }

    // Join all worker threads (they exit when ev_break fires from the hold timer)
    for (auto* info : cpuInfos) {
        if (info->worker.joinable()) {
            info->worker.join();
        }
    }

    // Threads are joined, safe to free the per-CPU event loops
    for (auto* info : cpuInfos) {
        if (info->loop) {
            ev_loop_destroy(info->loop);
            info->loop = nullptr;
        }
    }
}

struct ev_loop* SfuEvPool::GetLoop(const std::string& key) {
    if (cpuInfos.empty()) {
        return nullptr;
    }
    uint32_t hash = HashKey(key);
    int index = hash % static_cast<int>(cpuInfos.size());
    return cpuInfos[index]->loop;
}

ThreadInfo* SfuEvPool::GetThreadInfo(const std::string& key) {
    if (cpuInfos.empty()) {
        return nullptr;
    }
    uint32_t hash = HashKey(key);
    int index = hash % static_cast<int>(cpuInfos.size());
    return cpuInfos[index];
}

void SfuEvPool::ScheduleStart(const std::string& key, SfuServerBase* server) {
    ThreadInfo* info = GetThreadInfo(key);
    if (!info) {
        return;
    }

    // 控制线程 ⇄ ev_loop 线程：跨线程入队需要加锁
    std::lock_guard<std::mutex> lock(info->listMx);
    EvPoolMsg msg;
    msg.type = 1; // start
    info->willStartList.emplace_back(msg, server);
}

void SfuEvPool::ScheduleStop(const std::string& key, SfuServerBase* server) {
    ThreadInfo* info = GetThreadInfo(key);
    if (!info) {
        return;
    }

    // 控制线程 ⇄ ev_loop 线程：跨线程入队需要加锁
    std::lock_guard<std::mutex> lock(info->listMx);
    info->willStopList.push_back(server);
}

void SfuEvPool::WatchdogFun(SfuEvPool* pool) {
    while (pool->running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(700));

        // Check all threads' watchdog flags
        for (auto* info : pool->cpuInfos) {
            if (!info || !info->loop) {
                continue;
            }

            if (!info->watchDogFlag.load()) {
                // Thread may be blocked - log warning（限速：持续卡死时每 700ms 检查一次）
                static uint64_t last_watchdog_log_us = 0;
                uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (now_us - last_watchdog_log_us >= 5'000'000) {
                    LOG(ERROR) << "Watchdog: event loop on CPU " << info->cpuNum
                               << " may be blocked!";
                    last_watchdog_log_us = now_us;
                }
            }
            info->watchDogFlag.store(false);
        }
    }
}

} // namespace sfu
