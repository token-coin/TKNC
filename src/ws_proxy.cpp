#include <ws_proxy.h>

#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/keyvalq_struct.h>
#include <event2/thread.h>
#include <event2/util.h>

#include <crypto/sha1.h>
#include <logging.h>
#include <common/args.h>
#include <random.h>
#include <util/strencodings.h>
#include <common/netif.h>
#include <common/pcp.h>
#include <netaddress.h>
#include <util/threadinterrupt.h>
#include <net.h>
#include <seed_register.h>

#include <string>
#include <thread>
#include <vector>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

static const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static const char* MINER_HOST = "127.0.0.1";
static const int MINER_PORT = 9332;

struct ProxySession {
    struct bufferevent* client_bev;
    struct bufferevent* miner_bev;
    struct event_base* base;
    std::string miner_read_buf;
    int state;
};

static std::string GenerateWSKey() {
    unsigned char key_bytes[16];
    GetRandBytes(key_bytes);
    return EncodeBase64(std::string(reinterpret_cast<char*>(key_bytes), 16));
}

static void cleanup_session(ProxySession* session) {
    if (session->client_bev) {
        bufferevent_free(session->client_bev);
        session->client_bev = nullptr;
    }
    if (session->miner_bev) {
        bufferevent_free(session->miner_bev);
        session->miner_bev = nullptr;
    }
    delete session;
}

static void forward_readcb(struct bufferevent* bev, void* ctx) {
    auto* session = static_cast<ProxySession*>(ctx);
    struct bufferevent* dst = (bev == session->client_bev) ? session->miner_bev : session->client_bev;
    if (!dst) return;

    struct evbuffer* input = bufferevent_get_input(bev);
    struct evbuffer* output = bufferevent_get_output(dst);
    evbuffer_add_buffer(output, input);
}

static void forward_eventcb(struct bufferevent* bev, short events, void* ctx) {
    auto* session = static_cast<ProxySession*>(ctx);
    if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) {
        LogInfo("WSProxy: Connection closed (events=0x%x)", events);
        cleanup_session(session);
    }
}

static void miner_connect_cb(struct bufferevent* bev, short events, void* ctx) {
    auto* session = static_cast<ProxySession*>(ctx);

    if (events & BEV_EVENT_CONNECTED) {
        std::string ws_key = GenerateWSKey();
        std::string request;
        request += "GET /ws/chat HTTP/1.1\r\n";
        request += "Host: 127.0.0.1:9332\r\n";
        request += "Upgrade: websocket\r\n";
        request += "Connection: Upgrade\r\n";
        request += "Sec-WebSocket-Key: " + ws_key + "\r\n";
        request += "Sec-WebSocket-Version: 13\r\n";
        request += "\r\n";

        bufferevent_write(bev, request.data(), request.size());
        session->state = 1;
        LogInfo("WSProxy: Connected to miner, sent upgrade request");
    } else {
        LogInfo("WSProxy: Failed to connect to miner (events=0x%x)", events);
        cleanup_session(session);
    }
}

static void miner_read_cb(struct bufferevent* bev, void* ctx) {
    auto* session = static_cast<ProxySession*>(ctx);

    if (session->state == 1) {
        struct evbuffer* input = bufferevent_get_input(bev);
        size_t len = evbuffer_get_length(input);
        if (len == 0) return;

        std::vector<char> data(len);
        evbuffer_copyout(input, data.data(), len);
        session->miner_read_buf.append(data.data(), len);

        size_t header_end = session->miner_read_buf.find("\r\n\r\n");
        if (header_end == std::string::npos) return;

        bool is_101 = (session->miner_read_buf.find("101") != std::string::npos);

        if (is_101) {
            size_t consumed = header_end + 4;
            evbuffer_drain(input, consumed);
            session->miner_read_buf.clear();

            session->state = 2;

            bufferevent_setcb(session->client_bev, forward_readcb, nullptr, forward_eventcb, session);
            bufferevent_setcb(session->miner_bev, forward_readcb, nullptr, forward_eventcb, session);
            bufferevent_enable(session->client_bev, EV_READ | EV_WRITE);
            bufferevent_enable(session->miner_bev, EV_READ | EV_WRITE);

            LogInfo("WSProxy: Bidirectional forwarding established");
        } else {
            LogInfo("WSProxy: Miner returned non-101 response");
            cleanup_session(session);
        }
    }
}

