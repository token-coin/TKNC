#include <escrow/escrow.h>
#include <escrow/escrow_db.h>
#include <billing/billing_receipt.h>
#include <rpc/escrow_rpc.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <rpc/rawtransaction_util.h>
#include <wallet/wallet.h>
#include <wallet/context.h>
#include <wallet/spend.h>
#include <wallet/coincontrol.h>
#include <wallet/rpc/util.h>
#include <key_io.h>
#include <univalue.h>
#include <util/log.h>
#include <util/moneystr.h>
#include <util/time.h>
#include <util/result.h>
#include <core_io.h>
#include <primitives/transaction.h>
#include <hash.h>

// Persistent spending limit database (replaces in-memory map)
static std::unique_ptr<CSpendingLimitDB> g_spending_limit_db;
static std::mutex g_escrow_mutex;

// Initialize spending limit DB (called from init.cpp)
void InitEscrowDB(const fs::path& path)
{
    g_spending_limit_db = std::make_unique<CSpendingLimitDB>(path, 1 << 20, false);
    LogInfo("InitEscrowDB: Spending limit database initialized at %s", PathToString(path));
}

bool HasEscrowDB()
{
    return g_spending_limit_db != nullptr;
}

// Helper: find spending limit by api_key (exported via escrow_rpc.h)
std::optional<SpendingLimit> FindSpendingLimitByAPIKey(const std::string& api_key)
{
    std::lock_guard<std::mutex> lock(g_escrow_mutex);
    return g_spending_limit_db->FindSpendingLimitByAPIKey(api_key);
}

// === Wallet access for pay-as-you-go inference billing ===
// Replaces the old PaymentWallet singleton. Any loaded wallet owning the
// escrow.user_wallet address can be used for inference payment.
static wallet::WalletContext* g_wallet_ctx = nullptr;

void SetWalletContext(wallet::WalletContext* ctx)
{
    g_wallet_ctx = ctx;
    LogInfo("SetWalletContext: wallet context registered for spending limit billing");
}

std::shared_ptr<wallet::CWallet> FindWalletByAddress(const std::string& address)
{
    if (!g_wallet_ctx) return nullptr;
    CTxDestination dest = DecodeDestination(address);
    if (dest.index() == 0) return nullptr;
    LOCK(g_wallet_ctx->wallets_mutex);
    for (const auto& w : g_wallet_ctx->wallets) {
        if (w && w->IsMine(dest)) return w;
    }
    return nullptr;
}

bool TransferFromWallet(std::shared_ptr<wallet::CWallet> pwallet,
                        const std::string& to_address, CAmount amount, std::string& txid)
{
    if (!pwallet) { LogError("[EscrowBilling] Wallet null"); return false; }
    if (pwallet->IsLocked()) { LogError("[EscrowBilling] Wallet '%s' locked", pwallet->GetName()); return false; }
    CTxDestination dest = DecodeDestination(to_address);
    if (dest.index() == 0) { LogError("[EscrowBilling] Invalid address %s", to_address.c_str()); return false; }
    wallet::CRecipient recipient{dest, amount, /*subtract_fee_from_amount=*/false};
    std::vector<wallet::CRecipient> recipients{recipient};
    wallet::CCoinControl coin_control;
    coin_control.m_allow_other_inputs = true;
    auto tx_result = wallet::CreateTransaction(*pwallet, recipients, std::nullopt, coin_control, /*sign=*/true);
    if (!tx_result) {
        LogError("[EscrowBilling] CreateTransaction failed: %s", util::ErrorString(tx_result).original.c_str());
        return false;
    }
    pwallet->CommitTransaction(tx_result->tx, {}, {});
    txid = tx_result->tx->GetHash().GetHex();
    LogInfo("[EscrowBilling] Transfer %s TKNC from '%s' to %s — txid=%s",
            FormatMoney(amount).c_str(), pwallet->GetName(), to_address.c_str(), txid.c_str());
    return true;
}

