#ifndef TKN_NET_ENDPOINT_REGISTRY_H
#define TKN_NET_ENDPOINT_REGISTRY_H

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>

// ============================================================
// EndpointEntry — A registered node endpoint
// ============================================================
struct EndpointEntry {
    std::string node_id;            // Unique node identifier (e.g., wallet address)
    std::string ip;                 // Public IP address
    uint16_t p2p_port = 9333;       // P2P listening port
    uint16_t api_port = 9332;       // Miner API port (if local miner exists)
    std::string model_name;         // Model this node can serve (empty = unknown)

    int64_t conn_node_id = -1;      // CConnman CNode::GetId() for direct SendCallback routing (-1 = unknown)

    int64_t registered_at = 0;      // When we first learned about this endpoint
    int64_t last_seen = 0;          // Last time we received a heartbeat/advert from it
    bool self_reported = false;     // True if this is our own entry

    // Reachability info (populated after hole punch attempt)
    bool reachable_directly = false;
    bool reachable_via_punch = false;
    bool reachable_via_tcp = false;

    // Quality score for routing decisions (higher = better)
    int quality_score = 0;

    /** Time since last seen in seconds */
    int64_t AgeSeconds() const;

    /** Check if entry is still considered valid */
    bool IsValid(int64_t max_age_seconds = 300) const;

    std::string ToString() const;
};

// ============================================================
// EndpointRegistry — Global registry of known node endpoints
//
// Purpose:
//   Maps miner_id (wallet address) to network endpoint (ip:port).
//   This enables RemoteBackend to resolve a target peer's address
//   before sending APIREQ messages.
//
// Data sources:
//   1. Self-registration: our own endpoint advertised via FINDNODE response
//   2. Peer advertisements: remote peers send their endpoint in version/handshake
//   3. Seed node exchange: bootstrap peers provide initial endpoint list
//   4. P2P gossip: nodes forward endpoint info in MINER_INFO messages
//
// Integration:
//   - CConnman populates on new peer connections
//   - RemoteBackend queries before routing inference requests
//   - Periodic cleanup removes stale entries
// ============================================================

class EndpointRegistry {
public:
    EndpointRegistry();
    ~EndpointRegistry() = default;

    // --- Registration ---

    /**
     * Register or update a node endpoint.
     * Called when we learn about a peer's public address.
     */
    void Register(const EndpointEntry& entry);
    void Register(const std::string& node_id, const std::string& ip,
                  uint16_t p2p_port, uint16_t api_port = 9332);

    /** Remove a node from the registry */
    void Unregister(const std::string& node_id);

    // --- Lookup ---

    /** Find endpoint by node_id (miner wallet address) */
    const EndpointEntry* Find(const std::string& node_id) const;
    EndpointEntry* Find(const std::string& node_id);

    /** Find all nodes that serve a specific model */
    std::vector<const EndpointEntry*> FindByModel(const std::string& model_hint) const;

    /** Get our own registered endpoint (if any) */
    const EndpointEntry* SelfEndpoint() const { return m_self_entry.get(); }

    // --- Queries ---

    size_t TotalCount() const;
    size_t OnlineCount(int64_t max_age_seconds = 300) const;

    /** Get all reachable endpoints (direct + punch + tcp) */
    std::vector<const EndpointEntry*> GetReachableEndpoints() const;

    /**
     * Select best endpoint for an inference request.
     * Considers: reachability, quality score, latency, model match.
     */
    const EndpointEntry* SelectBest(const std::string& model_hint = "") const;

    // --- Maintenance ---

    /** Remove entries older than max_age_seconds. Returns count removed. */
    int CleanupStaleEntries(int64_t max_age_seconds = 300);

    /** Mark ourselves as online with given endpoint */
    void SetSelfEndpoint(const std::string& ip, uint16_t p2p_port, uint16_t api_port = 9332);

    /** Update reachability status for an endpoint (after punch attempt) */
    void UpdateReachability(const std::string& node_id, bool direct, bool punch, bool tcp);

    /** Update quality score (called after successful/failed request) */
    void UpdateQualityScore(const std::string& node_id, int delta);

    // --- Status ---
    std::string GetStatusSummary() const;

private:
    mutable std::mutex m_mutex;
    std::map<std::string, EndpointEntry> m_entries;  // node_id -> entry
    std::unique_ptr<EndpointEntry> m_self_entry;      // Our own entry

    static int64_t NowSeconds();
};

#endif // TKN_NET_ENDPOINT_REGISTRY_H
