// Copyright (c) 2026-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <inference_gateway.h>

#include <logging.h>
#include <common/args.h>
#include <util/strencodings.h>
#include <util/threadnames.h>
#include <util/fs_helpers.h>  // GetExeDir()
#include <seed_register.h>   // GetMinerWalletAddress()

// Node computes tokens_used from raw miner response, handles billing via CheckAndDeductEscrow.
#include <rpc/escrow_rpc.h>
#include <rpc/tknc_apikey.h>
#include <apikey/api_key.h>
#include <apikey/api_key_db.h>
#include <billing/billing_receipt.h>
#include <util/moneystr.h>

#include <event2/buffer.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <event2/util.h>
#include <event2/event.h>
#include <event2/bufferevent.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#ifndef SOCKET
#define SOCKET int
#endif
#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#ifndef SOCKET_ERROR
#define SOCKET_ERROR (-1)
#endif
#define closesocket close
#endif

using namespace std::chrono_literals;

/** Get CORS origin from environment variable (default: disabled for security) */
static const char* GetGatewayCorsOrigin() {
    const char* origin = std::getenv("TKNC_GATEWAY_CORS_ORIGIN");
    return (origin && origin[0] != '\0') ? origin : nullptr;
}

/** Add CORS header only if origin is configured */
static void AddCorsHeader(struct evkeyvalq* headers) {
    const char* origin = GetGatewayCorsOrigin();
    if (origin) {
        evhttp_add_header(headers, "Access-Control-Allow-Origin", origin);
    }
}

/** Default port for Inference Gateway — non-HTTP port to avoid ISP filtering of 80/8080/443 */
static const int DEFAULT_INFERENCE_PORT = 9313;

/** Miner local address (iron rule: 127.0.0.1:9332 only) */
static const char* MINER_LOCAL_HOST = "127.0.0.1";
static const int MINER_LOCAL_PORT = 9332;

/** Request timeout for miner forwarding (seconds).
 *  86400s = 24h. The system is a bridge — the only real bottleneck is the miner's
 *  hardware and bandwidth. No artificial timeout should cut off long-running
 *  inference (e.g. 10000B models on data-center-grade miners). */
static const int MINER_REQUEST_TIMEOUT_SEC = 86400;

static struct event_base* g_gateway_base = nullptr;
static struct evhttp* g_gateway_http = nullptr;
static std::thread g_gateway_thread;
static std::atomic<bool> g_gateway_running{false};

// Shared secret for X-TKNC-Skip-Billing header (prevents localhost bypass).
// Generated once at startup; the proxy (same process) uses the same secret.
// External clients cannot guess this secret, so they cannot bypass billing.
static std::string g_skip_billing_secret;
static std::once_flag g_skip_billing_secret_init;
static void InitSkipBillingSecret() {
    std::call_once(g_skip_billing_secret_init, []() {
        // Generate a random 32-byte hex secret
        std::random_device rd;
        std::stringstream ss;
        for (int i = 0; i < 32; ++i) {
            ss << std::hex << (rd() & 0xFF);
        }
        g_skip_billing_secret = "tknc_internal_" + ss.str();
        LogInfo("[InferenceGateway] Skip-billing secret generated (length=%zu)", g_skip_billing_secret.size());
    });
}

// Deterministic skip-billing secret based on API key.
// Both the proxy and the remote miner can compute this independently.
// Format: "tknc_proxy_" + simple hash of api_key.
// This allows cross-node proxy billing (proxy does billing, miner skips it).
static std::string ComputeSkipBillingSecret(const std::string& api_key) {
    if (api_key.empty()) return "";
    // Simple deterministic hash: sum of char values + position weighting
    // This is NOT cryptographic — it's just to prevent accidental matching.
    // Security comes from the fact that only the proxy code adds this header.
    uint64_t h = 5381;
    for (char c : api_key) {
        h = ((h << 5) + h) + (unsigned char)c;
    }
    std::ostringstream ss;
    ss << "tknc_proxy_" << std::hex << h;
    return ss.str();
}

// P2P routing: store node context for remote inference fallback
#include <node/context.h>
#include <net_processing.h>  // PeerManager, SendInferenceRequest
#include <net/p2p_llm.h>  // P2PLLM message types
#include <net/api_protocol.h>  // APIResponse, APIRequest
#include <fstream>
// Store NodeContext pointer directly (not pointer-to-any) to avoid dangling pointer
// The caller passes &node (NodeContext*) wrapped in std::any — extract it immediately
static node::NodeContext* g_node_ctx = nullptr;

// Forward-declare proxy target variables (defined later in the proxy section)
// These are needed by HandleChatCompletions to forward to remote gateway when local miner is offline.
static std::string g_proxy_target_ip;
static int g_proxy_target_port = 9313;
static std::atomic<bool> g_proxy_target_set{false};
static std::mutex g_proxy_target_mutex;

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / 4);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

/** Minimal JSON value parser for extracting specific fields */
struct SimpleJsonParser {
    std::string raw;

    explicit SimpleJsonParser(const std::string& data) : raw(data) {}

    /** Extract a string field value, returns empty if not found */
    std::string getString(const std::string& key) const {
        std::string search = "\"" + key + "\"";
        size_t pos = raw.find(search);
        if (pos == std::string::npos) return "";

        pos = raw.find(':', pos);
        if (pos == std::string::npos) return "";
        pos++;

        // Skip whitespace
        while (pos < raw.size() && (raw[pos] == ' ' || raw[pos] == '\t' || raw[pos] == '\n' || raw[pos] == '\r'))
            pos++;

        if (pos >= raw.size()) return "";
        if (raw[pos] != '"') return "";

        pos++; // skip opening quote
        std::string val;
        while (pos < raw.size() && raw[pos] != '"') {
            if (raw[pos] == '\\' && pos + 1 < raw.size()) {
                pos++;
                switch (raw[pos]) {
                    case 'n': val += '\n'; break;
                    case 'r': val += '\r'; break;
                    case 't': val += '\t'; break;
                    default: val += raw[pos]; break;
                }
            } else {
                val += raw[pos];
            }
            pos++;
        }
        return val;
    }

    /** Extract a bool field value */
    bool getBool(const std::string& key, bool def = false) const {
        std::string search = "\"" + key + "\"";
        size_t pos = raw.find(search);
        if (pos == std::string::npos) return def;

        pos = raw.find(':', pos);
        if (pos == std::string::npos) return def;
        pos++;

        while (pos < raw.size() && (raw[pos] == ' ' || raw[pos] == '\t'))
            pos++;

        if (pos >= raw.size()) return def;
        if (raw.substr(pos, 4) == "true") return true;
        if (raw.substr(pos, 5) == "false") return false;
        return def;
    }

    /** Check if key exists */
    bool hasKey(const std::string& key) const {
        return raw.find("\"" + key + "\"") != std::string::npos;
    }

    /** Extract an integer field value, returns def if not found */
    int64_t getInt(const std::string& key, int64_t def = 0) const {
        std::string search = "\"" + key + "\"";
        size_t pos = raw.find(search);
        if (pos == std::string::npos) return def;

        pos = raw.find(':', pos);
        if (pos == std::string::npos) return def;
        pos++;

        while (pos < raw.size() && (raw[pos] == ' ' || raw[pos] == '\t' || raw[pos] == '\n' || raw[pos] == '\r'))
            pos++;

        if (pos >= raw.size()) return def;

        // Parse optional sign
        bool negative = false;
        if (raw[pos] == '-') { negative = true; pos++; }

        int64_t val = 0;
        bool has_digit = false;
        while (pos < raw.size() && raw[pos] >= '0' && raw[pos] <= '9') {
            val = val * 10 + (raw[pos] - '0');
            pos++;
            has_digit = true;
        }

        if (!has_digit) return def;
        return negative ? -val : val;
    }

    /** Extract messages array as ChatML-formatted prompt.
     *  Only extracts from the "messages" array — ignores "tools", "functions", etc.
     *  This is critical because IDEs (Trae, ZCode) send large "tools" arrays with
     *  "content" fields inside function descriptions that must NOT be treated as
     *  user messages.
     */
    std::string extractPromptFromMessages() const {
        LogInfo("[extractPromptFromMessages] raw JSON size=%zu, first 200 chars: %.200s", raw.size(), raw.c_str());

        // Find the "messages" array — search for "messages":[
        std::string messages_key = "\"messages\"";
        size_t msg_pos = raw.find(messages_key);
        if (msg_pos == std::string::npos) {
            // Fallback: try "prompt" or "content" as flat key
            std::string p = getString("prompt");
            if (p.empty()) p = getString("content");
            return p;
        }

        // Find the opening bracket of the messages array
        size_t bracket = raw.find('[', msg_pos);
        if (bracket == std::string::npos) {
            std::string p = getString("prompt");
            if (p.empty()) p = getString("content");
            return p;
        }

        // Parse each message object { "role": "...", "content": "..." } within the array
        std::string prompt;
        size_t pos = bracket + 1;

        while (pos < raw.size()) {
            // Find next object opening brace
            size_t obj_start = raw.find('{', pos);
            if (obj_start == std::string::npos) break;

            // Find matching closing brace for this object
            int depth = 1;
            size_t obj_end = obj_start + 1;
            while (obj_end < raw.size() && depth > 0) {
                if (raw[obj_end] == '{') depth++;
                else if (raw[obj_end] == '}') depth--;
                // Skip strings to avoid braces inside string values
                if (raw[obj_end] == '"') {
                    obj_end++;
                    while (obj_end < raw.size()) {
                        if (raw[obj_end] == '\\' && obj_end + 1 < raw.size()) {
                            obj_end += 2;
                            continue;
                        }
                        if (raw[obj_end] == '"') break;
                        obj_end++;
                    }
                }
                obj_end++;
            }
            if (depth != 0) break;

            // Extract this message object as a substring
            std::string msg_obj = raw.substr(obj_start, obj_end - obj_start);

            // Extract role from this message object
            std::string role;
            {
                std::string role_key = "\"role\"";
                size_t rp = msg_obj.find(role_key);
                if (rp != std::string::npos) {
                    size_t colon = msg_obj.find(':', rp + role_key.size());
                    if (colon != std::string::npos) {
                        size_t rs = colon + 1;
                        while (rs < msg_obj.size() && (msg_obj[rs] == ' ' || msg_obj[rs] == '\t')) rs++;
                        if (rs < msg_obj.size() && msg_obj[rs] == '"') {
                            rs++;
                            while (rs < msg_obj.size() && msg_obj[rs] != '"') {
                                if (msg_obj[rs] == '\\' && rs + 1 < msg_obj.size()) {
                                    rs += 2;
                                    continue;
                                }
                                role += msg_obj[rs];
                                rs++;
                            }
                        }
                    }
                }
            }

            // Extract content from this message object
            std::string content;
            {
                std::string content_key = "\"content\"";
                size_t cp = msg_obj.find(content_key);
                if (cp != std::string::npos) {
                    size_t colon = msg_obj.find(':', cp + content_key.size());
                    if (colon != std::string::npos) {
                        size_t cs = colon + 1;
                        while (cs < msg_obj.size() && (msg_obj[cs] == ' ' || msg_obj[cs] == '\t' || msg_obj[cs] == '\n' || msg_obj[cs] == '\r')) cs++;
                        if (cs < msg_obj.size() && msg_obj[cs] == '"') {
                            cs++;
                            while (cs < msg_obj.size()) {
                                if (msg_obj[cs] == '\\' && cs + 1 < msg_obj.size()) {
                                    char next = msg_obj[cs + 1];
                                    if (next == 'n') content += '\n';
                                    else if (next == 'r') content += '\r';
                                    else if (next == 't') content += '\t';
                                    else if (next == '"') content += '"';
                                    else if (next == '\\') content += '\\';
                                    else { content += msg_obj[cs]; content += next; }
                                    cs += 2;
                                    continue;
                                }
                                if (msg_obj[cs] == '"') break;
                                content += msg_obj[cs];
                                cs++;
                            }
                        }
                    }
                }
            }

            // Build ChatML format for this message
            if (!content.empty()) {
                if (role == "system") {
                    prompt += "<|im_start|>system\n" + content + "<|im_end|>\n";
                } else if (role == "assistant") {
                    prompt += "<|im_start|>assistant\n" + content + "<|im_end|>\n";
                } else {
                    // user or unknown → treat as user
                    prompt += "<|im_start|>user\n" + content + "<|im_end|>\n";
                }
            }

            pos = obj_end;
        }

        // Add final assistant prompt to trigger generation
        if (!prompt.empty()) {
            prompt += "<|im_start|>assistant\n";
        }

        LogInfo("[extractPromptFromMessages] extracted prompt length=%zu, first 200 chars: %.200s",
                prompt.size(), prompt.c_str());

        if (prompt.empty()) {
            std::string p = getString("prompt");
            if (p.empty()) p = getString("content");
            return p;
        }
        return prompt;
    }
};

static std::string HttpPostToMiner(const std::string& host, int port,
                                    const std::string& path,
                                    const std::string& body,
                                    int timeout_sec = MINER_REQUEST_TIMEOUT_SEC) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        LogError("[InferenceGateway] socket() failed");
        return "";
    }

#ifdef WIN32
    DWORD tv = timeout_sec * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));

#ifdef WIN32
    addr.sin_addr.S_un.S_addr = inet_addr(host.c_str());
#else
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
#endif

    if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        LogError("[InferenceGateway] connect to %s:%d failed (miner not running?)", host.c_str(), port);
#ifdef WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return "";
    }

    LogInfo("[E10-DIAG] Connected to miner %s:%d, sending request (body size=%zu)", host.c_str(), port, body.size());

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n";
    req << "Host: " << host << ":" << port << "\r\n";
    req << "Content-Type: application/json\r\n";
    req << "Content-Length: " << body.size() << "\r\n";
    req << "Connection: close\r\n";
    req << "\r\n";
    req << body;

    std::string request_str = req.str();

    ssize_t sent = send(sock, request_str.c_str(), static_cast<int>(request_str.size()), 0);
    if (sent == SOCKET_ERROR) {
        LogError("[InferenceGateway] send() failed");
#ifdef WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return "";
    }

    std::string response;
    char buf[4096];
    int recv_count = 0;
    while (true) {
        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        recv_count++;
        if (n <= 0) {
            LogInfo("[E10-DIAG] recv loop ended: n=%zd recv_count=%d total_size=%zu (n<=0 means connection closed or error)",
                    n, recv_count, response.size());
            break;
        }
        buf[n] = '\0';
        response.append(buf);
    }

    LogInfo("[E10-DIAG] HttpPostToMiner total response size=%zu, first 500 chars: %.500s",
            response.size(), response.c_str());

#ifdef WIN32
    closesocket(sock);
#else
    close(sock);
#endif

    size_t header_end = response.find("\r\n\r\n");
    if (header_end != std::string::npos) {
        return response.substr(header_end + 4);
    }
    return response;
}

// Async streaming: keepalive fires every 2s while miner processes, then response streams back as SSE.
struct StreamContext {
    struct evhttp_request* client_req;
    struct bufferevent* miner_bev;
    struct event* keepalive_timer;
    struct evhttp_connection* client_conn;
    std::string chat_id;
    std::string model;
    int64_t created;
    std::string miner_http_request;
    std::string miner_response_accumulated;
    bool finished;
    bool client_disconnected;

    // Non-stream mode: collect full response, send as JSON (not SSE)
    bool non_stream_mode = false;
    std::string api_key;
    std::string prompt;
    int max_tokens = 0;

