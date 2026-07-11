#include <net/p2p_llm.h>
#include <apikey/api_key.h>
#include <model/llm_inference.h>
#include <util/log.h>
#include <net/inference_engine.h>
#include <rpc/escrow_rpc.h>
#include <common/args.h>

#include <cstring>
#include <algorithm>
#include <memory>
#include <chrono>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <util/fs.h>

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
#include <fcntl.h>
#include <errno.h>
#define closesocket close
#define SOCKET int
#define ioctlsocket ioctl
#define WSAEWOULDBLOCK EWOULDBLOCK
#endif

static std::atomic<int64_t> g_p2p_total_requests{0};
static std::atomic<int64_t> g_p2p_no_apikey_requests{0};

struct P2PRateLimitEntry {
    int64_t count{0};
    int64_t window_start{0};
};

static std::unordered_map<std::string, P2PRateLimitEntry> g_p2p_ip_counts;
static std::mutex g_p2p_stats_mutex;

static const int64_t RATE_LIMIT_WINDOW_SEC = 60;
static int64_t g_cleanup_counter = 0;

static bool CheckP2PRateLimit(const std::string& peer_id) {
    g_p2p_total_requests++;
    std::lock_guard<std::mutex> lock(g_p2p_stats_mutex);

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    int64_t max_requests = gArgs.GetIntArg("-p2pratelimit", 100);
    int64_t cleanup_threshold = gArgs.GetIntArg("-p2pratelimitcleanup", 1000);

    g_cleanup_counter++;
    if (g_cleanup_counter >= cleanup_threshold) {
        g_cleanup_counter = 0;
        for (auto it = g_p2p_ip_counts.begin(); it != g_p2p_ip_counts.end(); ) {
            if (now - it->second.window_start > RATE_LIMIT_WINDOW_SEC * 2) {
                it = g_p2p_ip_counts.erase(it);
            } else {
                ++it;
            }
        }
    }

    auto& entry = g_p2p_ip_counts[peer_id];

    if (now - entry.window_start > RATE_LIMIT_WINDOW_SEC) {
        entry.window_start = now;
        entry.count = 0;
    }

    entry.count++;

    if (entry.count > max_requests) {
        LogInfo("[P2P-AUDIT][RATE-LIMIT] Peer %s blocked: %ld requests in %llds window (max: %lld)",
            peer_id.c_str(), entry.count, (long long)RATE_LIMIT_WINDOW_SEC, (long long)max_requests);
        return false;
    }
    return true;
}

static std::string GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    char buf[64];
    struct tm tm_buf;
#ifdef WIN32
    localtime_s(&tm_buf, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_buf);
#endif
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(buf);
}

static std::string DiscoverP2PModelPath() {
#ifdef _WIN32
    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    fs::path models_dir = fs::path(exe_path).parent_path() / "models";
#else
    char exe_path[1024] = {0};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) { exe_path[len] = 0; }
    fs::path models_dir = fs::path(exe_path).parent_path() / "models";
#endif

    if (!fs::exists(models_dir) || !fs::is_directory(models_dir)) {
        return "";
    }

    std::vector<fs::path> gguf_files;
    for (const auto& entry : fs::directory_iterator(models_dir)) {
        if (entry.path().extension() == ".gguf" && fs::is_regular_file(entry)) {
            gguf_files.push_back(entry.path());
        }
    }

    if (gguf_files.empty()) {
        return "";
    }

    std::sort(gguf_files.begin(), gguf_files.end(), [](const fs::path& a, const fs::path& b) {
        return fs::file_size(a) > fs::file_size(b);
    });

    fs::path best = gguf_files[0];
    std::u8string u8s = best.u8string();
    return std::string(u8s.begin(), u8s.end());
}

static uint16_t HostToNetworkUint16(uint16_t val) {
    uint8_t bytes[2];
    bytes[0] = (val >> 8) & 0xFF;
    bytes[1] = val & 0xFF;
    return *reinterpret_cast<uint16_t*>(bytes);
}

static uint16_t NetworkToHostUint16(uint16_t val) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&val);
    return (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1];
}

static uint32_t HtoN32(uint32_t val) {
    uint8_t bytes[4];
    bytes[0] = (val >> 24) & 0xFF;
    bytes[1] = (val >> 16) & 0xFF;
    bytes[2] = (val >> 8) & 0xFF;
    bytes[3] = val & 0xFF;
    return *reinterpret_cast<uint32_t*>(bytes);
}

static uint32_t NtoH32(uint32_t val) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&val);
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           bytes[3];
}

static uint64_t HtoN64(uint64_t val) {
    uint32_t hi = static_cast<uint32_t>(val >> 32);
    uint32_t lo = static_cast<uint32_t>(val & 0xFFFFFFFF);
    return (static_cast<uint64_t>(HtoN32(hi)) << 32) |
           static_cast<uint64_t>(HtoN32(lo));
}

static uint64_t NtoH64(uint64_t val) {
    uint32_t hi = static_cast<uint32_t>(val >> 32);
    uint32_t lo = static_cast<uint32_t>(val & 0xFFFFFFFF);
    return (static_cast<uint64_t>(NtoH32(hi)) << 32) |
           static_cast<uint64_t>(NtoH32(lo));
}

