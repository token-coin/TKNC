#include <miner/miner.h>
#include <miner/mode.h>
#include <miner/mode_switcher.h>
#include <pow/tknchash.h>
#include <pow/gpu_transformerpow.h>
#include <pow/opencl_miner.h>
#include <pow/real_gpu_miner.h>
#include <pow/difficulty.h>
#include <net/api_protocol.h>
#include <model/loader.h>
#include <model/gpu_memory.h>
#include <economics/emission.h>
#include <util/log.h>
#include <chain.h>
#include <chainparams.h>
#include <common/system.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <hash.h>
#include <node/miner.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <util/time.h>
#include <util/strencodings.h>
#include <util/fs_helpers.h>  // GetExeDir() for cookie path
#include <arith_uint256.h>
#include <script/script.h>
#include <addresstype.h>
#include <key_io.h>
#include <streams.h>
#include <core_io.h>
#include <univalue.h>

#include <chrono>
#include <ctime>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <algorithm>
#include <cinttypes>
#include <sstream>
#include <fstream>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/keyvalq_struct.h>
#include <event2/http.h>
#include <event2/http_struct.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif


#ifndef SER_NETWORK
#define SER_NETWORK 0
#endif

#ifndef PROTOCOL_VERSION
#define PROTOCOL_VERSION 70016
#endif

static std::atomic<bool> fGenerateTKNC(false);
static ModeSwitcher g_mode_switcher;
static ModelLoader g_model_loader;

uint64_t g_request_counter = 0;
static std::string g_current_model_name = "min-model";

static std::atomic<int64_t> g_blocks_found(0);
std::atomic<int64_t> g_current_mining_height(0);

static std::string Base64Encode(const std::string& input) {
    return EncodeBase64(input);
}
static std::atomic<uint64_t> g_total_hashes(0);
static std::chrono::time_point<std::chrono::steady_clock> g_mining_start_time;

// Global block template (mutex-protected, refreshed via RPC — not trivially copyable for atomic).
static node::CBlockTemplate g_block_template;
static std::mutex g_template_mutex;

// D-C01-FIX: Read credentials from environment variables (NEVER hardcode in production)
static std::string g_rpc_user = []() {
    const char* env = std::getenv("TKNC_RPC_USER");
    return env ? std::string(env) : "tkncadmin";
}();
static std::string g_rpc_password = []() {
    const char* env = std::getenv("TKNC_RPC_PASS");
    return env ? std::string(env) : "tkncpass123";
}();
static std::string g_rpc_connect = "127.0.0.1";
static int g_rpc_port = 9331;
static std::string g_mining_address;

ModelLoader& GetModelLoader() {
    return g_model_loader;
}

ModeSwitcher& GetModeSwitcher() {
    return g_mode_switcher;
}

void SetRPCConfig(const std::string& rpcUser, const std::string& rpcPassword,
                  const std::string& rpcConnect, int rpcPort,
                  const std::string& miningAddress) {
    g_rpc_user = rpcUser;
    g_rpc_password = rpcPassword;
    g_rpc_connect = rpcConnect;
    g_rpc_port = rpcPort;
    g_mining_address = miningAddress;
}

static void PrintMiningStatus(int64_t height, double hashrate, float gpu_load, int req_count) {
    static double floor_h = 0.0;
    static float floor_g = 0.0f;
    if (hashrate >= 1000.0) floor_h = hashrate;
    if (gpu_load >= 1.0f) floor_g = gpu_load;
    if (hashrate < 1.0 && floor_h > 0.0) hashrate = floor_h;
    if (gpu_load < 1.0f && floor_g > 0.0f) gpu_load = floor_g;
    const char* unit = "H/s";
    double display_hashrate = hashrate;
    if (hashrate >= 1e6) { display_hashrate = hashrate / 1e6; unit = "MH/s"; }
    else if (hashrate >= 1e3) { display_hashrate = hashrate / 1e3; unit = "KH/s"; }
    printf("[Height:%" PRId64 "] [pow] [Rewards:1] [%.2f %s] [GPU:%.1f%%] [Req:%d]\n",
           height, display_hashrate, unit, gpu_load, req_count);
    fflush(stdout);
}

void PrintInferenceStatus(int64_t height, double hashrate, float gpu_load, int req_count) {
    const char* unit = "H/s";
    double display_hashrate = hashrate;
    if (hashrate >= 1e6) { display_hashrate = hashrate / 1e6; unit = "MH/s"; }
    else if (hashrate >= 1e3) { display_hashrate = hashrate / 1e3; unit = "KH/s"; }
    printf("[Height:%" PRId64 "] [LLM] [Rewards:1] [%.2f %s] [GPU:%.1f%%] [Req:%d]\n",
           height, display_hashrate, unit, gpu_load, req_count);
    fflush(stdout);
}

// Read node's RPC cookie file for authentication (matches node behavior)
// Cookie file: <exe>/.cookie  (format: __cookie__:hex_password)
// The node writes cookie to GetExeDir()/.cookie (same dir as tkncd.exe)
// so the miner must read from the same location.
bool TryReadCookieAuth(std::string& outUser, std::string& outPass)
{
    fs::path cookieFile = GetExeDir() / ".cookie";

    std::ifstream file(cookieFile.std_path());
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    if (!std::getline(file, line) || line.empty()) {
        return false;
    }

    // Format: __cookie__:hex_password
    size_t colon = line.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    outUser = line.substr(0, colon);
    outPass = line.substr(colon + 1);

    LogInfo("[RPC] Using cookie auth from: %s", PathToString(cookieFile).c_str());
    return true;
}