    // Chunked transfer decoder state
    enum ChunkState { CHUNK_HEADERS, CHUNK_LENGTH, CHUNK_DATA, CHUNK_TRAILER, CHUNK_DONE };
    ChunkState chunk_state;
    std::string chunk_line;
    size_t chunk_bytes_remaining;
    bool headers_parsed;
    bool is_chunked;
    bool sse_forwarded;

    // Independent token counting (anti-cheat): node counts SSE chunks with non-empty content.
    // Each chunk = 1 LLM token. Not trusted from miner's self-reported count.
    int node_output_token_count = 0;
    // Accumulated output text for content-based verification
    std::string output_content_accumulated;

    // Skip billing when request comes from another gateway/proxy (X-TKNC-Skip-Billing header)
    bool skip_billing = false;

    // Reserved cost for this in-flight request (for concurrent flood prevention)
    CAmount reserved_cost = 0;
};

static void StreamKeepaliveCb(evutil_socket_t fd, short what, void* arg) {
    StreamContext* ctx = static_cast<StreamContext*>(arg);
    if (!ctx || ctx->finished || ctx->client_disconnected) return;

    if (ctx->sse_forwarded) {
        LogInfo("[StreamGateway] Keepalive stopped (true streaming in progress)");
        return;
    }

    struct evbuffer* buf = evbuffer_new();
    evbuffer_add_printf(buf, ": keepalive\n\n");
    evhttp_send_reply_chunk(ctx->client_req, buf);
    evbuffer_free(buf);

    LogInfo("[StreamGateway] Keepalive sent (sse_forwarded=%d)", ctx->sse_forwarded);

    struct timeval tv = {2, 0};
    event_add(ctx->keepalive_timer, &tv);
}

// Forward declaration
static void StreamClientCloseCb(struct evhttp_connection* conn, void* arg);

// CRITICAL: Safe context deletion — MUST be used instead of `delete ctx`.
// Removes the client connection close callback BEFORE deleting ctx to prevent
// use-after-free: if the close callback fires after ctx is deleted, it would
// access freed memory (the root cause of the 0xFFFFFFFFFF crash).
static void StreamSafeDelete(StreamContext* ctx) {
    if (!ctx) return;

    // Release any reserved cost to prevent pending cost leaks on error paths.
    if (ctx->reserved_cost > 0 && !ctx->api_key.empty()) {
        ReleaseEscrowCost(ctx->api_key, ctx->reserved_cost);
        ctx->reserved_cost = 0;
    }

    // Remove the close callback from client_conn so StreamClientCloseCb
    // is never called with a dangling pointer after we delete ctx.
    if (ctx->client_conn) {
        evhttp_connection_set_closecb(ctx->client_conn, nullptr, nullptr);
        ctx->client_conn = nullptr;
    }

    // Free the miner bufferevent if it hasn't been freed yet.
    if (ctx->miner_bev) {
        bufferevent_free(ctx->miner_bev);
        ctx->miner_bev = nullptr;
    }

    delete ctx;
}

static void StreamFinishAndCleanup(StreamContext* ctx) {
    if (!ctx || ctx->finished) return;
    ctx->finished = true;

    if (ctx->keepalive_timer) {
        event_del(ctx->keepalive_timer);
        event_free(ctx->keepalive_timer);
        ctx->keepalive_timer = nullptr;
    }

    if (ctx->client_disconnected) {
        LogInfo("[StreamGateway] Client already disconnected, skipping response");
        StreamSafeDelete(ctx);
        return;
    }

    // Extract miner response body (strip HTTP headers)
    std::string miner_body = ctx->miner_response_accumulated;
    size_t hdr_end = miner_body.find("\r\n\r\n");
    if (hdr_end != std::string::npos) {
        miner_body = miner_body.substr(hdr_end + 4);
    }

    // Parse content and token counts from miner response
    SimpleJsonParser miner_json(miner_body);
    std::string content = miner_json.getString("response");
    if (content.empty()) content = miner_json.getString("content");

    if (content.empty()) {
        // In streaming mode, the response body is SSE chunks (not a single JSON object).
        // Use the accumulated output content from StreamForwardToClient as fallback.
        if (!ctx->output_content_accumulated.empty()) {
            content = ctx->output_content_accumulated;
        } else if (miner_body.find("\"error\"") != std::string::npos) {
            std::string err_msg = miner_json.getString("error");
            if (err_msg.empty()) err_msg = "Miner error";
            content = "[Error: " + err_msg + "]";
        } else {
            content = "[Miner returned empty response]";
        }
    }

    // Parse token counts from miner response (may be 0 in streaming mode — miner doesn't send them)
    int prompt_tokens = 0, completion_tokens = 0;
    {
        size_t pt_pos = miner_body.find("\"prompt_tokens\"");
        if (pt_pos != std::string::npos) {
            size_t colon = miner_body.find(':', pt_pos);
            if (colon != std::string::npos)
                prompt_tokens = std::atoi(miner_body.c_str() + colon + 1);
        }
        size_t ct_pos = miner_body.find("\"completion_tokens\"");
        if (ct_pos != std::string::npos) {
            size_t colon = miner_body.find(':', ct_pos);
            if (colon != std::string::npos)
                completion_tokens = std::atoi(miner_body.c_str() + colon + 1);
        }
    }

    // Independent token verification: node count is authoritative (cannot be inflated by miner).
    // If both available: >30% discrepancy → use node count; within tolerance → use miner count (real tokenizer).
    // If only node count (streaming): use node count. If neither: content-based estimate.
    int node_count = ctx->node_output_token_count;
    int verified_completion_tokens = completion_tokens;

    if (node_count > 0) {
        if (completion_tokens > 0) {
            // Both available — verify
            if (completion_tokens > static_cast<int>(node_count * 1.3)) {
                LogWarning("[StreamGateway] TOKEN ANOMALY: miner reported completion_tokens=%d, "
                           "but node independently counted %d SSE chunks (>30%% discrepancy). "
                           "Using node count to protect client from overbilling.",
                           completion_tokens, node_count);
                verified_completion_tokens = node_count;
            } else if (completion_tokens < static_cast<int>(node_count * 0.7)) {
                LogWarning("[StreamGateway] TOKEN ANOMALY: miner reported completion_tokens=%d, "
                           "but node independently counted %d SSE chunks (<30%% discrepancy). "
                           "Using node count for accuracy.",
                           completion_tokens, node_count);
                verified_completion_tokens = node_count;
            }
            // else: within tolerance, use miner's count (more accurate — has real tokenizer)
        } else {
            // Streaming mode: miner didn't report completion_tokens, use node's count
            verified_completion_tokens = node_count;
        }
    }

    int tokens_used = prompt_tokens + verified_completion_tokens;

    // Fallback: if still 0, estimate from content or prompt
    if (tokens_used == 0) {
        // Try content-based estimate first
        if (!content.empty() && content[0] != '[') {
            tokens_used = static_cast<int>(content.length() / 4);
        }
        if (tokens_used == 0) {
            tokens_used = static_cast<int>(ctx->prompt.length() / 4);
        }
    }
    if (tokens_used < 1) tokens_used = 1;

    LogInfo("[StreamGateway] Token verification: miner_completion=%d, node_sse_count=%d, "
            "verified_completion=%d, prompt=%d, total=%d",
            completion_tokens, node_count, verified_completion_tokens, prompt_tokens, tokens_used);

    // ===== POST-INFERENCE BILLING / TRACKING =====
    // CLIENT-SIDE (skip_billing=false): Deduct from escrow and transfer TKNC to miner.
    //   This is where the client pays the miner via on-chain transfer.
    // MINER-SIDE (skip_billing=true): Track served tokens for independent payment verification.
    //   The miner tracks how much inference it has provided, so it can refuse
    //   future requests if the client hasn't paid (CheckMinerReceivedPayment).
    if (!ctx->api_key.empty() && tokens_used > 0 && !content.empty()
        && content.find("[Error:") == std::string::npos
        && content.find("[Miner returned") == std::string::npos
        && content.find("[Inference unavailable:") == std::string::npos
        && content.find("[LLM inference error:") == std::string::npos
        && content.find("[FATAL:") == std::string::npos) {
        if (ctx->skip_billing) {
            // MINER-SIDE: Track served tokens — this is the miner's own counter,
            // used by CheckMinerReceivedPayment to verify payment.
            TrackMinerServedTokens(ctx->api_key, tokens_used);
            LogInfo("[StreamGateway] Miner served %d tokens for api_key=%s... (tracked for payment verification)",
                    tokens_used, ctx->api_key.substr(0, 8).c_str());
        } else {
            // CLIENT-SIDE: Deduct from escrow and transfer TKNC to miner
            BillingReceipt receipt;
            if (CheckAndDeductEscrow(ctx->api_key, tokens_used, receipt)) {
                LogInfo("[StreamGateway] Billing SUCCESS: tokens=%d, cost=%s TKNC, remaining=%s TKNC",
                        tokens_used, FormatMoney(receipt.cost_tknc).c_str(),
                        FormatMoney(receipt.remaining_limit).c_str());
            } else {
                LogWarning("[StreamGateway] Billing FAILED: tokens=%d — escrow exhausted/expired/invalid",
                           tokens_used);
            }
        }
    }

    // Release the reserved cost (prevents concurrent flood bypass)
    if (ctx->reserved_cost > 0 && !ctx->api_key.empty()) {
        ReleaseEscrowCost(ctx->api_key, ctx->reserved_cost);
        ctx->reserved_cost = 0;
    }

    // ===== Non-stream mode: send JSON response (OpenAI compatible) =====
    if (ctx->non_stream_mode) {
        std::string json_resp = "{\n"
            "  \"id\": \"" + ctx->chat_id + "\",\n"
            "  \"object\": \"chat.completion\",\n"
            "  \"created\": " + std::to_string(ctx->created) + ",\n"
            "  \"model\": \"" + JsonEscape(ctx->model) + "\",\n"
            "  \"choices\": [\n"
            "    {\n"
            "      \"index\": 0,\n"
            "      \"message\": {\n"
            "        \"role\": \"assistant\",\n"
            "        \"content\": \"" + JsonEscape(content) + "\"\n"
            "      },\n"
            "      \"finish_reason\": \"stop\"\n"
            "    }\n"
            "  ],\n"
            "  \"usage\": {\n"
            "    \"prompt_tokens\": " + std::to_string(prompt_tokens) + ",\n"
            "    \"completion_tokens\": " + std::to_string(completion_tokens) + ",\n"
            "    \"total_tokens\": " + std::to_string(tokens_used) + "\n"
            "  }\n"
            "}\n";

        struct evbuffer* buf = evbuffer_new();
        evbuffer_add(buf, json_resp.c_str(), json_resp.size());
        struct evkeyvalq* out_hdrs = evhttp_request_get_output_headers(ctx->client_req);
        evhttp_add_header(out_hdrs, "Content-Type", "application/json");
        AddCorsHeader(out_hdrs);
        evhttp_send_reply(ctx->client_req, 200, "OK", buf);
        evbuffer_free(buf);

        LogInfo("[StreamGateway] Non-stream JSON response sent: content_length=%zu, tokens=%d",
                content.size(), tokens_used);
        StreamSafeDelete(ctx);
        return;
    }

    // ===== Stream mode: SSE response =====

    // If SSE data was already forwarded (true streaming), just end the response
    if (ctx->sse_forwarded) {
        LogInfo("[StreamGateway] True streaming complete, ending response");
        evhttp_send_reply_end(ctx->client_req);
        StreamSafeDelete(ctx);
        return;
    }

    // Fallback: miner returned non-SSE (error or blocking JSON), parse and send as SSE
    LogInfo("[StreamGateway] Fallback: processing miner response: body_size=%zu", miner_body.size());

    std::string content_chunk = "data: {\"id\":\"" + ctx->chat_id + "\","
        "\"object\":\"chat.completion.chunk\","
        "\"created\":" + std::to_string(ctx->created) + ","
        "\"model\":\"" + ctx->model + "\","
        "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"" + JsonEscape(content) + "\"},\"finish_reason\":null}]}\n\n";

    struct evbuffer* content_buf = evbuffer_new();
    evbuffer_add(content_buf, content_chunk.c_str(), content_chunk.size());
    evhttp_send_reply_chunk(ctx->client_req, content_buf);
    evbuffer_free(content_buf);

    std::string finish_chunk = "data: {\"id\":\"" + ctx->chat_id + "\","
        "\"object\":\"chat.completion.chunk\","
        "\"created\":" + std::to_string(ctx->created) + ","
        "\"model\":\"" + ctx->model + "\","
        "\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";

    struct evbuffer* finish_buf = evbuffer_new();
    evbuffer_add(finish_buf, finish_chunk.c_str(), finish_chunk.size());
    evhttp_send_reply_chunk(ctx->client_req, finish_buf);
    evbuffer_free(finish_buf);

    evhttp_send_reply_end(ctx->client_req);

    LogInfo("[StreamGateway] Fallback streaming complete: content_length=%zu", content.size());

    StreamSafeDelete(ctx);
}

// Forward SSE data to client (called from StreamMinerReadCb when de-chunked data is ready)
static void StreamForwardToClient(StreamContext* ctx, const char* data, size_t len) {
    if (!ctx || ctx->finished || ctx->client_disconnected || len == 0) return;

    // In non-stream mode, don't forward SSE chunks — collect full response for JSON
    if (ctx->non_stream_mode) return;

    // === Independent token counting: count SSE chunks with non-empty content ===
    // Each SSE "data:" line with "content":"<non-empty>" = 1 LLM output token.
    // This is the node's independent count, used for billing verification.
    // The miner cannot inflate this count because the node counts what actually passes through.
    {
        std::string str(data, len);
        size_t pos = 0;
        while ((pos = str.find("\"content\":\"", pos)) != std::string::npos) {
            size_t content_start = pos + 11;  // length of "content":"
            if (content_start < str.size() && str[content_start] != '"') {
                // Non-empty content = 1 token
                ctx->node_output_token_count++;

                // Accumulate content for additional verification
                size_t content_end = str.find('"', content_start);
                if (content_end != std::string::npos) {
                    ctx->output_content_accumulated += str.substr(content_start, content_end - content_start);
                }
            }
            pos = content_start;
        }
    }

    struct evbuffer* buf = evbuffer_new();
    evbuffer_add(buf, data, len);
    evhttp_send_reply_chunk(ctx->client_req, buf);
    evbuffer_free(buf);
    ctx->sse_forwarded = true;
}

