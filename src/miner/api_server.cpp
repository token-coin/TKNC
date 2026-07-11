#include <miner/api_server.h>
#include <miner/mode_switcher.h>
#include <economics/dynamic_pricing.h>
#include <economics/emission.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/time.h>
#include <random.h>
#include <hash.h>
#include <logging.h>
#include <model/gpu_memory.h>
#include <pubkey.h>
#include <util/strencodings.h>
#include <crypto/sha1.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include <univalue.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#ifdef WIN32
#include <windows.h>  // Required for SEH (__try/__except) to catch ACCESS_VIOLATION
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <errno.h>
#include <unistd.h>
// Windows-to-Linux Socket API compatibility macros
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#define WSAGetLastError() (errno)
#define _stricmp strcasecmp
typedef uint32_t DWORD;
#endif

extern std::atomic<int64_t> g_current_mining_height;
extern uint64_t g_request_counter;
extern void PrintInferenceStatus(int64_t height, double hashrate, float gpu_load, int req_count);
#include <event2/bufferevent.h>
#include <event2/keyvalq_struct.h>
#include <inttypes.h>
#include <chrono>
#include <ctime>
#include <fstream>

constexpr size_t MAX_CHAT_BODY_SIZE = 100 * 1024 * 1024; // 100MB — no artificial limit. Large prompts (e.g. long context for 10000B models) should pass through.

extern ModeSwitcher& GetModeSwitcher();

// Atomic counter avoids concurrent request_id collision from steady_clock timestamps.
static std::atomic<uint64_t> g_request_id_counter{1};

APIServer::APIServer(ModelRuntime* runtime, const std::string& datadir, const std::string& model_path, int api_port,
                     const std::string& rpc_host, int rpc_port,
                     const std::string& rpc_user, const std::string& rpc_password,
                     const std::string& bind_address)
    : model_runtime(runtime), data_dir(datadir), model_path_(model_path), port(api_port),
      bind_address_(getenv("API_BIND_ADDRESS") ? getenv("API_BIND_ADDRESS") : bind_address),
      running(false), event_base_ptr(nullptr), http_ptr(nullptr),
      web_server_url_(getenv("WEB_SERVER_URL") ? getenv("WEB_SERVER_URL") : "http://66.154.101.183"),
      rpc_host_(rpc_host), rpc_port_(rpc_port), rpc_user_(rpc_user), rpc_password_(rpc_password) {

    llm_engine = std::make_unique<LLMInference>();
}

APIServer::~APIServer() {
    Stop();
}

bool APIServer::Start() {
    LoadConfig();
    
#ifdef WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "[API Server] WSAStartup failed\n";
        return false;
    }
#endif

    event_base_ptr = event_base_new();
    if (!event_base_ptr) {
        std::cerr << "[API Server] event_base_new failed\n";
        return false;
    }
    
    http_ptr = evhttp_new(event_base_ptr);
    if (!http_ptr) {
        std::cerr << "[API Server] evhttp_new failed\n";
        event_base_free(event_base_ptr);
        return false;
    }
    
    evhttp_set_timeout(http_ptr, 300);

    // [IRON RULE] Miner API MUST bind 127.0.0.1 only (doc L163)
    // All external requests go through Node RPC (p2pinference) → localhost forward to miner
    int bind_retries = 0;
    const int max_port_retries = 10;
    int try_port = port;
    bool bound = false;

    while (bind_retries <= max_port_retries && !bound) {
        if (evhttp_bind_socket(http_ptr, "127.0.0.1", try_port) == 0) {
            bound = true;
            if (try_port != port) {
                port = try_port;
                std::cout << "[API Server] Using port " << try_port << " (original " << port << " was in use)\n" << std::flush;
            }
            std::cout << "[API Server] Bound to 127.0.0.1:" << port << " (local only, via node)\n" << std::flush;
            break;
        }
        bind_retries++;
        if (bind_retries <= max_port_retries) {
            try_port++;
            std::cerr << "[API Server] Bind failed on 127.0.0.1:" << (try_port - 1) << ", retrying " << try_port << "...\n";
        }
    }

    if (!bound) {
        std::cerr << "[API Server] All bind retries exhausted\n";
        evhttp_free(http_ptr);
        event_base_free(event_base_ptr);
        return false;
    }

    std::cout << "[API Server] API server ready on port " << port << "\n" << std::endl;
        evhttp_set_cb(http_ptr, "/api/v1/chat", StaticChatHandler, this);
    evhttp_set_cb(http_ptr, "/ws/chat", StaticWSHandler, this);
    
    RegisterExplorerEndpoints();

    std::string model_path = model_path_;
    if (model_path.empty()) {
        fs::path models_dir = GetExeDir() / "models";
        std::vector<fs::path> gguf_files;
        if (fs::exists(models_dir) && fs::is_directory(models_dir)) {
            for (const auto& entry : fs::directory_iterator(models_dir)) {
                if (entry.path().extension() == ".gguf" && fs::is_regular_file(entry)) {
                    gguf_files.push_back(entry.path());
                    LogInfo("API Server: Found model file: %s", fs::PathToString(entry.path()).c_str());
                }
            }
        }
        if (!gguf_files.empty()) {
            std::sort(gguf_files.begin(), gguf_files.end(), [](const fs::path& a, const fs::path& b) {
                return fs::file_size(a) > fs::file_size(b);
            });
            model_path = fs::PathToString(gguf_files[0]);
            model_path_ = model_path;
            LogInfo("API Server: Auto-loaded model (largest file): %s", model_path.c_str());
            LogInfo("API Server: Total models found: %d, loaded: %s", (int)gguf_files.size(), model_path.c_str());
        } else {
            LogWarning("API Server: No .gguf model files found in models/ directory. Running in mining-only mode (no LLM inference).");
            LogWarning("API Server: To enable inference, place .gguf model files in the models/ folder next to the miner executable.");
        }
    }
    if (fs::exists(fs::u8path(model_path))) {
        // Set DLL search path BEFORE GPU detection so GPUMemoryManager can find
        // ggml-cuda.dll / ggml-vulkan.dll in the exe-relative dll/ directory.
        // Previously this was set as a side effect of ModelLoader::PreloadModel,
        // but that caused a duplicate model load into VRAM (now removed).
#ifdef WIN32
        {
            fs::path dll_dir = GetExeDir() / "dll";
            if (fs::exists(dll_dir)) {
                SetDllDirectoryA(fs::PathToString(dll_dir).c_str());
                LogInfo("[API Server] DLL search path set to: %s", fs::PathToString(dll_dir).c_str());
            }
        }
#endif
        GPUMemoryManager gpu_mgr;
        
        bool enum_success = gpu_mgr.EnumerateAllGPUs();
        
        if (!enum_success) {
            std::cerr << "Warning: GPU enum failed, trying single-GPU...\n";

            bool gpu_detected = gpu_mgr.InitializeGPU(0);

            if (!gpu_detected) {
                std::cerr << "[LLM] FATAL: No supported GPU found! TKNC Miner requires NVIDIA (CUDA) or AMD (Vulkan) GPU.\n";
                std::cerr << "[LLM] CPU-only mode is NOT supported. Exiting.\n";
                LogError("[LLM] FATAL: No supported GPU detected. CPU mode is forbidden. Exiting.");
                exit(1);
            } else {
                LLMInference::Config llm_config;
                llm_config.model_path = model_path;
                llm_config.dll_path = "llama.dll";
                llm_config.n_ctx = n_ctx_configured;  // Configurable via -n_ctx (default 131072=128K). Real limit is GPU VRAM.
                llm_config.n_predict = -1;  // -1 = unlimited (generate until EOS)
                llm_config.temperature = 0.7f;
                llm_config.top_p = 0.9f;
                llm_config.n_threads = 1;
                llm_config.n_gpu_layers = 24;
                
                if (!llm_engine->Initialize(llm_config)) {
                    std::cerr << "Warning: LLM engine init failed. Mining-only mode.\n";
                }
            }
        } else {
            gpu_mgr.PrintGPUInfoSummary();
            
            auto file_size_mb = fs::file_size(model_path) / (1024 * 1024);
            
            int optimal_layers = gpu_mgr.CalculateOptimalGPULayersForAllGPUs(file_size_mb, 32);
            
            LLMInference::Config llm_config;
            llm_config.model_path = model_path;
            llm_config.dll_path = "llama.dll";
            llm_config.n_ctx = n_ctx_configured;  // Configurable via -n_ctx (default 131072=128K). Real limit is GPU VRAM.
            llm_config.n_predict = -1;  // -1 = unlimited (generate until EOS)
            llm_config.temperature = 0.7f;
            llm_config.top_p = 0.9f;
            llm_config.n_threads = 1;
            llm_config.n_gpu_layers = optimal_layers;
            
            if (!llm_engine->Initialize(llm_config)) {
                std::cerr << "Warning: LLM engine init failed. Mining-only mode.\n";
            }
        }
    } else {
        LogWarning("API: No model file found. Mining-only mode (inference unavailable)");
    }

    running = true;
    server_thread = std::thread(&APIServer::ServerLoop, this);

    // Start async worker threads for LLM inference
    try {
        StartWorkerThreads();
    } catch (const std::exception& e) {
        LogError("API: [FATAL] StartWorkerThreads crashed: %s", e.what());
        // Continue without worker threads - API will accept connections but fail on inference
    } catch (...) {
        LogError("API: [FATAL] StartWorkerThreads unknown crash");
    }

    LogInfo("API: Server started on port %d", port);
    LogInfo("API: Async worker pool initialized (%d threads)", MAX_WORKER_THREADS);
    
    {
        LiveMinerInfo self;
        char hostname[256] = {0};
#ifdef WIN32
        DWORD size = sizeof(hostname);
        GetComputerNameA(hostname, &size);
#else
        gethostname(hostname, sizeof(hostname));
#endif
        std::string wallet = wallet_address_;  // Keep empty if not set — prevents registration with invalid wallet
        self.miner_id = wallet_address_.empty() ? (std::string(hostname) + "-" + std::to_string(GetTime())) : wallet_address_;
        self_miner_id_ = self.miner_id;
        self.wallet_address = wallet;
        // Model name is derived from the discovered model file path,
        // regardless of whether the LLM engine initialized successfully.
        // The miner knows its model name even if GPU loading failed —
        // the model name should be reported correctly to the Web server.
        if (!model_path_.empty()) {
            size_t last_sep = model_path_.find_last_of("/\\");
            std::string filename = (last_sep != std::string::npos) ? model_path_.substr(last_sep + 1) : model_path_;
            size_t dot_pos = filename.find_last_of('.');
            self.model_name = (dot_pos != std::string::npos) ? filename.substr(0, dot_pos) : filename;
        } else {
            self.model_name = "mining-only";
        }
        
        std::string gpu_name = "Unknown GPU";
        int64_t vram_mb = 0;
        {
            GPUMemoryManager gpu_mgr;
            if (gpu_mgr.EnumerateAllGPUs()) {
                auto& gpus = gpu_mgr.GetAllGPUs();
                if (!gpus.empty()) {
                    gpu_name = gpus[0].name;
                    vram_mb = gpus[0].total_memory_mb;
                    LogInfo("API: Self-register GPU (nvidia-smi): %s (%lld MB)", gpu_name.c_str(), (long long)vram_mb);
                }
            }
        }
        if (vram_mb == 0 || gpu_name == "Unknown GPU") {
            GPUDeviceInfo dxgi_info;
            if (GPUMemoryManager::DetectPrimaryGPU(dxgi_info)) {
                gpu_name = dxgi_info.name;
                vram_mb = dxgi_info.total_memory_mb;
                LogInfo("API: Self-register GPU (DXGI): %s (%lld MB)", gpu_name.c_str(), (long long)vram_mb);
            } else {
                LogWarning("API: GPU enumeration failed during self-registration");
            }
        }
        self.gpu_name = gpu_name;
        self.gpu_vram_total_mb = vram_mb;

        // A2.7 compliance: Miner does NOT detect public IP — node handles it.
        // The node calls DetectPublicIPForMiner() after receiving miner_ready RPC.
        self.ip_address = "0.0.0.0";  // Placeholder, node will report real public IP to Web
        
        self.api_port = port;
        self.status = "online";
        self.price_per_1m_tknc = 10;
        
        RegisterMiner(self);
        LogInfo("API: Self-registration complete - miner_id=%s wallet=%s",
                self.miner_id.c_str(), self.wallet_address.c_str());

        if (!web_server_url_.empty() && !self.wallet_address.empty()) {
            // A2.7 compliance: Notify node via RPC instead of direct web communication
            std::string miner_ready_params = "[";
            miner_ready_params += "\"" + self.wallet_address + "\",";
            miner_ready_params += "\"" + self.model_name + "\",";
            miner_ready_params += "\"" + self.gpu_name + "\",";
            miner_ready_params += std::to_string(self.gpu_vram_total_mb) + ",";
            miner_ready_params += std::to_string(self.gpu_vram_used_mb) + ",";
            miner_ready_params += std::to_string(self.gpu_utilization) + ",";
            miner_ready_params += "9313,";  // Gateway port for external access (node's inference gateway)
            miner_ready_params += "\"" + web_server_url_ + "\"";
            miner_ready_params += "]";
            
            std::thread([this, miner_ready_params]() {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                std::string rpc_response = CallNodeRPC("miner_ready", miner_ready_params);
                if (!rpc_response.empty()) {
                    LogInfo("API: miner_ready RPC acknowledged by node");
                } else {
                    LogWarning("API: miner_ready RPC failed - miner may not appear on Web");
                }
            }).detach();
            LogInfo("API: miner_ready RPC scheduled for node (%s)", web_server_url_.c_str());
        }

        // D-M02-FIX: Removed deprecated HeartbeatLoop thread launch (was L361-369)
        // A2.7/A2.8: Heartbeat to web server is now handled by the node via miner_ready RPC.
        // The old direct-to-web heartbeat pattern is obsolete and wastes resources.
        LogInfo("API: Heartbeat skipped (node-managed via miner_ready RPC)");
    }
    // The node runs the heartbeat loop on behalf of the miner.
    // See: node/miner_registry.cpp

    return true;
}