static void handle_request(struct evhttp_request* req, void* arg) {
    const char* uri = evhttp_request_get_uri(req);

    if (!uri || strcmp(uri, "/ws/chat") != 0 || evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, "{\"error\": \"Not found\"}");
        evhttp_send_reply(req, 404, "Not Found", buf);
        evbuffer_free(buf);
        return;
    }

    struct evkeyvalq* headers = evhttp_request_get_input_headers(req);
    const char* upgrade = evhttp_find_header(headers, "Upgrade");
    const char* ws_key = evhttp_find_header(headers, "Sec-WebSocket-Key");

    if (!upgrade ||
#ifdef _WIN32
        _stricmp(upgrade, "websocket") != 0
#else
        strcasecmp(upgrade, "websocket") != 0
#endif
        || !ws_key) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, "{\"error\": \"WebSocket upgrade required\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }

    std::string accept_input = std::string(ws_key) + WS_GUID;
    CSHA1 sha1;
    sha1.Write(reinterpret_cast<const unsigned char*>(accept_input.data()), accept_input.size());
    unsigned char hash[CSHA1::OUTPUT_SIZE];
    sha1.Finalize(hash);
    std::string accept_key = EncodeBase64(std::string(reinterpret_cast<char*>(hash), CSHA1::OUTPUT_SIZE));

    struct evkeyvalq* out_headers = evhttp_request_get_output_headers(req);
    evhttp_add_header(out_headers, "Upgrade", "websocket");
    evhttp_add_header(out_headers, "Connection", "Upgrade");
    evhttp_add_header(out_headers, "Sec-WebSocket-Accept", accept_key.c_str());

    evhttp_send_reply_start(req, 101, "Switching Protocols");

    struct evhttp_connection* evcon = evhttp_request_get_connection(req);
    struct bufferevent* client_bev = evcon ? evhttp_connection_get_bufferevent(evcon) : nullptr;
    if (!client_bev) {
        LogInfo("WSProxy: Failed to get client bufferevent");
        return;
    }

    struct event_base* base = (struct event_base*)arg;

    auto* session = new ProxySession();
    session->client_bev = client_bev;
    session->miner_bev = nullptr;
    session->base = base;
    session->state = 0;

    session->miner_bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
    if (!session->miner_bev) {
        LogInfo("WSProxy: Failed to create miner bufferevent");
        cleanup_session(session);
        return;
    }

    bufferevent_setcb(session->miner_bev, miner_read_cb, nullptr, miner_connect_cb, session);

    struct sockaddr_in miner_addr;
    memset(&miner_addr, 0, sizeof(miner_addr));
    miner_addr.sin_family = AF_INET;
    miner_addr.sin_port = htons(MINER_PORT);
    inet_pton(AF_INET, MINER_HOST, &miner_addr.sin_addr);

    if (bufferevent_socket_connect(session->miner_bev, (struct sockaddr*)&miner_addr, sizeof(miner_addr)) < 0) {
        LogInfo("WSProxy: Failed to initiate miner connection");
        cleanup_session(session);
        return;
    }

    LogInfo("WSProxy: Client upgraded, connecting to miner at %s:%d", MINER_HOST, MINER_PORT);
}

