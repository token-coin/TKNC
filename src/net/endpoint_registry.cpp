#include <net/endpoint_registry.h>
#include <util/log.h>

#include <algorithm>

// ============================================================
// EndpointEntry methods
// ============================================================
int64_t EndpointEntry::AgeSeconds() const
{
    int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    return now - last_seen;
}

bool EndpointEntry::IsValid(int64_t max_age_seconds) const
{
    return AgeSeconds() <= max_age_seconds;
}

std::string EndpointEntry::ToString() const
{
    char buf[384];
    snprintf(buf, sizeof(buf),
        "Endpoint{id=%s %s:%d api=%d model=%s age=%llds "
        "reach={direct=%d punch=%d tcp=%d} score=%d}",
        node_id.c_str(), ip.c_str(), p2p_port, api_port,
        model_name.c_str(), (long long)AgeSeconds(),
        (int)reachable_directly, (int)reachable_via_punch,
        (int)reachable_via_tcp, quality_score);
    return std::string(buf);
}

// ============================================================
// EndpointRegistry methods
// ============================================================
EndpointRegistry::EndpointRegistry()
{
    m_self_entry = std::make_unique<EndpointEntry>();
    m_self_entry->self_reported = true;
    LogInfo("[EP-REG] Initialized");
}

void EndpointRegistry::Register(const EndpointEntry& entry)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    int64_t now = NowSeconds();
    auto it = m_entries.find(entry.node_id);
    if (it != m_entries.end()) {
        // Update existing entry, preserve reachability data
        it->second.ip = entry.ip;
        it->second.p2p_port = entry.p2p_port;
        it->second.api_port = entry.api_port > 0 ? entry.api_port : it->second.api_port;
        if (!entry.model_name.empty()) it->second.model_name = entry.model_name;
        it->second.last_seen = now;
        LogInfo("[EP-REG] Updated: %s -> %s:%d", entry.node_id.c_str(),
               entry.ip.c_str(), entry.p2p_port);
    } else {
        EndpointEntry new_entry = entry;
        new_entry.registered_at = now;
        new_entry.last_seen = now;
        m_entries.emplace(entry.node_id, std::move(new_entry));
        LogInfo("[EP-REG] Registered: %s -> %s:%d", entry.node_id.c_str(),
               entry.ip.c_str(), entry.p2p_port);
    }
}

void EndpointRegistry::Register(const std::string& node_id, const std::string& ip,
                                uint16_t p2p_port, uint16_t api_port)
{
    EndpointEntry e;
    e.node_id = node_id;
    e.ip = ip;
    e.p2p_port = p2p_port;
    e.api_port = api_port;
    Register(e);
}

void EndpointRegistry::Unregister(const std::string& node_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(node_id);
    if (it != m_entries.end()) {
        m_entries.erase(it);
        LogInfo("[EP-REG] Unregistered: %s", node_id.c_str());
    }
}

const EndpointEntry* EndpointRegistry::Find(const std::string& node_id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(node_id);
    if (it != m_entries.end()) return &it->second;
    return nullptr;
}

EndpointEntry* EndpointRegistry::Find(const std::string& node_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(node_id);
    if (it != m_entries.end()) return &it->second;
    return nullptr;
}

std::vector<const EndpointEntry*> EndpointRegistry::FindByModel(const std::string& model_hint) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<const EndpointEntry*> result;

    for (const auto& [id, entry] : m_entries) {
        if (!entry.IsValid()) continue;
        if (model_hint.empty() || entry.model_name.find(model_hint) != std::string::npos) {
            result.push_back(&entry);
        }
    }

    return result;
}

size_t EndpointRegistry::TotalCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.size();
}

size_t EndpointRegistry::OnlineCount(int64_t max_age_seconds) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t count = 0;
    for (const auto& [id, entry] : m_entries) {
        if (entry.IsValid(max_age_seconds)) ++count;
    }
    return count;
}

