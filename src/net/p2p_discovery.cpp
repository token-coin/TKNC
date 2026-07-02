#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif

#include <net/p2p_discovery.h>
#include <net/message.h>
#include <util/log.h>
#include <net.h>

extern std::map<CNetAddr, LocalServiceInfo> mapLocalHost;
extern GlobalMutex g_maplocalhost_mutex;

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/util.h>

#include <cstring>
#include <sstream>
#include <algorithm>
#include <chrono>

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#ifndef _WIN32
#include <fcntl.h>
#endif
#endif

struct P2PConnection {
    std::string peer_id;
    PeerInfo peer_info;
    struct bufferevent* bev;
    int retry_count;
    std::chrono::steady_clock::time_point last_heartbeat_sent;
    std::chrono::steady_clock::time_point last_heartbeat_received;

    P2PConnection()
        : bev(nullptr)
        , retry_count(0)
    {}
};

class P2PDiscovery::Impl {
public:
    struct event_base* base;
    std::map<std::string, std::unique_ptr<P2PConnection>> active_connections;

    Impl()
        : base(nullptr)
    {}

    ~Impl() {
        if (base) {
            event_base_loopexit(base, nullptr);
            event_base_free(base);
            base = nullptr;
        }
        active_connections.clear();
    }

    bool Initialize() {
        base = event_base_new();
        if (!base) {
            LogError("P2PDiscovery: Failed to create libevent base");
            return false;
        }
        return true;
    }
};

std::vector<uint8_t> P2PDiscovery::HeartbeatMessage::Serialize() const {
    std::vector<uint8_t> data(13);
    memcpy(data.data(), &magic, 4);
    data[4] = type;
    memcpy(data.data() + 5, &timestamp, 8);
    return data;
}

bool P2PDiscovery::HeartbeatMessage::Deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 13) return false;
    memcpy(&magic, data.data(), 4);
    type = data[4];
    memcpy(&timestamp, data.data() + 5, 8);
    return magic == HEARTBEAT_MAGIC && (type == MSG_PING || type == MSG_PONG);
}

P2PDiscovery& P2PDiscovery::GetInstance() {
    static P2PDiscovery instance;
    return instance;
}

P2PDiscovery::P2PDiscovery()
    : local_port_(9333)
    , running_(false)
{
    impl_ = std::make_unique<Impl>();
    llm_handler_ = std::make_unique<P2PLLMPeerHandler>();
}

P2PDiscovery::~P2PDiscovery() {
    StopDiscovery();
}

void P2PDiscovery::SetRole(const std::string& role) {
    role_ = role;
    LogInfo("P2PDiscovery: Role set to: %s", role.c_str());
}

void P2PDiscovery::SetLocalPort(int port) {
    local_port_ = port;
}

void P2PDiscovery::SetWallet(const std::string& wallet) {
    local_wallet_ = wallet;
}

void P2PDiscovery::SetWsPort(int port) {
    ws_port_ = port;
}

void P2PDiscovery::StartDiscovery() {
    if (running_.load()) {
        LogWarning("P2PDiscovery: Already running");
        return;
    }

    if (!impl_->Initialize()) {
        LogError("P2PDiscovery: Failed to initialize implementation");
        return;
    }

    running_.store(true);

    discovery_thread_ = std::thread(&P2PDiscovery::DiscoveryLoop, this);
    heartbeat_thread_ = std::thread(&P2PDiscovery::HeartbeatLoop, this);

    LogInfo("P2PDiscovery: Started (role=%s, port=%d)", role_.c_str(), local_port_);
}

void P2PDiscovery::StopDiscovery() {
    if (!running_.load()) return;

    running_.store(false);

    if (discovery_thread_.joinable()) {
        discovery_thread_.join();
    }

    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& pair : known_peers_) {
            pair.second.connected = false;
        }
    }

    LogInfo("P2PDiscovery: Stopped");
}

void P2PDiscovery::OnPeerDiscovered(PeerDiscoveredCallback cb) {
    on_peer_discovered_ = cb;
}

void P2PDiscovery::OnPeerConnected(PeerConnectedCallback cb) {
    on_peer_connected_ = cb;
}