bool StartWSProxyServer(int port) {
    struct event_base* base = event_base_new();
    if (!base) {
        LogInfo("WSProxy: Failed to create event_base");
        return false;
    }

    struct evhttp* http = evhttp_new(base);
    if (!http) {
        LogInfo("WSProxy: Failed to create evhttp");
        event_base_free(base);
        return false;
    }

    evhttp_set_gencb(http, handle_request, base);

    std::string ws_bind = gArgs.GetArg("-wsproxybind", "127.0.0.1");
    struct evhttp_bound_socket* handle = evhttp_bind_socket_with_handle(http, ws_bind.c_str(), port);
    if (!handle) {
        LogInfo("WSProxy: Failed to bind to %s:%d", ws_bind.c_str(), port);
        evhttp_free(http);
        event_base_free(base);
        return false;
    }

    std::thread dispatch_thread([base]() {
        event_base_dispatch(base);
    });
    dispatch_thread.detach();

    LogInfo("WSProxy: WebSocket proxy started on %s:%d", ws_bind.c_str(), port);

    static CThreadInterrupt wsproxy_mapport_interrupt;
    std::thread([base, port]() {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        try {
            auto gateway4 = QueryDefaultGateway(NET_IPV4);
            if (gateway4) {
                auto res = NATPMPRequestPortMap(*gateway4,
                    port, 3600, wsproxy_mapport_interrupt);

                if (auto* mapping = std::get_if<MappingResult>(&res)) {
                    LogInfo("WSProxy: Port %d mapped to external %s", port, mapping->external.ToStringAddr().c_str());
                    AddLocal(mapping->external, LOCAL_MAPPED);
                    NodeRegistrationInfo info;
                    info.public_ip = mapping->external.ToStringAddr();
                    RegisterNodeToSeed(info);
                } else {
                    LogWarning("WSProxy: Failed to map port %d via NAT-PMP", port);
                }
            } else {
                LogWarning("WSProxy: No IPv4 gateway found for port mapping");
            }
        } catch (...) {
            LogWarning("WSProxy: Exception during port 9332 mapping");
        }
    }).detach();

    return true;
}

struct ReverseConnectSession {
    struct bufferevent* remote_bev;
    struct bufferevent* miner_bev;
    struct event_base* base;
    std::string remote_read_buf;
    std::string miner_read_buf;
    int remote_state;
    int miner_state;
    std::string target_url;
};

static void rc_cleanup(ReverseConnectSession* session) {
    if (session->remote_bev) {
        bufferevent_free(session->remote_bev);
        session->remote_bev = nullptr;
    }
    if (session->miner_bev) {
        bufferevent_free(session->miner_bev);
        session->miner_bev = nullptr;
    }
    delete session;
}

static void rc_forward_readcb(struct bufferevent* bev, void* ctx) {
    auto* session = static_cast<ReverseConnectSession*>(ctx);
    struct bufferevent* dst = (bev == session->remote_bev) ? session->miner_bev : session->remote_bev;
    if (!dst) return;

    struct evbuffer* input = bufferevent_get_input(bev);
    struct evbuffer* output = bufferevent_get_output(dst);
    evbuffer_add_buffer(output, input);
}

static void rc_forward_eventcb(struct bufferevent* bev, short events, void* ctx) {
    auto* session = static_cast<ReverseConnectSession*>(ctx);
    if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) {
        LogInfo("ReverseConnect: Connection closed (events=0x%x)", events);
        rc_cleanup(session);
    }
}

static void rc_remote_read_cb(struct bufferevent* bev, void* ctx) {
    auto* session = static_cast<ReverseConnectSession*>(ctx);

    if (session->remote_state == 1) {
        struct evbuffer* input = bufferevent_get_input(bev);
        size_t len = evbuffer_get_length(input);
        if (len == 0) return;

        std::vector<char> data(len);
        evbuffer_copyout(input, data.data(), len);
        session->remote_read_buf.append(data.data(), len);

        size_t header_end = session->remote_read_buf.find("\r\n\r\n");
        if (header_end == std::string::npos) return;

        bool is_101 = (session->remote_read_buf.find("101") != std::string::npos);

        if (is_101) {
            size_t consumed = header_end + 4;
            evbuffer_drain(input, consumed);
            session->remote_read_buf.clear();
            session->remote_state = 2;

            if (session->miner_state == 2) {
                bufferevent_setcb(session->remote_bev, rc_forward_readcb, nullptr, rc_forward_eventcb, session);
                bufferevent_setcb(session->miner_bev, rc_forward_readcb, nullptr, rc_forward_eventcb, session);
                bufferevent_enable(session->remote_bev, EV_READ | EV_WRITE);
                bufferevent_enable(session->miner_bev, EV_READ | EV_WRITE);
                LogInfo("ReverseConnect: Bidirectional forwarding established");
            }
        } else {
            LogInfo("ReverseConnect: Remote returned non-101 response");
            rc_cleanup(session);
        }
    }
}

