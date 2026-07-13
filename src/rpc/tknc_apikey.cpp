#include <apikey/api_key.h>
#include <apikey/api_key_db.h>
#include <consensus/amount.h>
#include <core_io.h>
#include <inference_gateway.h>
#include <net.h>
#include <net/message.h>
#include <netmessagemaker.h>
#include <node/context.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/protocol.h>
#include <rpc/util.h>
#include <support/events.h>
#include <univalue.h>
#include <util/log.h>
#include <util/moneystr.h>
#include <util/time.h>

#include <event2/buffer.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

#include <cstring>

// ============================================================
// Handshake verification: HTTP POST to remote miner's /v1/chat/handshake
// ============================================================

namespace {

/** Callback context for synchronous HTTP request */
struct HandshakeHttpCtx {
    bool done = false;
    int response_code = 0;
    std::string response;
};

/** libevent callback: captures response body */
void handshake_http_done(struct evhttp_request* req, void* ctx)
{
    auto* hctx = static_cast<HandshakeHttpCtx*>(ctx);
    hctx->done = true;
    if (!req) {
        LogWarning("[Handshake] HTTP request failed (connection error)\n");
        return;
    }
    hctx->response_code = evhttp_request_get_response_code(req);
    struct evbuffer* inbuf = evhttp_request_get_input_buffer(req);
    size_t len = evbuffer_get_length(inbuf);
    if (len > 0) {
        hctx->response.resize(len);
        evbuffer_copyout(inbuf, &hctx->response[0], len);
    }
}

/** Synchronous HTTP POST to a remote host:port/path.
 *  Returns the response body string (empty on failure).
 *  This is used ONLY in RPC handlers (not in the libevent event loop),
 *  so blocking is acceptable. */
std::string HttpPostToRemote(const std::string& host, int port,
                             const std::string& path, const std::string& body,
                             const std::string& auth_bearer = "",
                             int timeout_sec = 30)
{
    try {
        raii_event_base base = obtain_event_base();
        raii_evhttp_connection evcon = obtain_evhttp_connection_base(base.get(), host, static_cast<uint16_t>(port));
        evhttp_connection_set_timeout(evcon.get(), timeout_sec);

        HandshakeHttpCtx ctx;
        raii_evhttp_request req = obtain_evhttp_request(handshake_http_done, &ctx);

        struct evkeyvalq* headers = evhttp_request_get_output_headers(req.get());
        evhttp_add_header(headers, "Host", host.c_str());
        evhttp_add_header(headers, "Content-Type", "application/json");
        if (!auth_bearer.empty()) {
            evhttp_add_header(headers, "Authorization", ("Bearer " + auth_bearer).c_str());
        }

        struct evbuffer* outbuf = evhttp_request_get_output_buffer(req.get());
        evbuffer_add(outbuf, body.c_str(), body.size());

        int r = evhttp_make_request(evcon.get(), req.release(), EVHTTP_REQ_POST, path.c_str());
        if (r != 0) {
            LogWarning("[Handshake] evhttp_make_request failed for %s:%d%s\n", host.c_str(), port, path.c_str());
            return "";
        }

        event_base_dispatch(base.get());

        if (ctx.done && !ctx.response.empty()) {
            return ctx.response;
        }
    } catch (const std::exception& e) {
        LogWarning("[Handshake] HttpPostToRemote exception: %s\n", e.what());
    }
    return "";
}

/** Parse a JSON integer field from a raw JSON string (simple substring search). */
int64_t ParseJsonInt(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return -1;
    size_t colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) return -1;
    // Skip whitespace after colon
    size_t start = colon + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t' || json[start] == '\n')) start++;
    return std::strtoll(json.c_str() + start, nullptr, 10);
}

/** Parse a JSON string field from a raw JSON string (simple substring search). */
std::string ParseJsonStr(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    size_t colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) return "";
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

/** Parse a JSON boolean field from a raw JSON string. */
bool ParseJsonBool(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    size_t colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) return false;
    size_t start = colon + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) start++;
    return (json.compare(start, 4, "true") == 0);
}

} // anonymous namespace

