// Copyright (c) 2026-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TKN_INFERENCE_GATEWAY_H
#define TKN_INFERENCE_GATEWAY_H

#include <any>
#include <string>

class JSONRPCRequest;

/** OpenAI-compatible HTTP API Gateway for TKNC node (port 9313, standalone libevent, independent from RPC port 9331).
 *  Architecture: External User → Node(:9313)/v1/chat/completions → Miner(127.0.0.1:9332).
 *  Seed server (66.154.101.183) is display-only, never provides API. */

/** Start Inference Gateway (standalone libevent HTTP on port 9313, independent from RPC server). */
bool StartInferenceGateway(const std::any& context);

/** Interrupt Inference Gateway (breaks event loop). */
void InterruptInferenceGateway();

/** Stop Inference Gateway (joins thread, frees resources). */
void StopInferenceGateway();

// Local IPv6→IPv4 Proxy: IDE → 127.0.0.1:<listen_port> → [target_ip]:<target_port> (transparent, no auth/billing).
// The proxy auto-starts on node boot. Target IP is set dynamically via RPC (tknc_setinferproxytarget).

/** Start local inference proxy (auto-started on node boot, no target needed).
 *  @param listen_port   Local port to listen on (e.g., 9393, configurable via tknc.conf)
 *  @returns true if proxy started successfully
 */
bool StartInferProxy(int listen_port);

/** Dynamically set proxy target (called when user configures via CLI/RPC).
 *  @param target_ip    Remote miner IP (IPv4 or IPv6, no brackets)
 *  @param target_port  Remote port (e.g., 9313)
 *  @returns true if target set successfully
 */
bool SetInferProxyTarget(const std::string& target_ip, int target_port);

/** Stop local inference proxy (breaks event loop, joins thread, frees resources). */
void StopInferProxy();

/** Inference proxy configuration snapshot. */
struct InferProxyConfig {
    bool running;
    int listen_port;
    std::string target_ip;      // Remote miner IP (IPv4 or IPv6), empty if not set
    int target_port;
    bool target_set;            // Whether target has been configured via RPC
};

/** Get current inference proxy configuration (thread-safe). */
struct InferProxyConfig GetInferProxyConfig();

#endif // TKN_INFERENCE_GATEWAY_H