static void rc_remote_connect_cb(struct bufferevent* bev, short events, void* ctx) {
    auto* session = static_cast<ReverseConnectSession*>(ctx);

    if (events & BEV_EVENT_CONNECTED) {
        std::string ws_key = GenerateWSKey();
        std::string request;
        request += "GET /ws/chat HTTP/1.1\r\n";
        request += "Host: " + session->target_url + "\r\n";
        request += "Upgrade: websocket\r\n";
        request += "Connection: Upgrade\r\n";
        request += "Sec-WebSocket-Key: " + ws_key + "\r\n";
        request += "Sec-WebSocket-Version: 13\r\n";
        request += "\r\n";

        bufferevent_write(bev, request.data(), request.size());
        session->remote_state = 1;
        LogInfo("ReverseConnect: Connected to remote %s, sent upgrade request", session->target_url.c_str());
    } else {
        LogInfo("ReverseConnect: Failed to connect to remote (events=0x%x)", events);
        rc_cleanup(session);
    }
}

static void rc_miner_read_cb(struct bufferevent* bev, void* ctx) {
    auto* session = static_cast<ReverseConnectSession*>(ctx);

    if (session->miner_state == 1) {
        struct evbuffer* input = bufferevent_get_input(bev);
        size_t len = evbuffer_get_length(input);
        if (len == 0) return;

        std::vector<char> data(len);
        evbuffer_copyout(input, data.data(), len);
        session->miner_read_buf.append(data.data(), len);

        size_t header_end = session->miner_read_buf.find("\r\n\r\n");
        if (header_end == std::string::npos) return;

        bool is_101 = (session->miner_read_buf.find("101") != std::string::npos);

        if (is_101) {
            size_t consumed = header_end + 4;
            evbuffer_drain(input, consumed);
            session->miner_read_buf.clear();
            session->miner_state = 2;

            if (session->remote_state == 2) {
                bufferevent_setcb(session->remote_bev, rc_forward_readcb, nullptr, rc_forward_eventcb, session);
                bufferevent_setcb(session->miner_bev, rc_forward_readcb, nullptr, rc_forward_eventcb, session);
                bufferevent_enable(session->remote_bev, EV_READ | EV_WRITE);
                bufferevent_enable(session->miner_bev, EV_READ | EV_WRITE);
                LogInfo("ReverseConnect: Bidirectional forwarding established");
            }
        } else {
            LogInfo("ReverseConnect: Miner returned non-101 response");
            rc_cleanup(session);
        }
    }
}

static void rc_miner_connect_cb(struct bufferevent* bev, short events, void* ctx) {
    auto* session = static_cast<ReverseConnectSession*>(ctx);

    if (events & BEV_EVENT_CONNECTED) {
        std::string ws_key = GenerateWSKey();
        std::string request;
        request += "GET /ws/chat HTTP/1.1\r\n";
        request += "Host: 127.0.0.1:9332\r\n";
        request += "Upgrade: websocket\r\n";
        request += "Connection: Upgrade\r\n";
        request += "Sec-WebSocket-Key: " + ws_key + "\r\n";
        request += "Sec-WebSocket-Version: 13\r\n";
        request += "\r\n";

        bufferevent_write(bev, request.data(), request.size());
        session->miner_state = 1;
        LogInfo("ReverseConnect: Connected to miner, sent upgrade request");
    } else {
        LogInfo("ReverseConnect: Failed to connect to miner (events=0x%x)", events);
        rc_cleanup(session);
    }
}

