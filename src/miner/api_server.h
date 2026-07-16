

#ifndef TKN_MINER_API_SERVER_H
#define TKN_MINER_API_SERVER_H

#include <model/runtime.h>
#include <model/llm_inference.h>
#include <apikey/api_key.h>

#include <net/api_protocol.h>
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#include <chrono>
#include <future>
#include <queue>
#include <functional>

struct event_base;
struct evhttp;

struct LiveMinerInfo {
    std::string miner_id;
    std::string model_name;
    std::string model_path;

    std::string status;
    double hashrate;
    int64_t last_heartbeat;

    std::string gpu_name;
    int64_t gpu_vram_total_mb;
    int64_t gpu_vram_used_mb;
    double gpu_utilization;

    int64_t total_blocks_found;
    int64_t total_inference_requests;
    int64_t total_earned;
    int64_t current_block_height;

    std::string ip_address;
    int p2p_port;
    int api_port;

    int64_t registration_time;

    std::string description;
    int64_t tokens_per_tknc;
    std::string wallet_address;

    LiveMinerInfo() : hashrate(0), last_heartbeat(0), gpu_vram_total_mb(0),
                       gpu_vram_used_mb(0), gpu_utilization(0),
                       total_blocks_found(0), total_inference_requests(0),
                       total_earned(0), current_block_height(0),
                       p2p_port(0), api_port(0),
    tokens_per_tknc(0) {}
};

struct UsageRecord {
    std::string user_api_key;
    std::string miner_id;
    int64_t start_time;
    int64_t end_time;
    CAmount amount_paid;
    std::string tx_hash;

    UsageRecord() : start_time(0), end_time(0), amount_paid(0) {}
};

struct MinerIdentity {
    std::string miner_id;
    std::string public_key;
    std::string wallet_address;
    int64_t registered_at;

    MinerIdentity() : registered_at(0) {}
};

struct ReviewRequest {
    std::string request_id;
    std::string miner_id;
    std::string user_api_key;
    CAmount locked_amount;
    std::string status;
    int64_t created_at;
    int64_t expires_at;

    std::string review_text;
    int rating;
    bool liked;
    bool submitted;

    ReviewRequest() : locked_amount(0), created_at(0), expires_at(0),
                       rating(0), liked(false), submitted(false) {}
};

struct StoredReview {
    std::string review_id;
    std::string miner_id;
    std::string reviewer_api_key;
    int rating;
    std::string text;
    int64_t timestamp;
    std::string type;
    int likes;
    bool is_miner_initiated;
    int64_t usage_duration_seconds;
    std::string request_id;

    StoredReview() : rating(0), timestamp(0), likes(0),
                     is_miner_initiated(false),
                     usage_duration_seconds(0) {}
};

struct AuthChallenge {
    std::string challenge_id;
    std::string miner_id;
    std::string challenge_text;
    int64_t created_at;
    int64_t expires_at;
    bool used;

    AuthChallenge() : created_at(0), expires_at(0), used(false) {}
};

class APIServer {
private:
    ModelRuntime* model_runtime;
    std::unique_ptr<LLMInference> llm_engine;
    std::string data_dir;
    std::string model_path_;
    int port;
    std::string bind_address_;
    std::string wallet_address_;
    std::string manual_public_ip_;
    std::atomic<bool> running;
    std::thread server_thread;
    std::mutex db_mutex;
    struct event_base* event_base_ptr;
    struct evhttp* http_ptr;

    std::map<std::string, LiveMinerInfo> live_miners_;
    std::mutex miners_mutex_;
    std::thread cleanup_thread_;
    std::atomic<bool> cleanup_running_;

    std::map<std::string, UsageRecord> usage_records_;
    std::mutex usage_mutex_;
    std::map<std::string, MinerIdentity> miner_identities_;
    std::mutex identity_mutex_;

    std::map<std::string, ReviewRequest> pending_reviews_;
    std::mutex reviews_mutex_;