static std::string CallRPC(const std::string& method, const std::string& params = "[]") {
    std::string jsonBody;
    if (params.empty() || params == "[]") {
        jsonBody = "{\"jsonrpc\":\"1.0\",\"method\":\"" + method + "\",\"params\":[],\"id\":1}";
    } else if (params[0] == '[' || params[0] == '{') {
        jsonBody = "{\"jsonrpc\":\"1.0\",\"method\":\"" + method + "\",\"params\":" + params + ",\"id\":1}";
    } else {
        jsonBody = "{\"jsonrpc\":\"1.0\",\"method\":\"" + method + "\",\"params\":[\"" + params + "\"],\"id\":1}";
    }

    // Ensure correct address
    std::string rpcHost = g_rpc_connect;
    if (rpcHost.empty() || rpcHost == "127") {
        rpcHost = "127.0.0.1";
    }

    // Initialize Winsock / Socket
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        LogError("Miner: CallRPC - WSAStartup failed");
        return "";
    }

    // Create socket
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        LogError("Miner: CallRPC - socket creation failed, error=%d", WSAGetLastError());
        WSACleanup();
        return "";
    }

    // Set timeout (10 seconds)
    DWORD timeout = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

    // Connect to server
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(g_rpc_port);
    inet_pton(AF_INET, rpcHost.c_str(), &serverAddr.sin_addr);

    if (connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        LogError("Miner: CallRPC - connect failed to %s:%d, error=%d",
                 rpcHost.c_str(), g_rpc_port, WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return "";
    }

    // Build HTTP request
    std::string auth = g_rpc_user + ":" + g_rpc_password;
    std::string encodedAuth = Base64Encode(auth);

    std::stringstream httpRequest;
    httpRequest << "POST / HTTP/1.1\r\n";
    httpRequest << "Host: " << rpcHost << ":" << g_rpc_port << "\r\n";
    httpRequest << "Content-Type: application/json\r\n";
    httpRequest << "Authorization: Basic " << encodedAuth << "\r\n";
    httpRequest << "Connection: close\r\n";
    httpRequest << "Content-Length: " << jsonBody.size() << "\r\n";
    httpRequest << "\r\n";
    httpRequest << jsonBody;

    std::string requestStr = httpRequest.str();

    // Send request
    int sendResult = send(sock, requestStr.c_str(), (int)requestStr.size(), 0);
    if (sendResult == SOCKET_ERROR) {
        LogError("Miner: CallRPC - send failed, error=%d", WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return "";
    }

    // Receive response
    std::string response;
    char buffer[4096];
    int bytesRead;

    while ((bytesRead = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytesRead] = '\0';
        response += buffer;
    }

    closesocket(sock);
    WSACleanup();
#else
    // Create socket (POSIX)
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        LogError("Miner: CallRPC - socket creation failed, error=%d", errno);
        return "";
    }

    // Set timeout (10 seconds)
    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    // Connect to server
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(g_rpc_port);
    inet_pton(AF_INET, rpcHost.c_str(), &serverAddr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        LogError("Miner: CallRPC - connect failed to %s:%d, error=%d",
                 rpcHost.c_str(), g_rpc_port, errno);
        close(sock);
        return "";
    }

    // Build HTTP request
    std::string auth = g_rpc_user + ":" + g_rpc_password;
    std::string encodedAuth = Base64Encode(auth);

    std::stringstream httpRequest;
    httpRequest << "POST / HTTP/1.1\r\n";
    httpRequest << "Host: " << rpcHost << ":" << g_rpc_port << "\r\n";
    httpRequest << "Content-Type: application/json\r\n";
    httpRequest << "Authorization: Basic " << encodedAuth << "\r\n";
    httpRequest << "Connection: close\r\n";
    httpRequest << "Content-Length: " << jsonBody.size() << "\r\n";
    httpRequest << "\r\n";
    httpRequest << jsonBody;

    std::string requestStr = httpRequest.str();

    // Send request
    ssize_t sendResult = send(sock, requestStr.c_str(), requestStr.size(), 0);
    if (sendResult < 0) {
        LogError("Miner: CallRPC - send failed, error=%d", errno);
        close(sock);
        return "";
    }

    // Receive response
    std::string response;
    char buffer[4096];
    ssize_t bytesRead;

    while ((bytesRead = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytesRead] = '\0';
        response += buffer;
    }

    close(sock);
#endif

    // Parse HTTP response to extract body
    std::string responseBody;
    size_t bodyStart = response.find("\r\n\r\n");
    if (bodyStart != std::string::npos) {
        responseBody = response.substr(bodyStart + 4);

        if (response.find("200 OK") == std::string::npos) {
            LogWarning("Miner: CallRPC - HTTP error for method=%s, status line: %s", 
                        method.c_str(), response.substr(0, response.find("\r\n")).c_str());
        }
    } else {
        LogError("Miner: CallRPC - invalid HTTP response for method=%s", method.c_str());
    }

    return responseBody;
}