std::vector<PeerInfo> P2PDiscovery::GetConnectedPeers() const {
    std::vector<PeerInfo> result;
    std::lock_guard<std::mutex> lock(peers_mutex_);
    for (const auto& pair : known_peers_) {
        if (pair.second.connected) {
            result.push_back(pair.second);
        }
    }
    return result;
}

std::vector<PeerInfo> P2PDiscovery::GetMinerPeers() const {
    std::vector<PeerInfo> result;
    std::lock_guard<std::mutex> lock(peers_mutex_);
    for (const auto& pair : known_peers_) {
        if (pair.second.connected && pair.second.role == "miner") {
            result.push_back(pair.second);
        }
    }
    return result;
}

void P2PDiscovery::DiscoveryLoop() {
    while (running_.load()) {
        if (role_ == "client") {
            QuerySeedForPeers();
        } else if (role_ == "miner" || role_ == "node") {
            // P2P-only: miner discovery via MINER_INFO gossip broadcast (net_processing.cpp)
            // HTTP registration removed — pseudo-distributed layer eliminated
        }

        std::this_thread::sleep_for(std::chrono::seconds(DISCOVERY_INTERVAL_SECONDS));
    }
}

void P2PDiscovery::QuerySeedForPeers() {
    LogInfo("P2PDiscovery: Querying seed for miner peers...");

    std::vector<PeerInfo> new_peers;
    if (!FetchPeersFromSeed(new_peers)) {
        LogWarning("P2PDiscovery: Failed to fetch peers from seed");
        return;
    }

    for (const auto& peer : new_peers) {
        bool is_known = false;
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            auto it = known_peers_.find(peer.node_id);
            if (it != known_peers_.end()) {
                is_known = true;
                it->second.last_seen = peer.last_seen;
                if (!it->second.connected && peer.role == "miner") {
                    ConnectToPeer(peer);
                }
            }
        }

        if (!is_known) {
            {
                std::lock_guard<std::mutex> lock(peers_mutex_);
                known_peers_[peer.node_id] = peer;
            }

            if (on_peer_discovered_) {
                on_peer_discovered_(peer);
            }

            if (role_ == "client" && peer.role == "miner") {
                ConnectToPeer(peer);
            }
        }
    }
}

void P2PDiscovery::RegisterWithSeed() {
    LogInfo("P2PDiscovery: Registering as miner with seed...");
    RegisterWithSeedServer();
}

bool P2PDiscovery::FetchPeersFromSeed(std::vector<PeerInfo>& peers) {
    peers.clear();

#ifdef WIN32
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        LogError("P2PDiscovery: Create HTTP socket failed: %d", WSAGetLastError());
        return false;
    }
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        LogError("P2PDiscovery: Create HTTP socket failed: %s", strerror(errno));
        return false;
    }
#endif

    sockaddr_in seed_addr;
    memset(&seed_addr, 0, sizeof(seed_addr));
    seed_addr.sin_family = AF_INET;
    seed_addr.sin_port = htons(80);

#ifdef WIN32
    inet_pton(AF_INET, "127.0.0.1", &seed_addr.sin_addr);
#else
    inet_pton(AF_INET, "127.0.0.1", &seed_addr.sin_addr);
#endif

#ifdef WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    int connect_result = connect(sock, (sockaddr*)&seed_addr, sizeof(seed_addr));
    if (connect_result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(sock);
        return false;
    }

    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(sock, &write_fds);
    timeval timeout{5, 0};
    if (select(0, nullptr, &write_fds, nullptr, &timeout) <= 0) {
        closesocket(sock);
        return false;
    }

    mode = 0;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int connect_result = connect(sock, (struct sockaddr*)&seed_addr, sizeof(seed_addr));
    if (connect_result < 0 && errno != EINPROGRESS) {
        close(sock);
        return false;
    }

    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(sock, &write_fds);
    timeval timeout{5, 0};
    if (select(sock + 1, nullptr, &write_fds, nullptr, &timeout) <= 0) {
        close(sock);
        return false;
    }

    flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
#endif

    std::string request = "GET /api/p2p/peers?role=miner HTTP/1.1\r\n"
                          "Host: 66.154.101.183:80\r\n"
                          "Connection: close\r\n\r\n";

#ifdef WIN32
    send(sock, request.c_str(), request.length(), 0);
#else
    send(sock, request.c_str(), request.length(), 0);
#endif

    char buffer[4096];
    std::string response;