std::vector<uint8_t> P2PLLMInferenceRequest::Serialize() const {
    std::vector<uint8_t> data;

    auto write_string = [&data](const std::string& str) {
        uint32_t len = static_cast<uint32_t>(str.size());
        uint32_t net_len = HtoN32(len);
        data.insert(data.end(), reinterpret_cast<uint8_t*>(&net_len),
                     reinterpret_cast<uint8_t*>(&net_len) + sizeof(net_len));
        data.insert(data.end(), str.begin(), str.end());
    };

    write_string(api_key);
    write_string(system_prompt);
    write_string(user_message);

    // max_tokens (appended for forward compatibility — old receivers ignore trailing bytes)
    int32_t net_max_tokens = static_cast<int32_t>(HtoN32(static_cast<uint32_t>(max_tokens)));
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&net_max_tokens),
                 reinterpret_cast<uint8_t*>(&net_max_tokens) + sizeof(net_max_tokens));

    return data;
}

bool P2PLLMInferenceRequest::Deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < sizeof(uint32_t) * 3) {
        LogInfo("P2PLLM: Request data too short");
        return false;
    }

    size_t pos = 0;

    auto read_string = [&data, &pos](std::string& str) -> bool {
        if (pos + sizeof(uint32_t) > data.size()) return false;
        uint32_t net_len;
        std::memcpy(&net_len, data.data() + pos, sizeof(uint32_t));
        pos += sizeof(uint32_t);
        uint32_t len = NtoH32(net_len);
        if (pos + len > data.size()) return false;
        str.assign(data.begin() + pos, data.begin() + pos + len);
        pos += len;
        return true;
    };

    if (!read_string(api_key)) return false;
    if (!read_string(system_prompt)) return false;
    if (!read_string(user_message)) return false;

    // max_tokens (backward compatible: old senders don't have this field)
    max_tokens = 0;  // default: not specified
    if (pos + sizeof(int32_t) <= data.size()) {
        int32_t net_max_tokens;
        std::memcpy(&net_max_tokens, data.data() + pos, sizeof(int32_t));
        max_tokens = static_cast<int>(NtoH32(static_cast<uint32_t>(net_max_tokens)));
    }

    return true;
}

std::vector<uint8_t> P2PLLMInferenceToken::Serialize() const {
    std::vector<uint8_t> data;

    uint32_t text_len = static_cast<uint32_t>(token_text.size());
    uint32_t net_text_len = HtoN32(text_len);
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&net_text_len),
                 reinterpret_cast<uint8_t*>(&net_text_len) + sizeof(net_text_len));
    data.insert(data.end(), token_text.begin(), token_text.end());

    int32_t net_index = static_cast<int32_t>(token_index);
#ifdef WIN32
    uint8_t index_bytes[4];
    index_bytes[0] = (net_index >> 24) & 0xFF;
    index_bytes[1] = (net_index >> 16) & 0xFF;
    index_bytes[2] = (net_index >> 8) & 0xFF;
    index_bytes[3] = net_index & 0xFF;
    data.insert(data.end(), index_bytes, index_bytes + 4);
#else
    int32_t host_index = htonl(net_index);
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&host_index),
                 reinterpret_cast<uint8_t*>(&host_index) + sizeof(host_index));
#endif

    return data;
}

bool P2PLLMInferenceToken::Deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < sizeof(uint32_t) + sizeof(int32_t)) {
        LogInfo("P2PLLM: Token data too short");
        return false;
    }

    size_t pos = 0;

    uint32_t net_text_len;
    std::memcpy(&net_text_len, data.data() + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t);
    uint32_t text_len = NtoH32(net_text_len);

    if (pos + text_len > data.size()) {
        LogInfo("P2PLLM: Token text length out of bounds");
        return false;
    }
    token_text.assign(data.begin() + pos, data.begin() + pos + text_len);
    pos += text_len;

    int32_t raw_index;
    std::memcpy(&raw_index, data.data() + pos, sizeof(int32_t));
#ifdef WIN32
    const uint8_t* idx_bytes = reinterpret_cast<const uint8_t*>(&raw_index);
    token_index = (static_cast<int32_t>(idx_bytes[0]) << 24) |
                  (static_cast<int32_t>(idx_bytes[1]) << 16) |
                  (static_cast<int32_t>(idx_bytes[2]) << 8) |
                  idx_bytes[3];
#else
    token_index = ntohl(raw_index);
#endif

    return true;
}

std::vector<uint8_t> P2PLLMInferenceError::Serialize() const {
    std::vector<uint8_t> data;

    int32_t net_code = static_cast<int32_t>(error_code);
#ifdef WIN32
    uint8_t code_bytes[4];
    code_bytes[0] = (net_code >> 24) & 0xFF;
    code_bytes[1] = (net_code >> 16) & 0xFF;
    code_bytes[2] = (net_code >> 8) & 0xFF;
    code_bytes[3] = net_code & 0xFF;
    data.insert(data.end(), code_bytes, code_bytes + 4);
#else
    int32_t host_code = htonl(net_code);
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&host_code),
                 reinterpret_cast<uint8_t*>(&host_code) + sizeof(host_code));
