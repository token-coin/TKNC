#include <net/connection_manager.h>
#include <net/p2p_llm.h>
#include <util/time.h>
#include <cstring>
#ifndef WIN32
#include <fcntl.h>
#endif

#ifdef WIN32
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#endif

ConnectionManager& ConnectionManager::GetInstance() {
    static ConnectionManager instance;
    return instance;
}

ConnectionManager::ConnectionManager()
#ifdef WIN32
    : server_socket(INVALID_SOCKET)
#else
    : server_socket(-1)
#endif
    , local_port(0)
    , running(false)
    , initialized(false)
{
    llm_handler = std::make_unique<P2PLLMPeerHandler>();
}

ConnectionManager::~ConnectionManager() {
    Shutdown();
}

bool ConnectionManager::Initialize(uint16_t port) {
    if (initialized.load()) {
        LogInfo("ConnectionManager: Already initialized");
        return true;
    }
    
#ifdef WIN32
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        LogInfo("ConnectionManager: WSAStartup failed, error code: %d", result);
        return false;
    }
    
    server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == INVALID_SOCKET) {
        LogInfo("ConnectionManager: Create socket failed, error code: %d", WSAGetLastError());
        WSACleanup();
        return false;
    }
#else
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        LogInfo("ConnectionManager: CreatesocketFailed: %s", strerror(errno));
        return false;
    }
#endif
    
    int opt = 1;
#ifdef WIN32
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
    
    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
#ifdef WIN32
    if (bind(server_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        LogInfo("ConnectionManager: Bind port failed, error code: %d", WSAGetLastError());
        closesocket(server_socket);
        WSACleanup();
        return false;
    }
#else
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        LogInfo("ConnectionManager: BindPortFailed: %s", strerror(errno));
        close(server_socket);
        return false;
    }
#endif
    
#ifdef WIN32
    if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR) {
        LogInfo("ConnectionManager: Listen failed, error code: %d", WSAGetLastError());
        closesocket(server_socket);
        WSACleanup();
        return false;
    }
#else
    if (listen(server_socket, SOMAXCONN) < 0) {
        LogInfo("ConnectionManager: Listen failed: %s", strerror(errno));
        close(server_socket);
        return false;
    }
#endif
    
    sockaddr_in actual_addr;
#ifdef WIN32
    int addr_len = sizeof(actual_addr);
    getsockname(server_socket, (sockaddr*)&actual_addr, &addr_len);
#else
    socklen_t addr_len = sizeof(actual_addr);
    getsockname(server_socket, (struct sockaddr*)&actual_addr, &addr_len);
#endif
    
    local_port = ntohs(actual_addr.sin_port);
    initialized.store(true);
    running.store(true);
    
    accept_thread = std::thread(&ConnectionManager::AcceptLoop, this);
    heartbeat_thread = std::thread(&ConnectionManager::HeartbeatLoop, this);
    
    LogInfo("ConnectionManager: Initialize success, listening on port: %d", local_port);
    return true;
}

void ConnectionManager::Shutdown() {
    if (!running.load()) return;
    
    running.store(false);
    
    DisconnectAll();
    
#ifdef WIN32
    if (server_socket != INVALID_SOCKET) {
        shutdown(server_socket, SD_BOTH);
        closesocket(server_socket);
        server_socket = INVALID_SOCKET;
    }
#else
    if (server_socket >= 0) {
        shutdown(server_socket, SHUT_RDWR);
        close(server_socket);
        server_socket = -1;
    }
#endif
    
    if (accept_thread.joinable()) {
        accept_thread.join();
    }
    
    for (auto& pair : receive_threads) {
        if (pair.second.joinable()) {
            pair.second.join();
        }
    }
    receive_threads.clear();
    
    if (heartbeat_thread.joinable()) {
        heartbeat_thread.join();
    }
    
#ifdef WIN32
    WSACleanup();
#endif
    
    initialized.store(false);
    LogInfo("ConnectionManager: Closed");
}

