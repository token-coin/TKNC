#include <seed_register.h>
#include <logging.h>
#include <net.h>
#include <netbase.h>
#include <support/events.h>
#include <univalue.h>
#include <common/args.h>
#include <util/thread.h>
#include <util/time.h>
#include <init.h>

#include <chrono>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <algorithm>

#include <event2/buffer.h>
#include <event2/http.h>

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
#include <ifaddrs.h>
#include <unistd.h>
#endif

static const char* SEED_HOSTS[] = {"66.154.101.183"};
static const int NUM_SEED_HOSTS = 1;
static const int SEED_HTTP_PORT = 80;
static const char* REGISTER_PATH = "/api/p2p/register";
static const char* PEERS_PATH = "/api/p2p/peers";
static const char* HEARTBEAT_PATH = "/api/p2p/heartbeat";
static const int REGISTER_TIMEOUT_SECS = 10;

// Miner local API port range (supports multiple miners on the same machine).
// First miner binds 9332, second 9333, etc. (see api_server.cpp port-retry logic).
static const int MINER_LOCAL_PORT_START = 9332;
static const int MINER_LOCAL_PORT_END = 9342;

static NodeRegistrationInfo g_registration_info;
static std::string g_node_id;
static std::string g_wallet_address; // Legacy: last-set wallet (backward compat)
static std::string g_model_name;
static bool g_registered = false;
static NodeRole g_forced_role = NodeRole::NODE;
static bool g_role_forced = false;
static node::NodeContext* g_node_context = nullptr;

// Multi-miner wallet tracking: when multiple miners run on the same machine,
// each calls SetMinerWalletAddress via miner_ready RPC. We track ALL of them
// so the maintenance loop can send heartbeats for every miner.
static std::vector<std::string> g_miner_wallets;
static std::mutex g_wallets_mutex;

static std::atomic<int64_t> g_miner_last_active{0};
static std::atomic<bool> g_miner_registered_web{false};

static std::string RoleToString(NodeRole role)
{
 if (role == NodeRole::MINER) return "miner";
 if (role == NodeRole::CLIENT) return "client";
 return "node";
}

struct HttpCallbackCtx {
 bool done = false;
 std::string response; // Filled by callback before event_base_dispatch returns
};

static void http_request_done(struct evhttp_request* req, void* ctx)
{
 auto* cb_ctx = static_cast<HttpCallbackCtx*>(ctx);
 cb_ctx->done = true;
 if (req == nullptr) {
 LogError("SeedRegister: HTTP request failed (connection error)\n");
 return;
 }
 int code = evhttp_request_get_response_code(req);
 LogInfo("SeedRegister: HTTP response code %d\n", code);

 // Capture response body HERE while req is guaranteed valid by libevent
 struct evbuffer* inbuf = evhttp_request_get_input_buffer(req);
 size_t len = evbuffer_get_length(inbuf);
 if (len > 0) {
 cb_ctx->response.resize(len);
 evbuffer_copyout(inbuf, &cb_ctx->response[0], len);
 }
}

static std::string HttpGet(const char* path, const std::string& query)
{
 std::string url_path = path;
 if (!query.empty()) {
 url_path += "?" + query;
 }

 for (int i = 0; i < NUM_SEED_HOSTS; ++i) {
 try {
 raii_event_base base = obtain_event_base();
 raii_evhttp_connection evcon = obtain_evhttp_connection_base(
 base.get(), SEED_HOSTS[i], SEED_HTTP_PORT);

 evhttp_connection_set_timeout(evcon.get(), REGISTER_TIMEOUT_SECS);

 HttpCallbackCtx ctx;
 raii_evhttp_request req = obtain_evhttp_request(http_request_done, &ctx);

 struct evkeyvalq* headers = evhttp_request_get_output_headers(req.get());
 evhttp_add_header(headers, "Host", SEED_HOSTS[i]);

 int r = evhttp_make_request(evcon.get(), req.release(),
 EVHTTP_REQ_GET, url_path.c_str());
 if (r != 0) {
 LogError("SeedRegister: GET request failed for %s (seed %s)\n", path, SEED_HOSTS[i]);
 continue;
 }

 event_base_dispatch(base.get());

 if (ctx.done && !ctx.response.empty()) {
 return ctx.response;
 }
 } catch (const std::exception& e) {
 LogError("SeedRegister: GET exception on seed %s: %s\n", SEED_HOSTS[i], e.what());
 continue;
 }
 }
 return "";
}

