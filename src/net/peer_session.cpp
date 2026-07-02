#include <net/peer_session.h>
#include <util/log.h>

#include <algorithm>

static int64_t NowSeconds()
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// ============================================================
// SessionState string
// ============================================================
const char* SessionStateToString(SessionState s)
{
    switch (s) {
        case SessionState::NONE:        return "NONE";
        case SessionState::ESTABLISHED:  return "ESTABLISHED";
        case SessionState::ACTIVE:       return "ACTIVE";
        case SessionState::IDLE:         return "IDLE";
        case SessionState::EXPIRED:      return "EXPIRED";
        case SessionState::FAULT:        return "FAULT";
        default:                         return "UNKNOWN";
    }
}

// ============================================================
// PeerSession methods
// ============================================================
PeerSession::PeerSession(const std::string& pid, const std::string& ep)
    : peer_id(pid), peer_endpoint(ep)
{
    int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    created_at = now;
    last_activity = now;
    last_heartbeat = now;
    state = SessionState::ESTABLISHED;
}

void PeerSession::MarkActive()
{
    last_activity = NowSeconds();
    state = SessionState::ACTIVE;
}

void PeerSession::MarkIdle()
{
    state = SessionState::IDLE;
}

void PeerSession::MarkError(const std::string& /*reason*/)
{
    state = SessionState::FAULT;
}

void PeerSession::MarkExpired()
{
    state = SessionState::EXPIRED;
}

bool PeerSession::IsAlive() const
{
    SessionState s = state.load();
    return s == SessionState::ACTIVE || s == SessionState::ESTABLISHED ||
           s == SessionState::IDLE;
}

bool PeerSession::IsUsable() const
{
    SessionState s = state.load();
    return s == SessionState::ESTABLISHED || s == SessionState::ACTIVE || s == SessionState::IDLE;
}

int64_t PeerSession::IdleSeconds(int64_t now) const
{
    return now - last_activity;
}

std::string PeerSession::ToString() const
{
    char buf[512];
    snprintf(buf, sizeof(buf),
        "session{peer=%s ep=%s state=%s udp=%d tcp=%d reqs=%u lat=%.1fms idle=%llds}",
        peer_id.c_str(), peer_endpoint.c_str(),
        SessionStateToString(state.load()),
        (int)udp_punch_used, (int)tcp_fallback_used,
        requests_served, avg_latency_ms,
        (long long)IdleSeconds(NowSeconds()));
    return std::string(buf);
}

// ============================================================
// PeerSessionManager methods
// ============================================================
PeerSessionManager::PeerSessionManager()
{
    LogInfo("[SESSION-MGR] Initialized");
}

PeerSession* PeerSessionManager::CreateSession(
    const std::string& peer_id,
    const std::string& peer_endpoint,
    bool udp_punch,
    bool tcp_fallback)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_sessions.find(peer_id);
    if (it != m_sessions.end()) {
        // Update existing session
        it->second.peer_endpoint = peer_endpoint;
        it->second.udp_punch_used = udp_punch;
        it->second.tcp_fallback_used = tcp_fallback;
        it->second.MarkActive();
        LogInfo("[SESSION-MGR] Updated session: %s", peer_id.c_str());
        return &it->second;
    }

    auto result = m_sessions.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(peer_id),
        std::forward_as_tuple(peer_id, peer_endpoint));

    PeerSession& sess = result.first->second;
    sess.udp_punch_used = udp_punch;
    sess.tcp_fallback_used = tcp_fallback;

    LogInfo("[SESSION-MGR] Created session: %s -> %s (udp=%d tcp=%d)",
           peer_id.c_str(), peer_endpoint.c_str(),
           (int)udp_punch, (int)tcp_fallback);

    return &sess;
}