static std::unique_ptr<CAPIKeyDB> g_apikeydb;

void InitAPIKeyDB(const fs::path& data_dir)
{
    if (g_apikeydb) return;
    g_apikeydb = std::make_unique<CAPIKeyDB>(data_dir, 1 << 20, false);
}

void ShutdownAPIKeyDB()
{
    g_apikeydb.reset();
}

CAPIKeyDB* GetAPIKeyDB()
{
    return g_apikeydb.get();
}

// Write API Key received from P2P sync (called by net_processing when APIKEYSYNC message received)
// Returns true if written successfully, false if key already exists or format invalid
bool WriteAPIKeyFromP2P(const APIKey& key_data)
{
    if (!g_apikeydb) {
        LogWarning("[APIKey-P2P] Cannot write key: database not initialized");
        return false;
    }

    // Validate key format
    if (!ValidateAPIKeyFormat(key_data.key)) {
        LogWarning("[APIKey-P2P] Cannot write key: invalid format: %s", key_data.key.substr(0, 10) + "...");
        return false;
    }

    // Check if key already exists — do NOT overwrite locally-created keys
    auto existing = g_apikeydb->ReadAPIKey(key_data.key);
    if (existing.has_value()) {
        LogInfo("[APIKey-P2P] Key already exists locally, skipping sync: %s", key_data.key.substr(0, 10) + "...");
        return true;  // Idempotent: key already present
    }

    // Write the key
    if (!g_apikeydb->WriteAPIKey(key_data.key, key_data)) {
        LogWarning("[APIKey-P2P] Failed to write key to database: %s", key_data.key.substr(0, 10) + "...");
        return false;
    }

    LogInfo("[APIKey-P2P] Synced API Key from peer: %s, balance=%lld, model=%s",
              (key_data.key.substr(0, 10) + "..."), (long long)key_data.balance, key_data.model_name);
    return true;
}

static RPCMethod tknc_createapikey()
{
    return RPCMethod{"tknc_createapikey",
        "Create a new API Key for prepaid model calls",
        {
            {"balance", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "Prepaid balance (TKNC)"},
            {"model_name", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Available model name (default: all)"},
            {"expiry_days", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Expiry days (default: 365)"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "api_key", "Generated API Key (tkn_[32 hex chars])"},
                {RPCResult::Type::STR, "miner_id", "Miner node ID"},
                {RPCResult::Type::NUM, "balance", "Prepaid balance (tokens)"},
                {RPCResult::Type::NUM, "expiry_time", "Expiry time (Unix timestamp)"},
                {RPCResult::Type::STR, "model_name", "Available model name"},
            }
        },
        RPCExamples{
            HelpExampleCli("tknc_createapikey", "100.0")
            + HelpExampleRpc("tknc_createapikey", "100.0")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!g_apikeydb) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "API Key database not initialized");
            }

            CAmount balance = AmountFromValue(request.params[0]);
            std::string model_name = request.params.size() > 1 ? request.params[1].get_str() : "all";
            int expiry_days = request.params.size() > 2 ? request.params[2].getInt<int>() : 365;

            std::string api_key = GenerateAPIKey();
            std::string miner_id = ParseAPIKeyMinerID(api_key);
            int64_t expiry_time = GetTime() + (expiry_days * 86400);

            APIKey key_data;
            key_data.key = api_key;
            key_data.miner_id = miner_id;
            key_data.balance = balance;
            key_data.locked_balance = 0;
            key_data.total_tokens_used = 0;
            key_data.expiry_time = expiry_time;
            key_data.model_name = model_name;
            key_data.payment_tx_hash = "";
            key_data.active = true;

            if (!g_apikeydb->WriteAPIKey(api_key, key_data)) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to write API Key to database");
            }

            // P2P API Key Sync: broadcast to peers so miners on other nodes can validate locally.
            int sync_count = 0;
            try {
                node::NodeContext& node_ctx = EnsureAnyNodeContext(request.context);
                CConnman& connman = EnsureConnman(node_ctx);
                // NetMsg::Make handles serialization via APIKey's SERIALIZE_METHODS
                connman.ForEachNode([&](CNode* pnode) {
                    connman.PushMessage(pnode, NetMsg::Make(std::string(MessageTypes::APIKEYSYNC), key_data));
                    sync_count++;
                });

                LogInfo("[APIKey-P2P] Broadcast APIKEYSYNC to %d peers: key=%s",
                          sync_count, api_key.substr(0, 10) + "...");
            } catch (const std::exception& e) {
                // P2P broadcast failure is non-fatal — key is still valid locally
                LogWarning("[APIKey-P2P] Failed to broadcast APIKEYSYNC: %s", e.what());
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("api_key", api_key);
            result.pushKV("miner_id", miner_id);
            result.pushKV("balance", ValueFromAmount(balance));
            result.pushKV("expiry_time", expiry_time);
            result.pushKV("model_name", model_name);
            result.pushKV("synced_peers", sync_count);

            LogInfo("RPC: tknc_createapikey - Created API Key: %s, balance: %s TKNC, synced to %d peers",
                      api_key.substr(0, 10) + "...", FormatMoney(balance), sync_count);

            return result;
        },
    };
}

