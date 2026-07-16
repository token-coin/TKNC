#ifndef TKN_NET_INFERENCE_ENGINE_H
#define TKN_NET_INFERENCE_ENGINE_H

#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <future>
#include <net/compute_backend.h>
#include <net/backend_router.h>
#include <net/local_backend.h>
#include <net/remote_backend.h>
#include <net/p2p_reachability.h>
#include <net/peer_session.h>
#include <net/endpoint_registry.h>

// (Chinese comment removed)
// (Chinese comment removed)
struct InferenceResult {
 bool success = false;
 std::string content; // LLM response text
 int64_t prompt_tokens = 0; // Prompt tokens (from miner's LLM engine)
 int64_t completion_tokens = 0; // Completion tokens (from miner's LLM engine)
 int64_t tokens_used = 0; // Total tokens = prompt_tokens + completion_tokens (computed by node)
 int64_t cost = 0; // Cost in smallest unit
 std::string error_message; // Set when success=false
 int http_status_code = 0; // HTTP status if available
};

// Message for chat completion API (legacy compat layer)
struct InferenceMessage {
 std::string role; // "user", "assistant", etc.
 std::string content;
};

/**
 * InferenceEngine ?Unified entry point for LLM inference requests.
 *
 * Architecture (Phase D: Compute Routing Abstraction):
 *
 * Caller (net_processing / p2p_llm / rpc)
 * ? * InferenceEngine::RequestLocalMiner() ?legacy API, unchanged signature
 * ? * BackendRouter::Dispatch(ComputeRequest) ?NEW: routing abstraction
 * ? * [LocalBackend | RemoteBackend(stub)] ?ComputeBackend implementations
 * ? * LocalBackend ?HTTP POST localhost:9332 ?proven IPC path (unchanged)
 *
 * What changed:
 * - Removed hardcoded 127.0.0.1:9332 from this file
 * - Backend selection is now pluggable (LOCAL_FIRST / ROUND_ROBIN / ...)
 * - Remote backend is registered as disabled stub (ready for P2P activation)
 * - Miner process and IPC protocol are completely unchanged
 */
class InferenceEngine {
public:
 /**
 * Get the singleton BackendRouter.
 * Auto-registers LocalBackend(127.0.0.1, 9332) and RemoteBackend stub on first call.
 */
 static BackendRouter& Router();

 /**
 * Send inference request through the backend routing system.
 *
 * Backward-compatible API ?same signature as before.
 * Internally converts to ComputeRequest ?BackendRouter::Dispatch() ?InferenceResult.
 *
 * @param api_key API key for authentication
 * @param model Model name hint
 * @param messages Chat messages (role + content pairs)
 * @return InferenceResult with content or error details
 */
 static InferenceResult RequestLocalMiner(
 const std::string& api_key,
 const std::string& model,
 const std::vector<InferenceMessage>& messages);

 /**
 * Quick single-message overload (backward compatible).
 */
 static InferenceResult RequestLocalMiner(
 const std::string& api_key,
 const std::string& model,
 const std::string& user_message);

 /**
 * Single-message overload with max_tokens support.
 * max_tokens: -1=unlimited, 0=engine default, >0=specific limit
 */
 static InferenceResult RequestLocalMiner(
 const std::string& api_key,
 const std::string& model,
 const std::string& user_message,
 int max_tokens);

 /**
 * Phase 2 initialization ?called from AppInit after CConnman is ready.
 * Creates P2P subsystems (EndpointRegistry, PeerSessionManager, P2PReachability),
 * wires them into RemoteBackend, enables remote routing, and binds CConnman callbacks.
 *
 * @param connman Active CConnman instance for message sending / peer resolution
 */
 static void InitP2PRemoteRouting(class CConnman* connman);

 /**
 * Scan connected P2P peers and register them in EndpointRegistry.
 * Called after InitP2PRemoteRouting() to populate the registry with
 * reachable peer endpoints so RemoteBackend can discover and route to them.
 *
 * @param connman Active CConnman instance for peer enumeration
 */
 static void ScanAndRegisterPeers(class CConnman* connman);

 /**
 * Handle incoming APIRESP message for RemoteBackend async path.
 * Called by net_processing.cpp when an APIRESP arrives that matches
 * a pending RemoteBackend request (promise/future pattern).
 *
 * @param request_id The request ID from the APIRESP (0 = ignore)
 * @param content LLM response content
 * @param tokens Token count
 * @param cost Cost in smallest unit
 */
 static void HandleAPIResponse(
 uint64_t request_id,
 const std::string& content,
 int64_t tokens,
 int64_t cost);

private:
 static bool s_initialized;
 static bool s_p2p_wired; // Whether Phase 2 (P2P wiring) completed
 static void EnsureInitialized();

 // Async P2P response infrastructure (for RemoteBackend promise/future path)
 struct P2PPendingResponse {
 std::string content;
 int64_t tokens = 0;
 int64_t cost = 0;
 };
 static std::mutex s_pending_mutex;
 static std::map<uint64_t, std::shared_ptr<std::promise<P2PPendingResponse>>> s_pending_responses;
 static std::atomic<uint64_t> s_next_request_id;
};

#endif // TKN_NET_INFERENCE_ENGINE_H