static std::string HttpPost(const char* path, const std::string& body, const std::string& authToken = "")
{
 for (int i = 0; i < NUM_SEED_HOSTS; ++i) {
 try {
 raii_event_base base = obtain_event_base();
 raii_evhttp_connection evcon = obtain_evhttp_connection_base(
 base.get(), SEED_HOSTS[i], SEED_HTTP_PORT);

 evhttp_connection_set_timeout(evcon.get(), REGISTER_TIMEOUT_SECS);

 HttpCallbackCtx ctx;
 raii_evhttp_request req = obtain_evhttp_request(http_request_done, &ctx);

 struct evkeyvalq* headers = evhttp_request_get_output_headers(req.get());
 evhttp_add_header(headers, "Host", SEED_HOSTS[i]);
 evhttp_add_header(headers, "Content-Type", "application/json");
 if (!authToken.empty()) {
 evhttp_add_header(headers, "Authorization", ("Bearer " + authToken).c_str());
 }

 struct evbuffer* outbuf = evhttp_request_get_output_buffer(req.get());
 evbuffer_add(outbuf, body.c_str(), body.size());

 int r = evhttp_make_request(evcon.get(), req.release(),
 EVHTTP_REQ_POST, path);
 if (r != 0) {
 LogError("SeedRegister: POST request failed for %s (seed %s)\n", path, SEED_HOSTS[i]);
 continue;
 }

 event_base_dispatch(base.get());

 if (ctx.done) {
 return ctx.response;
 }
 } catch (const std::exception& e) {
 LogError("SeedRegister: POST exception on seed %s: %s\n", SEED_HOSTS[i], e.what());
 continue;
 }
 }
 return "";
}

// Post to ALL seed servers (don't stop at first success) - used for miner notifications
static void HttpPostToAll(const char* path, const std::string& body, const std::string& authToken = "")
{
 for (int i = 0; i < NUM_SEED_HOSTS; ++i) {
 try {
 raii_event_base base = obtain_event_base();
 raii_evhttp_connection evcon = obtain_evhttp_connection_base(
 base.get(), SEED_HOSTS[i], SEED_HTTP_PORT);

 evhttp_connection_set_timeout(evcon.get(), REGISTER_TIMEOUT_SECS);

 HttpCallbackCtx ctx;
 raii_evhttp_request req = obtain_evhttp_request(http_request_done, &ctx);

 struct evkeyvalq* headers = evhttp_request_get_output_headers(req.get());
 evhttp_add_header(headers, "Host", SEED_HOSTS[i]);
 evhttp_add_header(headers, "Content-Type", "application/json");
 if (!authToken.empty()) {
 evhttp_add_header(headers, "Authorization", ("Bearer " + authToken).c_str());
 }

 struct evbuffer* outbuf = evhttp_request_get_output_buffer(req.get());
 evbuffer_add(outbuf, body.c_str(), body.size());

 int r = evhttp_make_request(evcon.get(), req.release(),
 EVHTTP_REQ_POST, path);
 if (r != 0) {
 LogError("SeedRegister: POST-to-all failed for seed %s\n", SEED_HOSTS[i]);
 continue;
 }

 event_base_dispatch(base.get());

 if (ctx.done) {
 LogInfo("SeedRegister: Notified seed %s: %s\n", SEED_HOSTS[i],
 ctx.response.substr(0, 80).c_str());
 }
 } catch (const std::exception& e) {
 LogError("SeedRegister: POST-to-all exception on seed %s: %s\n", SEED_HOSTS[i], e.what());
 }
 }
}

