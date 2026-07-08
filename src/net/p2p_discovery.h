#ifndef TKN_P2P_DISCOVERY_H
#define TKN_P2P_DISCOVERY_H

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <map>
#include <net/p2p_llm.h>
#include <netbase.h>

inline const char* SEED_HOST = "66.154.101.183";
static const int SEED_HTTP_PORT = 80;

struct PeerInfo {
    std::string node_id;
    std::string address;
    std::string role;
    std::vector<std::string> capabilities;
    std::string public_ip;
    int p2p_port;
    long last_seen;
    bool connected;

    PeerInfo()
        : p2p_port(0)
        , last_seen(0)
        , connected(false)
    {}
};

using PeerDiscoveredCallback = std::function<void(const PeerInfo&)>;
using PeerConnectedCallback = std::function<void(const PeerInfo&, void*)>;

class P2PDiscovery {
public:
    static P2PDiscovery& GetInstance();

    void SetRole(const std::string& role);
    void SetLocalPort(int port);
    void SetWallet(const std::string& wallet);
    void SetWsPort(int port);

    void StartDiscovery();
    void StopDiscovery();

    void OnPeerDiscovered(PeerDiscoveredCallback cb);
    void OnPeerConnected(PeerConnectedCallback cb);

    std::vector<PeerInfo> GetConnectedPeers() const;
    std::vector<PeerInfo> GetMinerPeers() const;

private:
    P2PDiscovery();
    ~P2PDiscovery();

    P2PDiscovery(const P2PDiscovery&) = delete;
    P2PDiscovery& operator=(const P2PDiscovery&) = delete;

    void DiscoveryLoop();
    void ConnectToPeer(const PeerInfo& peer);
    void HeartbeatLoop();

    bool SendHandshake(void* bev, const PeerInfo& local_info);
    bool ProcessHeartbeatResponse(const std::vector<uint8_t>& data);
    void DisconnectTimedOutPeer(const std::string& peer_id);

    void RegisterWithSeedServer();
    bool FetchPeersFromSeed(std::vector<PeerInfo>& peers);
    void ParsePeerListJson(const std::string& json, std::vector<PeerInfo>& peers);

    void QuerySeedForPeers();
    void RegisterWithSeed();

    bool CheckAndRouteLLMMessage(const std::string& peer_id, const std::vector<uint8_t>& data, void* bev);
    void HandleIncomingData(const std::string& peer_id, const std::vector<uint8_t>& data, void* bev);

    class Impl;
    std::unique_ptr<Impl> impl_;

    std::string role_;
    int local_port_;
    std::string local_wallet_;
    int ws_port_;
    std::atomic<bool> running_;

    mutable std::mutex peers_mutex_;
    std::map<std::string, PeerInfo> known_peers_;

    PeerDiscoveredCallback on_peer_discovered_;
    PeerConnectedCallback on_peer_connected_;

    std::unique_ptr<P2PLLMPeerHandler> llm_handler_;

    std::thread discovery_thread_;
    std::thread heartbeat_thread_;

    static constexpr int DISCOVERY_INTERVAL_SECONDS = 10;
    static constexpr int HEARTBEAT_INTERVAL_SECONDS = 30;
    static constexpr int CONNECTION_TIMEOUT_SECONDS = 15;
    static constexpr int MAX_PEER_RETRIES = 3;

    static constexpr uint32_t HEARTBEAT_MAGIC = 0x50424B00;
    static constexpr uint8_t MSG_PING = 0x01;
    static constexpr uint8_t MSG_PONG = 0x02;

    struct HeartbeatMessage {
        uint32_t magic;
        uint8_t type;
        uint64_t timestamp;

        std::vector<uint8_t> Serialize() const;
        bool Deserialize(const std::vector<uint8_t>& data);
    };
};

#endif