#endif

    uint32_t msg_len = static_cast<uint32_t>(error_message.size());
    uint32_t net_msg_len = HtoN32(msg_len);
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&net_msg_len),
                 reinterpret_cast<uint8_t*>(&net_msg_len) + sizeof(net_msg_len));
    data.insert(data.end(), error_message.begin(), error_message.end());

    return data;
}

bool P2PLLMInferenceError::Deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < sizeof(int32_t) + sizeof(uint32_t)) {
        LogInfo("P2PLLM: Error data too short");
        return false;
    }

    size_t pos = 0;

    int32_t raw_code;
    std::memcpy(&raw_code, data.data() + pos, sizeof(int32_t));
    pos += sizeof(int32_t);
#ifdef WIN32
    const uint8_t* code_bytes = reinterpret_cast<const uint8_t*>(&raw_code);
    error_code = (static_cast<int32_t>(code_bytes[0]) << 24) |
                 (static_cast<int32_t>(code_bytes[1]) << 16) |
                 (static_cast<int32_t>(code_bytes[2]) << 8) |
                 code_bytes[3];
#else
    error_code = ntohl(raw_code);
#endif

    uint32_t net_msg_len;
    std::memcpy(&net_msg_len, data.data() + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t);
    uint32_t msg_len = NtoH32(net_msg_len);

    if (pos + msg_len > data.size()) {
        LogInfo("P2PLLM: Error message length out of bounds");
        return false;
    }
    error_message.assign(data.begin() + pos, data.begin() + pos + msg_len);

    return true;
}

std::vector<uint8_t> P2PLLMInferenceDone::Serialize() const {
    std::vector<uint8_t> data;

    uint32_t resp_len = static_cast<uint32_t>(full_response.size());
    uint32_t net_resp_len = HtoN32(resp_len);
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&net_resp_len),
                 reinterpret_cast<uint8_t*>(&net_resp_len) + sizeof(net_resp_len));
    data.insert(data.end(), full_response.begin(), full_response.end());

    int32_t net_tokens = static_cast<int32_t>(total_tokens);
#ifdef WIN32
    uint8_t tok_bytes[4];
    tok_bytes[0] = (net_tokens >> 24) & 0xFF;
    tok_bytes[1] = (net_tokens >> 16) & 0xFF;
    tok_bytes[2] = (net_tokens >> 8) & 0xFF;
    tok_bytes[3] = net_tokens & 0xFF;
    data.insert(data.end(), tok_bytes, tok_bytes + 4);
#else
    int32_t host_tokens = htonl(net_tokens);
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&host_tokens),
                 reinterpret_cast<uint8_t*>(&host_tokens) + sizeof(host_tokens));
#endif

    return data;
}

bool P2PLLMInferenceDone::Deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < sizeof(uint32_t) + sizeof(int32_t)) {
        LogInfo("P2PLLM: Done data too short");
        return false;
    }

    size_t pos = 0;

    uint32_t net_resp_len;
    std::memcpy(&net_resp_len, data.data() + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t);
    uint32_t resp_len = NtoH32(net_resp_len);

    if (pos + resp_len > data.size()) {
        LogInfo("P2PLLM: Done response length out of bounds");
        return false;
    }
    full_response.assign(data.begin() + pos, data.begin() + pos + resp_len);
    pos += resp_len;

    int32_t raw_tokens;
    std::memcpy(&raw_tokens, data.data() + pos, sizeof(int32_t));
#ifdef WIN32
    const uint8_t* tok_bytes = reinterpret_cast<const uint8_t*>(&raw_tokens);
    total_tokens = (static_cast<int32_t>(tok_bytes[0]) << 24) |
                   (static_cast<int32_t>(tok_bytes[1]) << 16) |
                   (static_cast<int32_t>(tok_bytes[2]) << 8) |
                   tok_bytes[3];
#else
    total_tokens = ntohl(raw_tokens);
#endif

    return true;
}

// === P2PLLMHandshakeReq serialization ===

std::vector<uint8_t> P2PLLMHandshakeReq::Serialize() const {
    std::vector<uint8_t> data;
    auto write_string = [&](const std::string& s) {
        uint32_t len = static_cast<uint32_t>(s.size());
        uint32_t net_len = HtoN32(len);
        data.insert(data.end(), reinterpret_cast<const uint8_t*>(&net_len),
                    reinterpret_cast<const uint8_t*>(&net_len) + sizeof(net_len));
        data.insert(data.end(), s.begin(), s.end());
    };
    write_string(api_key);
    write_string(model_name);
    write_string(test_prompt);
    int64_t net_price = HtoN64(static_cast<uint64_t>(web_price_per_1m));
    data.insert(data.end(), reinterpret_cast<const uint8_t*>(&net_price),
                reinterpret_cast<const uint8_t*>(&net_price) + sizeof(net_price));
    return data;
}

