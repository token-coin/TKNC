// Copyright (c) 2026 The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// A2.7/A2.8 Architecture compliance: Node handles all external communication
// for miners. Miners communicate only via localhost RPC to the node.

#include <node/miner_registry.h>
#include <logging.h>
#include <util/time.h>
#include <sstream>
#include <vector>

#ifdef WIN32
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
#include <sys/time.h>
#include <ifaddrs.h>
#endif

namespace node {

// Detect the REAL public IPv6 address using OS routing probe.
// PRIMARY: UDP connect + getsockname — returns the OS-chosen outbound source address.
// On Windows with RFC 4941 privacy extension enabled (default), this returns the
// TEMPORARY address that the router actually uses for external communication.
//
// Interface enumeration is NOT used because EUI-64/DHCPv6 stable addresses returned
// by enumeration are NOT reachable from outside — the router's NDP only forwards to
// the temporary (RFC 4941) address used for outbound traffic. Broadcasting a stable
// address causes P2P inference routing to fail.
#ifdef WIN32
static std::string DetectLocalPublicIPv6()
{
    SOCKET probe_sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (probe_sock != INVALID_SOCKET) {
        struct sockaddr_in6 target {};
        target.sin6_family = AF_INET6;
        target.sin6_port = htons(53);
        inet_pton(AF_INET6, "2001:4860:4860::8888", &target.sin6_addr);

        DWORD timeout_ms = 3000;
        setsockopt(probe_sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));

        if (connect(probe_sock, (struct sockaddr*)&target, sizeof(target)) == 0) {
            struct sockaddr_in6 local_addr {};
            int local_len = sizeof(local_addr);
            if (getsockname(probe_sock, (struct sockaddr*)&local_addr, &local_len) == 0) {
                char buf[64] = {};
                inet_ntop(AF_INET6, &local_addr.sin6_addr, buf, sizeof(buf));
                std::string ip(buf);
                if (!ip.empty() && ip.find("fe80") != 0 && ip.find("::1") != 0 &&
                    ip.find("ff") != 0 && ip != "::" && ip.find("::ffff:") == std::string::npos) {
                    closesocket(probe_sock);
                    return ip;
                }
            }
        }
        closesocket(probe_sock);
    }

    // No interface enumeration fallback — EUI-64/DHCPv6 stable addresses are NOT
    // reachable from outside (router only forwards to the temporary address used
    // for outbound traffic). Returning empty lets the caller fall back to IPv4
    // instead of broadcasting an unreachable IPv6 that breaks P2P inference routing.
    LogWarning("MinerRegistry: OS routing probe failed for IPv6; skipping interface enumeration to avoid unreachable IPv6");
    return "";
}
#endif