static RPCMethod createescrow()
{
    return RPCMethod{"createescrow",
        "Create a new Escrow UTXO for on-chain locked payment.\n"
        "Locks user funds on-chain for LLM inference quota.\n",
        {
            {"user_wallet", RPCArg::Type::STR, RPCArg::Optional::NO, "User wallet address (payer)"},
            {"miner_wallet", RPCArg::Type::STR, RPCArg::Optional::NO, "Miner wallet address (payee)"},
            {"amount_tknc", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "Amount of TKNC to lock"},
            {"rate_tokens_per_tknc", RPCArg::Type::NUM, RPCArg::Optional::NO, "Exchange rate: tokens per 1 TKNC"},
            {"model_name", RPCArg::Type::STR, RPCArg::Optional::NO, "Target model name"},
            {"model_hash", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "SHA256 hash of model GGUF file"},
            {"api_key", RPCArg::Type::STR, RPCArg::Optional::NO, "Associated API Key"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "escrow_id", "Unique escrow identifier"},
                {RPCResult::Type::STR, "snapshot_hash", "SHA256 of pricing snapshot"},
                {RPCResult::Type::NUM, "total_tknc", "Total TKNC locked"},
                {RPCResult::Type::NUM, "quota_tokens", "Total token quota"},
                {RPCResult::Type::STR, "state", "Escrow state"},
            }
        },
        RPCExamples{
            HelpExampleCli("createescrow",
                "\"token1u...\" \"token1m...\" 100.0 1000000 \"qwen2.5-0.5b-instruct\" \"abc123...\" "
                "\"tknc_xxx...\"")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string user_wallet = request.params[0].get_str();
            std::string miner_wallet = request.params[1].get_str();
            CAmount amount = AmountFromValue(request.params[2]);
            int64_t rate_tokens_per_tknc = request.params[3].getInt<int64_t>();
            std::string model_name = request.params[4].get_str();
            std::string model_hash = request.params.size() > 5 ? request.params[5].get_str() : "";
            std::string api_key = request.params.size() > 6 ? request.params[6].get_str() : "";

            if (amount <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Amount must be positive");
            }
            if (rate_tokens_per_tknc <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Rate must be positive");
            }

            // Build pricing snapshot
            PricingSnapshot snapshot;
            snapshot.rate_tokens_per_tknc = rate_tokens_per_tknc;
            // Use high-precision: rate_tknc_per_token = COIN / rate_tokens_per_tknc
            // But for amounts < 1 COIN, use proportional calculation
            if (rate_tokens_per_tknc >= COIN) {
                snapshot.rate_tknc_per_token = 1;
            } else {
                snapshot.rate_tknc_per_token = COIN / rate_tokens_per_tknc;
            }
            // Use proportional calculation to avoid truncation for small amounts
            snapshot.quota_tokens = (amount * rate_tokens_per_tknc) / COIN;
            snapshot.total_tknc_paid = amount;
            snapshot.miner_wallet = miner_wallet;
            snapshot.user_wallet = user_wallet;
            snapshot.model_name = model_name;
            snapshot.model_hash = model_hash;
            snapshot.timestamp = GetTime();
            snapshot.version = 1;

            std::string snapshot_hash = snapshot.ComputeHash();

            // Create spending limit entry
            SpendingLimit escrow;
            escrow.escrow_id = GenerateSpendingLimitID(api_key);
            escrow.user_wallet = user_wallet;
            escrow.miner_wallet = miner_wallet;
            escrow.model_name = model_name;
            escrow.model_hash = model_hash;
            escrow.snapshot_hash = snapshot_hash;
            escrow.api_key = api_key;
            escrow.total_tknc = amount;
            escrow.consumed_tknc = 0;
            escrow.spending_limit = amount;
            escrow.quota_tokens = snapshot.quota_tokens;
            escrow.used_tokens = 0;
            escrow.rate_tokens_per_tknc = rate_tokens_per_tknc;
            escrow.rate_tknc_per_token = snapshot.rate_tknc_per_token;
            escrow.created_at = GetTime();
            escrow.expires_at = GetTime() + (90 * 86400); // 90 days
            escrow.last_activity = GetTime();
            escrow.state = SpendingLimit::State::CREATED;

            {
                std::lock_guard<std::mutex> lock(g_escrow_mutex);
                g_spending_limit_db->WriteSpendingLimit(escrow.escrow_id, escrow);
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("escrow_id", escrow.escrow_id);
            result.pushKV("snapshot_hash", snapshot_hash);
            result.pushKV("total_tknc", ValueFromAmount(escrow.total_tknc));
            result.pushKV("quota_tokens", escrow.quota_tokens);
            result.pushKV("state", SpendingLimitStateToString(escrow.state));

            LogInfo("RPC: createescrow - Created spending limit %s, %s TKNC, %lld tokens, hash=%s",
                     escrow.escrow_id, FormatMoney(amount),
                     escrow.quota_tokens, snapshot_hash);

            return result;
        },
    };
}

