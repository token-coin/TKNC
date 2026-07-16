#include <net/remote_backend.h>
#include <net/endpoint_registry.h>
#include <net/peer_session.h>
#include <net/p2p_reachability.h>
#include <net/api_protocol.h>
#include <util/log.h>
#include <util/time.h>

#include <chrono>

// ============================================================
// Constructor
// ============================================================
RemoteBackend::RemoteBackend(const std::string& id)
 : m_id(id), m_enabled(false)
{
}

// ============================================================
// Subsystem wiring
// ============================================================
void RemoteBackend::SetEndpointRegistry(EndpointRegistry* registry)
{
 m_endpoint_registry = registry;
}

void RemoteBackend::SetSessionManager(PeerSessionManager* mgr)
{
 m_session_manager = mgr;
}

void RemoteBackend::SetReachability(P2PReachability* reach)
{
 m_reachability = reach;
}

void RemoteBackend::SetSendCallback(SendAPIReqCallback cb)
{
 m_send_callback = std::move(cb);
}

void RemoteBackend::SetResolveCallback(ResolvePeerIdCallback cb)
{
 m_resolve_callback = std::move(cb);
}

void RemoteBackend::SetRefreshCallback(RefreshEndpointsCallback cb)
{
 m_refresh_callback = std::move(cb);
}

void RemoteBackend::SetEnabled(bool enabled)
{
 m_enabled = enabled;
 if (enabled) {
 LogInfo("[REMOTE-BACKEND] ENABLED ?P2P routing active");
 } else {
 LogInfo("[REMOTE-BACKEND] DISABLED ?P2P routing inactive");
 }
}

// ============================================================
// Health check
// ============================================================
HealthStatus RemoteBackend::CheckHealth() const
{
 if (!m_enabled) return HealthStatus::OFFLINE;

 // Check if we have subsystems wired
 if (!m_endpoint_registry || !m_session_manager) {
 return HealthStatus::DEGRADED;
 }

 // Check if we have any reachable endpoints
 if (m_endpoint_registry->OnlineCount() == 0) {
 return HealthStatus::DEGRADED; // No peers known yet
 }

 size_t reachable = m_endpoint_registry->GetReachableEndpoints().size();
 if (reachable > 0) {
 return HealthStatus::HEALTHY;
 }

 return HealthStatus::DEGRADED;
}