static bool ParseWSUrl(const std::string& url, std::string& host, int& port, std::string& path) {
    std::string remaining = url;
    if (remaining.substr(0, 5) == "ws://") {
        remaining = remaining.substr(5);
    } else if (remaining.substr(0, 6) == "wss://") {
        remaining = remaining.substr(6);
    } else {
        return false;
    }

    size_t slash_pos = remaining.find('/');
    if (slash_pos != std::string::npos) {
        path = remaining.substr(slash_pos);
        remaining = remaining.substr(0, slash_pos);
    } else {
        path = "/ws/chat";
    }

    size_t colon_pos = remaining.rfind(':');
    if (colon_pos != std::string::npos) {
        host = remaining.substr(0, colon_pos);
        port = std::stoi(remaining.substr(colon_pos + 1));
    } else {
        host = remaining;
        port = 80;
    }

    return !host.empty();
}

bool StartReverseConnect(const std::string& target_ws_url) {
    std::string host;
    int port = 80;
    std::string path;

    if (!ParseWSUrl(target_ws_url, host, port, path)) {
        LogInfo("ReverseConnect: Invalid URL format: %s", target_ws_url.c_str());
        return false;
    }

    LogInfo("ReverseConnect: Parsing URL -> host=%s, port=%d, path=%s", host.c_str(), port, path.c_str());

    struct event_base* base = event_base_new();
    if (!base) {
        LogInfo("ReverseConnect: Failed to create event_base");
        return false;
    }

    auto* session = new ReverseConnectSession();
    session->base = base;
    session->remote_bev = nullptr;
    session->miner_bev = nullptr;
    session->remote_state = 0;
    session->miner_state = 0;
    session->target_url = host + ":" + std::to_string(port);

    session->remote_bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
    if (!session->remote_bev) {
        LogInfo("ReverseConnect: Failed to create remote bufferevent");
        delete session;
        event_base_free(base);
        return false;
    }

    session->miner_bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
    if (!session->miner_bev) {
        LogInfo("ReverseConnect: Failed to create miner bufferevent");
        bufferevent_free(session->remote_bev);
        delete session;
        event_base_free(base);
        return false;
    }

    bufferevent_setcb(session->remote_bev, rc_remote_read_cb, nullptr, rc_remote_connect_cb, session);
    bufferevent_setcb(session->miner_bev, rc_miner_read_cb, nullptr, rc_miner_connect_cb, session);

    struct sockaddr_in remote_addr;
    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &remote_addr.sin_addr);

    if (bufferevent_socket_connect(session->remote_bev, (struct sockaddr*)&remote_addr, sizeof(remote_addr)) < 0) {
        LogInfo("ReverseConnect: Failed to initiate remote connection to %s:%d", host.c_str(), port);
        bufferevent_free(session->remote_bev);
        bufferevent_free(session->miner_bev);
        delete session;
        event_base_free(base);
        return false;
    }

    struct sockaddr_in miner_addr;
    memset(&miner_addr, 0, sizeof(miner_addr));
    miner_addr.sin_family = AF_INET;
    miner_addr.sin_port = htons(MINER_PORT);
    inet_pton(AF_INET, MINER_HOST, &miner_addr.sin_addr);

    if (bufferevent_socket_connect(session->miner_bev, (struct sockaddr*)&miner_addr, sizeof(miner_addr)) < 0) {
        LogInfo("ReverseConnect: Failed to initiate miner connection");
        bufferevent_free(session->remote_bev);
        bufferevent_free(session->miner_bev);
        delete session;
        event_base_free(base);
        return false;
    }

    std::thread dispatch_thread([base]() {
        event_base_dispatch(base);
    });
    dispatch_thread.detach();

    LogInfo("ReverseConnect: Started, connecting to remote %s:%d and miner %s:%d",
        host.c_str(), port, MINER_HOST, MINER_PORT);

    return true;
}