#ifndef TKN_NET_PEER_SESSION_H
#define TKN_NET_PEER_SESSION_H

#include <cstdint>
#include <string>
#include <chrono>
#include <atomic>

// ============================================================
// (Chinese comment removed)
//
// Lifecycle:
// (Chinese comment removed)
// (Chinese comment removed)
//
// Sessions are created by P2PReachability after successful hole punch,
// and consumed by RemoteBackend for inference routing.
// ============================================================

enum class SessionState : int {
 NONE = 0,
 ESTABLISHED, // Hole punch / TCP connect succeeded
 ACTIVE, // Data flowing, heartbeats passing
 IDLE, // No recent activity but still valid
 EXPIRED, // (Chinese comment removed)
 FAULT // Connection lost or protocol error
};

const char* SessionStateToString(SessionState s);

struct PeerSession {
 // Identity
 std::string peer_id; // Unique identifier for remote node (e.g., wallet addr)
 std::string peer_endpoint; // "ip:port" of the remote node

 // Transport info
 std::string local_addr; // Our address as seen by peer
 bool udp_punch_used = false; // True if UDP hole punching succeeded
 bool tcp_fallback_used = false; // True if TCP fallback was used

 // State machine
 std::atomic<SessionState> state{SessionState::NONE};

 // Timing
 int64_t created_at = 0; // Unix timestamp when session was created
 int64_t last_activity = 0; // Last data/heartbeat timestamp
 int64_t last_heartbeat = 0; // Last sent heartbeat

 // Statistics
 uint64_t bytes_sent = 0;
 uint64_t bytes_recv = 0;
 uint32_t requests_served = 0; // Number of APIREQs handled through this session
 double avg_latency_ms = 0.0;

 // --- Methods ---
 PeerSession() = default;
 PeerSession(const std::string& pid, const std::string& ep);

 void MarkActive();
 void MarkIdle();
 void MarkError(const std::string& reason);
 void MarkExpired();

 bool IsAlive() const;
 bool IsUsable() const; // Can be used for routing (ACTIVE or IDLE)

 int64_t IdleSeconds(int64_t now) const;

 std::string ToString() const;
};

// ============================================================
// (Chinese comment removed)
//
// Thread safety: All public methods are safe to call from any thread.
// Uses internal mutex for session map access.
// ============================================================
#include <map>
#include <mutex>
#include <vector>
#include <functional>

class PeerSessionManager {
public:
 PeerSessionManager();
 ~PeerSessionManager() = default;

 // --- Session lifecycle ---

 /** Create and register a new session. Returns pointer to stored session. */
 PeerSession* CreateSession(
 const std::string& peer_id,
 const std::string& peer_endpoint,
 bool udp_punch = false,
 bool tcp_fallback = false);

 /** Remove a session by peer_id */
 bool RemoveSession(const std::string& peer_id);

 /** Look up session by peer_id */
 PeerSession* GetSession(const std::string& peer_id);
 const PeerSession* GetSession(const std::string& peer_id) const;

 /**
 * Get existing usable session, or create/update one.
 * Core binding point: ensures endpointession is always connected.
 * Returns pointer to stored session (never null on success).
 */
 PeerSession* GetOrCreateSession(
 const std::string& peer_id,
 const std::string& peer_endpoint,
 bool udp_punch = false,
 bool tcp_fallback = false);

 // --- Queries ---

 /** Count sessions in a given state */
 size_t CountByState(SessionState state) const;

 /** Total number of tracked sessions */
 size_t TotalCount() const;

 /** Get all usable sessions (ACTIVE + IDLE) */
 std::vector<PeerSession*> GetUsableSessions();

 /** Get best session for a request (lowest latency, most active) */
 PeerSession* SelectBest(const std::string& model_hint = "");

 // --- Maintenance ---

 /**
 * Expire idle sessions that have exceeded max_idle_seconds.
 * Should be called periodically (e.g., every 30s).
 * @return Number of sessions expired
 */
 int ExpireIdleSessions(int max_idle_seconds = 120);

 /**
 * Send heartbeat on all active sessions.
 * Marks sessions as IDLE if no response within timeout.
 * @return Number of heartbeats sent
 */
 int HeartbeatAll();

 /** Get status summary string */
 std::string GetStatusSummary() const;

private:
 mutable std::mutex m_mutex;
 std::map<std::string, PeerSession> m_sessions; // peer_id -> session
};

#endif // TKN_NET_PEER_SESSION_H