bool ConnectionManager::ConnectToPeer(const std::string& address, uint16_t port) {
    if (!initialized.load()) {
        LogInfo("ConnectionManager: Not initialized");
        return false;
    }
    
    std::string peer_id = GeneratePeerId(address, port);
    
    {
        std::lock_guard<std::mutex> lock(connections_mutex);
        if (connections.find(peer_id) != connections.end()) {
            LogInfo("ConnectionManager: Connection already exists: %s", peer_id.c_str());
            return connections[peer_id]->state == CONNECTED;
        }
        
        if (connections.size() >= MAX_CONNECTIONS) {
            LogInfo("ConnectionManager: Max connections reached: %d", MAX_CONNECTIONS);
            return false;
        }
    }
    
#ifdef WIN32
    SOCKET client_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client_socket == INVALID_SOCKET) {
        LogInfo("ConnectionManager: Create client socket failed, error code: %d", WSAGetLastError());
        return false;
    }
#else
    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket < 0) {
        LogInfo("ConnectionManager: Create client socket failed: %s", strerror(errno));
        return false;
    }
#endif
    
    sockaddr_in peer_addr;
    memset(&peer_addr, 0, sizeof(peer_addr));
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(port);
    
#ifdef WIN32
    inet_pton(AF_INET, address.c_str(), &peer_addr.sin_addr);
    
    u_long mode = 1;
    ioctlsocket(client_socket, FIONBIO, &mode);
    
    int connect_result = connect(client_socket, (sockaddr*)&peer_addr, sizeof(peer_addr));
    if (connect_result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        LogInfo("ConnectionManager: Connect failed, error code: %d", WSAGetLastError());
        closesocket(client_socket);
        return false;
    }
    
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(client_socket, &write_fds);
    
    timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    
    int select_result = select(0, nullptr, &write_fds, nullptr, &timeout);
    if (select_result <= 0) {
        LogInfo("ConnectionManager: JoinTimeout");
        closesocket(client_socket);
        return false;
    }
    
    mode = 0;
    ioctlsocket(client_socket, FIONBIO, &mode);
#else
    inet_pton(AF_INET, address.c_str(), &peer_addr.sin_addr);
    
    int flags = fcntl(client_socket, F_GETFL, 0);
    fcntl(client_socket, F_SETFL, flags | O_NONBLOCK);
    
    int connect_result = connect(client_socket, (struct sockaddr*)&peer_addr, sizeof(peer_addr));
    if (connect_result < 0 && errno != EINPROGRESS) {
        LogInfo("ConnectionManager: JoinFailed: %s", strerror(errno));
        close(client_socket);
        return false;
    }
    
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(client_socket, &write_fds);
    
    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    
    int select_result = select(client_socket + 1, nullptr, &write_fds, nullptr, &timeout);
    if (select_result <= 0) {
        LogInfo("ConnectionManager: JoinTimeout");
        close(client_socket);
        return false;
    }
    
    flags = fcntl(client_socket, F_GETFL, 0);
    fcntl(client_socket, F_SETFL, flags & ~O_NONBLOCK);
#endif
    
    auto conn_info = std::make_unique<ConnectionInfo>();
    conn_info->peer_id = peer_id;
    conn_info->address = address;
    conn_info->port = port;
    conn_info->state = CONNECTED;
    conn_info->last_activity = std::chrono::steady_clock::now();
    conn_info->socket_fd = client_socket;
    
    {
        std::lock_guard<std::mutex> lock(connections_mutex);
        connections[peer_id] = std::move(conn_info);
    }
    
    receive_threads[peer_id] = std::thread(&ConnectionManager::ReceiveLoop, this, peer_id);
    
    if (connection_handler) {
        connection_handler(peer_id, true);
    }
    
    LogInfo("ConnectionManager: Successfully connected to node: %s (%s:%d)", 
            peer_id.c_str(), address.c_str(), port);
    
    return true;
}

