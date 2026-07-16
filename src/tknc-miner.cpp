#include <miner/miner.h>
#include <miner/mode.h>
#include <miner/mode_switcher.h>
#include <pow/transformerpow.h>
#include <pow/difficulty.h>
#include <net/api_protocol.h>
#include <model/loader.h>
#include <model/gpu_memory.h>
#include <model/llm_inference.h>
#include <pow/real_gpu_miner.h>
#include <apikey/api_key.h>
#include <apikey/api_key_db.h>
#include <miner/api_server.h>
#include <economics/emission.h>
#include <util/log.h>
#include <chain.h>
#include <chainparams.h>
#include <common/system.h>
#include <common/args.h>
#include <util/fs_helpers.h>
#include <util/fs.h>
#include <init.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <node/context.h>
#include <node/miner.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <util/time.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif
#include <util/thread.h>
#include <util/strencodings.h>
#include <random.h>
#include <arith_uint256.h>
#include <script/script.h>
#include <wallet/wallet_util.h>
#include <univalue.h>
#include <util/translation.h>

#include <chrono>
#include <thread>
#include <atomic>
#include <memory>
#include <algorithm>
#include <cinttypes>
#include <iostream>
#include <string>
#include <csignal>
#include <fstream>

const TranslateFn G_TRANSLATION_FUN{nullptr};

static std::atomic<bool> g_shutdown_requested(false);

void SignalHandler(int signal) {
    LogInfo("TKNC Miner: Received shutdown signal %d", signal);
    g_shutdown_requested.store(true);
}

bool SendHTTPPost(const std::string& url, const std::string& json_body, std::string& response) {
    try {
#ifdef _WIN32
        struct WSAInit {
            WSAInit() { WSAStartup(MAKEWORD(2, 2), &data); }
            ~WSAInit() { WSACleanup(); }
            WSADATA data;
        } wsa;

        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) {
            LogError("TKNC Miner: Failed to create socket");
            return false;
        }

        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);

        std::string host;
        std::string path;
        int port = 80;

        size_t proto_end = url.find("://");
        if (proto_end == std::string::npos) {
            closesocket(sock);
            return false;
        }

        size_t host_start = proto_end + 3;
        size_t port_sep = url.find(':', host_start);
        size_t path_start = url.find('/', host_start);

        if (port_sep != std::string::npos && port_sep < path_start) {
            host = url.substr(host_start, port_sep - host_start);
            port = std::stoi(url.substr(port_sep + 1, path_start - port_sep - 1));
        } else {
            host = url.substr(host_start, path_start - host_start);
        }
        path = url.substr(path_start);

        struct sockaddr_in server;
        memset(&server, 0, sizeof(server));
        server.sin_family = AF_INET;
        server.sin_port = htons(port);

        struct addrinfo hints, *result;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0) {
            closesocket(sock);
            return false;
        }

        memcpy(&server, result->ai_addr, result->ai_addrlen);
        freeaddrinfo(result);

        if (connect(sock, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                closesocket(sock);
                return false;
            }

            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(sock, &writeSet);
            timeval timeout;
            timeout.tv_sec = 5;
            timeout.tv_usec = 0;

            if (select(0, NULL, &writeSet, NULL, &timeout) <= 0) {
                closesocket(sock);
                return false;
            }
        }

        mode = 0;
        ioctlsocket(sock, FIONBIO, &mode);

        std::string request = "POST " + path + " HTTP/1.1\r\n";
        request += "Host: " + host + "\r\n";
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(json_body.size()) + "\r\n";
        request += "Connection: close\r\n\r\n";
        request += json_body;

        if (send(sock, request.c_str(), (int)request.size(), 0) == SOCKET_ERROR) {
            closesocket(sock);
            return false;
        }

        char buffer[4096];
        response = "";
        int bytesRead;
        while ((bytesRead = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
            buffer[bytesRead] = '\0';
            response += buffer;
        }

        closesocket(sock);

        size_t header_end = response.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            response = response.substr(header_end + 4);
        }

        bool success = !response.empty() && response.find("ERROR") == std::string::npos;
        return success;
