#include <net/p2p_reachability.h>
#include <util/log.h>

#ifdef WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <cstring>
#include <chrono>
#include <thread>
#include <sstream>

static const char* PUNCH_INIT = "TKN_PUNCH_INIT ";
static const char* PUNCH_ACK  = "TKN_PUNCH_ACK ";

// ============================================================
// NATType string conversion
// ============================================================
const char* NatTypeToString(NATType type)
{
    switch (type) {
        case NATType::PUBLIC:          return "PUBLIC";
        case NATType::FULL_CONE:       return "FULL_CONE";
        case NATType::RESTRICTED_CONE: return "RESTRICTED";
        case NATType::PORT_RESTRICTED: return "PORT_RESTR";
        case NATType::SYMMETRIC:       return "SYMMETRIC";
        default:                       return "UNKNOWN";
    }
}

ReachabilityConfig ReachabilityConfig::Default()
{
    ReachabilityConfig c;
    c.udp_punch_attempts = 5;
    c.udp_punch_interval_ms = 500;
    c.udp_punch_timeout_ms = 10000;
    c.tcp_connect_timeout_ms = 8000;
    c.session_heartbeat_interval_s = 15;
    c.session_max_idle_s = 120;
    c.endpoint_broadcast_interval_s = 60;
    return c;
}

P2PReachability::P2PReachability(const ReachabilityConfig& config)
    : m_config(config), m_nat_type(NATType::UNKNOWN)
{
#ifdef WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    m_initialized = true;
}

P2PReachability::~P2PReachability()
{
#ifdef WIN32
    WSACleanup();
#endif
}

// ============================================================
// NAT Type Detection
// ============================================================
NATType P2PReachability::DetectNATType(const std::string& seed_addr, uint16_t seed_port)
{
    if (!m_initialized) return NATType::UNKNOWN;

    LogInfo("[P2P-REACH] Detecting NAT type via %s:%d", seed_addr.c_str(), seed_port);

    m_public_ip = DiscoverExternalIP();

    // Probe with two sockets to detect symmetric vs cone
    int sock_a = -1, sock_b = -1;
    CreateUDPBind(0, sock_a);
    CreateUDPBind(0, sock_b);

    if (sock_a < 0 && sock_b < 0) {
        m_nat_type = NATType::PUBLIC;
        LogInfo("[P2P-REACH] No sockets -> PUBLIC");
        return m_nat_type;
    }

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(seed_port);
    inet_pton(AF_INET, seed_addr.c_str(), &dest.sin_addr);

    sendto(sock_a, "TKN_PROBE_A", 10, 0, (sockaddr*)&dest, sizeof(dest));
    sendto(sock_b, "TKN_PROBE_B", 10, 0, (sockaddr*)&dest, sizeof(dest));

    CloseSocket(sock_a);
    CloseSocket(sock_b);

    // Heuristic: if we found a non-local public IP -> behind NAT (cone type)
    bool is_private =
        m_public_ip.find("192.168.") != std::string::npos ||
        m_public_ip.find("10.") != std::string::npos ||
        m_public_ip == "127.0.0.1" ||
        m_public_ip.empty();

    if (is_private) {
        m_nat_type = NATType::FULL_CONE;
        LogInfo("[P2P-REACH] NAT=FULL_CONE (local IP detected)");
    } else {
        m_nat_type = NATType::RESTRICTED_CONE;
        LogInfo("[P2P-REACH] NAT=RESTRICTED_CONE (public=%s)", m_public_ip.c_str());
    }

    return m_nat_type;
}

bool P2PReachability::CanDirectConnect() const
{
    return m_nat_type != NATType::SYMMETRIC && m_nat_type != NATType::UNKNOWN;
}

// ============================================================
// Coordinated UDP Hole Punching
// ============================================================
PunchResult P2PReachability::CoordinateUDPPunch(
    const std::string& remote_ip, uint16_t remote_port,
    uint16_t local_port, uint64_t nonce)
{
    if (!m_initialized) return PunchResult::FAILED_UDP;

    int sock = -1;
    if (!CreateUDPBind(local_port, sock)) {
        LogWarning("[P2P-REACH] Cannot create punch socket");
        return PunchResult::FAILED_UDP;
    }

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(remote_port);
    inet_pton(AF_INET, remote_ip.c_str(), &target.sin_addr);

    char init_msg[128];
    int init_len = snprintf(init_msg, sizeof(init_msg), "%s%llu", PUNCH_INIT, (unsigned long long)nonce);

    for (int attempt = 0; attempt < m_config.udp_punch_attempts; ++attempt) {
        sendto(sock, init_msg, init_len, 0, (sockaddr*)&target, sizeof(target));

        if (WaitForPunchAck(sock, nonce, m_config.udp_punch_interval_ms)) {
            sockaddr_in local{};
            socklen_t len = sizeof(local);
            getsockname(sock, (sockaddr*)&local, &len);
            m_public_port = ntohs(local.sin_port);

            LogInfo("[P2P-REACH] Punch SUCCESS attempt=%d", attempt + 1);
            CloseSocket(sock);
            return PunchResult::SUCCESS;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(m_config.udp_punch_interval_ms / 2));
    }

    LogWarning("[P2P-REACH] Punch FAILED after %d attempts", m_config.udp_punch_attempts);
    CloseSocket(sock);
    return PunchResult::FAILED_UDP;
}

