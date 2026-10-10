#ifndef SFU_KV_STORE_H
#define SFU_KV_STORE_H

#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <optional>
#include <chrono>
#include <functional>
#include <cstdint>

namespace sfu {

// Key-Value entry with optional TTL
struct KvEntry {
    std::string value;
    std::chrono::steady_clock::time_point created_at;
    std::optional<std::chrono::milliseconds> ttl;

    bool isExpired() const {
        if (!ttl.has_value()) return false;
        return std::chrono::steady_clock::now() - created_at > ttl.value();
    }
};

// Thread-safe Key-Value store for SFU session management
class KvStore {
public:
    static KvStore& Instance();

    // Set a value (with optional TTL)
    void Set(const std::string& key, const std::string& value,
             std::optional<std::chrono::milliseconds> ttl = std::nullopt);

    // Get a value (returns nullopt if not found or expired)
    std::optional<std::string> Get(const std::string& key);

    // Delete a key
    bool Delete(const std::string& key);

    // Check if key exists (and not expired)
    bool Exists(const std::string& key);

    // Get all keys matching a prefix
    std::vector<std::string> KeysWithPrefix(const std::string& prefix);

    // Clean up expired entries
    size_t CleanupExpired();

    // Get store size (including expired)
    size_t Size() const;

private:
    KvStore() = default;

    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, KvEntry> m_store;
};

// ===== Session Management Helpers =====

// Session info stored in KV（流模型：streamId 即流名，全局唯一）
struct SessionInfo {
    uint32_t sessionId = 0;
    std::string userId;
    std::string streamId;
    bool subscribeAudio = true;
    bool subscribeVideo = true;
    int64_t createdAt = 0;

    std::string Serialize() const;
    static std::optional<SessionInfo> Deserialize(const std::string& data);
};

// Session manager using KvStore
class SessionManager {
public:
    // Create a new session, returns assigned sessionId
    static uint32_t CreateSession(const SessionInfo& info);

    // Get session by ID
    static std::optional<SessionInfo> GetSession(uint32_t sessionId);

    // Get session by userId + streamId
    static std::optional<SessionInfo> GetSession(const std::string& userId, const std::string& streamId);

    // Update session
    static bool UpdateSession(uint32_t sessionId, const SessionInfo& info);

    // Delete session
    static bool DeleteSession(uint32_t sessionId);

    // Find all sessions of a stream（publisher + subscribers）
    static std::vector<SessionInfo> GetStreamSessions(const std::string& streamId);

    // Find sessions that a user should subscribe to
    static std::vector<SessionInfo> GetSubscribeTargets(uint32_t sessionId);

private:
    static std::string SessionKey(uint32_t sessionId);
    static std::string UserStreamKey(const std::string& userId, const std::string& streamId);
};

} // namespace sfu

#endif // SFU_KV_STORE_H