static RPCMethod tknc_validateapikey()
{
    return RPCMethod{"tknc_validateapikey",
        "Validate API Key format and validity",
        {
            {"api_key", RPCArg::Type::STR, RPCArg::Optional::NO, "API Key to validate"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::BOOL, "valid", "Whether valid"},
                {RPCResult::Type::STR, "miner_id", "Miner node ID (if valid)"},
            }
        },
        RPCExamples{
            HelpExampleCli("tknc_validateapikey", "tknc_abc123...")
            + HelpExampleRpc("tknc_validateapikey", "tknc_abc123...")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
            {
            std::string api_key = request.params[0].get_str();
            bool valid = ValidateAPIKeyFormat(api_key);

            // SECURITY: Check active status — revoked keys must return valid=false
            // Anti-question test: revoked key should NOT pass validation
            if (valid && g_apikeydb) {
                auto key_opt = g_apikeydb->ReadAPIKey(api_key);
                if (key_opt.has_value() && !key_opt.value().active) {
                    valid = false;
                }
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("valid", valid);
            if (valid) {
                result.pushKV("miner_id", ParseAPIKeyMinerID(api_key));
            }

            return result;
        },
    };
}

static RPCMethod tknc_getapikeyinfo()
{
    return RPCMethod{"tknc_getapikeyinfo",
        "Get detailed information about an API Key",
        {
            {"api_key", RPCArg::Type::STR, RPCArg::Optional::NO, "API Key"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "api_key", "API Key"},
                {RPCResult::Type::STR, "miner_id", "Miner node ID"},
                {RPCResult::Type::NUM, "balance", "Current balance (tokens)"},
                {RPCResult::Type::NUM, "expiry_time", "Expiry time (Unix timestamp)"},
                {RPCResult::Type::STR, "model_name", "Available model name"},
                {RPCResult::Type::BOOL, "active", "Whether active"},
            }
        },
        RPCExamples{
            HelpExampleCli("tknc_getapikeyinfo", "tknc_abc123...")
            + HelpExampleRpc("tknc_getapikeyinfo", "tknc_abc123...")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!g_apikeydb) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "API Key database not initialized");
            }

            std::string api_key = request.params[0].get_str();

            auto key_opt = g_apikeydb->ReadAPIKey(api_key);
            if (!key_opt) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "API Key not found");
            }

            const APIKey& key_data = *key_opt;

            UniValue result(UniValue::VOBJ);
            result.pushKV("api_key", key_data.key);
            result.pushKV("miner_id", key_data.miner_id);
            result.pushKV("balance", ValueFromAmount(key_data.balance));
            result.pushKV("expiry_time", key_data.expiry_time);
            result.pushKV("model_name", key_data.model_name);
            result.pushKV("active", key_data.active);

            return result;
        },
    };
}