static RPCMethod getescrowinfo()
{
    return RPCMethod{"getescrowinfo",
        "Get detailed information about an escrow.",
        {
            {"escrow_id", RPCArg::Type::STR, RPCArg::Optional::NO, "Escrow ID"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "escrow_id", ""},
                {RPCResult::Type::STR, "user_wallet", ""},
                {RPCResult::Type::STR, "miner_wallet", ""},
                {RPCResult::Type::STR, "model_name", ""},
                {RPCResult::Type::STR, "model_hash", ""},
                {RPCResult::Type::STR, "snapshot_hash", ""},
                {RPCResult::Type::STR, "api_key", ""},
                {RPCResult::Type::NUM, "total_tknc", ""},
                {RPCResult::Type::NUM, "consumed_tknc", ""},
                {RPCResult::Type::NUM, "spending_limit", ""},
                {RPCResult::Type::NUM, "quota_tokens", ""},
                {RPCResult::Type::NUM, "used_tokens", ""},
                {RPCResult::Type::NUM, "remaining_tokens", ""},
                {RPCResult::Type::NUM, "rate_tokens_per_tknc", ""},
                {RPCResult::Type::STR, "state", ""},
                {RPCResult::Type::NUM, "created_at", ""},
                {RPCResult::Type::NUM, "expires_at", ""},
            }
        },
        RPCExamples{
            HelpExampleCli("getescrowinfo", "\"escrow_xxx...\"")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string escrow_id = request.params[0].get_str();

            std::lock_guard<std::mutex> lock(g_escrow_mutex);
            auto escrow_opt = g_spending_limit_db->ReadSpendingLimit(escrow_id);
            if (!escrow_opt) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Escrow not found");
            }

            const SpendingLimit& escrow = *escrow_opt;

            UniValue result(UniValue::VOBJ);
            result.pushKV("escrow_id", escrow.escrow_id);
            result.pushKV("user_wallet", escrow.user_wallet);
            result.pushKV("miner_wallet", escrow.miner_wallet);
            result.pushKV("model_name", escrow.model_name);
            result.pushKV("model_hash", escrow.model_hash);
            result.pushKV("snapshot_hash", escrow.snapshot_hash);
            result.pushKV("api_key", escrow.api_key);
            result.pushKV("total_tknc", ValueFromAmount(escrow.total_tknc));
            result.pushKV("consumed_tknc", ValueFromAmount(escrow.consumed_tknc));
            result.pushKV("spending_limit", ValueFromAmount(escrow.spending_limit));
            result.pushKV("quota_tokens", escrow.quota_tokens);
            result.pushKV("used_tokens", escrow.used_tokens);
            result.pushKV("remaining_tokens", escrow.GetRemainingTokens());
            result.pushKV("rate_tokens_per_tknc", escrow.rate_tokens_per_tknc);
            result.pushKV("state", SpendingLimitStateToString(escrow.state));
            result.pushKV("created_at", escrow.created_at);
            result.pushKV("expires_at", escrow.expires_at);

            return result;
        },
    };
}

static RPCMethod listescrows()
{
    return RPCMethod{"listescrows",
        "List all escrows tracked by this node.",
        {},
        RPCResult{
            RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "escrow_id", ""},
                        {RPCResult::Type::STR, "user_wallet", ""},
                        {RPCResult::Type::STR, "miner_wallet", ""},
                        {RPCResult::Type::STR, "state", ""},
                        {RPCResult::Type::NUM, "total_tknc", ""},
                        {RPCResult::Type::NUM, "spending_limit", ""},
                        {RPCResult::Type::NUM, "remaining_tokens", ""},
                    }
                }
            }
        },
        RPCExamples{
            HelpExampleCli("listescrows", "")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
        {
            std::lock_guard<std::mutex> lock(g_escrow_mutex);
            UniValue result(UniValue::VARR);

            auto escrows = g_spending_limit_db->GetAllSpendingLimits();
            for (const auto& escrow : escrows) {
                UniValue obj(UniValue::VOBJ);
                obj.pushKV("escrow_id", escrow.escrow_id);
                obj.pushKV("user_wallet", escrow.user_wallet);
                obj.pushKV("miner_wallet", escrow.miner_wallet);
                obj.pushKV("state", SpendingLimitStateToString(escrow.state));
                obj.pushKV("total_tknc", ValueFromAmount(escrow.total_tknc));
                obj.pushKV("spending_limit", ValueFromAmount(escrow.spending_limit));
                obj.pushKV("remaining_tokens", escrow.GetRemainingTokens());
                result.push_back(obj);
            }

            return result;
        },
    };
}

