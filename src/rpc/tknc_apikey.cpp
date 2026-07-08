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
#include <univalue.h>
#include <util/log.h>
#include <util/moneystr.h>
#include <util/time.h>

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
        "Get local inference proxy configuration (IPv6→IPv4 proxy for IDE)",
        {},
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::BOOL, "running", "Whether the proxy is running"},
                {RPCResult::Type::NUM, "listen_port", "Local listen port (IDE connects to 127.0.0.1:<port>)"},
                {RPCResult::Type::STR, "target_ipv6", "Remote miner IPv6 address"},
                {RPCResult::Type::NUM, "target_port", "Remote miner port"},
                {RPCResult::Type::STR, "local_url", "Local URL for IDE (http://127.0.0.1:<port>/v1)"},
                {RPCResult::Type::STR, "remote_url", "Remote miner URL (http://[<ipv6>]:<port>)"},
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
            result.pushKV("target_ipv6", cfg.target_ipv6);
            result.pushKV("target_port", cfg.target_port);

            std::string local_url = "http://127.0.0.1:" + std::to_string(cfg.listen_port) + "/v1";
            result.pushKV("local_url", local_url);

            std::string remote_url = "http://[" + cfg.target_ipv6 + "]:" + std::to_string(cfg.target_port);
            result.pushKV("remote_url", remote_url);

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
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
