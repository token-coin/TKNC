#include <net/backend_router.h>
#include <util/log.h>
#include <chrono>

BackendRouter::BackendRouter() = default;

void BackendRouter::Register(std::unique_ptr<ComputeBackend> backend)
{
    if (!backend) return;
    std::string id = backend->GetId();

    // Replace existing if duplicate ID
    auto it = m_backends.begin();
    while (it != m_backends.end()) {
        if ((*it)->GetId() == id) {
            LogInfo("[BACKEND-ROUTER] Replacing backend: %s", id.c_str());
            it = m_backends.erase(it);
        } else {
            ++it;
        }
    }

    m_backends.push_back(std::move(backend));
    LogInfo("[BACKEND-ROUTER] Registered backend: %s (total=%zu)", id.c_str(), m_backends.size());
}

bool BackendRouter::Unregister(const std::string& id)
{
    auto it = m_backends.begin();
    while (it != m_backends.end()) {
        if ((*it)->GetId() == id) {
            it = m_backends.erase(it);
            LogInfo("[BACKEND-ROUTER] Unregistered backend: %s (total=%zu)", id.c_str(), m_backends.size());
            return true;
        }
        ++it;
    }
    return false;
}

size_t BackendRouter::AvailableCount() const
{
    size_t count = 0;
    for (const auto& b : m_backends) {
        if (b->IsAvailable()) count++;
    }
    return count;
}

std::vector<BackendRouter::BackendStatus> BackendRouter::GetAllStatus() const
{
    std::vector<BackendStatus> result;
    for (const auto& b : m_backends) {
        result.push_back({
            b->GetId(),
            b->GetName(),
            b->CheckHealth(),
            b->IsAvailable(),
            b->GetPriority()
        });
    }
    return result;
}

ComputeBackend* BackendRouter::SelectBackend(const ComputeRequest& request) const
{
    if (m_backends.empty()) return nullptr;

    switch (m_strategy) {
    case RoutingStrategy::LOCAL_FIRST: {
        // Sort by priority descending, pick first available that supports model
        ComputeBackend* best = nullptr;
        int best_priority = -1;
        for (const auto& b : m_backends) {
            if (!b->IsAvailable()) continue;
            if (!b->SupportsModel(request.model)) continue;
            if (b->GetPriority() > best_priority) {
                best_priority = b->GetPriority();
                best = b.get();
            }
        }
        return best;
    }

    case RoutingStrategy::ROUND_ROBIN: {
        // Cycle through available backends
        size_t start = m_round_robin_counter % m_backends.size();
        for (size_t i = 0; i < m_backends.size(); i++) {
            size_t idx = (start + i) % m_backends.size();
            if (m_backends[idx]->IsAvailable()) {
                m_round_robin_counter++;
                return m_backends[idx].get();
            }
        }
        return nullptr;  // None available
    }

    case RoutingStrategy::LATENCY_BASED:
    case RoutingStrategy::SCORE_BASED:
        // Future: implement latency tracking and weighted scoring
        // For now, fall through to LOCAL_FIRST behavior
        break;
    }

    // Default fallback: LOCAL_FIRST
    return SelectBackend(request);
}

ComputeResponse BackendRouter::Dispatch(const ComputeRequest& request)
{
    ComputeResponse response;

    // Phase 1: Try primary selection
    ComputeBackend* primary = SelectBackend(request);
    if (primary) {
        LogInfo("[BACKEND-ROUTER] Dispatching to backend=%s (strategy=%d)",
                primary->GetId().c_str(), (int)m_strategy);

        auto start_time = std::chrono::steady_clock::now();
        response = primary->Infer(request);
        double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_time).count();
        response.latency_ms = elapsed;
        response.backend_id = primary->GetId();

        if (response.success) {
            LogInfo("[BACKEND-ROUTER] Success: backend=%s tokens=%d latency=%.1fms",
                    response.backend_id.c_str(),
                    (int)response.tokens_used,
                    response.latency_ms);
            return response;
        }

        LogInfo("[BACKEND-ROUTER] Primary failed (%s), trying fallback...",
                response.error_message.c_str());
    } else {
        LogInfo("[BACKEND-ROUTER] No backend available (registered=%zu, strategy=%d)",
                m_backends.size(), (int)m_strategy);
    }

    // Phase 2: Fallback chain — try every other available backend
    for (auto& b : m_backends) {
        if (b.get() == primary) continue;  // Already tried
        if (!b->IsAvailable()) continue;

        LogInfo("[BACKEND-ROUTER] Fallback to backend=%s", b->GetId().c_str());
        auto start_time = std::chrono::steady_clock::now();
        response = b->Infer(request);
        double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_time).count();
        response.latency_ms = elapsed;
        response.backend_id = b->GetId();

        if (response.success) {
            LogInfo("[BACKEND-ROUTER] Fallback success: backend=%s tokens=%d latency=%.1fms",
                    response.backend_id.c_str(),
                    (int)response.tokens_used,
                    response.latency_ms);
            return response;
        }
    }

    // All backends exhausted
    if (m_backends.empty()) {
        response.success = false;
        response.error_message = "No compute backends registered in router";
        response.health = HealthStatus::OFFLINE;
    } else {
        response.success = false;
        response.error_message = "All compute backends exhausted or unavailable";
        response.health = HealthStatus::UNHEALTHY;
    }

    return response;
}
