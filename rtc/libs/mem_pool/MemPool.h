// MemPool.h - 单线程定长内存池
//
// 用途：UDP 收发流程中频繁分配/释放固定大小的缓冲区，
//      用内存池避免反复 malloc/free 带来的开销与碎片。
//
// 特性：
//   - 块大小固定为 kBlockSize（默认 1500 字节，覆盖常见 UDP MTU）
//   - 单线程，无锁
//   - 按 chunk 批量增长，每个 chunk 包含 N 个 block
//   - 释放只是挂回 freelist，不归还给系统
//   - 池销毁时一次性释放所有 chunk
//
// 用法：
//   mempool::FixedMemoryPool pool;          // 默认每 chunk 256 块
//   void* p = pool.allocate();              // 拿 1500 字节
//   ...
//   pool.deallocate(p);                     // 放回池中
//
// 注意：deallocate 必须传入 allocate 返回的指针，且只能归还到原池。
//      池析构后，所有发出去未归还的指针都会失效。

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mempool {

class FixedMemoryPool {
public:
    // 单块大小（字节）：1500 = 常见 UDP MTU
    static constexpr size_t kBlockSize = 1500;

    // initial_blocks      ：构造时预分配的块数（也是后续每次扩容的步长）
    // grow_blocks         ：扩容时一次新增多少块（0 表示用 initial_blocks）
    explicit FixedMemoryPool(size_t initial_blocks = 256,
                             size_t grow_blocks = 0);

    ~FixedMemoryPool();

    // 不可拷贝、不可移动（持有的裸指针所有权语义复杂）
    FixedMemoryPool(const FixedMemoryPool&) = delete;
    FixedMemoryPool& operator=(const FixedMemoryPool&) = delete;

    // 分配一块 kBlockSize 字节的内存。
    // 当前空闲链表为空时会自动扩容（再分配一个 chunk）。
    // 返回值非空。
    void* allocate();

    // 归还一块内存，p 必须来自 allocate()。传 nullptr 安全（直接忽略）。
    void deallocate(void* p);

    // ── 统计接口 ──────────────────────────────────────────
    size_t blockSize() const { return kBlockSize; }
    size_t totalBlocks() const { return m_total_blocks; }   // 池中总块数
    size_t freeBlocks() const { return m_free_blocks; }     // 当前空闲块数
    size_t usedBlocks() const { return m_total_blocks - m_free_blocks; }
    size_t chunkCount() const { return m_chunks.size(); }   // 已扩容次数

private:
    // 空闲链表节点，复用块的前 sizeof(FreeNode*) 字节
    struct FreeNode {
        FreeNode* next;
    };

    // 向系统申请一个 chunk，切成 grow_blocks 个块挂到 freelist
    void grow();

    FreeNode*               m_free_list = nullptr;  // 空闲链表头
    std::vector<uint8_t*>   m_chunks;               // 持有所有 chunk 起始指针，析构时统一 delete[]

    size_t                  m_grow_blocks = 0;
    size_t                  m_total_blocks = 0;
    size_t                  m_free_blocks  = 0;
};

} // namespace mempool