static void StreamMinerReadCb(struct bufferevent* bev, void* arg) {
    StreamContext* ctx = static_cast<StreamContext*>(arg);
    if (!ctx || ctx->finished) return;

    struct evbuffer* input = bufferevent_get_input(bev);
    size_t len = evbuffer_get_length(input);
    if (len == 0) return;

    std::string data(len, '\0');
    evbuffer_remove(input, &data[0], len);
    ctx->miner_response_accumulated += data;

    LogInfo("[StreamGateway] Received %zu bytes from miner (state=%d, total=%zu)",
            len, ctx->chunk_state, ctx->miner_response_accumulated.size());

    // Process through chunked decoder (or raw passthrough if not chunked)
    for (size_t i = 0; i < data.size() && !ctx->finished; i++) {
        char c = data[i];

        switch (ctx->chunk_state) {
            case StreamContext::CHUNK_HEADERS:
                ctx->chunk_line += c;
                if (ctx->chunk_line.size() >= 4 &&
                    ctx->chunk_line.compare(ctx->chunk_line.size() - 4, 4, "\r\n\r\n") == 0) {
                    // Check if response uses chunked transfer encoding
                    ctx->is_chunked = (ctx->chunk_line.find("Transfer-Encoding: chunked") != std::string::npos ||
                                       ctx->chunk_line.find("Transfer-Encoding:  chunked") != std::string::npos);
                    LogInfo("[StreamGateway] Headers parsed: chunked=%d", ctx->is_chunked);
                    ctx->headers_parsed = true;
                    ctx->chunk_line.clear();
                    if (ctx->is_chunked) {
                        ctx->chunk_state = StreamContext::CHUNK_LENGTH;
                    } else {
                        // Non-chunked: forward body directly (delimited by connection close)
                        ctx->chunk_state = StreamContext::CHUNK_DATA;
                        ctx->chunk_bytes_remaining = (size_t)-1;  // Unlimited
                    }
                }
                break;

            case StreamContext::CHUNK_LENGTH:
                if (c == '\r') {
                    // skip
                } else if (c == '\n') {
                    ctx->chunk_bytes_remaining = strtoul(ctx->chunk_line.c_str(), nullptr, 16);
                    ctx->chunk_line.clear();
                    if (ctx->chunk_bytes_remaining == 0) {
                        ctx->chunk_state = StreamContext::CHUNK_DONE;
                        LogInfo("[StreamGateway] Final chunk (0-length) received");
                    } else {
                        ctx->chunk_state = StreamContext::CHUNK_DATA;
                    }
                } else {
                    ctx->chunk_line += c;
                }
                break;

            case StreamContext::CHUNK_DATA: {
                if (ctx->is_chunked) {
                    size_t avail = std::min(data.size() - i, ctx->chunk_bytes_remaining);
                    if (avail > 0) {
                        StreamForwardToClient(ctx, data.c_str() + i, avail);
                        LogInfo("[StreamGateway] Forwarded %zu bytes of SSE data", avail);
                    }
                    i += avail - 1;
                    ctx->chunk_bytes_remaining -= avail;
                    if (ctx->chunk_bytes_remaining == 0) {
                        ctx->chunk_state = StreamContext::CHUNK_TRAILER;
                    }
                } else {
                    // Non-chunked: forward everything after headers
                    size_t avail = data.size() - i;
                    StreamForwardToClient(ctx, data.c_str() + i, avail);
                    LogInfo("[StreamGateway] Forwarded %zu bytes (non-chunked)", avail);
                    i = data.size();  // Done with this batch
                }
                break;
            }

            case StreamContext::CHUNK_TRAILER:
                if (c == '\n') {
                    ctx->chunk_state = StreamContext::CHUNK_LENGTH;
                }
                break;

            case StreamContext::CHUNK_DONE:
                // Ignore trailing data
                break;
        }
    }
}

static void StreamMinerEventCb(struct bufferevent* bev, short what, void* arg) {
    StreamContext* ctx = static_cast<StreamContext*>(arg);
    if (!ctx) return;

    if (what & BEV_EVENT_CONNECTED) {
        LogInfo("[StreamGateway] Connected to miner, sending request (%zu bytes)",
                ctx->miner_http_request.size());
        bufferevent_write(bev, ctx->miner_http_request.c_str(),
                         ctx->miner_http_request.size());
        return;
    }

    if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
        LogInfo("[StreamGateway] Miner event: 0x%x (EOF=%d ERR=%d TIMEOUT=%d), sse_forwarded=%d",
                what, (what & BEV_EVENT_EOF) != 0, (what & BEV_EVENT_ERROR) != 0,
                (what & BEV_EVENT_TIMEOUT) != 0, ctx->sse_forwarded);
        StreamFinishAndCleanup(ctx);
    }
}

static void StreamClientCloseCb(struct evhttp_connection* conn, void* arg) {
    StreamContext* ctx = static_cast<StreamContext*>(arg);
    if (!ctx) return;

    // CRITICAL: Check if StreamFinishAndCleanup already deleted this context.
    // If finished=true, ctx is still alive (StreamFinishAndCleanup hasn't deleted yet)
    // because we remove the closecb BEFORE deleting in StreamFinishAndCleanup.
    // So if we get here with finished=true, it means cleanup is in progress.
    if (ctx->finished) {
        LogInfo("[StreamGateway] Client disconnected, but cleanup already in progress (finished=true)");
        return;
    }

    LogInfo("[StreamGateway] Client disconnected, cleaning up");
    ctx->client_disconnected = true;

    // Don't directly free miner_bev here — let StreamFinishAndCleanup handle it.
    // Just mark disconnected and call StreamFinishAndCleanup for proper cleanup.
    StreamFinishAndCleanup(ctx);
}

static void HttpPostToMinerProgressive(const std::string& host, int port,
    const std::string& path,
    const std::string& body,
    struct evhttp_request* req,
    const std::string& chat_id,
    const std::string& model,
    int64_t created,
    bool non_stream_mode = false,
    const std::string& api_key = "",
    const std::string& prompt = "",
    int max_tokens = 0,
    int timeout_sec = MINER_REQUEST_TIMEOUT_SEC,
    bool skip_billing = false,
    CAmount reserved_cost = 0) {
    LogInfo("[StreamGateway] Starting ASYNC %s to miner %s:%d (non_stream=%d)",
            non_stream_mode ? "request" : "streaming", host.c_str(), port, non_stream_mode);

    StreamContext* ctx = new StreamContext;
    ctx->client_req = req;
    ctx->chat_id = chat_id;
    ctx->model = model;
    ctx->created = created;
    ctx->finished = false;
    ctx->client_disconnected = false;
    ctx->miner_bev = nullptr;
    ctx->keepalive_timer = nullptr;
    ctx->client_conn = nullptr;
    ctx->chunk_state = StreamContext::CHUNK_HEADERS;
    ctx->chunk_bytes_remaining = 0;
    ctx->headers_parsed = false;
    ctx->is_chunked = false;
    ctx->sse_forwarded = false;
    ctx->non_stream_mode = non_stream_mode;
    ctx->api_key = api_key;
    ctx->prompt = prompt;
    ctx->max_tokens = max_tokens;
    ctx->skip_billing = skip_billing;
    ctx->reserved_cost = reserved_cost;

    std::ostringstream http_req;
    http_req << "POST " << path << " HTTP/1.1\r\n";
    // Build Host header: [IPv6]:port or IPv4:port
    if (host.find(':') != std::string::npos) {
        http_req << "Host: [" << host << "]:" << port << "\r\n";
    } else {
        http_req << "Host: " << host << ":" << port << "\r\n";
    }
    http_req << "Content-Type: application/json\r\n";
    http_req << "Content-Length: " << body.size() << "\r\n";
    // Add skip-billing header when forwarding to remote gateway
    // This tells the remote gateway not to do billing (billing is handled by this node)
    // Use deterministic API-key-based secret for cross-node verification.
    if (host != "127.0.0.1" && host != "localhost") {
        std::string proxy_secret = ComputeSkipBillingSecret(api_key);
        if (!proxy_secret.empty()) {
            http_req << "X-TKNC-Skip-Billing: " << proxy_secret << "\r\n";
        }
        // Include payment txids so the miner can independently verify payment.
        // The miner checks each txid against its OWN wallet — it does NOT trust
        // this data blindly. Only txids that actually appear in the miner's
        // wallet are counted as received payment.
        if (!api_key.empty()) {
            auto esc = FindSpendingLimitByAPIKey(api_key);
            if (esc.has_value() && !esc->pending_txids.empty()) {
                http_req << "X-TKNC-Pending-Txids: " << esc->pending_txids << "\r\n";
            }
        }
    }
    http_req << "Connection: close\r\n";
    http_req << "\r\n";
    http_req << body;
    ctx->miner_http_request = http_req.str();

    // In stream mode: send SSE headers immediately and start keepalive.
    // In non-stream mode: defer headers — response will be sent as complete JSON.
    if (!non_stream_mode) {
        struct evkeyvalq* out_hdrs = evhttp_request_get_output_headers(req);
        evhttp_add_header(out_hdrs, "Content-Type", "text/event-stream");
        evhttp_add_header(out_hdrs, "Cache-Control", "no-cache");
        evhttp_add_header(out_hdrs, "Connection", "keep-alive");
        AddCorsHeader(out_hdrs);
        evhttp_send_reply_start(req, 200, "OK");
        LogInfo("[StreamGateway] Sent SSE headers, creating async miner connection");
    } else {
        LogInfo("[StreamGateway] Non-stream mode: deferring headers, creating async miner connection");
    }

    ctx->client_conn = evhttp_request_get_connection(req);
    if (ctx->client_conn) {
        evhttp_connection_set_closecb(ctx->client_conn, StreamClientCloseCb, ctx);
    }

    ctx->miner_bev = bufferevent_socket_new(g_gateway_base, -1, BEV_OPT_CLOSE_ON_FREE);
    if (!ctx->miner_bev) {
        LogError("[StreamGateway] bufferevent_socket_new failed");
        if (non_stream_mode) {
            struct evbuffer* err_buf = evbuffer_new();
            evbuffer_add_printf(err_buf, R"({"error":{"message":"Internal error","type":"server_error"}})");
            evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
            AddCorsHeader(evhttp_request_get_output_headers(req));
            evhttp_send_reply(req, 500, "Internal Server Error", err_buf);
            evbuffer_free(err_buf);
        } else {
            struct evbuffer* err_buf = evbuffer_new();
            evbuffer_add_printf(err_buf, "data: {\"error\":\"Internal error\"}\n\ndata: [DONE]\n\n");
            evhttp_send_reply_chunk(req, err_buf);
            evbuffer_free(err_buf);
            evhttp_send_reply_end(req);
        }
        StreamSafeDelete(ctx);
        return;
    }

    bufferevent_setcb(ctx->miner_bev, StreamMinerReadCb, nullptr, StreamMinerEventCb, ctx);
    bufferevent_enable(ctx->miner_bev, EV_READ | EV_WRITE);

    struct timeval tv_timeout = {timeout_sec, 0};
    bufferevent_set_timeouts(ctx->miner_bev, &tv_timeout, &tv_timeout);

    // Keepalive only needed in stream mode (non-stream mode hasn't started response yet)
    if (!non_stream_mode) {
        ctx->keepalive_timer = evtimer_new(g_gateway_base, StreamKeepaliveCb, ctx);
        struct timeval tv_keepalive = {2, 0};
        event_add(ctx->keepalive_timer, &tv_keepalive);
    }

    // Support both IPv4 and IPv6 miner addresses
    bool is_ipv6 = (host.find(':') != std::string::npos);
    int ret;
    if (is_ipv6) {
        struct sockaddr_in6 miner_addr6;
        memset(&miner_addr6, 0, sizeof(miner_addr6));
        miner_addr6.sin6_family = AF_INET6;
        miner_addr6.sin6_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET6, host.c_str(), &miner_addr6.sin6_addr);
        ret = bufferevent_socket_connect(ctx->miner_bev,
            reinterpret_cast<struct sockaddr*>(&miner_addr6), sizeof(miner_addr6));
    } else {
        struct sockaddr_in miner_addr;
        memset(&miner_addr, 0, sizeof(miner_addr));
        miner_addr.sin_family = AF_INET;
        miner_addr.sin_port = htons(static_cast<uint16_t>(port));
#ifdef WIN32
        miner_addr.sin_addr.S_un.S_addr = inet_addr(host.c_str());
#else
        inet_pton(AF_INET, host.c_str(), &miner_addr.sin_addr);
#endif
        ret = bufferevent_socket_connect(ctx->miner_bev,
            reinterpret_cast<struct sockaddr*>(&miner_addr), sizeof(miner_addr));
    }
    if (ret < 0) {
        LogError("[StreamGateway] bufferevent_socket_connect failed");
        if (non_stream_mode) {
            struct evbuffer* err_buf = evbuffer_new();
            evbuffer_add_printf(err_buf, R"({"error":{"message":"Cannot connect to miner","type":"server_error"}})");
            evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
            AddCorsHeader(evhttp_request_get_output_headers(req));
            evhttp_send_reply(req, 502, "Bad Gateway", err_buf);
            evbuffer_free(err_buf);
        } else {
            struct evbuffer* err_buf = evbuffer_new();
            evbuffer_add_printf(err_buf, "data: {\"error\":\"Cannot connect to miner\"}\n\ndata: [DONE]\n\n");
            evhttp_send_reply_chunk(req, err_buf);
            evbuffer_free(err_buf);
            evhttp_send_reply_end(req);
            if (ctx->keepalive_timer) { event_free(ctx->keepalive_timer); ctx->keepalive_timer = nullptr; }
        }
        bufferevent_free(ctx->miner_bev);
        ctx->miner_bev = nullptr;
        StreamSafeDelete(ctx);
        return;
    }

    LogInfo("[StreamGateway] Async connection initiated, returning to event loop");
}

static std::string ReadEvHttpBody(struct evhttp_request* req) {
    struct evbuffer* buf = evhttp_request_get_input_buffer(req);
    if (!buf) return "";
    size_t len = evbuffer_get_length(buf);
    if (len == 0) return "";
    std::string body(len, '\0');
    evbuffer_copyout(buf, &body[0], len);
    return body;
}

static std::pair<bool, std::string> GetEvHttpHeader(struct evhttp_request* req, const char* key) {
    // For incoming requests, read from input headers
    const char* val = evhttp_find_header(evhttp_request_get_input_headers(req), key);
    if (val) return {true, std::string(val)};
    return {false, ""};
}

// Async P2P inference context: polls future every 100ms via libevent timer to avoid blocking the event loop.
struct AsyncP2PCtx {
    struct evhttp_request* req;
    std::string chat_id;
    std::string model;
    int64_t created;
    std::string api_key;
    std::string prompt;
    int max_tokens;  // -1=unlimited, 0=not specified, >0=limit. Passed through to P2P peer.
    std::future<APIResponse> future;
    struct event* timer;
    int retry_count;
    int max_retries;
    std::vector<NodeId> candidate_peers;
    int current_peer_idx;
node::NodeContext* node_ctx;
bool client_gone;
    struct evhttp_connection* conn;
    std::chrono::steady_clock::time_point start_time;
};

static void AsyncP2PTimerCb(evutil_socket_t, short, void* arg);

static void AsyncP2PSendErrorAndClose(AsyncP2PCtx* ctx, const std::string& err_msg) {
    std::ostringstream err_chunk;
    err_chunk << "data: {\"id\":\"" << ctx->chat_id << "\",\"object\":\"chat.completion.chunk\","
              << "\"created\":" << ctx->created << ",\"model\":\"" << JsonEscape(ctx->model) << "\","
              << "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"" << JsonEscape(err_msg) << "\"},\"finish_reason\":null}]}\n\n";
    std::ostringstream fin_chunk;
    fin_chunk << "data: {\"id\":\"" << ctx->chat_id << "\",\"object\":\"chat.completion.chunk\","
              << "\"created\":" << ctx->created << ",\"model\":\"" << JsonEscape(ctx->model) << "\","
              << "\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
    struct evbuffer* buf = evbuffer_new();
    evbuffer_add(buf, err_chunk.str().c_str(), err_chunk.str().size());
    evbuffer_add(buf, fin_chunk.str().c_str(), fin_chunk.str().size());
    evhttp_send_reply_chunk(ctx->req, buf);
    evbuffer_free(buf);
    evhttp_send_reply_end(ctx->req);
    LogInfo("[AsyncP2P] Error sent: %s", err_msg.c_str());
}