bool P2PLLMHandshakeReq::Deserialize(const std::vector<uint8_t>& data) {
    size_t pos = 0;
    auto read_string = [&](std::string& out) -> bool {
        if (pos + sizeof(uint32_t) > data.size()) return false;
        uint32_t len;
        std::memcpy(&len, data.data() + pos, sizeof(uint32_t));
        len = NtoH32(len);
        pos += sizeof(uint32_t);
        if (pos + len > data.size()) return false;
        out.assign(data.begin() + pos, data.begin() + pos + len);
        pos += len;
        return true;
    };
    if (!read_string(api_key)) return false;
    if (!read_string(model_name)) return false;
    if (!read_string(test_prompt)) return false;
    if (pos + sizeof(int64_t) > data.size()) return false;
    uint64_t net_price;
    std::memcpy(&net_price, data.data() + pos, sizeof(uint64_t));
    web_price_per_1m = static_cast<int64_t>(NtoH64(net_price));
    return true;
}

// === P2PLLMHandshakeResp serialization ===

std::vector<uint8_t> P2PLLMHandshakeResp::Serialize() const {
    std::vector<uint8_t> data;
    // accepted (1 byte)
    data.push_back(accepted ? 1 : 0);
    // test_tokens
    auto write_string = [&](const std::string& s) {
        uint32_t len = static_cast<uint32_t>(s.size());
        uint32_t net_len = HtoN32(len);
        data.insert(data.end(), reinterpret_cast<const uint8_t*>(&net_len),
                    reinterpret_cast<const uint8_t*>(&net_len) + sizeof(net_len));
        data.insert(data.end(), s.begin(), s.end());
    };
    write_string(test_tokens);
    // node_token_count
    int32_t net_tc = HtoN32(static_cast<uint32_t>(node_token_count));
    data.insert(data.end(), reinterpret_cast<const uint8_t*>(&net_tc),
                reinterpret_cast<const uint8_t*>(&net_tc) + sizeof(net_tc));
    // verified_price_per_1m
    int64_t net_vp = HtoN64(static_cast<uint64_t>(verified_price_per_1m));
    data.insert(data.end(), reinterpret_cast<const uint8_t*>(&net_vp),
                reinterpret_cast<const uint8_t*>(&net_vp) + sizeof(net_vp));
    // tokens_per_tknc
    int64_t net_tp = HtoN64(static_cast<uint64_t>(tokens_per_tknc));
    data.insert(data.end(), reinterpret_cast<const uint8_t*>(&net_tp),
                reinterpret_cast<const uint8_t*>(&net_tp) + sizeof(net_tp));
    // miner_wallet
    write_string(miner_wallet);
    // rejection_reason
    write_string(rejection_reason);
    return data;
}

bool P2PLLMHandshakeResp::Deserialize(const std::vector<uint8_t>& data) {
    size_t pos = 0;
    if (data.size() < 1) return false;
    accepted = (data[pos++] != 0);
    auto read_string = [&](std::string& out) -> bool {
        if (pos + sizeof(uint32_t) > data.size()) return false;
        uint32_t len;
        std::memcpy(&len, data.data() + pos, sizeof(uint32_t));
        len = NtoH32(len);
        pos += sizeof(uint32_t);
        if (pos + len > data.size()) return false;
        out.assign(data.begin() + pos, data.begin() + pos + len);
        pos += len;
        return true;
    };
    if (!read_string(test_tokens)) return false;
    if (pos + sizeof(int32_t) > data.size()) return false;
    uint32_t net_tc;
    std::memcpy(&net_tc, data.data() + pos, sizeof(uint32_t));
    node_token_count = static_cast<int>(NtoH32(net_tc));
    pos += sizeof(uint32_t);
    if (pos + sizeof(int64_t) > data.size()) return false;
    uint64_t net_vp;
    std::memcpy(&net_vp, data.data() + pos, sizeof(uint64_t));
    verified_price_per_1m = static_cast<int64_t>(NtoH64(net_vp));
    pos += sizeof(uint64_t);
    if (pos + sizeof(int64_t) > data.size()) return false;
    uint64_t net_tp;
    std::memcpy(&net_tp, data.data() + pos, sizeof(uint64_t));
    tokens_per_tknc = static_cast<int64_t>(NtoH64(net_tp));
    pos += sizeof(uint64_t);
    if (!read_string(miner_wallet)) return false;
    if (!read_string(rejection_reason)) return false;
    return true;
}

std::vector<uint8_t> P2PLLMMessageHeader::Serialize() const {
    std::vector<uint8_t> header(P2P_LLM_HEADER_SIZE);

    uint32_t net_magic = HtoN32(magic);
    std::memcpy(header.data(), &net_magic, sizeof(net_magic));

    uint16_t net_type = HostToNetworkUint16(message_type);
    std::memcpy(header.data() + sizeof(uint32_t), &net_type, sizeof(net_type));

    uint32_t net_length = HtoN32(payload_length);
    std::memcpy(header.data() + sizeof(uint32_t) + sizeof(uint16_t), &net_length, sizeof(net_length));

    return header;
}

