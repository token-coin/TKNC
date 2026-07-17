#ifndef TKN_NET_P2P_REACHABILITY_H
#define TKN_NET_P2P_REACHABILITY_H

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <chrono>

// ============================================================
// NAT Type Classification (RFC 3489 / RFC 5389)
// ============================================================
enum class NATType : int {
 UNKNOWN = 0,
 PUBLIC,
 FULL_CONE, // Full cone NAT: any external host can send to mapped port
 RESTRICTED_CONE, // Restricted cone: only previously-contacted IP can send back
 PORT_RESTRICTED, // Port-restricted: only previously-contacted IP:port can send back
 SYMMETRIC // Symmetric NAT: each destination gets different mapping (hardest)
};

const char* NatTypeToString(NATType type);

// ============================================================
// Hole Punching Result
// ============================================================
enum class PunchResult : int {
 SUCCESS = 0,
 FAILED_UDP,
 FAILED_TCP_FALLBACK,
 TIMEOUT,
 NOT_ATTEMPTED,
 ALREADY_CONNECTED
};

struct PunchOutcome {
 PunchResult result = PunchResult::NOT_ATTEMPTED;
 std::string local_addr; // "local_ip:port"
 std::string remote_addr; // "remote_ip:port" (the address we reached)
 double latency_ms = 0.0;
 std::string error_message;
 bool tcp_fallback_used = false;
};

// ============================================================
// Reachability Config
// ============================================================
struct ReachabilityConfig {
 // STUN server for NAT detection (optional; uses seed nodes if empty)
 std::string stun_server;
 uint16_t stun_port = 3478;

 // UDP hole punch parameters
 int udp_punch_attempts = 5;
 int udp_punch_interval_ms = 500;
 int udp_punch_timeout_ms = 10000;

 // TCP fallback parameters
 int tcp_connect_timeout_ms = 8000;
 int tcp_keepalive_interval_s = 30;

 // Session parameters
 int session_heartbeat_interval_s = 15;
 int session_max_idle_s = 120;

 // Discovery: how often to broadcast our endpoint to peers
 int endpoint_broadcast_interval_s = 60;

 static ReachabilityConfig Default();
};

// ============================================================
//
// Architecture:
//
// Integration points:
// - Called by CConnman when a new peer connection is attempted
// - Called by RemoteBackend to establish path before sending APIREQ
// - Endpoint info exchanged via existing FINDNODE/NODES P2P messages
//
// No external dependencies:
// - Uses raw sockets (Winsock on Windows, BSD sockets on Linux)
// - Uses seed nodes as bootstrap exchange (no central STUN required)
// - Coordinates via existing P2P message channel (CConnman::PushMessage)
// ============================================================
class P2PReachability {
public:
 explicit P2PReachability(const ReachabilityConfig& config = ReachabilityConfig::Default());
 ~P2PReachability();

 // --- NAT Detection ---

 /**
 * Classify local NAT type using STUN-like probing.
 *
 * Strategy (no external STUN server):
 * Phase 1: Bind two UDP sockets ?compare external mappings via seed peer echo
 * Phase 2: If mappings differ ?SYMMETRIC
 * Phase 3: Send from socket A to seed ?receive response on both sockets
 * If B receives ?FULL_CONE
 * If only A receives ?RESTRICTED or PORT_RESTRICTED
 * Phase 4: If no NAT detected (external == local) ?PUBLIC
 *
 * @param seed_peer_addr Address of a known peer for echo testing (e.g., seed node)
 * @return Detected NAT type
 */
 NATType DetectNATType(const std::string& seed_peer_addr, uint16_t seed_peer_port);
 NATType GetDetectedNATType() const { return m_nat_type; }

 /** Quick check: can we likely establish direct connections? */
 bool CanDirectConnect() const;

 // --- Hole Punching ---

 /**
 * Attempt coordinated UDP hole punching with a remote peer.
 *
 * Protocol:
 * 1. Both sides create UDP socket bound to their respective ports
 * 2. Both send "TKN_PUNCH_INIT <nonce>" to each other's public addr
 * 3. Upon receiving INIT, reply with "TKN_PUNCH_ACK <nonce>"
 * 4. If ACK received ?channel open, return SUCCESS
 * 5. If no ACK after N attempts ?return FAILED_UDP
 *
 * @param remote_public_ip Peer's reported public IP (from FINDNODE/addr exchange)
 * @param remote_public_port Peer's reported public port
 * @param local_port Our UDP bind port (0 = auto-select)
 * @param nonce Unique identifier for this punch attempt
 */
 PunchResult CoordinateUDPPunch(
 const std::string& remote_public_ip,
 uint16_t remote_public_port,
 uint16_t local_port,
 uint64_t nonce);

 /**
 * Listen for incoming punch packets on a bound UDP socket.
 * Must be called in a separate thread or async context.
 * Returns immediately with the first matching packet found.
 *
 * @param listen_sock Already-bound UDP socket
 * @param expected_nonce The nonce we're expecting
 * @param timeout_ms How long to wait
 * @return true if valid ACK received
 */
 bool WaitForPunchAck(int listen_sock, uint64_t expected_nonce, int timeout_ms);

 // --- TCP Fallback ---

 /**
 * Attempt direct TCP connection to remote peer's P2P port.
 * This is the fallback when UDP hole punching fails.
 *
 * Note: For nodes behind symmetric NAT, this will also fail unless
 * one side has port forwarding (UPnP/NAT-PMP already tried).
 * In that case, relay through a third node is needed (future work).
 */
 PunchResult TryTCPFallback(const std::string& remote_ip, uint16_t remote_port);

 // --- Public Endpoint Discovery ---

 /** Get our best guess at our publicly reachable address */
 std::string GetPublicEndpoint() const;
 void SetPublicEndpoint(const std::string& ip, uint16_t port);

 // --- Status ---
 bool IsInitialized() const { return m_initialized; }
 std::string GetStatusString() const;

private:
 ReachabilityConfig m_config;
 NATType m_nat_type = NATType::UNKNOWN;
 std::string m_public_ip;
 uint16_t m_public_port = 0;
 bool m_initialized = false;

 // Internal helpers
 std::string DiscoverExternalIP();
 bool CreateUDPBind(uint16_t port, int& out_sock);
 void CloseSocket(int sock);
};

#endif // TKN_NET_P2P_REACHABILITY_H