void ConnectionManager::DisconnectPeer(const std::string& peer_id) {
    std::unique_ptr<ConnectionInfo> conn_info;
    
    {
        std::lock_guard<std::mutex> lock(connections_mutex);
        auto it = connections.find(peer_id);
        if (it == connections.end()) return;
        
        conn_info = std::move(it->second);
        connections.erase(it);
    }
    
    if (conn_info) {
        conn_info->state = DISCONNECTING;
        
#ifdef WIN32
        if (conn_info->socket_fd != INVALID_SOCKET) {
            shutdown(conn_info->socket_fd, SD_BOTH);
            closesocket(conn_info->socket_fd);
        }
#else
        if (conn_info->socket_fd >= 0) {
            shutdown(conn_info->socket_fd, SHUT_RDWR);
            close(conn_info->socket_fd);
        }
#endif
        
        auto thread_it = receive_threads.find(peer_id);
        if (thread_it != receive_threads.end() && thread_it->second.joinable()) {
            thread_it->second.join();
            receive_threads.erase(thread_it);
        }
        
        if (connection_handler) {
            connection_handler(peer_id, false);
        }
        
        LogInfo("ConnectionManager: DisconnectJoin: %s", peer_id.c_str());
    }
}

void ConnectionManager::DisconnectAll() {
    std::vector<std::string> peer_ids;
    
    {
        std::lock_guard<std::mutex> lock(connections_mutex);
        for (const auto& pair : connections) {
            peer_ids.push_back(pair.first);
        }
    }
    
    for (const auto& peer_id : peer_ids) {
        DisconnectPeer(peer_id);
    }
}

bool ConnectionManager::SendMessage(const std::string& peer_id, const std::vector<uint8_t>& data) {
    ConnectionInfo* conn_info = nullptr;
    
    {
        std::lock_guard<std::mutex> lock(connections_mutex);
        auto it = connections.find(peer_id);
        if (it == connections.end() || it->second->state != CONNECTED) {
            return false;
        }
        conn_info = it->second.get();
    }
    
    uint32_t length = static_cast<uint32_t>(data.size());
    std::vector<uint8_t> packet(sizeof(length) + data.size());
    memcpy(packet.data(), &length, sizeof(length));
    memcpy(packet.data() + sizeof(length), data.data(), data.size());
    
#ifdef WIN32
    int sent = send(conn_info->socket_fd, (const char*)packet.data(), packet.size(), 0);
#else
    ssize_t sent = send(conn_info->socket_fd, packet.data(), packet.size(), 0);
#endif
    
    if (sent > 0) {
        conn_info->bytes_sent += sent;
        conn_info->last_activity = std::chrono::steady_clock::now();
        return true;
    } else {
        LogInfo("ConnectionManager: SendMessageFailed: %s", peer_id.c_str());
        return false;
    }
}

void ConnectionManager::BroadcastMessage(const std::vector<uint8_t>& data) {
    std::vector<std::string> peer_ids;
    
    {
        std::lock_guard<std::mutex> lock(connections_mutex);
        for (const auto& pair : connections) {
            if (pair.second->state == CONNECTED) {
                peer_ids.push_back(pair.first);
            }
        }
    }
    
    for (const auto& peer_id : peer_ids) {
        SendMessage(peer_id, data);
    }
}

void ConnectionManager::SetMessageHandler(MessageHandler handler) {
    message_handler = handler;
}

void ConnectionManager::SetConnectionHandler(ConnectionHandler handler) {
    connection_handler = handler;
}

void ConnectionManager::SetLLMTokenCallback(LLMTokenCallback cb) {
    if (llm_handler.get()) {
        llm_handler->SetTokenCallback(cb);
    }
}

void ConnectionManager::SetLLMDoneCallback(LLMDoneCallback cb) {
    if (llm_handler.get()) {
        llm_handler->SetDoneCallback(cb);
    }
}

