#include <net/miner_registry.h>
#include <util/log.h>
#include <cstring> // for memset, strlen (Linux compatibility)

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

void MinerLocalRegistry::ParseMinersResponse(const std::string& json_body, uint16_t port)
{
 // is responsible for marking all entries offline before scanning ports.
 // This allows Probe to call ParseMinersResponse once per port without
 // wiping results from previously scanned ports.

 // Find all "status":"online" entries in JSON array
 // Miner API returns fields in this order: miner_id, model_name, status, gpu_name,
 // gpu_vram_total_mb, hashrate, wallet_address, tokens_per_tknc
 std::string online_marker = "\"status\":\"online\"";
 size_t search_pos = 0;

 while (true) {
 size_t online_pos = json_body.find(online_marker, search_pos);
 if (online_pos == std::string::npos) break;

 // Find the end of this JSON object (next closing brace after online_pos)
 size_t obj_end = json_body.find('}', online_pos);
 if (obj_end == std::string::npos) obj_end = json_body.size();

 // Extract wallet address: search FORWARD from online_pos (field comes after status)
 std::string wallet;
 {
 size_t wpos = json_body.find("\"wallet_address\"", online_pos);
 if (wpos != std::string::npos && wpos < obj_end) {
 size_t colon = json_body.find(":", wpos);
 size_t start = colon + 1;
 while (start < json_body.size() && (json_body[start] == ' ' || json_body[start] == '"')) start++;
 size_t end = json_body.find('"', start);
 if (end != std::string::npos) wallet = json_body.substr(start, end - start);
 }
 }

 // Extract model_name: search BACKWARD from online_pos (field comes before status)
 std::string model;
 {
 size_t mpos = json_body.rfind("\"model_name\"", online_pos);
 if (mpos != std::string::npos) {
 size_t colon = json_body.find(":", mpos);
 size_t start = colon + 1;
 while (start < json_body.size() && (json_body[start] == ' ' || json_body[start] == '"')) start++;
 size_t end = json_body.find('"', start);
 if (end != std::string::npos) model = json_body.substr(start, end - start);
 }
 }

 // Extract gpu_name: search FORWARD from online_pos (field comes after status)
 std::string gpu;
 {
 size_t gpos = json_body.find("\"gpu_name\"", online_pos);
 if (gpos != std::string::npos && gpos < obj_end) {
 size_t colon = json_body.find(":", gpos);
 size_t start = colon + 1;
 while (start < json_body.size() && (json_body[start] == ' ' || json_body[start] == '"')) start++;
 size_t end = json_body.find('"', start);
 if (end != std::string::npos) gpu = json_body.substr(start, end - start);
 }
 }

 if (!wallet.empty()) {
 int64_t now = GetTime(); // use chain time
 auto& entry = m_registry[wallet];
 entry.wallet_address = wallet;
 entry.model_name = model;
 entry.gpu_name = gpu;
 entry.api_port = port; // Store the actual port this miner was found on
 entry.last_seen = now;
 entry.online = true;

 LogInfo("[LOCAL-REG] Miner registered: wallet=%s model=%s gpu=%s port=%d",
 wallet.substr(0, 16).c_str(), model.c_str(), gpu.c_str(), (int)port);
 }

 search_pos = online_pos + online_marker.length();
 }
}

void MinerLocalRegistry::Probe()
{
 // Mark all existing entries as offline at the start of this probe cycle.
 // ParseMinersResponse (called per-port below) will mark entries online as
 // they are found on each port. Entries not found on any port remain offline.
 for (auto& [key, entry] : m_registry) {
 entry.online = false;
 }

 // Scan all ports in the multi-miner range (9332-9342).
 // Each miner's API server only lists its own wallet in /api/v1/miners,
 // so we must probe every port to discover all local miners.
 bool any_port_responded = false;

 for (uint16_t port = MINER_PORT_START; port <= MINER_PORT_END; ++port) {
#ifdef WIN32
 SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
 if (sock == INVALID_SOCKET) continue;
#else
 int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
 if (sock < 0) continue;
#endif

 struct sockaddr_in addr;
 memset(&addr, 0, sizeof(addr));
 addr.sin_family = AF_INET;
 addr.sin_port = htons(port);
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
 continue;
 }

 // HTTP GET /api/v1/miners
 char req_buf[256];
 snprintf(req_buf, sizeof(req_buf),
 "GET /api/v1/miners HTTP/1.1\r\n"
 "Host: 127.0.0.1:%d\r\n"
 "Connection: close\r\n\r\n", (int)port);
 send(sock, req_buf, (int)strlen(req_buf), 0);

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
 if (hdr_end == std::string::npos) continue;

 std::string body = response.substr(hdr_end + 4);
 ParseMinersResponse(body, port);
 any_port_responded = true;
 }

 if (!any_port_responded) {
 LogInfo("[LOCAL-REG] No local miner API responded on ports %d-%d",
 (int)MINER_PORT_START, (int)MINER_PORT_END);
 }
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