std::string DetectPublicIPForMiner(const std::string& wallet_address)
{
    LogInfo("MinerRegistry: Detecting public IP for miner %s...", wallet_address.substr(0, 16).c_str());

    // Priority 1: Use environment variable if set (supports IPv6)
    const char* env_ip = getenv("TKNC_MINER_PUBLIC_IP");
    if (!env_ip || env_ip[0] == '\0') {
        env_ip = getenv("TKNC_PUBLIC_IP");
    }
    if (env_ip && env_ip[0] != '\0') {
        std::string env_ip_str(env_ip);
        // Validate: not localhost/private
        if (env_ip_str != "127.0.0.1" && env_ip_str != "::1" &&
            env_ip_str != "localhost" && !env_ip_str.empty()) {
            LogInfo("MinerRegistry: Using TKNC_MINER_PUBLIC_IP=%s", env_ip_str.c_str());
            return env_ip_str;
        }
    }

// Priority 2: Detect local public IPv6 from network interfaces (before trying external service)
#ifdef WIN32
    {
        std::string ipv6 = DetectLocalPublicIPv6();
        if (!ipv6.empty()) {
            LogInfo("MinerRegistry: Using detected local IPv6: %s", ipv6.c_str());
            return ipv6;
        }
    }
#else
    // macOS/Linux: Use OS routing decision to find the REAL public IPv6.
    // Connect a UDP socket to an external address and call getsockname()
    // This is more reliable than scanning interfaces (avoids virtual/bridge interfaces)
    {
        int probe_sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (probe_sock >= 0) {
            struct sockaddr_in6 target {};
            target.sin6_family = AF_INET6;
            target.sin6_port = htons(53);
            inet_pton(AF_INET6, "2001:4860:4860::8888", &target.sin6_addr);

            struct timeval tv;
            tv.tv_sec = 3;
            tv.tv_usec = 0;
            setsockopt(probe_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            if (connect(probe_sock, (struct sockaddr*)&target, sizeof(target)) == 0) {
                struct sockaddr_in6 local_addr {};
                socklen_t local_len = sizeof(local_addr);
                if (getsockname(probe_sock, (struct sockaddr*)&local_addr, &local_len) == 0) {
                    char buf[INET6_ADDRSTRLEN] = {};
                    inet_ntop(AF_INET6, &local_addr.sin6_addr, buf, sizeof(buf));
                    std::string os_chosen_ip(buf);
                    // Validate: must be global unicast, not link-local/loopback/IPv4-mapped
                    if (!os_chosen_ip.empty() &&
                        os_chosen_ip.find("fe80") != 0 &&
                        os_chosen_ip.find("::1") != 0 &&
                        os_chosen_ip.find("ff") != 0 &&
                        os_chosen_ip != "::" &&
                        os_chosen_ip.find("::ffff:") == std::string::npos) {
                        LogInfo("MinerRegistry: OS routing chose public IPv6: %s", os_chosen_ip.c_str());
                        close(probe_sock);
                        return os_chosen_ip;
                    }
                }
            }
            close(probe_sock);
        }
    }

    // Fallback: Scan network interfaces for IPv4 only.
    // IPv6 interface enumeration removed — stable EUI-64/DHCP addresses are NOT
    // reachable from outside (router only forwards to the temporary address).
    {
        struct ifaddrs *ifap = nullptr;
        if (getifaddrs(&ifap) == 0) {
            std::string best_ipv4;
            for (struct ifaddrs *ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
                if (!ifa->ifa_addr) continue;
                if (ifa->ifa_addr->sa_family == AF_INET) {
                    struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
                    uint32_t ip = ntohl(sin->sin_addr.s_addr);
                    if (ip == 0x7f000001) continue;
                    if ((ip & 0xff000000) == 0x0a000000) continue;
                    if ((ip & 0xfff00020) == 0xac100000) continue;
                    if ((ip & 0xffff0000) == 0xc0a80000) continue;
                    if ((ip & 0xffc00000) == 0x64400000) continue;
                    char ip_str[INET_ADDRSTRLEN];
                    if (inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str))) {
                        best_ipv4 = ip_str;
                    }
                }
            }
            freeifaddrs(ifap);
            if (!best_ipv4.empty()) {
                LogInfo("MinerRegistry: Using detected local IPv4: %s", best_ipv4.c_str());
                return best_ipv4;
            }
        }
    }
#endif