std::vector<const EndpointEntry*> EndpointRegistry::GetReachableEndpoints() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<const EndpointEntry*> result;

    for (const auto& [id, entry] : m_entries) {
        if (!entry.IsValid()) continue;
        if (entry.reachable_directly || entry.reachable_via_punch || entry.reachable_via_tcp) {
            result.push_back(&entry);
        }
    }

    return result;
}

const EndpointEntry* EndpointRegistry::SelectBest(const std::string& model_hint) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    const EndpointEntry* best = nullptr;
    int best_score = -1;

    for (const auto& [id, entry] : m_entries) {
        if (!entry.IsValid()) continue;
        if (entry.self_reported) continue;  // Don't route to ourselves

        // Model filter
        if (!model_hint.empty() && !entry.model_name.empty() &&
            entry.model_name.find(model_hint) == std::string::npos) {
            continue;
        }

        // Scoring: prefer direct reachability, then punch, then tcp
        int score = entry.quality_score;
        if (entry.reachable_directly) score += 1000;
        if (entry.reachable_via_punch)  score += 500;
        if (entry.reachable_via_tcp)   score += 200;
        // Prefer recently seen
        score += std::max<int64_t>(0LL, 300LL - entry.AgeSeconds());

        if (score > best_score) {
            best_score = score;
            best = &entry;
        }
    }

    if (best) {
        LogInfo("[EP-REG] Selected best endpoint: %s (score=%d)",
               best->node_id.c_str(), best_score);
    }

    return best;
}

int EndpointRegistry::CleanupStaleEntries(int64_t max_age_seconds)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    int removed = 0;
    auto it = m_entries.begin();
    while (it != m_entries.end()) {
        if (!it->second.IsValid(max_age_seconds)) {
            LogInfo("[EP-REG] Cleaning stale: %s (age=%llds)",
                   it->first.c_str(), (long long)it->second.AgeSeconds());
            it = m_entries.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }

    if (removed > 0) {
        LogInfo("[EP-REG] Cleaned %d stale entries (max_age=%llds)",
               removed, (long long)max_age_seconds);
    }

    return removed;
}

void EndpointRegistry::SetSelfEndpoint(const std::string& ip, uint16_t p2p_port, uint16_t api_port)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    int64_t now = NowSeconds();
    m_self_entry->ip = ip;
    m_self_entry->p2p_port = p2p_port;
    m_self_entry->api_port = api_port;
    m_self_entry->last_seen = now;
    m_self_entry->reachable_directly = true;  // We can always reach ourselves

    LogInfo("[EP-REG] Self endpoint set: %s:%d (api:%d)", ip.c_str(), p2p_port, api_port);
}

void EndpointRegistry::UpdateReachability(const std::string& node_id,
                                           bool direct, bool punch, bool tcp)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(node_id);
    if (it != m_entries.end()) {
        it->second.reachable_directly = direct;
        it->second.reachable_via_punch = punch;
        it->second.reachable_via_tcp = tcp;
        it->second.last_seen = NowSeconds();
    }
}

void EndpointRegistry::UpdateQualityScore(const std::string& node_id, int delta)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(node_id);
    if (it != m_entries.end()) {
        it->second.quality_score += delta;
        // Clamp to reasonable range
        if (it->second.quality_score < -100) it->second.quality_score = -100;
        if (it->second.quality_score > 1000) it->second.quality_score = 1000;
    }
}

std::string EndpointRegistry::GetStatusSummary() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    size_t total = m_entries.size();
    size_t online = 0, reachable = 0;
    for (const auto& [id, entry] : m_entries) {
        if (entry.IsValid(300)) ++online;
        if (entry.reachable_directly || entry.reachable_via_punch || entry.reachable_via_tcp)
            ++reachable;
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
        "EndpointReg{total=%zu online=%zu reachable=%zu self=%s:%d}",
        total, online, reachable,
        m_self_entry->ip.c_str(), m_self_entry->p2p_port);

    return std::string(buf);
}

int64_t EndpointRegistry::NowSeconds()
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}
