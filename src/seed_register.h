#ifndef TKN_SEED_REGISTER_H
#define TKN_SEED_REGISTER_H

#include <string>
#include <vector>
#include <cstdint>

namespace node { struct NodeContext; }

enum class NodeRole {
    NODE,
    MINER,
    CLIENT
};

struct NodeRegistrationInfo {
    std::string public_ip;
    NodeRole role = NodeRole::NODE;
    std::vector<std::string> capabilities;
    int p2p_port = 9333;
    int ws_port = 9332;
    std::string wallet_address;
    std::string model_name;
};

void RegisterNodeToSeed(const NodeRegistrationInfo& info);
void StartSeedRegisterThread();
void QueryPeersFromSeed(const std::string& role_filter, const std::string& capability_filter);
void SendHeartbeatToSeed();
void UnregisterFromSeed();
void SetNodeRole(NodeRole role);
NodeRole DetectNodeRole();
void StartP2PMaintenanceThread();
void SetNodeContext(node::NodeContext* ctx);
void SetMinerWalletAddress(const std::string& addr);
void SetMinerModelName(const std::string& name);
std::string GetMinerModelName();

void SignalMinerActive();
int64_t GetMinerLastActive();

#endif // TKN_SEED_REGISTER_H