static RPCMethod tknc_topupapikey()
{
    return RPCMethod{"tknc_topupapikey",
        "Top up balance for an API Key",
        {
            {"api_key", RPCArg::Type::STR, RPCArg::Optional::NO, "API Key"},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "Top-up amount (TKNC)"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "api_key", "API Key"},
                {RPCResult::Type::NUM, "new_balance", "New balance (tokens)"},
                {RPCResult::Type::STR, "txid", "Transaction ID"},
            }
        },
        RPCExamples{
            HelpExampleCli("tknc_topupapikey", "tknc_abc123... 50.0")
            + HelpExampleRpc("tknc_topupapikey", "tknc_abc123..., 50.0")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!g_apikeydb) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "API Key database not initialized");
            }

            std::string api_key = request.params[0].get_str();
            CAmount amount;
            if (request.params[1].isNum()) {
                amount = static_cast<CAmount>(request.params[1].get_real() * COIN);
            } else {
                amount = AmountFromValue(request.params[1]);
            }

            if (!ValidateAPIKeyFormat(api_key)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid API Key format");
            }

            auto key_opt = g_apikeydb->ReadAPIKey(api_key);
            if (!key_opt) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "API Key not found");
            }

            APIKey key_data = *key_opt;
            
            if (amount < 0 && key_data.balance < -amount) {
                throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient balance for deduction");
            }
            
            CAmount new_balance = key_data.balance + amount;

            if (!g_apikeydb->UpdateAPIKeyBalance(api_key, new_balance)) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to update balance");
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("api_key", api_key);
            result.pushKV("new_balance", ValueFromAmount(new_balance));
            result.pushKV("txid", "");

            std::string operation = (amount >= 0) ? "top-up" : "deduction";
            LogInfo("RPC: tknc_topupapikey - API Key: %s, %s: %s TKNC, new balance: %s",
                      api_key.substr(0, 10) + "...", operation.c_str(), FormatMoney(amount), FormatMoney(new_balance));

            return result;
        },
    };
}

static RPCMethod tknc_listapikeys()
{
    return RPCMethod{"tknc_listapikeys",
        "List all API Keys",
        {},
        RPCResult{
            RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "api_key", "API Key"},
                        {RPCResult::Type::STR, "miner_id", "Miner node ID"},
                        {RPCResult::Type::NUM, "balance", "Balance (tokens)"},
                        {RPCResult::Type::NUM, "expiry_time", "Expiry time"},
                        {RPCResult::Type::BOOL, "active", "Whether active"},
                    }
                }
            }
        },
        RPCExamples{
            HelpExampleCli("tknc_listapikeys", "")
            + HelpExampleRpc("tknc_listapikeys", "")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!g_apikeydb) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "API Key database not initialized");
            }

            auto all_keys = g_apikeydb->ListAllAPIKeys();
            UniValue result(UniValue::VARR);

            for (const auto& [key, key_data] : all_keys) {
                UniValue obj(UniValue::VOBJ);
                obj.pushKV("api_key", key_data.key);
                obj.pushKV("miner_id", key_data.miner_id);
                obj.pushKV("balance", ValueFromAmount(key_data.balance));
                obj.pushKV("expiry_time", key_data.expiry_time);
                obj.pushKV("active", key_data.active);
                result.push_back(obj);
            }

            return result;
        },
    };
}

static RPCMethod tknc_revokeapikey()
{
    return RPCMethod{"tknc_revokeapikey",
        "Revoke an API Key",
        {
            {"api_key", RPCArg::Type::STR, RPCArg::Optional::NO, "API Key to revoke"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::BOOL, "success", "Whether successful"},
                {RPCResult::Type::STR, "api_key", "API Key"},
            }
        },
        RPCExamples{
            HelpExampleCli("tknc_revokeapikey", "tknc_abc123...")
            + HelpExampleRpc("tknc_revokeapikey", "tknc_abc123...")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!g_apikeydb) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "API Key database not initialized");
            }

            std::string api_key = request.params[0].get_str();

            if (!ValidateAPIKeyFormat(api_key)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid API Key format");
            }

            auto key_opt = g_apikeydb->ReadAPIKey(api_key);
            if (!key_opt) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "API Key not found");
            }

            APIKey key_data = *key_opt;
            key_data.active = false;

            if (!g_apikeydb->WriteAPIKey(api_key, key_data)) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to revoke API Key");
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("success", true);
            result.pushKV("api_key", api_key);

            LogInfo("RPC: tknc_revokeapikey - Revoked API Key: %s", api_key.substr(0, 10) + "...");

            return result;
        },
    };
}