static void AsyncP2PSendSuccessAndClose(AsyncP2PCtx* ctx, const std::string& content,
                                         int prompt_tokens, int completion_tokens) {
    // === Independent token verification (P2P path) ===
    // The client's node receives completion_tokens from the miner's node via P2P.
    // Verify against content-based estimate to detect inflation.
    int verified_completion = completion_tokens;
    if (!content.empty() && completion_tokens > 0) {
        // Content-based estimate: ~4 chars per token for English, ~1.5 for CJK
        int estimated = static_cast<int>(content.length() / 4);
        if (estimated > 0) {
            if (completion_tokens > static_cast<int>(estimated * 1.3)) {
                LogWarning("[AsyncP2P] TOKEN ANOMALY: miner reported completion_tokens=%d, "
                           "but content-based estimate is %d (>30%% discrepancy). "
                           "Using estimated count to protect client.",
                           completion_tokens, estimated);
                verified_completion = estimated;
            } else if (completion_tokens < static_cast<int>(estimated * 0.7)) {
                LogWarning("[AsyncP2P] TOKEN ANOMALY: miner reported completion_tokens=%d, "
                           "but content-based estimate is %d (<30%% discrepancy). "
                           "Using estimated count for accuracy.",
                           completion_tokens, estimated);
                verified_completion = estimated;
            }
        }
    }

    // Billing
    int tokens_used = prompt_tokens + verified_completion;
    if (tokens_used == 0 && !content.empty()) {
        tokens_used = static_cast<int>(content.length() / 4);
    }
    if (tokens_used == 0) tokens_used = static_cast<int>(ctx->prompt.length() / 4);
    if (tokens_used < 1) tokens_used = 1;

    LogInfo("[AsyncP2P] Token verification: miner_completion=%d, verified=%d, prompt=%d, total=%d",
            completion_tokens, verified_completion, prompt_tokens, tokens_used);

    // SECURITY: Do NOT bill if the miner returned an error/empty response.
    // The content may contain error markers like [Error:], [Miner returned,
    // [Inference unavailable:], [LLM inference error:], or [FATAL:].
    // Billing for failed inference would drain the user's wallet without providing service.
    bool miner_error = (content.empty()
        || content.find("[Error:") != std::string::npos
        || content.find("[Miner returned") != std::string::npos
        || content.find("[Inference unavailable:") != std::string::npos
        || content.find("[LLM inference error:") != std::string::npos
        || content.find("[FATAL:") != std::string::npos);

    if (!ctx->api_key.empty() && tokens_used > 0 && !miner_error) {
        BillingReceipt receipt;
        if (CheckAndDeductEscrow(ctx->api_key, tokens_used, receipt)) {
            LogInfo("[AsyncP2P] Billing SUCCESS: tokens=%d, cost=%s TKNC",
                    tokens_used, FormatMoney(receipt.cost_tknc).c_str());
        } else {
            LogWarning("[AsyncP2P] Billing FAILED: tokens=%d", tokens_used);
        }
    } else if (miner_error) {
        LogWarning("[AsyncP2P] SKIPPED billing: miner returned error/empty response "
                   "(tokens=%d, content_prefix=%.60s)",
                   tokens_used, content.substr(0, 60).c_str());
    }

    // Content delta
    struct evbuffer* sse_buf = evbuffer_new();
    if (!content.empty()) {
        std::ostringstream chunk2;
        chunk2 << "data: {\"id\":\"" << ctx->chat_id << "\",\"object\":\"chat.completion.chunk\","
               << "\"created\":" << ctx->created << ",\"model\":\"" << JsonEscape(ctx->model) << "\","
               << "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"" << JsonEscape(content) << "\"},\"finish_reason\":null}]}\n\n";
        evbuffer_add(sse_buf, chunk2.str().c_str(), chunk2.str().size());
    }
    // Finish + [DONE]
    std::ostringstream chunk3;
    chunk3 << "data: {\"id\":\"" << ctx->chat_id << "\",\"object\":\"chat.completion.chunk\","
           << "\"created\":" << ctx->created << ",\"model\":\"" << JsonEscape(ctx->model) << "\","
           << "\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
    evbuffer_add(sse_buf, chunk3.str().c_str(), chunk3.str().size());

    evhttp_send_reply_chunk(ctx->req, sse_buf);
    evhttp_send_reply_end(ctx->req);
    evbuffer_free(sse_buf);

    auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - ctx->start_time).count();
    LogInfo("[AsyncP2P] SSE success sent: content_length=%zu, tokens=%d, elapsed=%.0fms",
            content.size(), tokens_used, elapsed);
}

// Forward declaration
static void AsyncP2PCleanup(AsyncP2PCtx* ctx) {
    if (!ctx) return;

    // CRITICAL: Remove close callback BEFORE deleting to prevent use-after-free
    if (ctx->conn) {
        evhttp_connection_set_closecb(ctx->conn, nullptr, nullptr);
        ctx->conn = nullptr;
    }

    if (ctx->timer) {
        event_del(ctx->timer);
        event_free(ctx->timer);
        ctx->timer = nullptr;
    }
    delete ctx;
}

static void AsyncP2PReschedule(AsyncP2PCtx* ctx) {
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100 * 1000;  // 100ms
    event_add(ctx->timer, &tv);
}

// Try sending to next peer. Returns true if request was sent (timer rescheduled),
// false if no more peers to try (caller should send error + cleanup).
static bool AsyncP2PTryNextPeer(AsyncP2PCtx* ctx) {
    while (ctx->current_peer_idx < (int)ctx->candidate_peers.size()) {
        NodeId target_peer = ctx->candidate_peers[ctx->current_peer_idx];
        LogInfo("[AsyncP2P] Trying peer %d (attempt %d/%d)", target_peer,
                ctx->current_peer_idx + 1, ctx->max_retries);

        APIRequest api_req;
        api_req.api_key = ctx->api_key;
        api_req.model = ctx->model;
        api_req.max_tokens = ctx->max_tokens;
        {
            static std::atomic<uint64_t> s_req_counter{0};
            uint64_t cv = s_req_counter.fetch_add(1);
            api_req.request_id = static_cast<uint64_t>(GetTime()) * 1000000 + cv;
            api_req.nonce = static_cast<uint64_t>(GetTime()) * 1000000 + cv + 1;
        }
        std::string sig_str = "gateway_p2p_relay";
        api_req.signature.assign(sig_str.begin(), sig_str.end());
        ChatMessage msg;
        msg.role = "user";
        msg.content = ctx->prompt;
        api_req.messages.push_back(msg);

        auto& node = *ctx->node_ctx;
        PeerManager* peerman = node.peerman.get();
        try {
            ctx->future = peerman->SendInferenceRequest(target_peer, api_req);
            LogInfo("[AsyncP2P] Sent to peer %d, waiting...", target_peer);
            ctx->start_time = std::chrono::steady_clock::now();
            AsyncP2PReschedule(ctx);
            return true;
        } catch (const std::exception& e) {
            LogError("[AsyncP2P] SendInferenceRequest threw: %s", e.what());
            ctx->current_peer_idx++;
            // Loop to try next peer
        }
    }
    return false;  // No more peers
}

static void __attribute__((unused)) AsyncP2PTimerCb(evutil_socket_t, short, void* arg) {
    AsyncP2PCtx* ctx = static_cast<AsyncP2PCtx*>(arg);
    if (!ctx || ctx->client_gone) {
        if (ctx) { AsyncP2PCleanup(ctx); }
        return;
    }

    // Check if future is ready (non-blocking — 0ms wait)
    auto status = ctx->future.wait_for(std::chrono::milliseconds(0));
    if (status == std::future_status::ready) {
        APIResponse response;
        try {
            response = ctx->future.get();
        } catch (const std::exception& e) {
            LogError("[AsyncP2P] future.get() threw: %s", e.what());
            ctx->current_peer_idx++;
            if (AsyncP2PTryNextPeer(ctx)) return;
            AsyncP2PSendErrorAndClose(ctx, "[Error: P2P inference failed]");
            AsyncP2PCleanup(ctx);
            return;
        }

        std::string content = response.content;
        if (!content.empty() &&
            (content.find("[Error:") != std::string::npos ||
             content.find("[Miner returned") != std::string::npos ||
             content.find("[LLM inference error:") != std::string::npos ||
             content.find("[FATAL:") != std::string::npos ||
             content.find("[Inference unavailable:") != std::string::npos ||
             content.find("Inference unavailable:") != std::string::npos)) {
            LogWarning("[AsyncP2P] Peer returned error: %s", content.c_str());
            ctx->current_peer_idx++;
            if (AsyncP2PTryNextPeer(ctx)) return;
            AsyncP2PSendErrorAndClose(ctx, content);
            AsyncP2PCleanup(ctx);
            return;
        }

        // Success!
        LogInfo("[AsyncP2P] Inference success: content_length=%zu", content.size());
        AsyncP2PSendSuccessAndClose(ctx, content, response.prompt_tokens, response.completion_tokens);
        AsyncP2PCleanup(ctx);
        return;
    }

    // Not ready yet — check timeout (120 seconds per attempt)
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - ctx->start_time).count();
    if (elapsed > 120.0) {
        LogWarning("[AsyncP2P] Timed out after 120s, trying next peer");
        ctx->current_peer_idx++;
        if (AsyncP2PTryNextPeer(ctx)) return;
        AsyncP2PSendErrorAndClose(ctx, "[Error: P2P inference timed out]");
        AsyncP2PCleanup(ctx);
        return;
    }

    // Reschedule timer for 100ms
    AsyncP2PReschedule(ctx);
}

static void __attribute__((unused)) AsyncP2PClientCloseCb(struct evhttp_connection*, void* arg) {
    AsyncP2PCtx* ctx = static_cast<AsyncP2PCtx*>(arg);
    if (ctx) {
        ctx->client_gone = true;
        LogInfo("[AsyncP2P] Client disconnected");
        // Note: do NOT delete ctx here. The timer callback will detect
        // client_gone=true and call AsyncP2PCleanup which properly removes
        // this close callback before deleting.
    }
}

