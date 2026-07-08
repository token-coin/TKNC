#ifndef TKN_NET_DHT_CORE_H
#define TKN_NET_DHT_CORE_H

#include <net/dht/routing_table.h>
#include <pubkey.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>

using DHTLookupCallback = std::function<void(const std::vector<NodeInfo>&)>;

class DHTNetworkInterface {
public:
    virtual ~DHTNetworkInterface() = default;
    
    virtual bool SendFindNode(const NodeInfo& target, const NodeID& search_id) = 0;
    
    virtual bool SendPing(const NodeInfo& target) = 0;
    
    virtual NodeID GetLocalNodeID() const = 0;
};

class DHTCore {
private:
    RoutingTable routing_table;
    NodeID local_node_id;
    std::shared_ptr<DHTNetworkInterface> network_interface;
    
    std::atomic<bool> running{false};
    
    struct LookupState {
        NodeID target;
        std::vector<NodeInfo> closest_nodes;
        std::set<NodeID> queried_nodes;
        int alpha;
        int max_iterations;
    };
    
    std::vector<NodeInfo> LookupIteration(LookupState& state);
    
public:
    DHTCore() {}
    
    void Initialize(const CPubKey& pubkey, 
                   std::shared_ptr<DHTNetworkInterface> net_interface);
    
    void Start();
    
    void Stop();
    
    std::vector<NodeInfo> FindClosestNodes(const NodeID& target, int k = 20);
    
    void FindClosestNodesAsync(const NodeID& target, 
                              DHTLookupCallback callback,
                              int k = 20);
    
    void HandleFindNodeResponse(const NodeID& from_node,
                               const std::vector<NodeInfo>& found_nodes);
    
    std::vector<NodeInfo> HandleFindNodeRequest(const NodeID& target);
    
    void AddBootstrapNodes(const std::vector<NodeInfo>& nodes);
    
    void RefreshRoutingTable();
    
    void Cleanup();
    
    size_t GetRoutingTableSize() const { return routing_table.GetNodeCount(); }
    
    NodeID GetLocalNodeID() const { return local_node_id; }
    
    bool IsRunning() const { return running.load(); }
};

std::unique_ptr<DHTCore>& GetGlobalDHT();

#endif // TKN_NET_DHT_CORE_H