static RPCMethod commitpricing()
{
    return RPCMethod{"commitpricing",
        "Commit pricing snapshot hash to blockchain via OP_RETURN.\n"
        "Creates a raw transaction with OP_RETURN output containing the\n"
        "SHA256 hash of the pricing snapshot. This makes the exchange rate\n"
        "tamper-proof and verifiable on-chain.\n"
        "\nThe returned hex must be signed and broadcast separately.\n",
        {
            {"rate_tokens_per_tknc", RPCArg::Type::NUM, RPCArg::Optional::NO, "Exchange rate: tokens per 1 TKNC"},
            {"miner_wallet", RPCArg::Type::STR, RPCArg::Optional::NO, "Miner wallet address (payee)"},
            {"user_wallet", RPCArg::Type::STR, RPCArg::Optional::NO, "User wallet address (payer)"},
            {"amount_tknc", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "Amount of TKNC to lock in escrow"},
            {"model_name", RPCArg::Type::STR, RPCArg::Optional::NO, "Target model name"},
            {"model_hash", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "SHA256 hash of model GGUF file"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "snapshot_hash", "SHA256 hash of the pricing snapshot"},
                {RPCResult::Type::STR, "op_return_hex", "OP_RETURN hex data (for tx creation)"},
                {RPCResult::Type::STR, "hex", "Raw transaction hex (unsigned)"},
                {RPCResult::Type::STR, "message", "Instructions for signing and broadcasting"},
            }
        },
        RPCExamples{
            HelpExampleCli("commitpricing",
                "1000000 \"token1m...\" \"token1u...\" 100.0 \"qwen2.5-0.5b-instruct\" \"sha256:abc123...\"")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
        {
            int64_t rate_tokens_per_tknc = request.params[0].getInt<int64_t>();
            std::string miner_wallet = request.params[1].get_str();
            std::string user_wallet = request.params[2].get_str();
            CAmount amount = AmountFromValue(request.params[3]);
            std::string model_name = request.params[4].get_str();
            std::string model_hash = request.params.size() > 5 ? request.params[5].get_str() : "";

            if (rate_tokens_per_tknc <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Rate must be positive");
            }
            if (amount <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Amount must be positive");
            }
            if (miner_wallet.empty() || user_wallet.empty()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Wallet addresses must not be empty");
            }
            if (model_name.empty()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Model name must not be empty");
            }

            // Build pricing snapshot
            PricingSnapshot snapshot;
            snapshot.rate_tokens_per_tknc = rate_tokens_per_tknc;
            // Use high-precision: rate_tknc_per_token = COIN / rate_tokens_per_tknc
            // But for amounts < 1 COIN, use proportional calculation
            if (rate_tokens_per_tknc >= COIN) {
                snapshot.rate_tknc_per_token = 1;
            } else {
                snapshot.rate_tknc_per_token = COIN / rate_tokens_per_tknc;
            }
            // Use proportional calculation to avoid truncation for small amounts
            snapshot.quota_tokens = (amount * rate_tokens_per_tknc) / COIN;
            snapshot.total_tknc_paid = amount;
            snapshot.miner_wallet = miner_wallet;
            snapshot.user_wallet = user_wallet;
            snapshot.model_name = model_name;
            snapshot.model_hash = model_hash;
            snapshot.timestamp = GetTime();
            snapshot.version = 1;

            if (!snapshot.Validate()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Pricing snapshot validation failed. Check rate consistency.");
            }

            std::string snapshot_hash = snapshot.ComputeHash();

            // Create OP_RETURN output with snapshot hash
            // OP_RETURN <snapshot_hash_bytes>
            std::vector<uint8_t> op_return_data;
            op_return_data.push_back(0x6a); // OP_RETURN
            op_return_data.push_back(static_cast<uint8_t>(snapshot_hash.length() / 2)); // Push 32 bytes (64 hex chars)
            // Convert hex string to bytes
            for (size_t i = 0; i < snapshot_hash.length(); i += 2) {
                std::string byte_str = snapshot_hash.substr(i, 2);
                op_return_data.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
            }

            std::string op_return_hex = HexStr(op_return_data);

            // Create a raw transaction with OP_RETURN output
            // This is a minimal transaction that commits the pricing hash to chain
            CMutableTransaction tx;
            tx.version = 2;
            tx.nLockTime = 0;

            // Add OP_RETURN output (0 value)
            CTxOut op_return_out;
            op_return_out.nValue = 0;
            // Build script: OP_RETURN <hash>
            CScript op_return_script;
            op_return_script << OP_RETURN;
            // Convert hash hex to bytes and push
            std::vector<uint8_t> hash_bytes;
            for (size_t i = 0; i < snapshot_hash.length(); i += 2) {
                std::string byte_str = snapshot_hash.substr(i, 2);
                hash_bytes.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
            }
            op_return_script << hash_bytes;
            op_return_out.scriptPubKey = op_return_script;
            tx.vout.push_back(op_return_out);

            // Serialize transaction to hex
            std::string tx_hex = EncodeHexTx(CTransaction(tx));

            UniValue result(UniValue::VOBJ);
            result.pushKV("snapshot_hash", snapshot_hash);
            result.pushKV("op_return_hex", HexStr(op_return_data));
            result.pushKV("hex", tx_hex);
            result.pushKV("message",
                "OP_RETURN pricing commitment created. "
                "Add inputs to this transaction, sign it, and broadcast with sendrawtransaction. "
                "The snapshot_hash MUST be stored and passed to createescrow.");

            LogInfo("RPC: commitpricing - Created OP_RETURN commitment, hash=%s, rate=%lld tokens/TKNC",
                     snapshot_hash, rate_tokens_per_tknc);

            return result;
        },
    };
}

