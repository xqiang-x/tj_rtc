// MemPool.cpp - 单线程定长内存池实现

#include "MemPool.h"

#include <cassert>
#include <new>      // std::bad_alloc

namespace mempool {

FixedMemoryPool::FixedMemoryPool(size_t initial_blocks, size_t grow_blocks) {
    if (initial_blocks == 0) initial_blocks = 256;
    m_grow_blocks = (grow_blocks == 0) ? initial_blocks : grow_blocks;

    // 临时把扩容步长设成 initial_blocks，预分配第一个 chunk
    size_t saved_grow = m_grow_blocks;
    m_grow_blocks = initial_blocks;
    grow();
    m_grow_blocks = saved_grow;
}

FixedMemoryPool::~FixedMemoryPool() {
    // 一次性释放所有 chunk，发出去未归还的指针在此刻失效
    for (auto* chunk : m_chunks) {
        ::operator delete[](chunk);
    }
    m_chunks.clear();
    m_free_list = nullptr;
    m_total_blocks = 0;
    m_free_blocks  = 0;
}

void FixedMemoryPool::grow() {
    // 块大小至少要能放下一个 FreeNode 指针（编译期常量，永远成立）
    static_assert(kBlockSize >= sizeof(FreeNode),
                  "kBlockSize must be large enough for free-list node");

    const size_t n = m_grow_blocks;
    // 用全局 operator new 拿原始字节，避免触发对象构造/析构
    uint8_t* chunk = static_cast<uint8_t*>(
        ::operator new[](n * kBlockSize));
    if (!chunk) {
        throw std::bad_alloc();
    }
    m_chunks.push_back(chunk);

    // 把新 chunk 切片，依次挂到 freelist 头部
    // 头插法：新增块的链表顺序与遍历顺序无关，无需关心
    for (size_t i = 0; i < n; ++i) {
        auto* node = reinterpret_cast<FreeNode*>(chunk + i * kBlockSize);
        node->next  = m_free_list;
        m_free_list = node;
    }

    m_total_blocks += n;
    m_free_blocks  += n;
}

void* FixedMemoryPool::allocate() {
    if (m_free_list == nullptr) {
        grow();   // 自动扩容，扩完保证 m_free_list 非空
    }

    FreeNode* node = m_free_list;
    m_free_list    = node->next;
    --m_free_blocks;

    // 返回原始字节空间。调用者负责按需写入；前 8 字节（FreeNode::next）
    // 在归还前由调用者随意使用。
    return static_cast<void*>(node);
}

void FixedMemoryPool::deallocate(void* p) {
    if (p == nullptr) return;

    auto* node  = static_cast<FreeNode*>(p);
    node->next  = m_free_list;
    m_free_list = node;
    ++m_free_blocks;

    // 调试断言：归还数不应超过总数（理论上发生即说明被 double-free 或外来指针）
    assert(m_free_blocks <= m_total_blocks && "double free or alien pointer");
}

} // namespace mempool