bool P2PLLMMessageHeader::Deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < P2P_LLM_HEADER_SIZE) {
        return false;
    }

    std::memcpy(&magic, data.data(), sizeof(magic));
    magic = NtoH32(magic);

    uint16_t raw_type;
    std::memcpy(&raw_type, data.data() + sizeof(uint32_t), sizeof(raw_type));
    message_type = NetworkToHostUint16(raw_type);

    std::memcpy(&payload_length, data.data() + sizeof(uint32_t) + sizeof(uint16_t), sizeof(payload_length));
    payload_length = NtoH32(payload_length);

    return true;
}

bool P2PLLMMessageHeader::IsValid() const {
    return magic == P2P_LLM_MAGIC &&
           message_type >= P2P_LLM_INFERENCE_REQUEST &&
           message_type <= P2P_LLM_HANDSHAKE_RESP &&
           payload_length <= 10 * 1024 * 1024;
}

bool SendLLMMessage(int socket_fd, P2PLLMMessageType msg_type, const std::vector<uint8_t>& payload) {
    if (socket_fd < 0) {
        LogInfo("P2PLLM: Invalid socket fd");
        return false;
    }

    P2PLLMMessageHeader header;
    header.message_type = static_cast<uint16_t>(msg_type);
    header.payload_length = static_cast<uint32_t>(payload.size());

    std::vector<uint8_t> header_data = header.Serialize();
    std::vector<uint8_t> packet;
    packet.reserve(header_data.size() + payload.size());
    packet.insert(packet.end(), header_data.begin(), header_data.end());
    packet.insert(packet.end(), payload.begin(), payload.end());

#ifdef WIN32
    int sent = ::send(socket_fd, reinterpret_cast<const char*>(packet.data()), packet.size(), 0);
#else
    ssize_t sent = ::send(socket_fd, packet.data(), packet.size(), 0);
#endif

    if (sent != static_cast<int>(packet.size())) {
        LogInfo("P2PLLM: Send failed, sent %d of %zu bytes", sent, packet.size());
        return false;
    }

    return true;
}

bool SendInferenceRequest(int socket_fd, const P2PLLMInferenceRequest& req) {
    std::vector<uint8_t> payload = req.Serialize();
    return SendLLMMessage(socket_fd, P2P_LLM_INFERENCE_REQUEST, payload);
}

bool SendInferenceToken(int socket_fd, const P2PLLMInferenceToken& token) {
    std::vector<uint8_t> payload = token.Serialize();
    return SendLLMMessage(socket_fd, P2P_LLM_INFERENCE_TOKEN, payload);
}

bool SendInferenceError(int socket_fd, const P2PLLMInferenceError& error) {
    std::vector<uint8_t> payload = error.Serialize();
    return SendLLMMessage(socket_fd, P2P_LLM_INFERENCE_ERROR, payload);
}

bool SendInferenceDone(int socket_fd, const P2PLLMInferenceDone& done) {
    std::vector<uint8_t> payload = done.Serialize();
    return SendLLMMessage(socket_fd, P2P_LLM_INFERENCE_DONE, payload);
}

bool SendHandshakeReq(int socket_fd, const P2PLLMHandshakeReq& req) {
    std::vector<uint8_t> payload = req.Serialize();
    return SendLLMMessage(socket_fd, P2P_LLM_HANDSHAKE_REQ, payload);
}

bool SendHandshakeResp(int socket_fd, const P2PLLMHandshakeResp& resp) {
    std::vector<uint8_t> payload = resp.Serialize();
    return SendLLMMessage(socket_fd, P2P_LLM_HANDSHAKE_RESP, payload);
}

bool ParseLLMMessage(const std::vector<uint8_t>& raw_data,
                      P2PLLMMessageHeader& header,
                      std::vector<uint8_t>& payload) {
    if (raw_data.size() < P2P_LLM_HEADER_SIZE) {
        return false;
    }

    if (!header.Deserialize(raw_data)) {
        return false;
    }

    if (!header.IsValid()) {
        LogInfo("P2PLLM: Invalid message header (magic=0x%08X, type=%u, len=%u)",
                header.magic, header.message_type, header.payload_length);
        return false;
    }

    if (raw_data.size() < P2P_LLM_HEADER_SIZE + header.payload_length) {
        LogInfo("P2PLLM: Incomplete message (have %zu, need %zu)",
                raw_data.size(), P2P_LLM_HEADER_SIZE + header.payload_length);
        return false;
    }

    payload.assign(raw_data.begin() + P2P_LLM_HEADER_SIZE,
                   raw_data.begin() + P2P_LLM_HEADER_SIZE + header.payload_length);

    return true;
}