static void HandleChatCompletions(struct evhttp_request* req) {
    if (!req) return;

    enum evhttp_cmd_type cmd = evhttp_request_get_command(req);

    // Handle CORS preflight (IDE/cursor/cline send OPTIONS before POST)
    if (cmd == EVHTTP_REQ_OPTIONS) {
        struct evbuffer* buf = evbuffer_new();
        struct evkeyvalq* out_hdrs = evhttp_request_get_output_headers(req);
        AddCorsHeader(out_hdrs);
        evhttp_add_header(out_hdrs, "Access-Control-Allow-Methods", "POST, OPTIONS");
        evhttp_add_header(out_hdrs, "Access-Control-Allow-Headers", "Content-Type, Authorization");
        evhttp_add_header(out_hdrs, "Access-Control-Max-Age", "86400");
        evhttp_send_reply(req, 200, "OK", buf);
        evbuffer_free(buf);
        return;
    }

    // Only accept POST
    if (cmd != EVHTTP_REQ_POST) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, R"({"error":{"message":"Method not allowed","type":"invalid_request_error"}})");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
        evhttp_send_reply(req, 405, "Method Not Allowed", buf);
        evbuffer_free(buf);
        return;
    }

    // Read body
    std::string body = ReadEvHttpBody(req);
    if (body.empty()) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, R"({"error":{"message":"Empty request body","type":"invalid_request_error"}})");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }

    LogInfo("[InferenceGateway] Request body size: %zu bytes, body: %.500s", body.size(), body.c_str());

    // Parse request
    SimpleJsonParser json(body);

    // 1. Authenticate via Bearer Token (api_key)
    auto auth_header = GetEvHttpHeader(req, "Authorization");
    std::string api_key;

    if (auth_header.first) {
        const std::string& auth_val = auth_header.second;
        if (auth_val.size() > 7 && auth_val.substr(0, 7) == "Bearer ") {
            api_key = auth_val.substr(7);
            while (!api_key.empty() && api_key[0] == ' ')
                api_key.erase(api_key.begin());
        }
    }

    if (api_key.empty() && json.hasKey("api_key")) {
        api_key = json.getString("api_key");
    }

    if (api_key.empty()) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, R"({"error":{"message":"Missing API key. Provide Authorization: Bearer header or api_key in body.","type":"authentication_error"}})");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
        evhttp_send_reply(req, 401, "Unauthorized", buf);
        evbuffer_free(buf);
        return;
    }

    // Validate API key format — reject invalid keys early to prevent free inference.
    if (!ValidateAPIKeyFormat(api_key)) {
        LogWarning("[InferenceGateway] Rejected invalid API key format (length=%zu)", api_key.size());
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, R"({"error":{"message":"Invalid API key format. API keys must start with 'tknc_' followed by 32 hex characters.","type":"authentication_error"}})");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
        evhttp_send_reply(req, 401, "Unauthorized", buf);
        evbuffer_free(buf);
        return;
    }

    LogInfo("[InferenceGateway] API Key: %s...%s (length=%zu)",
             api_key.substr(0, 6).c_str(),
             api_key.size() > 10 ? api_key.substr(api_key.size() - 6).c_str() : "",
             api_key.size());

    // 2. Get model name (optional - miner will use its loaded model if empty)
    std::string model = json.getString("model");
    // No whitelist validation - the miner validates the model against its loaded model.
    // This allows any model file to be loaded by the miner without node-side restrictions.

    // 3. Extract messages/prompt
    std::string prompt = json.extractPromptFromMessages();
    if (prompt.empty()) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, R"({"error":{"message":"Messages array is empty or invalid","type":"invalid_request_error"}})");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }

    // 4. Stream mode — always streaming (SSE)
    LogInfo("[InferenceGateway] Streaming mode: ALWAYS ON (unified SSE streaming)");

    // 4b. Parse max_tokens (-1=unlimited, 0=not specified, >0=limit)
    int max_tokens = 0;
    if (json.hasKey("max_tokens")) {
        max_tokens = (int)json.getInt("max_tokens", 0);
    } else if (json.hasKey("n_predict")) {
        max_tokens = (int)json.getInt("n_predict", 0);
    }
    LogInfo("[InferenceGateway] max_tokens=%d (%s)", max_tokens, max_tokens < 0 ? "unlimited" : max_tokens == 0 ? "not specified" : "limited");

    LogInfo("[InferenceGateway] Model=%s, Prompt length=%zu", model.c_str(), prompt.size());

    // Stream mode early-return block removed to allow P2P fallback for streaming requests.
    // Wallet lock pre-check removed — CheckAndDeductEscrow() handles it at billing time.

    // No synchronous handshake — it would block the event loop. Billing is deferred to StreamFinishAndCleanup.
    // Forward to local miner via non-blocking bufferevent.

    // Pre-generate chat_id and created timestamp
    std::string chat_id = "chatcmpl-tknc-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    int64_t created = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Add CORS headers
    struct evkeyvalq* out_hdrs = evhttp_request_get_output_headers(req);
    AddCorsHeader(out_hdrs);
    evhttp_add_header(out_hdrs, "Access-Control-Allow-Methods", "POST, OPTIONS, GET");
    evhttp_add_header(out_hdrs, "Access-Control-Allow-Headers", "Content-Type, Authorization");

    // Build miner request body, pass through all client parameters (bridge architecture).
    std::string max_tokens_str;
    if (max_tokens != 0) {
        max_tokens_str = ",\n  \"max_tokens\": " + std::to_string(max_tokens);
    }
    std::string miner_request_body = "{\n";
    miner_request_body += "  \"api_key\": \"" + api_key + "\",\n";
    miner_request_body += "  \"messages\": [{\"role\": \"user\", \"content\": \"" + JsonEscape(prompt) + "\"}],\n";
    miner_request_body += "  \"stream\": true\n";  // Always streaming — miner uses GenerateStream
    miner_request_body += max_tokens_str;
    miner_request_body += "}";

    LogInfo("[InferenceGateway] Forwarding to miner at %s:%d (stream=ALWAYS_TRUE, max_tokens=%d)...",
            MINER_LOCAL_HOST, MINER_LOCAL_PORT, max_tokens);

    // Determine target: local miner or remote gateway (when proxy target is set)
    // When the user has configured a remote miner via tknc_setinferproxytarget,
    // the gateway forwards to the remote gateway instead of the local miner.
    // This enables billing on the client side (where the user wallet is).
    std::string miner_host = MINER_LOCAL_HOST;
    int miner_port = MINER_LOCAL_PORT;
    std::string miner_path = "/api/v1/chat";
    bool use_remote = false;

    if (g_proxy_target_set.load()) {
        std::lock_guard<std::mutex> lock(g_proxy_target_mutex);
        miner_host = g_proxy_target_ip;
        miner_port = g_proxy_target_port;
        miner_path = "/v1/chat/completions";  // Remote gateway uses OpenAI-compatible endpoint
        use_remote = true;
    }

    // Check for skip-billing header (request from another gateway/proxy).
    // SECURITY: Accept skip-billing if:
    //   1. Header matches per-process secret (same process, localhost bypass prevention)
    //   2. Header matches deterministic hash of the API key (cross-node proxy billing)
    InitSkipBillingSecret();
    auto skip_hdr = GetEvHttpHeader(req, "X-TKNC-Skip-Billing");
    bool skip_billing = false;
    if (skip_hdr.first && skip_hdr.second == g_skip_billing_secret) {
        skip_billing = true;
        LogInfo("[InferenceGateway] Skip-billing accepted (valid per-process secret)");
    } else if (skip_hdr.first && !api_key.empty()) {
        // Check deterministic cross-node secret (proxy → miner)
        std::string expected_proxy_secret = ComputeSkipBillingSecret(api_key);
        if (skip_hdr.second == expected_proxy_secret) {
            skip_billing = true;
            LogInfo("[InferenceGateway] Skip-billing accepted (valid proxy secret for api_key=%s...)",
                    api_key.substr(0, 8).c_str());
        }
    }
    if (skip_hdr.first && !skip_billing) {
        LogWarning("[InferenceGateway] BLOCKED skip-billing header — invalid secret (got length=%zu)",
                   skip_hdr.second.size());
    }

    // SECURITY: Verify that this API key is bound to THIS miner's wallet.
    // The escrow record for the API key contains miner_wallet — it must match
    // this node's local miner wallet. This prevents an API key created for
    // miner A from being used on miner B.
    if (!api_key.empty()) {
        std::string local_wallet = GetMinerWalletAddress();
        if (!local_wallet.empty()) {
            auto escrow_opt = FindSpendingLimitByAPIKey(api_key);
            if (escrow_opt.has_value()) {
                const auto& escrow = *escrow_opt;
                if (!escrow.miner_wallet.empty() && escrow.miner_wallet != local_wallet) {
                    LogWarning("[InferenceGateway] REJECTED: API key %s... is bound to miner %s, not this node's miner %s",
                               api_key.substr(0, 8).c_str(),
                               escrow.miner_wallet.substr(0, 16).c_str(),
                               local_wallet.substr(0, 16).c_str());
                    struct evbuffer* err_buf = evbuffer_new();
                    evbuffer_add_printf(err_buf,
                        "{\"error\":{\"message\":\"This API key is not authorized for this miner. "
                        "API keys are bound to a specific miner wallet.\",\"type\":\"authorization_error\"}}");
                    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
                    evhttp_send_reply(req, 403, "Forbidden", err_buf);
                    evbuffer_free(err_buf);
                    return;
                }
            }
        }
    }

    // Two-layer billing gate:
    //   Layer 1 (client-side, advisory): CanAffordInference — checks user wallet balance.
    //   Layer 2 (miner-side, authoritative): CheckMinerReceivedPayment — verifies miner wallet.
    // The miner is the reliable gatekeeper — it controls the compute resources.
    CAmount reserved_cost = 0;
    if (!skip_billing && !api_key.empty()) {
        // === CLIENT-SIDE checks (advisory — client can bypass) ===
        EnsureEscrowForAPIKey(api_key);

        std::string balance_error;
        if (!CanAffordInference(api_key, balance_error)) {
            LogWarning("[InferenceGateway] Inference REFUSED (client-side): %s", balance_error.c_str());
            struct evbuffer* err_buf = evbuffer_new();
            evbuffer_add_printf(err_buf,
                "{\"error\":{\"message\":\"Inference refused: %s\","
                "\"type\":\"insufficient_balance\"}}",
                balance_error.c_str());
            evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
            evhttp_send_reply(req, 402, "Payment Required", err_buf);
            evbuffer_free(err_buf);
            return;
        }

        // Reserve estimated cost to prevent concurrent request flood bypass.
        int64_t estimated_tokens = (max_tokens > 0 ? max_tokens : 100) + (int64_t)(prompt.length() / 4);
        std::string local_mw = GetMinerWalletAddress();
        int64_t tokens_per_tknc = GetTokensPerTknc(local_mw);
        if (tokens_per_tknc <= 0 && !api_key.empty()) {
            auto esc = FindSpendingLimitByAPIKey(api_key);
            if (esc.has_value() && esc->rate_tokens_per_tknc > 0) {
                tokens_per_tknc = esc->rate_tokens_per_tknc;
            }
        }
        CAmount rate_tknc_per_token = (tokens_per_tknc > 0) ? (CAmount)(COIN / tokens_per_tknc) : 1;
        if (rate_tknc_per_token <= 0) rate_tknc_per_token = 1;
        reserved_cost = estimated_tokens * rate_tknc_per_token;
        if (reserved_cost < COIN) reserved_cost = COIN;
        ReserveEscrowCost(api_key, reserved_cost);
        LogInfo("[InferenceGateway] Reserved %s TKNC for api_key=%s... (estimated_tokens=%lld)",
                FormatMoney(reserved_cost).c_str(), api_key.substr(0, 8).c_str(), (long long)estimated_tokens);
    }

    // === MINER-SIDE payment verification (authoritative — always runs) ===
    // This is the ONLY check that matters for security. The miner independently
    // verifies it has received payment by checking its OWN wallet for txids
    // reported by the client. A malicious client cannot fake a transaction
    // in the miner's wallet.
    //
    // When skip_billing=true, the request came from a client proxy. The client
    // includes X-TKNC-Pending-Txids header with payment txids. The miner
    // verifies each txid against its own wallet.
    // When skip_billing=false, this is the client node — the miner's wallet is
    // not here, so CheckMinerReceivedPayment will return true (not the miner).
    if (!api_key.empty()) {
        std::string pending_txids_override;
        if (skip_billing) {
            // Read payment txids from client proxy's HTTP header
            auto txids_hdr = GetEvHttpHeader(req, "X-TKNC-Pending-Txids");
            if (txids_hdr.first) {
                pending_txids_override = txids_hdr.second;
            }
        }

        std::string miner_pay_error;
        if (!CheckMinerReceivedPayment(api_key, miner_pay_error, pending_txids_override)) {
            LogWarning("[InferenceGateway] Inference REFUSED (miner-side): %s", miner_pay_error.c_str());
            if (reserved_cost > 0) ReleaseEscrowCost(api_key, reserved_cost);
            struct evbuffer* err_buf = evbuffer_new();
            evbuffer_add_printf(err_buf,
                "{\"error\":{\"message\":\"Inference refused: %s\","
                "\"type\":\"insufficient_balance\"}}",
                miner_pay_error.c_str());
            evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
            evhttp_send_reply(req, 402, "Payment Required", err_buf);
            evbuffer_free(err_buf);
            return;
        }
    }

    LogInfo("[InferenceGateway] Forwarding to %s at [%s]:%d path=%s (skip_billing=%d)...",
            use_remote ? "REMOTE gateway" : "local miner",
            miner_host.c_str(), miner_port, miner_path.c_str(), skip_billing);

    // Always use async path, never block the event loop.
    // HttpPostToMinerProgressive creates a bufferevent (non-blocking) and returns immediately.
    // The event loop continues processing other requests while the miner works.
    // When the miner responds, StreamMinerReadCb/StreamMinerEventCb handle the response.
    HttpPostToMinerProgressive(miner_host, miner_port,
                                miner_path, miner_request_body,
                                req, chat_id, model, created,
                                false,  // non_stream_mode = ALWAYS false — unified streaming
                                api_key, prompt, max_tokens,
                                MINER_REQUEST_TIMEOUT_SEC,
                                skip_billing, reserved_cost);
    // Return immediately, event loop continues.
    // Response will be sent by StreamFinishAndCleanup when miner completes.
}

/** Static callback wrapper for chat completions */
static void EvHttpChatCompletionsCb(struct evhttp_request* req, void*) {
    const char* uri = evhttp_request_get_uri(req);
    enum evhttp_cmd_type cmd = evhttp_request_get_command(req);
    const char* method = (cmd == EVHTTP_REQ_POST) ? "POST" :
                         (cmd == EVHTTP_REQ_GET) ? "GET" :
                         (cmd == EVHTTP_REQ_OPTIONS) ? "OPTIONS" : "OTHER";
    LogInfo("[InferenceGateway] === Request received: %s %s ===", method, uri ? uri : "(null)");
    HandleChatCompletions(req);
}

/** Handle GET /v1/models — list available models */
static void HandleListModels(struct evhttp_request* req) {
    if (!req) return;

    enum evhttp_cmd_type cmd = evhttp_request_get_command(req);

    // Handle CORS preflight
    if (cmd == EVHTTP_REQ_OPTIONS) {
        struct evbuffer* buf = evbuffer_new();
        struct evkeyvalq* out_hdrs = evhttp_request_get_output_headers(req);
        AddCorsHeader(out_hdrs);
        evhttp_add_header(out_hdrs, "Access-Control-Allow-Methods", "GET, OPTIONS");
        evhttp_add_header(out_hdrs, "Access-Control-Allow-Headers", "Content-Type, Authorization");
        evhttp_add_header(out_hdrs, "Access-Control-Max-Age", "86400");
        evhttp_send_reply(req, 200, "OK", buf);
        evbuffer_free(buf);
        return;
    }

    if (cmd != EVHTTP_REQ_GET) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, R"({"error":{"message":"Method not allowed","type":"invalid_request_error"}})");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
        evhttp_send_reply(req, 405, "Method Not Allowed", buf);
        evbuffer_free(buf);
        return;
    }

    // Return generic model entry - actual model is determined by the miner's loaded model.
    // The miner auto-discovers .gguf files from its models/ directory.
    std::string response = "{\n"
        "  \"object\": \"list\",\n"
        "  \"data\": [\n"
        "    {\n"
        "      \"id\": \"tknc-miner-model\",\n"
        "      \"object\": \"model\",\n"
        "      \"created\": 1717772800,\n"
        "      \"owned_by\": \"tknc-miner\",\n"
        "      \"permission\": [\n"
        "        {\n"
        "          \"id\": \"model-readonly\",\n"
        "          \"object\": \"model_permission\",\n"
        "          \"created\": 1717772800,\n"
        "          \"allow_create_engine\": false,\n"
        "          \"sampling\": {\n"
        "            \"temperature\": true,\n"
        "            \"top_p\": true,\n"
        "            \"max_tokens\": true\n"
        "          }\n"
        "        }\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n";

    struct evbuffer* buf = evbuffer_new();
    evbuffer_add(buf, response.c_str(), response.size());
    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
    AddCorsHeader(evhttp_request_get_output_headers(req));
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

/** Static callback wrapper for list models */
static void EvHttpListModelsCb(struct evhttp_request* req, void*) {
    HandleListModels(req);
}

// Handshake endpoint — /v1/chat/handshake: pre-inference verification for client.

/** Handle POST /v1/chat/handshake — pre-inference handshake verification */
static void HandleHandshake(struct evhttp_request* req) {
    if (!req) return;

    enum evhttp_cmd_type cmd = evhttp_request_get_command(req);

    // Handle CORS preflight
    if (cmd == EVHTTP_REQ_OPTIONS) {
        struct evbuffer* buf = evbuffer_new();
        struct evkeyvalq* out_hdrs = evhttp_request_get_output_headers(req);
        AddCorsHeader(out_hdrs);
        evhttp_add_header(out_hdrs, "Access-Control-Allow-Methods", "POST, OPTIONS");
        evhttp_add_header(out_hdrs, "Access-Control-Allow-Headers", "Content-Type, Authorization");
        evhttp_add_header(out_hdrs, "Access-Control-Max-Age", "86400");
        evhttp_send_reply(req, 200, "OK", buf);
        evbuffer_free(buf);
        return;
    }

    if (cmd != EVHTTP_REQ_POST) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, R"({"error":{"message":"Method not allowed","type":"invalid_request_error"}})");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
        evhttp_send_reply(req, 405, "Method Not Allowed", buf);
        evbuffer_free(buf);
        return;
    }

    std::string body = ReadEvHttpBody(req);
    if (body.empty()) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, R"({"error":{"message":"Empty request body","type":"invalid_request_error"}})");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }

    SimpleJsonParser json(body);

    // Extract API key
    auto auth_header = GetEvHttpHeader(req, "Authorization");
    std::string api_key;
    if (auth_header.first) {
        const std::string& auth_val = auth_header.second;
        if (auth_val.size() > 7 && auth_val.substr(0, 7) == "Bearer ") {
            api_key = auth_val.substr(7);
            while (!api_key.empty() && api_key[0] == ' ')
                api_key.erase(api_key.begin());
        }
    }
    if (api_key.empty() && json.hasKey("api_key")) {
        api_key = json.getString("api_key");
    }

    if (api_key.empty()) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, R"({"error":{"message":"Missing API key","type":"authentication_error"}})");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
        evhttp_send_reply(req, 401, "Unauthorized", buf);
        evbuffer_free(buf);
        return;
    }

    // Execute micro-inference for token counting verification
    std::string test_prompt = "Hi";
    std::string test_body = "{\n";
    test_body += "  \"api_key\": \"" + api_key + "\",\n";
    test_body += "  \"messages\": [{\"role\": \"user\", \"content\": \"" + JsonEscape(test_prompt) + "\"}],\n";
    test_body += "  \"stream\": false\n";
    test_body += "}";

    LogInfo("[HANDSHAKE-ENDPOINT] Executing micro-inference for api_key=%s...", api_key.substr(0, 8).c_str());
    // Use 30s timeout instead of 86400s default — micro-inference ("Hi") should complete in seconds.
    // The 86400s default would block the gateway event loop if the miner is unresponsive.
    std::string test_response = HttpPostToMiner(MINER_LOCAL_HOST, MINER_LOCAL_PORT,
                                                 "/api/v1/chat", test_body, 30);

    int miner_prompt_tokens = 0;
    int miner_completion_tokens = 0;
    std::string test_content;
    if (!test_response.empty()) {
        // Parse raw prompt_tokens and completion_tokens — node computes total independently
        size_t pt_pos = test_response.find("\"prompt_tokens\"");
        if (pt_pos != std::string::npos) {
            size_t colon = test_response.find(':', pt_pos);
            if (colon != std::string::npos) {
                miner_prompt_tokens = std::atoi(test_response.c_str() + colon + 1);
            }
        }
        size_t ct_pos = test_response.find("\"completion_tokens\"");
        if (ct_pos != std::string::npos) {
            size_t colon = test_response.find(':', ct_pos);
            if (colon != std::string::npos) {
                miner_completion_tokens = std::atoi(test_response.c_str() + colon + 1);
            }
        }
        size_t content_pos = test_response.find("\"response\"");
        if (content_pos != std::string::npos) {
            size_t q1 = test_response.find('"', content_pos + 11);
            size_t q2 = test_response.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                test_content = test_response.substr(q1 + 1, q2 - q1 - 1);
            }
        }
    }
    // Node independently computes total token count
    int miner_token_count = miner_prompt_tokens + miner_completion_tokens;

    // Token count sanity verification
    bool token_count_sane = true;
    std::string token_verdict = "verified";
    if (miner_token_count > 500) {
        token_count_sane = false;
        token_verdict = "anomaly_detected";
    } else if (miner_token_count > 0) {
        int content_len = static_cast<int>(test_content.length());
        if (content_len > 0) {
            double chars_per_token = static_cast<double>(content_len) / miner_token_count;
            if (chars_per_token > 20.0 || chars_per_token < 0.1) {
                token_count_sane = false;
                token_verdict = "anomaly_detected";
            }
        }
    }

