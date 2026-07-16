#ifndef TKN_NET_MINER_REGISTRY_H
#define TKN_NET_MINER_REGISTRY_H

#include <string>
#include <map>
#include <vector>
#include <cstdint>

/**
 * Local Miner Registry ?Node-Centric Discovery
 *
 * Architecture principle:
 * tkncd = only P2P participant
 * tknc-miner = pure local executor (no network)
 *
 * This registry is populated by IPC probing localhost:9332-9342,
 * NOT by P2P MINER_INFO messages from remote peers.
 *
 * When multiple miners run on the same machine, each binds to a different
 * port (9332, 9333, ...). The Probe() method scans all ports in the range
 * to discover every local miner.
 *
 * Truth sources (priority order):
 * 1. HTTP GET localhost:{9332-9342}/api/v1/miners ?JSON with "status":"online"
 * 2. Environment variable MINER_WALLET
 * 3. Nothing ?registry empty ?no inference available locally
 */
struct LocalMinerEntry {
 std::string wallet_address;
 std::string model_name;
 std::string gpu_name;
 uint16_t api_port = 9332;
 int64_t last_seen = 0; // Unix timestamp of last successful probe
 bool online = false;

 // Scoring factors for deterministic selection
 int model_match_score(const std::string& hint) const {
 if (hint.empty() || model_name.empty()) return 0;
 return (model_name.find(hint) != std::string::npos) ? 50 : 0;
 }

 int freshness_score(int64_t now) const {
 if (last_seen == 0) return 0;
 int age_secs = static_cast<int>(now - last_seen);
 if (age_secs < 0) age_secs = 0;
 // +30 base, decays 1 point per minute after 30min
 return std::max(0, 30 - age_secs / 60);
 }
};

class MinerLocalRegistry {
public:
 static constexpr int PROBE_INTERVAL_SECONDS = 30;
 static constexpr int MINER_TIMEOUT_SECONDS = 120; // 2 min no response = offline
 static constexpr const char* LOCAL_MINER_HOST = "127.0.0.1";
 static constexpr uint16_t DEFAULT_API_PORT = 9332;

 // Port range for multi-miner support: first miner binds 9332, second 9333, etc.
 // (see api_server.cpp port-retry logic)
 static constexpr uint16_t MINER_PORT_START = 9332;
 static constexpr uint16_t MINER_PORT_END = 9342; // supports up to 11 miners

 /**
 * Probe local miner API via HTTP GET /api/v1/miners.
 * Updates registry based on response.
 * Call this periodically (every PROBE_INTERVAL_SECONDS).
 */
 void Probe();

 /**
 * Check if any local miner is currently online.
 */
 bool HasOnlineMiner() const;

 /**
 * Get best matching local miner for a request.
 * @param model_hint Optional model name preference for scoring.
 * @return Pointer to entry (owned by registry, do NOT free), or nullptr if none available.
 */
 const LocalMinerEntry* SelectBest(const std::string& model_hint = "") const;

 /**
 * Get all currently registered local miners (for RPC/debug output).
 */
 std::vector<LocalMinerEntry> GetAll() const;

 /**
 * Get count of online miners.
 */
 size_t OnlineCount() const;

private:
 std::map<std::string, LocalMinerEntry> m_registry; // key = wallet_address

 // Internal: parse JSON response from /api/v1/miners endpoint.
 // @param json_body The HTTP response body.
 // @param port The port the response came from (stored in entry.api_port).
 // (Chinese comment removed)
 void ParseMinersResponse(const std::string& json_body, uint16_t port);
};

#endif // TKN_NET_MINER_REGISTRY_H
