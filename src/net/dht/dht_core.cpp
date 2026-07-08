#include <net/dht/dht_core.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <iostream>
#include <random>

static std::unique_ptr<DHTCore> g_dht;

std::unique_ptr<DHTCore>& GetGlobalDHT() {
    if (!g_dht) {
        g_dht = std::make_unique<DHTCore>();
    }
    return g_dht;
}

void DHTCore::Initialize(const CPubKey& pubkey,
                         std::shared_ptr<DHTNetworkInterface> net_interface) {
    local_node_id = NodeID::FromPublicKey(pubkey);
    network_interface = net_interface;
    
    std::cout << "DHT Core initialized with NodeID: " << local_node_id.ToString() << std::endl;
}

void DHTCore::Start() {
    running.store(true);
    std::cout << "DHT Core started" << std::endl;
}

void DHTCore::Stop() {
    running.store(false);
    routing_table.Clear();
    std::cout << "DHT Core stopped" << std::endl;
}

std::vector<NodeInfo> DHTCore::FindClosestNodes(const NodeID& target, int k) {
    // Find from local routing table
    auto closest = routing_table.FindNearest(target, k);
    
    // If routing table is empty or insufficient nodes, return empty list
    if (closest.empty()) {
        return closest;
    }
    
    // If network interface available, try sending FIND_NODE request to these nodes
    // Simplified implementation, using local routing table data only
    return closest;
}

std::vector<NodeInfo> DHTCore::LookupIteration(LookupState& state) {
    // Select alpha closest unqueried nodes
    std::vector<NodeInfo> to_query;
    for (const auto& node : state.closest_nodes) {
        if (state.queried_nodes.count(node.node_id) == 0 && 
            to_query.size() < static_cast<size_t>(state.alpha)) {
            to_query.push_back(node);
        }
    }
    
    // Mark as queried
    for (const auto& node : to_query) {
        state.queried_nodes.insert(node.node_id);
        
        // Send FIND_NODE request (if network interface available)
        if (network_interface) {
            network_interface->SendFindNode(node, state.target);
        }
    }
    
    // Return current closest nodes
    return state.closest_nodes;
}

void DHTCore::FindClosestNodesAsync(const NodeID& target,
                                   DHTLookupCallback callback,
                                   int k) {
    // Simplified: call sync version and invoke callback
    auto result = FindClosestNodes(target, k);
    if (callback) {
        callback(result);
    }
}

void DHTCore::HandleFindNodeResponse(const NodeID& from_node,
                                    const std::vector<NodeInfo>& found_nodes) {
    // Add discovered nodes to routing table
    for (const auto& node : found_nodes) {
        routing_table.AddNode(node);
    }
    
    // Update responsiveness of responding node
    routing_table.UpdateNodeActivity(from_node);
}

std::vector<NodeInfo> DHTCore::HandleFindNodeRequest(const NodeID& target) {
    // Find K closest nodes to target and return
    return routing_table.FindNearest(target);
}

void DHTCore::AddBootstrapNodes(const std::vector<NodeInfo>& nodes) {
    for (const auto& node : nodes) {
        routing_table.AddNode(node);
    }
    
    std::cout << "Added " << nodes.size() << " bootstrap nodes to routing table" << std::endl;
}

void DHTCore::RefreshRoutingTable() {
    // Randomly select target ID for lookup to refresh routing table
    uint8_t random_bytes[20];
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    for (int i = 0; i < 20; i++) {
        random_bytes[i] = static_cast<uint8_t>(dis(gen));
    }
    NodeID random_target(random_bytes);
    
    // Execute lookup
    FindClosestNodes(random_target);
    
    // Clean up expired nodes
    routing_table.RemoveInactiveNodes();
}

void DHTCore::Cleanup() {
    routing_table.RemoveInactiveNodes();
}
