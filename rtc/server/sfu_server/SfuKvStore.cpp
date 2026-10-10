#include "SfuKvStore.h"
#include <sstream>
#include <algorithm>
#include <mutex>
#include <atomic>

namespace sfu {

// ===== KvStore Implementation =====

KvStore& KvStore::Instance() {
    static KvStore instance;
    return instance;
}

void KvStore::Set(const std::string& key, const std::string& value,
                  std::optional<std::chrono::milliseconds> ttl) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    KvEntry entry;
    entry.value = value;
    entry.created_at = std::chrono::steady_clock::now();
    entry.ttl = ttl;
    m_store[key] = std::move(entry);
}

std::optional<std::string> KvStore::Get(const std::string& key) {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_store.find(key);
    if (it == m_store.end()) {
        return std::nullopt;
    }
    if (it->second.isExpired()) {
        return std::nullopt;
    }
    return it->second.value;
}

bool KvStore::Delete(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    return m_store.erase(key) > 0;
}

bool KvStore::Exists(const std::string& key) {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_store.find(key);
    if (it == m_store.end()) return false;
    return !it->second.isExpired();
}

std::vector<std::string> KvStore::KeysWithPrefix(const std::string& prefix) {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    std::vector<std::string> result;
    for (const auto& [key, entry] : m_store) {
        if (!entry.isExpired() && key.rfind(prefix, 0) == 0) {
            result.push_back(key);
        }
    }
    return result;
}

size_t KvStore::CleanupExpired() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    size_t count = 0;
    for (auto it = m_store.begin(); it != m_store.end(); ) {
        if (it->second.isExpired()) {
            it = m_store.erase(it);
            ++count;
        } else {
            ++it;
        }
    }
    return count;
}

size_t KvStore::Size() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_store.size();
}

// ===== SessionInfo Serialization =====
// Format: sessionId|userId|streamId|subAudio|subVideo|createdAt

std::string SessionInfo::Serialize() const {
    std::ostringstream oss;
    oss << sessionId << "|"
        << userId << "|"
        << streamId << "|"
        << (subscribeAudio ? '1' : '0') << "|"
        << (subscribeVideo ? '1' : '0') << "|"
        << createdAt;

    return oss.str();
}

std::optional<SessionInfo> SessionInfo::Deserialize(const std::string& data) {
    SessionInfo info;
    std::istringstream iss(data);
    std::string token;

    // sessionId
    if (!std::getline(iss, token, '|')) return std::nullopt;
    info.sessionId = std::stoul(token);

    // userId
    if (!std::getline(iss, token, '|')) return std::nullopt;
    info.userId = token;

    // streamId
    if (!std::getline(iss, token, '|')) return std::nullopt;
    info.streamId = token;

    // subscribeAudio
    if (!std::getline(iss, token, '|')) return std::nullopt;
    info.subscribeAudio = (token == "1");

    // subscribeVideo
    if (!std::getline(iss, token, '|')) return std::nullopt;
    info.subscribeVideo = (token == "1");

    // createdAt
    if (!std::getline(iss, token, '|')) return std::nullopt;
    info.createdAt = std::stoll(token);

    return info;
}

// ===== SessionManager Implementation =====

std::string SessionManager::SessionKey(uint32_t sessionId) {
    return "session:" + std::to_string(sessionId);
}

std::string SessionManager::UserStreamKey(const std::string& userId, const std::string& streamId) {
    return "userstream:" + userId + ":" + streamId;
}

uint32_t SessionManager::CreateSession(const SessionInfo& info) {
    // Generate unique 32-bit session ID
    static std::atomic<uint32_t> s_sessionCounter{1000};
    uint32_t sessionId = s_sessionCounter.fetch_add(1);

    SessionInfo sessionInfo = info;
    sessionInfo.sessionId = sessionId;
    sessionInfo.createdAt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::string data = sessionInfo.Serialize();

    // Store by session ID
    KvStore::Instance().Set(SessionKey(sessionId), data, std::chrono::hours(24));

    // Store userId -> streamId mapping for lookup
    KvStore::Instance().Set(UserStreamKey(info.userId, info.streamId),
                            std::to_string(sessionId),
                            std::chrono::hours(24));

    return sessionId;
}

std::optional<SessionInfo> SessionManager::GetSession(uint32_t sessionId) {
    auto value = KvStore::Instance().Get(SessionKey(sessionId));
    if (!value.has_value()) return std::nullopt;
    return SessionInfo::Deserialize(value.value());
}

std::optional<SessionInfo> SessionManager::GetSession(const std::string& userId, const std::string& streamId) {
    auto sessionIdStr = KvStore::Instance().Get(UserStreamKey(userId, streamId));
    if (!sessionIdStr.has_value()) return std::nullopt;
    return GetSession(std::stoul(sessionIdStr.value()));
}

bool SessionManager::UpdateSession(uint32_t sessionId, const SessionInfo& info) {
    auto existing = GetSession(sessionId);
    if (!existing.has_value()) return false;

    std::string data = info.Serialize();
    KvStore::Instance().Set(SessionKey(sessionId), data, std::chrono::hours(24));
    KvStore::Instance().Set(UserStreamKey(info.userId, info.streamId),
                            std::to_string(sessionId),
                            std::chrono::hours(24));
    return true;
}

bool SessionManager::DeleteSession(uint32_t sessionId) {
    auto session = GetSession(sessionId);
    if (!session.has_value()) return false;

    KvStore::Instance().Delete(SessionKey(sessionId));
    KvStore::Instance().Delete(UserStreamKey(session->userId, session->streamId));
    return true;
}

std::vector<SessionInfo> SessionManager::GetStreamSessions(const std::string& streamId) {
    std::vector<SessionInfo> result;
    auto keys = KvStore::Instance().KeysWithPrefix("session:");

    for (const auto& key : keys) {
        auto value = KvStore::Instance().Get(key);
        if (value.has_value()) {
            auto session = SessionInfo::Deserialize(value.value());
            if (session.has_value() && session->streamId == streamId) {
                result.push_back(session.value());
            }
        }
    }

    return result;
}

std::vector<SessionInfo> SessionManager::GetSubscribeTargets(uint32_t sessionId) {
    auto session = GetSession(sessionId);
    if (!session.has_value()) return {};

    // 流模型：同一流上的其它会话（publisher 与 subscribers）
    auto streamSessions = GetStreamSessions(session->streamId);

    std::vector<SessionInfo> targets;
    for (const auto& other : streamSessions) {
        // Skip self
        if (other.sessionId == sessionId) continue;
        targets.push_back(other);
    }

    return targets;
}

} // namespace sfu