void ConnectionManager::SetLLMErrorCallback(LLMErrorCallback cb) {
    if (llm_handler.get()) {
        llm_handler->SetErrorCallback(cb);
    }
}

bool ConnectionManager::CheckAndRouteLLMMessage(const std::string& peer_id, const std::vector<uint8_t>& data) {
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

    ConnectionInfo* conn_info = nullptr;
    {
        std::lock_guard<std::mutex> lock(connections_mutex);
        auto it = connections.find(peer_id);
        if (it != connections.end()) {
            conn_info = it->second.get();
        }
    }

    int socket_fd = -1;
#ifdef WIN32
    if (conn_info && conn_info->socket_fd != INVALID_SOCKET) {
        socket_fd = static_cast<int>(conn_info->socket_fd);
    }
#else
    if (conn_info && conn_info->socket_fd >= 0) {
        socket_fd = conn_info->socket_fd;
    }
#endif

    llm_handler->HandleIncomingMessage(peer_id, data, socket_fd);
    return true;
}

size_t ConnectionManager::GetConnectionCount() const {
    std::lock_guard<std::mutex> lock(connections_mutex);
    return connections.size();
}

std::vector<std::string> ConnectionManager::GetConnectedPeers() const {
    std::vector<std::string> peers;
    std::lock_guard<std::mutex> lock(connections_mutex);
    for (const auto& pair : connections) {
        if (pair.second->state == CONNECTED) {
            peers.push_back(pair.first);
        }
    }
    return peers;
}

const ConnectionInfo* ConnectionManager::GetConnectionInfo(const std::string& peer_id) const {
    std::lock_guard<std::mutex> lock(connections_mutex);
    auto it = connections.find(peer_id);
    if (it != connections.end()) {
        return it->second.get();
    }
    return nullptr;
}