bool APIServer::RegisterMiner(const LiveMinerInfo& info) {
    std::lock_guard<std::mutex> lock(miners_mutex_);
    
    if (!info.wallet_address.empty()) {
        for (auto it = live_miners_.begin(); it != live_miners_.end(); ) {
            if (it->second.wallet_address == info.wallet_address && it->first != info.miner_id) {
                LogInfo("API: Dedup removing old miner_id=%s (same wallet=%s)", 
                        it->first.c_str(), info.wallet_address.c_str());
                it = live_miners_.erase(it);
            } else if (it->second.wallet_address == "unknown" && info.wallet_address != "unknown") {
                LogInfo("API: Removing stale 'unknown' wallet entry: %s (replaced by %s)",
                        it->first.c_str(), info.wallet_address.c_str());
                it = live_miners_.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    live_miners_[info.miner_id] = info;
    live_miners_[info.miner_id].registration_time = GetTime();
    live_miners_[info.miner_id].last_heartbeat = GetTime();
    live_miners_[info.miner_id].status = "online";
    
    LogInfo("API: Miner registered - %s (model: %s, gpu: %s)", 
            info.miner_id.c_str(), info.model_name.c_str(), info.gpu_name.c_str());
    return true;
}

bool APIServer::UpdateMinerHeartbeat(const std::string& miner_id, const LiveMinerInfo& update) {
    std::lock_guard<std::mutex> lock(miners_mutex_);
    
    auto it = live_miners_.find(miner_id);
    if (it == live_miners_.end()) {
        return false;
    }
    
    it->second.last_heartbeat = GetTime();
    it->second.hashrate = update.hashrate;
    it->second.status = update.status;
    it->second.gpu_utilization = update.gpu_utilization;
    it->second.gpu_vram_used_mb = update.gpu_vram_used_mb;
    if (!update.model_name.empty() && update.model_name != "Unknown") {
        it->second.model_name = update.model_name;
    }
    if (update.total_blocks_found > it->second.total_blocks_found) {
        it->second.total_blocks_found = update.total_blocks_found;
    }
    if (update.total_inference_requests > it->second.total_inference_requests) {
        it->second.total_inference_requests = update.total_inference_requests;
    }
    if (update.current_block_height > it->second.current_block_height) {
        it->second.current_block_height = update.current_block_height;
    }
    
    return true;
}

void APIServer::CleanupOfflineMiners() {
    std::lock_guard<std::mutex> lock(miners_mutex_);
    
    int64_t now = GetTime();
    const int64_t HEARTBEAT_TIMEOUT = 30;
    
    for (auto& pair : live_miners_) {
        if (pair.second.wallet_address == wallet_address_) {
            pair.second.last_heartbeat = now;
            continue;
        }
        if (now - pair.second.last_heartbeat > HEARTBEAT_TIMEOUT) {
            if (pair.second.status != "offline") {
                LogInfo("API: Miner %s went offline (no heartbeat for %lds)", 
                        pair.first.c_str(), now - pair.second.last_heartbeat);
                pair.second.status = "offline";
            }
        }
    }
}

void APIServer::CleanupLoop() {
    int save_counter = 0;
    while (cleanup_running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        CleanupOfflineMiners();
        save_counter++;
        if (save_counter >= 6) {
            SaveMinerData();
            save_counter = 0;
        }
    }
}

// DEAD CODE REMOVED: HeartbeatLoop() and web registration moved to node/miner_registry.cpp.

void APIServer::AddCORSHeaders(struct evhttp_request* req) {
    // D-H04-FIX: CORS restricted to prevent cross-site attacks on miner API
    // Miner API binds 127.0.0.1:9332 (localhost only), but defense-in-depth still applies
    const char* allowed_origin = std::getenv("TKNC_MINER_CORS_ORIGIN");
    if (allowed_origin && strlen(allowed_origin) > 0) {
        evhttp_add_header(evhttp_request_get_output_headers(req), "Access-Control-Allow-Origin", allowed_origin);
    }
    // In production (no env var), omit header entirely - browser same-origin policy blocks cross-site
    evhttp_add_header(evhttp_request_get_output_headers(req), "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    evhttp_add_header(evhttp_request_get_output_headers(req), "Access-Control-Allow-Headers", "Content-Type, Authorization, Bearer");
}

void APIServer::HandleMinerRegisterRequest(struct evhttp_request* req) {
    if (!req) {
        return;
    }

    AddCORSHeaders(req);

    struct evbuffer* buf = evbuffer_new();
    if (!buf) {
        evhttp_send_error(req, HTTP_INTERNAL, "Failed to create buffer");
        return;
    }

    try {
        struct evbuffer* input_buf = evhttp_request_get_input_buffer(req);
        if (!input_buf) {
            evbuffer_add_printf(buf, "{\"error\": \"No input buffer\"}");
            evhttp_send_reply(req, 400, "Bad Request", buf);
            evbuffer_free(buf);
            return;
        }

        size_t len = evbuffer_get_length(input_buf);

        auto body = std::make_unique<std::string>();
        if (len > 0 && len < 1024 * 1024) {
            body->resize(len);
            evbuffer_copyout(input_buf, &(*body)[0], len);

            LogInfo("API: Received body length=%d, content=%.200s", (int)len, body->c_str());
        } else {
            evbuffer_add_printf(buf, "{\"error\": \"Invalid body size: %d\"}", (int)len);
            evhttp_send_reply(req, 400, "Bad Request", buf);
            evbuffer_free(buf);
            return;
        }

        auto json_request = std::make_unique<UniValue>();
        bool parse_ok = false;

        try {
            parse_ok = json_request->read(*body);
            LogInfo("API: JSON parse result=%s", parse_ok ? "success" : "failed");
        } catch (const std::exception& e) {
            LogError("API: JSON parse exception: %s", e.what());
            parse_ok = false;
        }

        if (!parse_ok) {
            evbuffer_add_printf(buf, "{\"error\": \"JSON format error\"}");
            evhttp_send_reply(req, 400, "Bad Request", buf);
            evbuffer_free(buf);
            return;
        }

        std::string miner_id = "";
        std::string model_name = "";
        std::string gpu_name = "";
        int64_t gpu_vram_total_mb = 0;

        try {
            if (json_request->exists("miner_id")) {
                miner_id = (*json_request)["miner_id"].get_str();
            }
            if (json_request->exists("model_name")) {
                model_name = (*json_request)["model_name"].get_str();
            }
            if (json_request->exists("gpu_name")) {
                gpu_name = (*json_request)["gpu_name"].get_str();
            }
            if (json_request->exists("gpu_vram_total_mb")) {
                gpu_vram_total_mb = (int64_t)(*json_request)["gpu_vram_total_mb"].get_real();
            }
            LogInfo("API: Extracted miner_id=%s, model_name=%s, gpu_name=%s, vram=%lld",
                    miner_id.c_str(), model_name.c_str(), gpu_name.c_str(), (long long)gpu_vram_total_mb);
        } catch (const std::exception& e) {
            LogError("API: Field extraction exception: %s", e.what());
        }

        if (miner_id.empty()) {
            evbuffer_add_printf(buf, "{\"error\": \"miner_id required\"}");
            evhttp_send_reply(req, 400, "Bad Request", buf);
            evbuffer_free(buf);
            return;
        }

        if (gpu_name.empty()) {
            gpu_name = "Unknown GPU";
        }

        if (wallet_address_.empty()) {
            LogError("API: Miner registration failed - wallet address not set");
            evbuffer_add_printf(buf, "{\"error\": \"wallet address not configured\"}");
            evhttp_send_reply(req, 400, "Bad Request", buf);
            evbuffer_free(buf);
            return;
        }

        LiveMinerInfo info;
        info.miner_id = miner_id;
        info.model_name = model_name;
        info.gpu_name = gpu_name;
        info.gpu_vram_total_mb = gpu_vram_total_mb;
        info.api_port = port;
        std::string public_ip;
        try {
            if (json_request->exists("public_ip")) {
                public_ip = (*json_request)["public_ip"].get_str();
            }
        } catch (const std::exception& e) {
            LogError("API: public_ip extraction exception: %s", e.what());
        }
        if (!public_ip.empty() && public_ip != "127.0.0.1" && public_ip != "::1" && public_ip != "localhost") {
            info.ip_address = public_ip;
            LogInfo("API: Miner %s registered with public_ip=%s", miner_id.c_str(), public_ip.c_str());
        } else {
            info.ip_address = "";
            LogInfo("API: Miner %s registered without valid public_ip, will not be externally reachable", miner_id.c_str());
        }
        info.wallet_address = wallet_address_;

        bool success = RegisterMiner(info);
        LogInfo("API: RegisterMiner result=%s", success ? "success" : "failed");

        evbuffer_add_printf(buf,
            "{\"status\":\"%s\",\"message\":\"Miner registered\",\"miner_id\":\"%s\",\"timestamp\":%ld}",
            success ? "success" : "error",
            miner_id.c_str(),
            (long)GetTime()
        );
        evhttp_send_reply(req, 200, "OK", buf);

    } catch (const std::exception& e) {
        LogError("API: Register handler exception: %s", e.what());
        evbuffer_add_printf(buf, "{\"error\": \"Internal server error\"}");
        evhttp_send_reply(req, 500, "Internal Server Error", buf);
    }

    evbuffer_free(buf);
}

void APIServer::SaveMinerData() {
    std::string data_dir = this->data_dir;
    fs::create_directories(fs::u8path(data_dir));
    
    {
        std::lock_guard<std::mutex> lock(miners_mutex_);
        
        UniValue miners_array(UniValue::VARR);
        for (const auto& pair : live_miners_) {
            const LiveMinerInfo& info = pair.second;
            UniValue miner_obj(UniValue::VOBJ);
            miner_obj.pushKV("miner_id", info.miner_id);
            miner_obj.pushKV("model_name", info.model_name);
            miner_obj.pushKV("model_path", info.model_path);
            miner_obj.pushKV("status", info.status);
            miner_obj.pushKV("hashrate", info.hashrate);
            miner_obj.pushKV("gpu_name", info.gpu_name);
            miner_obj.pushKV("gpu_vram_total_mb", info.gpu_vram_total_mb);
            miner_obj.pushKV("total_blocks_found", info.total_blocks_found);
            miner_obj.pushKV("total_inference_requests", info.total_inference_requests);
            miner_obj.pushKV("total_earned", info.total_earned);
            miner_obj.pushKV("ip_address", info.ip_address);
            miner_obj.pushKV("api_port", info.api_port);
            miner_obj.pushKV("p2p_port", info.p2p_port);
            miner_obj.pushKV("registration_time", info.registration_time);
            miner_obj.pushKV("description", info.description);
            miner_obj.pushKV("price_per_1m_tknc", info.price_per_1m_tknc);
            miner_obj.pushKV("wallet_address", info.wallet_address);
            miner_obj.pushKV("last_seen", GetTime());
            miners_array.push_back(miner_obj);
        }
        
        std::string filepath = data_dir + "/miners_cache.json";
        std::ofstream file(filepath);
        if (file.is_open()) {
            file << miners_array.write();
            file.close();
            LogInfo("MinerData: Saved %d miners to %s", (int)live_miners_.size(), filepath.c_str());
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(identity_mutex_);
        UniValue identities_array(UniValue::VARR);
        for (const auto& pair : miner_identities_) {
            const MinerIdentity& id = pair.second;
            UniValue id_obj(UniValue::VOBJ);
            id_obj.pushKV("miner_id", id.miner_id);
            id_obj.pushKV("public_key", id.public_key);
            id_obj.pushKV("wallet_address", id.wallet_address);
            id_obj.pushKV("registered_at", id.registered_at);
            identities_array.push_back(id_obj);
        }
        std::string filepath = data_dir + "/identities_cache.json";
        std::ofstream file(filepath);
        if (file.is_open()) {
            file << identities_array.write();
            file.close();
            LogInfo("IdentityData: Saved %d identities to %s", (int)miner_identities_.size(), filepath.c_str());
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(stored_reviews_mutex_);
        UniValue reviews_obj(UniValue::VOBJ);
        for (const auto& pair : stored_reviews_) {
            UniValue reviews_array(UniValue::VARR);
            for (const auto& review : pair.second) {
                UniValue r(UniValue::VOBJ);
                r.pushKV("review_id", review.review_id);
                r.pushKV("miner_id", review.miner_id);
                r.pushKV("reviewer_api_key", review.reviewer_api_key);
                r.pushKV("rating", review.rating);
                r.pushKV("text", review.text);
                r.pushKV("timestamp", review.timestamp);
                r.pushKV("type", review.type);
                r.pushKV("likes", review.likes);
                r.pushKV("is_miner_initiated", review.is_miner_initiated);
                r.pushKV("usage_duration_seconds", review.usage_duration_seconds);
                r.pushKV("request_id", review.request_id);
                reviews_array.push_back(r);
            }
            reviews_obj.pushKV(pair.first, reviews_array);
        }
        std::string filepath = data_dir + "/reviews_cache.json";
        std::ofstream file(filepath);
        if (file.is_open()) {
            file << reviews_obj.write();
            file.close();
            LogInfo("ReviewData: Saved reviews for %d miners to %s", (int)stored_reviews_.size(), filepath.c_str());
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(usage_mutex_);
        UniValue usage_array(UniValue::VARR);
        for (const auto& pair : usage_records_) {
            const UsageRecord& rec = pair.second;
            UniValue r(UniValue::VOBJ);
            r.pushKV("user_api_key", rec.user_api_key);
            r.pushKV("miner_id", rec.miner_id);
            r.pushKV("start_time", rec.start_time);
            r.pushKV("end_time", rec.end_time);
            r.pushKV("amount_paid", rec.amount_paid);
            r.pushKV("tx_hash", rec.tx_hash);
            usage_array.push_back(r);
        }
        std::string filepath = data_dir + "/usage_cache.json";
        std::ofstream file(filepath);
        if (file.is_open()) {
            file << usage_array.write();
            file.close();
            LogInfo("UsageData: Saved %d usage records to %s", (int)usage_records_.size(), filepath.c_str());
        }
    }
}

void APIServer::LoadMinerData() {
    std::string data_dir = this->data_dir.empty() ? "data" : this->data_dir;
    
    {
        std::lock_guard<std::mutex> lock(miners_mutex_);
        
        std::string filepath = data_dir + "/miners_cache.json";
        std::ifstream file(filepath);
        if (file.is_open()) {
            std::string content((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
            file.close();
            
            UniValue miners_array;
            if (miners_array.read(content) && miners_array.isArray()) {
                for (size_t i = 0; i < miners_array.size(); i++) {
                    const UniValue& m = miners_array[i];
                    LiveMinerInfo info;
                    info.miner_id = m.exists("miner_id") ? m["miner_id"].get_str() : "";
                    info.model_name = m.exists("model_name") ? m["model_name"].get_str() : "";
                    info.model_path = m.exists("model_path") ? m["model_path"].get_str() : "";
                    info.status = m.exists("status") ? m["status"].get_str() : "offline";
                    if (!info.wallet_address.empty() && info.wallet_address == wallet_address_) {
                        info.status = "online";
                    }
                    info.hashrate = m.exists("hashrate") ? m["hashrate"].get_real() : 0;
                    info.gpu_name = m.exists("gpu_name") ? m["gpu_name"].get_str() : "";
                    info.gpu_vram_total_mb = m.exists("gpu_vram_total_mb") ? m["gpu_vram_total_mb"].getInt<int64_t>() : 0;
                    info.total_blocks_found = m.exists("total_blocks_found") ? m["total_blocks_found"].getInt<int64_t>() : 0;
                    info.total_inference_requests = m.exists("total_inference_requests") ? m["total_inference_requests"].getInt<int64_t>() : 0;
                    info.total_earned = m.exists("total_earned") ? m["total_earned"].getInt<int64_t>() : 0;
                    info.ip_address = m.exists("ip_address") ? m["ip_address"].get_str() : "";
                    info.api_port = m.exists("api_port") ? m["api_port"].getInt<int>() : 0;
                    info.p2p_port = m.exists("p2p_port") ? m["p2p_port"].getInt<int>() : 0;
                    info.registration_time = m.exists("registration_time") ? m["registration_time"].getInt<int64_t>() : 0;
                    info.description = m.exists("description") ? m["description"].get_str() : "";
                    info.price_per_1m_tknc = m.exists("price_per_1m_tknc") ? m["price_per_1m_tknc"].getInt<int64_t>() : 10;
                    info.wallet_address = m.exists("wallet_address") ? m["wallet_address"].get_str() : "";
                    info.last_heartbeat = m.exists("last_seen") ? m["last_seen"].getInt<int64_t>() : 0;
                    
                    if (!info.miner_id.empty()) {
                    bool walletDuplicate = false;
                    if (!info.wallet_address.empty()) {
                        for (const auto& [id, existing] : live_miners_) {
                            if (existing.wallet_address == info.wallet_address) {
                                walletDuplicate = true;
                                break;
                            }
                        }
                    }
                    if (!walletDuplicate) {
                        auto existing = live_miners_.find(info.miner_id);
                        if (existing != live_miners_.end() &&
                            (existing->second.status == "online" ||
                             existing->second.status == "mining" ||
                             existing->second.status == "inference")) {
                            info.status = existing->second.status;
                            info.last_heartbeat = existing->second.last_heartbeat;
                            info.hashrate = existing->second.hashrate;
                            info.gpu_utilization = existing->second.gpu_utilization;
                            info.current_block_height = existing->second.current_block_height;
                        }
                        live_miners_[info.miner_id] = info;
                    } else {
                        LogInfo("MinerData: Skipped duplicate wallet=%s (old id=%s) from cache",
                                info.wallet_address.c_str(), info.miner_id.c_str());
                    }
                }
                }
                LogInfo("MinerData: Loaded %d miners from cache", (int)live_miners_.size());
            }
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(identity_mutex_);
        std::string filepath = data_dir + "/identities_cache.json";
        std::ifstream file(filepath);
        if (file.is_open()) {
            std::string content((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
            file.close();
            
            UniValue identities_array;
            if (identities_array.read(content) && identities_array.isArray()) {
                for (size_t i = 0; i < identities_array.size(); i++) {
                    const UniValue& id_obj = identities_array[i];
                    MinerIdentity id;
                    id.miner_id = id_obj.exists("miner_id") ? id_obj["miner_id"].get_str() : "";
                    id.public_key = id_obj.exists("public_key") ? id_obj["public_key"].get_str() : "";
                    id.wallet_address = id_obj.exists("wallet_address") ? id_obj["wallet_address"].get_str() : "";
                    id.registered_at = id_obj.exists("registered_at") ? id_obj["registered_at"].getInt<int64_t>() : 0;
                    
                    if (!id.miner_id.empty()) {
                        miner_identities_[id.miner_id] = id;
                    }
                }
                LogInfo("IdentityData: Loaded %d identities from cache", (int)miner_identities_.size());
            }
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(stored_reviews_mutex_);
        std::string filepath = data_dir + "/reviews_cache.json";
        std::ifstream file(filepath);
        if (file.is_open()) {
            std::string content((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
            file.close();
            
            UniValue reviews_obj;
            if (reviews_obj.read(content) && reviews_obj.isObject()) {
                std::vector<std::string> keys = reviews_obj.getKeys();
                for (const auto& key : keys) {
                    const UniValue& reviews_array = reviews_obj[key];
                    if (!reviews_array.isArray()) continue;
                    
                    std::vector<StoredReview> reviews;
                    for (size_t i = 0; i < reviews_array.size(); i++) {
                        const UniValue& r = reviews_array[i];
                        StoredReview review;
                        review.review_id = r.exists("review_id") ? r["review_id"].get_str() : "";
                        review.miner_id = r.exists("miner_id") ? r["miner_id"].get_str() : "";
                        review.reviewer_api_key = r.exists("reviewer_api_key") ? r["reviewer_api_key"].get_str() : "";
                        review.rating = r.exists("rating") ? r["rating"].getInt<int>() : 0;
                        review.text = r.exists("text") ? r["text"].get_str() : "";
                        review.timestamp = r.exists("timestamp") ? r["timestamp"].getInt<int64_t>() : 0;
                        review.type = r.exists("type") ? r["type"].get_str() : "";
                        review.likes = r.exists("likes") ? r["likes"].getInt<int>() : 0;
                        review.is_miner_initiated = r.exists("is_miner_initiated") ? r["is_miner_initiated"].get_bool() : false;
                        review.usage_duration_seconds = r.exists("usage_duration_seconds") ? r["usage_duration_seconds"].getInt<int64_t>() : 0;
                        review.request_id = r.exists("request_id") ? r["request_id"].get_str() : "";
                        reviews.push_back(review);
                    }
                    stored_reviews_[key] = reviews;
                }
                LogInfo("ReviewData: Loaded reviews for %d miners from cache", (int)stored_reviews_.size());
            }
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(usage_mutex_);
        std::string filepath = data_dir + "/usage_cache.json";
        std::ifstream file(filepath);
        if (file.is_open()) {
            std::string content((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
            file.close();
            
            UniValue usage_array;
            if (usage_array.read(content) && usage_array.isArray()) {
                for (size_t i = 0; i < usage_array.size(); i++) {
                    const UniValue& r = usage_array[i];
                    UsageRecord rec;
                    rec.user_api_key = r.exists("user_api_key") ? r["user_api_key"].get_str() : "";
                    rec.miner_id = r.exists("miner_id") ? r["miner_id"].get_str() : "";
                    rec.start_time = r.exists("start_time") ? r["start_time"].getInt<int64_t>() : 0;
                    rec.end_time = r.exists("end_time") ? r["end_time"].getInt<int64_t>() : 0;
                    rec.amount_paid = r.exists("amount_paid") ? r["amount_paid"].getInt<int64_t>() : 0;
                    rec.tx_hash = r.exists("tx_hash") ? r["tx_hash"].get_str() : "";
                    
                    std::string record_key = rec.user_api_key + "_" + rec.miner_id + "_" + std::to_string(rec.start_time);
                    usage_records_[record_key] = rec;
                }
                LogInfo("UsageData: Loaded %d usage records from cache", (int)usage_records_.size());
            }
        }
    }
}

void APIServer::SaveConfig() {
    std::string filepath = data_dir + "/miner_config.json";
    UniValue config(UniValue::VOBJ);
    config.pushKV("manual_public_ip", manual_public_ip_);
    
    std::ofstream file(filepath);
    if (file.is_open()) {
        file << config.write();
        file.close();
        LogInfo("Config: Saved manual_public_ip=%s to %s", manual_public_ip_.c_str(), filepath.c_str());
    }
}

void APIServer::LoadConfig() {
    std::string filepath = data_dir + "/miner_config.json";
    std::ifstream file(filepath);
    if (!file.is_open()) return;
    
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();
    
    UniValue config;
    if (!config.read(content) || !config.isObject()) return;
    
    if (config.exists("manual_public_ip") && manual_public_ip_.empty()) {
        manual_public_ip_ = config["manual_public_ip"].get_str();
        if (!manual_public_ip_.empty()) {
            LogInfo("Config: Loaded manual_public_ip=%s from %s", manual_public_ip_.c_str(), filepath.c_str());
        }
    }
}

void APIServer::StaticChallengeHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleChallengeRequest(req);
}

void APIServer::HandleChallengeRequest(struct evhttp_request* req) {
    AddCORSHeaders(req);
    
    struct evbuffer* buf = evbuffer_new();
    if (!buf) {
        evhttp_send_error(req, HTTP_INTERNAL, "Failed to create buffer");
        return;
    }
    
    if (evhttp_request_get_command(req) == EVHTTP_REQ_OPTIONS) {
        evhttp_send_reply(req, 200, "OK", buf);
        evbuffer_free(buf);
        return;
    }
    
    std::string miner_id = GetQueryParam(req, "miner_id");
    
    if (miner_id.empty()) {
        struct evbuffer* in_buf = evhttp_request_get_input_buffer(req);
        size_t len = evbuffer_get_length(in_buf);
        if (len > 0 && len < 1024) {
            char* data = new char[len + 1];
            evbuffer_copyout(in_buf, data, len);
            data[len] = '\0';
            UniValue json_request;
            std::string data_str(data, len);
            if (json_request.read(data_str) && json_request.exists("miner_id")) {
                miner_id = json_request["miner_id"].get_str();
            }
            delete[] data;
        }
    }
    
    if (miner_id.empty()) {
        evbuffer_add_printf(buf, "{\"error\": \"miner_id required\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }
    
    std::string nonce;
    {
        std::vector<unsigned char> nonce_bytes(16);
        GetRandBytes(nonce_bytes);
        nonce = HexStr(nonce_bytes);
    }
    
    int64_t now = GetTime();
    std::string challenge_text = "TKNC-AUTH-" + nonce + "-" + std::to_string(now);
    
    AuthChallenge challenge;
    challenge.challenge_id = "ch_" + std::to_string(now) + "_" + nonce.substr(0, 8);
    challenge.miner_id = miner_id;
    challenge.challenge_text = challenge_text;
    challenge.created_at = now;
    challenge.expires_at = now + 600;
    challenge.used = false;
    
    {
        std::lock_guard<std::mutex> lock(challenges_mutex_);
        auth_challenges_[challenge.challenge_id] = challenge;
    }
    
    LogInfo("Auth: Challenge generated for miner %s: %s", 
            miner_id.substr(0, 8).c_str(), challenge.challenge_id.c_str());
    
    UniValue response(UniValue::VOBJ);
    response.pushKV("status", "success");
    response.pushKV("challenge_id", challenge.challenge_id);
    response.pushKV("challenge", challenge_text);
    response.pushKV("miner_id", miner_id);
    response.pushKV("expires_at", challenge.expires_at);
    response.pushKV("expires_in_seconds", 600);
    response.pushKV("instruction", "Sign this challenge with your wallet private key using: tknc-cli signmessage \"" + challenge_text + "\"");
    
    std::string json_response = response.write();
    evbuffer_add_printf(buf, "%s", json_response.c_str());
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

void APIServer::StaticWSHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleWSUpgrade(req);
}

void APIServer::HandleWSUpgrade(struct evhttp_request* req) {
    if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, "{\"error\": \"WebSocket requires GET method\"}");
        evhttp_send_reply(req, 405, "Method Not Allowed", buf);
        evbuffer_free(buf);
        return;
    }

    struct evkeyvalq* headers = evhttp_request_get_input_headers(req);
    const char* upgrade = evhttp_find_header(headers, "Upgrade");
    const char* ws_key = evhttp_find_header(headers, "Sec-WebSocket-Key");

    if (!upgrade || _stricmp(upgrade, "websocket") != 0 || !ws_key) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, "{\"error\": \"Use WebSocket upgrade for /ws/chat\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }

    static const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string accept_input = std::string(ws_key) + WS_GUID;

    CSHA1 sha1;
    sha1.Write(reinterpret_cast<const unsigned char*>(accept_input.data()), accept_input.size());
    unsigned char hash[CSHA1::OUTPUT_SIZE];
    sha1.Finalize(hash);

    std::string accept_key = EncodeBase64(std::string(reinterpret_cast<char*>(hash), CSHA1::OUTPUT_SIZE));

    struct evhttp_connection* evcon = evhttp_request_get_connection(req);
    struct bufferevent* bev = evcon ? evhttp_connection_get_bufferevent(evcon) : nullptr;

    if (!bev) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, "{\"error\": \"WebSocket upgrade failed: no connection\"}");
        evhttp_send_reply(req, 500, "Internal Server Error", buf);
        evbuffer_free(buf);
        return;
    }

    struct evkeyvalq* out_headers = evhttp_request_get_output_headers(req);
    evhttp_add_header(out_headers, "Upgrade", "websocket");
    evhttp_add_header(out_headers, "Connection", "Upgrade");
    evhttp_add_header(out_headers, "Sec-WebSocket-Accept", accept_key.c_str());
    // D-H04-FIX: Removed wildcard CORS from WebSocket upgrade - not needed for WS protocol
    // Browser same-origin policy does not apply to WebSocket handshakes

    evhttp_send_reply_start(req, 101, "Switching Protocols");

    LogInfo("WS: WebSocket upgrade successful");

    struct WSCallbackCtx {
        APIServer* server;
        std::string buffer;
    };
    auto* ws_ctx = new WSCallbackCtx();
    ws_ctx->server = this;

    bufferevent_setcb(bev,
        [](struct bufferevent* rbev, void* ctx) {
            auto* ws = static_cast<WSCallbackCtx*>(ctx);
            auto* input = bufferevent_get_input(rbev);
            size_t avail = evbuffer_get_length(input);
            if (avail == 0) return;

            std::vector<unsigned char> data(avail);
            evbuffer_remove(input, data.data(), avail);
            ws->buffer.append(reinterpret_cast<char*>(data.data()), avail);

            while (ws->buffer.size() >= 2) {
                const unsigned char* p = reinterpret_cast<const unsigned char*>(ws->buffer.data());
                unsigned char opcode = p[0] & 0x0F;
                bool masked = (p[1] & 0x80) != 0;
                uint64_t payload_len = p[1] & 0x7F;
                size_t header_size = 2;

                if (payload_len == 126) {
                    if (ws->buffer.size() < 4) return;
                    payload_len = (static_cast<uint64_t>(p[2]) << 8) | p[3];
                    header_size = 4;
                } else if (payload_len == 127) {
                    if (ws->buffer.size() < 10) return;
                    payload_len = 0;
                    for (int i = 0; i < 8; i++)
                        payload_len = (payload_len << 8) | p[2 + i];
                    header_size = 10;
                }

                size_t mask_offset = header_size;
                size_t frame_size = mask_offset + (masked ? 4 : 0) + static_cast<size_t>(payload_len);
                if (ws->buffer.size() < frame_size) return;

                if (opcode == 0x8) {
                    bufferevent_free(rbev);
                    delete ws;
                    return;
                }

                if (opcode == 0x9) {
                    std::string pong;
                    pong.push_back(static_cast<char>(0x8A));
                    pong.push_back(static_cast<char>(payload_len & 0x7F));
                    pong.append(reinterpret_cast<const char*>(p + mask_offset + (masked ? 4 : 0)),
                               static_cast<size_t>(payload_len));
                    bufferevent_write(rbev, pong.data(), pong.size());
                    ws->buffer.erase(0, frame_size);
                    continue;
                }

                if (opcode == 0x1) {
                    std::string payload;
                    if (masked) {
                        const unsigned char* mk = p + mask_offset;
                        const unsigned char* md = p + mask_offset + 4;
                        for (uint64_t i = 0; i < payload_len; i++)
                            payload.push_back(static_cast<char>(md[i] ^ mk[i % 4]));
                    } else {
                        payload.assign(reinterpret_cast<const char*>(p + mask_offset),
                                      static_cast<size_t>(payload_len));
                    }

                    UniValue req_json;
                    if (req_json.read(payload) && req_json.exists("api_key") && req_json.exists("messages")) {
                        std::string api_key = req_json["api_key"].get_str();
                        std::string prompt;
                        if (req_json["messages"].isArray()) {
                            const UniValue& msgs = req_json["messages"].get_array();
                            for (size_t i = 0; i < msgs.size(); i++)
                                if (msgs[i].exists("content"))
                                    prompt += msgs[i]["content"].get_str();
                        }

                        if (!prompt.empty()) {
                            if (!api_key.empty()) {
                                ws->server->llm_engine->GenerateStream(prompt,
                                    [rbev](const std::string& token, int) {
                                        std::string frame;
                                        frame.push_back(static_cast<char>(0x81));
                                        if (token.size() < 126) {
                                            frame.push_back(static_cast<char>(token.size()));
                                        } else if (token.size() < 65536) {
                                            frame.push_back(static_cast<char>(126));
                                            frame.push_back(static_cast<char>((token.size() >> 8) & 0xFF));
                                            frame.push_back(static_cast<char>(token.size() & 0xFF));
                                        }
                                        frame.append(token);
                                        bufferevent_write(rbev, frame.data(), frame.size());
                                    });
                                std::string done_frame;
                                done_frame.push_back(static_cast<char>(0x81));
                                std::string done_msg = "{\"type\":\"done\",\"status\":\"complete\"}";
                                done_frame.push_back(static_cast<char>(done_msg.size()));
                                done_frame.append(done_msg);
                                bufferevent_write(rbev, done_frame.data(), done_frame.size());
                            } else {
                                std::string err_frame;
                                err_frame.push_back(static_cast<char>(0x81));
                                std::string err_msg = "{\"type\":\"error\",\"message\":\"Invalid key or balance\"}";
                                err_frame.push_back(static_cast<char>(err_msg.size()));
                                err_frame.append(err_msg);
                                bufferevent_write(rbev, err_frame.data(), err_frame.size());
                            }
                        }
                    }
                }

                ws->buffer.erase(0, frame_size);
            }
        },
        nullptr,
        [](struct bufferevent* rbev, short events, void* ctx) {
            auto* ws = static_cast<WSCallbackCtx*>(ctx);
            delete ws;
        },
        ws_ctx);

    bufferevent_enable(bev, EV_READ);
    bufferevent_setwatermark(bev, EV_READ, 1, 0);
}

// DEAD CODE REMOVED: DetectPublicIP() moved to node/miner_registry.cpp.

void APIServer::StaticEditProfileHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleEditProfileRequest(req);
}

void APIServer::HandleEditProfileRequest(struct evhttp_request* req) {
    AddCORSHeaders(req);
    
    struct evbuffer* buf = evbuffer_new();
    if (!buf) {
        evhttp_send_error(req, HTTP_INTERNAL, "Failed to create buffer");
        return;
    }
    
    if (evhttp_request_get_command(req) == EVHTTP_REQ_OPTIONS) {
        evhttp_send_reply(req, 200, "OK", buf);
        evbuffer_free(buf);
        return;
    }
    
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        evbuffer_add_printf(buf, "{\"error\": \"POST only\"}");
        evhttp_send_reply(req, 405, "Method Not Allowed", buf);
        evbuffer_free(buf);
        return;
    }
    
    struct evbuffer* in_buf = evhttp_request_get_input_buffer(req);
    size_t len = evbuffer_get_length(in_buf);
    
    if (len == 0 || len > 10240) {
        evbuffer_add_printf(buf, "{\"error\": \"Invalid request body\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }
    
    char* data = new char[len + 1];
    evbuffer_copyout(in_buf, data, len);
    data[len] = '\0';
    
    UniValue json_request;
    std::string data_str(data, len);
    if (!json_request.read(data_str)) {
        delete[] data;
        evbuffer_add_printf(buf, "{\"error\": \"JSON format error\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }
    delete[] data;
    
    std::string miner_id = json_request.exists("miner_id") ? json_request["miner_id"].get_str() : "";
    std::string signature = json_request.exists("signature") ? json_request["signature"].get_str() : "";
    std::string public_key = json_request.exists("public_key") ? json_request["public_key"].get_str() : "";
    std::string challenge_id = json_request.exists("challenge_id") ? json_request["challenge_id"].get_str() : "";
    
    if (miner_id.empty() || signature.empty() || public_key.empty()) {
        evbuffer_add_printf(buf, "{\"error\": \"miner_id, signature, and public_key required\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(identity_mutex_);
        auto it = miner_identities_.find(miner_id);
        if (it == miner_identities_.end() || it->second.public_key != public_key) {
            evbuffer_add_printf(buf, "{\"error\": \"Identity not registered or public_key mismatch\"}");
            evhttp_send_reply(req, 403, "Forbidden", buf);
            evbuffer_free(buf);
            return;
        }
    }
    
    std::string challenge_text;
    if (!challenge_id.empty()) {
        std::lock_guard<std::mutex> lock(challenges_mutex_);
        auto it = auth_challenges_.find(challenge_id);
        if (it == auth_challenges_.end() || it->second.used || GetTime() > it->second.expires_at) {
            evbuffer_add_printf(buf, "{\"error\": \"Challenge not found, already used, or expired\"}");
            evhttp_send_reply(req, 401, "Unauthorized", buf);
            evbuffer_free(buf);
            return;
        }
        challenge_text = it->second.challenge_text;
        it->second.used = true;
    } else {
        challenge_text = json_request.exists("message") ? json_request["message"].get_str() : "";
    }
    
    bool valid_signature = false;
    {
        CHash256 hasher;
        std::span<const unsigned char> msg_span(
            reinterpret_cast<const unsigned char*>(challenge_text.data()),
            challenge_text.size()
        );
        hasher.Write(msg_span);
        uint256 message_hash;
        hasher.Finalize(message_hash);
        
        try {
            std::vector<unsigned char> sig_bytes = ParseHex(signature);
            CPubKey pubkey(ParseHex(public_key));
            if (pubkey.IsValid() && sig_bytes.size() > 0) {
                valid_signature = pubkey.Verify(message_hash, sig_bytes);
            }
        } catch (...) {}
    }
    
    if (!valid_signature) {
        evbuffer_add_printf(buf, "{\"error\": \"Invalid signature\"}");
        evhttp_send_reply(req, 401, "Unauthorized", buf);
        evbuffer_free(buf);
        return;
    }
    
    std::string description = json_request.exists("description") ? json_request["description"].get_str() : "";
    int64_t price_per_1m = json_request.exists("price_per_1m_tknc") ? json_request["price_per_1m_tknc"].getInt<int64_t>() : -1;
    
    {
        std::lock_guard<std::mutex> lock(miners_mutex_);
        auto it = live_miners_.find(miner_id);
        if (it == live_miners_.end()) {
            evbuffer_add_printf(buf, "{\"error\": \"Miner not found\"}");
            evhttp_send_reply(req, 404, "Not Found", buf);
            evbuffer_free(buf);
            return;
        }
        
        if (!description.empty()) {
            it->second.description = description;
        }
        if (price_per_1m >= 0) {
            it->second.price_per_1m_tknc = price_per_1m;
        }
    }
    
    LogInfo("EditProfile: Miner %s updated profile (desc=%s, price=%ld)",
            miner_id.substr(0, 8).c_str(), 
            description.empty() ? "unchanged" : "updated",
            price_per_1m >= 0 ? price_per_1m : -1);
    
    UniValue response(UniValue::VOBJ);
    response.pushKV("status", "success");
    response.pushKV("message", "Profile updated successfully");
    response.pushKV("miner_id", miner_id);
    
    std::string json_response = response.write();
    evbuffer_add_printf(buf, "%s", json_response.c_str());
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

void APIServer::StaticPublicIPNonceHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandlePublicIPNonceRequest(req);
}

void APIServer::HandlePublicIPNonceRequest(struct evhttp_request* req) {
    AddCORSHeaders(req);
    struct evbuffer* buf = evbuffer_new();
    
    if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
        evbuffer_add_printf(buf, "{\"error\": \"GET required\"}");
        evhttp_send_reply(req, 405, "Method Not Allowed", buf);
        evbuffer_free(buf);
        return;
    }
    
    std::string miner_id = GetQueryParam(req, "miner_id");
    if (miner_id.empty()) {
        evbuffer_add_printf(buf, "{\"error\": \"miner_id required\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }
    
    std::string nonce_str = std::to_string(GetTime());
    {
        std::lock_guard<std::mutex> lock(public_ip_nonces_mutex_);
        public_ip_nonces_[miner_id] = nonce_str;
    }
    
    std::string message = "SetPublicIP:" + miner_id + ":" + nonce_str;
    
    UniValue response(UniValue::VOBJ);
    response.pushKV("nonce", nonce_str);
    response.pushKV("miner_id", miner_id);
    response.pushKV("message", message);
    response.pushKV("message_type", "set_public_ip");
    
    std::string json_response = response.write();
    evbuffer_add_printf(buf, "%s", json_response.c_str());
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

void APIServer::StaticSetPublicIPHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleSetPublicIPRequest(req);
}

void APIServer::HandleSetPublicIPRequest(struct evhttp_request* req) {
    AddCORSHeaders(req);
    struct evbuffer* buf = evbuffer_new();
    
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        evbuffer_add_printf(buf, "{\"error\": \"POST required\"}");
        evhttp_send_reply(req, 405, "Method Not Allowed", buf);
        evbuffer_free(buf);
        return;
    }
    
    try {
        struct evbuffer* input_buf = evhttp_request_get_input_buffer(req);
        if (!input_buf) {
            evbuffer_add_printf(buf, "{\"error\": \"No input buffer\"}");
            evhttp_send_reply(req, 400, "Bad Request", buf);
            evbuffer_free(buf);
            return;
        }
        
        size_t len = evbuffer_get_length(input_buf);
        auto body = std::make_unique<std::string>();
        if (len > 0 && len < 1024 * 1024) {
            body->resize(len);
            evbuffer_copyout(input_buf, &(*body)[0], len);
        } else {
            evbuffer_add_printf(buf, "{\"error\": \"Invalid body\"}");
            evhttp_send_reply(req, 400, "Bad Request", buf);
            evbuffer_free(buf);
            return;
        }

        UniValue parsed(UniValue::VOBJ);
        if (!parsed.read(*body)) {
            evbuffer_add_printf(buf, "{\"error\": \"Invalid JSON\"}");
            evhttp_send_reply(req, 400, "Bad Request", buf);
            evbuffer_free(buf);
            return;
        }

        std::string public_ip;
        std::string wallet_address;
        std::string signature;
        std::string nonce_str;
        std::string miner_id;

        if (!parsed.exists("public_ip")) {
            evbuffer_add_printf(buf, "{\"error\": \"Missing public_ip field\"}");
            evhttp_send_reply(req, 400, "Bad Request", buf);
            evbuffer_free(buf);
            return;
        }
        public_ip = parsed["public_ip"].get_str();

        if (parsed.exists("wallet_address"))
            wallet_address = parsed["wallet_address"].get_str();
        if (parsed.exists("signature"))
            signature = parsed["signature"].get_str();
        if (parsed.exists("nonce"))
            nonce_str = parsed["nonce"].get_str();
        if (parsed.exists("miner_id"))
            miner_id = parsed["miner_id"].get_str();

        if (miner_id.empty() || wallet_address.empty() || signature.empty() || nonce_str.empty()) {
            evbuffer_add_printf(buf, "{\"error\": \"Missing required fields: miner_id, wallet_address, signature, nonce\"}");
            evhttp_send_reply(req, 400, "Bad Request", buf);
            evbuffer_free(buf);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(public_ip_nonces_mutex_);
            auto nit = public_ip_nonces_.find(miner_id);
            if (nit == public_ip_nonces_.end() || nit->second != nonce_str) {
                evbuffer_add_printf(buf, "{\"error\": \"Invalid or expired nonce. Please request a new one.\"}");
                evhttp_send_reply(req, 400, "Bad Request", buf);
                evbuffer_free(buf);
                return;
            }
            public_ip_nonces_.erase(nit);
        }

        std::string message = "SetPublicIP:" + miner_id + ":" + nonce_str;
        bool sig_valid = VerifyWalletSignature(wallet_address, signature, message);
        if (!sig_valid) {
            evbuffer_add_printf(buf, "{\"error\": \"Signature verification failed. You must sign with the miner owner wallet.\"}");
            evhttp_send_reply(req, 403, "Forbidden", buf);
            evbuffer_free(buf);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(miners_mutex_);
            auto it = live_miners_.find(miner_id);
            if (it != live_miners_.end()) {
                it->second.ip_address = public_ip;
            }
        }

        manual_public_ip_ = public_ip;
        SaveConfig();

        LogInfo("SetPublicIP: Miner %s updated public_ip=%s [SIGNATURE VERIFIED]",
                miner_id.substr(0, 8).c_str(), public_ip.c_str());

        UniValue response(UniValue::VOBJ);
        response.pushKV("status", "success");
        response.pushKV("message", "Public IP updated and verified");
        response.pushKV("miner_id", miner_id);
        response.pushKV("public_ip", public_ip);

        std::string json_response = response.write();
        evbuffer_add_printf(buf, "%s", json_response.c_str());
        evhttp_send_reply(req, 200, "OK", buf);
    } catch (const std::exception& e) {
        evbuffer_add_printf(buf, "{\"error\": \"Internal error: %s\"}", e.what());
        evhttp_send_reply(req, 500, "Internal Server Error", buf);
    }
    evbuffer_free(buf);
}

void APIServer::StaticMinerReviewsHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleMinerReviewsRequest(req);
}

void APIServer::HandleMinerReviewsRequest(struct evhttp_request* req) {
    AddCORSHeaders(req);

    struct evbuffer* buf = evbuffer_new();
    if (!buf) {
        evhttp_send_error(req, HTTP_INTERNAL, "Failed to create buffer");
        return;
    }

    std::string miner_id = GetQueryParam(req, "miner_id");

    if (miner_id.empty()) {
        evbuffer_add_printf(buf, "{\"error\": \"miner_id query parameter required\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }

    UniValue reviews(UniValue::VARR);
    {
        std::lock_guard<std::mutex> lock(stored_reviews_mutex_);
        auto it = stored_reviews_.find(miner_id);
        if (it != stored_reviews_.end()) {
            for (const auto& stored : it->second) {
                UniValue review(UniValue::VOBJ);
                review.pushKV("review_id", stored.review_id);
                review.pushKV("miner_id", stored.miner_id);
                review.pushKV("reviewer", stored.reviewer_api_key.substr(0, 8) + "...");
                review.pushKV("rating", stored.rating);
                review.pushKV("text", stored.text);
                review.pushKV("timestamp", stored.timestamp);
                review.pushKV("type", stored.type);
                review.pushKV("likes", stored.likes);
                review.pushKV("is_miner_initiated", stored.is_miner_initiated);
                review.pushKV("usage_duration_seconds", stored.usage_duration_seconds);
                reviews.push_back(review);
            }
        }
    }

    UniValue response(UniValue::VOBJ);
    response.pushKV("status", "success");
    response.pushKV("miner_id", miner_id);
    response.pushKV("total_reviews", (int)reviews.size());
    response.pushKV("reviews", reviews);

    std::string json_response = response.write();
    evbuffer_add_printf(buf, "%s", json_response.c_str());
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

void APIServer::StaticSystemResourceHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleSystemResourceRequest(req);
}

void APIServer::HandleSystemResourceRequest(struct evhttp_request* req) {
    AddCORSHeaders(req);

    struct evbuffer* buf = evbuffer_new();
    if (!buf) {
        evhttp_send_error(req, HTTP_INTERNAL, "Failed to create buffer");
        return;
    }

#ifdef WIN32
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);

    FILETIME idleTime, kernelTime, userTime;
    GetSystemTimes(&idleTime, &kernelTime, &userTime);

    ULARGE_INTEGER idle, kernel, user;
    idle.LowPart = idleTime.dwLowDateTime;
    idle.HighPart = idleTime.dwHighDateTime;
    kernel.LowPart = kernelTime.dwLowDateTime;
    kernel.HighPart = kernelTime.dwHighDateTime;
    user.LowPart = userTime.dwLowDateTime;
    user.HighPart = userTime.dwHighDateTime;

    double cpu_usage = 0.0;
    static ULARGE_INTEGER prev_idle;
    static ULARGE_INTEGER prev_kernel;
    static ULARGE_INTEGER prev_user;
    static bool first_call = true;
    if (first_call) {
        prev_idle.QuadPart = 0;
        prev_kernel.QuadPart = 0;
        prev_user.QuadPart = 0;
    }

    if (!first_call) {
        ULONGLONG idle_diff = idle.QuadPart - prev_idle.QuadPart;
        ULONGLONG kernel_diff = kernel.QuadPart - prev_kernel.QuadPart;
        ULONGLONG user_diff = user.QuadPart - prev_user.QuadPart;
        ULONGLONG total_diff = kernel_diff + user_diff - (kernel_diff - prev_kernel.QuadPart) +
                                (user_diff - prev_user.QuadPart);

        if (total_diff > 0) {
            cpu_usage = 100.0 - (double)(idle_diff * 100.0) / total_diff;
        }
    } else {
        first_call = false;
    }

    prev_idle = idle;
    prev_kernel = kernel;
    prev_user = user;
#else
    double cpu_usage = 0.0;
#endif

    float gpu_utilization = 0.0f;
    uint64_t gpu_total_memory = 0;
    uint64_t gpu_used_memory = 0;

    if (model_runtime && model_runtime->IsLoaded()) {
        GPUMemoryManager gpu_mgr;
        if (gpu_mgr.InitializeGPU(0)) {
            GPUStats stats = gpu_mgr.GetGPUStats(0);
            gpu_utilization = stats.usage_percent;
            gpu_total_memory = stats.memory.total / (1024 * 1024);
            gpu_used_memory = stats.memory.used / (1024 * 1024);
        }
    }

    bool cpu_warning = false;
    std::string system_status = "normal";

    if (cpu_usage > 30.0) {
        cpu_warning = true;
        system_status = "warning";

        LogWarning("System: HIGH CPU USAGE DETECTED: %.1f%% - Check for CPU-based mining/inference!",
                   cpu_usage);
    }

    if (gpu_utilization < 50.0 && gpu_utilization > 5.0) {
        LogWarning("System: Low GPU utilization (%.1f%%) with active mining - possible issue",
                   gpu_utilization);
    }

    UniValue resources(UniValue::VOBJ);
    resources.pushKV("timestamp", (int64_t)GetTime());

    UniValue cpu_info(UniValue::VOBJ);
#ifdef WIN32
    cpu_info.pushKV("usage_percent", cpu_usage);
    cpu_info.pushKV("warning_threshold_exceeded", cpu_warning);
    cpu_info.pushKV("status", cpu_warning ? "HIGH - Investigate immediately" : "Normal");
#endif
    resources.pushKV("cpu", cpu_info);

    UniValue memory_info(UniValue::VOBJ);
#ifdef WIN32
    memory_info.pushKV("total_mb", (int64_t)(memInfo.ullTotalPhys / (1024 * 1024)));
    memory_info.pushKV("available_mb", (int64_t)(memInfo.ullAvailPhys / (1024 * 1024)));
    memory_info.pushKV("used_mb", (int64_t)((memInfo.ullTotalPhys - memInfo.ullAvailPhys) / (1024 * 1024)));
    memory_info.pushKV("usage_percent",
                       (double)(memInfo.ullTotalPhys - memInfo.ullAvailPhys) * 100.0 / memInfo.ullTotalPhys);
#endif
    resources.pushKV("memory", memory_info);

    UniValue gpu_info(UniValue::VOBJ);
    gpu_info.pushKV("utilization_percent", gpu_utilization);
    gpu_info.pushKV("total_memory_mb", (int64_t)gpu_total_memory);
    gpu_info.pushKV("used_memory_mb", (int64_t)gpu_used_memory);
    gpu_info.pushKV("free_memory_mb", (int64_t)(gpu_total_memory - gpu_used_memory));
    gpu_info.pushKV("status", "Active");
    resources.pushKV("gpu", gpu_info);

    resources.pushKV("system_status", system_status);
    resources.pushKV("compliance_check", !cpu_warning ? "PASS" : "FAIL - CPU usage too high");
    resources.pushKV("enforcement_rule", "GPU-only computation required");

    UniValue response(UniValue::VOBJ);
    response.pushKV("status", "success");
    response.pushKV("resources", resources);

    std::string json_response = response.write();
    evbuffer_add_printf(buf, "%s", json_response.c_str());
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

void APIServer::StaticVerifyUsageHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleVerifyUsageRequest(req);
}

void APIServer::StaticVerifyMinerSignatureHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleVerifyMinerSignatureRequest(req);
}

void APIServer::StaticRegisterMinerIdentityHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleRegisterMinerIdentityRequest(req);
}

void APIServer::HandleVerifyUsageRequest(struct evhttp_request* req) {
    struct evbuffer* buf = evbuffer_new();
    if (!buf) {
        evhttp_send_error(req, HTTP_INTERNAL, "Failed to create buffer");
        return;
    }

    struct evbuffer* in_buf = evhttp_request_get_input_buffer(req);
    size_t len = evbuffer_get_length(in_buf);

    if (len == 0 || len > 1024 * 1024) {
        evbuffer_add_printf(buf, "{\"error\": \"Invalid request body\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }

    char* data = new char[len + 1];
    evbuffer_copyout(in_buf, data, len);
    data[len] = '\0';

    UniValue json_request;
    std::string data_str(data, len);
    if (!json_request.read(data_str)) {
        delete[] data;
        evbuffer_add_printf(buf, "{\"error\": \"JSON format error\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }
    delete[] data;

    std::string miner_id = json_request.exists("miner_id") ? json_request["miner_id"].get_str() : "";
    std::string tx_hash = json_request.exists("tx_hash") ? json_request["tx_hash"].get_str() : "";
    std::string user_address = json_request.exists("user_address") ? json_request["user_address"].get_str() : "";

    if (miner_id.empty() || tx_hash.empty()) {
        evbuffer_add_printf(buf, "{\"error\": \"miner_id and tx_hash required\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }

    std::lock_guard<std::mutex> lock(usage_mutex_);

    std::string record_key = user_address + "_" + miner_id + "_" + tx_hash;
    auto it = usage_records_.find(record_key);

    bool valid = false;
    UsageRecord record;

    if (it != usage_records_.end()) {
        valid = true;
        record = it->second;

        int64_t now = GetTime();
        record.end_time = now;
        usage_records_[record_key] = record;

        LogInfo("Auth: Verified usage - user=%s miner=%s tx=%s",
                user_address.substr(0, 8).c_str(),
                miner_id.substr(0, 8).c_str(),
                tx_hash.substr(0, 16).c_str());
    } else {
        LogInfo("Auth: No usage record found - user=%s miner=%s tx=%s",
                user_address.substr(0, 8).c_str(),
                miner_id.substr(0, 8).c_str(),
                tx_hash.substr(0, 16).c_str());
    }

    UniValue response(UniValue::VOBJ);
    response.pushKV("status", "success");
    response.pushKV("valid", valid);

    if (valid) {
        UniValue usage_info(UniValue::VOBJ);
        usage_info.pushKV("duration_seconds", record.end_time - record.start_time);
        usage_info.pushKV("amount_paid", (int64_t)(record.amount_paid / COIN));
        usage_info.pushKV("tx_hash", record.tx_hash);
        usage_info.pushKV("first_use_time", record.start_time);
        usage_info.pushKV("last_use_time", record.end_time);

        bool suspicious = ((record.end_time - record.start_time) < 600);
        usage_info.pushKV("suspicious_short_usage", suspicious);

        response.pushKV("usage_info", usage_info);
    }

    std::string json_response = response.write();
    evbuffer_add_printf(buf, "%s", json_response.c_str());
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

void APIServer::HandleVerifyMinerSignatureRequest(struct evhttp_request* req) {
    struct evbuffer* buf = evbuffer_new();
    if (!buf) {
        evhttp_send_error(req, HTTP_INTERNAL, "Failed to create buffer");
        return;
    }

    struct evbuffer* in_buf = evhttp_request_get_input_buffer(req);
    size_t len = evbuffer_get_length(in_buf);

    if (len == 0 || len > 1024 * 1024) {
        evbuffer_add_printf(buf, "{\"error\": \"Invalid request body\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }

    char* data = new char[len + 1];
    evbuffer_copyout(in_buf, data, len);
    data[len] = '\0';

    UniValue json_request;
    std::string data_str2(data, len);
    if (!json_request.read(data_str2)) {
        delete[] data;
        evbuffer_add_printf(buf, "{\"error\": \"JSON format error\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }
    delete[] data;

    std::string miner_id = json_request.exists("miner_id") ? json_request["miner_id"].get_str() : "";
    std::string message = json_request.exists("message") ? json_request["message"].get_str() : "";
    std::string signature = json_request.exists("signature") ? json_request["signature"].get_str() : "";
    std::string public_key = json_request.exists("public_key") ? json_request["public_key"].get_str() : "";

    if (miner_id.empty() || signature.empty() || public_key.empty()) {
        evbuffer_add_printf(buf, "{\"error\": \"miner_id, signature, and public_key required\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }

    std::lock_guard<std::mutex> lock(identity_mutex_);

    auto it = miner_identities_.find(miner_id);
    bool is_owner = false;
    bool valid_signature = false;

    if (it != miner_identities_.end()) {
        const MinerIdentity& identity = it->second;

        if (identity.public_key == public_key) {
            is_owner = true;

            CHash256 hasher;
            std::span<const unsigned char> msg_span(
                reinterpret_cast<const unsigned char*>(message.data()),
                message.size()
            );
            hasher.Write(msg_span);
            uint256 message_hash;
            hasher.Finalize(message_hash);

            std::vector<unsigned char> sig_bytes;
            try {
                sig_bytes = ParseHex(signature);

                CPubKey pubkey(ParseHex(public_key));

                if (pubkey.IsValid() && sig_bytes.size() > 0) {
                    valid_signature = pubkey.Verify(message_hash, sig_bytes);

                    LogInfo("Auth: Signature verification for %s: %s",
                            miner_id.substr(0, 8).c_str(),
                            valid_signature ? "VALID" : "INVALID");
                }
            } catch (...) {
                LogWarning("Auth: Invalid signature format from %s", miner_id.substr(0, 8).c_str());
            }
        }
    }

    UniValue response(UniValue::VOBJ);
    response.pushKV("status", "success");
    response.pushKV("valid", valid_signature);
    response.pushKV("is_owner", is_owner);
    response.pushKV("miner_id", miner_id);

    if (valid_signature && is_owner) {
        response.pushKV("permission_level", "full_edit");
        response.pushKV("can_edit_profile", true);
        response.pushKV("can_set_description", true);
        response.pushKV("can_manage_model_info", true);
    } else if (is_owner) {
        response.pushKV("permission_level", "view_only");
        response.pushKV("error", "Invalid signature");
    } else {
        response.pushKV("permission_level", "none");
        response.pushKV("error", "Not registered as owner");
    }

    std::string json_response = response.write();
    evbuffer_add_printf(buf, "%s", json_response.c_str());
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

void APIServer::HandleRegisterMinerIdentityRequest(struct evhttp_request* req) {
    struct evbuffer* buf = evbuffer_new();
    if (!buf) {
        evhttp_send_error(req, HTTP_INTERNAL, "Failed to create buffer");
        return;
    }

    struct evbuffer* in_buf = evhttp_request_get_input_buffer(req);
    size_t len = evbuffer_get_length(in_buf);

    if (len == 0 || len > 1024 * 1024) {
        evbuffer_add_printf(buf, "{\"error\": \"Invalid request body\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }

    char* data = new char[len + 1];
    evbuffer_copyout(in_buf, data, len);
    data[len] = '\0';

    UniValue json_request;
    std::string data_str3(data, len);
    if (!json_request.read(data_str3)) {
        delete[] data;
        evbuffer_add_printf(buf, "{\"error\": \"JSON format error\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }
    delete[] data;

    std::string miner_id = json_request.exists("miner_id") ? json_request["miner_id"].get_str() : "";
    std::string public_key = json_request.exists("public_key") ? json_request["public_key"].get_str() : "";
    std::string wallet_address = json_request.exists("wallet_address") ? json_request["wallet_address"].get_str() : "";

    if (miner_id.empty() || public_key.empty()) {
        evbuffer_add_printf(buf, "{\"error\": \"miner_id and public_key required\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }

    std::lock_guard<std::mutex> lock(identity_mutex_);

    MinerIdentity identity;
    identity.miner_id = miner_id;
    identity.public_key = public_key;
    identity.wallet_address = wallet_address;
    identity.registered_at = GetTime();

    miner_identities_[miner_id] = identity;

    LogInfo("Auth: Registered identity - miner=%s wallet=%s pub_key=%s...",
            miner_id.substr(0, 8).c_str(),
            wallet_address.substr(0, 10).c_str(),
            public_key.substr(0, 16).c_str());

    UniValue response(UniValue::VOBJ);
    response.pushKV("status", "success");
    response.pushKV("message", "Identity registered successfully");
    response.pushKV("miner_id", miner_id);
    response.pushKV("registered_at", identity.registered_at);
    response.pushKV("warning", "Never share your private key. Only sign messages locally.");

    std::string json_response = response.write();
    evbuffer_add_printf(buf, "%s", json_response.c_str());
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

void APIServer::HandleMinerHeartbeatRequest(struct evhttp_request* req) {
    struct evbuffer* buf = evbuffer_new();
    if (!buf) {
        evhttp_send_error(req, HTTP_INTERNAL, "Failed to create buffer");
        return;
    }

    struct evbuffer* input_buf = evhttp_request_get_input_buffer(req);
    size_t len = evbuffer_get_length(input_buf);

    if (len > MAX_CHAT_BODY_SIZE) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, "{\"error\": \"Request body too large\"}");
        evhttp_send_reply(req, 413, "Payload Too Large", buf);
        evbuffer_free(buf);
        return;
    }

    std::string body;
    body.resize(len);
    evbuffer_copyout(input_buf, &body[0], len);

    UniValue json_request;
    if (!json_request.read(body)) {
        evbuffer_add_printf(buf, "{\"error\": \"JSON format error\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }

    std::string miner_id = json_request.exists("miner_id") ? json_request["miner_id"].get_str() : "";

    if (miner_id.empty()) {
        evbuffer_add_printf(buf, "{\"error\": \"miner_id required\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }

    LiveMinerInfo update;
    update.model_name = json_request.exists("model_name") ? json_request["model_name"].get_str() : "";
    update.hashrate = json_request.exists("hashrate") ? json_request["hashrate"].get_real() : 0;
    update.status = json_request.exists("status") ? json_request["status"].get_str() : "mining";
    update.gpu_utilization = json_request.exists("gpu_utilization") ? json_request["gpu_utilization"].get_real() : 0;
    update.gpu_vram_used_mb = json_request.exists("gpu_vram_used_mb") ? json_request["gpu_vram_used_mb"].getInt<int64_t>() : 0;
    update.total_blocks_found = json_request.exists("total_blocks_found") ? json_request["total_blocks_found"].getInt<int64_t>() : 0;
    update.total_inference_requests = json_request.exists("total_inference_requests") ? json_request["total_inference_requests"].getInt<int64_t>() : 0;
    update.current_block_height = json_request.exists("current_block_height") ? json_request["current_block_height"].getInt<int64_t>() : 0;

    bool success = UpdateMinerHeartbeat(miner_id, update);

    if (!success) {
        LiveMinerInfo new_info;
        new_info.miner_id = miner_id;
        new_info.model_name = json_request.exists("model_name") ? json_request["model_name"].get_str() : "Unknown";
        new_info.hashrate = update.hashrate;
        new_info.status = update.status;
        RegisterMiner(new_info);
        success = true;
    }

    UniValue response(UniValue::VOBJ);
    response.pushKV("status", "success");
    response.pushKV("message", "Heartbeat received");
    response.pushKV("timestamp", (int64_t)GetTime());

    std::string json_response = response.write();
    evbuffer_add_printf(buf, "%s", json_response.c_str());
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

void APIServer::StaticMinerRegisterHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleMinerRegisterRequest(req);
}

void APIServer::StaticMinerHeartbeatHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleMinerHeartbeatRequest(req);
}

void APIServer::ServerLoop() {
    std::cout << "[API Server] ServerLoop(): entering event_base_dispatch()" << std::endl;
    event_base_dispatch(event_base_ptr);
    std::cout << "[API Server] ServerLoop(): event_base_dispatch() returned" << std::endl;
}

void APIServer::Stop() {
    if (running.exchange(false)) {

        // Stop async worker threads first (before event loop)
        StopWorkerThreads();

        cleanup_running_ = false;
        if (cleanup_thread_.joinable()) {
            cleanup_thread_.join();
        }

        // heartbeat_to_web_running_ and heartbeat_to_web_thread_ removed (dead code cleanup 2026-06-28)



        if (event_base_ptr) {
            event_base_loopbreak(event_base_ptr);
        }
        
        if (server_thread.joinable()) {
            server_thread.join();
        }
        
        if (http_ptr) {
            evhttp_free(http_ptr);
            http_ptr = nullptr;
        }
        
        if (event_base_ptr) {
            event_base_free(event_base_ptr);
            event_base_ptr = nullptr;
        }
        
        LogInfo("API: Server stopped, %d miners tracked", (int)live_miners_.size());
        SaveMinerData();
        
#ifdef WIN32
        WSACleanup();
#endif
    }
}

void APIServer::StaticChatHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleChatRequest(req);
}

// JSON escape helper for SSE streaming
static std::string SseJsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / 4);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Force-flush evhttp output buffer to socket (bypass event loop).
// This allows streaming from worker threads without event_base_once.
// evbuffer_write synchronously writes pending data to the socket FD.
static void FlushEvhttpOutput(struct evhttp_request* req) {
    if (!req) return;
    struct evhttp_connection* conn = evhttp_request_get_connection(req);
    if (!conn) return;
    struct bufferevent* bev = evhttp_connection_get_bufferevent(conn);
    if (!bev) return;
    struct evbuffer* output = bufferevent_get_output(bev);
    if (!output || evbuffer_get_length(output) == 0) return;
    evutil_socket_t fd = bufferevent_getfd(bev);
    if (fd >= 0) {
        evbuffer_write(output, fd);
    }
}

// Send SSE chunk directly from worker thread + force flush to socket.
// No event_base_once needed — evbuffer_write bypasses the event loop.
static void SendSseChunkDirect(struct evhttp_request* req,
                                const std::string& data,
                                bool is_start = false, bool is_end = false) {
    if (!req) return;

    if (is_start) {
        struct evkeyvalq* hdrs = evhttp_request_get_output_headers(req);
        evhttp_add_header(hdrs, "Content-Type", "text/event-stream");
        evhttp_add_header(hdrs, "Cache-Control", "no-cache");
        evhttp_add_header(hdrs, "Connection", "keep-alive");
        evhttp_send_reply_start(req, 200, "OK");
        FlushEvhttpOutput(req);
    }

    if (!data.empty()) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add(buf, data.c_str(), data.size());
        evhttp_send_reply_chunk(req, buf);
        evbuffer_free(buf);
        FlushEvhttpOutput(req);
    }

    if (is_end) {
        evhttp_send_reply_end(req);
        FlushEvhttpOutput(req);
    }
}

// REMOVED: HandleAPIRequest() prototype, replaced by StaticChatHandler → HandleChatRequest → CallLLM.

std::string APIServer::CallNodeRPC(const std::string& method, const std::string& params) {
    std::string jsonBody;
    if (params.empty() || params == "[]") {
        jsonBody = "{\"jsonrpc\":\"1.0\",\"method\":\"" + method + "\",\"params\":[],\"id\":1}";
    } else if (params[0] == '[' || params[0] == '{') {
        jsonBody = "{\"jsonrpc\":\"1.0\",\"method\":\"" + method + "\",\"params\":" + params + ",\"id\":1}";
    } else {
        jsonBody = "{\"jsonrpc\":\"1.0\",\"method\":\"" + method + "\",\"params\":[\"" + params + "\"],\"id\":1}";
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) { LogError("[RPC] socket creation failed: %d", WSAGetLastError()); return ""; }

    DWORD timeout = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(rpc_port_);
    int ptonResult = inet_pton(AF_INET, rpc_host_.c_str(), &serverAddr.sin_addr);
    if (ptonResult <= 0) {
        LogError("[RPC] invalid RPC host address: %s", rpc_host_.c_str());
        closesocket(sock);
        return "";
    }

    if (connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        LogError("[RPC] connection failed %s:%d error code: %d", rpc_host_.c_str(), rpc_port_, WSAGetLastError());
        closesocket(sock);
        return "";
    }

    std::string auth = rpc_user_ + ":" + rpc_password_;
    std::string encodedAuth = EncodeBase64(auth);

    std::string httpRequest =
        "POST / HTTP/1.1\r\n"
        "Host: " + rpc_host_ + ":" + std::to_string(rpc_port_) + "\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Basic " + encodedAuth + "\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(jsonBody.size()) + "\r\n"
        "\r\n" + jsonBody;

    if (send(sock, httpRequest.c_str(), (int)httpRequest.size(), 0) == SOCKET_ERROR) {
        LogError("[RPC] send request failed: %d", WSAGetLastError());
        closesocket(sock);
        return "";
    }

    std::string response;
    char buffer[4096];
    int bytesRead;
    while ((bytesRead = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytesRead] = '\0';
        response += buffer;
    }

    closesocket(sock);

    if (response.empty()) {
        LogWarning("[RPC] warning: %s returned empty response", method.c_str());
    }

    size_t bodyStart = response.find("\r\n\r\n");
    if (bodyStart != std::string::npos) {
        return response.substr(bodyStart + 4);
    }

    LogInfo("[RPC] response parse failed: no HTTP body separator");
    return "";
}

bool APIServer::VerifyWalletSignature(const std::string& wallet, const std::string& signature, const std::string& message) {
    if (wallet.empty() || signature.empty() || message.empty()) return false;
    
    std::string escaped_msg = message;
    size_t pos = 0;
    while ((pos = escaped_msg.find('"', pos)) != std::string::npos) {
        escaped_msg.insert(pos, "\\");
        pos += 2;
    }
    while ((pos = escaped_msg.find('\n', 0)) != std::string::npos) {
        escaped_msg.erase(pos, 1);
    }
    
    std::string params = "[\"" + wallet + "\",\"" + signature + "\",\"" + escaped_msg + "\"]";
    std::string response = CallNodeRPC("verifymessage", params);
    
    if (response.empty()) return false;
    
    UniValue rpc_result;
    if (!rpc_result.read(response)) return false;
    
    if (rpc_result.exists("result") && rpc_result["result"].isBool()) {
        return rpc_result["result"].get_bool();
    }
    
    return false;
}

bool APIServer::VerifyOnChainPayment(const std::string& tx_hash, const std::string& wallet_address, int64_t min_amount, int64_t& received_amount, const std::string& block_hash) {
    received_amount = 0;
    
    std::string txParams;
    if (!block_hash.empty()) {
        txParams = "[\"" + tx_hash + "\", 1, \"" + block_hash + "\"]";
    } else {
        txParams = "[\"" + tx_hash + "\", 1]";
    }
    std::string response = CallNodeRPC("getrawtransaction", txParams);
    if (response.empty()) {
        LogInfo("VerifyOnChainPayment: Transaction %s not found", tx_hash.substr(0,16).c_str());
        return false;
    }
    
    UniValue rpc_result;
    if (!rpc_result.read(response)) {
        LogInfo("VerifyOnChainPayment: Invalid JSON response");
        return false;
    }
    
    UniValue tx_data;
    if (rpc_result.exists("result")) {
        tx_data = rpc_result["result"];
    } else {
        tx_data = rpc_result;
    }
    
    if (!tx_data.isObject()) return false;
    
    int64_t confirmations = tx_data.exists("confirmations") ? tx_data["confirmations"].getInt<int64_t>() : 0;
    if (confirmations < 1) {
        LogInfo("VerifyOnChainPayment: Transaction not confirmed (confirmations=%ld)", confirmations);
        return false;
    }
    
    if (tx_data.exists("vout") && tx_data["vout"].isArray()) {
        const UniValue& vout = tx_data["vout"].get_array();
        for (size_t i = 0; i < vout.size(); i++) {
            const UniValue& output = vout[i];
            if (output.exists("scriptPubKey") && output["scriptPubKey"].isObject()) {
                const UniValue& script = output["scriptPubKey"];
                if (script.exists("address") && script["address"].get_str() == wallet_address) {
                    double value = output.exists("value") ? output["value"].get_real() : 0.0;
                    received_amount += static_cast<int64_t>(value * COIN);
                }
            }
        }
    }
    
    if (received_amount < min_amount * COIN) {
        LogInfo("VerifyOnChainPayment: Insufficient amount (received=%.8f, required=%ld TKNC)",
                static_cast<double>(received_amount) / COIN, min_amount);
        return false;
    }
    
    LogInfo("VerifyOnChainPayment: Verified tx=%s, received=%.8f TKNC, confirmations=%ld",
            tx_hash.substr(0,16).c_str(), static_cast<double>(received_amount) / COIN, confirmations);
    return true;
}

LLMInference::GenerationResult APIServer::CallLLM(const std::string& prompt) {
    LLMInference::GenerationResult result;  // Default: success=false, empty text
    if (llm_engine && llm_engine->IsInitialized()) {
        try {
            result = llm_engine->Generate(prompt,
                "You are a helpful assistant. Answer concisely and accurately.");

            if (result.success) {
                // Clean thinking markers from output text
                size_t think_pos = result.text.find("...done thinking.");
                if (think_pos != std::string::npos) {
                    result.text = result.text.substr(think_pos + strlen("...done thinking."));
                    while (!result.text.empty() && (result.text[0] == '\n' || result.text[0] == ' ')) {
                        result.text.erase(0, 1);
                    }
                }
                // prompt_tokens and completion_tokens are now preserved from llama.cpp
                return result;
            } else {
                LogError("[API] LLM inference failed: %s", result.error.c_str());
                result.text = "[LLM inference error: " + result.error + "]";
                return result;
            }
        } catch (const std::exception& e) {
            LogError("[API] FATAL: Exception during LLM inference: %s", e.what());
            result.success = false;
            result.error = std::string("FATAL: LLM exception - ") + e.what();
            result.text = "[FATAL: LLM inference crashed with exception - see logs for details]";
            return result;
        } catch (...) {
            LogError("[API] FATAL: Unknown exception during LLM inference (possibly Access Violation)");
            result.success = false;
            result.error = "FATAL: Unknown LLM exception (GPU memory?)";
            result.text = "[FATAL: LLM inference crashed - possible GPU memory access violation]";
            return result;
        }
    } else {
        result.success = false;
        result.error = "No model loaded";
        result.text = "[Inference unavailable: this miner has no LLM model loaded. Mining-only mode.]";
        return result;
    }
}

void APIServer::HandleChatRequest(struct evhttp_request* req) {
    try {
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, "{\"error\": \"POST only\"}");
        evhttp_send_reply(req, 405, "Method Not Allowed", buf);
        evbuffer_free(buf);
        return;
    }

    if (!llm_engine || !llm_engine->IsInitialized()) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, "{\"error\": \"Inference unavailable: this miner has no LLM model loaded. Mining-only mode.\"}");
        evhttp_send_reply(req, 503, "Service Unavailable", buf);
        evbuffer_free(buf);
        return;
    }

    struct evbuffer* input_buf = evhttp_request_get_input_buffer(req);
    size_t len = evbuffer_get_length(input_buf);

    std::string body;
    body.resize(len);
    evbuffer_copyout(input_buf, &body[0], len);

    UniValue json_request;
    if (!json_request.read(body)) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, "{\"error\": \"JSON format error\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }

    bool stream_mode = false;
    if (json_request.exists("stream") && json_request["stream"].isBool()) {
        stream_mode = json_request["stream"].get_bool();
    }

    // Miner only executes inference; authentication and billing handled by node.
    std::string api_key;
    if (json_request.exists("api_key")) {
        api_key = json_request["api_key"].get_str();
    }

    std::string prompt;
    if (json_request.exists("messages") && json_request["messages"].isArray()) {
        const UniValue& messages = json_request["messages"].get_array();
        for (size_t i = 0; i < messages.size(); i++) {
            if (messages[i].exists("content")) {
                prompt += messages[i]["content"].get_str();
            }
        }
    } else if (json_request.exists("prompt") && json_request["prompt"].isStr()) {
        prompt = json_request["prompt"].get_str();
    }

    if (prompt.empty()) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, "{\"error\": \"Message content is empty\"}");
        evhttp_send_reply(req, 400, "Bad Request", buf);
        evbuffer_free(buf);
        return;
    }

    // Parse max_tokens from request (-1 = unlimited, 0 = not specified, >0 = specific limit)
    int max_tokens = 0;
    if (json_request.exists("max_tokens")) {
        if (json_request["max_tokens"].isNum()) {
            max_tokens = (int)json_request["max_tokens"].getInt<int>();
        }
    } else if (json_request.exists("n_predict")) {
        if (json_request["n_predict"].isNum()) {
            max_tokens = (int)json_request["n_predict"].getInt<int>();
        }
    }

    // === ASYNC: Submit task to worker thread pool ===
    {
        AsyncChatTask task;
        task.req = req;
        task.prompt = prompt;
        task.api_key = api_key;
        task.stream_mode = stream_mode;
        task.request_id = g_request_id_counter.fetch_add(1, std::memory_order_relaxed);
        task.max_tokens = max_tokens;

        LogInfo("API: Queued async LLM inference request (id=%llu, stream=%s, max_tokens=%d)",
                (unsigned long long)task.request_id, stream_mode ? "true" : "false", max_tokens);

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);

            if (task_queue_.size() >= static_cast<size_t>(MAX_QUEUE_SIZE)) {
                struct evbuffer* buf = evbuffer_new();
                evbuffer_add_printf(buf, "{\"error\": \"Server busy: too many concurrent requests\"}");
                evhttp_send_reply(req, 503, "Service Unavailable", buf);
                evbuffer_free(buf);
                LogWarning("API: Task queue full, rejected request");
                return;
            }

            task_queue_.push(std::move(task));
        }
        queue_cv_.notify_one();

        LogInfo("API: Request queued for async processing (queue_size=%zu)", task_queue_.size());
    }
    } catch (const std::exception& e) {
        LogError("[API-CHAT] Exception in HandleChatRequest: %s", e.what());
        struct evbuffer* err_buf = evbuffer_new();
        evbuffer_add_printf(err_buf, "{\"error\": \"Internal error: %s\"}", e.what());
        evhttp_send_reply(req, 500, "Internal Server Error", err_buf);
        evbuffer_free(err_buf);
    } catch (...) {
        LogError("[API-CHAT] Unknown exception in HandleChatRequest");
        struct evbuffer* err_buf2 = evbuffer_new();
        evbuffer_add_printf(err_buf2, "{\"error\": \"Fatal internal error\"}");
        evhttp_send_reply(req, 500, "Internal Server Error", err_buf2);
        evbuffer_free(err_buf2);
    }
}

void APIServer::ProcessAsyncChatTask(AsyncChatTask& task) {
    if (!task.req) {
        LogError("[Async] Cannot send response: null request");
        return;
    }

    LogInfo("[Async] [GPU-SWITCH] Starting request %llu - switching GPU to INFERENCE mode (stream=%s)",
            (unsigned long long)task.request_id, task.stream_mode ? "true" : "false");

    // [BUG FIX #11] Use ModeSwitcher to prevent GPU conflict between PoW and LLM
    ModeSwitcher& mode_switcher = GetModeSwitcher();

    RequestGuard gpu_guard(mode_switcher, task.request_id);  // RAII: auto-restore on scope exit

    LogInfo("[Async] [GPU-SWITCH] GPU in INFERENCE mode, executing CallLLM for request %llu", (unsigned long long)task.request_id);

    // Apply max_tokens from client request. The system is a bridge — it passes
    // the client's request through to the LLM engine without capping it.
    // max_tokens = -1 means unlimited (generate until EOS).
    // max_tokens = 0 means use engine default (which is -1 = unlimited).
    // max_tokens > 0 means generate up to N tokens.
    if (llm_engine && llm_engine->IsInitialized()) {
        if (task.max_tokens != 0) {
            llm_engine->SetMaxTokens(task.max_tokens);
        }
    }

    // Streaming: GenerateStream + evbuffer_write for real-time SSE output from worker threads.
    if (task.stream_mode && llm_engine && llm_engine->IsInitialized()) {
        int64_t created = (int64_t)time(nullptr);
        std::string chat_id = "chatcmpl-miner-" + std::to_string(task.request_id);

        // Extract short model name (e.g. "qwen2.5-0.5b-instruct" from full path)
        // OpenAI clients (Trae IDE) require model field to match the requested model name
        std::string model_name;
        if (!model_path_.empty()) {
            size_t last_sep = model_path_.find_last_of("/\\");
            std::string filename = (last_sep != std::string::npos) ? model_path_.substr(last_sep + 1) : model_path_;
            size_t dot_pos = filename.find_last_of('.');
            model_name = (dot_pos != std::string::npos) ? filename.substr(0, dot_pos) : filename;
        } else {
            model_name = "mining-only";
        }

        // 1. Send HTTP headers + role chunk immediately (force flush to socket)
        std::string role_chunk =
            "data: {\"id\":\"" + chat_id + "\",\"object\":\"chat.completion.chunk\","
            "\"created\":" + std::to_string(created) + ",\"model\":\"" + SseJsonEscape(model_name) + "\","
            "\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"\"},\"finish_reason\":null}]}\n\n";
        SendSseChunkDirect(task.req, role_chunk, true, false);

        LogInfo("[Async] [STREAM] Role chunk sent + flushed for request %llu", (unsigned long long)task.request_id);

        // 2. Call GenerateStream — on_token callback sends each token as SSE chunk
        // Filter ChatML control tokens (<|im_end|>, <|im_start|>) and role markers
        int token_count = 0;
        std::string full_response;
        bool stop_emitting = false;
        std::string pending_lt;

        LLMInference::GenerationResult llm_result = llm_engine->GenerateStream(task.prompt,
            [&](const std::string& token, int index) {
                full_response += token;
                token_count++;

                if (stop_emitting) return;

                // Complete control token detected
                if (full_response.find("<|") != std::string::npos) {
                    stop_emitting = true;
                    pending_lt.clear();
                    return;
                }

                // Flush pending "<" if current token is not "|..."
                if (!pending_lt.empty()) {
                    if (!token.empty() && token[0] == '|') {
                        pending_lt.clear();
                        return;
                    }
                    std::string content_chunk =
                        "data: {\"id\":\"" + chat_id + "\",\"object\":\"chat.completion.chunk\","
                        "\"created\":" + std::to_string(created) + ",\"model\":\"" + SseJsonEscape(model_name) + "\","
                        "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"" + SseJsonEscape(pending_lt) + "\"},\"finish_reason\":null}]}\n\n";
                    SendSseChunkDirect(task.req, content_chunk, false, false);
                    pending_lt.clear();
                }

                // Skip ChatML control fragments and role markers
                if (token.find("|>") != std::string::npos) return;
                if (token == "Human:" || token == "Assistant:") return;

                // Hold back lone "<" — it may be the start of "<|im_end|>"
                if (token == "<") {
                    pending_lt = "<";
                    return;
                }

                std::string content_chunk =
                    "data: {\"id\":\"" + chat_id + "\",\"object\":\"chat.completion.chunk\","
                    "\"created\":" + std::to_string(created) + ",\"model\":\"" + SseJsonEscape(model_name) + "\","
                    "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"" + SseJsonEscape(token) + "\"},\"finish_reason\":null}]}\n\n";
                SendSseChunkDirect(task.req, content_chunk, false, false);
            },
            "You are a helpful assistant. Answer concisely and accurately.");

        LogInfo("[Async] [STREAM] GenerateStream completed for request %llu: tokens=%d, completion_tokens=%d",
                (unsigned long long)task.request_id, token_count, llm_result.completion_tokens);

        // 3. Send finish chunk + [DONE]
        std::string finish_chunk =
            "data: {\"id\":\"" + chat_id + "\",\"object\":\"chat.completion.chunk\","
            "\"created\":" + std::to_string(created) + ",\"model\":\"" + SseJsonEscape(model_name) + "\","
            "\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
            "data: [DONE]\n\n";
        SendSseChunkDirect(task.req, finish_chunk, false, true);

        // Accumulate inference request count
        {
            std::lock_guard<std::mutex> lock(miners_mutex_);
            auto mit = live_miners_.begin();
            if (mit != live_miners_.end()) {
                mit->second.total_inference_requests++;
            }
        }

        LogInfo("[Async] [STREAM] Sent streaming response: prompt_tokens=%d, completion_tokens=%d",
                llm_result.prompt_tokens, llm_result.completion_tokens);
        return;
    }

    // === NON-STREAMING MODE: original blocking path ===
    // Execute LLM inference (GPU now dedicated to LLM, PoW paused)
    LLMInference::GenerationResult llm_result = CallLLM(task.prompt);
    std::string llm_response = llm_result.text;

    LogInfo("[Async] [GPU-SWITCH] CallLLM completed for request %llu, response length=%zu, prompt_tokens=%d, completion_tokens=%d",
            (unsigned long long)task.request_id, llm_response.length(),
            llm_result.prompt_tokens, llm_result.completion_tokens);

    // Miner returns raw token counts; node computes and verifies tokens_used.
    UniValue json_response(UniValue::VOBJ);
    json_response.pushKV("response", llm_response);
    json_response.pushKV("prompt_tokens", llm_result.prompt_tokens);
    json_response.pushKV("completion_tokens", llm_result.completion_tokens);
    json_response.pushKV("model", model_path_);
    json_response.pushKV("status", "success");

    try {
        std::string json_str = json_response.write();

        // THREAD-SAFETY: schedule evhttp reply on event loop thread
        struct ReplyCtx {
            struct evhttp_request* req;
            std::string json_str;
        };
        auto* ctx = new ReplyCtx{task.req, std::move(json_str)};
        struct timeval tv_zero = {0, 0};
        LogInfo("[Async] Scheduling evhttp reply on event loop thread (json_size=%zu)", ctx->json_str.size());
        int rc = event_base_once(event_base_ptr, -1, EV_TIMEOUT,
            [](evutil_socket_t, short, void* arg) {
                auto* c = static_cast<ReplyCtx*>(arg);
                LogInfo("[Async] event_base_once callback executing, json_size=%zu", c->json_str.size());
                struct evbuffer* buf = evbuffer_new();
                if (!buf) {
                    evhttp_send_error(c->req, HTTP_INTERNAL, "Failed to create response buffer");
                    delete c;
                    return;
                }
                evbuffer_add_printf(buf, "%s", c->json_str.c_str());
                struct evkeyvalq* headers = evhttp_request_get_output_headers(c->req);
                if (headers) {
                    evhttp_add_header(headers, "Content-Type", "application/json");
                    const char* allowed_origin = std::getenv("TKNC_MINER_CORS_ORIGIN");
                    if (allowed_origin && strlen(allowed_origin) > 0) {
                        evhttp_add_header(headers, "Access-Control-Allow-Origin", allowed_origin);
                    }
                }
                evhttp_send_reply(c->req, 200, "OK", buf);
                evbuffer_free(buf);
                LogInfo("[Async] evhttp_send_reply completed");
                delete c;
            }, ctx, &tv_zero);
        LogInfo("[Async] event_base_once returned %d", rc);

        // Accumulate inference request count (token counting is done by the node, not the miner)
        {
            std::lock_guard<std::mutex> lock(miners_mutex_);
            auto mit = live_miners_.begin();
            if (mit != live_miners_.end()) {
                mit->second.total_inference_requests++;
            }
        }

        LogInfo("[Async] Sent chat response: prompt_tokens=%d, completion_tokens=%d (token counting is done by node)",
                llm_result.prompt_tokens, llm_result.completion_tokens);

    } catch (const std::exception& e) {
        LogError("[Async] Exception sending response: %s", e.what());
        if (task.req) {
            auto* err_req = task.req;
            struct timeval tv_zero = {0, 0};
            event_base_once(event_base_ptr, -1, EV_TIMEOUT,
                [](evutil_socket_t, short, void* arg) {
                    auto* r = static_cast<struct evhttp_request*>(arg);
                    evhttp_send_error(r, HTTP_INTERNAL, "Failed to send response");
                }, err_req, &tv_zero);
        }
    }
}

// === Missing function implementations ===

std::string APIServer::GetQueryParam(struct evhttp_request* req, const std::string& param) {
    const char* uri = evhttp_request_get_uri(req);
    if (!uri) return "";

    const char* query_start = strchr(uri, '?');
    if (!query_start) return "";
    query_start++; // skip '?'

    std::string query(query_start);
    std::string key = param + "=";
    size_t pos = query.find(key);
    if (pos == std::string::npos) return "";

    pos += key.length();
    size_t end = query.find('&', pos);
    if (end == std::string::npos) {
        return query.substr(pos);
    }
    return query.substr(pos, end - pos);
}

void APIServer::StartWorkerThreads() {
    workers_running_ = true;
    for (int i = 0; i < MAX_WORKER_THREADS; i++) {
        worker_threads_.emplace_back(&APIServer::WorkerThreadFunc, this, i);
    }
    LogInfo("API: Started %d async worker threads", MAX_WORKER_THREADS);
}

void APIServer::StopWorkerThreads() {
    workers_running_ = false;
    queue_cv_.notify_all();
    for (auto& t : worker_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    worker_threads_.clear();
    LogInfo("API: Stopped all async worker threads");
}

void APIServer::WorkerThreadFunc(int worker_id) {
    LogInfo("API: Worker thread %d started", worker_id);
    while (workers_running_) {
        AsyncChatTask task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() {
                return !workers_running_ || !task_queue_.empty();
            });

            if (!workers_running_ && task_queue_.empty()) {
                break;
            }

            task = std::move(task_queue_.front());
            task_queue_.pop();
        }

        LogInfo("API: Worker %d processing request (id=%llu)", worker_id,
                (unsigned long long)task.request_id);
        ProcessAsyncChatTask(task);
    }
    LogInfo("API: Worker thread %d stopped", worker_id);
}

void APIServer::RegisterExplorerEndpoints() {
    evhttp_set_cb(http_ptr, "/api/v1/miners", StaticMinersListHandler, this);
    evhttp_set_cb(http_ptr, "/api/v1/miner/detail", StaticMinerDetailHandler, this);
    evhttp_set_cb(http_ptr, "/api/v1/miner/submit_review", StaticSubmitReviewHandler, this);
    evhttp_set_cb(http_ptr, "/api/v1/miner/review/like", StaticLikeReviewHandler, this);
    evhttp_set_cb(http_ptr, "/api/v1/miner/request_review", StaticRequestReviewHandler, this);
    evhttp_set_cb(http_ptr, "/api/v1/miner/user_initiated_review", StaticUserInitiatedReviewHandler, this);
    evhttp_set_cb(http_ptr, "/api/v1/network/stats", StaticNetworkStatsHandler, this);
    evhttp_set_cb(http_ptr, "/api/v1/miner/register", StaticMinerRegisterHandler, this);
    evhttp_set_cb(http_ptr, "/api/v1/miner/heartbeat", StaticMinerHeartbeatHandler, this);
    evhttp_set_cb(http_ptr, "/api/v1/miner/verify_usage", StaticVerifyUsageHandler, this);
    evhttp_set_cb(http_ptr, "/api/v1/miner/verify_signature", StaticVerifyMinerSignatureHandler, this);
    evhttp_set_cb(http_ptr, "/api/v1/miner/register_identity", StaticRegisterMinerIdentityHandler, this);
    evhttp_set_cb(http_ptr, "/api/v1/miner/system_resource", StaticSystemResourceHandler, this);
    evhttp_set_cb(http_ptr, "/api/v1/miner/challenge", StaticChallengeHandler, this);
    evhttp_set_cb(http_ptr, "/api/v1/miner/edit_profile", StaticEditProfileHandler, this);
    evhttp_set_cb(http_ptr, "/api/v1/miner/public_ip_nonce", StaticPublicIPNonceHandler, this);
    evhttp_set_cb(http_ptr, "/api/v1/miner/set_public_ip", StaticSetPublicIPHandler, this);
    evhttp_set_cb(http_ptr, "/api/v1/miner/reviews", StaticMinerReviewsHandler, this);
    LogInfo("API: Registered explorer endpoints");
}

// === Missing explorer endpoint static handlers ===

void APIServer::StaticMinersListHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleMinersListRequest(req);
}

void APIServer::StaticMinerDetailHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleMinerDetailRequest(req);
}

void APIServer::StaticSubmitReviewHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleSubmitReviewRequest(req);
}

void APIServer::StaticLikeReviewHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleReviewLikeRequest(req);
}

void APIServer::StaticRequestReviewHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleRequestReviewRequest(req);
}

void APIServer::StaticUserInitiatedReviewHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleUserInitiatedReviewRequest(req);
}

void APIServer::StaticNetworkStatsHandler(struct evhttp_request* req, void* arg) {
    APIServer* server = static_cast<APIServer*>(arg);
    server->HandleNetworkStatsRequest(req);
}

// === Missing explorer endpoint handlers ===

void APIServer::HandleMinersListRequest(struct evhttp_request* req) {
    AddCORSHeaders(req);
    std::lock_guard<std::mutex> lock(miners_mutex_);
    UniValue response(UniValue::VOBJ);
    UniValue miners_arr(UniValue::VARR);
    for (const auto& pair : live_miners_) {
        const LiveMinerInfo& m = pair.second;
        UniValue miner_obj(UniValue::VOBJ);
        miner_obj.pushKV("miner_id", m.miner_id);
        miner_obj.pushKV("model_name", m.model_name);
        miner_obj.pushKV("status", m.status);
        miner_obj.pushKV("gpu_name", m.gpu_name);
        miner_obj.pushKV("gpu_vram_total_mb", m.gpu_vram_total_mb);
        miner_obj.pushKV("hashrate", m.hashrate);
        miner_obj.pushKV("wallet_address", m.wallet_address);
        miner_obj.pushKV("price_per_1m_tknc", m.price_per_1m_tknc);
        miners_arr.push_back(miner_obj);
    }
    response.pushKV("miners", miners_arr);
    response.pushKV("count", (int64_t)live_miners_.size());
    struct evbuffer* buf = evbuffer_new();
    evbuffer_add_printf(buf, "%s", response.write().c_str());
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

void APIServer::HandleMinerDetailRequest(struct evhttp_request* req) {
    AddCORSHeaders(req);
    std::string miner_id = GetQueryParam(req, "miner_id");
    std::lock_guard<std::mutex> lock(miners_mutex_);
    auto it = live_miners_.find(miner_id);
    struct evbuffer* buf = evbuffer_new();
    if (it != live_miners_.end()) {
        const LiveMinerInfo& m = it->second;
        UniValue detail(UniValue::VOBJ);
        detail.pushKV("miner_id", m.miner_id);
        detail.pushKV("model_name", m.model_name);
        detail.pushKV("status", m.status);
        detail.pushKV("gpu_name", m.gpu_name);
        detail.pushKV("gpu_vram_total_mb", m.gpu_vram_total_mb);
        detail.pushKV("gpu_vram_used_mb", m.gpu_vram_used_mb);
        detail.pushKV("gpu_utilization", m.gpu_utilization);
        detail.pushKV("hashrate", m.hashrate);
        detail.pushKV("wallet_address", m.wallet_address);
        detail.pushKV("price_per_1m_tknc", m.price_per_1m_tknc);
        detail.pushKV("total_blocks_found", m.total_blocks_found);
        detail.pushKV("total_inference_requests", m.total_inference_requests);
        evbuffer_add_printf(buf, "%s", detail.write().c_str());
    } else {
        evbuffer_add_printf(buf, "{\"error\": \"Miner not found\"}");
    }
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

void APIServer::HandleSubmitReviewRequest(struct evhttp_request* req) {
    AddCORSHeaders(req);
    struct evbuffer* buf = evbuffer_new();
    evbuffer_add_printf(buf, "{\"success\": false, \"error\": \"Review submission handled by Web server\"}");
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

void APIServer::HandleReviewLikeRequest(struct evhttp_request* req) {
    AddCORSHeaders(req);
    struct evbuffer* buf = evbuffer_new();
    evbuffer_add_printf(buf, "{\"success\": false, \"error\": \"Review likes handled by Web server\"}");
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

void APIServer::HandleRequestReviewRequest(struct evhttp_request* req) {
    AddCORSHeaders(req);
    struct evbuffer* buf = evbuffer_new();
    evbuffer_add_printf(buf, "{\"success\": false, \"error\": \"Review requests handled by Web server\"}");
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

void APIServer::HandleUserInitiatedReviewRequest(struct evhttp_request* req) {
    AddCORSHeaders(req);
    struct evbuffer* buf = evbuffer_new();
    evbuffer_add_printf(buf, "{\"success\": false, \"error\": \"User reviews handled by Web server\"}");
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}

void APIServer::HandleNetworkStatsRequest(struct evhttp_request* req) {
    AddCORSHeaders(req);
    std::string stats = CallNodeRPC("getblockchaininfo", "[]");
    UniValue result;
    if (stats.empty() || !result.read(stats)) {
        struct evbuffer* buf = evbuffer_new();
        evbuffer_add_printf(buf, "{\"error\": \"Failed to get network stats\"}");
        evhttp_send_reply(req, 500, "Internal Server Error", buf);
        evbuffer_free(buf);
        return;
    }
    struct evbuffer* buf = evbuffer_new();
    UniValue rpc_result = result.exists("result") ? result["result"] : result;
    evbuffer_add_printf(buf, "%s", rpc_result.write().c_str());
    evhttp_send_reply(req, 200, "OK", buf);
    evbuffer_free(buf);
}
