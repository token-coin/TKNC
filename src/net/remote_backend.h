#ifndef TKN_NET_REMOTE_BACKEND_H
#define TKN_NET_REMOTE_BACKEND_H

#include <net/compute_backend.h>
#include <string>
#include <memory>
#include <functional>

// Forward declarations (avoid heavy includes)
class EndpointRegistry;
class PeerSessionManager;
class P2PReachability;
struct PeerSession;

/**
 * Callback type for sending an APIREQ to a remote peer and waiting for APIRESP.
 *
 * The implementation of this callback lives in net_processing.cpp where
 * CConnman::PushMessage() and m_pending_api_responses are accessible.
 *
 * @param target_peer_id   Node ID of the target peer in CConnman
 * @param request_data      Serialized APIRequest bytes
 * @param timeout_seconds   How long to wait for response
 * @param out_content       [out] LLM response content on success
 * @param out_tokens        [out] Token count on success
 * @param out_cost          [out] Cost in smallest unit
 * @return true if response received successfully
 */
using SendAPIReqCallback = std::function<bool(
    int64_t target_peer_id,
    const std::vector<uint8_t>& request_data,
    int timeout_seconds,
    std::string& out_content,
    int64_t& out_tokens,
    int64_t& out_cost
)>;

/**
 * Callback type for resolving a node_id (wallet addr) to CConnman NodeId.
 * This bridges the gap between wallet addresses and internal peer IDs.
 */
using ResolvePeerIdCallback = std::function<int64_t(const std::string& node_id)>;

/**
 * RemoteBackend — Real P2P inference routing (activated from stub).
 *
 * Architecture:
 *
 *   Infer(request)
 *       │
 *       ▼
 *   m_endpoint_registry->SelectBest(model)     ← Find target node
 *       │
 *       ▼
 *   m_reachability->CoordinateUDPPunch(...)     ← Establish direct channel
 *       │ (or TCP fallback)
 *       ▼
 *   m_session_manager->CreateSession(...)       ← Track connection state
 *       │
 *       ▼
 *   m_send_callback(peer_id, serialized_req)   ← Send APIREQ via CConnman
 *       │
 *       ▼
 *   Wait for APIRESP (promise/future)           ← Receive result
 *       │
 *       ▼
 *   Return ComputeResponse with LLM output
 *
 * Activation prerequisites (checked at runtime):
 *   ✅ EndpointRegistry has entries (peers discovered)
 *   ✅ P2PReachability initialized (NAT detected)
 *   ✅ Send callback wired (net_processing integration)
 *   ✅ Resolve callback wired (node_id mapping)
 */
class RemoteBackend final : public ComputeBackend {
public:
    explicit RemoteBackend(const std::string& id = "remote-p2p");

    // --- ComputeBackend interface ---
    std::string GetId() const override { return m_id; }
    std::string GetName() const override { return "P2P Remote Miner"; }
    ComputeResponse Infer(const ComputeRequest& request) override;
    HealthStatus CheckHealth() const override;
    int GetPriority() const override { return 10; }  // Lower than LocalBackend(100)

    // --- Subsystem wiring (must be called during node init) ---

    /** Set the endpoint registry (required for peer lookup) */
    void SetEndpointRegistry(EndpointRegistry* registry);

    /** Set the session manager (required for connection tracking) */
    void SetSessionManager(PeerSessionManager* mgr);

    /** Set the reachability engine (required for NAT traversal) */
    void SetReachability(P2PReachability* reach);

    /**
     * Set the callback for actually sending APIREQ over P2P.
     * This must be set before Infer() can work.
     * The callback should serialize the request, push it via CConnman,
     * and wait for APIRESP with timeout.
     */
    void SetSendCallback(SendAPIReqCallback cb);

    /**
     * Set the callback for resolving node_id (wallet address) to CConnman NodeId.
     */
    void SetResolveCallback(ResolvePeerIdCallback cb);

    /**
     * Set callback to refresh peer endpoints before routing.
     * Called at the start of every Infer() to ensure EndpointRegistry
     * has the latest connected peers (handles late-connecting peers).
     */
    using RefreshEndpointsCallback = std::function<void()>;
    void SetRefreshCallback(RefreshEndpointsCallback cb);

    // --- Enable / Disable ---
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return m_enabled; }

    // --- Reachability preflight (called before every Infer) ---
    enum class PreflightResult {
        OK = 0,              // Session alive, route confirmed
        NO_SESSION,          // Cannot create/bind session
        SESSION_DEAD,        // Session exists but unusable
        NO_ROUTE             // No reachability path (punch+tcp both failed)
    };

    /**
     * Verify target is reachable before sending inference request.
     * Binds endpoint → session, validates session state,
     * confirms transport channel (UDP punch or TCP fallback).
     * @param target  Endpoint entry from registry
     * @param out_session  [out] Bound session pointer for this request
     * @return OK if ready to send, error code otherwise
     */
    PreflightResult PreflightCheck(
        const struct EndpointEntry& target,
        PeerSession*& out_session);

    // --- Diagnostics ---
    std::string GetDiagnostics() const;

private:
    std::string m_id;
    bool m_enabled = false;

    // Subsystem pointers (not owned)
    EndpointRegistry* m_endpoint_registry = nullptr;
    PeerSessionManager* m_session_manager = nullptr;
    P2PReachability* m_reachability = nullptr;

    // Callbacks (wired from net_processing layer)
    SendAPIReqCallback m_send_callback;
    ResolvePeerIdCallback m_resolve_callback;
    RefreshEndpointsCallback m_refresh_callback;

    // Internal helpers
    bool EnsureSubsystemsReady(std::string& error_out) const;
};

#endif // TKN_NET_REMOTE_BACKEND_H