#ifdef WIN32
    int bytes;
    while ((bytes = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes] = '\0';
        response += buffer;
    }
    closesocket(sock);
#else
    ssize_t bytes;
    while ((bytes = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes] = '\0';
        response += buffer;
    }
    close(sock);
#endif

    size_t body_start = response.find("\r\n\r\n");
    if (body_start == std::string::npos) {
        return false;
    }
    body_start += 4;

    std::string body = response.substr(body_start);
    if (body.empty() || body == "[]") {
        return true;
    }

    ParsePeerListJson(body, peers);
    return true;
}

void P2PDiscovery::ParsePeerListJson(const std::string& json, std::vector<PeerInfo>& peers) {
    size_t pos = 0;
    while ((pos = json.find("{", pos)) != std::string::npos) {
        size_t end = json.find("}", pos);
        if (end == std::string::npos) break;

        std::string obj = json.substr(pos, end - pos + 1);
        PeerInfo info;

        size_t node_id_pos = obj.find("\"node_id\"");
        if (node_id_pos != std::string::npos) {
            size_t start = obj.find("\"", node_id_pos + 10) + 1;
            size_t end2 = obj.find("\"", start);
            info.node_id = obj.substr(start, end2 - start);
        }

        size_t addr_pos = obj.find("\"address\"");
        if (addr_pos != std::string::npos) {
            size_t start = obj.find("\"", addr_pos + 10) + 1;
            size_t end2 = obj.find("\"", start);
            info.address = obj.substr(start, end2 - start);
        }

        size_t role_pos = obj.find("\"role\"");
        if (role_pos != std::string::npos) {
            size_t start = obj.find("\"", role_pos + 7) + 1;
            size_t end2 = obj.find("\"", start);
            info.role = obj.substr(start, end2 - start);
        }

        size_t ip_pos = obj.find("\"public_ip\"");
        if (ip_pos != std::string::npos) {
            size_t start = obj.find("\"", ip_pos + 12) + 1;
            size_t end2 = obj.find("\"", start);
            info.public_ip = obj.substr(start, end2 - start);
        }

        size_t port_pos = obj.find("\"p2p_port\"");
        if (port_pos != std::string::npos) {
            size_t colon = obj.find(":", port_pos + 10);
            if (colon != std::string::npos) {
                info.p2p_port = std::stoi(obj.substr(colon + 1));
            }
        }

        info.last_seen = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if (!info.node_id.empty()) {
            peers.push_back(info);
        }

        pos = end + 1;
    }
}