// Get this node's miner wallet address for binding verification
std::string local_miner_wallet = GetMinerWalletAddress();

// Get verified token rate — use the miner's SPECIFIC wallet, not empty string.
// GetTokensPerTknc("") returns a random first entry from the map, which may be stale.
int64_t tokens_per_tknc = GetTokensPerTknc(local_miner_wallet);
// No clamping — allow any positive rate (e.g. -token=10 means 10 tokens per TKNC)

    // Build handshake response for client display
    std::ostringstream resp;
    resp << "{\n";
    resp << "  \"handshake\": {\n";
    resp << "    \"token_verification\": \"" << token_verdict << "\",\n";
    resp << "    \"node_prompt_tokens\": " << miner_prompt_tokens << ",\n";
    resp << "    \"node_completion_tokens\": " << miner_completion_tokens << ",\n";
    resp << "    \"node_total_tokens\": " << miner_token_count << ",\n";
    resp << "    \"test_content_length\": " << test_content.length() << ",\n";
    resp << "    \"token_count_sane\": " << (token_count_sane ? "true" : "false") << ",\n";
resp << "    \"tokens_per_tknc\": " << tokens_per_tknc << ",\n";
resp << "    \"exchange_rate_display\": \"1 TKNC = " << tokens_per_tknc << " tokens\",\n";
resp << "    \"miner_wallet\": \"" << local_miner_wallet << "\",\n";
    resp << "    \"can_proceed\": " << (token_count_sane ? "true" : "false") << "\n";
    resp << "  },\n";
    resp << "  \"message\": \"" << (token_count_sane ?
        "Token verification passed. Please confirm to start inference at 1 TKNC = " + std::to_string(tokens_per_tknc) + " tokens." :
        "Token verification FAILED. Possible cheating detected. Connection refused.") << "\"\n";
    resp << "}\n";

    std::string response_str = resp.str();

    struct evbuffer* buf = evbuffer_new();
    evbuffer_add(buf, response_str.c_str(), response_str.size());
    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
    AddCorsHeader(evhttp_request_get_output_headers(req));

    if (token_count_sane) {
        evhttp_send_reply(req, 200, "OK", buf);
    } else {
        evhttp_send_reply(req, 403, "Handshake Failed", buf);
    }
    evbuffer_free(buf);
}

/** Static callback wrapper for handshake */
static void EvHttpHandshakeCb(struct evhttp_request* req, void*) {
    HandleHandshake(req);
}

static void EvHttpGenericV1Cb(struct evhttp_request* req, void*) {
    if (!req) return;
    enum evhttp_cmd_type cmd = evhttp_request_get_command(req);
    // Handle CORS preflight for any /v1/* path
    if (cmd == EVHTTP_REQ_OPTIONS) {
        struct evbuffer* buf = evbuffer_new();
        struct evkeyvalq* out_hdrs = evhttp_request_get_output_headers(req);
        AddCorsHeader(out_hdrs);
        evhttp_add_header(out_hdrs, "Access-Control-Allow-Methods", "POST, OPTIONS, GET");
        evhttp_add_header(out_hdrs, "Access-Control-Allow-Headers", "Content-Type, Authorization");
        evhttp_add_header(out_hdrs, "Access-Control-Max-Age", "86400");
        evhttp_send_reply(req, 200, "OK", buf);
        evbuffer_free(buf);
        return;
    }
    struct evbuffer* buf = evbuffer_new();
    evbuffer_add_printf(buf, R"({"error":{"message":"Not found","type":"not_found_error"}})");
    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
    evhttp_send_reply(req, 404, "Not Found", buf);
    evbuffer_free(buf);
}

// Create Key handler: forwards /api/v1/create_key to local miner via node's public port.
static void HandleCreateKey(struct evhttp_request* req) {
    if (!req) return;

    enum evhttp_cmd_type cmd = evhttp_request_get_command(req);
    if (cmd == EVHTTP_REQ_OPTIONS) {
        struct evbuffer* buf = evbuffer_new();
        struct evkeyvalq* out_hdrs = evhttp_request_get_output_headers(req);
        AddCorsHeader(out_hdrs);
        evhttp_add_header(out_hdrs, "Access-Control-Allow-Methods", "POST, OPTIONS");
        evhttp_add_header(out_hdrs, "Access-Control-Allow-Headers", "Content-Type, Authorization");
        evhttp_send_reply(req, 200, "OK", buf);
        evbuffer_free(buf);
        return;
    }

    if (cmd != EVHTTP_REQ_POST) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, R"({"error":{"message":"Method not allowed","type":"invalid_request_error"}})");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
        evhttp_send_reply(req, 405, "Method Not Allowed", buf);
        evbuffer_free(buf);
        return;
    }

    // SECURITY FIX: create_key requires admin auth (RPC password or cookie verification).
    auto auth_header = GetEvHttpHeader(req, "Authorization");
    bool authorized = false;
    if (auth_header.first) {
        const std::string& auth_val = auth_header.second;
        // Check for admin token: "Bearer admin:<rpcpassword>"
        if (auth_val.size() > 13 && auth_val.substr(0, 13) == "Bearer admin:") {
            std::string provided_pass = auth_val.substr(13);
            // Verify against the node's actual RPC password
            const std::string& rpc_pass = gArgs.GetArg("-rpcpassword", "");
            if (!rpc_pass.empty() && provided_pass == rpc_pass) {
                authorized = true;
            } else {
                // Cookie mode: read .cookie file for authentication
                // Cookie is at ExeDir/.cookie (same location tkncd writes to)
                fs::path cookie_arg = gArgs.GetPathArg("-rpccookiefile", ".cookie");
                fs::path cookie_path = cookie_arg.is_absolute() ? cookie_arg : fsbridge::AbsPathJoin(GetExeDir(), cookie_arg);
                std::ifstream cookie_file(cookie_path.utf8string());
                if (cookie_file.good()) {
                    std::string line;
                    std::getline(cookie_file, line);
                    size_t colon = line.find(':');
                    if (colon != std::string::npos) {
                        std::string cookie_pass = line.substr(colon + 1);
                        if (!cookie_pass.empty() && provided_pass == cookie_pass) {
                            authorized = true;
                        }
                    }
                }
            }
        }
    }

    if (!authorized) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, R"({"error":{"message":"Unauthorized. CreateKey requires admin authentication (Bearer admin:<rpcpassword>).","type":"authentication_error"}})");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
        evhttp_send_reply(req, 401, "Unauthorized", buf);
        evbuffer_free(buf);
        return;
    }

    std::string body = ReadEvHttpBody(req);
    if (body.empty()) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, R"({"error":{"message":"Empty request body","type":"invalid_request_error"}})");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }

    LogInfo("[InferenceGateway] CreateKey request body size: %zu bytes", body.size());

    // Forward to local miner's /api/v1/create_key endpoint
    // Use 30s timeout — key creation should be near-instant, not block the event loop for hours.
    std::string miner_response = HttpPostToMiner(MINER_LOCAL_HOST, MINER_LOCAL_PORT,
                                                 "/api/v1/create_key", body, 30);

    if (miner_response.empty()) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, R"({"error":{"message":"Miner unavailable","type":"server_error"}})");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
        evhttp_send_reply(req, 502, "Bad Gateway", buf);
        evbuffer_free(buf);
        return;
    }

    // Return miner's response directly
    struct evbuffer* buf = evbuffer_new();
    evbuffer_add(buf, miner_response.c_str(), miner_response.size());
    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
    AddCorsHeader(evhttp_request_get_output_headers(req));
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

static void EvHttpCreateKeyCb(struct evhttp_request* req, void*) {
    HandleCreateKey(req);
}

static void GatewayThreadFunc() {
    util::ThreadRename("api-gateway");
    LogInfo("[InferenceGateway] Entering API Gateway event loop (port %d)", DEFAULT_INFERENCE_PORT);
    event_base_dispatch(g_gateway_base);
    LogInfo("[InferenceGateway] Exited API Gateway event loop");
}

bool StartInferenceGateway(const std::any& context) {
    // Extract NodeContext* immediately from std::any (caller passes &node)
    // Store as raw pointer — NodeContext lives for entire process lifetime in AppInitMain
    g_node_ctx = std::any_cast<node::NodeContext*>(context);
    if (!g_node_ctx) {
        LogError("[InferenceGateway] Failed to extract NodeContext from context parameter");
        // Continue anyway — P2P fallback won't work but local inference will
    } else {
        LogInfo("[InferenceGateway] NodeContext captured for P2P routing");
    }

    // Determine port from config or default
    int gw_port = gArgs.GetIntArg("-apiport", DEFAULT_INFERENCE_PORT);

#ifdef WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        LogError("[InferenceGateway] WSAStartup failed");
        return false;
    }
#endif

    // Create standalone event base for gateway (separate from RPC server)
    g_gateway_base = event_base_new();
    if (!g_gateway_base) {
        LogError("[InferenceGateway] event_base_new failed");
        return false;
    }

    // Create standalone HTTP server for gateway
    g_gateway_http = evhttp_new(g_gateway_base);
    if (!g_gateway_http) {
        LogError("[InferenceGateway] evhttp_new failed");
        event_base_free(g_gateway_base);
        g_gateway_base = nullptr;
        return false;
    }

    evhttp_set_timeout(g_gateway_http, 300);
    // Allow OPTIONS method for CORS preflight requests from IDE/browser clients
    evhttp_set_allowed_methods(g_gateway_http,
        EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_HEAD | EVHTTP_REQ_OPTIONS);

    // Read bind address from config (default: all interfaces for remote access)
    std::string gw_bind = gArgs.GetArg("-gatewaybind", "0.0.0.0");
    bool bound = false;
    if (gw_bind == "0.0.0.0" || gw_bind == "::") {
        // Dual-stack binding when explicitly binding to all interfaces
        if (evhttp_bind_socket(g_gateway_http, "::", gw_port) == 0) {
            LogInfo("[InferenceGateway] Bound to [::]:%d (IPv6)", gw_port);
            bound = true;
        }
        if (evhttp_bind_socket(g_gateway_http, "0.0.0.0", gw_port) == 0) {
            LogInfo("[InferenceGateway] Bound to 0.0.0.0:%d (IPv4)", gw_port);
            bound = true;
        }
    } else {
        // Single address binding (default: localhost)
        if (evhttp_bind_socket(g_gateway_http, gw_bind.c_str(), gw_port) == 0) {
            LogInfo("[InferenceGateway] Bound to %s:%d", gw_bind.c_str(), gw_port);
            bound = true;
        }
    }
    if (!bound) {
        LogError("[InferenceGateway] Failed to bind to %s:%d", gw_bind.c_str(), gw_port);
        evhttp_free(g_gateway_http);
        event_base_free(g_gateway_base);
        g_gateway_http = nullptr;
        g_gateway_base = nullptr;
        return false;
    }

    // Register both /v1/* and /* paths for IDE base_url compatibility.
    evhttp_set_cb(g_gateway_http, "/v1/chat/completions", EvHttpChatCompletionsCb, nullptr);
    evhttp_set_cb(g_gateway_http, "/chat/completions", EvHttpChatCompletionsCb, nullptr);
    evhttp_set_cb(g_gateway_http, "/v1/chat/handshake", EvHttpHandshakeCb, nullptr);
    evhttp_set_cb(g_gateway_http, "/v1/models", EvHttpListModelsCb, nullptr);
    evhttp_set_cb(g_gateway_http, "/models", EvHttpListModelsCb, nullptr);
    // Create Key endpoint — allows Web server to create API keys via node's public port
    evhttp_set_cb(g_gateway_http, "/api/v1/create_key", EvHttpCreateKeyCb, nullptr);

    // Set generic handler for all other /v1/ paths (CORS + 404)
    evhttp_set_gencb(g_gateway_http, EvHttpGenericV1Cb, nullptr);

    g_gateway_running = true;

    // Start event loop in dedicated thread
    g_gateway_thread = std::thread(GatewayThreadFunc);

    LogInfo("[InferenceGateway] OpenAI-compatible HTTP API Gateway started on port %d", gw_port);
    LogInfo("[InferenceGateway] Registered endpoints:");
    LogInfo("[InferenceGateway]   POST :%d/v1/chat/completions  (OpenAI-compatible chat completion)", gw_port);
    LogInfo("[InferenceGateway]   POST :%d/v1/chat/handshake    (Pre-inference handshake verification)", gw_port);
    LogInfo("[InferenceGateway]   GET  :%d/v1/models            (List available models)", gw_port);
    LogInfo("[InferenceGateway]   All /v1/*                  (CORS preflight support)");

    return true;
}

void InterruptInferenceGateway() {
    if (g_gateway_running && g_gateway_base) {
        LogInfo("[InferenceGateway] Interrupting...");
        // Break the event loop
        event_base_loopbreak(g_gateway_base);
    }
}

void StopInferenceGateway() {
    if (!g_gateway_running) return;

    g_gateway_running = false;

    // Wait for thread to finish
    if (g_gateway_thread.joinable()) {
        g_gateway_thread.join();
    }

    // Cleanup
    if (g_gateway_http) {
        evhttp_free(g_gateway_http);
        g_gateway_http = nullptr;
    }
    if (g_gateway_base) {
        event_base_free(g_gateway_base);
        g_gateway_base = nullptr;
    }

    LogInfo("[InferenceGateway] Stopped.");
}

// Local IPv6→IPv4 Proxy: IDE → 127.0.0.1:9393 → [remote-IPv6]:9313 (transparent, no auth/billing).
// Note: g_proxy_target_ip, g_proxy_target_port, g_proxy_target_set, g_proxy_target_mutex
// are defined at the top of this file (needed by HandleChatCompletions for remote forwarding).

static int g_proxy_listen_port = 0;
static struct event_base* g_proxy_base = nullptr;
static struct evhttp* g_proxy_http = nullptr;
static std::thread g_proxy_thread;
static std::atomic<bool> g_proxy_running{false};

struct ProxyCtx {
    struct evhttp_request* client_req;
    struct evhttp_connection* client_conn;
    struct bufferevent* remote_bev;
    std::string forward_request;

    enum RespState { RESP_HEADERS, RESP_BODY };
    RespState resp_state = RESP_HEADERS;
    std::string header_buf;
    bool response_started = false;
    bool finished = false;
    bool client_disconnected = false;
    int resp_status = 200;
    bool is_chunked = false;

    bool chunk_read_size = true;
    bool chunk_expect_crlf = false; // chunk data consumed, waiting for trailing \r\n
    std::string chunk_line;
    size_t chunk_remaining = 0;

    // === Billing fields: proxy-side billing for remote inference ===
    std::string api_key;              // API key extracted from request for billing
    std::string prompt;               // Prompt extracted from request for token estimation
    int node_output_token_count = 0;  // Independent count of output tokens (SSE chunks)
    std::string response_body_accumulated; // Accumulated SSE response body for token counting
    CAmount reserved_cost = 0;        // Reserved cost for concurrent flood prevention
};

// Forward declaration — ProxyFinishCleanup is defined later but needed by ProxyClientCloseCb
static void ProxyFinishCleanup(ProxyCtx* ctx);