// ============================================================
// (Chinese comment removed)
// ============================================================
ComputeResponse RemoteBackend::Infer(const ComputeRequest& request)
{
 ComputeResponse result;
 result.backend_id = m_id;

 auto start_time = std::chrono::steady_clock::now();

 // Step 1: Check enabled state
 if (!m_enabled) {
 result.success = false;
 result.health = HealthStatus::OFFLINE;
 result.error_message = "Remote P2P backend is disabled. Call SetEnabled(true).";
 return result;
 }

 // Step 1.5: Refresh peer endpoints (lazy discovery for late-connecting peers)
 if (m_refresh_callback) {
 m_refresh_callback();
 }

 // Step 2: Verify all subsystems are ready
 std::string sub_err;
 if (!EnsureSubsystemsReady(sub_err)) {
 result.success = false;
 result.health = HealthStatus::UNHEALTHY;
 result.error_message = "Subsystem not ready: " + sub_err;
 return result;
 }

 // Step 3: Select best target endpoint from registry
 const EndpointEntry* target = m_endpoint_registry->SelectBest(request.model);

 if (!target) {
 result.success = false;
 result.health = HealthStatus::UNHEALTHY;
 result.error_message =
 "No remote endpoint available in registry. "
 "Ensure peers are connected and advertising their endpoints.";
 LogWarning("[REMOTE-BACKEND] No endpoint for model=%s", request.model.c_str());
 return result;
 }

 // (Chinese comment removed)
 PeerSession* session = nullptr;
 {
 auto preflight = PreflightCheck(*target, session);
 if (preflight != PreflightResult::OK) {
 result.success = false;
 result.health = HealthStatus::UNHEALTHY;
 result.error_message = "PreflightCheck failed: peer=" + target->node_id +
 " reason=" + std::to_string(static_cast<int>(preflight)) +
 " (NO_REACHABLE_MINER)";
 m_endpoint_registry->UpdateQualityScore(target->node_id, -15);
 LogWarning("[REMOTE-BACKEND] Preflight FAIL: %s code=%d",
 target->node_id.c_str(), static_cast<int>(preflight));
 return result;
 }
 }

 LogInfo("[REMOTE-BACKEND] Routing to peer=%s (%s:%d, model=%s) [session=%p]",
 target->node_id.c_str(), target->ip.c_str(),
 target->p2p_port, target->model_name.c_str(), (void*)session);

 // Step 5: Use stored CConnman NodeId directly (set by ScanAndRegisterPeers),
 // fallback to resolve callback for legacy entries
 int64_t connman_node_id = target->conn_node_id;
 if (connman_node_id < 0 && m_resolve_callback) {
 connman_node_id = m_resolve_callback(target->node_id);
 }

 if (connman_node_id < 0) {
 result.success = false;
 result.health = HealthStatus::UNHEALTHY;
 result.error_message =
 "Cannot resolve peer '" + target->node_id + "' to CConnman NodeId. "
 "Peer may not be connected or resolve callback not configured.";
 m_endpoint_registry->UpdateQualityScore(target->node_id, -10);
 return result;
 }

 // (Chinese comment removed)
 // (BP2: reachability confirmed, session alive, route validated)
 (void)session; // Used later for response stats

 // Step 7: Serialize request and send via callback
 if (!m_send_callback) {
 result.success = false;
 result.health = HealthStatus::UNHEALTHY;
 result.error_message = "Send callback not configured. Cannot send APIREQ.";
 return result;
 }

 // Build serialized APIRequest using proper binary format (must match APIRequest::Deserialize on receiver)
 // CRITICAL: Previous text-format code was incompatible with net_processing.cpp's Deserialize()
 APIRequest wire_req;
 wire_req.api_key = request.api_key;
 wire_req.model = request.model;
 wire_req.max_tokens = request.max_tokens; // Pass through client's max_tokens
 wire_req.messages.reserve(request.messages.size());
 for (const auto& msg : request.messages) {
 ChatMessage cm;
 cm.role = msg.role;
 cm.content = msg.content;
 wire_req.messages.push_back(cm);
 }
 // Auto-generate unique request_id for tracking
 {
 static std::atomic<uint64_t> s_wire_counter{0};
 wire_req.request_id = static_cast<uint64_t>(GetTime()) * 1000000 + s_wire_counter.fetch_add(1);
 wire_req.nonce = wire_req.request_id + 1;
 }
 std::vector<uint8_t> request_data = wire_req.Serialize();

 std::string response_content;
 int64_t response_tokens = 0;
 int64_t response_cost = 0;

 bool sent_ok = m_send_callback(
 connman_node_id,
 request_data,
 86400, // (Chinese comment removed)
 response_content,
 response_tokens,
 response_cost);

 auto elapsed = std::chrono::duration<double, std::milli>(
 std::chrono::steady_clock::now() - start_time).count();

 // (Chinese comment removed)
 if (sent_ok && response_content.find("Error:") != 0) {
 // (Chinese comment removed)
 result.success = true;
 result.content = response_content;
 result.tokens_used = response_tokens;
 result.cost = response_cost;
 result.health = HealthStatus::HEALTHY;
 result.latency_ms = elapsed;

 // Update session stats (use bound session from PreflightCheck)
 if (session) {
 session->MarkActive();
 session->requests_served++;
 session->avg_latency_ms = (session->avg_latency_ms * (session->requests_served - 1) + elapsed)
 / session->requests_served;
 }

 // Update quality score (positive reinforcement)
 m_endpoint_registry->UpdateQualityScore(target->node_id, 5);

 LogInfo("[REMOTE-BACKEND] SUCCESS: peer=%s tokens=%lld cost=%.1fms",
 target->node_id.c_str(), (long long)response_tokens, elapsed);
 } else {
 result.success = false;
 result.content = response_content; // May contain error message from peer
 result.tokens_used = 0;
 result.cost = 0;
 result.health = HealthStatus::DEGRADED;
 result.latency_ms = elapsed;
 result.error_message = "P2P inference request failed or timed out";

 // Update quality score (negative reinforcement)
 m_endpoint_registry->UpdateQualityScore(target->node_id, -15);

 LogWarning("[REMOTE-BACKEND] FAILED: peer=%s latency=%.1fms",
 target->node_id.c_str(), elapsed);
 }

 return result;
}