static void HttpDelete(const char* path, const std::string& query)
{
 std::string url_path = path;
 if (!query.empty()) {
 url_path += "?" + query;
 }

 for (int i = 0; i < NUM_SEED_HOSTS; ++i) {
 try {
 raii_event_base base = obtain_event_base();
 raii_evhttp_connection evcon = obtain_evhttp_connection_base(
 base.get(), SEED_HOSTS[i], SEED_HTTP_PORT);

 evhttp_connection_set_timeout(evcon.get(), REGISTER_TIMEOUT_SECS);

 bool done = false;
 raii_evhttp_request req = obtain_evhttp_request(http_request_done, &done);

 struct evkeyvalq* headers = evhttp_request_get_output_headers(req.get());
 evhttp_add_header(headers, "Host", SEED_HOSTS[i]);

 int r = evhttp_make_request(evcon.get(), req.release(),
 EVHTTP_REQ_DELETE, url_path.c_str());
 if (r != 0) {
 LogError("SeedRegister: DELETE request failed for %s (seed %s)\n", path, SEED_HOSTS[i]);
 continue;
 }

 event_base_dispatch(base.get());

 if (done) {
 LogInfo("SeedRegister: Unregistered from seed %s\n", SEED_HOSTS[i]);
 }
 } catch (const std::exception& e) {
 LogError("SeedRegister: DELETE exception on seed %s: %s\n", SEED_HOSTS[i], e.what());
 }
 }
}

void RegisterNodeToSeed(const NodeRegistrationInfo& info)
{
 std::string effective_ip = info.public_ip;
 if (effective_ip.empty()) {
 effective_ip = "127.0.0.1";
 LogInfo("SeedRegister: No public IP, using fallback %s for registration\n", effective_ip.c_str());
 }

 g_registration_info = info;
 g_node_id = effective_ip + ":" + std::to_string(info.p2p_port);

 UniValue json(UniValue::VOBJ);
 json.pushKV("node_id", g_node_id);
 json.pushKV("role", RoleToString(info.role));
 json.pushKV("wallet_address", info.wallet_address);
 json.pushKV("p2p_port", info.p2p_port);
 json.pushKV("ws_port", info.ws_port);
 json.pushKV("model_name", info.model_name);
 json.pushKV("public_ip", effective_ip);

 UniValue caps(UniValue::VARR);
 for (const auto& cap : info.capabilities) {
 caps.push_back(cap);
 }
 json.pushKV("capabilities", caps);

 json.pushKV("timestamp", GetTime());

 std::string body = json.write();

 std::string response = HttpPost(REGISTER_PATH, body);

 if (!response.empty()) {
 g_registered = true;
 LogInfo("SeedRegister: Registered as %s with seed, node_id=%s, role=%s\n",
 info.wallet_address.substr(0, 16).c_str(),
 g_node_id.c_str(),
 RoleToString(info.role).c_str());
 }
}

void SendHeartbeatToSeed()
{
 if (!g_registered || g_node_id.empty()) return;

 UniValue json(UniValue::VOBJ);
 json.pushKV("node_id", g_node_id);
 json.pushKV("timestamp", GetTime());

 std::string body = json.write();

 HttpPost(HEARTBEAT_PATH, body);
 LogInfo("SeedRegister: Heartbeat sent for %s\n", g_node_id.c_str());
}

void QueryPeersFromSeed(const std::string& role_filter, const std::string& capability_filter)
{
 std::string query = "role=" + role_filter;
 if (!capability_filter.empty()) {
 query += "&has_capability=" + capability_filter;
 }

 std::string response = HttpGet(PEERS_PATH, query);

 if (!response.empty()) {
 UniValue resp;
 resp.read(response);

 if (resp.exists("peers")) {
 const auto& peers = resp["peers"].getValues();
 LogInfo("SeedRegister: Found %zu peers (role=%s, cap=%s)\n",
 peers.size(), role_filter.c_str(), capability_filter.c_str());

 for (const auto& peer : peers) {
 std::string peer_ip = peer["public_ip"].getValStr();
 int peer_port = peer["p2p_port"].getInt<int>();
 std::string peer_role = peer["role"].getValStr();
 LogInfo("SeedRegister: Discovered peer %s:%d (role=%s)\n",
 peer_ip.c_str(), peer_port, peer_role.c_str());
 }
 }
 }
}

