#include <net/inference_engine.h>
#include <net/endpoint_registry.h>
#include <net/peer_session.h>
#include <net/p2p_reachability.h>
#include <net.h> // CNode, CConnman
#include <netmessagemaker.h> // NetMsg::Make
#include <util/log.h>
#include <map>
#include <mutex>
#include <future>
#include <atomic>

// ============================================================
// Static state: singleton BackendRouter with pre-registered backends
// ============================================================
bool InferenceEngine::s_initialized = false;
bool InferenceEngine::s_p2p_wired = false;
static BackendRouter s_router;

// P2P subsystem singletons (created in Phase 2, owned here)
static std::unique_ptr<EndpointRegistry> s_endpoint_registry;
static std::unique_ptr<PeerSessionManager> s_session_manager;
static std::unique_ptr<P2PReachability> s_reachability;

// Async P2P response infrastructure (promise/future for RemoteBackend)
std::mutex InferenceEngine::s_pending_mutex;
std::map<uint64_t, std::shared_ptr<std::promise<InferenceEngine::P2PPendingResponse>>> InferenceEngine::s_pending_responses;
std::atomic<uint64_t> InferenceEngine::s_next_request_id{1};

BackendRouter& InferenceEngine::Router()
{
 EnsureInitialized();
 return s_router;
}

void InferenceEngine::EnsureInitialized()
{
 if (s_initialized) return;
 s_initialized = true;

 // Register LocalBackend (current proven IPC path: localhost:9332)
 // This replaces the old hardcoded DEFAULT_MINER_HOST / DEFAULT_MINER_PORT
 s_router.Register(std::make_unique<LocalBackend>("127.0.0.1", 9332));

 // Register RemoteBackend stub (disabled until P2P discovery is verified)
 auto remote = std::make_unique<RemoteBackend>("remote-p2p");
 remote->SetEnabled(false);
 s_router.Register(std::move(remote));

 s_router.SetStrategy(RoutingStrategy::LOCAL_FIRST);

 LogInfo("[InferenceEngine] BackendRouter initialized: local(127.0.0.1:9332) + remote-p2p(stub), strategy=LOCAL_FIRST");
}

// ============================================================
// ============================================================

InferenceResult InferenceEngine::RequestLocalMiner(
 const std::string& api_key,
 const std::string& model,
 const std::vector<InferenceMessage>& messages)
{
 EnsureInitialized();

 ComputeRequest request;
 request.api_key = api_key;
 request.model = model;
 for (const auto& msg : messages) {
 request.messages.push_back({msg.role, msg.content});
 }

 // Dispatch through routing abstraction (no hardcoded IP!)
 ComputeResponse response = s_router.Dispatch(request);

 InferenceResult result;
 result.success = response.success;
 result.content = response.content;
 result.tokens_used = response.tokens_used;
 result.cost = response.cost;
 result.error_message = response.error_message;

 if (!response.backend_id.empty()) {
 LogInfo("[InferenceEngine] Request routed to backend=%s latency=%.1fms tokens=%lld",
 response.backend_id, response.latency_ms, (long long)response.tokens_used);
 }

 return result;
}

InferenceResult InferenceEngine::RequestLocalMiner(
 const std::string& api_key,
 const std::string& model,
 const std::string& user_message)
{
 std::vector<InferenceMessage> messages = {{"user", user_message}};
 return RequestLocalMiner(api_key, model, messages);
}

InferenceResult InferenceEngine::RequestLocalMiner(
 const std::string& api_key,
 const std::string& model,
 const std::string& user_message,
 int max_tokens)
{
 EnsureInitialized();

 ComputeRequest request;
 request.api_key = api_key;
 request.model = model;
 request.max_tokens = max_tokens;
 request.messages.push_back({"user", user_message});

 ComputeResponse response = s_router.Dispatch(request);

 InferenceResult result;
 result.success = response.success;
 result.content = response.content;
 result.tokens_used = response.tokens_used;
 result.cost = response.cost;
 result.error_message = response.error_message;

 if (!response.backend_id.empty()) {
 LogInfo("[InferenceEngine] Request routed to backend=%s latency=%.1fms tokens=%lld",
 response.backend_id, response.latency_ms, (long long)response.tokens_used);
 }

 return result;
}

