#ifndef TKN_NET_COMPUTE_BACKEND_H
#define TKN_NET_COMPUTE_BACKEND_H

#include <string>
#include <vector>
#include <cstdint>

// ============================================================
// ============================================================
//
// Architecture:
// tkncd (node) owns BackendRouter
// BackendRouter holds multiple ComputeBackend implementations
// Each backend = one compute target (local IPC, P2P remote, etc.)
// of LocalBackend's transport layer.
// ============================================================

struct ComputeRequest {
 std::string api_key;
 std::string model;
 struct Message {
 std::string role; // "user", "assistant", "system"
 std::string content;
 };
 std::vector<Message> messages;
 int max_tokens; // -1=unlimited, 0=not specified, >0=limit

 // P2P routing context (injected by RemoteBackend for BP3 binding)
 // These ensure response can be routed back to correct requester node/session
 std::string session_id; // Bound PeerSession identifier
 std::string source_node_id; // Originating node identity

 ComputeRequest() : max_tokens(0) {}

 // Convenience: single-message constructor
 static ComputeRequest Single(const std::string& model, const std::string& user_msg) {
 ComputeRequest req;
 req.model = model;
 req.messages.push_back({"user", user_msg});
 return req;
 }
};

enum class HealthStatus : int {
 UNKNOWN = 0,
 HEALTHY = 1, // Fully operational
 DEGRADED = 2, // Working but slow / high load
 UNHEALTHY = 3, // Errors occurring
 OFFLINE = 4 // Not reachable
};

struct ComputeResponse {
 bool success = false;
 std::string content;
 int64_t tokens_used = 0;
 int64_t cost = 0;
 HealthStatus health = HealthStatus::UNKNOWN;
 std::string error_message;
 std::string backend_id; // Which backend handled this
 double latency_ms = 0.0; // Round-trip time
};

/**
 * Abstract interface for all compute backends.
 *
 * Implementations:
 * - LocalBackend: HTTP to localhost:9332 (current IPC path)
 * - RemoteBackend: P2P-routed inference to peer miners (future)
 * - FallbackBackend: Error response when nothing available (future)
 */
class ComputeBackend {
public:
 virtual ~ComputeBackend() = default;

 /** Unique identifier for this backend instance */
 virtual std::string GetId() const = 0;

 /** Human-readable name for logging */
 virtual std::string GetName() const = 0;

 /**
 * Execute an LLM inference request on this backend.
 * Thread-safe: may be called from multiple threads concurrently.
 */
 virtual ComputeResponse Infer(const ComputeRequest& request) = 0;

 /**
 * Check the current health of this backend.
 * Called periodically by BackendRouter to prune dead backends.
 */
 virtual HealthStatus CheckHealth() const = 0;

 /**
 * Priority level for scheduling decisions.
 * Higher value = preferred when multiple backends are healthy.
 */
 virtual int GetPriority() const { return 0; }

 /**
 * Whether this backend supports the given model.
 * Empty hint = any model accepted.
 */
 virtual bool SupportsModel(const std::string& /*model_hint*/) const { return true; }

 /** Whether this backend is currently accepting requests */
 virtual bool IsAvailable() const { return CheckHealth() <= HealthStatus::DEGRADED; }
};

#endif // TKN_NET_COMPUTE_BACKEND_H