    std::map<std::string, std::vector<StoredReview>> stored_reviews_;
    std::mutex stored_reviews_mutex_;

    std::map<std::string, AuthChallenge> auth_challenges_;
    std::mutex challenges_mutex_;

    std::map<std::string, std::map<std::string, bool>> review_likes_;
    std::mutex review_likes_mutex_;

    std::map<std::string, std::string> public_ip_nonces_;
    std::mutex public_ip_nonces_mutex_;

    std::string web_server_url_;
    std::string self_miner_id_;
    int n_ctx_configured = 131072;  // Context window size, configurable via -n_ctx. Default 128K. Real limit is GPU VRAM.
int64_t tokens_per_tknc_configured = 0;  // Tokens per 1 TKNC, configurable via -token. 0 = not set (miner refuses to start).
    // heartbeat_to_web_thread_ and heartbeat_to_web_running_ removed (dead code cleanup 2026-06-28)

    struct AsyncChatTask {
        struct evhttp_request* req;
        std::string prompt;
        std::string api_key;
        bool stream_mode;
        uint64_t request_id;
        int max_tokens;  // -1 = unlimited, 0 = use default, >0 = specific limit

        AsyncChatTask() : req(nullptr), stream_mode(false), request_id(0), max_tokens(0) {}
    };

    std::vector<std::thread> worker_threads_;
    std::queue<AsyncChatTask> task_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::atomic<bool> workers_running_;
    // No artificial concurrency limits. A data-center miner (e.g. Tencent-level)
    // with multiple GPUs can handle many concurrent requests. The real limit is
    // the miner's hardware, not this bridge.
    const int MAX_WORKER_THREADS = 16;
    const int MAX_QUEUE_SIZE = 1000;

public:
    APIServer(ModelRuntime* runtime, const std::string& datadir, const std::string& model_path, int api_port = 9332,
              const std::string& rpc_host = "127.0.0.1", int rpc_port = 9331,
              const std::string& rpc_user = "", const std::string& rpc_password = "",
              const std::string& bind_address = "::");  // Default IPv6 dual-stack for IPv4+IPv6 accessibility; use API_BIND_ADDRESS env to override
    ~APIServer();

    bool Start();
    void Stop();
    bool IsRunning() const { return running.load(); }

    void SetWalletAddress(const std::string& addr) { wallet_address_ = addr; }
    void SetPublicIP(const std::string& ip) { manual_public_ip_ = ip; }
    void SetContextLength(int n_ctx) { n_ctx_configured = n_ctx; }
void SetTokensPerTknc(int64_t rate) { tokens_per_tknc_configured = rate; }
    void SetWebServerUrl(const std::string& url) { web_server_url_ = url; }
    std::string GetWalletAddress() const { return wallet_address_; }

    void SaveMinerData();
    void LoadMinerData();

    APIResponse HandleAPIRequest(const APIRequest& request);  // REMOVED: dead code (fake output), see cpp for details

    std::string CallNodeRPC(const std::string& method, const std::string& params = "[]");

    void ServerLoop();  // Public: start libevent event loop (called from main thread)

private:
    std::string rpc_host_;
    int rpc_port_;
    std::string rpc_user_;
    std::string rpc_password_;

    bool VerifyOnChainPayment(const std::string& tx_hash, const std::string& wallet_address, int64_t min_amount, int64_t& received_amount, const std::string& block_hash = "");
    bool VerifyWalletSignature(const std::string& wallet, const std::string& signature, const std::string& message);

    LLMInference::GenerationResult CallLLM(const std::string& prompt);  // Returns full GenerationResult for precise token counting
    void HandleChatRequest(struct evhttp_request* req);
    void HandleWSUpgrade(struct evhttp_request* req);