static RPCMethod tknc_getinferproxyconfig()
{
    return RPCMethod{"tknc_getinferproxyconfig",
        "Get local inference proxy configuration (IPv6/IPv4 → localhost proxy for IDE)",
        {},
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::BOOL, "running", "Whether the proxy is running"},
                {RPCResult::Type::NUM, "listen_port", "Local listen port (IDE connects to 127.0.0.1:<port>)"},
                {RPCResult::Type::BOOL, "target_set", "Whether target miner IP has been configured"},
                {RPCResult::Type::STR, "target_ip", "Remote miner IP (IPv4 or IPv6), empty if not set"},
                {RPCResult::Type::NUM, "target_port", "Remote miner port"},
                {RPCResult::Type::STR, "local_url", "Local URL for IDE (http://127.0.0.1:<port>/v1)"},
                {RPCResult::Type::STR, "remote_url", "Remote miner URL"},
            }
        },
        RPCExamples{
            HelpExampleCli("tknc_getinferproxyconfig", "")
            + HelpExampleRpc("tknc_getinferproxyconfig", "")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
        {
            InferProxyConfig cfg = GetInferProxyConfig();

            UniValue result(UniValue::VOBJ);
            result.pushKV("running", cfg.running);
            result.pushKV("listen_port", cfg.listen_port);
            result.pushKV("target_set", cfg.target_set);
            result.pushKV("target_ip", cfg.target_ip);
            result.pushKV("target_port", cfg.target_port);

            std::string local_url = "http://127.0.0.1:" + std::to_string(cfg.listen_port) + "/v1";
            result.pushKV("local_url", local_url);

            std::string remote_url;
            if (!cfg.target_ip.empty()) {
                if (cfg.target_ip.find(':') != std::string::npos) {
                    remote_url = "http://[" + cfg.target_ip + "]:" + std::to_string(cfg.target_port);
                } else {
                    remote_url = "http://" + cfg.target_ip + ":" + std::to_string(cfg.target_port);
                }
            }
            result.pushKV("remote_url", remote_url);

            return result;
        },
    };
}