// Fetch real block template from node via RPC
static bool FetchBlockTemplateFromRPC() {
    std::string response = CallRPC("getblocktemplate", "[{\"rules\":[\"segwit\"]}]");

    if (response.empty()) {
        LogError("Miner: Failed to fetch block template - empty response");
        return false;
    }
    
    UniValue json_response;
    if (!json_response.read(response)) {
        LogError("Miner: Failed to parse block template JSON: %s", response.substr(0, 100).c_str());
        return false;
    }
    
    if (json_response.exists("error") && !json_response["error"].isNull()) {
        UniValue error = json_response["error"];
        LogError("Miner: RPC error fetching block template: %s", error.write().c_str());
        return false;
    }
    
    if (!json_response.exists("result") || json_response["result"].isNull()) {
        LogError("Miner: Block template result is null/missing, full response: %s", response.substr(0, 300).c_str());
        return false;
    }
    
    UniValue result = json_response["result"];
    
    if (!result.exists("height") || !result.exists("bits")) {
        LogError("Miner: Block template missing required fields (height/bits), has height: %s, has bits: %s", 
                 result.exists("height") ? "yes" : "no", result.exists("bits") ? "yes" : "no");
        return false;
    }
    
    auto real_template = std::make_unique<node::CBlockTemplate>();
    
    int64_t realHeight = result["height"].getInt<int64_t>();
    g_current_mining_height.store(realHeight);
    g_mode_switcher.SetBlockHeight(realHeight);
    
    uint32_t nBits = 0;
    if (result["bits"].isStr()) {
        std::string bitsStr = result["bits"].get_str();
        nBits = static_cast<uint32_t>(std::stoul(bitsStr, nullptr, 16));
    } else {
        nBits = static_cast<uint32_t>(result["bits"].getInt<int64_t>());
    }
    real_template->block.nBits = nBits;
    
    if (result.exists("version")) {
        real_template->block.nVersion = static_cast<int32_t>(result["version"].getInt<int>());
    }
    
    if (result.exists("previousblockhash")) {
        auto prevHashOpt = uint256::FromHex(result["previousblockhash"].get_str());
        if (prevHashOpt.has_value()) {
            real_template->block.hashPrevBlock = prevHashOpt.value();
        } else {
            LogWarning("Miner: Failed to parse previousblockhash, using zero");
            real_template->block.hashPrevBlock = uint256();
        }
    }
    
    real_template->block.nTime = GetTime();
    
    CMutableTransaction coinbaseTx;
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].scriptSig = CScript() << realHeight << std::vector<unsigned char>(32, 0);
    coinbaseTx.vout.resize(2);

    CAmount blockReward = GetTKNCBlockSubsidy(realHeight);

    // Respect emission schedule: after year 9, reward is 0 (fees only).
    if (blockReward <= 0) {
        LogWarning("Miner: Block reward at height %d is zero - emission complete, only fees", realHeight);
        blockReward = 0;  // Transaction fees only after emission ends
    }

    // GetTKNCBlockSubsidy returns satoshis — no COIN multiplier needed.

    CAmount minerReward = blockReward * 90 / 100;
    CAmount teamReward = blockReward - minerReward;

    coinbaseTx.vout[0].nValue = minerReward;
    
    CScript scriptPubKey;
    CTxDestination dest = DecodeDestination(g_mining_address);
    scriptPubKey = GetScriptForDestination(dest);
    coinbaseTx.vout[0].scriptPubKey = scriptPubKey;

    CTxDestination teamDest = DecodeDestination(TKNC_TEAM_WALLET_ADDRESS);
    CScript teamScript = GetScriptForDestination(teamDest);
    coinbaseTx.vout[1].nValue = teamReward;
    coinbaseTx.vout[1].scriptPubKey = teamScript;

    real_template->block.vtx.push_back(MakeTransactionRef(std::move(coinbaseTx)));

    if (result.exists("transactions") && result["transactions"].isArray()) {
        const UniValue& txs = result["transactions"].get_array();
        for (size_t i = 0; i < txs.size(); i++) {
            const UniValue& txEntry = txs[i];
            if (txEntry.exists("data")) {
                CMutableTransaction mtx;
                std::string hexData = txEntry["data"].get_str();
                if (DecodeHexTx(mtx, hexData, false, true)) {
                    real_template->block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
                } else {
                    LogWarning("Miner: Failed to decode mempool tx #%d, skipping", i);
                }
            }
        }
        LogInfo("Miner: Block template includes %d mempool transaction(s)", txs.size());
    }

    {
        uint256 witnessroot = BlockWitnessMerkleRoot(real_template->block);
        std::vector<unsigned char> witness_nonce(32, 0x00);
        CHash256().Write(witnessroot).Write(witness_nonce).Finalize(witnessroot);
        CTxOut commitment_out;
        commitment_out.nValue = 0;
        commitment_out.scriptPubKey.resize(MINIMUM_WITNESS_COMMITMENT);
        commitment_out.scriptPubKey[0] = OP_RETURN;
        commitment_out.scriptPubKey[1] = 0x24;
        commitment_out.scriptPubKey[2] = 0xaa;
        commitment_out.scriptPubKey[3] = 0x21;
        commitment_out.scriptPubKey[4] = 0xa9;
        commitment_out.scriptPubKey[5] = 0xed;
        memcpy(&commitment_out.scriptPubKey[6], witnessroot.begin(), 32);

        CMutableTransaction coinbase_with_commitment(*real_template->block.vtx[0]);
        coinbase_with_commitment.vout.push_back(commitment_out);
        coinbase_with_commitment.vin[0].scriptWitness.stack.resize(1);
        coinbase_with_commitment.vin[0].scriptWitness.stack[0] = witness_nonce;
        real_template->block.vtx[0] = MakeTransactionRef(std::move(coinbase_with_commitment));
        LogInfo("Miner: Added witness commitment to coinbase (segwit enabled)");
    }

    real_template->block.hashMerkleRoot = BlockMerkleRoot(real_template->block);

    {
        std::lock_guard<std::mutex> lock(g_template_mutex);
        g_block_template = *real_template;
    }

    return true;
}

