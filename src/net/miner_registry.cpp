#include <net/miner_registry.h>
#include <util/log.h>
#include <cstring>  // for memset, strlen (Linux compatibility)

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define closesocket close
#endif

void MinerLocalRegistry::ParseMinersResponse(const std::string& json_body)
{
    // Clear online status — all miners start as offline this round
    for (auto& [key, entry] : m_registry) {
        entry.online = false;
    }

    // Find all "status":"online" entries in JSON array
    std::string online_marker = "\"status\":\"online\"";
    size_t search_pos = 0;

    while (true) {
        size_t online_pos = json_body.find(online_marker, search_pos);
        if (online_pos == std::string::npos) break;

        // Search backwards from "status":"online" to find wallet_address in same object
        size_t obj_start = json_body.rfind("\"wallet_address\"", online_pos);
        if (obj_start == std::string::npos || obj_start > online_pos) {
            search_pos = online_pos + online_marker.length();
            continue;
        }

        // Extract wallet address
        std::string wallet;
        {
            size_t colon = json_body.find(":", obj_start);
            size_t start = colon + 1;
            while (start < json_body.size() && (json_body[start] == ' ' || json_body[start] == '"')) start++;
            size_t end = json_body.find('"', start);
            if (end != std::string::npos) wallet = json_body.substr(start, end - start);
        }

        // Extract model_name
        std::string model;
        {
            size_t mpos = json_body.rfind("\"model_name\"", online_pos);
            if (mpos != std::string::npos && mpos < online_pos) {
                size_t colon = json_body.find(":", mpos);
                size_t start = colon + 1;
                while (start < json_body.size() && (json_body[start] == ' ' || json_body[start] == '"')) start++;
                size_t end = json_body.find('"', start);
                if (end != std::string::npos) model = json_body.substr(start, end - start);
            }
        }

        // Extract gpu_name
        std::string gpu;
        {
            size_t gpos = json_body.rfind("\"gpu_name\"", online_pos);
            if (gpos != std::string::npos && gpos < online_pos) {
                size_t colon = json_body.find(":", gpos);
                size_t start = colon + 1;
                while (start < json_body.size() && (json_body[start] == ' ' || json_body[start] == '"')) start++;
                size_t end = json_body.find('"', start);
                if (end != std::string::npos) gpu = json_body.substr(start, end - start);
            }
        }

        if (!wallet.empty()) {
            int64_t now = GetTime();  // use chain time
            auto& entry = m_registry[wallet];
            entry.wallet_address = wallet;
            entry.model_name = model;
            entry.gpu_name = gpu;
            entry.api_port = DEFAULT_API_PORT;
            entry.last_seen = now;
            entry.online = true;

            LogInfo("[LOCAL-REG] Miner registered: wallet=%s model=%s gpu=%s",
                    wallet.substr(0, 16).c_str(), model.c_str(), gpu.c_str());
        }

        search_pos = online_pos + online_marker.length();
    }
}

void MinerLocalRegistry::Probe()
{
#ifdef WIN32
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
#ifdef WIN32
    if (sock == INVALID_SOCKET) return;
#else
    if (sock < 0) return;
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DEFAULT_API_PORT);
    inet_pton(AF_INET, LOCAL_MINER_HOST, &addr.sin_addr);

#ifdef WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#endif

    int cr = connect(sock, (struct sockaddr*)&addr, sizeof(addr));

#ifdef WIN32
    if (cr != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set ws; FD_ZERO(&ws); FD_SET(sock, &ws);
        timeval tv; tv.tv_sec = 2; tv.tv_usec = 0;
        if (select(0, NULL, &ws, NULL, &tv) > 0) cr = 0;
    }
    mode = 0;
    ioctlsocket(sock, FIONBIO, &mode);
#endif

    if (cr != 0) {
        closesocket(sock);
        // Mark all existing entries as potentially stale (not updated this probe)
        LogInfo("[LOCAL-REG] Local miner API not reachable at %s:%d", LOCAL_MINER_HOST, DEFAULT_API_PORT);
        return;
    }

    // HTTP GET /api/v1/miners
    const char* req =
        "GET /api/v1/miners HTTP/1.1\r\n"
        "Host: 127.0.0.1:9332\r\n"
        "Connection: close\r\n\r\n";
    send(sock, req, (int)strlen(req), 0);

    char buf[8196] = {};
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int r = recv(sock, buf + total, sizeof(buf) - total - 1, 0);
        if (r <= 0) break;
        total += r;
    }
    closesocket(sock);

    std::string response(buf, total);
    size_t hdr_end = response.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return;

    std::string body = response.substr(hdr_end + 4);
    ParseMinersResponse(body);
}

bool MinerLocalRegistry::HasOnlineMiner() const
{
    int64_t now = GetTime();
    for (const auto& [key, entry] : m_registry) {
        if (entry.online && (now - entry.last_seen) < MINER_TIMEOUT_SECONDS)
            return true;
    }
    return false;
}

const LocalMinerEntry* MinerLocalRegistry::SelectBest(const std::string& model_hint) const
{
    const LocalMinerEntry* best = nullptr;
    int best_score = -1;
    int64_t now = GetTime();

    for (const auto& [key, entry] : m_registry) {
        if (!entry.online) continue;
        if ((now - entry.last_seen) >= MINER_TIMEOUT_SECONDS) continue;

        int score = 0;
        score += entry.model_match_score(model_hint);
        score += entry.freshness_score(now);
        if (!entry.wallet_address.empty()) score += 20;

        if (score > best_score) {
            best_score = score;
            best = &entry;
        }
    }

    if (best) {
        LogInfo("[LOCAL-REG] Selected miner: wallet=%s model=%s score=%d",
                best->wallet_address.substr(0, 16).c_str(),
                best->model_name.c_str(), best_score);
    }

    return best;
}

std::vector<LocalMinerEntry> MinerLocalRegistry::GetAll() const
{
    std::vector<LocalMinerEntry> result;
    int64_t now = GetTime();
    for (const auto& [key, entry] : m_registry) {
        if ((now - entry.last_seen) < MINER_TIMEOUT_SECONDS) {
            result.push_back(entry);
        }
    }
    return result;
}

size_t MinerLocalRegistry::OnlineCount() const
{
    int64_t now = GetTime();
    size_t count = 0;
    for (const auto& [key, entry] : m_registry) {
        if (entry.online && (now - entry.last_seen) < MINER_TIMEOUT_SECONDS) count++;
    }
    return count;
}
