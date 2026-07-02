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

// Local IPv6→IPv4 Proxy: IDE → 127.0.0.1:<listen_port> → [target_ipv6]:<target_port> (transparent, no auth/billing).

/** Start local IPv6→IPv4 proxy.
 *  @param listen_port   Local port to listen on (e.g., 9393)
 *  @param target_ipv6   Remote IPv6 address (no brackets)
 *  @param target_port   Remote port (e.g., 9313)
 *  @returns true if proxy started successfully
 */
bool StartInferProxy(int listen_port, const std::string& target_ipv6, int target_port);

/** Stop local IPv6→IPv4 proxy (breaks event loop, joins thread, frees resources). */
void StopInferProxy();

/** Inference proxy configuration snapshot. */
struct InferProxyConfig {
    bool running;
    int listen_port;
    std::string target_ipv6;
    int target_port;
};

/** Get current inference proxy configuration (thread-safe). */
struct InferProxyConfig GetInferProxyConfig();

#endif // TKN_INFERENCE_GATEWAY_H