void SetMinerWalletAddress(const std::string& addr)
{
 if (!addr.empty()) {
 g_wallet_address = addr; // Legacy backward compat

 // Add to multi-miner wallet list (dedup)
 std::lock_guard<std::mutex> lock(g_wallets_mutex);
 if (std::find(g_miner_wallets.begin(), g_miner_wallets.end(), addr) == g_miner_wallets.end()) {
 g_miner_wallets.push_back(addr);
 LogInfo("SeedRegister: Miner wallet added to tracking list: %s (total: %zu)\n",
 addr.substr(0, 16).c_str(), g_miner_wallets.size());
 }
 }
 const char* env_wallet = getenv("MINER_WALLET");
 if (env_wallet && strlen(env_wallet) > 0 && g_wallet_address.empty()) {
 g_wallet_address = env_wallet;
 std::lock_guard<std::mutex> lock(g_wallets_mutex);
 if (std::find(g_miner_wallets.begin(), g_miner_wallets.end(), g_wallet_address) == g_miner_wallets.end()) {
 g_miner_wallets.push_back(g_wallet_address);
 }
 }
 if (!g_wallet_address.empty()) {
 LogInfo("SeedRegister: Wallet address set to %s\n", g_wallet_address.c_str());
 }
}

void SetMinerModelName(const std::string& name)
{
 if (!name.empty()) {
 g_model_name = name;
 LogInfo("SeedRegister: Miner model name set to %s\n", g_model_name.c_str());
 }
}

std::string GetMinerWalletAddress()
{
 return g_wallet_address;
}

std::string GetMinerModelName()
{
 return g_model_name;
}

void UnregisterFromSeed()
{
 if (!g_registered || g_node_id.empty()) return;

 std::string query = "node_id=" + g_node_id;
 HttpDelete("/api/p2p/register", query);

 g_registered = false;
 LogInfo("SeedRegister: Node unregistered: %s\n", g_node_id.c_str());
}

NodeRole DetectNodeRole()
{
 if (g_role_forced) {
 return g_forced_role;
 }
 return NodeRole::NODE;
}

void SetNodeRole(NodeRole role)
{
 g_forced_role = role;
 g_role_forced = true;
}

void SetNodeContext(node::NodeContext* ctx)
{
 g_node_context = ctx;
}

static std::string GetBestPublicIP()
{
 // Probe OS routing for reachable IPv6 (RFC 4941 temporary), not EUI-64 stable addresses.
#ifdef WIN32
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
 std::string os_chosen_ip(buf);
 if (!os_chosen_ip.empty() && os_chosen_ip.find("fe80") != 0 &&
 os_chosen_ip.find("::1") != 0 && os_chosen_ip.find("ff") != 0 &&
 os_chosen_ip != "::") {
 closesocket(probe_sock);
 return os_chosen_ip;
 }
 }
 }
 closesocket(probe_sock);
 }
 }

 LogWarning("[SeedRegister] OS routing probe failed for IPv6; skipping interface enumeration to avoid unreachable IPv6");

 // Priority 2 (WIN32): IPv4 fallback via interface enumeration
 {
 ULONG outBufLen = 15000;
 PIP_ADAPTER_ADDRESSES pAddresses = nullptr;
 ULONG iterations = 0;

 do {
 pAddresses = (PIP_ADAPTER_ADDRESSES)malloc(outBufLen);
 if (!pAddresses) break;

 DWORD ret = GetAdaptersAddresses(AF_INET,
 GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
 nullptr, pAddresses, &outBufLen);
 if (ret == NO_ERROR) break;
 free(pAddresses);
 pAddresses = nullptr;
 if (ret != ERROR_BUFFER_OVERFLOW) break;
 iterations++;
 } while (iterations < 3);

 if (pAddresses) {
 std::string best_ipv4;
 PIP_ADAPTER_ADDRESSES pCurr = pAddresses;
 while (pCurr) {
 if (pCurr->IfType != IF_TYPE_SOFTWARE_LOOPBACK &&
 pCurr->OperStatus == IfOperStatusUp) {
 PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurr->FirstUnicastAddress;
 while (pUnicast) {
 if (pUnicast->Address.lpSockaddr->sa_family == AF_INET) {
 SOCKADDR_IN* addr4 = (SOCKADDR_IN*)pUnicast->Address.lpSockaddr;
 char buf[64] = {};
 inet_ntop(AF_INET, &addr4->sin_addr, buf, sizeof(buf));
 std::string ip(buf);
 if (!ip.empty() && ip.find("127.") != 0 &&
 ip.find("10.") != 0 && ip.find("192.168.") != 0 &&
 ip.find("172.") != 0) {
 if (best_ipv4.empty()) best_ipv4 = ip;
 }
 }
 pUnicast = pUnicast->Next;
 }
 }
 pCurr = pCurr->Next;
 }
 free(pAddresses);
 if (!best_ipv4.empty()) return best_ipv4;
 }
 }