#else
        (void)url; (void)json_body; (void)response;
        return false;
#endif
    } catch (const std::exception& e) {
        LogError("TKNC Miner: SendHTTPPost exception: %s", e.what());
        return false;
    }
}

std::string AutoDiscoverModel()
{
    fs::path exe_dir = GetExeDir();
    fs::path models_dir = exe_dir / "models";

    if (!fs::exists(models_dir) || !fs::is_directory(models_dir)) {
        LogWarning("AutoDiscoverModel: Models directory not found: %s", PathToString(models_dir).c_str());
        return "";
    }

    std::vector<fs::path> gguf_files;
    for (const auto& entry : fs::directory_iterator(models_dir)) {
        if (entry.path().extension() == ".gguf" && fs::is_regular_file(entry)) {
            gguf_files.push_back(entry.path());
        }
    }

    if (gguf_files.empty()) {
        LogWarning("AutoDiscoverModel: No .gguf files found in: %s", PathToString(models_dir).c_str());
        return "";
    }

    std::sort(gguf_files.begin(), gguf_files.end(), [](const fs::path& a, const fs::path& b) {
        return fs::file_size(a) > fs::file_size(b);
    });

    std::string discovered_model = PathToString(gguf_files[0]);
    return discovered_model;
}

std::string AutoDiscoverWallet(const std::string& data_dir)
{
    fs::path wallets_dir = fs::u8path(data_dir) / "wallets";

    if (!fs::exists(wallets_dir) || !fs::is_directory(wallets_dir)) {
        LogWarning("AutoDiscoverWallet: Wallets directory not found: %s", PathToString(wallets_dir).c_str());
        return "";
    }

    std::vector<fs::path> wallet_files;
    for (const auto& entry : fs::directory_iterator(wallets_dir)) {
        std::string ext = entry.path().extension().string();
        if ((ext == ".dat" || ext == ".wallet") && fs::is_regular_file(entry)) {
            wallet_files.push_back(entry.path());
        }
    }

    if (wallet_files.empty()) {
        LogWarning("AutoDiscoverWallet: No wallet files found in: %s", PathToString(wallets_dir).c_str());
        return "";
    }

    std::sort(wallet_files.begin(), wallet_files.end(), [](const fs::path& a, const fs::path& b) {
        return fs::last_write_time(a) > fs::last_write_time(b);
    });

    std::string wallet_filename = wallet_files[0].stem().string();
    return wallet_filename;
}