void P2PLLMPeerHandler::HandleIncomingMessage(const std::string& peer_id,
                                               const std::vector<uint8_t>& message_data,
                                               int socket_fd) {
    P2PLLMMessageHeader header;
    std::vector<uint8_t> payload;

    if (!ParseLLMMessage(message_data, header, payload)) {
        LogInfo("P2PLLM: Failed to parse message from peer: %s", peer_id.c_str());
        return;
    }

    switch (header.message_type) {
        case P2P_LLM_INFERENCE_REQUEST: {
            P2PLLMInferenceRequest req;
            if (req.Deserialize(payload)) {
                ProcessInferenceRequest(peer_id, req, socket_fd);
            } else {
                LogInfo("P2PLLM: Failed to deserialize inference request from: %s", peer_id.c_str());
            }
            break;
        }
        case P2P_LLM_INFERENCE_TOKEN: {
            P2PLLMInferenceToken token;
            if (token.Deserialize(payload)) {
                ProcessInferenceToken(peer_id, token);
            } else {
                LogInfo("P2PLLM: Failed to deserialize inference token from: %s", peer_id.c_str());
            }
            break;
        }
        case P2P_LLM_INFERENCE_DONE: {
            P2PLLMInferenceDone done;
            if (done.Deserialize(payload)) {
                ProcessInferenceDone(peer_id, done);
            } else {
                LogInfo("P2PLLM: Failed to deserialize inference done from: %s", peer_id.c_str());
            }
            break;
        }
        case P2P_LLM_INFERENCE_ERROR: {
            P2PLLMInferenceError error;
            if (error.Deserialize(payload)) {
                ProcessInferenceError(peer_id, error);
            } else {
                LogInfo("P2PLLM: Failed to deserialize inference error from: %s", peer_id.c_str());
            }
            break;
        }
        case P2P_LLM_HANDSHAKE_REQ: {
            P2PLLMHandshakeReq hs_req;
            if (hs_req.Deserialize(payload)) {
                ProcessHandshakeReq(peer_id, hs_req, socket_fd);
            } else {
                LogInfo("P2PLLM: Failed to deserialize handshake request from: %s", peer_id.c_str());
            }
            break;
        }
        case P2P_LLM_HANDSHAKE_RESP: {
            P2PLLMHandshakeResp hs_resp;
            if (hs_resp.Deserialize(payload)) {
                ProcessHandshakeResp(peer_id, hs_resp);
            } else {
                LogInfo("P2PLLM: Failed to deserialize handshake response from: %s", peer_id.c_str());
            }
            break;
        }
        default:
            LogInfo("P2PLLM: Unknown message type %u from: %s", header.message_type, peer_id.c_str());
            break;
    }
}