bool SubmitBlockToNode(const std::string& hexBlock) {
    std::string jsonBody = "{\"jsonrpc\":\"1.0\",\"method\":\"submitblock\",\"params\":[\"" + hexBlock + "\"],\"id\":1}";

    std::string rpcHost = g_rpc_connect;
    if (rpcHost.empty() || rpcHost == "127") {
        rpcHost = "127.0.0.1";
    }

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        LogError("Miner: SubmitBlock - WSAStartup failed");
        return false;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        LogError("Miner: SubmitBlock - socket creation failed, error=%d", WSAGetLastError());
        WSACleanup();
        return false;
    }

    DWORD timeout = 15000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(g_rpc_port);
    inet_pton(AF_INET, rpcHost.c_str(), &serverAddr.sin_addr);

    if (connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        LogError("Miner: SubmitBlock - connect failed to %s:%d, error=%d",
                 rpcHost.c_str(), g_rpc_port, WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return false;
    }

    std::string auth = g_rpc_user + ":" + g_rpc_password;
    std::string encodedAuth = Base64Encode(auth);

    std::stringstream httpRequest;
    httpRequest << "POST / HTTP/1.1\r\n";
    httpRequest << "Host: " << rpcHost << ":" << g_rpc_port << "\r\n";
    httpRequest << "Content-Type: application/json\r\n";
    httpRequest << "Authorization: Basic " << encodedAuth << "\r\n";
    httpRequest << "Connection: close\r\n";
    httpRequest << "Content-Length: " << jsonBody.size() << "\r\n";
    httpRequest << "\r\n";
    httpRequest << jsonBody;

    std::string requestStr = httpRequest.str();

    int sendResult = send(sock, requestStr.c_str(), (int)requestStr.size(), 0);
    if (sendResult == SOCKET_ERROR) {
        LogError("Miner: SubmitBlock - send failed, error=%d", WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return false;
    }

    std::string response;
    std::vector<char> buffer(8192);  // Heap allocation to avoid stack overflow
    int bytesRead;
    while ((bytesRead = recv(sock, buffer.data(), buffer.size() - 1, 0)) > 0) {
        response.append(buffer.data(), bytesRead);
    }

    closesocket(sock);
    WSACleanup();
#else
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        LogError("Miner: SubmitBlock - socket creation failed, error=%d", errno);
        return false;
    }

    struct timeval timeout;
    timeout.tv_sec = 15;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(g_rpc_port);
    inet_pton(AF_INET, rpcHost.c_str(), &serverAddr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        LogError("Miner: SubmitBlock - connect failed to %s:%d, error=%d",
                 rpcHost.c_str(), g_rpc_port, errno);
        close(sock);
        return false;
    }

    std::string auth = g_rpc_user + ":" + g_rpc_password;
    std::string encodedAuth = Base64Encode(auth);

    std::stringstream httpRequest;
    httpRequest << "POST / HTTP/1.1\r\n";
    httpRequest << "Host: " << rpcHost << ":" << g_rpc_port << "\r\n";
    httpRequest << "Content-Type: application/json\r\n";
    httpRequest << "Authorization: Basic " << encodedAuth << "\r\n";
    httpRequest << "Connection: close\r\n";
    httpRequest << "Content-Length: " << jsonBody.size() << "\r\n";
    httpRequest << "\r\n";
    httpRequest << jsonBody;

    std::string requestStr = httpRequest.str();

    ssize_t sendResult = send(sock, requestStr.c_str(), requestStr.size(), 0);
    if (sendResult < 0) {
        LogError("Miner: SubmitBlock - send failed, error=%d", errno);
        close(sock);
        return false;
    }

    std::string response;
    std::vector<char> buffer(8192);  // Heap allocation to avoid stack overflow
    ssize_t bytesRead;
    while ((bytesRead = recv(sock, buffer.data(), buffer.size() - 1, 0)) > 0) {
        response.append(buffer.data(), bytesRead);
    }

    close(sock);
#endif

    std::string responseBody;
    size_t bodyStart = response.find("\r\n\r\n");
    if (bodyStart != std::string::npos) {
        responseBody = response.substr(bodyStart + 4);
    }

    bool success = false;

    if (response.find("200 OK") != std::string::npos && !responseBody.empty()) {
        UniValue jsonResp;
        if (jsonResp.read(responseBody)) {
            if (jsonResp.exists("result")) {
                const UniValue& result = jsonResp["result"];
                if (result.isNull()) {
                    success = true;
                } else if (result.isStr()) {
                    std::string rejectReason = result.get_str();
                    if (rejectReason == "duplicate") {
                        LogWarning("Miner: Block is duplicate (already known)");
                    } else {
                        LogError("Miner: Block rejected: %s", rejectReason.c_str());
                    }
                }
            }
        }
    } else {
        LogError("Miner: submitblock HTTP error, response: %s", response.substr(0, 200).c_str());
    }

    return success;
}