void P2PDiscovery::RegisterWithSeedServer() {
    LogInfo("P2PDiscovery: Registering with seed server (http://%s:%d)...",
           SEED_HOST, SEED_HTTP_PORT);

#ifdef WIN32
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
#endif

#ifdef WIN32
    if (sock == INVALID_SOCKET) {
#else
    if (sock < 0) {
#endif
        LogWarning("P2PDiscovery: Failed to create registration socket");
        return;
    }

    sockaddr_in seed_addr;
    memset(&seed_addr, 0, sizeof(seed_addr));
    seed_addr.sin_family = AF_INET;
    seed_addr.sin_port = htons(SEED_HTTP_PORT);

    if (inet_pton(AF_INET, SEED_HOST, &seed_addr.sin_addr) != 1) {
        LogWarning("P2PDiscovery: Invalid seed address %s", SEED_HOST);
#ifdef WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return;
    }

#ifdef WIN32
    u_long mode = 1; ioctlsocket(sock, FIONBIO, &mode);
#else
    fcntl(sock, F_SETFL, O_NONBLOCK);
#endif

    int ret = connect(sock, (struct sockaddr*)&seed_addr, sizeof(seed_addr));
#ifdef WIN32
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
#else
    if (ret < 0 && errno != EINPROGRESS) {
#endif
        LogWarning("P2PDiscovery: Connection to seed failed");
#ifdef WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return;
    }

    fd_set write_fds;
    FD_ZERO(&write_fds); FD_SET(sock, &write_fds);
    timeval tv = {5, 0};
    select((int)(sock + 1), nullptr, &write_fds, nullptr, &tv);

    std::string public_ip = "127.0.0.1";
    {
        LOCK(g_maplocalhost_mutex);
        for (auto it = mapLocalHost.begin(); it != mapLocalHost.end(); ++it) {
            const CNetAddr& addr = it->first;
            if (addr.IsRoutable() && !addr.IsLocal()) {
                public_ip = addr.ToStringAddr();
                break;
            }
        }
    }

    std::string capabilities = (role_ == "miner") ? "\"llm_inference\",\"mining\"" : "\"blockchain_sync\"";
    std::string model_name = (role_ == "miner") ? "\"qwen2.5-0.5b-instruct\"" : "\"\"";

    std::string body = "{"
        "\"node_id\":\"" + public_ip + ":" + std::to_string(local_port_) + "\","
        "\"role\":\"" + role_ + "\","
        "\"wallet_address\":\"" + local_wallet_ + "\","
        "\"public_ip\":\"" + public_ip + "\","
        "\"p2p_port\":" + std::to_string(local_port_) + ","
        "\"ws_port\":" + std::to_string(ws_port_) + ","
        "\"model_name\":" + model_name + ","
        "\"capabilities\":[" + capabilities + "],"
        "\"timestamp\":" + std::to_string(GetTime()) +
    "}";

    std::string request = "POST /api/p2p/register HTTP/1.1\r\n"
        "Host: " + std::string(SEED_HOST) + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;

#ifdef WIN32
    int total_sent = 0;
    int remaining = (int)request.size();
    const char* ptr = request.c_str();
    while (total_sent < (int)request.size()) {
        int sent = send(sock, ptr + total_sent, remaining, 0);
        if (sent <= 0) break;
        total_sent += sent;
        remaining -= sent;
    }
#else
    int total_sent = 0;
    int remaining = (int)request.size();
    const char* ptr = request.c_str();
    while (total_sent < (int)request.size()) {
        int sent = send(sock, ptr + total_sent, remaining, 0);
        if (sent <= 0) break;
        total_sent += sent;
        remaining -= sent;
    }
#endif

    char resp_buf[1024] = {0};
#ifdef WIN32
    recv(sock, resp_buf, sizeof(resp_buf) - 1, 0);
    closesocket(sock);
#else
    recv(sock, resp_buf, sizeof(resp_buf) - 1, 0);
    close(sock);
#endif

    if (strstr(resp_buf, "200") || strstr(resp_buf, "201")) {
        LogInfo("P2PDiscovery: Registration successful");
    } else {
        LogInfo("P2PDiscovery: Registration response: %.100s", resp_buf);
    }
}

void P2PDiscovery::ConnectToPeer(const PeerInfo& peer) {
    std::string target_address = peer.public_ip.empty() ? peer.address : peer.public_ip;
    int target_port = peer.p2p_port > 0 ? peer.p2p_port : 9333;

    if (target_address.empty()) {
        LogWarning("P2PDiscovery: Cannot connect to peer %s - no address", peer.node_id.c_str());
        return;
    }

    LogInfo("P2PDiscovery: Connecting to peer %s at %s:%d",
            peer.node_id.c_str(), target_address.c_str(), target_port);

    sockaddr_in peer_addr;
    memset(&peer_addr, 0, sizeof(peer_addr));
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(target_port);

#ifdef WIN32
    if (inet_pton(AF_INET, target_address.c_str(), &peer_addr.sin_addr) != 1) {
        LogWarning("P2PDiscovery: Invalid address %s", target_address.c_str());
        return;
    }
#else
    if (inet_pton(AF_INET, target_address.c_str(), &peer_addr.sin_addr) != 1) {
        LogWarning("P2PDiscovery: Invalid address %s", target_address.c_str());
        return;
    }
#endif

    struct bufferevent* bev = bufferevent_socket_new(impl_->base, -1, BEV_OPT_CLOSE_ON_FREE);
    if (!bev) {
        LogError("P2PDiscovery: Failed to create bufferevent for peer %s", peer.node_id.c_str());
        return;
    }

    bufferevent_setcb(bev,
        [](struct bufferevent *bev, void *ctx) {
            auto* discovery = static_cast<P2PDiscovery*>(ctx);
            struct evbuffer* input = bufferevent_get_input(bev);
            size_t len = evbuffer_get_length(input);
            if (len > 0) {
                std::vector<uint8_t> data(len);
                evbuffer_copyout(input, data.data(), len);
                evbuffer_drain(input, len);

                std::string peer_id;
                {
                    std::lock_guard<std::mutex> lock(discovery->peers_mutex_);
                    for (auto& pair : discovery->impl_->active_connections) {
                        if (pair.second->bev == bev) {
                            peer_id = pair.first;
                            break;
                        }
                    }
                }

                if (!peer_id.empty()) {
                    discovery->HandleIncomingData(peer_id, data, bev);
                }
            }
        },
        [](struct bufferevent *bev, void *ctx) {},
        [](struct bufferevent *bev, short events, void *ctx) {
            auto* discovery = static_cast<P2PDiscovery*>(ctx);
            if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) {
                std::string peer_id;
                {
                    std::lock_guard<std::mutex> lock(discovery->peers_mutex_);
                    for (auto& pair : discovery->impl_->active_connections) {
                        if (pair.second->bev == bev) {
                            peer_id = pair.first;
                            pair.second->peer_info.connected = false;
                            break;
                        }
                    }
                }
                if (!peer_id.empty()) {
                    LogWarning("P2PDiscovery: Connection error/EOF on peer %s", peer_id.c_str());
                    discovery->impl_->active_connections.erase(peer_id);
                }
                bufferevent_free(bev);
            }
        },
        this
    );

    bufferevent_enable(bev, EV_READ | EV_WRITE);

    if (bufferevent_socket_connect(bev, (struct sockaddr*)&peer_addr, sizeof(peer_addr)) < 0) {
        LogError("P2PDiscovery: Connection initiated failed for peer %s", peer.node_id.c_str());
        bufferevent_free(bev);
        return;
    }

    auto conn = std::make_unique<P2PConnection>();
    conn->peer_id = peer.node_id;
    conn->peer_info = peer;
    conn->bev = bev;
    conn->last_heartbeat_sent = std::chrono::steady_clock::now();
    conn->last_heartbeat_received = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        impl_->active_connections[peer.node_id] = std::move(conn);
        known_peers_[peer.node_id].connected = true;
    }

    PeerInfo local_info;
    local_info.node_id = "local-" + role_;
    local_info.role = role_;
    local_info.p2p_port = local_port_;

    SendHandshake(static_cast<void*>(bev), local_info);

    if (on_peer_connected_) {
        on_peer_connected_(peer, static_cast<void*>(bev));
    }

    LogInfo("P2PDiscovery: Successfully connected to peer %s (%s:%d)",
            peer.node_id.c_str(), target_address.c_str(), target_port);
}

