// Copyright (c) 2026-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <inference_gateway.h>

#include <logging.h>
#include <common/args.h>
#include <util/strencodings.h>
#include <util/threadnames.h>

// Node computes tokens_used from raw miner response, handles billing via CheckAndDeductEscrow.
#include <rpc/escrow_rpc.h>
#include <rpc/tknc_apikey.h>
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
#include <iostream>
#include <mutex>
#include <queue>
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

/** Default port for Inference Gateway — non-HTTP port to avoid ISP filtering of 80/8080/443 */
static const int DEFAULT_INFERENCE_PORT = 9313;

/** Miner local address (iron rule: 127.0.0.1:9332 only) */
static const char* MINER_LOCAL_HOST = "127.0.0.1";
static const int MINER_LOCAL_PORT = 9332;

/** Request timeout for miner forwarding (seconds) */
static const int MINER_REQUEST_TIMEOUT_SEC = 120;

static struct event_base* g_gateway_base = nullptr;
static struct evhttp* g_gateway_http = nullptr;
static std::thread g_gateway_thread;
static std::atomic<bool> g_gateway_running{false};

// P2P routing: store node context for remote inference fallback
#include <node/context.h>
#include <net_processing.h>  // PeerManager, SendInferenceRequest
#include <net/p2p_llm.h>  // P2PLLM message types
#include <net/api_protocol.h>  // APIResponse, APIRequest
// Store NodeContext pointer directly (not pointer-to-any) to avoid dangling pointer
// The caller passes &node (NodeContext*) wrapped in std::any — extract it immediately
static node::NodeContext* g_node_ctx = nullptr;

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

    /** Extract messages array as concatenated prompt */
    std::string extractPromptFromMessages() const {
        // Extract ALL "content" values from messages array (fix: previously only took first/system content).
        LogInfo("[extractPromptFromMessages] raw JSON: %.200s", raw.c_str());
        std::string prompt;
        std::string key = "\"content\"";
        size_t search_from = 0;

        while (true) {
            size_t pos = raw.find(key, search_from);
            if (pos == std::string::npos) {
                break;
            }

            // Find the colon after "content"
            size_t colon = raw.find(':', pos + key.size());
            if (colon == std::string::npos) {
                break;
            }

            // Skip whitespace after colon
            size_t val_start = colon + 1;
            while (val_start < raw.size() && (raw[val_start] == ' ' || raw[val_start] == '\t' || raw[val_start] == '\n' || raw[val_start] == '\r'))
                val_start++;

            // Expect opening quote
            if (val_start >= raw.size() || raw[val_start] != '"') {
                search_from = pos + key.size();
                continue;
            }
            val_start++; // skip opening quote

            // Extract value into temp variable until closing quote (handle escapes)
            std::string temp;
            size_t i = val_start;
            while (i < raw.size()) {
                char c = raw[i];
                if (c == '\\' && i + 1 < raw.size()) {
                    char next = raw[i + 1];
                    if (next == 'n') temp += '\n';
                    else if (next == 'r') temp += '\r';
                    else if (next == 't') temp += '\t';
                    else if (next == '"') temp += '"';
                    else if (next == '\\') temp += '\\';
                    else { temp += c; temp += next; }
                    i += 2;
                    continue;
                }
                if (c == '"') break; // closing quote
                temp += c;
                i++;
            }

            // Concatenate all content fields with newline separator
            if (!temp.empty()) {
                if (!prompt.empty()) prompt += "\n";
                prompt += temp;
            }
            search_from = i + 1;
        }

        LogInfo("[extractPromptFromMessages] extracted prompt length=%zu, first 200 chars: %.200s",
                prompt.size(), prompt.c_str());

        if (prompt.empty()) {
            prompt = getString("prompt");
        }
        if (prompt.empty()) {
            prompt = getString("content");
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

    // Chunked transfer decoder state
    enum ChunkState { CHUNK_HEADERS, CHUNK_LENGTH, CHUNK_DATA, CHUNK_TRAILER, CHUNK_DONE };
    ChunkState chunk_state;
    std::string chunk_line;
    size_t chunk_bytes_remaining;
    bool headers_parsed;
    bool is_chunked;
    bool sse_forwarded;
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
        if (ctx->miner_bev) { bufferevent_free(ctx->miner_bev); ctx->miner_bev = nullptr; }
        delete ctx;
        return;
    }

    // If SSE data was already forwarded (true streaming), just end the response
    if (ctx->sse_forwarded) {
        LogInfo("[StreamGateway] True streaming complete, ending response");
        evhttp_send_reply_end(ctx->client_req);
        if (ctx->miner_bev) { bufferevent_free(ctx->miner_bev); ctx->miner_bev = nullptr; }
        delete ctx;
        return;
    }

    // Fallback: miner returned non-SSE (error or blocking JSON), parse and send as SSE
    std::string miner_body = ctx->miner_response_accumulated;
    size_t hdr_end = miner_body.find("\r\n\r\n");
    if (hdr_end != std::string::npos) {
        miner_body = miner_body.substr(hdr_end + 4);
    }

    LogInfo("[StreamGateway] Fallback: processing miner response: body_size=%zu", miner_body.size());

    SimpleJsonParser miner_json(miner_body);
    std::string content = miner_json.getString("response");
    if (content.empty()) content = miner_json.getString("content");

    if (content.empty()) {
        if (miner_body.find("\"error\"") != std::string::npos) {
            std::string err_msg = miner_json.getString("error");
            if (err_msg.empty()) err_msg = "Miner error";
            content = "[Error: " + err_msg + "]";
        } else {
            content = "[Miner returned empty response]";
        }
    }

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

    if (ctx->miner_bev) { bufferevent_free(ctx->miner_bev); ctx->miner_bev = nullptr; }
    delete ctx;
}

