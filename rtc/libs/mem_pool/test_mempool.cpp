// test_mempool.cpp - 简单单元测试
//
// 编译（standalone）：
//   cd mem_pool && mkdir -p build && cd build
//   cmake .. && make && ./test_mempool

#include "MemPool.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#include <chrono>

using namespace mempool;

void test_basic_alloc_dealloc() {
    FixedMemoryPool pool(16);  // 初始 16 块

    assert(pool.totalBlocks() == 16);
    assert(pool.freeBlocks()  == 16);
    assert(pool.usedBlocks()  == 0);

    // 分配一块
    void* p = pool.allocate();
    assert(p != nullptr);
    assert(pool.usedBlocks() == 1);
    assert(pool.freeBlocks() == 15);

    // 写满 1500 字节不崩溃
    std::memset(p, 0xAB, FixedMemoryPool::kBlockSize);

    // 释放
    pool.deallocate(p);
    assert(pool.usedBlocks() == 0);
    assert(pool.freeBlocks() == 16);

    std::cout << "[PASS] test_basic_alloc_dealloc" << std::endl;
}

void test_auto_grow() {
    FixedMemoryPool pool(4);  // 初始 4 块，扩容也是 4 块

    // 分配 4 块，耗尽初始 chunk
    std::vector<void*> ptrs;
    for (int i = 0; i < 4; ++i) {
        ptrs.push_back(pool.allocate());
    }
    assert(pool.freeBlocks() == 0);
    assert(pool.chunkCount() == 1);

    // 第 5 次分配触发扩容
    void* p5 = pool.allocate();
    assert(p5 != nullptr);
    assert(pool.chunkCount() == 2);
    assert(pool.totalBlocks() == 8);
    ptrs.push_back(p5);

    // 全部释放
    for (auto* p : ptrs) {
        pool.deallocate(p);
    }
    assert(pool.usedBlocks() == 0);
    assert(pool.freeBlocks() == 8);

    std::cout << "[PASS] test_auto_grow" << std::endl;
}

void test_nullptr_dealloc() {
    FixedMemoryPool pool(4);
    pool.deallocate(nullptr);  // 安全不崩溃
    assert(pool.freeBlocks() == 4);

    std::cout << "[PASS] test_nullptr_dealloc" << std::endl;
}

void test_reuse() {
    // 分配→释放→再分配，应该拿到同一块内存（LIFO）
    FixedMemoryPool pool(4);
    void* p1 = pool.allocate();
    pool.deallocate(p1);
    void* p2 = pool.allocate();
    assert(p1 == p2);  // freelist LIFO

    pool.deallocate(p2);
    std::cout << "[PASS] test_reuse" << std::endl;
}

void bench_alloc_dealloc() {
    FixedMemoryPool pool(4096);
    constexpr int N = 1'000'000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; ++i) {
        void* p = pool.allocate();
        pool.deallocate(p);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    std::cout << "[BENCH] " << N << " alloc+dealloc cycles: "
              << us << " us (" << (double)us / N * 1000 << " ns/op)" << std::endl;

    // 对比 malloc/free
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; ++i) {
        void* p = ::malloc(1500);
        ::free(p);
    }
    end = std::chrono::high_resolution_clock::now();
    us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    std::cout << "[BENCH] " << N << " malloc+free cycles:    "
              << us << " us (" << (double)us / N * 1000 << " ns/op)" << std::endl;
}

int main() {
    test_basic_alloc_dealloc();
    test_auto_grow();
    test_nullptr_dealloc();
    test_reuse();
    bench_alloc_dealloc();

    std::cout << "\n✅ All tests passed!" << std::endl;
    return 0;
}
