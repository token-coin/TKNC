#ifndef TKN_NET_LOCAL_BACKEND_H
#define TKN_NET_LOCAL_BACKEND_H

#include <net/compute_backend.h>
#include <string>
#include <cstdint> // for uint16_t (Linux compatibility)

/**
 * LocalBackend ?Wraps the local miner's HTTP API as a ComputeBackend.
 *
 * Transport: HTTP POST to localhost:9332/api/v1/chat
 * This is the CURRENT working path (proven by Test-A).
 *
 * Configuration:
 * Host: configurable (default 127.0.0.1)
 * Port: configurable (default 9332)
 * Connect timeout: 2 seconds
 *
 * This backend does NOT know about miners, wallets, or P2P.
 * It only knows about an HTTP endpoint that returns LLM responses.
 */
class LocalBackend final : public ComputeBackend {
public:
 explicit LocalBackend(
 const std::string& host = "127.0.0.1",
 uint16_t port = 9332);

 // --- ComputeBackend interface ---
 std::string GetId() const override { return "local"; }
 std::string GetName() const override { return "Local IPC Miner"; }
 ComputeResponse Infer(const ComputeRequest& request) override;
 HealthStatus CheckHealth() const override;
 int GetPriority() const override { return 100; } // Highest priority (local = fastest)

 // --- Local-specific config ---
 void SetHost(const std::string& host) { m_host = host; }
 void SetPort(uint16_t port) { m_port = port; }
 std::string GetHost() const { return m_host; }
 uint16_t GetPort() const { return m_port; }

private:
 std::string m_host;
 uint16_t m_port;

 static std::string EscapeJson(const std::string& raw);
};

#endif // TKN_NET_LOCAL_BACKEND_H