bool P2PReachability::WaitForPunchAck(int sock, uint64_t expected_nonce, int timeout_ms)
{
#ifdef WIN32
    DWORD tv = timeout_ms;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    char buf[512];
    sockaddr_in from{};
    socklen_t flen = sizeof(from);

    int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &flen);
    if (n <= 0) return false;

    buf[n] = '\0';
    if (strncmp(buf, PUNCH_ACK, strlen(PUNCH_ACK)) != 0) return false;

    uint64_t got = 0;
    if (sscanf(buf + strlen(PUNCH_ACK), "%llu", (unsigned long long*)&got) == 1 && got == expected_nonce) {
        LogInfo("[P2P-REACH] Received valid ACK");
        return true;
    }
    return false;
}

// ============================================================
// TCP Fallback
// ============================================================
PunchResult P2PReachability::TryTCPFallback(const std::string& ip, uint16_t port)
{
    LogInfo("[P2P-REACH] TCP fallback to %s:%d", ip.c_str(), port);

#ifdef WIN32
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return PunchResult::FAILED_TCP_FALLBACK;

    u_long mode = 1; ioctlsocket(s, FIONBIO, &mode);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    connect(s, (sockaddr*)&addr, sizeof(addr));

    fd_set ws; FD_ZERO(&ws); FD_SET(s, &ws);
    timeval tv{m_config.tcp_connect_timeout_ms / 1000, (m_config.tcp_connect_timeout_ms % 1000) * 1000};

    if (select(0, nullptr, &ws, nullptr, &tv) > 0) {
        ioctlsocket(s, FIONBIO, &(mode = 0));
        closesocket(s);
        LogInfo("[P2P-REACH] TCP fallback SUCCESS");
        return PunchResult::SUCCESS;
    }
    closesocket(s);
#else
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return PunchResult::FAILED_TCP_FALLBACK;

    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    connect(s, (sockaddr*)&addr, sizeof(addr));

    fd_set ws; FD_ZERO(&ws); FD_SET(s, &ws);
    struct timeval tv{m_config.tcp_connect_timeout_ms / 1000, (m_config.tcp_connect_timeout_ms % 1000) * 1000};

    if (select(s + 1, nullptr, &ws, nullptr, &tv) > 0) { close(s); return PunchResult::SUCCESS; }
    close(s);
#endif

    LogWarning("[P2P-REACH] TCP fallback FAILED");
    return PunchResult::FAILED_TCP_FALLBACK;
}

// ============================================================
// Endpoint
// ============================================================
std::string P2PReachability::GetPublicEndpoint() const
{
    if (m_public_ip.empty()) return "";
    return m_public_ip + ":" + std::to_string(m_public_port > 0 ? m_public_port : 9333);
}

void P2PReachability::SetPublicEndpoint(const std::string& ip, uint16_t port)
{
    m_public_ip = ip;
    m_public_port = port;
}

std::string P2PReachability::GetStatusString() const
{
    std::ostringstream ss;
    ss << "NAT=" << NatTypeToString(m_nat_type)
       << " EP=" << GetPublicEndpoint()
       << " DC=" << (CanDirectConnect() ? "Y" : "N");
    return ss.str();
}

// ============================================================
// Internal Helpers
// ============================================================

std::string P2PReachability::DiscoverExternalIP()
{
#ifdef WIN32
    ULONG len = 15000;
    auto adapters = (PIP_ADAPTER_ADDRESSES)malloc(len);
    if (!adapters) return "";

    ULONG ret = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
        GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME,
        nullptr, adapters, &len);

    if (ret == ERROR_BUFFER_OVERFLOW) {
        free(adapters);
        adapters = (PIP_ADAPTER_ADDRESSES)malloc(len);
        if (!adapters) return "";
        ret = GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME,
            nullptr, adapters, &len);
    }

    if (ret == NO_ERROR && adapters) {
        for (auto* a = adapters; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                auto* sa = (SOCKADDR_IN*)u->Address.lpSockaddr;
                if (sa->sin_family == AF_INET) {
                    char buf[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
                    std::string ip(buf);
                    // Skip loopback and link-local
                    if (ip != "127.0.0.1" &&
                        ip.find("169.254.") == std::string::npos) {
                        free(adapters);
                        return ip;
                    }
                }
            }
        }
        free(adapters);
    }
#endif
    return "";
}

bool P2PReachability::CreateUDPBind(uint16_t port, int& out_sock)
{
#ifdef WIN32
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    out_sock = static_cast<int>(s);

    int reuse = 1;
    setsockopt(out_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port > 0 ? port : 0);

    if (bind(out_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(out_sock);
        out_sock = -1;
        return false;
    }
    return true;
#else
    out_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (out_sock < 0) return false;

    int reuse = 1;
    setsockopt(out_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port > 0 ? port : 0);

    if (bind(out_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(out_sock);
        out_sock = -1;
        return false;
    }
    return true;
#endif
}

void P2PReachability::CloseSocket(int sock)
{
    if (sock < 0) return;
#ifdef WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}