bool P2PDiscovery::SendHandshake(void* connection, const PeerInfo& local_info) {
    auto* bev = static_cast<struct bufferevent*>(connection);
    if (!bev) return false;

    P2PMessage version_msg;
    version_msg.SetCommand(MessageTypes::VERSION);

    std::string payload_str = local_info.role + "|" + std::to_string(local_info.p2p_port);
    version_msg.payload.assign(payload_str.begin(), payload_str.end());
    version_msg.CalculateChecksum();

    std::vector<uint8_t> serialized = version_msg.Serialize();

    if (bufferevent_write(bev, serialized.data(), serialized.size()) < 0) {
        LogError("P2PDiscovery: Failed to send handshake");
        return false;
    }

    P2PMessage verack_msg;
    verack_msg.SetCommand(MessageTypes::VERACK);
    verack_msg.CalculateChecksum();
    std::vector<uint8_t> verack_data = verack_msg.Serialize();

    if (bufferevent_write(bev, verack_data.data(), verack_data.size()) < 0) {
        LogError("P2PDiscovery: Failed to send verack");
        return false;
    }

    LogInfo("P2PDiscovery: Handshake sent to peer");
    return true;
}

void P2PDiscovery::HeartbeatLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_INTERVAL_SECONDS));

        if (!running_.load()) break;

        auto now = std::chrono::steady_clock::now();
        std::vector<std::string> timeout_peers;

        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            for (auto& pair : impl_->active_connections) {
                auto* conn = pair.second.get();
                if (!conn->bev) continue;

                HeartbeatMessage ping;
                ping.magic = HEARTBEAT_MAGIC;
                ping.type = MSG_PING;
                ping.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();

                std::vector<uint8_t> ping_data = ping.Serialize();
                if (bufferevent_write(conn->bev, ping_data.data(), ping_data.size()) >= 0) {
                    conn->last_heartbeat_sent = now;
                    LogDebug(BCLog::NET, "P2PDiscovery: Ping sent to %s", conn->peer_id.c_str());
                }

                auto duration_since_last_response = std::chrono::duration_cast<std::chrono::seconds>(
                    now - conn->last_heartbeat_received).count();

                if (duration_since_last_response > CONNECTION_TIMEOUT_SECONDS * 2) {
                    timeout_peers.push_back(pair.first);
                    LogWarning("P2PDiscovery: Peer %s heartbeat timeout after %I64d seconds",
                               conn->peer_id.c_str(),
                               static_cast<long long>(duration_since_last_response));
                }
            }
        }

        for (const auto& peer_id : timeout_peers) {
            DisconnectTimedOutPeer(peer_id);
        }
    }
}