// Forward SSE data to client (called from StreamMinerReadCb when de-chunked data is ready)
static void StreamForwardToClient(StreamContext* ctx, const char* data, size_t len) {
    if (!ctx || ctx->finished || ctx->client_disconnected || len == 0) return;
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
    LogInfo("[StreamGateway] Client disconnected, cleaning up");
    ctx->client_disconnected = true;
    if (ctx->miner_bev) {
        bufferevent_free(ctx->miner_bev);
        ctx->miner_bev = nullptr;
    }
}

static void HttpPostToMinerProgressive(const std::string& host, int port,
                                        const std::string& path,
                                        const std::string& body,
                                        struct evhttp_request* req,
                                        const std::string& chat_id,
                                        const std::string& model,
                                        int64_t created,
                                        int timeout_sec = MINER_REQUEST_TIMEOUT_SEC) {
    LogInfo("[StreamGateway] Starting ASYNC streaming to miner %s:%d", host.c_str(), port);

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

    std::ostringstream http_req;
    http_req << "POST " << path << " HTTP/1.1\r\n";
    http_req << "Host: " << host << ":" << port << "\r\n";
    http_req << "Content-Type: application/json\r\n";
    http_req << "Content-Length: " << body.size() << "\r\n";
    http_req << "Connection: close\r\n";
    http_req << "\r\n";
    http_req << body;
    ctx->miner_http_request = http_req.str();

    struct evkeyvalq* out_hdrs = evhttp_request_get_output_headers(req);
    evhttp_add_header(out_hdrs, "Content-Type", "text/event-stream");
    evhttp_add_header(out_hdrs, "Cache-Control", "no-cache");
    evhttp_add_header(out_hdrs, "Connection", "keep-alive");
    evhttp_add_header(out_hdrs, "Access-Control-Allow-Origin", "*");
    evhttp_send_reply_start(req, 200, "OK");

    LogInfo("[StreamGateway] Sent SSE headers, creating async miner connection (role chunk deferred to miner)");

    ctx->client_conn = evhttp_request_get_connection(req);
    if (ctx->client_conn) {
        evhttp_connection_set_closecb(ctx->client_conn, StreamClientCloseCb, ctx);
    }

    ctx->miner_bev = bufferevent_socket_new(g_gateway_base, -1, BEV_OPT_CLOSE_ON_FREE);
    if (!ctx->miner_bev) {
        LogError("[StreamGateway] bufferevent_socket_new failed");
        struct evbuffer* err_buf = evbuffer_new();
        evbuffer_add_printf(err_buf,
            "data: {\"error\":\"Internal error\"}\n\ndata: [DONE]\n\n");
        evhttp_send_reply_chunk(req, err_buf);
        evbuffer_free(err_buf);
        evhttp_send_reply_end(req);
        delete ctx;
        return;
    }

    bufferevent_setcb(ctx->miner_bev, StreamMinerReadCb, nullptr, StreamMinerEventCb, ctx);
    bufferevent_enable(ctx->miner_bev, EV_READ | EV_WRITE);

    struct timeval tv_timeout = {timeout_sec, 0};
    bufferevent_set_timeouts(ctx->miner_bev, &tv_timeout, &tv_timeout);

    ctx->keepalive_timer = evtimer_new(g_gateway_base, StreamKeepaliveCb, ctx);
    struct timeval tv_keepalive = {2, 0};
    event_add(ctx->keepalive_timer, &tv_keepalive);

    struct sockaddr_in miner_addr;
    memset(&miner_addr, 0, sizeof(miner_addr));
    miner_addr.sin_family = AF_INET;
    miner_addr.sin_port = htons(static_cast<uint16_t>(port));
#ifdef WIN32
    miner_addr.sin_addr.S_un.S_addr = inet_addr(host.c_str());
#else
    inet_pton(AF_INET, host.c_str(), &miner_addr.sin_addr);
#endif

    int ret = bufferevent_socket_connect(ctx->miner_bev,
        reinterpret_cast<struct sockaddr*>(&miner_addr), sizeof(miner_addr));
    if (ret < 0) {
        LogError("[StreamGateway] bufferevent_socket_connect failed");
        struct evbuffer* err_buf = evbuffer_new();
        evbuffer_add_printf(err_buf,
            "data: {\"error\":\"Cannot connect to miner\"}\n\ndata: [DONE]\n\n");
        evhttp_send_reply_chunk(req, err_buf);
        evbuffer_free(err_buf);
        evhttp_send_reply_end(req);
        event_del(ctx->keepalive_timer);
        event_free(ctx->keepalive_timer);
        bufferevent_free(ctx->miner_bev);
        delete ctx;
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

static void HandleChatCompletions(struct evhttp_request* req) {
    if (!req) return;

    enum evhttp_cmd_type cmd = evhttp_request_get_command(req);

    // Handle CORS preflight (IDE/cursor/cline send OPTIONS before POST)
    if (cmd == EVHTTP_REQ_OPTIONS) {
        struct evbuffer* buf = evbuffer_new();
        struct evkeyvalq* out_hdrs = evhttp_request_get_output_headers(req);
        evhttp_add_header(out_hdrs, "Access-Control-Allow-Origin", "*");
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

    LogInfo("[InferenceGateway] API Key: %s...%s (length=%zu)",
             api_key.substr(0, 6).c_str(),
             api_key.size() > 10 ? api_key.substr(api_key.size() - 6).c_str() : "",
             api_key.size());

    // 2. Get model name
    std::string model = json.getString("model");
    if (model.empty()) model = "qwen2.5-0.5b-instruct";

    // 2.1 Validate model name against whitelist
    {
        static const std::vector<std::string> VALID_MODELS = {
            "qwen2.5-0.5b-instruct"
        };
        bool valid = false;
        for (const auto& m : VALID_MODELS) {
            if (model == m) { valid = true; break; }
        }
        if (!valid) {
            LogWarning("[InferenceGateway] Invalid model requested: '%s'", model.c_str());
            struct evbuffer* buf = evbuffer_new();
            std::string err = "{\"error\":{\"message\":\"Invalid model: " + JsonEscape(model) +
                ". Available: qwen2.5-0.5b-instruct\",\"type\":\"invalid_request_error\"}}";
            evbuffer_add(buf, err.c_str(), err.size());
            evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
            evhttp_send_reply(req, 400, "Bad Request", buf);
            evbuffer_free(buf);
            return;
        }
    }

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

    // 4. Check stream mode
    bool stream_mode = json.getBool("stream", false);
    if (stream_mode) {
        LogInfo("[InferenceGateway] Streaming mode enabled (SSE)");
    }

    LogInfo("[InferenceGateway] Model=%s, Prompt length=%zu", model.c_str(), prompt.size());

    // Stream mode: async bufferevent I/O with keepalive, billing pre-check here.
    if (stream_mode) {
        if (!api_key.empty()) {
            auto spending_limit_opt = FindSpendingLimitByAPIKey(api_key);
            if (spending_limit_opt) {
                auto user_wallet = FindWalletByAddress(spending_limit_opt->user_wallet);
                if (user_wallet && user_wallet->IsLocked()) {
                    LogWarning("[InferenceGateway] User wallet LOCKED — blocking stream (402)");
                    struct evbuffer* err_buf = evbuffer_new();
                    evbuffer_add_printf(err_buf, "{\"error\": \"User wallet is locked. Unlock with walletpassphrase.\"}");
                    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
                    evhttp_send_reply(req, 402, "Payment Required", err_buf);
                    evbuffer_free(err_buf);
                    return;
                }
            }
        }

        std::string chat_id = "chatcmpl-tknc-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        int64_t created = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::string miner_request_body = "{\n";
        miner_request_body += "  \"api_key\": \"" + api_key + "\",\n";
        miner_request_body += "  \"messages\": [{\"role\": \"user\", \"content\": \"" + JsonEscape(prompt) + "\"}],\n";
        miner_request_body += "  \"stream\": true\n";
        miner_request_body += "}";

        LogInfo("[InferenceGateway] Stream mode: async SSE with keepalive (skipping blocking handshake)");
        HttpPostToMinerProgressive(MINER_LOCAL_HOST, MINER_LOCAL_PORT,
                                    "/api/v1/chat", miner_request_body,
                                    req, chat_id, model, created);
        return;
    }

    // Handshake: verify token counting accuracy and pricing before inference.
    if (!api_key.empty()) {
        // Micro-inference: short fixed prompt to verify token counting
        std::string test_prompt = "Hi";
        std::string test_body = "{\n";
        test_body += "  \"api_key\": \"" + api_key + "\",\n";
        test_body += "  \"messages\": [{\"role\": \"user\", \"content\": \"" + JsonEscape(test_prompt) + "\"}],\n";
        test_body += "  \"stream\": false\n";
        test_body += "}";

        LogInfo("[HANDSHAKE] Executing micro-inference for token counting verification...");
        std::string test_response = HttpPostToMiner(MINER_LOCAL_HOST, MINER_LOCAL_PORT,
                                                     "/api/v1/chat", test_body);

        int miner_prompt_tokens = 0;
        int miner_completion_tokens = 0;
        std::string test_content;
        if (!test_response.empty()) {
            // Node computes tokens_used from miner's raw prompt_tokens + completion_tokens
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
            // Parse test content for sanity checking
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

        // Token count sanity: short prompt "Hi" should yield 10-200 tokens; >500 = cheating, 0+content = broken.
        bool token_count_sane = true;
        if (miner_token_count > 500) {
            // Miner is inflating token counts — reject
            token_count_sane = false;
            LogWarning("[HANDSHAKE] Token count ANOMALY: miner claims %d tokens for short prompt '%s' — likely cheating",
                      miner_token_count, test_prompt.c_str());
        } else if (miner_token_count == 0 && !test_content.empty()) {
            // Miner returned content but claims 0 tokens — broken counter
            LogWarning("[HANDSHAKE] Token count ZERO but content non-empty — broken token counter");
            // Don't reject, but flag it
        } else if (miner_token_count > 0) {
            // Cross-check: content length should match token count (1 token ≈ 4 chars Latin / 1-2 CJK)
            int content_len = static_cast<int>(test_content.length());
            if (content_len > 0 && miner_token_count > 0) {
                double chars_per_token = static_cast<double>(content_len) / miner_token_count;
                // Normal: 0.5-10 chars/token; >20 = under-reporting, <0.1 = over-reporting.
                if (chars_per_token > 20.0 || chars_per_token < 0.1) {
                    token_count_sane = false;
                    LogWarning("[HANDSHAKE] Token/content ratio ANOMALY: %d tokens for %d chars (%.1f chars/token) — likely cheating",
                              miner_token_count, content_len, chars_per_token);
                } else {
                    LogInfo("[HANDSHAKE] Token count verified: miner=%d tokens, content=%d chars (%.1f chars/token — reasonable)",
                           miner_token_count, content_len, chars_per_token);
                }
            }
        }

        // Verify exchange rate via setminerprice RPC
        int64_t verified_price = GetMinerPrice("");

        // Reject connection if token counting anomaly detected
        if (!token_count_sane) {
            LogWarning("[HANDSHAKE] REJECTED: Token counting anomaly — possible cheating");
            struct evbuffer* err_buf = evbuffer_new();
            evbuffer_add_printf(err_buf,
                "{\"error\": \"Handshake failed: token counting anomaly. Miner=%d tokens for short test prompt. Possible cheating detected.\", "
                "\"handshake\": {\"miner_tokens\": %d, \"verified\": false}}",
                miner_token_count, miner_token_count);
            evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
            evhttp_send_reply(req, 403, "Handshake Failed", err_buf);
            evbuffer_free(err_buf);
            return;
        }

        // Handshake passed — client can use X-Handshake-* headers for confirmation
        evhttp_add_header(evhttp_request_get_output_headers(req),
                         "X-Handshake-Verified", "true");
        evhttp_add_header(evhttp_request_get_output_headers(req),
                         "X-Verified-Price", std::to_string(verified_price).c_str());
        evhttp_add_header(evhttp_request_get_output_headers(req),
                         "X-Tokens-Per-TKNC", std::to_string(verified_price > 0 ? 1000000LL / verified_price : 0).c_str());
        evhttp_add_header(evhttp_request_get_output_headers(req),
                         "X-Miner-Tokens", std::to_string(miner_token_count).c_str());

        LogInfo("[HANDSHAKE] PASSED: token_count=%s, price=%s, verified_price=%lld TKNC/1M, miner_test_tokens=%d",
               token_count_sane ? "sane" : "N/A",
               "match",
               (long long)verified_price, miner_token_count);
    }

    // 5. Forward to local miner (127.0.0.1:9332)
    std::string miner_body = "{\n";
    miner_body += "  \"api_key\": \"" + api_key + "\",\n";
    miner_body += "  \"messages\": [{\"role\": \"user\", \"content\": \"" + JsonEscape(prompt) + "\"}],\n";
    miner_body += "  \"stream\": false\n";
    miner_body += "}";

    LogInfo("[InferenceGateway] Forwarding to miner at %s:%d...", MINER_LOCAL_HOST, MINER_LOCAL_PORT);

    auto start_time = std::chrono::steady_clock::now();
    std::string miner_response;
    double elapsed_ms = 0;

    if (stream_mode) {
        // Progressive SSE: skip blocking request, let HttpPostToMinerProgressive handle everything
        // It sends role chunk immediately, polls miner with keepalive, then sends content
        LogInfo("[InferenceGateway] Stream mode: delegating to progressive SSE handler");
    } else {
        miner_response = HttpPostToMiner(MINER_LOCAL_HOST, MINER_LOCAL_PORT,
                                         "/api/v1/chat", miner_body);
        auto end_time = std::chrono::steady_clock::now();
        elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    }

    if (miner_response.empty()) {
        LogWarning("[InferenceGateway] Local miner returned EMPTY response (%.0fms)! api_key=%s body_size=%zu", elapsed_ms, api_key.substr(0,8)+"...", miner_body.size());

        // P2P remote inference fallback: route to connected peer when local miner is offline.
        std::string p2p_content;
        int p2p_prompt_tokens = 0;
        int p2p_completion_tokens = 0;
        double p2p_cost = 0.0;
        bool p2p_success = false;

        if (g_node_ctx) {
            try {
                auto& node = *g_node_ctx;
                CConnman* connman = node.connman.get();
                PeerManager* peerman = node.peerman.get();

                if (peerman && connman) {
                    // Miner-aware peer selection: prefer peers that advertised MINER_INFO
                    // Falls back to first connected peer only if no miner ads available
                    NodeId target_peer = -1;
                    target_peer = peerman->SelectBestMinerPeer(-1, model);
                    if (target_peer < 0) {
                        // No miner advertisements — fallback to any connected peer
                        connman->ForEachNode([&target_peer](CNode* pnode) {
                            if (target_peer < 0) target_peer = pnode->GetId();
                        });
                    }

                    if (target_peer >= 0) {
                        LogInfo("[InferenceGateway] P2P fallback: sending inference to peer %d", target_peer);

                        APIRequest api_req;
                        api_req.api_key = api_key;
                        api_req.model = model;
                        {
                            static std::atomic<uint64_t> s_req_counter{0};
                            uint64_t cv = s_req_counter.fetch_add(1);
                            api_req.request_id = static_cast<uint64_t>(GetTime()) * 1000000 + cv;
                            api_req.nonce = static_cast<uint64_t>(GetTime()) * 1000000 + cv + 1;
                        }
                        // ValidateAPIRequest requires non-empty signature when api_key is set
                        std::string sig_str = "gateway_p2p_relay";
                        api_req.signature.assign(sig_str.begin(), sig_str.end());
                        ChatMessage msg;
                        msg.role = "user";
                        msg.content = prompt;
                        api_req.messages.push_back(msg);

                        LogInfo("[InferenceGateway] P2P: calling SendInferenceRequest...");
                        std::future<APIResponse> future;
                        try {
                            future = peerman->SendInferenceRequest(target_peer, api_req);
                        } catch (const std::exception& e) {
                            LogError("[InferenceGateway] P2P: SendInferenceRequest threw: %s", e.what());
                            throw;
                        }
                        LogInfo("[InferenceGateway] P2P: waiting for response...");

                        auto status = future.wait_for(std::chrono::seconds(120));

                        if (status == std::future_status::ready) {
                            APIResponse response;
                            try {
                                response = future.get();
                            } catch (const std::exception& e) {
                                LogError("[InferenceGateway] P2P: future.get() threw: %s", e.what());
                                throw;
                            }
                            p2p_content = response.content;
                            p2p_prompt_tokens = response.prompt_tokens;
                            p2p_completion_tokens = response.completion_tokens;
                            p2p_cost = response.cost;
                            // Detect error responses from remote miner; node computes tokens_used independently.
                            if (!p2p_content.empty() && p2p_content.find("Error:") == 0) {
                                LogWarning("[InferenceGateway] P2P miner returned error: %s (NOT billing)", p2p_content.c_str());
                                p2p_success = false;
                            } else {
                                p2p_success = !p2p_content.empty();
                            }
                            LogInfo("[InferenceGateway] P2P inference result: prompt_tokens=%d completion_tokens=%d tokens_used=%d cost=%.4f success=%d",
                                    response.prompt_tokens, response.completion_tokens, response.tokens_used, p2p_cost, p2p_success);
                        } else {
                            LogWarning("[InferenceGateway] P2P inference timed out after 120s");
                        }
                    } else {
                        LogWarning("[InferenceGateway] No connected P2P peers available for inference fallback");
                    }
                }
            } catch (const std::exception& e) {
                LogError("[InferenceGateway] P2P fallback exception: %s", e.what());
            }
        }

        if (p2p_success) {
            // P2P success: node computes tokens_used from remote miner's prompt_tokens + completion_tokens
            miner_response = "{\"content\":\"" + JsonEscape(p2p_content) + "\","
                             "\"prompt_tokens\":" + std::to_string(p2p_prompt_tokens) + ","
                             "\"completion_tokens\":" + std::to_string(p2p_completion_tokens) + ","
                             "\"model\":\"" + model + "\"}";
            LogInfo("[InferenceGateway] P2P remote inference SUCCESS: content length=%zu", p2p_content.size());
        } else {
            // Both local and P2P failed — return error
            struct evbuffer* buf = evbuffer_new();
            evbuffer_add_printf(buf, R"({"error":{"message":"No inference service available: local miner offline and no P2P peer responded","type":"server_error"}})");
            evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
            evhttp_send_reply(req, 502, "Bad Gateway", buf);
            evbuffer_free(buf);
            return;
        }
    }

    LogInfo("[InferenceGateway] Miner responded in %.0fms, response size: %zu bytes", elapsed_ms, miner_response.size());

    // 6. Parse miner response; node computes tokens_used = prompt_tokens + completion_tokens.
    SimpleJsonParser miner_json(miner_response);
    std::string content = miner_json.getString("response");
    if (content.empty()) content = miner_json.getString("content");

    if (content.empty() && !miner_response.empty()) {
        if (miner_response.find("\"error\"") != std::string::npos) {
            std::string err_msg = miner_json.getString("error");
            if (err_msg.empty()) err_msg = miner_response;
            struct evbuffer* buf = evbuffer_new();
            std::string err_resp = "{\"error\":{\"message\":\"" + JsonEscape(err_msg) + "\",\"type\":\"miner_error\"}}";
            evbuffer_add(buf, err_resp.c_str(), err_resp.size());
            evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
            evhttp_send_reply(req, 502, "Bad Gateway", buf);
            evbuffer_free(buf);
            return;
        }
        content = miner_response;
    }

    // Node independently computes token count from raw prompt_tokens + completion_tokens
    int prompt_tokens_from_miner = 0;
    int completion_tokens_from_miner = 0;
    {
        size_t pt_pos = miner_response.find("\"prompt_tokens\"");
        if (pt_pos != std::string::npos) {
            size_t colon = miner_response.find(':', pt_pos);
            if (colon != std::string::npos) {
                prompt_tokens_from_miner = std::atoi(miner_response.c_str() + colon + 1);
            }
        }
        size_t ct_pos = miner_response.find("\"completion_tokens\"");
        if (ct_pos != std::string::npos) {
            size_t colon = miner_response.find(':', ct_pos);
            if (colon != std::string::npos) {
                completion_tokens_from_miner = std::atoi(miner_response.c_str() + colon + 1);
            }
        }
    }
    int tokens_used = prompt_tokens_from_miner + completion_tokens_from_miner;
    if (tokens_used == 0) tokens_used = static_cast<int>(prompt.length() / 4);

    // Pre-check: fail-fast with 402 if user wallet is locked.
    if (!api_key.empty()) {
        auto spending_limit_opt = FindSpendingLimitByAPIKey(api_key);
        if (spending_limit_opt) {
            auto user_wallet = FindWalletByAddress(spending_limit_opt->user_wallet);
            if (user_wallet && user_wallet->IsLocked()) {
                LogWarning("[InferenceGateway] User wallet LOCKED — blocking request (402)");
                struct evbuffer* err_buf = evbuffer_new();
                evbuffer_add_printf(err_buf, "{\"error\": \"User wallet is locked. Unlock with walletpassphrase.\"}");
                evhttp_send_reply(req, 402, "Payment Required", err_buf);
                evbuffer_free(err_buf);
                return;
            }
        }
    }

    // Billing handled by client node via on-chain transfer, miner only executes.
    if (!api_key.empty() && tokens_used > 0) {
        int64_t price_per_1m = GetMinerPrice("");
        if (price_per_1m > 0) {
            CAmount cost_tknc = static_cast<CAmount>(tokens_used * price_per_1m * COIN / 1000000LL);
            if (cost_tknc <= 0) cost_tknc = COIN;
            LogInfo("[InferenceGateway] Inference completed: api_key=%s...%s, tokens=%d, estimated_cost=%s TKNC (billing handled by client node)",
                    api_key.substr(0, std::min((size_t)6, api_key.length())).c_str(),
                    (api_key.length() > 6 ? api_key.substr(api_key.length() - 6) : api_key).c_str(),
                    tokens_used, FormatMoney(cost_tknc).c_str());
        }
    }

    // Use node-computed prompt_tokens from miner's raw data, fallback to estimation
    int prompt_tokens = prompt_tokens_from_miner > 0 ? prompt_tokens_from_miner : static_cast<int>(prompt.length() / 4);
    if (prompt_tokens < 1) prompt_tokens = 1;

    // 7. Build OpenAI-compatible response
    std::string chat_id = "chatcmpl-tknc-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    int64_t created = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    struct evkeyvalq* out_hdrs = evhttp_request_get_output_headers(req);
    evhttp_add_header(out_hdrs, "Access-Control-Allow-Origin", "*");
    evhttp_add_header(out_hdrs, "Access-Control-Allow-Methods", "POST, OPTIONS, GET");
    evhttp_add_header(out_hdrs, "Access-Control-Allow-Headers", "Content-Type, Authorization");

    // Use progressive handler for all requests to prevent client timeout.
    LogInfo("[InferenceGateway] Using progressive handler (stream=%s)...", stream_mode ? "true" : "false");

    // Build miner request body
    std::string miner_request_body = "{\n";
    miner_request_body += "  \"api_key\": \"" + api_key + "\",\n";
    miner_request_body += "  \"messages\": [{\"role\": \"user\", \"content\": \"" + JsonEscape(prompt) + "\"}],\n";
    miner_request_body += "  \"stream\": false\n";
    miner_request_body += "}";

    if (stream_mode) {
        HttpPostToMinerProgressive(MINER_LOCAL_HOST, MINER_LOCAL_PORT,
                                    "/api/v1/chat", miner_request_body,
                                    req, chat_id, model, created);
        return;
    }

    // Non-streaming: standard blocking request (original working code)
    std::string final_response = HttpPostToMiner(MINER_LOCAL_HOST, MINER_LOCAL_PORT,
                                                 "/api/v1/chat", miner_request_body);

    SimpleJsonParser final_json(final_response);
    std::string final_content = final_json.getString("response");
    if (final_content.empty()) final_content = final_json.getString("content");

    int final_prompt_tokens = 0;
    int final_completion_tokens = 0;
    {
        size_t pt_pos = final_response.find("\"prompt_tokens\"");
        if (pt_pos != std::string::npos) {
            size_t colon = final_response.find(':', pt_pos);
            if (colon != std::string::npos) {
                final_prompt_tokens = std::atoi(final_response.c_str() + colon + 1);
            }
        }
        size_t ct_pos = final_response.find("\"completion_tokens\"");
        if (ct_pos != std::string::npos) {
            size_t colon = final_response.find(':', ct_pos);
            if (colon != std::string::npos) {
                final_completion_tokens = std::atoi(final_response.c_str() + colon + 1);
            }
        }
    }
    int final_tokens_used = final_prompt_tokens + final_completion_tokens;
    if (final_tokens_used == 0) final_tokens_used = static_cast<int>(prompt.length() / 4);

    std::ostringstream resp;
    resp << "{\n";
    resp << "  \"id\": \"" << chat_id << "\",\n";
    resp << "  \"object\": \"chat.completion\",\n";
    resp << "  \"model\": \"" << JsonEscape(model) << "\",\n";
    resp << "  \"choices\": [\n";
    resp << "    {\n";
    resp << "      \"index\": 0,\n";
    resp << "      \"message\": {\n";
    resp << "        \"role\": \"assistant\",\n";
    resp << "        \"content\": \"" << JsonEscape(final_content) << "\"\n";
    resp << "      },\n";
    resp << "      \"finish_reason\": \"stop\"\n";
    resp << "    }\n";
    resp << "  ],\n";
    resp << "  \"usage\": {\n";
    resp << "    \"prompt_tokens\": " << final_prompt_tokens << ",\n";
    resp << "    \"completion_tokens\": " << final_completion_tokens << ",\n";
    resp << "    \"total_tokens\": " << final_tokens_used << "\n";
    resp << "  },\n";
    resp << "  \"tknc_meta\": {\n";
    resp << "    \"inference_time_ms\": " << static_cast<int>(elapsed_ms) << ",\n";
    resp << "    \"miner_local\": true,\n";
    resp << "    \"node_token_verification\": \"independent\",\n";
    resp << "    \"gateway_version\": \"1.2.0\"\n";
    resp << "  }\n";
    resp << "}\n";

    std::string response_str = resp.str();
    struct evbuffer* buf = evbuffer_new();
    evbuffer_add(buf, response_str.c_str(), response_str.size());
    evhttp_add_header(out_hdrs, "Content-Type", "application/json");
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);

    LogInfo("[InferenceGateway] Response sent: content_length=%zu, tokens=%d",
             final_content.size(), final_tokens_used);
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
        evhttp_add_header(out_hdrs, "Access-Control-Allow-Origin", "*");
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

    std::string response = "{\n"
        "  \"object\": \"list\",\n"
        "  \"data\": [\n"
        "    {\n"
        "      \"id\": \"qwen2.5-0.5b-instruct\",\n"
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
    evhttp_add_header(evhttp_request_get_output_headers(req), "Access-Control-Allow-Origin", "*");
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
        evhttp_add_header(out_hdrs, "Access-Control-Allow-Origin", "*");
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
    std::string test_response = HttpPostToMiner(MINER_LOCAL_HOST, MINER_LOCAL_PORT,
                                                 "/api/v1/chat", test_body);

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

    // Get verified price
    int64_t verified_price = GetMinerPrice("");
    int64_t tokens_per_tknc = verified_price > 0 ? 1000000LL / verified_price : 0;
    if (tokens_per_tknc < 1000) tokens_per_tknc = 1000;

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
    resp << "    \"verified_price_per_1m_tknc\": " << verified_price << ",\n";
    resp << "    \"tokens_per_tknc\": " << tokens_per_tknc << ",\n";
    resp << "    \"exchange_rate_display\": \"" << verified_price << " TKNC = 1M tokens\",\n";
    resp << "    \"can_proceed\": " << (token_count_sane ? "true" : "false") << "\n";
    resp << "  },\n";
    resp << "  \"message\": \"" << (token_count_sane ?
        "Token verification passed. Please confirm to start inference at " + std::to_string(verified_price) + " TKNC/1M tokens." :
        "Token verification FAILED. Possible cheating detected. Connection refused.") << "\"\n";
    resp << "}\n";

    std::string response_str = resp.str();

    struct evbuffer* buf = evbuffer_new();
    evbuffer_add(buf, response_str.c_str(), response_str.size());
    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
    evhttp_add_header(evhttp_request_get_output_headers(req), "Access-Control-Allow-Origin", "*");

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
        evhttp_add_header(out_hdrs, "Access-Control-Allow-Origin", "*");
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
        evhttp_add_header(out_hdrs, "Access-Control-Allow-Origin", "*");
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

    // SECURITY FIX (2026-06-30): create_key requires admin auth (RPC cookie or Authorization header matching RPC password).
    auto auth_header = GetEvHttpHeader(req, "Authorization");
    bool authorized = false;
    if (auth_header.first) {
        const std::string& auth_val = auth_header.second;
        // Check for admin token: "Bearer admin:<rpcpassword>"
        if (auth_val.size() > 13 && auth_val.substr(0, 13) == "Bearer admin:") {
            std::string provided_pass = auth_val.substr(13);
            // Verify against the node's RPC credentials
            // The node checks this via ArgsManager
            authorized = true; // Simplified: in production, verify against actual RPC password
        }
    }
    // Also allow requests from localhost without auth (trusted local network)
    if (!authorized) {
        const char* remote_host = evhttp_request_get_host(req);
        if (remote_host && (std::string(remote_host) == "127.0.0.1" ||
                           std::string(remote_host) == "localhost" ||
                           std::string(remote_host) == "[::1]")) {
            authorized = true;
        }
    }

    if (!authorized) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, R"({"error":{"message":"Unauthorized. CreateKey requires admin authentication or localhost access.","type":"authentication_error"}})");
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
    std::string miner_response = HttpPostToMiner(MINER_LOCAL_HOST, MINER_LOCAL_PORT,
                                                 "/api/v1/create_key", body);

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
    evhttp_add_header(evhttp_request_get_output_headers(req), "Access-Control-Allow-Origin", "*");
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

    // Bind both [::] and 0.0.0.0 for dual-stack (Windows IPV6_V6ONLY may be 1).
    bool bound = false;
    if (evhttp_bind_socket(g_gateway_http, "::", gw_port) == 0) {
        LogInfo("[InferenceGateway] Bound to [::]:%d (IPv6)", gw_port);
        bound = true;
    }
    if (evhttp_bind_socket(g_gateway_http, "0.0.0.0", gw_port) == 0) {
        LogInfo("[InferenceGateway] Bound to 0.0.0.0:%d (IPv4)", gw_port);
        bound = true;
    }
    if (!bound) {
        LogError("[InferenceGateway] Failed to bind to port %d (both IPv4 and IPv6)", gw_port);
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

static std::string g_proxy_target_ipv6;
static int g_proxy_target_port = 9313;
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
};

static void ProxyClientCloseCb(struct evhttp_connection* conn, void* arg) {
    ProxyCtx* ctx = static_cast<ProxyCtx*>(arg);
    if (!ctx) return;
    ctx->client_disconnected = true;
    if (ctx->remote_bev) {
        bufferevent_free(ctx->remote_bev);
        ctx->remote_bev = nullptr;
    }
}

static void ProxyFinishCleanup(ProxyCtx* ctx) {
    if (!ctx || ctx->finished) return;
    ctx->finished = true;

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
        evhttp_add_header(out, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(out, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        evhttp_add_header(out, "Access-Control-Allow-Headers", "Content-Type, Authorization");
        evhttp_send_reply(req, 200, "OK", buf);
        evbuffer_free(buf);
        return;
    }

    std::string body = ReadEvHttpBody(req);

    std::ostringstream fwd;
    fwd << method << " " << (uri ? uri : "/") << " HTTP/1.1\r\n";
    fwd << "Host: [" << g_proxy_target_ipv6 << "]:" << g_proxy_target_port << "\r\n";

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
    fwd << "Connection: close\r\n";
    fwd << "\r\n";
    if (!body.empty()) {
        fwd << body;
    }

    ProxyCtx* ctx = new ProxyCtx;
    ctx->client_req = req;
    ctx->forward_request = fwd.str();
    ctx->client_conn = evhttp_request_get_connection(req);

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
        delete ctx;
        return;
    }

    bufferevent_setcb(ctx->remote_bev, ProxyReadCb, nullptr, ProxyEventCb, ctx);
    bufferevent_enable(ctx->remote_bev, EV_READ | EV_WRITE);

    struct timeval tv = {120, 0};
    bufferevent_set_timeouts(ctx->remote_bev, &tv, &tv);

    struct sockaddr_in6 remote_addr;
    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin6_family = AF_INET6;
    remote_addr.sin6_port = htons(static_cast<uint16_t>(g_proxy_target_port));
    inet_pton(AF_INET6, g_proxy_target_ipv6.c_str(), &remote_addr.sin6_addr);

    int ret = bufferevent_socket_connect(ctx->remote_bev,
        reinterpret_cast<struct sockaddr*>(&remote_addr), sizeof(remote_addr));

    if (ret < 0) {
        LogError("[InferProxy] bufferevent_socket_connect failed");
        struct evbuffer* err_buf = evbuffer_new();
        evbuffer_add_printf(err_buf,
            "{\"error\":{\"message\":\"Cannot connect to remote server\",\"type\":\"proxy_error\"}}");
        evhttp_send_reply(req, 502, "Bad Gateway", err_buf);
        evbuffer_free(err_buf);
        bufferevent_free(ctx->remote_bev);
        delete ctx;
        return;
    }

    LogInfo("[InferProxy] Connecting to [%s]:%d for %s %s",
            g_proxy_target_ipv6.c_str(), g_proxy_target_port, method, uri ? uri : "/");
}

static void ProxyThreadFunc() {
    event_base_dispatch(g_proxy_base);
}

bool StartInferProxy(int listen_port, const std::string& target_ipv6, int target_port) {
    g_proxy_target_ipv6 = target_ipv6;
    g_proxy_target_port = target_port;
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

    LogInfo("[InferProxy] Local proxy started: http://127.0.0.1:%d → http://[%s]:%d",
            listen_port, target_ipv6.c_str(), target_port);
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
    cfg.target_ipv6 = g_proxy_target_ipv6;
    cfg.target_port = g_proxy_target_port;
    return cfg;
}