// ============================================================
// Internal helpers
// ============================================================
bool RemoteBackend::EnsureSubsystemsReady(std::string& error_out) const
{
 if (!m_endpoint_registry) {
 error_out = "EndpointRegistry is null";
 return false;
 }
 if (!m_session_manager) {
 error_out = "PeerSessionManager is null";
 return false;
 }
 if (!m_send_callback) {
 error_out = "Send callback not set";
 return false;
 }
 // Reachability is optional (can work without it using existing connections)
 return true;
}

// ============================================================
// (Chinese comment removed)
// ============================================================
RemoteBackend::PreflightResult RemoteBackend::PreflightCheck(
 const EndpointEntry& target, PeerSession*& out_session)
{
 out_session = nullptr;

 // (Chinese comment removed)
 if (!m_session_manager) return PreflightResult::NO_SESSION;

 out_session = m_session_manager->GetOrCreateSession(
 target.node_id,
 target.ip + ":" + std::to_string(target.p2p_port),
 target.reachable_via_punch,
 target.reachable_via_tcp);

 if (!out_session) return PreflightResult::NO_SESSION;

 // 2. Validate session is usable
 if (!out_session->IsUsable()) {
 LogWarning("[REMOTE-BACKEND] Preflight: session %s is DEAD state=%s",
 target.node_id.c_str(), SessionStateToString(out_session->state.load()));
 return PreflightResult::SESSION_DEAD;
 }

 // 3. Confirm reachability path exists
 bool has_route = target.reachable_directly ||
 target.reachable_via_punch ||
 target.reachable_via_tcp;

 if (!has_route && m_reachability) {
 // Attempt on-the-fly hole punch as last resort
 uint64_t nonce = static_cast<uint64_t>(
 std::chrono::duration_cast<std::chrono::microseconds>(
 std::chrono::steady_clock::now().time_since_epoch()).count());

 PunchResult pr = m_reachability->CoordinateUDPPunch(
 target.ip, target.p2p_port, 0, nonce);
 if (pr == PunchResult::SUCCESS) {
 has_route = true;
 m_endpoint_registry->UpdateReachability(target.node_id, false, true, false);
 out_session->udp_punch_used = true;
 LogInfo("[REMOTE-BACKEND] Preflight: emergency punch OK to %s", target.node_id.c_str());
 } else {
 pr = m_reachability->TryTCPFallback(target.ip, target.p2p_port);
 if (pr == PunchResult::SUCCESS) {
 has_route = true;
 m_endpoint_registry->UpdateReachability(target.node_id, false, false, true);
 out_session->tcp_fallback_used = true;
 LogInfo("[REMOTE-BACKEND] Preflight: emergency TCP OK to %s", target.node_id.c_str());
 }
 }
 }

 if (!has_route) {
 LogWarning("[REMOTE-BACKEND] Preflight: NO_ROUTE to %s (no direct/punch/tcp)",
 target.node_id.c_str());
 return PreflightResult::NO_ROUTE;
 }

 out_session->MarkActive();
 LogInfo("[REMOTE-BACKEND] Preflight OK: %s session=%p route=%s",
 target.node_id.c_str(), (void*)out_session,
 target.reachable_directly ? "direct" : target.reachable_via_punch ? "punch" : "tcp");
 return PreflightResult::OK;
}

std::string RemoteBackend::GetDiagnostics() const
{
 char buf[512];
 snprintf(buf, sizeof(buf),
 "RemoteBackend{id=%s enabled=%d ep_reg=%p sess_mgr=%p reach=%p "
 "send_cb=%s resolve_cb=%s health=%s}",
 m_id.c_str(), (int)m_enabled,
 (void*)m_endpoint_registry, (void*)m_session_manager, (void*)m_reachability,
 m_send_callback ? "set" : "null",
 m_resolve_callback ? "set" : "null",
 NatTypeToString(m_reachability ? m_reachability->GetDetectedNATType() : NATType::UNKNOWN));

 return std::string(buf);
}