void ConnectionManager::AcceptLoop() {
    while (running.load()) {
        if (!HandleNewConnection()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

bool ConnectionManager::HandleNewConnection() {
    sockaddr_in client_addr;
#ifdef WIN32
    int addr_len = sizeof(client_addr);
    SOCKET client_socket = accept(server_socket, (sockaddr*)&client_addr, &addr_len);
    if (client_socket == INVALID_SOCKET) {
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
            LogInfo("ConnectionManager: Accept failed, error code: %d", error);
        }
        return false;
    }
#else
    socklen_t addr_len = sizeof(client_addr);
    int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &addr_len);
    if (client_socket < 0) {
#if EWOULDBLOCK != EAGAIN
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
#else
        if (errno != EAGAIN) {
#endif
            LogInfo("ConnectionManager: Accept connection failed: %s", strerror(errno));
        }
        return false;
    }
#endif
    
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
    uint16_t client_port = ntohs(client_addr.sin_port);
    
    std::string peer_id = GeneratePeerId(ip_str, client_port);
    
    {
        std::lock_guard<std::mutex> lock(connections_mutex);
        if (connections.find(peer_id) != connections.end()) {
#ifdef WIN32
            closesocket(client_socket);
#else
            close(client_socket);
#endif
            return true;
        }
        
        if (connections.size() >= MAX_CONNECTIONS) {
            LogInfo("ConnectionManager: Max connections reached, rejecting new connection");
#ifdef WIN32
            closesocket(client_socket);
#else
            close(client_socket);
#endif
            return true;
        }
        
        auto conn_info = std::make_unique<ConnectionInfo>();
        conn_info->peer_id = peer_id;
        conn_info->address = ip_str;
        conn_info->port = client_port;
        conn_info->state = CONNECTED;
        conn_info->last_activity = std::chrono::steady_clock::now();
        conn_info->socket_fd = client_socket;
        
        connections[peer_id] = std::move(conn_info);
    }
    
    receive_threads[peer_id] = std::thread(&ConnectionManager::ReceiveLoop, this, peer_id);
    
    if (connection_handler) {
        connection_handler(peer_id, true);
    }
    
    LogInfo("ConnectionManager: Accepted new connection: %s (%s:%d)", 
            peer_id.c_str(), ip_str, client_port);
    
    return true;
}

void ConnectionManager::ReceiveLoop(const std::string& peer_id) {
    std::vector<uint8_t> buffer(BUFFER_SIZE);
    std::vector<uint8_t> message_buffer;
    
    while (running.load()) {
        ConnectionInfo* conn_info = nullptr;
        
        {
            std::lock_guard<std::mutex> lock(connections_mutex);
            auto it = connections.find(peer_id);
            if (it == connections.end() || it->second->state != CONNECTED) {
                break;
            }
            conn_info = it->second.get();
        }
        
#ifdef WIN32
        int received = recv(conn_info->socket_fd, (char*)buffer.data(), buffer.size(), 0);
#else
        ssize_t received = recv(conn_info->socket_fd, buffer.data(), buffer.size(), 0);
#endif
        
        if (received <= 0) {
#ifdef WIN32
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK) {
                LogInfo("ConnectionManager: Receive error or peer disconnected: %s (Error Code: %d)", 
                        peer_id.c_str(), error);
                DisconnectPeer(peer_id);
                break;
            }
#else
#if EWOULDBLOCK != EAGAIN
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
#else
            if (errno != EAGAIN) {
#endif
                LogInfo("ConnectionManager: Receive error or peer disconnected: %s (%s)", 
                        peer_id.c_str(), strerror(errno));
                DisconnectPeer(peer_id);
                break;
            }
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        conn_info->bytes_received += received;
        conn_info->last_activity = std::chrono::steady_clock::now();
        
        message_buffer.insert(message_buffer.end(), buffer.begin(), buffer.begin() + received);
        
        ProcessReceivedData(peer_id, message_buffer);
    }
}

void ConnectionManager::ProcessReceivedData(const std::string& peer_id, std::vector<uint8_t>& buffer) {
    size_t offset = 0;
    
    while (offset + sizeof(uint32_t) <= buffer.size()) {
        uint32_t message_length;
        memcpy(&message_length, buffer.data() + offset, sizeof(message_length));
        
        if (message_length > BUFFER_SIZE || message_length == 0) {
            LogInfo("ConnectionManager: Invalid message length: %u", message_length);
            DisconnectPeer(peer_id);
            buffer.clear();
            return;
        }
        
        if (offset + sizeof(uint32_t) + message_length > buffer.size()) {
            break;
        }
        
        std::vector<uint8_t> message_data(buffer.data() + offset + sizeof(uint32_t),
                                           buffer.data() + offset + sizeof(uint32_t) + message_length);

        if (!CheckAndRouteLLMMessage(peer_id, message_data)) {
            if (message_handler) {
                message_handler(peer_id, message_data);
            }
        }
        
        offset += sizeof(uint32_t) + message_length;
    }
    
    if (offset > 0) {
        buffer.erase(buffer.begin(), buffer.begin() + offset);
    }
}

void ConnectionManager::HeartbeatLoop() {
    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_INTERVAL_SECONDS));
        
        if (!running.load()) break;
        
        auto now = std::chrono::steady_clock::now();
        std::vector<std::string> timeout_peers;
        
        {
            std::lock_guard<std::mutex> lock(connections_mutex);
            for (const auto& pair : connections) {
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(
                    now - pair.second->last_activity).count();
                
                if (duration > CONNECTION_TIMEOUT_SECONDS) {
                    timeout_peers.push_back(pair.first);
                }
            }
        }
        
        for (const auto& peer_id : timeout_peers) {
            LogInfo("ConnectionManager: JoinTimeout, disconnect: %s", peer_id.c_str());
            DisconnectPeer(peer_id);
        }
    }
}

std::string ConnectionManager::GeneratePeerId(const std::string& address, uint16_t port) {
    return address + ":" + std::to_string(port);
}