void P2PLLMPeerHandler::ProcessInferenceRequest(const std::string& peer_id,
                                                  const P2PLLMInferenceRequest& req,
                                                  int socket_fd) {
    if (!CheckP2PRateLimit(peer_id)) {
        P2PLLMInferenceError err;
        err.error_code = 429;
        err.error_message = "Rate limit exceeded. Please try again later.";
        SendInferenceError(socket_fd, err);
        return;
    }

    LogInfo("[P2P-AUDIT] Inference request received from peer=%s, msg_len=%d, time=%s",
        peer_id.c_str(), (int)req.user_message.size(), GetTimestamp().c_str());

    if (req.api_key.empty()) {
        g_p2p_no_apikey_requests++;
        LogInfo("[P2P-AUDIT][NO-APIKEY] Request from %s without API key", peer_id.c_str());
    }

    auto start_time = std::chrono::steady_clock::now();

    LogInfo("P2PLLM: Received inference request from: %s", peer_id.c_str());

    if (!ValidateAPIKeyFormat(req.api_key)) {
        LogInfo("P2PLLM: Invalid API key format from: %s", peer_id.c_str());
        P2PLLMInferenceError err;
        err.error_code = 401;
        err.error_message = "Invalid API key format";
        SendInferenceError(socket_fd, err);
        return;
    }

    // === Unified Inference Engine: single code path to local miner ===
    {
        InferenceResult result = InferenceEngine::RequestLocalMiner(
            req.api_key, "p2p_forwarded", req.user_message, req.max_tokens);

        if (result.success) {
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

            P2PLLMInferenceToken token;
            token.token_text = result.content;
            token.token_index = 0;
            SendInferenceToken(socket_fd, token);

            P2PLLMInferenceDone done;
            done.full_response = result.content;
            // ARCHITECTURE: Use precise token count from InferenceResult (node computes from prompt_tokens + completion_tokens)
            done.total_tokens = static_cast<int>(result.tokens_used);
            // Fallback: if tokens_used is 0 but content exists, use content-based estimation
            if (done.total_tokens <= 0 && !result.content.empty()) {
                done.total_tokens = static_cast<int>(result.content.size() / 4);
            }
            if (done.total_tokens < 1) done.total_tokens = 1;
            SendInferenceDone(socket_fd, done);

            LogInfo("P2PLLM: Inference completed via LOCAL MINER (unified engine) for: %s, chars=%d, duration=%lldms",
                peer_id.c_str(), (int)result.content.size(), (long long)duration);
            LogInfo("[P2P-AUDIT] Request completed: result=success(via_miner), chars=%d, duration=%lldms",
                (int)result.content.size(), (long long)duration);
            return;
        } else {
            LogInfo("P2PLLM: Local miner not available (%s), using local LLM engine fallback",
                result.error_message.c_str());
        }
    }

    static LLMInference llm_engine;
    static bool engine_initialized = false;

    if (!engine_initialized) {
        LLMInference::Config cfg;
        std::string discovered_model = DiscoverP2PModelPath();
        if (discovered_model.empty()) {
            LogInfo("P2PLLM: No model found in models/ directory for P2P inference");
            P2PLLMInferenceError err;
            err.error_code = 500;
            err.error_message = "No LLM model found. Place a .gguf file in the models/ directory.";
            SendInferenceError(socket_fd, err);
            return;
        }
        cfg.model_path = discovered_model;
        cfg.dll_path = "";
        cfg.n_gpu_layers = -1;
        cfg.temperature = 0.7f;
        cfg.top_p = 0.9f;

        LogInfo("P2PLLM: Initializing LLM engine with model: %s (GPU layers: %d, FORCE GPU MODE)", discovered_model.c_str(), cfg.n_gpu_layers);

        if (!llm_engine.Initialize(cfg)) {
            // GPU init failed, no CPU fallback.
            LogError("P2PLLM: FATAL: GPU init failed! CPU mode is FORBIDDEN. Check CUDA/Vulkan DLLs.");
            P2PLLMInferenceError err;
            err.error_code = 503;
            err.error_message = "GPU inference failed. CPU mode is not supported. Ensure NVIDIA/AMD GPU drivers and DLLs are installed.";
            SendInferenceError(socket_fd, err);
            return;
        }
        engine_initialized = true;
        LogInfo("P2PLLM: LLM engine ready for P2P inference");
    }

    std::string full_response;
    int total_tokens = 0;

    auto on_token = [&](const std::string& token_text, int index) -> void {
        P2PLLMInferenceToken token;
        token.token_text = token_text;
        token.token_index = index;
        SendInferenceToken(socket_fd, token);
        full_response += token_text;
        total_tokens++;
    };

    // Pass through max_tokens from the P2P request (-1=unlimited, 0=default, >0=limit)
    llm_engine.SetMaxTokens(req.max_tokens);

    LLMInference::GenerationResult result = llm_engine.GenerateStream(
        req.user_message,
        on_token,
        req.system_prompt
    );

    if (result.success) {
        P2PLLMInferenceDone done;
        done.full_response = full_response;
        done.total_tokens = total_tokens;
        SendInferenceDone(socket_fd, done);
        LogInfo("P2PLLM: Inference completed for: %s, tokens: %d", peer_id.c_str(), total_tokens);

        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

        LogInfo("[P2P-AUDIT] Request completed: result=success, tokens=%d, duration=%lldms",
            total_tokens, duration);
    } else {
        P2PLLMInferenceError err;
        err.error_code = 500;
        err.error_message = result.error.empty() ? "Inference failed" : result.error;
        SendInferenceError(socket_fd, err);
        LogInfo("P2PLLM: Inference failed for: %s, error: %s", peer_id.c_str(), err.error_message.c_str());

        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

        LogInfo("[P2P-AUDIT] Request completed: result=failed, error=%s, duration=%lldms",
            err.error_message.c_str(), duration);
    }
}

void P2PLLMPeerHandler::ProcessInferenceToken(const std::string& peer_id,
                                                const P2PLLMInferenceToken& token) {
    LogInfo("P2PLLM: Received token[%d] from: %s, len: %zu",
            token.token_index, peer_id.c_str(), token.token_text.size());

    if (token_callback) {
        token_callback(token.token_text, token.token_index);
    }
}

void P2PLLMPeerHandler::ProcessInferenceDone(const std::string& peer_id,
                                               const P2PLLMInferenceDone& done) {
    LogInfo("P2PLLM: Received inference done from: %s, total_tokens: %d",
            peer_id.c_str(), done.total_tokens);

    if (done_callback) {
        done_callback(done.full_response, done.total_tokens);
    }
}

void P2PLLMPeerHandler::ProcessInferenceError(const std::string& peer_id,
                                                const P2PLLMInferenceError& error) {
    LogInfo("P2PLLM: Received inference error from: %s, code: %d, msg: %s",
            peer_id.c_str(), error.error_code, error.error_message.c_str());

    if (error_callback) {
        error_callback(error.error_code, error.error_message);
    }
}