static RPCMethod suspendescrow()
{
    return RPCMethod{"suspendescrow",
        "Suspend an active escrow (e.g., when miner goes offline).\n",
        {
            {"escrow_id", RPCArg::Type::STR, RPCArg::Optional::NO, "Escrow ID to suspend"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "escrow_id", "Escrow ID"},
                {RPCResult::Type::STR, "state", "SUSPENDED"},
            }
        },
        RPCExamples{
            HelpExampleCli("suspendescrow", "\"escrow_xxx...\"")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string escrow_id = request.params[0].get_str();

            std::lock_guard<std::mutex> lock(g_escrow_mutex);
            auto escrow_opt = g_spending_limit_db->ReadSpendingLimit(escrow_id);
            if (!escrow_opt) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Escrow not found");
            }

            SpendingLimit escrow = *escrow_opt;

            if (escrow.state != SpendingLimit::State::CREATED &&
                escrow.state != SpendingLimit::State::ACTIVE) {
                throw JSONRPCError(RPC_MISC_ERROR, "Escrow is not in an active state");
            }

            escrow.state = SpendingLimit::State::SUSPENDED;
            g_spending_limit_db->WriteSpendingLimit(escrow_id, escrow);

            UniValue result(UniValue::VOBJ);
            result.pushKV("escrow_id", escrow.escrow_id);
            result.pushKV("state", SpendingLimitStateToString(escrow.state));

            LogInfo("RPC: suspendescrow - Escrow %s suspended", escrow_id);

            return result;
        },
    };
}

