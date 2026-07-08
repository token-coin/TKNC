#include <net/dht/routing_table.h>
#include <algorithm>
#include <random>

int RoutingTable::GetBucketIndex(const NodeID& target) const {
    // Calculate XOR distance between target node and local node
    // Find first different bit to determine bucket index
    for (int i = 0; i < 20; i++) {
        if (target.GetData()[i] != 0) {
            unsigned char byte = target.GetData()[i];
            int leading_zeros = 0;
            while ((byte & 0x80) == 0 && leading_zeros < 8) {
                byte <<= 1;
                leading_zeros++;
            }
            return (19 - i) * 8 + (7 - leading_zeros);
        }
    }
    return 159;  // All-zero case goes to last bucket
}

void RoutingTable::AddNode(const NodeInfo& node) {
    std::lock_guard<std::mutex> lock(mutex);
    
    int bucket_index = GetBucketIndex(node.node_id);
    
    // Check if node already exists
    for (auto& existing_node : buckets[bucket_index]) {
        if (existing_node.node_id == node.node_id) {
            // Update last-seen time of existing node
            existing_node.UpdateLastSeen();
            existing_node.ip_address = node.ip_address;
            existing_node.port = node.port;
            return;
        }
    }
    
    // Bucket not full, add directly
    if (buckets[bucket_index].size() < K_PARAM) {
        buckets[bucket_index].push_back(node);
        return;
    }
    
    // Bucket full, check if expired node can be replaced
    for (auto it = buckets[bucket_index].begin(); it != buckets[bucket_index].end(); ++it) {
        if (it->IsExpired()) {
            buckets[bucket_index].erase(it);
            buckets[bucket_index].push_back(node);
            return;
        }
    }
    
    // All nodes active, discard new node (or replace least responsive)
    // Simple strategy: do not add
}

void RoutingTable::RemoveNode(const NodeID& node_id) {
    std::lock_guard<std::mutex> lock(mutex);
    
    for (int i = 0; i < BUCKET_COUNT; i++) {
        for (auto it = buckets[i].begin(); it != buckets[i].end(); ++it) {
            if (it->node_id == node_id) {
                buckets[i].erase(it);
                return;
            }
        }
    }
}

std::vector<NodeInfo> RoutingTable::FindNearest(const NodeID& target, int k) const {
    std::lock_guard<std::mutex> lock(mutex);
    
    std::vector<std::pair<uint256, NodeInfo>> candidates;
    
    // Collect all nodes and calculate distances
    for (int i = 0; i < BUCKET_COUNT; i++) {
        for (const auto& node : buckets[i]) {
            uint256 distance = target.Distance(node.node_id);
            candidates.emplace_back(distance, node);
        }
    }
    
    // Sort by distance (smaller XOR = closer)
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) {
                  return a.first < b.first;
              });
    
    // Return K closest nodes
    std::vector<NodeInfo> result;
    int count = std::min(k, static_cast<int>(candidates.size()));
    for (int i = 0; i < count; i++) {
        result.push_back(candidates[i].second);
    }
    
    return result;
}

void RoutingTable::UpdateNodeActivity(const NodeID& node_id) {
    std::lock_guard<std::mutex> lock(mutex);
    
    for (int i = 0; i < BUCKET_COUNT; i++) {
        for (auto& node : buckets[i]) {
            if (node.node_id == node_id) {
                node.UpdateLastSeen();
                return;
            }
        }
    }
}

void RoutingTable::RemoveInactiveNodes() {
    std::lock_guard<std::mutex> lock(mutex);
    
    for (int i = 0; i < BUCKET_COUNT; i++) {
        buckets[i].remove_if([](const NodeInfo& node) {
            return node.IsExpired();
        });
    }
}

std::vector<NodeInfo> RoutingTable::GetBucketNodes(int bucket_index) const {
    std::lock_guard<std::mutex> lock(mutex);
    
    if (bucket_index < 0 || bucket_index >= BUCKET_COUNT) {
        return {};
    }
    
    return std::vector<NodeInfo>(buckets[bucket_index].begin(), 
                                 buckets[bucket_index].end());
}

size_t RoutingTable::GetNodeCount() const {
    std::lock_guard<std::mutex> lock(mutex);
    
    size_t count = 0;
    for (int i = 0; i < BUCKET_COUNT; i++) {
        count += buckets[i].size();
    }
    return count;
}

void RoutingTable::Clear() {
    std::lock_guard<std::mutex> lock(mutex);
    
    for (int i = 0; i < BUCKET_COUNT; i++) {
        buckets[i].clear();
    }
}

bool RoutingTable::HasNode(const NodeID& node_id) const {
    std::lock_guard<std::mutex> lock(mutex);
    
    for (int i = 0; i < BUCKET_COUNT; i++) {
        for (const auto& node : buckets[i]) {
            if (node.node_id == node_id) {
                return true;
            }
        }
    }
    return false;
}

std::vector<NodeInfo> RoutingTable::GetRandomNodes(int count) const {
    std::lock_guard<std::mutex> lock(mutex);
    
    std::vector<NodeInfo> all_nodes;
    for (int i = 0; i < BUCKET_COUNT; i++) {
        for (const auto& node : buckets[i]) {
            all_nodes.push_back(node);
        }
    }
    
    // Randomly shuffle
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(all_nodes.begin(), all_nodes.end(), g);
    
    // Return specified count of nodes
    int actual_count = std::min(count, static_cast<int>(all_nodes.size()));
    return std::vector<NodeInfo>(all_nodes.begin(), all_nodes.begin() + actual_count);
}