static void ProxyClientCloseCb(struct evhttp_connection* conn, void* arg) {
    ProxyCtx* ctx = static_cast<ProxyCtx*>(arg);
    if (!ctx) return;

    // CRITICAL: Check if cleanup already in progress
    if (ctx->finished) {
        LogInfo("[InferProxy] Client disconnected, but cleanup already in progress");
        return;
    }

    LogInfo("[InferProxy] Client disconnected, cleaning up");
    ctx->client_disconnected = true;

    // Don't directly free remote_bev — let ProxyFinishCleanup handle it.
    ProxyFinishCleanup(ctx);
}

static void ProxyFinishCleanup(ProxyCtx* ctx) {
    if (!ctx || ctx->finished) return;
    ctx->finished = true;

    // === BILLING: Count tokens from accumulated SSE response and deduct from escrow ===
    // The proxy is the billing point for remote inference. The remote miner's gateway
    // cannot do billing because the user's wallet is on this (client) node.
    // We count output tokens by counting SSE chunks with non-empty "content" field,
    // same logic as the inference gateway's StreamForwardToClient.
    // Skip billing if the remote returned an error (4xx/5xx) — no actual inference happened.
    if (!ctx->api_key.empty() && !ctx->response_body_accumulated.empty()
        && ctx->resp_status >= 200 && ctx->resp_status < 300) {
        // Count output tokens from SSE response body
        std::string& str = ctx->response_body_accumulated;
        size_t pos = 0;
        while ((pos = str.find("\"content\":\"", pos)) != std::string::npos) {
            size_t content_start = pos + 11;  // length of "content":""
            if (content_start < str.size() && str[content_start] != '"') {
                // Non-empty content = 1 output token
                ctx->node_output_token_count++;
            }
            pos = content_start;
        }

        // Estimate prompt tokens (approx 4 chars per token)
        int prompt_tokens = static_cast<int>(ctx->prompt.length() / 4);
        int tokens_used = prompt_tokens + ctx->node_output_token_count;
        if (tokens_used < 1) tokens_used = 1;

        // SECURITY: Do NOT bill if the miner returned an error/empty response.
        // The response body contains "[Miner returned empty response]" or "[Error:"
        // when the miner failed to produce actual inference output.
        // Billing for failed inference would drain the user's wallet without providing service.
        bool miner_error = (str.find("[Miner returned") != std::string::npos
                           || str.find("[Error:") != std::string::npos
                           || str.find("[Inference unavailable:") != std::string::npos
                           || str.find("[LLM inference error:") != std::string::npos
                           || str.find("[FATAL:") != std::string::npos
                           || ctx->node_output_token_count == 0);

        if (tokens_used > 0 && !miner_error) {
            BillingReceipt receipt;
            if (CheckAndDeductEscrow(ctx->api_key, tokens_used, receipt)) {
                LogInfo("[InferProxy-Billing] SUCCESS: api_key=%s..., output_tokens=%d, prompt_tokens=%d, "
                        "total=%d, cost=%s TKNC, consumed=%s TKNC, remaining=%s TKNC",
                        ctx->api_key.substr(0, 8).c_str(), ctx->node_output_token_count,
                        prompt_tokens, tokens_used,
                        FormatMoney(receipt.cost_tknc).c_str(),
                        FormatMoney(receipt.consumed_tknc).c_str(),
                        FormatMoney(receipt.remaining_limit).c_str());
            } else {
                LogWarning("[InferProxy-Billing] FAILED: api_key=%s..., tokens=%d — "
                           "escrow not found/exhausted/expired/invalid",
                           ctx->api_key.substr(0, 8).c_str(), tokens_used);
            }
        } else if (miner_error) {
            LogWarning("[InferProxy-Billing] SKIPPED billing: miner returned error/empty response "
                       "(api_key=%s..., output_tokens=%d, resp_status=%d)",
                       ctx->api_key.substr(0, 8).c_str(), ctx->node_output_token_count, ctx->resp_status);
        }
    }

    // Release the reserved cost (prevents concurrent flood bypass)
    if (ctx->reserved_cost > 0 && !ctx->api_key.empty()) {
        ReleaseEscrowCost(ctx->api_key, ctx->reserved_cost);
        ctx->reserved_cost = 0;
    }

    // CRITICAL: Remove close callback BEFORE deleting to prevent use-after-free
    if (ctx->client_conn) {
        evhttp_connection_set_closecb(ctx->client_conn, nullptr, nullptr);
        ctx->client_conn = nullptr;
    }

    if (ctx->remote_bev) {
        bufferevent_free(ctx->remote_bev);
        ctx->remote_bev = nullptr;
    }

    if (!ctx->client_disconnected && ctx->response_started) {
        evhttp_send_reply_end(ctx->client_req);
    }

    delete ctx;
}

static void ProxyForwardBody(ProxyCtx* ctx, const char* data, size_t len) {
    if (!ctx || ctx->finished || ctx->client_disconnected || len == 0) return;

    if (!ctx->is_chunked) {
        // === Accumulate SSE response body for billing token counting ===
        ctx->response_body_accumulated.append(data, len);

        struct evbuffer* buf = evbuffer_new();
        evbuffer_add(buf, data, len);
        evhttp_send_reply_chunk(ctx->client_req, buf);
        evbuffer_free(buf);
        return;
    }

    for (size_t i = 0; i < len && !ctx->finished; i++) {
        if (ctx->chunk_expect_crlf) {
            if (data[i] == '\r') {
                if (i + 1 < len && data[i + 1] == '\n') {
                    i++;
                }
                ctx->chunk_expect_crlf = false;
                ctx->chunk_read_size = true;
                ctx->chunk_line.clear();
            } else if (data[i] == '\n') {
                ctx->chunk_expect_crlf = false;
                ctx->chunk_read_size = true;
                ctx->chunk_line.clear();
            }
            continue;
        }
        if (ctx->chunk_read_size) {
            ctx->chunk_line += data[i];
            if (ctx->chunk_line.size() >= 2 &&
                ctx->chunk_line[ctx->chunk_line.size() - 2] == '\r' &&
                ctx->chunk_line[ctx->chunk_line.size() - 1] == '\n') {
                ctx->chunk_remaining = strtoul(ctx->chunk_line.c_str(), nullptr, 16);
                ctx->chunk_line.clear();
                ctx->chunk_read_size = false;

                if (ctx->chunk_remaining == 0) {
                    ProxyFinishCleanup(ctx);
                    return;
                }
            }
        } else {
            size_t avail = len - i;
            size_t to_read = (avail < ctx->chunk_remaining) ? avail : ctx->chunk_remaining;

            // === Accumulate SSE response body for billing token counting ===
            ctx->response_body_accumulated.append(data + i, to_read);

            struct evbuffer* buf = evbuffer_new();
            evbuffer_add(buf, data + i, to_read);
            evhttp_send_reply_chunk(ctx->client_req, buf);
            evbuffer_free(buf);

            i += to_read - 1;
            ctx->chunk_remaining -= to_read;

            if (ctx->chunk_remaining == 0) {
                ctx->chunk_expect_crlf = true;
            }
        }
    }
}

static void ProxyProcessData(ProxyCtx* ctx, const char* data, size_t len) {
    if (ctx->resp_state == ProxyCtx::RESP_HEADERS) {
        for (size_t i = 0; i < len; i++) {
            ctx->header_buf += data[i];

            if (ctx->header_buf.size() >= 4) {
                size_t sz = ctx->header_buf.size();
                if (ctx->header_buf[sz - 4] == '\r' && ctx->header_buf[sz - 3] == '\n' &&
                    ctx->header_buf[sz - 2] == '\r' && ctx->header_buf[sz - 1] == '\n') {

                    // Parse status code from first line: HTTP/1.1 200 OK
                    size_t sp1 = ctx->header_buf.find(' ');
                    if (sp1 != std::string::npos) {
                        size_t sp2 = ctx->header_buf.find(' ', sp1 + 1);
                        std::string status_str = ctx->header_buf.substr(
                            sp1 + 1, sp2 != std::string::npos ? sp2 - sp1 - 1 : std::string::npos);
                        ctx->resp_status = atoi(status_str.c_str());
                    }
                    LogInfo("[InferProxy-Resp] Remote status: %d, header_buf size=%zu, first 200 chars: %.200s",
                            ctx->resp_status, ctx->header_buf.size(), ctx->header_buf.c_str());

                    // Parse and forward response headers
                    // Skip headers that evhttp manages internally
                    struct evkeyvalq* out_hdrs = evhttp_request_get_output_headers(ctx->client_req);
                    size_t line_start = ctx->header_buf.find("\r\n") + 2; // skip status line
                    while (line_start < ctx->header_buf.size() - 2) {
                        size_t line_end = ctx->header_buf.find("\r\n", line_start);
                        if (line_end == std::string::npos || line_end == line_start) break;

                        std::string line = ctx->header_buf.substr(line_start, line_end - line_start);
                        size_t colon = line.find(':');
                        if (colon != std::string::npos) {
                            std::string key = line.substr(0, colon);
                            std::string val = line.substr(colon + 1);
                            while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(0, 1);

                            if (evutil_ascii_strcasecmp(key.c_str(), "Transfer-Encoding") == 0) {
                                if (val.find("chunked") != std::string::npos) ctx->is_chunked = true;
                            } else if (evutil_ascii_strcasecmp(key.c_str(), "Connection") != 0 &&
                                       evutil_ascii_strcasecmp(key.c_str(), "Content-Length") != 0) {
                                evhttp_add_header(out_hdrs, key.c_str(), val.c_str());
                            }
                        }
                        line_start = line_end + 2;
                    }

                    LogInfo("[InferProxy-Resp] is_chunked=%d, starting reply to client with status %d",
                            ctx->is_chunked ? 1 : 0, ctx->resp_status);
                    evhttp_send_reply_start(ctx->client_req, ctx->resp_status, "OK");
                    ctx->response_started = true;
                    ctx->resp_state = ProxyCtx::RESP_BODY;

                    if (i + 1 < len) {
                        ProxyForwardBody(ctx, data + i + 1, len - i - 1);
                    }
                    return;
                }
            }
        }
    } else {
        ProxyForwardBody(ctx, data, len);
    }
}

static void ProxyReadCb(struct bufferevent* bev, void* arg) {
    ProxyCtx* ctx = static_cast<ProxyCtx*>(arg);
    if (!ctx || ctx->finished) return;

    struct evbuffer* input = bufferevent_get_input(bev);
    size_t total = evbuffer_get_length(input);

    while (total > 0 && !ctx->finished) {
        char buf[16384];
        size_t n = (total < sizeof(buf)) ? total : sizeof(buf);
        evbuffer_remove(input, buf, n);
        ProxyProcessData(ctx, buf, n);
        total = evbuffer_get_length(input);
    }
}

static void ProxyEventCb(struct bufferevent* bev, short what, void* arg) {
    ProxyCtx* ctx = static_cast<ProxyCtx*>(arg);
    if (!ctx) return;

    if (what & BEV_EVENT_CONNECTED) {
        LogInfo("[InferProxy] Connected to remote, sending request (%zu bytes)",
                ctx->forward_request.size());
        bufferevent_write(bev, ctx->forward_request.c_str(),
                         ctx->forward_request.size());
        return;
    }

    if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
        int errcode = EVUTIL_SOCKET_ERROR();
        const char* errstr = evutil_socket_error_to_string(errcode);
        LogInfo("[InferProxy] Remote event: 0x%x (EOF=%d ERR=%d TIMEOUT=%d), socket_err=%d (%s), response_started=%d, header_buf_size=%zu",
                what, (what & BEV_EVENT_EOF) != 0, (what & BEV_EVENT_ERROR) != 0,
                (what & BEV_EVENT_TIMEOUT) != 0, errcode, errstr,
                ctx->response_started ? 1 : 0, ctx->header_buf.size());

        if (!ctx->response_started) {
            LogError("[InferProxy] Returning 502 to client: remote unreachable before any response. header_buf='%.200s'",
                     ctx->header_buf.c_str());
            struct evbuffer* err_buf = evbuffer_new();
            evbuffer_add_printf(err_buf,
                "{\"error\":{\"message\":\"Remote server unreachable\",\"type\":\"proxy_error\"}}");
            evhttp_send_reply(ctx->client_req, 502, "Bad Gateway", err_buf);
            evbuffer_free(err_buf);
            ctx->response_started = true;
            ctx->finished = true;
            if (ctx->client_conn) {
                evhttp_connection_set_closecb(ctx->client_conn, nullptr, nullptr);
                ctx->client_conn = nullptr;
            }
            if (ctx->remote_bev) { bufferevent_free(ctx->remote_bev); ctx->remote_bev = nullptr; }
            delete ctx;
            return;
        }

        ProxyFinishCleanup(ctx);
    }
}