static RPCMethod closeescrow()
{
    return RPCMethod{"closeescrow",
        "Close an expired escrow.\n",
        {
            {"escrow_id", RPCArg::Type::STR, RPCArg::Optional::NO, "Escrow ID to close"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "escrow_id", "Escrow ID"},
                {RPCResult::Type::STR, "state", "CLOSED"},
                {RPCResult::Type::NUM, "consumed_tknc", "Total TKNC consumed"},
            }
        },
        RPCExamples{
            HelpExampleCli("closeescrow", "\"escrow_xxx...\"")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string escrow_id = request.params[0].get_str();

            std::lock_guard<std::mutex> lock(g_escrow_mutex);
            auto escrow_opt = g_spending_limit_db->ReadSpendingLimit(escrow_id);
            if (!escrow_opt) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Escrow not found");
            }

            SpendingLimit escrow = *escrow_opt;

            if (!escrow.IsExpired()) {
                throw JSONRPCError(RPC_MISC_ERROR, "Escrow has not expired yet");
            }

            if (escrow.state == SpendingLimit::State::CLOSED) {
                throw JSONRPCError(RPC_MISC_ERROR, "Escrow is already closed");
            }

            escrow.state = SpendingLimit::State::CLOSED;
            g_spending_limit_db->WriteSpendingLimit(escrow_id, escrow);

            UniValue result(UniValue::VOBJ);
            result.pushKV("escrow_id", escrow.escrow_id);
            result.pushKV("state", SpendingLimitStateToString(escrow.state));
            result.pushKV("consumed_tknc", ValueFromAmount(escrow.consumed_tknc));

            LogInfo("RPC: closeescrow - Escrow %s closed, consumed %s TKNC",
                     escrow_id, FormatMoney(escrow.consumed_tknc));

            return result;
        },
    };
}

// Miner pricing storage (node-side, replaces miner-side set_price)
// Miners login to WEB to set their exchange rate, but the node stores it
// for handshake verification and billing calculation.
static std::map<std::string, int64_t> g_miner_prices;  // miner_wallet -> price_per_1m_tknc
static std::mutex g_miner_price_mutex;


static RPCMethod setminerprice()
{
    return RPCMethod{"setminerprice",
        "Set miner's token exchange rate (TKNC per 1M tokens).\n"
        "This is stored on the node for billing calculation and handshake verification.\n"
        "Miners set their price via WEB login; this RPC is for programmatic access.\n",
        {
            {"miner_wallet", RPCArg::Type::STR, RPCArg::Optional::NO, "Miner wallet address"},
            {"price_per_1m_tknc", RPCArg::Type::NUM, RPCArg::Optional::NO, "Price: TKNC per 1 million tokens (must be > 0)"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "miner_wallet", "Miner wallet address"},
                {RPCResult::Type::NUM, "price_per_1m_tknc", "Set price"},
                {RPCResult::Type::NUM, "tokens_per_tknc", "Calculated: tokens per 1 TKNC"},
                {RPCResult::Type::STR, "status", "success"},
            }
        },
        RPCExamples{
            HelpExampleCli("setminerprice", "\"token1m...\" 100")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string miner_wallet = request.params[0].get_str();
            int64_t price_per_1m = request.params[1].getInt<int64_t>();

            if (price_per_1m <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "price_per_1m_tknc must be positive");
            }

            int64_t tokens_per_tknc = 1000000LL / price_per_1m;
            if (tokens_per_tknc < 1000) tokens_per_tknc = 1000;
            if (tokens_per_tknc > 1000000) tokens_per_tknc = 1000000;

            {
                std::lock_guard<std::mutex> lock(g_miner_price_mutex);
                g_miner_prices[miner_wallet] = price_per_1m;
            }

            // Persist to LevelDB so price survives node restart
            if (g_spending_limit_db) {
                g_spending_limit_db->WriteMinerPrice(miner_wallet, price_per_1m);
            }

            LogInfo("setminerprice: miner=%s, price_per_1m=%lld, tokens_per_tknc=%lld",
                     miner_wallet, price_per_1m, tokens_per_tknc);

            UniValue result(UniValue::VOBJ);
            result.pushKV("miner_wallet", miner_wallet);
            result.pushKV("price_per_1m_tknc", price_per_1m);
            result.pushKV("tokens_per_tknc", tokens_per_tknc);
            result.pushKV("status", "success");
            return result;
        }
    };
}