// Priority 3: External IPv4 detection via api.ipify.org (fallback)
#ifdef WIN32
    struct addrinfo hints = {}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo("api.ipify.org", "80", &hints, &result) != 0) {
        LogInfo("MinerRegistry: DNS resolution failed, using 127.0.0.1");
        return "127.0.0.1";
    }

    SOCKET sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock == INVALID_SOCKET) {
        LogInfo("MinerRegistry: Socket creation failed, using 127.0.0.1");
        freeaddrinfo(result);
        return "127.0.0.1";
    }

    DWORD timeout = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    if (connect(sock, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
        LogInfo("MinerRegistry: Connection failed, using 127.0.0.1");
        closesocket(sock);
        freeaddrinfo(result);
        return "127.0.0.1";
    }

    freeaddrinfo(result);

    const char* request =
        "GET / HTTP/1.1\r\n"
        "Host: api.ipify.org\r\n"
        "User-Agent: TKNC-Node/1.0\r\n"
        "Connection: close\r\n"
        "\r\n";

    if (send(sock, request, (int)strlen(request), 0) == SOCKET_ERROR) {
        LogInfo("MinerRegistry: HTTP send failed, using 127.0.0.1");
        closesocket(sock);
        return "127.0.0.1";
    }

    char buffer[4096] = {};
    int totalReceived = 0;
    int bytesReceived;
    while ((bytesReceived = recv(sock, buffer + totalReceived, (int)(sizeof(buffer) - 1 - totalReceived), 0)) > 0) {
        totalReceived += bytesReceived;
        if (totalReceived >= (int)(sizeof(buffer) - 1)) break;
    }
    buffer[totalReceived] = '\0';

    closesocket(sock);

    std::string response(buffer);
    size_t bodyStart = response.find("\r\n\r\n");
    if (bodyStart == std::string::npos) {
        LogInfo("MinerRegistry: Invalid HTTP response, using 127.0.0.1");
        return "127.0.0.1";
    }

    std::string ip = response.substr(bodyStart + 4);
    while (!ip.empty() && (ip.back() == '\r' || ip.back() == '\n' || ip.back() == ' '))
        ip.pop_back();
    while (!ip.empty() && (ip.front() == '\r' || ip.front() == '\n' || ip.front() == ' '))
        ip.erase(0, 1);

    if (ip.empty()) {
        LogInfo("MinerRegistry: Empty IP response, using 127.0.0.1");
        return "127.0.0.1";
    }

    LogInfo("MinerRegistry: Public IP detected: %s", ip.c_str());
    return ip;
#else
    return "127.0.0.1";
#endif
}

static bool SendHTTPPostToWeb(const std::string& web_server_url,
                              const std::string& path,
                              const std::string& json_body)
{
#ifdef WIN32
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        LogWarning("MinerRegistry: Socket creation failed\n");
        return false;
    }

    std::string web_host = "127.0.0.1";
    int web_port = 80;

    size_t proto_end = web_server_url.find("://");
    if (proto_end != std::string::npos) {
        size_t host_start = proto_end + 3;
        size_t port_sep = web_server_url.find(':', host_start);
        size_t path_start = web_server_url.find('/', host_start);

        if (port_sep != std::string::npos && (path_start == std::string::npos || port_sep < path_start)) {
            web_host = web_server_url.substr(host_start, port_sep - host_start);
            web_port = std::stoi(web_server_url.substr(port_sep + 1));
        } else if (path_start != std::string::npos) {
            web_host = web_server_url.substr(host_start, path_start - host_start);
        } else {
            web_host = web_server_url.substr(host_start);
        }
    }

    struct sockaddr_in server = {};
    server.sin_family = AF_INET;
    server.sin_port = htons(web_port);
    inet_pton(AF_INET, web_host.c_str(), &server.sin_addr);

    if (connect(sock, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR) {
        closesocket(sock);
        return false;
    }

    std::string request = "POST " + path + " HTTP/1.1\r\n";
    request += "Host: " + web_host + ":" + std::to_string(web_port) + "\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(json_body.length()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += json_body;

    send(sock, request.c_str(), (int)request.length(), 0);

    char buffer[4096];
    int total = 0;
    while (true) {
        int r = recv(sock, buffer + total, sizeof(buffer) - total - 1, 0);
        if (r <= 0) break;
        total += r;
    }
    buffer[total] = '\0';
    closesocket(sock);

    return (total > 0);
#else
    return false;
#endif
}