static RPCMethod tknc_setinferproxytarget()
{
    return RPCMethod{"tknc_setinferproxytarget",
        "Set inference proxy target miner IP with handshake verification.\n"
        "The proxy on 127.0.0.1:<port> will forward requests to this miner.\n"
        "User obtains miner IP and API key from the web page, then runs this command locally.\n"
        "Node validates the API key, performs handshake with the miner (verifies token counting\n"
        "and exchange ratio), sets the proxy target, and returns IDE configuration info.\n"
        "\n"
        "Handshake verifies:\n"
        "  1. Miner is reachable and responding\n"
        "  2. Token counting is sane (no cheating)\n"
        "  3. Exchange ratio matches what the miner set on the web (if expected_price is provided)",
        {
            {"miner_ip", RPCArg::Type::STR, RPCArg::Optional::NO, "Remote miner public IP (IPv4 or IPv6, no brackets)"},
            {"api_key", RPCArg::Type::STR, RPCArg::Optional::NO, "API key obtained from web page (validated locally)"},
            {"model_name", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Model name (optional, for display in IDE config info)"},
            {"target_port", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Remote miner port (default: 9313)"},
            {"expected_price", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Expected price_per_1m_tknc from web page (optional, for ratio verification). If provided, handshake will warn if the miner's actual price differs."},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::BOOL, "success", "Whether the target was set successfully"},
                {RPCResult::Type::STR, "local_url", "Local URL for IDE configuration"},
                {RPCResult::Type::STR, "api_key", "API key for IDE"},
                {RPCResult::Type::STR, "model", "Model name"},
                {RPCResult::Type::STR, "target_ip", "Remote miner IP"},
                {RPCResult::Type::NUM, "target_port", "Remote miner port"},
                {RPCResult::Type::OBJ, "handshake", "Handshake verification result",
                {
                    {RPCResult::Type::BOOL, "performed", "Whether handshake was performed"},
                    {RPCResult::Type::BOOL, "passed", "Whether token verification passed"},
                    {RPCResult::Type::NUM, "verified_price_per_1m_tknc", "Miner's actual price (from LevelDB)"},
                    {RPCResult::Type::STR, "exchange_rate_display", "Human-readable exchange rate"},
                    {RPCResult::Type::BOOL, "price_matches", "Whether verified price matches expected price (if expected_price was provided)"},
                    {RPCResult::Type::STR, "warning", "Warning message if any (empty if none)"},
                }},
                {RPCResult::Type::STR, "instructions", "Instructions for IDE setup"},
            }
        },
        RPCExamples{
            HelpExampleCli("tknc_setinferproxytarget", "\"2408:8244:bb00:b7f:ec41:d2bf:8f73:c207\" \"tknc_963d11048b75192865d3baaf0d01132b\" \"Qwen2.5-7B-Instruct-Q5_K_M\"")
            + HelpExampleCli("tknc_setinferproxytarget", "\"203.0.113.50\" \"tknc_963d11048b75192865d3baaf0d01132b\" \"\" 9313 10")
            + HelpExampleCli("tknc_setinferproxytarget", "\"203.0.113.50\" \"tknc_963d11048b75192865d3baaf0d01132b\"")
            + HelpExampleRpc("tknc_setinferproxytarget", "\"2408:8244:bb00:b7f:ec41:d2bf:8f73:c207\", \"tknc_963d11048b75192865d3baaf0d01132b\"")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string miner_ip = request.params[0].get_str();
            std::string api_key = request.params[1].get_str();
            std::string model_name = request.params.size() > 2 ? request.params[2].get_str() : "";
            int target_port = request.params.size() > 3 ? request.params[3].getInt<int>() : 9313;
            // Optional: expected price from web (for ratio verification)
            bool has_expected_price = request.params.size() > 4 && !request.params[4].isNull();
            int64_t expected_price = has_expected_price ? request.params[4].getInt<int64_t>() : -1;

            // 1. Validate API key format
            if (!ValidateAPIKeyFormat(api_key)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid API key format");
            }

            // 2. Validate API key exists in local LevelDB
            if (g_apikeydb) {
                auto key_opt = g_apikeydb->ReadAPIKey(api_key);
                if (!key_opt.has_value()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "API key not found in local database. Make sure the node is synced with the network.");
                }
                if (!key_opt.value().active) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "API key has been revoked");
                }
                // Use model from API key record if not provided
                if (model_name.empty() && !key_opt.value().model_name.empty()) {
                    model_name = key_opt.value().model_name;
                }
            } else {
                LogWarning("[RPC] API key database not initialized, skipping key validation");
            }

            // 3. Handshake verification: call miner's /v1/chat/handshake endpoint
            //    This verifies: (a) miner is reachable, (b) token counting is sane,
            //    (c) exchange ratio matches web price (if expected_price provided).
            //    This is a blocking HTTP call in an RPC handler — acceptable because
            //    RPC handlers are synchronous. It does NOT affect the inference
            //    event loop, streaming, or proxy.
            UniValue handshake_result(UniValue::VOBJ);
            handshake_result.pushKV("performed", false);

            LogInfo("[Handshake] Initiating handshake with miner [%s]:%d for api_key=%s...",
                     miner_ip.c_str(), target_port, api_key.substr(0, 8).c_str());

            // Build handshake request body
            std::string hs_body = "{\"api_key\": \"" + api_key + "\"}";

            // POST to miner's /v1/chat/handshake (30s timeout — micro-inference "Hi" should complete in seconds)
            std::string hs_response = HttpPostToRemote(miner_ip, target_port,
                                                        "/v1/chat/handshake", hs_body,
                                                        api_key, 30);

            if (!hs_response.empty()) {
                handshake_result.pushKV("performed", true);

                // Parse handshake response fields
                bool can_proceed = ParseJsonBool(hs_response, "can_proceed");
                int64_t verified_price = ParseJsonInt(hs_response, "verified_price_per_1m_tknc");
                std::string exchange_display = ParseJsonStr(hs_response, "exchange_rate_display");
                std::string token_verdict = ParseJsonStr(hs_response, "token_verification");

                handshake_result.pushKV("passed", can_proceed);
                handshake_result.pushKV("verified_price_per_1m_tknc", verified_price);
                handshake_result.pushKV("exchange_rate_display", exchange_display);
                handshake_result.pushKV("token_verification", token_verdict);

                // Price comparison (if expected_price was provided)
                std::string warning;
                bool price_matches = true;
                if (has_expected_price && expected_price > 0 && verified_price > 0) {
                    if (verified_price != expected_price) {
                        price_matches = false;
                        warning = "Price mismatch! Web shows " + std::to_string(expected_price)
                                + " TKNC/1M tokens, but miner reports " + std::to_string(verified_price)
                                + " TKNC/1M tokens. Proceed with caution.";
                        LogWarning("[Handshake] %s", warning.c_str());
                    } else {
                        LogInfo("[Handshake] Price verified: %lld TKNC/1M tokens matches web display", verified_price);
                    }
                }
                handshake_result.pushKV("price_matches", price_matches);
                handshake_result.pushKV("warning", warning);

                if (!can_proceed) {
                    LogWarning("[Handshake] Token verification FAILED — anomaly detected. Proxy target set but inference may be unreliable.");
                }
            } else {
                // Handshake failed — miner unreachable or timed out
                handshake_result.pushKV("passed", false);
                handshake_result.pushKV("verified_price_per_1m_tknc", -1);
                handshake_result.pushKV("exchange_rate_display", "unavailable");
                handshake_result.pushKV("price_matches", false);
                handshake_result.pushKV("warning", "Handshake failed: miner unreachable or timed out. Proxy target set but connection quality is unverified.");
                LogWarning("[Handshake] Failed to reach miner [%s]:%d — proxy target set without verification", miner_ip.c_str(), target_port);
            }

            // 4. Set proxy target (always set, even if handshake fails — user can decide)
            if (!SetInferProxyTarget(miner_ip, target_port)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Failed to set proxy target: invalid IP or port");
            }

            // 5. Get proxy config for local_url
            InferProxyConfig cfg = GetInferProxyConfig();
            std::string local_url = "http://127.0.0.1:" + std::to_string(cfg.listen_port) + "/v1";

            // 6. Return IDE configuration info with handshake results
            UniValue result(UniValue::VOBJ);
            result.pushKV("success", true);
            result.pushKV("local_url", local_url);
            result.pushKV("api_key", api_key);
            result.pushKV("model", model_name);
            result.pushKV("target_ip", miner_ip);
            result.pushKV("target_port", target_port);
            result.pushKV("handshake", handshake_result);
            result.pushKV("instructions",
                "Configure your IDE with:\n"
                "  Base URL: " + local_url + "\n"
                "  API Key: " + api_key + "\n"
                "  Model: " + (model_name.empty() ? "(use any)" : model_name) + "\n"
                "The proxy will forward all requests to [" + miner_ip + "]:" + std::to_string(target_port));

            return result;
        },
    };
}

void RegisterTKNCAPIKeyRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"tknc", &tknc_createapikey},
        {"tknc", &tknc_validateapikey},
        {"tknc", &tknc_getapikeyinfo},
        {"tknc", &tknc_topupapikey},
        {"tknc", &tknc_listapikeys},
        {"tknc", &tknc_revokeapikey},
        {"tknc", &tknc_getinferproxyconfig},
        {"tknc", &tknc_setinferproxytarget},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
