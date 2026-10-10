#pragma once

#include <cstdint>
#include <unordered_set>
#include <mutex>
#include <deque>

namespace p2p {

class P2PDeduplicator {
public:
    explicit P2PDeduplicator(size_t maxEntries = 10000)
        : m_maxEntries(maxEntries) {}

    bool seen(uint64_t seqNum) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto [it, inserted] = m_seenSet.insert(seqNum);
        if (!inserted) return true;

        m_seenQueue.push_back(seqNum);
        if (m_seenQueue.size() > m_maxEntries) {
            m_seenSet.erase(m_seenQueue.front());
            m_seenQueue.pop_front();
        }
        return false;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_seenSet.clear();
        m_seenQueue.clear();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_seenSet.size();
    }

private:
    mutable std::mutex m_mutex;
    std::unordered_set<uint64_t> m_seenSet;
    std::deque<uint64_t> m_seenQueue;
    size_t m_maxEntries;
};

} // namespace p2p
