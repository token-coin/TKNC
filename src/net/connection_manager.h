#ifndef TKN_CONNECTION_MANAGER_H
#define TKN_CONNECTION_MANAGER_H

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#endif

#include <util/log.h>

class P2PLLMPeerHandler;

enum ConnectionState {
    DISCONNECTED = 0,
    CONNECTING = 1,
    CONNECTED = 2,
    DISCONNECTING = 3,
    CONNECTION_ERROR = 4
};

struct ConnectionInfo {
    std::string peer_id;
    std::string address;
    uint16_t port;
    ConnectionState state;
    std::chrono::steady_clock::time_point last_activity;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    
#ifdef WIN32
    SOCKET socket_fd;
#else
    int socket_fd;
#endif
    
    ConnectionInfo() 
        : port(0)
        , state(DISCONNECTED)
        , bytes_sent(0)
        , bytes_received(0)
#ifdef WIN32
        , socket_fd(INVALID_SOCKET)
#else
        , socket_fd(-1)
#endif
    {}
};

using MessageHandler = std::function<void(const std::string& peer_id, const std::vector<uint8_t>& data)>;
using ConnectionHandler = std::function<void(const std::string& peer_id, bool connected)>;
using LLMTokenCallback = std::function<void(const std::string& token, int index)>;
using LLMDoneCallback = std::function<void(const std::string& full_response, int total_tokens)>;
using LLMErrorCallback = std::function<void(int error_code, const std::string& error_msg)>;

class ConnectionManager {
public:
    static ConnectionManager& GetInstance();
    
    bool Initialize(uint16_t port = 9333);
    void Shutdown();
    
    bool ConnectToPeer(const std::string& address, uint16_t port);
    void DisconnectPeer(const std::string& peer_id);
    void DisconnectAll();
    
    bool SendMessage(const std::string& peer_id, const std::vector<uint8_t>& data);
    void BroadcastMessage(const std::vector<uint8_t>& data);
    
    void SetMessageHandler(MessageHandler handler);
    void SetConnectionHandler(ConnectionHandler handler);

    P2PLLMPeerHandler& GetLLMHandler() { return *llm_handler; }
    void SetLLMTokenCallback(LLMTokenCallback cb);
    void SetLLMDoneCallback(LLMDoneCallback cb);
    void SetLLMErrorCallback(LLMErrorCallback cb);

    size_t GetConnectionCount() const;
    std::vector<std::string> GetConnectedPeers() const;
    const ConnectionInfo* GetConnectionInfo(const std::string& peer_id) const;
    
    bool IsRunning() const { return running.load(); }
    uint16_t GetLocalPort() const { return local_port; }
    
private:
    ConnectionManager();
    ~ConnectionManager();
    
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;
    
    void AcceptLoop();
    void ReceiveLoop(const std::string& peer_id);
    void HeartbeatLoop();
    
    bool HandleNewConnection();
    void ProcessReceivedData(const std::string& peer_id, std::vector<uint8_t>& buffer);
    
    std::string GeneratePeerId(const std::string& address, uint16_t port);
    
#ifdef WIN32
    SOCKET server_socket;
#else
    int server_socket;
#endif
    
    uint16_t local_port;
    std::atomic<bool> running;
    std::atomic<bool> initialized;
    
    mutable std::mutex connections_mutex;
    std::map<std::string, std::unique_ptr<ConnectionInfo>> connections;
    
    std::thread accept_thread;
    std::map<std::string, std::thread> receive_threads;
    std::thread heartbeat_thread;
    
    MessageHandler message_handler;
    ConnectionHandler connection_handler;

    std::unique_ptr<P2PLLMPeerHandler> llm_handler;
    bool CheckAndRouteLLMMessage(const std::string& peer_id, const std::vector<uint8_t>& data);

    static constexpr int MAX_CONNECTIONS = 125;
    static constexpr int HEARTBEAT_INTERVAL_SECONDS = 30;
    static constexpr int CONNECTION_TIMEOUT_SECONDS = 120;
    static constexpr int BUFFER_SIZE = 65536;
};

#endif // TKN_CONNECTION_MANAGER_H