    void HandleMinersListRequest(struct evhttp_request* req);
    void HandleMinerDetailRequest(struct evhttp_request* req);
    void HandleSubmitReviewRequest(struct evhttp_request* req);
    void HandleReviewLikeRequest(struct evhttp_request* req);
    void HandleRequestReviewRequest(struct evhttp_request* req);
    void HandleUserInitiatedReviewRequest(struct evhttp_request* req);
    void HandleNetworkStatsRequest(struct evhttp_request* req);
    void HandleMinerRegisterRequest(struct evhttp_request* req);
    void HandleMinerHeartbeatRequest(struct evhttp_request* req);

    void HandleVerifyUsageRequest(struct evhttp_request* req);
    void HandleVerifyMinerSignatureRequest(struct evhttp_request* req);
    void HandleRegisterMinerIdentityRequest(struct evhttp_request* req);
    void HandleSystemResourceRequest(struct evhttp_request* req);
    void HandleChallengeRequest(struct evhttp_request* req);
    void HandleEditProfileRequest(struct evhttp_request* req);
    void HandlePublicIPNonceRequest(struct evhttp_request* req);
    void HandleSetPublicIPRequest(struct evhttp_request* req);
    void HandleMinerReviewsRequest(struct evhttp_request* req);

    static void AddCORSHeaders(struct evhttp_request* req);

    void RegisterExplorerEndpoints();

    bool RegisterMiner(const LiveMinerInfo& info);
    bool UpdateMinerHeartbeat(const std::string& miner_id, const LiveMinerInfo& update);
    void CleanupOfflineMiners();
    void CleanupLoop();
    void SaveConfig();
    void LoadConfig();

    void StartWorkerThreads();
    void StopWorkerThreads();
    void WorkerThreadFunc(int worker_id);
    void ProcessAsyncChatTask(AsyncChatTask& task);

    static void StaticChatHandler(struct evhttp_request* req, void* arg);
    static void StaticWSHandler(struct evhttp_request* req, void* arg);
    static void StaticMinersListHandler(struct evhttp_request* req, void* arg);
    static void StaticMinerDetailHandler(struct evhttp_request* req, void* arg);
    static void StaticSubmitReviewHandler(struct evhttp_request* req, void* arg);
    static void StaticLikeReviewHandler(struct evhttp_request* req, void* arg);
    static void StaticRequestReviewHandler(struct evhttp_request* req, void* arg);
    static void StaticUserInitiatedReviewHandler(struct evhttp_request* req, void* arg);
    static void StaticNetworkStatsHandler(struct evhttp_request* req, void* arg);
    static void StaticMinerRegisterHandler(struct evhttp_request* req, void* arg);
    static void StaticMinerHeartbeatHandler(struct evhttp_request* req, void* arg);

    static void StaticVerifyUsageHandler(struct evhttp_request* req, void* arg);
    static void StaticVerifyMinerSignatureHandler(struct evhttp_request* req, void* arg);
    static void StaticRegisterMinerIdentityHandler(struct evhttp_request* req, void* arg);
    static void StaticSystemResourceHandler(struct evhttp_request* req, void* arg);
    static void StaticChallengeHandler(struct evhttp_request* req, void* arg);
    static void StaticEditProfileHandler(struct evhttp_request* req, void* arg);
    static void StaticPublicIPNonceHandler(struct evhttp_request* req, void* arg);
    static void StaticSetPublicIPHandler(struct evhttp_request* req, void* arg);
    static void StaticMinerReviewsHandler(struct evhttp_request* req, void* arg);

    bool HasUserUsedMiner(const std::string& api_key, const std::string& miner_id);
    bool ToggleReviewLike(const std::string& miner_id, const std::string& review_id, const std::string& api_key);
    int GetReviewLikeCount(const std::string& miner_id, const std::string& review_id);
    bool TransferTokensFromMinerToUser(const std::string& miner_id, const std::string& user_api_key, CAmount amount);
    std::string GetQueryParam(struct evhttp_request* req, const std::string& param);
};

#endif