#else
 // Use OS routing to find real public IPv6 (UDP socket + getsockname).
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
 LogInfo("[SeedRegister] OS routing chose public IPv6: %s", os_chosen_ip.c_str());
 close(probe_sock);
 return os_chosen_ip;
 }
 }
 }
 close(probe_sock);
 }

 // macOS/Linux: fallback to IPv4 interface enumeration only.
 struct ifaddrs *ifap = nullptr;
 if (getifaddrs(&ifap) == 0) {
 std::string best_ipv4_fallback;
 for (struct ifaddrs *ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
 if (!ifa->ifa_addr) continue;
 int family = ifa->ifa_addr->sa_family;
 if (family == AF_INET) {
 struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
 char buf[INET_ADDRSTRLEN] = {};
 inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
 std::string ip(buf);
 // Skip private ranges
 if (!ip.empty() && ip != "127.0.0.1" &&
 ip.find("10.") != 0 && ip.find("192.168.") != 0 &&
 ip.find("172.") != 0) {
 if (best_ipv4_fallback.empty()) best_ipv4_fallback = ip;
 }
 }
 }
 freeifaddrs(ifap);
 if (!best_ipv4_fallback.empty()) return best_ipv4_fallback;
 }
#endif

 return "";
}

static void ThreadSeedRegister()
{
 for (int attempt = 0; attempt < 12; ++attempt) {
 std::this_thread::sleep_for(std::chrono::seconds(5));

 std::string best_ip = GetBestPublicIP();

 if (!best_ip.empty()) {
 NodeRole role = DetectNodeRole();

 NodeRegistrationInfo info;
 info.public_ip = best_ip;
 info.role = role;
 info.p2p_port = 9333;
 info.ws_port = gArgs.GetIntArg("-apiport", 9313); // Gateway port for external API access

 if (role == NodeRole::MINER) {
 info.capabilities.emplace_back("llm_inference");
 info.model_name = g_model_name.empty() ? "Unknown" : g_model_name;
 info.wallet_address = g_wallet_address;
 }

 RegisterNodeToSeed(info);
 return;
 }
 }

 LogWarning("SeedRegister: Could not determine public IP after 60s\n");
}

void StartSeedRegisterThread()
{
 std::thread(&util::TraceThread, "seedregister", &ThreadSeedRegister).detach();
}



void SignalMinerActive()
{
 g_miner_last_active = GetTime();
 LogInfo("SeedRegister: Miner activity signaled\n");
}

int64_t GetMinerLastActive()
{
 return g_miner_last_active.load();
}

// TCP probe a single port to check if a miner API server is listening.
static bool ProbeMinerPortSingle(int port)
{
#ifdef _WIN32
 WSADATA wsaData;
 static bool wsa_initialized = false;
 if (!wsa_initialized) {
 int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
 if (wsaResult != 0) return false;
 wsa_initialized = true;
 }

 SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
 if (sock == INVALID_SOCKET) return false;

 struct sockaddr_in addr = {};
 addr.sin_family = AF_INET;
 addr.sin_port = htons(static_cast<uint16_t>(port));
 addr.sin_addr.s_addr = inet_addr("127.0.0.1");

 u_long nonblock = 1;
 ioctlsocket(sock, FIONBIO, &nonblock);
 int connResult = connect(sock, (sockaddr*)&addr, sizeof(addr));

 bool alive = false;
 if (connResult == 0) {
 alive = true;
 } else {
 int connErr = WSAGetLastError();
 if (connErr == WSAEWOULDBLOCK) {
 fd_set writeSet;
 FD_ZERO(&writeSet);
 FD_SET(sock, &writeSet);
 timeval tv = {1, 0};
 int selResult = select(0, NULL, &writeSet, NULL, &tv);
 if (selResult > 0) {
 int so_error = 0;
 int len = sizeof(so_error);
 getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
 alive = (so_error == 0);
 }
 }
 }
 closesocket(sock);
 return alive;
#else
 int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
 if (sock < 0) return false;

 struct sockaddr_in addr = {};
 addr.sin_family = AF_INET;
 addr.sin_port = htons(static_cast<uint16_t>(port));
 addr.sin_addr.s_addr = inet_addr("127.0.0.1");

 int flags = fcntl(sock, F_GETFL, 0);
 fcntl(sock, F_SETFL, flags | O_NONBLOCK);
 int connResult = connect(sock, (struct sockaddr*)&addr, sizeof(addr));

 bool alive = false;
 if (connResult == 0) {
 alive = true;
 } else if (errno == EINPROGRESS) {
 fd_set writeSet;
 FD_ZERO(&writeSet);
 FD_SET(sock, &writeSet);
 timeval tv = {1, 0};
 int selResult = select(sock + 1, NULL, &writeSet, NULL, &tv);
 if (selResult > 0) {
 int so_error = 0;
 socklen_t len = sizeof(so_error);
 getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
 alive = (so_error == 0);
 }
 }
 close(sock);
 return alive;
#endif
}