// ============================================================
// Phase 2: P2P Remote Routing initialization (called from AppInit)
// ============================================================
void InferenceEngine::InitP2PRemoteRouting(CConnman* connman)
{
 if (s_p2p_wired || !connman) return;
 s_p2p_wired = true;

 EnsureInitialized(); // Ensure Router + backends exist

 // Create P2P subsystems
 s_endpoint_registry = std::make_unique<EndpointRegistry>();
 s_session_manager = std::make_unique<PeerSessionManager>();
 s_reachability = std::make_unique<P2PReachability>();

 // Find and wire RemoteBackend in Router
 RemoteBackend* remote = nullptr;
 for (auto& backend : s_router.GetBackends()) {
 remote = dynamic_cast<RemoteBackend*>(backend.get());
 if (remote) break;
 }

 if (!remote) {
 LogWarning("[InferenceEngine] P2P init: no RemoteBackend found in Router!");
 return;
 }

 // Wire subsystems into RemoteBackend
 remote->SetEndpointRegistry(s_endpoint_registry.get());
 remote->SetSessionManager(s_session_manager.get());
 remote->SetReachability(s_reachability.get());

 // SendCallback: sends APIREQ to target peer via CConnman P2P network.
 // Response delivered via HandleAPIResponse() when APIRESP arrives.
 remote->SetSendCallback([connman](
 int64_t target_peer_id,
 const std::vector<uint8_t>& request_data,
 int timeout_seconds,
 std::string& out_content,
 int64_t& out_tokens,
 int64_t& out_cost) -> bool
 {
 if (!connman) return false;

 // Allocate unique request ID for this P2P inference call
 uint64_t req_id = s_next_request_id.fetch_add(1);

 // Create promise/future for async response delivery
 auto promise = std::make_shared<std::promise<P2PPendingResponse>>();
 auto future = promise->get_future();

 {
 std::lock_guard<std::mutex> lock(s_pending_mutex);
 s_pending_responses[req_id] = promise;
 }

 // Send APIREQ message to target peer via P2P network
 bool sent = false;
 std::string target_addr; // DIAG: capture target address for logging
 connman->ForNode(target_peer_id, [&](CNode* node) {
 target_addr = node->m_addr_name; // Use pre-resolved address string
 LogInfo("[InferenceEngine] P2P SENDING APIREQ req_id=%llu target_peer=%lld addr=%s size=%zu",
 (unsigned long long)req_id, (long long)target_peer_id,
 target_addr.c_str(), request_data.size());
 connman->PushMessage(node, NetMsg::Make(std::string("apireq"), request_data));
 sent = true;
 return true;
 });

 if (!sent) {
 LogWarning("[InferenceEngine] P2P SEND FAILED req_id=%llu target_peer=%lld ?node not found or not fully connected",
 (unsigned long long)req_id, (long long)target_peer_id);
 // Clean up pending entry on send failure
 std::lock_guard<std::mutex> lock(s_pending_mutex);
 s_pending_responses.erase(req_id);
 return false;
 }

 LogInfo("[InferenceEngine] P2P APIREQ SENT req_id=%llu target_peer=%lld addr=%s ?waiting for APIRESP...",
 (unsigned long long)req_id, (long long)target_peer_id, target_addr.c_str());

 // Wait for async response (APIRESP) with configurable timeout
 auto status = future.wait_for(std::chrono::seconds(timeout_seconds > 0 ? timeout_seconds : 120));

 if (status == std::future_status::timeout) {
 LogWarning("[InferenceEngine] P2P remote inference TIMEOUT req_id=%llu target_peer=%lld",
 (unsigned long long)req_id, (long long)target_peer_id);
 std::lock_guard<std::mutex> lock(s_pending_mutex);
 s_pending_responses.erase(req_id);
 out_content = "Error: Remote inference timed out";
 out_tokens = 0;
 out_cost = 0;
 return true; // Sent OK but response timed out
 }

 P2PPendingResponse resp = future.get();
 out_content = resp.content;
 out_tokens = resp.tokens;
 out_cost = resp.cost;

 LogInfo("[InferenceEngine] P2P remote inference COMPLETE req_id=%llu tokens=%lld cost=%lld",
 (unsigned long long)req_id, (long long)out_tokens, (long long)out_cost);
 return true;
 });

 remote->SetResolveCallback([connman](const std::string& peer_id_str) -> int64_t {
 if (!connman) return -1;
 int64_t found = -1;
 connman->ForEachNode([&](CNode* pnode) {
 if (found >= 0) return;
 // Match by addr name (simple string match, no lock needed)
 if (pnode->m_addr_name.find(peer_id_str) != std::string::npos) {
 found = pnode->GetId();
 }
 });
 return found;
 });

 remote->SetRefreshCallback([connman]() {
 InferenceEngine::ScanAndRegisterPeers(connman);
 });

 remote->SetEnabled(true);

 // Register self endpoint so other nodes can discover us
 EndpointEntry self_entry;
 self_entry.node_id = "self"; // Will be updated with real wallet/node_id later
 self_entry.ip = "0.0.0.0"; // Placeholder; actual IP discovered at runtime
 self_entry.p2p_port = 9333;
 self_entry.api_port = 9332;
 self_entry.reachable_directly = true;
 self_entry.quality_score = 100;
 self_entry.self_reported = true; // Mark as self so SelectBest() skips it
 s_endpoint_registry->Register(self_entry);
 s_endpoint_registry->SetSelfEndpoint("0.0.0.0", 9333, 9332);

 LogInfo("[InferenceEngine] P2P Remote Routing ENABLED: "
 "endpoint_registry + session_manager + reachability + connman_callbacks WIRED");

 // Scan already-connected peers and register them as potential remote targets
 ScanAndRegisterPeers(connman);
}