int main(int argc, char* argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#endif

    std::cout << "TKNC Miner v1.0" << std::endl;

    try {
        ArgsManager args;
        args.AddArg("-wallet", "Wallet address for mining rewards", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
        args.AddArg("-model", "LLM model path (auto-discover from models/ dir if omitted)", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
        args.AddArg("-n_ctx", "LLM context window size in tokens (default: 131072=128K. For 1M context use 1048576. Real limit is GPU VRAM)", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
args.AddArg("-token", "REQUIRED: Token exchange rate: N tokens = 1 TKNC. e.g. -token=100 means 100 tokens = 1 TKNC. Miner will NOT start without this.", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
        args.AddArg("-apiport", "API server port (default: 9332)", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
        // Miner does not participate in P2P network.
        args.AddArg("-rpcuser", "RPC username for tkncd connection", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
        args.AddArg("-rpcpassword", "RPC password for tkncd connection", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
        args.AddArg("-webserver", "Web server URL for miner registration (e.g. http://66.154.101.183)", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
        args.AddArg("-rpcport", "RPC port for tkncd (default: 9331)", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
        args.AddArg("-rpcconnect", "RPC host for tkncd (default: 127.0.0.1)", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
        args.AddArg("-miner-datadir", "Miner data directory for API keys LevelDB (default: ./data relative to exe)", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
        args.AddArg("-conf", "Specify configuration file (default: tknc.conf in exe directory). Reads rpcuser/rpcpassword from it.", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
        args.AddArg("-help", "Print this help message and exit (also -h or -?)", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    std::string error;
    if (!args.ParseParameters(argc, argv, error)) {
            std::cerr << "Error parsing parameters: " << error << std::endl;
            return 1;
    }

    // Read tknc.conf from exe directory to get rpcuser/rpcpassword (same as node).
    // FIX: Previously the miner did NOT read the config file, so it had no RPC credentials
    // when started without -rpcuser/-rpcpassword command-line args. This caused:
    //   1. Mining RPC (CallRPC) to fail with 401 Unauthorized
    //   2. miner_ready RPC (CallNodeRPC via APIServer) to fail with 401 Unauthorized
    //   3. Node never registered the miner with the web server
    //   4. Miner never appeared on WEB
    args.SelectConfigNetwork("main");
    std::string confError;
    if (!args.ReadConfigFiles(confError, true)) {
        LogWarning("TKNC Miner: Warning reading config file: %s", confError.c_str());
    } else {
        LogInfo("TKNC Miner: Config file loaded");
    }

    // === Help & Usage ===
    if (args.IsArgSet("-h") || args.IsArgSet("-help") || args.IsArgSet("-?")) {
        std::cout << R"(╔══════════════════════════════════════════════════════════════╗
║              TKNC Miner v1.0 - Usage Guide                 ║
╠══════════════════════════════════════════════════════════════╣
║                                                              ║
║  ARCHITECTURE:                                               ║
║    Miner(tknc-miner) → Node(tkncd:9331) → Seed(66.154.101.183)║
║    Miner ONLY listens on 127.0.0.1:9332 (local only)         ║
║                                                              ║
║  PREREQUISITES (must start FIRST):                           ║
║    1. tkncd.exe (node) must be running locally               ║
║    2. Model file: ./models/*.gguf (auto-discovered)          ║
║    3. DLLs in same directory as exe (Windows)                ║
║                                                              ║
╠══════════════════════════════════════════════════════════════╣
║  USAGE:                                                      ║
║    tknc-miner.exe [options]                                   ║
║                                                              ║
║  REQUIRED OPTIONS:                                           ║
║    -wallet=<addr>    Wallet address for mining rewards       ║
║                      Example: token1q9w6gxh...zp8jfpk64      ║
║                                                              ║
║  OPTIONAL OPTIONS:                                           ║
║    -rpcuser=<user>   RPC username for node (default: cookie auth)║
║    -rpcpassword=<pw> RPC password for node (default: cookie auth)║
║    -rpcport=<port>   Node RPC port (default: 9331)           ║
║    -rpcconnect=<host>Node RPC host (default: 127.0.0.1)      ║
║    -apiport=<port>  Miner API port (default: 9332)           ║
║    -model=<path>    LLM model path (auto-discover if omitted) ║
║    -n_ctx=<N>       LLM context window tokens (default: 131072=128K)║
║                      For 1M context: -n_ctx=1048576           ║
║                      Real limit is GPU VRAM, not this number  ║
║    -token=<N>       REQUIRED: N tokens = 1 TKNC               ║
║                      e.g. -token=100 means 100 tokens = 1 TKNC║
║                      Miner will NOT start without this!        ║
║    -webserver=<url> Web server URL for registration          ║
║    -miner-datadir=<path> Data directory (default: ./data)    ║
║                                                              ║
╠══════════════════════════════════════════════════════════════╣
║  EXAMPLES:                                                   ║
║                                                              ║
║  [1] Minimal start (auto-discover wallet + model):           ║
║      tknc-miner.exe                                          ║
║      # Requires: wallets/wallet.dat + models/*.gguf       ║
║                                                              ║
║  [2] Standard start with explicit wallet:                    ║
║      tknc-miner.exe -wallet=token1q9w6gxh...zp8jfpk64        ║
║                                                              ║
║  [3] Connect to custom node credentials:                     ║
║      tknc-miner.exe -wallet=token1q...                       ║
║                   -rpcuser=myuser -rpcpassword=mypass        ║
║                                                              ║
║  [4] Full production setup:                                  ║
║      tknc-miner.exe -wallet=token1q...                       ║
║                   -rpcuser=tknc -rpcpassword=pass123         ║
║                   -apiport=9332 -webserver=http://66.154.101.183║
║                                                              ║
╠══════════════════════════════════════════════════════════════╣
║  DIRECTORY STRUCTURE (relative to exe):                      ║
║                                                              ║
║    tknc-miner.exe          ← Executable                      ║
║    ├── models/             ← LLM model files                 ║
║    │   └── *.gguf  (auto-discovered, largest file loaded)   ║
║    ├── data/               ← Runtime data (blocks, chainstate)  ║
║    │   └── api_keys/         ← LevelDB (auto-created)        ║
║    ├── wallets/            ← Wallet files (root level)        ║
║    │   └── wallet.dat       (auto-created or pre-placed)      ║
║    ├── *.dll               ← Windows DLL dependencies       ║
│       (ggml.dll, llama.dll, etc.)                            ║
║                                                              ║
╠══════════════════════════════════════════════════════════════╣
║  TROUBLESHOOTING:                                            ║
║    • "Cannot connect to TKNC node" → Start tkncd.exe first!  ║
║    • "Model file not found" → Put .gguf in ./models/        ║
║    • "Wallet address required" → Use -wallet= or place      ║
║      wallet.dat in wallets/                                ║
║    • DLL missing → Ensure all .dll files are next to exe     ║
║                                                              ║
╚══════════════════════════════════════════════════════════════╝
)" << std::endl;
        return 0;
    }

    SelectParams(ChainType::MAIN);

        ECC_Context ecc_context;
        RandomInit();
        if (!SetupNetworking()) {
            LogError("TKNC Miner: Failed to setup networking");
            return 1;
        }
        InitLogging(args);

        // -token is MANDATORY: miner must not start without setting a token exchange rate.
        if (!args.IsArgSet("-token")) {
            std::cerr << "\nERROR: -token parameter is required!\n"
                         "   Usage: tknc-miner -token=<N> (N tokens = 1 TKNC)\n"
                         "   Example: tknc-miner -token=100  (100 tokens per 1 TKNC)\n"
                         "   The miner cannot start without setting the exchange rate.\n";
            LogError("TKNC Miner: Refusing to start - -token parameter is required.");
            return -1;
        }
        int token_rate = std::atoi(args.GetArg("-token", "0").c_str());
        if (token_rate <= 0) {
            std::cerr << "\nERROR: Invalid -token value! Must be a positive integer.\n"
                         "   Example: tknc-miner -token=100  (100 tokens per 1 TKNC)\n";
            LogError("TKNC Miner: Refusing to start - invalid -token value %d", token_rate);
            return -1;
        }

        // Ensure models directory exists at root level (Miner owns model management)
        TryCreateDirectories(GetExeDir() / "models");
        // Ensure dll directory exists at root level (Miner loads llama.dll from here)
        TryCreateDirectories(GetExeDir() / "dll");

        std::string walletAddress = args.GetArg("-wallet", "");
        std::string modelName = args.GetArg("-model", "");

        std::string modelPath;
        if (modelName.empty()) {
            std::string discovered_model = AutoDiscoverModel();
            if (!discovered_model.empty()) {
                modelPath = discovered_model;
                size_t last_slash = discovered_model.find_last_of("/\\");
                size_t last_dot = discovered_model.rfind(".");
                if (last_slash != std::string::npos && last_dot != std::string::npos && last_dot > last_slash) {
                    modelName = discovered_model.substr(last_slash + 1, last_dot - last_slash - 1);
                } else {
                    modelName = "auto-discovered";
                }
                LogInfo("Miner: Auto-discovered model: %s", modelPath.c_str());
            } else {
                LogWarning("Miner: No .gguf model files found in models/ directory. Running in mining-only mode (no LLM inference).");
                LogWarning("Miner: To enable inference, place .gguf model files in the models/ folder next to the miner executable.");
                modelPath = "";
                modelName = "mining-only";
            }
        } else {
            modelPath = modelName;
            // Extract short model name from user-specified path
            size_t last_slash = modelPath.find_last_of("/\\");
            size_t last_dot = modelPath.rfind(".");
            if (last_slash != std::string::npos && last_dot != std::string::npos && last_dot > last_slash) {
                modelName = modelPath.substr(last_slash + 1, last_dot - last_slash - 1);
            }
            LogInfo("Miner: Using user-specified model: %s (name: %s)", modelPath.c_str(), modelName.c_str());
        }
        int apiPort = args.GetIntArg("-apiport", 9332);
        // No P2P port — miner uses RPC only.

        std::string rpcUser = args.GetArg("-rpcuser", "");
        std::string rpcPassword = args.GetArg("-rpcpassword", "");
        int rpcPort = args.GetIntArg("-rpcport", 9331);
        std::string rpcConnect = args.GetArg("-rpcconnect", "127.0.0.1");
        std::string webServerUrl = args.GetArg("-webserver", "");
        if (rpcConnect.size() < 7 || rpcConnect.find('.') == std::string::npos) {
            LogWarning("rpcConnect truncated ('%s'), forcing to 127.0.0.1", rpcConnect.c_str());
            rpcConnect = "127.0.0.1";
        }

        if (walletAddress.empty()) {
            std::string dataDirEarly;
#ifdef _WIN32
            char exe_path_data[MAX_PATH] = {0};
            GetModuleFileNameA(NULL, exe_path_data, MAX_PATH);
            fs::path data_dir_path = fs::path(exe_path_data).parent_path();
            dataDirEarly = PathToString(data_dir_path);
#else
            dataDirEarly = ".";
#endif
            std::string discovered_wallet = AutoDiscoverWallet(dataDirEarly);
            if (!discovered_wallet.empty()) {
                walletAddress = discovered_wallet;
                LogInfo("Miner: Auto-discovered wallet: %s", walletAddress.c_str());
            } else {
                std::cerr << "Error: Wallet address is required!" << std::endl;
                std::cerr << "  No wallet found in: " << PathToString(fs::u8path(dataDirEarly) / "wallets") << std::endl;
                std::cerr << "  Solutions:" << std::endl;
                std::cerr << "  1. Specify -wallet=<address>" << std::endl;
                std::cerr << "  2. Place wallet.dat in wallets/" << std::endl;
                return 1;
            }
        }

        const CChainParams& chainparams = Params();

        std::cout << "Wallet: " << walletAddress << " | Model: " << modelName << std::endl;

        std::signal(SIGINT, SignalHandler);
        std::signal(SIGTERM, SignalHandler);

        LogInfo("TKNC Miner: Wallet address: %s", walletAddress);

        std::string addr_error;
        if (!ValidateWalletAddress(walletAddress, addr_error)) {
            std::cerr << "Error: Invalid wallet address!" << std::endl;
            std::cerr << "  Reason: " << addr_error << std::endl;
            std::cerr << "  Valid formats:" << std::endl;
            std::cerr << "    Bech32:  token1q... (42 chars)" << std::endl;
            std::cerr << "    Legacy:   t... (Base58 P2PKH)" << std::endl;
            return 1;
        }

        LogInfo("Miner: Model path: %s (name: %s)", modelPath.c_str(), modelName.c_str());

        // NOTE: Do NOT call ModelLoader::PreloadModel here.
        // PreloadModel creates a SEPARATE LLMInference instance that loads the model
        // into GPU VRAM (~4GB). APIServer::Start() later creates its own LLMInference
        // and tries to load the same model again. With the first copy still in VRAM,
        // the second load fails with Vulkan OutOfDeviceMemory.
        // APIServer::Start() handles all LLM initialization for inference requests.
        if (modelPath.empty() || !fs::exists(fs::PathFromString(modelPath))) {
            if (modelPath.empty()) {
                std::cerr << "Notice: No model file specified or discovered." << std::endl;
            } else {
                std::cerr << "Warning: Model file not found at: " << modelPath << std::endl;
            }
            std::cerr << "Starting in PoW-only mode. LLM inference will be unavailable." << std::endl;
            std::cerr << "To enable LLM inference, place .gguf model files in the models/ folder." << std::endl;
            LogWarning("Miner: No model loaded, starting in PoW-only mode");
        }

        std::string dataDir;
        std::string minerDatadirFromArgv = "";
        for (int i = 1; i < argc; i++) {
            std::string arg(argv[i]);
            if (arg.find("-miner-datadir=") == 0) {
                minerDatadirFromArgv = arg.substr(strlen("-miner-datadir="));
                break;
            }
        }
        if (!minerDatadirFromArgv.empty()) {
            dataDir = minerDatadirFromArgv;
            LogInfo("TKNC Miner: Using -miner-datadir=%s (from argv)", dataDir);
        } else {
            dataDir = fs::PathToString(GetExeDir().parent_path() / "data");
            LogInfo("TKNC Miner: Using default data dir: %s", dataDir.c_str());
        }
        // FIX: Resolve RPC credentials BEFORE constructing APIServer.
        // Previously, APIServer was constructed with empty rpcUser/rpcPassword when no
        // -rpcpassword was specified, because cookie auth was read AFTER construction.
        // This caused CallNodeRPC("miner_ready") to fail with 401, preventing the node
        // from registering the miner with the web server.
        //
        // Credential resolution order:
        //   1. Command-line args (-rpcuser/-rpcpassword)
        //   2. Config file (tknc.conf, read via ReadConfigFiles above)
        //   3. Cookie file (<exe>/data/.cookie, when node uses cookie auth)
        //   4. Environment variables (TKNC_RPC_USER/TKNC_RPC_PASS)
        //   5. Hardcoded defaults (tkncadmin/tkncpass123, set in miner.cpp globals)
        LogInfo("TKNC Miner: Connecting to RPC at %s:%d", rpcConnect.c_str(), rpcPort);

        // Cookie-based RPC auth: try reading node's cookie file if no -rpcpassword specified.
        if (rpcPassword.empty() && !args.IsArgSet("-rpcpassword")) {
            std::string cookieUser, cookiePass;
            if (TryReadCookieAuth(cookieUser, cookiePass)) {
                rpcUser = cookieUser;
                rpcPassword = cookiePass;
                LogInfo("TKNC Miner: Using cookie-based RPC authentication");
            }
        }

        // If still no credentials, fall back to environment variables.
        if (rpcUser.empty()) {
            const char* envUser = std::getenv("TKNC_RPC_USER");
            if (envUser) rpcUser = envUser;
        }
        if (rpcPassword.empty()) {
            const char* envPass = std::getenv("TKNC_RPC_PASS");
            if (envPass) rpcPassword = envPass;
        }

        // If still no credentials, use hardcoded defaults (must match tknc.conf).
        if (rpcUser.empty()) {
            rpcUser = "tkncadmin";
            LogWarning("TKNC Miner: No RPC credentials found, using default user: %s", rpcUser.c_str());
        }
        if (rpcPassword.empty()) {
            rpcPassword = "tkncpass123";
            LogWarning("TKNC Miner: No RPC password found, using default password");
        }

        LogInfo("TKNC Miner: RPC credentials resolved (user=%s)", rpcUser.c_str());

        // Set RPC config for mining RPC calls (CallRPC in miner.cpp)
        SetRPCConfig(rpcUser, rpcPassword, rpcConnect, rpcPort, walletAddress);

        // NOW construct APIServer with the correct resolved credentials.
        // This ensures CallNodeRPC("miner_ready") can authenticate with the node.
        APIServer apiServer(nullptr, dataDir, modelPath, apiPort, rpcConnect, rpcPort, rpcUser, rpcPassword);
        std::string minerId = walletAddress;
        apiServer.SetWalletAddress(walletAddress);
        // Parse -n_ctx parameter (context window size). Default 131072 (128K).
        // For 1M context models (e.g. GLM5.2), use -n_ctx=1048576 with sufficient GPU VRAM.
        // The real limit is the miner's GPU VRAM, not this number.
        if (args.IsArgSet("-n_ctx")) {
            int n_ctx = std::atoi(args.GetArg("-n_ctx", "131072").c_str());
            if (n_ctx > 0) {
                apiServer.SetContextLength(n_ctx);
                LogInfo("TKNC Miner: Context window set to %d tokens (-n_ctx)", n_ctx);
            }
        } else {
            LogInfo("TKNC Miner: Context window default 131072 (128K). Use -n_ctx=<N> to override.");
        }
// -token was already validated and parsed above (before wallet validation)
apiServer.SetTokensPerTknc(token_rate);
LogInfo("TKNC Miner: Token rate set to %d tokens = 1 TKNC", token_rate);
        if (!webServerUrl.empty()) {
            apiServer.SetWebServerUrl(webServerUrl);
            LogInfo("TKNC Miner: Web server URL overridden: %s", webServerUrl.c_str());
        } else {
            LogInfo("TKNC Miner: Using default Web server (seed): http://66.154.101.183");
        }
        if (!apiServer.Start()) {
            std::cerr << "Error: API server start failed" << std::endl;
            return 1;
        }
        LogInfo("TKNC Miner: API server on port %d (event loop started inside Start())", apiPort);
        // Miner only executes inference; auth/billing handled by node (inference_gateway.cpp).

        std::vector<unsigned char> rand_bytes(32);
        GetRandBytes(rand_bytes);

        if (!GenerateTKNC(true, 1, chainparams)) {
            std::cerr << "\n❌ Miner failed to start: Node not available or blockchain error." << std::endl;
            std::cerr << "   Please ensure tkncd.exe is running before starting the miner." << std::endl;
            LogError("TKNC Miner: Exiting due to node connection failure");
            return -1;
        }

        std::cout << "Mining started (Ctrl+C to stop)" << std::endl;

        auto lastLogTime = std::chrono::steady_clock::now();
        auto lastHeartbeatTime = std::chrono::steady_clock::now();

        int64_t totalBlocksFound = 0;
        uint64_t prevDispatchedHashes = 0;
        auto lastHashrateTime = std::chrono::steady_clock::now();
        std::string prevStatus = "pow";

        std::atomic<bool> monitor_running{true};
        std::thread monitor_thread([&monitor_running]() {
            while (monitor_running.load()) {
#ifdef _WIN32
                MEMORYSTATUSEX memInfo;
                memInfo.dwLength = sizeof(MEMORYSTATUSEX);
                GlobalMemoryStatusEx(&memInfo);
                DWORDLONG usedMB = (memInfo.ullTotalPhys - memInfo.ullAvailPhys) / (1024 * 1024);
                DWORDLONG totalMB = memInfo.ullTotalPhys / (1024 * 1024);
                LogInfo("[Monitor] Memory: %llu/%llu MB (%.1f%%)",
                       usedMB, totalMB,
                       (double)usedMB / totalMB * 100);
#endif
                std::this_thread::sleep_for(std::chrono::seconds(30));
            }
        });

        while (!g_shutdown_requested.load()) {
            try {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastLogTime).count();
                auto heartbeatElapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastHeartbeatTime).count();

                double currentHashrate = 0.0;
                std::string currentStatus = "pow";

                bool forcePrint = false;
                if (elapsed >= 30) {
                    forcePrint = true;
                }

                if (forcePrint) {
                    UniValue miningInfo(UniValue::VOBJ);
                    if (GetMiningInfo(chainparams, miningInfo)) {

                        if (miningInfo.exists("networkhashps")) {
                            currentHashrate = miningInfo["networkhashps"].get_real();
                        } else if (miningInfo.exists("hashrate")) {
                            currentHashrate = miningInfo["hashrate"].get_real();
                        }
                        if (RealGPUMiner::IsAvailable() && currentHashrate < 0.001) {
                            uint64_t dispatched = RealGPUMiner::GetTotalDispatchedHashes();
                            auto nowHashrate = std::chrono::steady_clock::now();
                            double elapsedSec = std::chrono::duration<double>(nowHashrate - lastHashrateTime).count();
                            if (prevDispatchedHashes > 0 && dispatched > prevDispatchedHashes && elapsedSec > 0) {
                                currentHashrate = static_cast<double>(dispatched - prevDispatchedHashes) / elapsedSec;
                            } else if (elapsedSec > 0 && dispatched > 0) {
                                currentHashrate = static_cast<double>(dispatched) / elapsedSec;
                            }
                            prevDispatchedHashes = dispatched;
                            lastHashrateTime = nowHashrate;
                        }
                        if (miningInfo.exists("mining_mode")) {
                            currentStatus = miningInfo["mining_mode"].get_str();
                        }

                        if (RealGPUMiner::IsAvailable()) {
                            auto localMode = RealGPUMiner::GetCurrentMode();
                            if (localMode == RealGPUMiner::GPUMode::INFERENCE) {
                                currentStatus = "LLM";
                            } else if (localMode == RealGPUMiner::GPUMode::IDLE) {
                                currentStatus = "idle";
                            }
                        }

                        int64_t globalHeight = miningInfo.exists("chain_height") ? miningInfo["chain_height"].getInt<int64_t>() : 0;
                        totalBlocksFound = miningInfo["blocks"].getInt<int64_t>();

                        if (currentStatus != prevStatus) {
                            LogInfo("Miner: Mode change %s -> %s at height=%" PRId64,
                                    prevStatus.c_str(), currentStatus.c_str(), globalHeight);
                            prevStatus = currentStatus;
                        }

                        float gpuLoad = RealGPUMiner::IsAvailable() ? RealGPUMiner::GetRealGPULoad() : 0.0f;
                        static double floor_h2 = 0.0;
                        static float floor_g2 = 0.0f;
                        if (currentHashrate >= 1.0) floor_h2 = currentHashrate;
                        if (gpuLoad >= 1.0f) floor_g2 = gpuLoad;
                        if (currentHashrate < 1.0 && floor_h2 > 0.0) currentHashrate = floor_h2;
                        if (gpuLoad < 1.0f && floor_g2 > 0.0f) gpuLoad = floor_g2;
                        if (currentHashrate < 1.0 || gpuLoad < 1.0f) continue;
                        std::string hashRateStr = RealGPUMiner::FormatHashrate(currentHashrate);

                        std::cout << "[Height:" << globalHeight << "]"
                                  << " [" << currentStatus << "]"
                                  << " [Rewards:" << totalBlocksFound << "]"
                                  << " [" << hashRateStr << "]"
                                  << " [GPU:" << std::fixed << std::setprecision(1) << gpuLoad << "%]"
                                  << " [Req:" << miningInfo["active_requests"].getInt<int64_t>() << "]"
                                  << std::endl;
                    }
                    lastLogTime = now;
                    lastHeartbeatTime = now;
                } else if (heartbeatElapsed >= 10) {
                    lastHeartbeatTime = now;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } catch (const std::exception& e) {
                LogError("TKNC Miner: Loop exception: %s", e.what());
                std::cerr << "[Warning] Loop error: " << e.what() << " - continuing..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            } catch (...) {
                LogError("TKNC Miner: Unknown loop exception");
                std::cerr << "[Warning] Unknown loop error - continuing..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        monitor_running.store(false);
        if (monitor_thread.joinable()) {
            monitor_thread.join();
        }

        LogInfo("TKNC Miner: Shutting down...");

        GenerateTKNC(false, 0, chainparams);

        apiServer.Stop();
        // P2P shutdown removed — P2P not initialized in miner

        std::cout << "TKNC Miner stopped" << std::endl;
        LogInfo("TKNC Miner: Shutdown complete");

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        LogError("TKNC Miner: Fatal error: %s", e.what());
        return 1;
    } catch (...) {
        std::cerr << "Fatal unknown error occurred" << std::endl;
        LogError("TKNC Miner: Fatal unknown error");
        return 1;
    }

    return 0;
}