static void MinerThread(const CChainParams& chainparams) {
    util::ThreadRename("tknc-miner");

    if (g_mining_address.empty()) {
        std::cerr << "FATAL: No mining address configured. Use -wallet=<address>\n";
        return;
    }
    CTxDestination dest_check = DecodeDestination(g_mining_address);
    if (!IsValidDestination(dest_check)) {
        std::cerr << "FATAL: Invalid mining address: " << g_mining_address << "\n";
        return;
    }

    g_mining_start_time = std::chrono::steady_clock::now();

    GPUMemoryManager gpu_mgr;
    bool gpu_detected = gpu_mgr.InitializeGPU(0);

    if (!gpu_detected) {
        LogWarning("Miner: LLM GPU (Vulkan/CUDA) not available — PoW-only mode continues");
    }

    GPUStats gpu_stats = gpu_mgr.GetGPUStats(0);
    uint64_t vram_gb = gpu_stats.memory.total / (1024 * 1024 * 1024);
    if (vram_gb == 0) vram_gb = gpu_stats.memory.total / (1024 * 1024);

    if (!OpenCLMiner::Initialize()) {
        std::cerr << "FATAL: TokenHash OpenCL GPU unavailable and no LLM GPU fallback\n";
        return;
    }

    try {
    if (!FetchBlockTemplateFromRPC()) {
        LogWarning("Miner: Block template fetch failed, using defaults");
    }
    
    node::CBlockTemplate block_template;
    {
        std::lock_guard<std::mutex> lock(g_template_mutex);
        block_template = g_block_template;
    }
    
    arith_uint256 target;
    target.SetCompact(block_template.block.nBits);

    uint64_t total_hashes = 0;
    auto mining_start = std::chrono::steady_clock::now();
    auto last_stats_time = std::chrono::steady_clock::now();
    auto last_template_refresh = std::chrono::steady_clock::now();
    auto last_height_sync = std::chrono::steady_clock::now();
    auto last_ibd_check = std::chrono::steady_clock::now();
    uint64_t last_total_hashes = 0;  // For hashrate calculation (5-second window)

    // Node disconnect detection: stop mining when node is unreachable
    int consecutive_rpc_failures = 0;
    const int MAX_RPC_FAILURES = 3;  // Stop after 3 consecutive failures

    while (fGenerateTKNC) {
        auto now = std::chrono::steady_clock::now();

        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_ibd_check).count() >= 30) {
            std::string bcInfo = CallRPC("getblockchaininfo");
            if (!bcInfo.empty()) {
                consecutive_rpc_failures = 0;  // Reset on success
                UniValue bcResp;
                if (bcResp.read(bcInfo)) {
                    if (bcResp.exists("result") && bcResp["result"].exists("initialblockdownload")) {
                        bool isIBD = bcResp["result"]["initialblockdownload"].get_bool();
                        if (isIBD) {
                            LogError("Miner: Node re-entered IBD state. Pausing mining until sync completes.");
                            fprintf(stderr, "\n  Node re-entered sync mode (IBD). Mining paused.\n");
                            fprintf(stderr, "   Waiting for node to finish syncing...\n\n");
                            std::this_thread::sleep_for(std::chrono::seconds(10));
                            continue;
                        }
                    }
                }
            } else {
                consecutive_rpc_failures++;
                LogWarning("Miner: Node unreachable (IBD check failure %d/%d, node=%s:%d)",
                           consecutive_rpc_failures, MAX_RPC_FAILURES, g_rpc_connect.c_str(), g_rpc_port);
                if (consecutive_rpc_failures >= MAX_RPC_FAILURES) {
                    LogError("Miner: Node %s:%d unreachable after %d consecutive failures. STOPPING MINING to save power.",
                             g_rpc_connect.c_str(), g_rpc_port, consecutive_rpc_failures);
                    fprintf(stderr, "\n[X] NODE DISCONNECTED: %s:%d unreachable. Mining stopped.\n", g_rpc_connect.c_str(), g_rpc_port);
                    fprintf(stderr, "    Blocks cannot be submitted without a node. Exiting miner.\n\n");
                    fGenerateTKNC = false;
                    break;
                }
            }
            last_ibd_check = now;
        }

        // CRITICAL FIX: Sync real chain height from RPC every 10 seconds (P0-3 fix)
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_height_sync).count() >= 10) {
            std::string heightResponse = CallRPC("getblockcount");
            if (!heightResponse.empty()) {
                consecutive_rpc_failures = 0;  // Reset on success
                UniValue jsonResp;
                if (jsonResp.read(heightResponse)) {
                    if (jsonResp.exists("result") && jsonResp["result"].isNum()) {
                        int64_t realHeight = jsonResp["result"].getInt<int64_t>();
                        int64_t currentHeight = g_current_mining_height.load();
                        if (realHeight > currentHeight) {
                            LogInfo("Miner: Chain height synced from RPC: %" PRId64 " → %" PRId64,
                                     currentHeight, realHeight);
                            g_current_mining_height.store(realHeight);
                            g_mode_switcher.SetBlockHeight(realHeight);
                        } else if (realHeight < currentHeight) {
                            LogWarning("Miner: Height regression detected (miner=%" PRId64 " node=%" PRId64 "), candidate block may be rejected. Keeping current height.",
                                       currentHeight, realHeight);
                        }
                    }
                }
            } else {
                consecutive_rpc_failures++;
                LogWarning("Miner: Failed to sync chain height from RPC (failure %d/%d, node=%s:%d)",
                           consecutive_rpc_failures, MAX_RPC_FAILURES, g_rpc_connect.c_str(), g_rpc_port);

                if (consecutive_rpc_failures >= MAX_RPC_FAILURES) {
                    LogError("Miner: Node %s:%d unreachable after %d consecutive failures. STOPPING MINING to save power.",
                             g_rpc_connect.c_str(), g_rpc_port, consecutive_rpc_failures);
                    fprintf(stderr, "\n[X] NODE DISCONNECTED: %s:%d unreachable. Mining stopped.\n", g_rpc_connect.c_str(), g_rpc_port);
                    fprintf(stderr, "    Blocks cannot be submitted without a node. Exiting miner.\n\n");
                    fGenerateTKNC = false;
                    break;
                }
            }
            last_height_sync = now;
        }

        // Refresh block template more frequently (every 30 seconds instead of 60)
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_template_refresh).count() >= 30) {
            // Re-fetch template from RPC to get latest transactions and state
            if (FetchBlockTemplateFromRPC()) {
                consecutive_rpc_failures = 0;  // Reset on success
                LogInfo("Miner: Block template refreshed from RPC at height %" PRId64, g_current_mining_height.load());
            }
            
            node::CBlockTemplate new_template;
            {
                std::lock_guard<std::mutex> lock(g_template_mutex);
                new_template = g_block_template;
            }
            
            // Update timestamp and height with REAL values
            new_template.block.nTime = GetTime();
            int64_t currentHeight = g_current_mining_height.load();  // Use synced real height
            
            // Recalculate coinbase reward with REAL height
            if (!new_template.block.vtx.empty()) {
                CMutableTransaction coinbaseTx(*new_template.block.vtx[0]);
                CAmount blockReward = GetTKNCBlockSubsidy(currentHeight);

                if (blockReward <= 0) {
                    // E03-FIX: Respect emission end - no fallback to hardcoded reward
                    blockReward = 0;
                }

                // FIX (2026-06-28): Removed erroneous COIN multiplier — see L451 comment.

                CAmount teamReward = blockReward * TKNC_TEAM_SHARE_PERCENT / 100;
                CAmount minerReward = blockReward - teamReward;
                coinbaseTx.vout[0].nValue = minerReward;
                if (coinbaseTx.vout.size() > 1) {
                    coinbaseTx.vout[1].nValue = teamReward;
                }
                new_template.block.vtx[0] = MakeTransactionRef(coinbaseTx);
                new_template.block.hashMerkleRoot = BlockMerkleRoot(new_template.block);
            }
            
            block_template = new_template;
            target.SetCompact(new_template.block.nBits);
            last_template_refresh = now;
            
            LogInfo("Miner: Block template refreshed at height %" PRId64, currentHeight);
        }
        
        if (g_mode_switcher.GetCurrentMode() != MiningMode::MODE_POW) {
            last_total_hashes = OpenCLMiner::GetTotalHashes();
            total_hashes = 0;
            // Prevent permanent LLM lock when RequestGuard destructor fails.
            g_mode_switcher.CleanupTimeoutRequests();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        CBlockHeader header = block_template.block;
        header.nTime = GetTime();
        header.nNonce = 0;

        uint32_t found_nonce = OpenCLMiner::MineBlockGPU(header);

        total_hashes = OpenCLMiner::GetTotalHashes();

        static std::chrono::time_point<std::chrono::steady_clock> last_resumed_time{};
        if (last_total_hashes == 0 && total_hashes > 0) {
            auto now_resume = std::chrono::steady_clock::now();
            if (last_resumed_time.time_since_epoch().count() == 0 || 
                std::chrono::duration<double>(now_resume - last_resumed_time).count() > 30.0) {
                last_resumed_time = now_resume;
            }
        }

        if (found_nonce != 0) {
            header.nNonce = found_nonce;

            CBlock full_block = block_template.block;
            full_block.nTime = header.nTime;
            full_block.nNonce = header.nNonce;

            // Bitcoin-style local PoW verification before RPC submit.
            // GPU kernel uses 64-bit coarse comparison; node requires full 256-bit.
            uint256 powHash = TKNCComputeHash(full_block);
            arith_uint256 hashVal = UintToArith256(powHash);
            arith_uint256 target256;
            target256.SetCompact(full_block.nBits);
            if (hashVal > target256) {
                continue;
            }

            uint256 blockHash = full_block.GetHash();
            std::string hexHash = blockHash.GetHex();

            DataStream block_ser;
            block_ser << TX_WITH_WITNESS(full_block);
            std::string hexBlock = HexStr(block_ser);

            bool submitted = SubmitBlockToNode(hexBlock);

            if (submitted) {
                int64_t currentHeight = g_current_mining_height.load();
                CAmount blockReward = GetTKNCBlockSubsidy(currentHeight);
                CAmount teamReward = blockReward * TKNC_TEAM_SHARE_PERCENT / 100;
                CAmount minerReward = blockReward - teamReward;

                double rewardTKNC = static_cast<double>(minerReward) / COIN;

                g_blocks_found.fetch_add(1);

                // Local time prefix for block found notification
                auto now_t = std::chrono::system_clock::now();
                std::time_t now_c = std::chrono::system_clock::to_time_t(now_t);
                char time_buf[32] = {0};
                std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now_c));

                printf("[%s] >>> BLOCK FOUND! Height: %" PRId64 "  Hash: %s...  Miner Reward: %.2f TKNC\n",
                       time_buf, currentHeight, hexHash.substr(0, 16).c_str(), rewardTKNC);
                printf("=================================================================\n");
                fflush(stdout);

                PrintMiningStatus(currentHeight, 0.0, 0.0, (int)g_request_counter);

                LogInfo("Block submitted successfully at height %" PRId64 " hash=%s miner_reward=%.2f TKNC (total=%.2f, team=%.2f)",
                        currentHeight, hexHash.c_str(), rewardTKNC,
                        static_cast<double>(blockReward) / COIN,
                        static_cast<double>(teamReward) / COIN);

                if (FetchBlockTemplateFromRPC()) {
                    node::CBlockTemplate new_template;
                    {
                        std::lock_guard<std::mutex> lock(g_template_mutex);
                        new_template = g_block_template;
                    }
                    new_template.block.nTime = GetTime();
                    target.SetCompact(new_template.block.nBits);
                    block_template = new_template;
                    last_template_refresh = std::chrono::steady_clock::now();
                }
            } else {
                // Submission failed - only log, no console output
                LogWarning("Miner: Block valid but submission failed (hash=%s)", hexHash.substr(0, 16).c_str());

                // Refresh stale template after submission failure.
                if (FetchBlockTemplateFromRPC()) {
                    node::CBlockTemplate new_template;
                    {
                        std::lock_guard<std::mutex> lock(g_template_mutex);
                        new_template = g_block_template;
                    }
                    new_template.block.nTime = GetTime();
                    target.SetCompact(new_template.block.nBits);
                    block_template = new_template;
                    last_template_refresh = std::chrono::steady_clock::now();
                } else {
                    // RPC unreachable: back off to avoid hammering the node.
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                }
            }
        }

        auto stats_time = std::chrono::steady_clock::now();
        double elapsed_since_last_stats = std::chrono::duration<double>(stats_time - last_stats_time).count();

        if (elapsed_since_last_stats >= 5.0) {
            uint64_t current_hashes = OpenCLMiner::GetTotalHashes();
            double current_hashrate = (elapsed_since_last_stats > 0) ?
                static_cast<double>(current_hashes - last_total_hashes) / elapsed_since_last_stats : 0.0;
            float gpu_load = OpenCLMiner::GetGPULoad();

            static double last_good_hashrate = 0.0;
            static float last_good_gpu = 0.0f;
            if (current_hashrate > 0 && gpu_load > 0) {
                last_good_hashrate = current_hashrate;
                last_good_gpu = gpu_load;
            } else if (last_good_hashrate > 0) {
                current_hashrate = last_good_hashrate;
                gpu_load = last_good_gpu;
            }

            PrintMiningStatus(g_current_mining_height.load(), current_hashrate, gpu_load, g_request_counter);
            last_stats_time = now;
            last_total_hashes = current_hashes;
        }

        if (g_blocks_found.load() % 10 == 0 && g_blocks_found.load() > 0) {
        }
    }

    auto total_time = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - mining_start).count();
    double avg_hashrate = total_time > 0 ? static_cast<double>(total_hashes) / total_time : 0;

    PrintMiningStatus(g_current_mining_height.load(), avg_hashrate, 0.0, g_request_counter);
    } catch (const std::exception& e) {
        LogError("Miner: EXCEPTION CAUGHT: %s", e.what());
    } catch (...) {
        LogError("Miner: UNKNOWN EXCEPTION CAUGHT");
    }
}