PeerSession* PeerSessionManager::GetOrCreateSession(
    const std::string& peer_id,
    const std::string& peer_endpoint,
    bool udp_punch,
    bool tcp_fallback)
{
    // 1. Try reuse existing alive session
    PeerSession* existing = GetSession(peer_id);
    if (existing && existing->IsUsable()) {
        existing->MarkActive();
        LogInfo("[SESSION-MGR] Reusing session: %s", peer_id.c_str());
        return existing;
    }

    // 2. Existing but dead/expired — remove and recreate
    if (existing) {
        RemoveSession(peer_id);
        LogInfo("[SESSION-MGR] Recreating dead session: %s", peer_id.c_str());
    }

    // 3. Create fresh session
    return CreateSession(peer_id, peer_endpoint, udp_punch, tcp_fallback);
}

bool PeerSessionManager::RemoveSession(const std::string& peer_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(peer_id);
    if (it != m_sessions.end()) {
        m_sessions.erase(it);
        LogInfo("[SESSION-MGR] Removed session: %s", peer_id.c_str());
        return true;
    }
    return false;
}

PeerSession* PeerSessionManager::GetSession(const std::string& peer_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(peer_id);
    if (it != m_sessions.end()) return &it->second;
    return nullptr;
}

const PeerSession* PeerSessionManager::GetSession(const std::string& peer_id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(peer_id);
    if (it != m_sessions.end()) return &it->second;
    return nullptr;
}

size_t PeerSessionManager::CountByState(SessionState target_state) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t count = 0;
    for (const auto& [id, sess] : m_sessions) {
        if (sess.state.load() == target_state) ++count;
    }
    return count;
}

size_t PeerSessionManager::TotalCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sessions.size();
}

std::vector<PeerSession*> PeerSessionManager::GetUsableSessions()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<PeerSession*> result;
    for (auto& [id, sess] : m_sessions) {
        if (sess.IsUsable()) result.push_back(&sess);
    }
    return result;
}

PeerSession* PeerSessionManager::SelectBest(const std::string& /*model_hint*/)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    PeerSession* best = nullptr;
    int64_t best_activity = 0;

    for (auto& [id, sess] : m_sessions) {
        if (!sess.IsUsable()) continue;
        // Prefer ACTIVE over IDLE, then most recently active
        if (!best ||
            (sess.state.load() == SessionState::ACTIVE && best->state.load() != SessionState::ACTIVE) ||
            (sess.last_activity > best_activity)) {
            best = &sess;
            best_activity = sess.last_activity;
        }
    }

    return best;
}

int PeerSessionManager::ExpireIdleSessions(int max_idle_seconds)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    int64_t now = NowSeconds();
    int expired_count = 0;

    for (auto& [id, sess] : m_sessions) {
        if (sess.IsAlive() && sess.IdleSeconds(now) > max_idle_seconds) {
            sess.MarkExpired();
            ++expired_count;
            LogInfo("[SESSION-MGR] Expired idle session: %s (idle %llds)",
                   id.c_str(), (long long)sess.IdleSeconds(now));
        }
    }

    if (expired_count > 0) {
        LogInfo("[SESSION-MGR] Expired %d sessions (max_idle=%ds)", expired_count, max_idle_seconds);
    }

    return expired_count;
}

int PeerSessionManager::HeartbeatAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    int sent = 0;
    int64_t now = NowSeconds();

    for (auto& [id, sess] : m_sessions) {
        if (sess.state.load() != SessionState::ACTIVE) continue;

        // If no activity for a while, mark as IDLE
        if (sess.IdleSeconds(now) > 30) {
            sess.MarkIdle();
            continue;
        }

        sess.last_heartbeat = now;
        ++sent;
    }

    return sent;
}

std::string PeerSessionManager::GetStatusSummary() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    char buf[256];
    snprintf(buf, sizeof(buf),
        "sessions{total=%zu active=%zu idle=%zu expired=%zu error=%zu}",
        m_sessions.size(),
        CountByState(SessionState::ACTIVE),
        CountByState(SessionState::IDLE),
        CountByState(SessionState::EXPIRED),
        CountByState(SessionState::FAULT));

    return std::string(buf);
}