// Probe ports 9332-9342 to check if ANY miner API server is listening.
static bool ProbeMinerPort()
{
 for (int port = MINER_LOCAL_PORT_START; port <= MINER_LOCAL_PORT_END; ++port) {
 if (ProbeMinerPortSingle(port)) return true;
 }
 return false;
}

// Get model name from a specific local miner port.
static std::string GetModelFromMinerPort(int port)
{
#ifdef WIN32
 SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
 if (sock == INVALID_SOCKET) return "";

 u_long mode = 1;
 ioctlsocket(sock, FIONBIO, &mode);

 struct sockaddr_in addr;
 memset(&addr, 0, sizeof(addr));
 addr.sin_family = AF_INET;
 addr.sin_port = htons(static_cast<uint16_t>(port));
 inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

 int cr = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
 if (cr == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
 fd_set ws; FD_ZERO(&ws); FD_SET(sock, &ws);
 timeval tv; tv.tv_sec = 2; tv.tv_usec = 0;
 if (select((int)sock + 1, nullptr, &ws, nullptr, &tv) > 0) cr = 0;
 }
 mode = 0;
 ioctlsocket(sock, FIONBIO, &mode);

 if (cr != 0) { closesocket(sock); return ""; }

 std::string req = "GET /api/v1/miners HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
 send(sock, req.c_str(), (int)req.size(), 0);

 char buf[4096] = {};
 int total = 0;
 while (total < (int)sizeof(buf) - 1) {
 int r = recv(sock, buf + total, sizeof(buf) - total - 1, 0);
 if (r <= 0) break;
 total += r;
 }
 closesocket(sock);

 std::string response(buf, total);
 size_t hdr_end = response.find("\r\n\r\n");
 if (hdr_end == std::string::npos) return "";
 std::string body = response.substr(hdr_end + 4);

 size_t online_pos = body.find("\"status\":\"online\"");
 if (online_pos != std::string::npos) {
 size_t model_pos = body.rfind("\"model_name\"", online_pos);
 if (model_pos != std::string::npos) {
 size_t colon = body.find(":", model_pos);
 size_t start = colon + 1;
 while (start < body.size() && (body[start] == ' ' || body[start] == '"')) start++;
 size_t end = body.find("\"", start);
 if (end != std::string::npos) return body.substr(start, end - start);
 }
 }
#endif
 return "";
}

static std::string GetModelFromMiner()
{
 for (int port = MINER_LOCAL_PORT_START; port <= MINER_LOCAL_PORT_END; ++port) {
 std::string model = GetModelFromMinerPort(port);
 if (!model.empty()) return model;
 }
 return "";
}