bool GenerateTKNC(bool fGenerate, int nThreads, const CChainParams& chainparams) {
    if (nThreads < 0) {
        nThreads = GetNumCores();
    }

    fGenerateTKNC = fGenerate;

    if (fGenerate) {
        // Initialize block template with genesis-like values (will be updated by RPC)
        node::CBlockTemplate initial_template;
        initial_template.block.nVersion = 1;
        initial_template.block.hashPrevBlock = uint256();
        initial_template.block.nTime = GetTime();
        initial_template.block.nBits = UintToArith256(chainparams.GetConsensus().powLimit).GetCompact();
        initial_template.block.nNonce = 0;

        CMutableTransaction coinbaseTx;
        coinbaseTx.vin.resize(1);
        coinbaseTx.vin[0].scriptSig = CScript() << CScriptNum(0) << std::vector<unsigned char>(32, 0);
        coinbaseTx.vout.resize(2);

        CAmount initBlockReward = GetTKNCBlockSubsidy(g_current_mining_height.load());

        if (initBlockReward <= 0) {
            // E03-FIX: Respect emission end - no fallback to hardcoded reward
            initBlockReward = 0;
        }

        // FIX (2026-06-28): Removed erroneous COIN multiplier — see L451 comment.

        CAmount teamReward = initBlockReward * TKNC_TEAM_SHARE_PERCENT / 100;
        CAmount minerReward = initBlockReward - teamReward;
        coinbaseTx.vout[0].nValue = minerReward;
        CTxDestination minerDest = DecodeDestination(g_mining_address);
        coinbaseTx.vout[0].scriptPubKey = GetScriptForDestination(minerDest);
        CTxDestination teamDest = DecodeDestination(TKNC_TEAM_WALLET_ADDRESS);
        coinbaseTx.vout[1].nValue = teamReward;
        coinbaseTx.vout[1].scriptPubKey = GetScriptForDestination(teamDest);

        initial_template.block.vtx.push_back(MakeTransactionRef(std::move(coinbaseTx)));
        initial_template.block.hashMerkleRoot = BlockMerkleRoot(initial_template.block);

        g_block_template = initial_template;

        LogInfo("Miner: Checking node connectivity before starting mining...");
        std::string nodeCheck = CallRPC("getblockcount");
        if (nodeCheck.empty()) {
            // P2P Architecture: Miner → Local Node → Seed Node
            // Miner MUST connect to a LOCAL node (tkncd), never to seed directly.
            LogError("Miner: Cannot connect to local node at %s:%d", g_rpc_connect.c_str(), g_rpc_port);
            fprintf(stderr, "\n❌ FATAL: Cannot connect to TKNC node at %s:%d\n", g_rpc_connect.c_str(), g_rpc_port);
            fprintf(stderr, "\n   Architecture: Miner → Local Node(tkncd) → Seed Node(66.154.101.183)\n");
            fprintf(stderr, "   The miner requires a LOCAL node to function.\n");
            fprintf(stderr, "\n   Please start tkncd.exe first (in the same directory), then run this miner.\n\n");
            fGenerateTKNC = false;
            return false;
        }

        UniValue nodeResp;
        if (!nodeResp.read(nodeCheck) || !nodeResp.exists("result") || !nodeResp["result"].isNum()) {
            LogError("Miner: FATAL - Invalid response from node. Cannot verify blockchain state.");
            fprintf(stderr, "\n❌ FATAL: Invalid response from TKNC node\n\n");
            fGenerateTKNC = false;
            return false;
        }

        int64_t nodeHeight = nodeResp["result"].getInt<int64_t>();
        if (nodeHeight < 0) {
            LogError("Miner: FATAL - Node returned invalid height (%" PRId64 ").", nodeHeight);
            fprintf(stderr, "\n❌ FATAL: Node returned invalid chain height\n\n");
            fGenerateTKNC = false;
            return false;
        }

        std::string blockchainInfo = CallRPC("getblockchaininfo");
        if (!blockchainInfo.empty()) {
            UniValue bcInfoResp;
            if (bcInfoResp.read(blockchainInfo)) {
                if (bcInfoResp.exists("result") && bcInfoResp["result"].exists("initialblockdownload")) {
                    bool isIBD = bcInfoResp["result"]["initialblockdownload"].get_bool();

                    int64_t headerHeight = -1;
                    if (bcInfoResp["result"].exists("headers")) {
                        headerHeight = bcInfoResp["result"]["headers"].getInt<int64_t>();
                    }
                    int64_t blockHeight = -1;
                    if (bcInfoResp["result"].exists("blocks")) {
                        blockHeight = bcInfoResp["result"]["blocks"].getInt<int64_t>();
                    }

                    double verificationProgress = 0.0;
                    if (bcInfoResp["result"].exists("verificationprogress")) {
                        verificationProgress = bcInfoResp["result"]["verificationprogress"].get_real();
                    }

                    bool isFullySynced = (blockHeight == headerHeight) && (verificationProgress >= 0.99);
                    if (isIBD && !isFullySynced) {
                        LogError("Miner: FATAL - Node is still syncing (IBD=true). Mining cannot start until sync is complete.");
                        fprintf(stderr, "\nError: Node is syncing — miner cannot start\n");
                        fprintf(stderr, "  Local node is synchronizing blocks from seed.\n");
                        fprintf(stderr, "  Miner must wait until sync is complete.\n");
                        fprintf(stderr, "  Wait for sync then restart miner.\n");
                        fprintf(stderr, "  Check status: tknc-cli getblockchaininfo\n\n");
                        fGenerateTKNC = false;
                        return false;
                    }

                    if (headerHeight > blockHeight) {
                        LogWarning("Miner: Node has %" PRId64 " headers but only %" PRId64 " blocks (still catching up)", headerHeight, blockHeight);
                        fprintf(stderr, "⏳ Node is catching up: headers=%" PRId64 ", blocks=%" PRId64 "\n", headerHeight, blockHeight);
                        fprintf(stderr, "   Please wait for full sync before starting the miner.\n\n");
                        fGenerateTKNC = false;
                        return false;
                    }
                }
            }
        }

        LogInfo("Miner: ✅ Node connected and fully synced. Chain height: %" PRId64 ". Starting mining...", nodeHeight);
        g_current_mining_height.store(nodeHeight);

        for (int i = 0; i < nThreads; i++) {
            std::thread t(MinerThread, std::cref(chainparams));
            t.detach();
        }
        return true;
    } else {
        return false;
    }
}

