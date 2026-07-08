#ifndef TKN_NET_DHT_ROUTING_TABLE_H
#define TKN_NET_DHT_ROUTING_TABLE_H

#include <net/dht/node_id.h>
#include <uint256.h>
#include <list>
#include <vector>
#include <mutex>
#include <chrono>
#include <string>

struct NodeInfo {
    NodeID node_id;
    std::string ip_address;
    uint16_t port;
    std::chrono::steady_clock::time_point last_seen;
    int failed_count;
    
    NodeInfo() : port(0), failed_count(0) {}
    
    NodeInfo(const NodeID& id, const std::string& ip, uint16_t p)
        : node_id(id), ip_address(ip), port(p), failed_count(0) {
        last_seen = std::chrono::steady_clock::now();
    }
    
    bool IsExpired() const {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::minutes>(now - last_seen);
        return duration.count() > 15;
    }
    
    void UpdateLastSeen() {
        last_seen = std::chrono::steady_clock::now();
        failed_count = 0;
    }
};

class RoutingTable {
private:
    static constexpr int BUCKET_COUNT = 160;
    static constexpr int K_PARAM = 20;
    
    std::list<NodeInfo> buckets[BUCKET_COUNT];
    mutable std::mutex mutex;
    
    int GetBucketIndex(const NodeID& target) const;
    
public:
    RoutingTable() {}
    
    void AddNode(const NodeInfo& node);
    
    void RemoveNode(const NodeID& node_id);
    
    std::vector<NodeInfo> FindNearest(const NodeID& target, int k = K_PARAM) const;
    
    void UpdateNodeActivity(const NodeID& node_id);
    
    void RemoveInactiveNodes();
    
    std::vector<NodeInfo> GetBucketNodes(int bucket_index) const;
    
    size_t GetNodeCount() const;
    
    void Clear();
    
    bool HasNode(const NodeID& node_id) const;
    
    std::vector<NodeInfo> GetRandomNodes(int count = K_PARAM) const;
};

#endif // TKN_NET_DHT_ROUTING_TABLE_H