// Check if a specific wallet is registered on a single local miner API port.
static bool IsWalletInMinerAPIPort(const std::string& wallet, int port)
{
 if (wallet.empty()) return false;

#ifdef WIN32
 SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
 if (sock == INVALID_SOCKET) return false;

 u_long mode = 1;
 ioctlsocket(sock, FIONBIO, &mode);

 struct sockaddr_in addr;
 memset(&addr, 0, sizeof(addr));
 addr.sin_family = AF_INET;
 addr.sin_port = htons(static_cast<uint16_t>(port));
 inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

 int cr = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
 if (cr == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
 fd_set ws; FD_ZERO(&ws); FD_SET(sock, &ws);
 timeval tv; tv.tv_sec = 2; tv.tv_usec = 0;
 if (select((int)sock + 1, nullptr, &ws, nullptr, &tv) > 0) cr = 0;
 }
 mode = 0;
 ioctlsocket(sock, FIONBIO, &mode);

 if (cr != 0) { closesocket(sock); return false; }

 std::string req = "GET /api/v1/miners HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
 send(sock, req.c_str(), (int)req.size(), 0);

 char buf[8192] = {};
 int total = 0;
 while (total < (int)sizeof(buf) - 1) {
 int r = recv(sock, buf + total, sizeof(buf) - total - 1, 0);
 if (r <= 0) break;
 total += r;
 }
 closesocket(sock);

 std::string response(buf, total);
 size_t hdr_end = response.find("\r\n\r\n");
 if (hdr_end == std::string::npos) return false;
 std::string body = response.substr(hdr_end + 4);
 return body.find(wallet) != std::string::npos;
#else
 int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
 if (sock < 0) return false;

 int flags = fcntl(sock, F_GETFL, 0);
 fcntl(sock, F_SETFL, flags | O_NONBLOCK);

 struct sockaddr_in addr = {};
 addr.sin_family = AF_INET;
 addr.sin_port = htons(static_cast<uint16_t>(port));
 inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

 int cr = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
 if (cr < 0 && errno == EINPROGRESS) {
 fd_set ws; FD_ZERO(&ws); FD_SET(sock, &ws);
 timeval tv; tv.tv_sec = 2; tv.tv_usec = 0;
 if (select(sock + 1, nullptr, &ws, nullptr, &tv) > 0) {
 int so_error = 0; socklen_t len = sizeof(so_error);
 getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
 if (so_error == 0) cr = 0;
 }
 }
 flags = fcntl(sock, F_GETFL, 0);
 fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);

 if (cr != 0) { close(sock); return false; }

 std::string req = "GET /api/v1/miners HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
 send(sock, req.c_str(), req.size(), 0);

 char buf[8192] = {};
 int total = 0;
 while (total < (int)sizeof(buf) - 1) {
 int r = recv(sock, buf + total, sizeof(buf) - total - 1, 0);
 if (r <= 0) break;
 total += r;
 }
 close(sock);

 std::string response(buf, total);
 size_t hdr_end = response.find("\r\n\r\n");
 if (hdr_end == std::string::npos) return false;
 std::string body = response.substr(hdr_end + 4);
 return body.find(wallet) != std::string::npos;
#endif
}

// Check if a specific wallet address is registered in any local miner API.
// Scans ports 9332-9342 to handle multiple miners on the same machine.
static bool IsWalletInMinerAPI(const std::string& wallet)
{
 if (wallet.empty()) return false;
 for (int port = MINER_LOCAL_PORT_START; port <= MINER_LOCAL_PORT_END; ++port) {
 if (IsWalletInMinerAPIPort(wallet, port)) return true;
 }
 return false;
}

// Send heartbeat for a single miner wallet to the web server.
static void SendMinerHeartbeatToWeb(const std::string& wallet, const std::string& effective_ip)
{
 if (wallet.empty()) return;

 // Verify this specific wallet is alive on some local miner port.
 if (!IsWalletInMinerAPI(wallet)) {
 LogInfo("SeedRegister: Wallet %s not found on any miner API port ?skipping heartbeat\n",
 wallet.substr(0, 16).c_str());
 return;
 }

 int64_t now = GetTime();

 UniValue json(UniValue::VOBJ);
 json.pushKV("wallet_address", wallet);
 json.pushKV("model_name", GetModelFromMiner());
 json.pushKV("p2p_port", g_registration_info.p2p_port);
 json.pushKV("ws_port", g_registration_info.ws_port);
 json.pushKV("status", "online");
 json.pushKV("role", "miner");
 json.pushKV("node_id", g_node_id);

 std::string detected_ip = !effective_ip.empty() ? effective_ip : GetBestPublicIP();
 if (!detected_ip.empty()) {
 json.pushKV("public_ip", detected_ip);
 }

 UniValue caps(UniValue::VARR);
 caps.push_back("llm_inference");
 caps.push_back("pow");
 json.pushKV("capabilities", caps);
 json.pushKV("timestamp", now);

 std::string body = json.write();
 const char* env_secret = getenv("TKNC_NOTIFY_SECRET");
 std::string authToken = env_secret ? std::string(env_secret) : "";
 HttpPostToAll("/api/node/miner-notify", body, authToken);
 LogInfo("SeedRegister: Miner ONLINE with seed web (wallet=%s)\n",
 wallet.substr(0, 16).c_str());
}

