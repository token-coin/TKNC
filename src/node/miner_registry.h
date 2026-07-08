// Copyright (c) 2026 The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TKN_NODE_MINER_REGISTRY_H
#define TKN_NODE_MINER_REGISTRY_H

#include <string>
#include <cstdint>

namespace node {

// A2.7/A2.8 compliance: These functions handle external communication on behalf of miners.
// Miners must NOT communicate externally; the node handles all external tasks
// (public IP detection, web server registration, heartbeat).

/** Detect the public IP address for a miner. */
std::string DetectPublicIPForMiner(const std::string& wallet_address);

/** Register a miner to the web server. Called by the node on behalf of the miner. */
bool RegisterMinerToWeb(const std::string& wallet_address,
                        const std::string& public_ip,
                        const std::string& web_server_url,
                        const std::string& model_name,
                        const std::string& gpu_name,
                        int64_t gpu_vram_total_mb,
                        int64_t gpu_vram_used_mb,
                        double gpu_utilization,
                        int api_port);

/** Send a heartbeat to the web server for a miner. Called by the node on behalf of the miner.
 *  out_miner_reachable: if non-null, set to true when the miner HTTP server is reachable,
 *                      false otherwise. Used by the heartbeat loop to detect miner process
 *                      exit and terminate the loop (prevents zombie heartbeat threads).
 */
bool SendMinerHeartbeat(const std::string& wallet_address,
                        const std::string& web_server_url,
                        const std::string& public_ip,
                        const std::string& model_name,
                        double hashrate,
                        const std::string& gpu_name,
                        int64_t gpu_vram_total_mb,
                        int64_t gpu_vram_used_mb,
                        double gpu_utilization,
                        int64_t registration_time,
                        int api_port,
                        bool* out_miner_reachable = nullptr);

} // namespace node

#endif // TKN_NODE_MINER_REGISTRY_H