static RPCMethod getminerprice()
{
    return RPCMethod{"getminerprice",
        "Get miner's token exchange rate (TKNC per 1M tokens).\n"
        "Returns the price set by the miner for billing calculation and handshake verification.\n",
        {
            {"miner_wallet", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Miner wallet address (omit to get first registered miner's price)"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "miner_wallet", "Miner wallet address"},
                {RPCResult::Type::NUM, "price_per_1m_tknc", "Price: TKNC per 1 million tokens"},
                {RPCResult::Type::NUM, "tokens_per_tknc", "Calculated: tokens per 1 TKNC"},
                {RPCResult::Type::STR, "exchange_rate_display", "Human-readable exchange rate"},
            }
        },
        RPCExamples{
            HelpExampleCli("getminerprice", "")
            + HelpExampleCli("getminerprice", "\"token1m...\"")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string miner_wallet;
            if (request.params.size() > 0) {
                miner_wallet = request.params[0].get_str();
            }

            int64_t price = GetMinerPrice(miner_wallet);
            int64_t tokens_per_tknc = price > 0 ? 1000000LL / price : 0;

            // Find the actual wallet key used
            std::string used_wallet;
            {
                std::lock_guard<std::mutex> lock(g_miner_price_mutex);
                if (!miner_wallet.empty()) {
                    auto it = g_miner_prices.find(miner_wallet);
                    if (it != g_miner_prices.end()) used_wallet = miner_wallet;
                }
                if (used_wallet.empty() && !g_miner_prices.empty()) {
                    used_wallet = g_miner_prices.begin()->first;
                }
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("miner_wallet", used_wallet);
            result.pushKV("price_per_1m_tknc", price);
            result.pushKV("tokens_per_tknc", tokens_per_tknc);
            result.pushKV("exchange_rate_display", strprintf("%d TKNC = 1M tokens", price));
            return result;
        }
    };
}

// Get miner price (exported for use in inference_gateway and handshake verification)
int64_t GetMinerPrice(const std::string& miner_wallet)
{
    std::lock_guard<std::mutex> lock(g_miner_price_mutex);

    // If memory cache is empty, try loading from LevelDB
    if (g_miner_prices.empty() && g_spending_limit_db) {
        auto db_prices = g_spending_limit_db->ListAllMinerPrices();
        for (const auto& [wallet, price] : db_prices) {
            g_miner_prices[wallet] = price;
        }
    }

    if (!miner_wallet.empty()) {
        auto it = g_miner_prices.find(miner_wallet);
        if (it != g_miner_prices.end()) {
            return it->second;
        }
    }
    // If wallet not specified or not found, return the first registered miner's price
    if (!g_miner_prices.empty()) {
        return g_miner_prices.begin()->second;
    }
    return 10;  // Default: 10 TKNC per 1M tokens
}

void RegisterEscrowRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"escrow", &createescrow},
        {"escrow", &getescrowinfo},
        {"escrow", &listescrows},
        {"escrow", &commitpricing},
        {"escrow", &suspendescrow},
        {"escrow", &closeescrow},
        {"escrow", &setminerprice},
        {"escrow", &getminerprice},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}