static void RegisterLocalMinerToWeb()
{
 std::string effective_ip = g_registration_info.public_ip;
 if (effective_ip.empty()) effective_ip = "127.0.0.1";

 // Quick health check: is ANY miner API server running on any port?
 bool any_alive = ProbeMinerPort();

 if (!any_alive) {
 int64_t now = GetTime();
 int64_t last_active = g_miner_last_active.load();
 int64_t idle_seconds = (last_active > 0) ? (now - last_active) : INT64_MAX;
 any_alive = (idle_seconds < 5);
 }

 if (!any_alive) {
 static int64_t last_offline_log = 0;
 int64_t now = GetTime();
 if (now - last_offline_log > 60) {
 last_offline_log = now;
 LogInfo("SeedRegister: All miners OFFLINE ?skipping heartbeat (ports %d-%d not responding)\n",
 MINER_LOCAL_PORT_START, MINER_LOCAL_PORT_END);
 }
 return;
 }

 // Send heartbeat for EACH tracked miner wallet.
 // Copy wallets under lock to avoid holding mutex during network I/O.
 std::vector<std::string> wallets_copy;
 {
 std::lock_guard<std::mutex> lock(g_wallets_mutex);
 wallets_copy = g_miner_wallets;
 }

 // Fallback: if no wallets tracked, use legacy g_wallet_address.
 if (wallets_copy.empty() && !g_wallet_address.empty()) {
 wallets_copy.push_back(g_wallet_address);
 }

 for (const auto& wallet : wallets_copy) {
 SendMinerHeartbeatToWeb(wallet, effective_ip);
 }
}

static std::string g_last_reported_public_ip; // Track IP for change detection

static void ThreadP2PMaintenance()
{
 LogInfo("SeedRegister: ThreadP2PMaintenance started (miner-aware heartbeat + IPv6 change monitor)\n");

 const char* env_wallet = getenv("MINER_WALLET");
 if (env_wallet && strlen(env_wallet) > 0) {
 g_wallet_address = env_wallet;
 LogInfo("SeedRegister: Got wallet from env MINER_WALLET: %s\n", g_wallet_address.c_str());
 }

 while (true) {
 // Check if any miner wallet is tracked (multi-miner aware).
 bool has_wallets;
 {
 std::lock_guard<std::mutex> lock(g_wallets_mutex);
 has_wallets = !g_miner_wallets.empty();
 }
 if (has_wallets || !g_wallet_address.empty()) {
 RegisterLocalMinerToWeb();
 }

 // Re-detect public IP each cycle; update registration immediately if changed.
 std::string current_ip = GetBestPublicIP();
 if (!current_ip.empty() && current_ip != g_last_reported_public_ip &&
 !g_last_reported_public_ip.empty()) {
 LogWarning("SeedRegister: *** PUBLIC IP CHANGED: %s ?%s (updating registration NOW) ***\n",
 g_last_reported_public_ip.c_str(), current_ip.c_str());
 g_registration_info.public_ip = current_ip;
 g_node_id = current_ip + ":" + std::to_string(g_registration_info.p2p_port);
 g_registered = false; // Force re-register with new IP
 RegisterNodeToSeed(g_registration_info);
 // Also immediately push new IP to Web
 if (!g_wallet_address.empty()) {
 RegisterLocalMinerToWeb();
 }
 }
 if (!current_ip.empty()) {
 g_last_reported_public_ip = current_ip;
 }

 std::this_thread::sleep_for(std::chrono::seconds(60));
 }
}

void StartP2PMaintenanceThread()
{
 std::thread(&util::TraceThread, "p2pmaintain", &ThreadP2PMaintenance).detach();
}