bool GetMiningInfo(const CChainParams& chainparams, UniValue& result) {
    int64_t blocks = g_blocks_found.load();
    result.pushKV("blocks", blocks);
    result.pushKV("currentblockweight", blocks > 0 ? 4000000 : 0);
    result.pushKV("currentblocktx", blocks > 0 ? 1 : 0);

    int64_t chain_height = g_current_mining_height.load();

    std::string rpcResponse = CallRPC("getblockcount");
    if (!rpcResponse.empty()) {
        UniValue jsonResp;
        if (jsonResp.read(rpcResponse)) {
            if (jsonResp.exists("result") && jsonResp["result"].isNum()) {
                int64_t realHeight = jsonResp["result"].getInt<int64_t>();
                if (realHeight > chain_height && realHeight < chain_height + 100) {
                    chain_height = realHeight;
                    g_current_mining_height.store(realHeight);
                    g_mode_switcher.SetBlockHeight(realHeight);
                }
            }
        }
    }

    result.pushKV("chain_height", chain_height);

    double networkhashps = 0.0;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(now - g_mining_start_time).count();
    if (elapsed > 0) {
        uint64_t total_hashes = g_total_hashes.load();
        networkhashps = total_hashes / elapsed;
    }
    result.pushKV("networkhashps", networkhashps);
    result.pushKV("pooledtx", 0);
    result.pushKV("chain", chainparams.GetChainTypeString());
    result.pushKV("warnings", "");
    result.pushKV("mining_mode", GetModeName(g_mode_switcher.GetCurrentMode()));
    result.pushKV("active_requests", static_cast<int>(g_mode_switcher.GetActiveRequestCount()));

    return true;
}