static void ProxyHandler(struct evhttp_request* req, void*) {
    const char* uri = evhttp_request_get_uri(req);
    enum evhttp_cmd_type cmd = evhttp_request_get_command(req);
    const char* method = (cmd == EVHTTP_REQ_POST) ? "POST" :
                         (cmd == EVHTTP_REQ_GET) ? "GET" :
                         (cmd == EVHTTP_REQ_OPTIONS) ? "OPTIONS" :
                         (cmd == EVHTTP_REQ_HEAD) ? "HEAD" : "GET";

    LogInfo("[InferProxy] %s %s", method, uri ? uri : "(null)");

    if (cmd == EVHTTP_REQ_OPTIONS) {
        struct evbuffer* buf = evbuffer_new();
        struct evkeyvalq* out = evhttp_request_get_output_headers(req);
        AddCorsHeader(out);
        evhttp_add_header(out, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        evhttp_add_header(out, "Access-Control-Allow-Headers", "Content-Type, Authorization");
        evhttp_send_reply(req, 200, "OK", buf);
        evbuffer_free(buf);
        return;
    }

    // Check if target has been set via RPC
    if (!g_proxy_target_set.load()) {
        struct evbuffer* err_buf = evbuffer_new();
        evbuffer_add_printf(err_buf,
            "{\"error\":{\"message\":\"Proxy target not configured. "
            "Run 'tknc-cli tknc_setinferproxytarget <miner_ip> <api_key> [model_name]' to set target.\","
            "\"type\":\"proxy_not_configured\"}}");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
        evhttp_send_reply(req, 503, "Service Unavailable", err_buf);
        evbuffer_free(err_buf);
        return;
    }

    // Read target IP/port under mutex
    std::string target_ip;
    int target_port;
    {
        std::lock_guard<std::mutex> lock(g_proxy_target_mutex);
        target_ip = g_proxy_target_ip;
        target_port = g_proxy_target_port;
    }

    std::string body = ReadEvHttpBody(req);

    // Build Host header: [IPv6]:port or IPv4:port
    std::string host_str;
    if (target_ip.find(':') != std::string::npos) {
        host_str = "[" + target_ip + "]:" + std::to_string(target_port);
    } else {
        host_str = target_ip + ":" + std::to_string(target_port);
    }

    std::ostringstream fwd;
    fwd << method << " " << (uri ? uri : "/") << " HTTP/1.1\r\n";
    fwd << "Host: " << host_str << "\r\n";

    struct evkeyvalq* in_hdrs = evhttp_request_get_input_headers(req);
    for (struct evkeyval* hdr = in_hdrs->tqh_first; hdr; hdr = hdr->next.tqe_next) {
        if (evutil_ascii_strcasecmp(hdr->key, "Host") == 0) continue;
        if (evutil_ascii_strcasecmp(hdr->key, "Accept-Encoding") == 0) continue;
        if (evutil_ascii_strcasecmp(hdr->key, "Connection") == 0) continue;
        if (evutil_ascii_strcasecmp(hdr->key, "Content-Length") == 0) continue;
        LogInfo("[InferProxy] FwdHeader: %s: %.100s", hdr->key, hdr->value);
        fwd << hdr->key << ": " << hdr->value << "\r\n";
    }

    if (!body.empty()) {
        fwd << "Content-Length: " << body.size() << "\r\n";
        LogInfo("[InferProxy] Body size: %zu bytes", body.size());
    }
    // Add X-TKNC-Skip-Billing header so the remote gateway does NOT try to do billing.
    // Billing is handled by THIS proxy (where the user's wallet is located).
    // Use deterministic API-key-based secret so the remote miner can verify it.
    {
        // Extract api_key from Authorization header for deterministic secret
        std::string proxy_api_key;
        auto auth_hdr = GetEvHttpHeader(req, "Authorization");
        if (auth_hdr.first) {
            const std::string& auth_val = auth_hdr.second;
            if (auth_val.size() > 7 && auth_val.substr(0, 7) == "Bearer ") {
                proxy_api_key = auth_val.substr(7);
                while (!proxy_api_key.empty() && proxy_api_key[0] == ' ')
                    proxy_api_key.erase(proxy_api_key.begin());
            }
        }
        std::string proxy_secret = ComputeSkipBillingSecret(proxy_api_key);
        if (!proxy_secret.empty()) {
            fwd << "X-TKNC-Skip-Billing: " << proxy_secret << "\r\n";
        }
        // Include payment txids so the miner can independently verify payment.
        if (!proxy_api_key.empty()) {
            auto esc = FindSpendingLimitByAPIKey(proxy_api_key);
            if (esc.has_value() && !esc->pending_txids.empty()) {
                fwd << "X-TKNC-Pending-Txids: " << esc->pending_txids << "\r\n";
            }
        }
    }
    fwd << "Connection: close\r\n";
    fwd << "\r\n";
    if (!body.empty()) {
        fwd << body;
    }

    ProxyCtx* ctx = new ProxyCtx;
    ctx->client_req = req;
    ctx->forward_request = fwd.str();
    ctx->client_conn = evhttp_request_get_connection(req);

    // === Extract api_key and prompt for billing ===
    // The proxy is the billing point for remote inference.
    // We extract the api_key from the Authorization header or request body,
    // and the prompt from the messages array, to enable post-inference billing.
    {
        // Try Authorization header first
        auto auth_hdr = GetEvHttpHeader(req, "Authorization");
        if (auth_hdr.first) {
            const std::string& auth_val = auth_hdr.second;
            if (auth_val.size() > 7 && auth_val.substr(0, 7) == "Bearer ") {
                ctx->api_key = auth_val.substr(7);
                while (!ctx->api_key.empty() && ctx->api_key[0] == ' ')
                    ctx->api_key.erase(ctx->api_key.begin());
            }
        }

        // Fallback: parse api_key from request body
        if (ctx->api_key.empty() && !body.empty()) {
            size_t ak_pos = body.find("\"api_key\"");
            if (ak_pos != std::string::npos) {
                size_t colon = body.find(':', ak_pos);
                if (colon != std::string::npos) {
                    size_t q1 = body.find('"', colon + 1);
                    if (q1 != std::string::npos) {
                        size_t q2 = body.find('"', q1 + 1);
                        if (q2 != std::string::npos) {
                            ctx->api_key = body.substr(q1 + 1, q2 - q1 - 1);
                        }
                    }
                }
            }
        }

        // Extract prompt from messages array (first "content" field)
        if (!body.empty()) {
            size_t content_pos = body.find("\"content\"");
            if (content_pos != std::string::npos) {
                size_t colon = body.find(':', content_pos);
                if (colon != std::string::npos) {
                    size_t q1 = body.find('"', colon + 1);
                    if (q1 != std::string::npos) {
                        size_t q2 = body.find('"', q1 + 1);
                        // Make sure we don't hit an escaped quote
                        while (q2 != std::string::npos && q2 > q1 && body[q2 - 1] == '\\') {
                            q2 = body.find('"', q2 + 1);
                        }
                        if (q2 != std::string::npos) {
                            ctx->prompt = body.substr(q1 + 1, q2 - q1 - 1);
                        }
                    }
                }
            }
        }

        if (!ctx->api_key.empty()) {
            // SECURITY: Validate API key format before any billing operations.
            if (!ValidateAPIKeyFormat(ctx->api_key)) {
                LogWarning("[InferProxy] Rejected invalid API key format (length=%zu)", ctx->api_key.size());
                struct evbuffer* err_buf = evbuffer_new();
                evbuffer_add_printf(err_buf,
                    "{\"error\":{\"message\":\"Invalid API key format. API keys must start with 'tknc_' followed by 32 hex characters.\","
                    "\"type\":\"authentication_error\"}}");
                evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
                evhttp_send_reply(req, 401, "Unauthorized", err_buf);
                evbuffer_free(err_buf);
                delete ctx;
                return;
            }

            LogInfo("[InferProxy-Billing] Extracted api_key=%s... for billing, prompt_len=%zu",
                    ctx->api_key.substr(0, 8).c_str(), ctx->prompt.length());

            // Auto-create escrow if one doesn't exist for this API key.
            // This handles the case where the user got a new API key from the web
            // but didn't re-run tknc_setinferproxytarget.
            EnsureEscrowForAPIKey(ctx->api_key);

            // Pre-inference balance check: refuse if user wallet has 0 balance.
            // This prevents free inference — the proxy is the billing point.
            std::string balance_error;
            if (!CanAffordInference(ctx->api_key, balance_error)) {
                LogWarning("[InferProxy] Inference REFUSED: %s", balance_error.c_str());
                struct evbuffer* err_buf = evbuffer_new();
                evbuffer_add_printf(err_buf,
                    "{\"error\":{\"message\":\"Inference refused: %s\","
                    "\"type\":\"insufficient_balance\"}}",
                    balance_error.c_str());
                evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
                evhttp_send_reply(req, 402, "Payment Required", err_buf);
                evbuffer_free(err_buf);
                delete ctx;
                return;
            }

            // Reserve estimated cost to prevent concurrent request flood bypass.
            int64_t estimated_tokens = 100 + (int64_t)(ctx->prompt.length() / 4);
            // Use this node's miner wallet to look up the specific rate — NOT empty string.
            std::string local_mw = GetMinerWalletAddress();
            int64_t tokens_per_tknc = GetTokensPerTknc(local_mw);
            // If no rate found for local miner, try escrow's rate
            if (tokens_per_tknc <= 0 && !ctx->api_key.empty()) {
                auto esc = FindSpendingLimitByAPIKey(ctx->api_key);
                if (esc.has_value() && esc->rate_tokens_per_tknc > 0) {
                    tokens_per_tknc = esc->rate_tokens_per_tknc;
                }
            }
            CAmount rate_tknc_per_token = (tokens_per_tknc > 0) ? (CAmount)(COIN / tokens_per_tknc) : 1;
            if (rate_tknc_per_token <= 0) rate_tknc_per_token = 1;
            ctx->reserved_cost = estimated_tokens * rate_tknc_per_token;
            if (ctx->reserved_cost < COIN) ctx->reserved_cost = COIN;
            ReserveEscrowCost(ctx->api_key, ctx->reserved_cost);
            LogInfo("[InferProxy] Reserved %s TKNC for api_key=%s... (estimated_tokens=%lld)",
                    FormatMoney(ctx->reserved_cost).c_str(), ctx->api_key.substr(0, 8).c_str(),
                    (long long)estimated_tokens);
        }
    }

    // (X-TKNC-Skip-Billing header is added above, before the forward request is finalized)

    if (ctx->client_conn) {
        evhttp_connection_set_closecb(ctx->client_conn, ProxyClientCloseCb, ctx);
    }

    ctx->remote_bev = bufferevent_socket_new(g_proxy_base, -1, BEV_OPT_CLOSE_ON_FREE);
    if (!ctx->remote_bev) {
        LogError("[InferProxy] bufferevent_socket_new failed");
        struct evbuffer* err_buf = evbuffer_new();
        evbuffer_add_printf(err_buf,
            "{\"error\":{\"message\":\"Internal proxy error\",\"type\":\"proxy_error\"}}");
        evhttp_send_reply(req, 500, "Internal Server Error", err_buf);
        evbuffer_free(err_buf);
        // Safe delete: remove close callback before deleting
        if (ctx->client_conn) {
            evhttp_connection_set_closecb(ctx->client_conn, nullptr, nullptr);
            ctx->client_conn = nullptr;
        }
        delete ctx;
        return;
    }

    bufferevent_setcb(ctx->remote_bev, ProxyReadCb, nullptr, ProxyEventCb, ctx);
    bufferevent_enable(ctx->remote_bev, EV_READ | EV_WRITE);

    struct timeval tv = {120, 0};
    bufferevent_set_timeouts(ctx->remote_bev, &tv, &tv);

    // Support both IPv4 and IPv6 targets
    bool is_ipv6 = (target_ip.find(':') != std::string::npos);
    int connect_ret;

    if (is_ipv6) {
        struct sockaddr_in6 remote_addr;
        memset(&remote_addr, 0, sizeof(remote_addr));
        remote_addr.sin6_family = AF_INET6;
        remote_addr.sin6_port = htons(static_cast<uint16_t>(target_port));
        inet_pton(AF_INET6, target_ip.c_str(), &remote_addr.sin6_addr);
        connect_ret = bufferevent_socket_connect(ctx->remote_bev,
            reinterpret_cast<struct sockaddr*>(&remote_addr), sizeof(remote_addr));
    } else {
        struct sockaddr_in remote_addr;
        memset(&remote_addr, 0, sizeof(remote_addr));
        remote_addr.sin_family = AF_INET;
        remote_addr.sin_port = htons(static_cast<uint16_t>(target_port));
        inet_pton(AF_INET, target_ip.c_str(), &remote_addr.sin_addr);
        connect_ret = bufferevent_socket_connect(ctx->remote_bev,
            reinterpret_cast<struct sockaddr*>(&remote_addr), sizeof(remote_addr));
    }

    int ret = connect_ret;

    if (ret < 0) {
        LogError("[InferProxy] bufferevent_socket_connect failed");
        struct evbuffer* err_buf = evbuffer_new();
        evbuffer_add_printf(err_buf,
            "{\"error\":{\"message\":\"Cannot connect to remote server\",\"type\":\"proxy_error\"}}");
        evhttp_send_reply(req, 502, "Bad Gateway", err_buf);
        evbuffer_free(err_buf);
        bufferevent_free(ctx->remote_bev);
        // Safe delete: remove close callback before deleting
        if (ctx->client_conn) {
            evhttp_connection_set_closecb(ctx->client_conn, nullptr, nullptr);
            ctx->client_conn = nullptr;
        }
        delete ctx;
        return;
    }

    LogInfo("[InferProxy] Connecting to %s:%d for %s %s",
            is_ipv6 ? ("[" + target_ip + "]").c_str() : target_ip.c_str(),
            target_port, method, uri ? uri : "/");
}

static void ProxyThreadFunc() {
    event_base_dispatch(g_proxy_base);
}

bool StartInferProxy(int listen_port) {
    g_proxy_listen_port = listen_port;

    g_proxy_base = event_base_new();
    if (!g_proxy_base) {
        LogError("[InferProxy] event_base_new failed");
        return false;
    }

    g_proxy_http = evhttp_new(g_proxy_base);
    if (!g_proxy_http) {
        LogError("[InferProxy] evhttp_new failed");
        event_base_free(g_proxy_base);
        g_proxy_base = nullptr;
        return false;
    }

    evhttp_set_timeout(g_proxy_http, 300);
    evhttp_set_allowed_methods(g_proxy_http,
        EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_HEAD | EVHTTP_REQ_OPTIONS);
    evhttp_set_max_body_size(g_proxy_http, 100 * 1024 * 1024);
    evhttp_set_max_headers_size(g_proxy_http, 100 * 1024 * 1024);

    if (evhttp_bind_socket(g_proxy_http, "127.0.0.1", listen_port) != 0) {
        LogError("[InferProxy] Failed to bind to 127.0.0.1:%d", listen_port);
        evhttp_free(g_proxy_http);
        event_base_free(g_proxy_base);
        g_proxy_http = nullptr;
        g_proxy_base = nullptr;
        return false;
    }

    LogInfo("[InferProxy] Bound to 127.0.0.1:%d", listen_port);

    evhttp_set_gencb(g_proxy_http, ProxyHandler, nullptr);

    g_proxy_running = true;
    g_proxy_thread = std::thread(ProxyThreadFunc);

    if (g_proxy_target_set.load()) {
        std::lock_guard<std::mutex> lock(g_proxy_target_mutex);
        LogInfo("[InferProxy] Local proxy started: http://127.0.0.1:%d → http://%s:%d",
                listen_port,
                g_proxy_target_ip.find(':') != std::string::npos
                    ? ("[" + g_proxy_target_ip + "]").c_str()
                    : g_proxy_target_ip.c_str(),
                g_proxy_target_port);
    } else {
        LogInfo("[InferProxy] Local proxy started on http://127.0.0.1:%d (target not yet configured)",
                listen_port);
        LogInfo("[InferProxy] Use tknc_setinferproxytarget RPC to configure target miner IP");
    }
    LogInfo("[InferProxy] Configure IDE with: http://127.0.0.1:%d/v1", listen_port);

    return true;
}

void StopInferProxy() {
    if (!g_proxy_running) return;
    g_proxy_running = false;

    if (g_proxy_base) {
        event_base_loopbreak(g_proxy_base);
    }

    if (g_proxy_thread.joinable()) {
        g_proxy_thread.join();
    }

    if (g_proxy_http) {
        evhttp_free(g_proxy_http);
        g_proxy_http = nullptr;
    }
    if (g_proxy_base) {
        event_base_free(g_proxy_base);
        g_proxy_base = nullptr;
    }

    LogInfo("[InferProxy] Stopped.");
}

struct InferProxyConfig GetInferProxyConfig() {
    InferProxyConfig cfg;
    cfg.running = g_proxy_running.load();
    cfg.listen_port = g_proxy_listen_port;
    {
        std::lock_guard<std::mutex> lock(g_proxy_target_mutex);
        cfg.target_ip = g_proxy_target_ip;
        cfg.target_port = g_proxy_target_port;
    }
    cfg.target_set = g_proxy_target_set.load();
    return cfg;
}

bool SetInferProxyTarget(const std::string& target_ip, int target_port) {
    if (target_ip.empty()) {
        LogError("[InferProxy] SetInferProxyTarget: empty IP");
        return false;
    }
    if (target_port <= 0 || target_port > 65535) {
        LogError("[InferProxy] SetInferProxyTarget: invalid port %d", target_port);
        return false;
    }

    // Validate IP format (IPv4 or IPv6)
    bool is_ipv6 = (target_ip.find(':') != std::string::npos);
    if (is_ipv6) {
        struct in6_addr addr6;
        if (inet_pton(AF_INET6, target_ip.c_str(), &addr6) != 1) {
            LogError("[InferProxy] SetInferProxyTarget: invalid IPv6 address: %s", target_ip.c_str());
            return false;
        }
    } else {
        struct in_addr addr4;
        if (inet_pton(AF_INET, target_ip.c_str(), &addr4) != 1) {
            LogError("[InferProxy] SetInferProxyTarget: invalid IPv4 address: %s", target_ip.c_str());
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_proxy_target_mutex);
        g_proxy_target_ip = target_ip;
        g_proxy_target_port = target_port;
    }
    g_proxy_target_set.store(true);

    LogInfo("[InferProxy] Target set to %s:%d",
            is_ipv6 ? ("[" + target_ip + "]").c_str() : target_ip.c_str(),
            target_port);
    LogInfo("[InferProxy] IDE can now use: http://127.0.0.1:%d/v1", g_proxy_listen_port);

    return true;
}