bool RegisterMinerToWeb(const std::string& wallet_address,
                        const std::string& public_ip,
                        const std::string& web_server_url,
                        const std::string& model_name,
                        const std::string& gpu_name,
                        int64_t gpu_vram_total_mb,
                        int64_t gpu_vram_used_mb,
                        double gpu_utilization,
                        int api_port)
{
    if (web_server_url.empty() || wallet_address.empty()) {
        LogInfo("MinerRegistry: Web server registration skipped (url or wallet empty)\n");
        return false;
    }

    std::ostringstream json_body;
    json_body << "{";
    json_body << "\"node_ip\":\"" << public_ip << "\",";
    json_body << "\"miners\":[{";
    json_body << "\"miner_id\":\"" << wallet_address << "\",";
    json_body << "\"wallet_address\":\"" << wallet_address << "\",";
    json_body << "\"model_name\":\"" << model_name << "\",";
    json_body << "\"hashrate\":0,";
    json_body << "\"gpu_name\":\"" << gpu_name << "\",";
    json_body << "\"gpu_vram_total_mb\":" << gpu_vram_total_mb << ",";
    json_body << "\"gpu_vram_used_mb\":" << gpu_vram_used_mb << ",";
    json_body << "\"gpu_utilization\":" << gpu_utilization << ",";
    json_body << "\"uptime_seconds\":0,";
    json_body << "\"status\":\"online\",";
    json_body << "\"api_port\":" << api_port << ",";
    json_body << "\"public_ip\":\"" << public_ip << "\"";
    json_body << "}]";
    json_body << "}";

    std::string path = "/api/node/miner-notify";
    bool success = SendHTTPPostToWeb(web_server_url, path, json_body.str());

    if (success) {
        LogInfo("MinerRegistry: Web server registration successful for %s\n", wallet_address.substr(0, 16).c_str());
    } else {
        LogWarning("MinerRegistry: Web server registration failed for %s\n", wallet_address.substr(0, 16).c_str());
    }

    return success;
}

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
                        int api_port)
{
    if (web_server_url.empty() || wallet_address.empty()) {
        return false;
    }

    int64_t uptime_seconds = 0;
    if (registration_time > 0) {
        uptime_seconds = GetTime() - registration_time;
    }

    // FIX (2026-06-18): Detect miner alive status before sending heartbeat.
    // Previously heartbeat always sent status=online even if miner process crashed,
    // causing Web UI to show miner online when inference service was unavailable.
    // Now we TCP-probe 127.0.0.1:api_port to verify miner HTTP server is running.
    std::string miner_status = "online";
    {
#ifdef WIN32
        SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (probe != INVALID_SOCKET) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<uint16_t>(api_port));
            addr.sin_addr.S_un.S_addr = inet_addr("127.0.0.1");
            // Set short timeout (1 second) for connect
            DWORD tv = 1000;
            setsockopt(probe, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
            if (connect(probe, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
                miner_status = "offline";
                LogWarning("MinerRegistry: Miner %s HTTP server (127.0.0.1:%d) unreachable, reporting offline\n",
                           wallet_address.substr(0, 16).c_str(), api_port);
            }
            closesocket(probe);
        }
#else
        int probe = socket(AF_INET, SOCK_STREAM, 0);
        if (probe >= 0) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<uint16_t>(api_port));
            inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            setsockopt(probe, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (connect(probe, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
                miner_status = "offline";
                LogWarning("MinerRegistry: Miner %s HTTP server (127.0.0.1:%d) unreachable, reporting offline\n",
                           wallet_address.substr(0, 16).c_str(), api_port);
            }
            close(probe);
        }
#endif
    }

    std::ostringstream json_body;
    json_body << "{";
    json_body << "\"node_ip\":\"" << public_ip << "\",";
    json_body << "\"miners\":[{";
    json_body << "\"miner_id\":\"" << wallet_address << "\",";
    json_body << "\"wallet_address\":\"" << wallet_address << "\",";
    json_body << "\"model_name\":\"" << model_name << "\",";
    json_body << "\"hashrate\":" << hashrate << ",";
    json_body << "\"gpu_name\":\"" << gpu_name << "\",";
    json_body << "\"gpu_vram_total_mb\":" << gpu_vram_total_mb << ",";
    json_body << "\"gpu_vram_used_mb\":" << gpu_vram_used_mb << ",";
    json_body << "\"gpu_utilization\":" << gpu_utilization << ",";
    json_body << "\"uptime_seconds\":" << uptime_seconds << ",";
    json_body << "\"status\":\"" << miner_status << "\",";
    json_body << "\"api_port\":" << api_port << ",";
    json_body << "\"public_ip\":\"" << public_ip << "\"";
    json_body << "}]";
    json_body << "}";

    std::string path = "/api/node/miner-notify";
    bool success = SendHTTPPostToWeb(web_server_url, path, json_body.str());

    if (!success) {
        LogWarning("MinerRegistry: Heartbeat failed for %s\n", wallet_address.substr(0, 16).c_str());
    }

    return success;
}

} // namespace node