bool P2PDiscovery::ProcessHeartbeatResponse(const std::vector<uint8_t>& data) {
    HeartbeatMessage msg;
    if (!msg.Deserialize(data)) {
        return false;
    }

    if (msg.type == MSG_PONG) {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& pair : impl_->active_connections) {
            pair.second->last_heartbeat_received = std::chrono::steady_clock::now();
        }
        LogDebug(BCLog::NET, "P2PDiscovery: Received pong response");
        return true;
    } else if (msg.type == MSG_PING) {
        HeartbeatMessage pong;
        pong.magic = HEARTBEAT_MAGIC;
        pong.type = MSG_PONG;
        pong.timestamp = msg.timestamp;

        std::vector<uint8_t> pong_data = pong.Serialize();

        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& pair : impl_->active_connections) {
            if (pair.second->bev) {
                bufferevent_write(pair.second->bev, pong_data.data(), pong_data.size());
            }
        }
        LogDebug(BCLog::NET, "P2PDiscovery: Responded to ping with pong");
        return true;
    }

    return false;
}

void P2PDiscovery::DisconnectTimedOutPeer(const std::string& peer_id) {
    std::unique_ptr<P2PConnection> conn;

    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto it = impl_->active_connections.find(peer_id);
        if (it == impl_->active_connections.end()) return;

        conn = std::move(it->second);
        impl_->active_connections.erase(it);

        auto peer_it = known_peers_.find(peer_id);
        if (peer_it != known_peers_.end()) {
            peer_it->second.connected = false;
        }
    }

    if (conn && conn->bev) {
        bufferevent_free(conn->bev);
        conn->bev = nullptr;
    }

    LogWarning("P2PDiscovery: Disconnected timed-out peer: %s", peer_id.c_str());
}

bool P2PDiscovery::CheckAndRouteLLMMessage(const std::string& peer_id, const std::vector<uint8_t>& data, void* bev) {
    if (data.size() < P2P_LLM_HEADER_SIZE) {
        return false;
    }

    uint32_t magic;
    std::memcpy(&magic, data.data(), sizeof(magic));

#ifdef WIN32
    const uint8_t* magic_bytes = reinterpret_cast<const uint8_t*>(&magic);
    uint32_t host_magic = (static_cast<uint32_t>(magic_bytes[0]) << 24) |
                          (static_cast<uint32_t>(magic_bytes[1]) << 16) |
                          (static_cast<uint32_t>(magic_bytes[2]) << 8) |
                          magic_bytes[3];
#else
    uint32_t host_magic = ntohl(magic);
#endif

    if (host_magic != P2P_LLM_MAGIC) {
        return false;
    }

    LogInfo("P2PDiscovery: Routing LLM message from peer: %s", peer_id.c_str());

    int socket_fd = -1;
#ifdef WIN32
    evutil_socket_t fd = bufferevent_getfd(static_cast<struct bufferevent*>(bev));
    socket_fd = static_cast<int>(fd);
#else
    int fd = bufferevent_getfd(static_cast<struct bufferevent*>(bev));
    socket_fd = fd;
#endif

    llm_handler_->HandleIncomingMessage(peer_id, data, socket_fd);
    return true;
}

void P2PDiscovery::HandleIncomingData(const std::string& peer_id, const std::vector<uint8_t>& data, void* bev) {
    if (!CheckAndRouteLLMMessage(peer_id, data, bev)) {
        ProcessHeartbeatResponse(data);
    }
}