void InferenceEngine::ScanAndRegisterPeers(CConnman* connman)
{
 if (!connman || !s_endpoint_registry) return;

 int registered = 0;
 connman->ForEachNode([&](CNode* pnode) {
 if (!pnode->fSuccessfullyConnected) return;

 std::string addr_name = pnode->m_addr_name;
 if (addr_name.empty()) addr_name = "unknown";

 // DIAG: Log ALL connected peers with their CConnman IDs for routing debug
 LogInfo("[InferenceEngine] SCAN: connected peer GetId()=%lld addr=%s IsInbound=%d fSuccessfullyConnected=%d",
 (long long)pnode->GetId(), addr_name.c_str(),
 (int)pnode->IsInboundConn(), (int)pnode->fSuccessfullyConnected);

 // Build node_id from address (unique per connected peer)
 std::string node_id = "peer-" + std::to_string(pnode->GetId());

 // Parse IP and port from m_addr_name (format: "x.x.x.x:port")
 std::string ip = addr_name;
 uint16_t p2p_port = 9333; // Default P2P port
 auto colon_pos = addr_name.rfind(':');
 if (colon_pos != std::string::npos) {
 ip = addr_name.substr(0, colon_pos);
 try { p2p_port = static_cast<uint16_t>(std::stoul(addr_name.substr(colon_pos + 1))); }
 catch (...) { /* keep default */ }
 }

 // Skip if this peer is already registered (avoid duplicate updates spam)
 if (s_endpoint_registry->Find(node_id)) return;

 EndpointEntry entry;
 entry.node_id = node_id;
 entry.conn_node_id = pnode->GetId(); // Store CConnman node ID for direct routing
 entry.ip = ip;
 entry.p2p_port = p2p_port;
 entry.api_port = 9332; // TKNC default miner API port
 entry.reachable_directly = true; // Connected via P2P = reachable
 entry.quality_score = 50;
 s_endpoint_registry->Register(entry);

 registered++;
 LogInfo("[InferenceEngine] Discovered peer: %s -> %s:%d (id=%lld)",
 node_id.c_str(), ip.c_str(), p2p_port, (long long)pnode->GetId());
 });

 if (registered > 0) {
 LogInfo("[InferenceEngine] Peer scan complete: %d peer(s) registered in EndpointRegistry", registered);
 } else {
 LogInfo("[InferenceEngine] Peer scan complete: no new peers to register (total=%zu)",
 s_endpoint_registry->TotalCount());
 }
}

// ============================================================
// ============================================================
void InferenceEngine::HandleAPIResponse(
 uint64_t request_id,
 const std::string& content,
 int64_t tokens,
 int64_t cost)
{
 if (request_id == 0) return; // 0 = not a RemoteBackend request

 std::shared_ptr<std::promise<P2PPendingResponse>> promise;
 {
 std::lock_guard<std::mutex> lock(s_pending_mutex);
 auto it = s_pending_responses.find(request_id);
 if (it == s_pending_responses.end()) {
 return;
 }
 promise = it->second;
 s_pending_responses.erase(it);
 }

 P2PPendingResponse resp;
 resp.content = content;
 resp.tokens = tokens;
 resp.cost = cost;
 promise->set_value(std::move(resp));

 LogInfo("[InferenceEngine] APIRESP delivered to RemoteBackend req_id=%llu tokens=%lld",
 (unsigned long long)request_id, (long long)tokens);
}