void P2PLLMPeerHandler::ProcessHandshakeReq(const std::string& peer_id,
                                               const P2PLLMHandshakeReq& req,
                                               int socket_fd) {
    LogInfo("[HANDSHAKE] Received handshake request from peer=%s, model=%s, web_price=%lld",
            peer_id.c_str(), req.model_name.c_str(), (long long)req.web_price_per_1m);

    P2PLLMHandshakeResp resp;

    // Step 1: Get miner's verified price from node-side storage
    int64_t verified_price = GetMinerPrice("");  // TODO: get miner_wallet from peer_id mapping
    if (verified_price <= 0) verified_price = 10;  // Default fallback

    resp.verified_price_per_1m = verified_price;
    resp.tokens_per_tknc = 1000000LL / verified_price;
    if (resp.tokens_per_tknc < 1000) resp.tokens_per_tknc = 1000;

    // Step 2: Execute micro-inference for token counting verification
    // Forward to local miner at 127.0.0.1:9332 for a real micro-inference
    std::string test_prompt = req.test_prompt;
    if (test_prompt.empty()) {
        test_prompt = "Hello";
    }

    // Build JSON request body for miner's /api/v1/chat endpoint
    std::string test_body = "{\n";
    test_body += "  \"api_key\": \"" + req.api_key + "\",\n";
    test_body += "  \"messages\": [{\"role\": \"user\", \"content\": \"" + test_prompt + "\"}],\n";
    test_body += "  \"stream\": false\n";
    test_body += "}";

    // Use InferenceEngine to call local miner (same path as ProcessInferenceRequest)
    InferenceResult miner_result = InferenceEngine::RequestLocalMiner(
        req.api_key, "handshake_verify", test_prompt);

    if (miner_result.success) {
        // Miner returned a result — use precise token count from LLM engine
        // ARCHITECTURE: Node uses prompt_tokens + completion_tokens from miner's LLM engine,
        // NOT content.size()/4 approximation. The miner's LLM engine provides exact token counts.
        resp.test_tokens = miner_result.content;
        // Use precise tokens_used from InferenceResult (computed by node from prompt_tokens + completion_tokens)
        resp.node_token_count = static_cast<int>(miner_result.tokens_used);
        // Fallback: if tokens_used is 0 but content exists, use content-based estimation
        if (resp.node_token_count <= 0 && !miner_result.content.empty()) {
            resp.node_token_count = static_cast<int>(miner_result.content.size() / 4);
            LogInfo("[HANDSHAKE] tokens_used=0 from miner, using content-based estimation: %d", resp.node_token_count);
        }
        if (resp.node_token_count < 1) resp.node_token_count = 1;

        // Sanity check: a short prompt like "Hello" should NOT produce thousands of tokens
        // If miner claims >500 tokens for "Hello", it's likely cheating
        if (resp.node_token_count > 500) {
            LogWarning("[HANDSHAKE] Miner returned suspiciously high token count (%d) for short prompt '%s' — possible cheating",
                       resp.node_token_count, test_prompt.c_str());
            resp.accepted = false;
            resp.rejection_reason = "Token count anomaly: miner returned " +
                std::to_string(resp.node_token_count) + " tokens for a short test prompt. Possible cheating.";
            SendHandshakeResp(socket_fd, resp);
            return;
        }

        LogInfo("[HANDSHAKE] Micro-inference result: content_len=%zu, estimated_tokens=%d",
                miner_result.content.size(), resp.node_token_count);
    } else {
        // Local miner not available — cannot verify token counting
        LogWarning("[HANDSHAKE] Local miner not available for micro-inference: %s", miner_result.error_message.c_str());
        resp.test_tokens = "";
        resp.node_token_count = 0;
        // Still accept handshake if miner is unreachable (P2P relay scenario)
        // Token counting verification will happen on the actual inference node
    }

    // Step 3: Verify pricing consistency
    if (req.web_price_per_1m > 0 && verified_price > 0) {
        double price_ratio = static_cast<double>(verified_price) / static_cast<double>(req.web_price_per_1m);
        if (price_ratio > 2.0 || price_ratio < 0.5) {
            LogWarning("[HANDSHAKE] Price mismatch: web=%lld, verified=%lld, ratio=%.2f",
                       (long long)req.web_price_per_1m, (long long)verified_price, price_ratio);
            // Price mismatch — reject connection to prevent billing disputes
            resp.accepted = false;
            resp.rejection_reason = "Price mismatch: WEB price=" +
                std::to_string(req.web_price_per_1m) + " TKNC/1M vs verified price=" +
                std::to_string(verified_price) + " TKNC/1M. Possible price cheating.";
            SendHandshakeResp(socket_fd, resp);
            return;
        }
    }

    // Step 4: Accept handshake
    resp.accepted = true;
    resp.miner_wallet = "";  // TODO: get from miner registration
    resp.rejection_reason = "";

    LogInfo("[HANDSHAKE] Sending response: accepted=%s, verified_price=%lld, tokens_per_tknc=%lld",
            resp.accepted ? "true" : "false",
            (long long)resp.verified_price_per_1m,
            (long long)resp.tokens_per_tknc);

    SendHandshakeResp(socket_fd, resp);
}

void P2PLLMPeerHandler::ProcessHandshakeResp(const std::string& peer_id,
                                                const P2PLLMHandshakeResp& resp) {
    LogInfo("[HANDSHAKE] Received handshake response from peer=%s: accepted=%s, verified_price=%lld, tokens_per_tknc=%lld, miner_wallet=%s",
            peer_id.c_str(),
            resp.accepted ? "true" : "false",
            (long long)resp.verified_price_per_1m,
            (long long)resp.tokens_per_tknc,
            resp.miner_wallet.c_str());

    if (!resp.accepted) {
        LogWarning("[HANDSHAKE] Handshake rejected by peer=%s: %s",
                   peer_id.c_str(), resp.rejection_reason.c_str());
    }
}
