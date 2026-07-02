#ifndef TKN_NET_BACKEND_ROUTER_H
#define TKN_NET_BACKEND_ROUTER_H

#include <net/compute_backend.h>
#include <memory>
#include <vector>
#include <string>

/**
 * Routing strategy for selecting which backend handles a request.
 */
enum class RoutingStrategy : int {
    LOCAL_FIRST = 0,    // Prefer local backends, fall back to remote
    ROUND_ROBIN = 1,    // Cycle through available backends evenly
    LATENCY_BASED = 2,  // Pick backend with lowest recent latency (future)
    SCORE_BASED = 3     // Weighted scoring: priority + health + latency (future)
};

/**
 * BackendRouter — Central dispatcher for all LLM compute requests.
 *
 * Owns registered ComputeBackend instances.
 * Selects the best backend per request using configurable strategy.
 * Provides fallback chain: primary → secondary → ... → error response.
 *
 * Usage in node (tkncd):
 *   BackendRouter router;
 *   router.Register(std::make_unique<LocalBackend>());
 *   router.SetStrategy(RoutingStrategy::LOCAL_FIRST);
 *   // On incoming inference request:
 *   auto response = router.Dispatch(request);
 */
class BackendRouter {
public:
    BackendRouter();

    /**
     * Register a backend. Takes ownership via unique_ptr.
     * Duplicate IDs replace existing backends.
     */
    void Register(std::unique_ptr<ComputeBackend> backend);

    /**
     * Remove a backend by ID. Returns true if found and removed.
     */
    bool Unregister(const std::string& id);

    /**
     * Dispatch an inference request to the best available backend.
     * Implements fallback chain if primary fails.
     *
     * @return ComputeResponse — check .success to determine outcome
     */
    ComputeResponse Dispatch(const ComputeRequest& request);

    /**
     * Set routing strategy.
     */
    void SetStrategy(RoutingStrategy strategy) { m_strategy = strategy; }
    RoutingStrategy GetStrategy() const { return m_strategy; }

    /**
     * Get count of registered backends.
     */
    size_t BackendCount() const { return m_backends.size(); }

    /**
     * Get reference to backend list (for P2P wiring / dynamic configuration).
     */
    std::vector<std::unique_ptr<ComputeBackend>>& GetBackends() { return m_backends; }

    /**
     * Get count of currently healthy/available backends.
     */
    size_t AvailableCount() const;

    /**
     * Get list of all backend IDs and their health status (for RPC/debug).
     */
    struct BackendStatus {
        std::string id;
        std::string name;
        HealthStatus health;
        bool available;
        int priority;
    };
    std::vector<BackendStatus> GetAllStatus() const;

private:
    /** Internal selection: pick best backend for given request */
    ComputeBackend* SelectBackend(const ComputeRequest& request) const;

    std::vector<std::unique_ptr<ComputeBackend>> m_backends;
    RoutingStrategy m_strategy = RoutingStrategy::LOCAL_FIRST;
    mutable uint64_t m_round_robin_counter = 0;  // For ROUND_ROBIN strategy (mutable: incremented in const SelectBackend)
};

#endif // TKN_NET_BACKEND_ROUTER_H