bool CheckAndDeductEscrow(const std::string& api_key, int64_t tokens_used, BillingReceipt& receipt)
{
    std::lock_guard<std::mutex> lock(g_escrow_mutex);
    auto escrow_list = g_spending_limit_db->ListAllSpendingLimits();
    for (auto& [id, escrow] : escrow_list) {
        if (escrow.api_key == api_key) {
            if (escrow.state != SpendingLimit::State::CREATED &&
                escrow.state != SpendingLimit::State::ACTIVE) {
                // PAYMENT_PENDING, SUSPENDED, EXHAUSTED, CLOSED all block inference
                LogWarning("[CheckAndDeductEscrow] Escrow %s in state %s — blocking inference",
                           escrow.escrow_id.c_str(), SpendingLimitStateToString(escrow.state));
                return false;
            }
            if (escrow.IsExhausted()) return false;
            if (escrow.IsExpired()) return false;

            // SECURITY FIX (2026-06-30): Integer overflow check for cost calculation.
            // tokens_used * rate_tknc_per_token could overflow int64_t if both are large.
            if (escrow.rate_tknc_per_token > 0 && tokens_used > INT64_MAX / escrow.rate_tknc_per_token) {
                LogWarning("CheckAndDeductEscrow: overflow detected (tokens=%lld rate=%lld)", tokens_used, escrow.rate_tknc_per_token);
                return false;
            }
            CAmount cost = tokens_used * escrow.rate_tknc_per_token;
            if (cost <= 0) cost = 1;
            // SECURITY: Use GetRemainingLimit() instead of stored spending_limit field to avoid stale data.
            CAmount remaining_limit = escrow.GetRemainingLimit();
            if (cost > remaining_limit) {
                cost = remaining_limit;
                if (escrow.rate_tknc_per_token > 0) {
                    tokens_used = static_cast<int64_t>(cost / escrow.rate_tknc_per_token);
                }
            }

            // Track previous consumption for on-chain payment trigger
            CAmount prev_consumed = escrow.consumed_tknc;

            escrow.consumed_tknc += cost;
            escrow.spending_limit = escrow.total_tknc - escrow.consumed_tknc;
            escrow.used_tokens += tokens_used;
            escrow.last_activity = GetTime();

            if (escrow.state == SpendingLimit::State::CREATED) {
                escrow.state = SpendingLimit::State::ACTIVE;
            }
            if (escrow.IsExhausted()) {
                escrow.state = SpendingLimit::State::EXHAUSTED;
            }

            g_spending_limit_db->WriteSpendingLimit(id, escrow);

            // === PAY-AS-YOU-GO: Every 1 TKNC consumed → on-chain transfer ===
            // Check if consumption crossed a 1 TKNC boundary
            CAmount COIN_UNIT = COIN;  // 1 TKNC in smallest unit
            CAmount prev_full_coins = prev_consumed / COIN_UNIT;
            CAmount curr_full_coins = escrow.consumed_tknc / COIN_UNIT;

            if (curr_full_coins > prev_full_coins && !escrow.miner_wallet.empty()) {
                CAmount coins_to_pay = (curr_full_coins - prev_full_coins) * COIN_UNIT;
                LogInfo("[PAY-AS-YOU-GO] Consumption crossed %lld TKNC boundary. "
                        "Transferring %s TKNC from user wallet → miner_wallet=%s",
                        (long long)curr_full_coins,
                        FormatMoney(coins_to_pay).c_str(),
                        escrow.miner_wallet.c_str());

                // Find the user's wallet by escrow.user_wallet address.
                // Any loaded wallet owning this address can be used — no special "payment" wallet needed.
                auto user_wallet = FindWalletByAddress(escrow.user_wallet);
                std::string txid;
                bool transfer_ok = false;
                if (user_wallet) {
                    if (user_wallet->IsLocked()) {
                        LogWarning("[PAY-AS-YOU-GO] User wallet '%s' is LOCKED — cannot transfer. "
                                   "Blocking further inference until wallet is unlocked.",
                                   user_wallet->GetName());
                    } else {
                        bool paid = TransferFromWallet(user_wallet, escrow.miner_wallet, coins_to_pay, txid);
                        if (paid && !txid.empty()) {
                            LogInfo("[PAY-AS-YOU-GO] Transfer succeeded: txid=%s", txid.c_str());
                            transfer_ok = true;
                        } else {
                            LogWarning("[PAY-AS-YOU-GO] Transfer FAILED — wallet may have insufficient balance. "
                                       "Blocking further inference until wallet is recharged.");
                        }
                    }
                } else {
                    LogWarning("[PAY-AS-YOU-GO] No loaded wallet owns address %s — transfer skipped. "
                               "Blocking further inference until wallet is loaded and unlocked.",
                               escrow.user_wallet.c_str());
                }

                // If transfer failed, mark escrow as PAYMENT_PENDING to block further inference
                if (!transfer_ok) {
                    escrow.state = SpendingLimit::State::PAYMENT_PENDING;
                    g_spending_limit_db->WriteSpendingLimit(id, escrow);
                    LogWarning("[PAY-AS-YOU-GO] Escrow %s marked as PAYMENT_PENDING — inference blocked until payment succeeds",
                               escrow.escrow_id.c_str());
                }
            }

            receipt.api_key = api_key;
            receipt.escrow_id = escrow.escrow_id;
            receipt.miner_wallet = escrow.miner_wallet;
            receipt.model_name = escrow.model_name;
            receipt.output_tokens = tokens_used;
            receipt.total_tokens = tokens_used;
            receipt.cost_tknc = cost;
            receipt.consumed_tknc = escrow.consumed_tknc;
            receipt.remaining_limit = escrow.spending_limit;
            receipt.timestamp = GetTime();

            LogInfo("CheckAndDeductEscrow: api_key=%s, escrow=%s, tokens=%lld, cost=%s, remaining=%lld, consumed=%s",
                     api_key, escrow.escrow_id, tokens_used, FormatMoney(cost),
                     escrow.GetRemainingTokens(), FormatMoney(escrow.consumed_tknc).c_str());
            return true;
        }
    }
    return false;
}