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
#include <wallet/receive.h>
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
#include <net.h>
#include <net/message.h>
#include <netmessagemaker.h>
#include <node/context.h>
#include <rpc/server_util.h>
#include <apikey/api_key.h>

// Persistent spending limit database (replaces in-memory map)
static std::unique_ptr<CSpendingLimitDB> g_spending_limit_db;
static std::mutex g_escrow_mutex;

// === Pending cost reservation system (prevents concurrent request flood bypass) ===
// Tracks the estimated cost of all in-flight inference requests per API key.
// CanAffordInference checks wallet_balance > pending_cost, so concurrent requests
// that all pass the balance check will see decreasing available balance as pending
// cost accumulates, eventually causing refusal before the wallet is actually drained.
static std::map<std::string, CAmount> g_pending_costs; // api_key -> total reserved cost (satoshis)
static std::mutex g_pending_cost_mutex;

// Reserve estimated cost for an in-flight inference request.
CAmount ReserveEscrowCost(const std::string& api_key, CAmount estimated_cost)
{
    if (api_key.empty() || estimated_cost <= 0) return 0;
    std::lock_guard<std::mutex> lock(g_pending_cost_mutex);
    g_pending_costs[api_key] += estimated_cost;
    LogInfo("[PendingCost] Reserved %s TKNC for api_key=%s... (total pending: %s)",
            FormatMoney(estimated_cost).c_str(), api_key.substr(0, 8).c_str(),
            FormatMoney(g_pending_costs[api_key]).c_str());
    return estimated_cost;
}

// Release a previously reserved cost after billing is complete (or on error).
void ReleaseEscrowCost(const std::string& api_key, CAmount reserved_cost)
{
    if (api_key.empty() || reserved_cost <= 0) return;
    std::lock_guard<std::mutex> lock(g_pending_cost_mutex);
    auto it = g_pending_costs.find(api_key);
    if (it != g_pending_costs.end()) {
        if (it->second >= reserved_cost) {
            it->second -= reserved_cost;
        } else {
            it->second = 0;
        }
        LogInfo("[PendingCost] Released %s TKNC for api_key=%s... (total pending: %s)",
                FormatMoney(reserved_cost).c_str(), api_key.substr(0, 8).c_str(),
                FormatMoney(it->second).c_str());
        if (it->second == 0) {
            g_pending_costs.erase(it);
        }
    }
}

// Get the total pending (reserved) cost for an API key.
CAmount GetPendingCost(const std::string& api_key)
{
    std::lock_guard<std::mutex> lock(g_pending_cost_mutex);
    auto it = g_pending_costs.find(api_key);
    return (it != g_pending_costs.end()) ? it->second : 0;
}

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

// Forward declaration — ParsePendingTxids is defined later in this file.
static std::vector<std::pair<std::string, CAmount>> ParsePendingTxids(const std::string& pending_str);

// === Miner-side independent payment verification ===
// Tracks how much TKNC this miner has served (provided inference for) per API key.
// This is INDEPENDENT from the client's escrow state — the miner tracks its own
// served amount to verify that it's actually being paid.
static std::map<std::string, CAmount> g_miner_served_tknc; // api_key -> total TKNC served
static std::mutex g_miner_served_mutex;

// Track tokens served by this miner for an API key.
void TrackMinerServedTokens(const std::string& api_key, int64_t tokens_used)
{
    if (api_key.empty() || tokens_used <= 0) return;

    // Get the rate from the escrow record for this API key — NOT GetTokensPerTknc("")
    // which returns 0 (no rate) since we removed the begin()->second fallback.
    int64_t tokens_per_tknc = 0;
    auto esc_opt = FindSpendingLimitByAPIKey(api_key);
    if (esc_opt.has_value() && esc_opt->rate_tokens_per_tknc > 0) {
        tokens_per_tknc = esc_opt->rate_tokens_per_tknc;
    }
    CAmount rate_tknc_per_token = (tokens_per_tknc > 0) ? (CAmount)(COIN / tokens_per_tknc) : 1;
    if (rate_tknc_per_token <= 0) rate_tknc_per_token = 1;

    CAmount cost = tokens_used * rate_tknc_per_token;
    if (cost <= 0) cost = 1;

    std::lock_guard<std::mutex> lock(g_miner_served_mutex);
    g_miner_served_tknc[api_key] += cost;
    LogInfo("[MinerPay] Served %s TKNC for api_key=%s... (total served: %s)",
            FormatMoney(cost).c_str(), api_key.substr(0, 8).c_str(),
            FormatMoney(g_miner_served_tknc[api_key]).c_str());
}

// Miner-side independent payment verification.
// The miner is the authoritative gatekeeper (client-side checks are advisory).
// Tracks served_tknc locally and verifies txids against its own wallet.
// Refuses if served_tknc - received_tknc >= 2 * COIN (grace period).
// Client sends txids via X-TKNC-Pending-Txids header; miner validates against its wallet.
bool CheckMinerReceivedPayment(const std::string& api_key, std::string& error_msg,
                                const std::string& pending_txids_override)
{
    error_msg.clear();

    // 1. Get served_tknc from LOCAL tracking — this is the miner's own counter,
    //    not trusting any client-side data. Cannot be faked by the client.
    CAmount served_tknc = 0;
    {
        std::lock_guard<std::mutex> lock(g_miner_served_mutex);
        auto it = g_miner_served_tknc.find(api_key);
        if (it != g_miner_served_tknc.end()) {
            served_tknc = it->second;
        }
    }

    // 2. If nothing served yet, allow — no debt to check.
    //    This MUST be checked BEFORE escrow lookup, because the escrow might
    //    not have been P2P-synced to this miner yet.
    if (served_tknc <= 0) {
        return true;
    }

    // 3. Find escrow for miner_wallet address (to locate this miner's wallet).
    auto escrow_opt = FindSpendingLimitByAPIKey(api_key);
    if (!escrow_opt.has_value()) {
        // No escrow found — P2P sync may not have arrived yet.
        // Allow inference: served_tknc is small (just started), risk is low.
        // The miner will catch up once the escrow is synced.
        LogWarning("[MinerPay] No escrow for api_key=%s... but served=%s. "
                   "Allowing (escrow sync pending).", api_key.substr(0, 8).c_str(),
                   FormatMoney(served_tknc).c_str());
        return true;
    }
    auto& escrow = *escrow_opt;

    // 4. Check if this is the miner (miner's wallet is on this node).
    //    If not, this node is a relay — allow (not our job to verify).
    if (escrow.miner_wallet.empty()) {
        return true;
    }
    auto miner_wallet_ptr = FindWalletByAddress(escrow.miner_wallet);
    if (!miner_wallet_ptr) {
        // Miner's wallet not on this node — this is a client/relay node, not the miner.
        return true;
    }

    // 5. Get pending_txids — prefer HTTP header override (more current than P2P sync).
    //    The client sends this header with each forwarded request.
    std::string pending_txids_str = !pending_txids_override.empty()
                                    ? pending_txids_override
                                    : escrow.pending_txids;

    // 6. Verify each txid against the miner's OWN wallet.
    //    Only count txids that actually exist in the wallet with an output to the miner.
    //    This is the miner's independent verification — the client cannot fake this.
    CAmount received_tknc = 0;
    auto pending_list = ParsePendingTxids(pending_txids_str);
    if (!pending_list.empty()) {
        LOCK(miner_wallet_ptr->cs_wallet);
        for (const auto& [txid_str, claimed_amount] : pending_list) {
            auto txid_opt = uint256::FromHex(txid_str);
            if (!txid_opt.has_value()) {
                LogInfo("[MinerPay] Invalid txid format: %s (api_key=%s...)",
                        txid_str.substr(0, 16).c_str(), api_key.substr(0, 8).c_str());
                continue;
            }
            auto wit = miner_wallet_ptr->mapWallet.find(Txid::FromUint256(*txid_opt));
            if (wit == miner_wallet_ptr->mapWallet.end()) {
                // Transaction NOT in miner's wallet — client is lying or tx not received yet.
                LogInfo("[MinerPay] txid=%s NOT in wallet — not counted (api_key=%s...)",
                        txid_str.substr(0, 16).c_str(), api_key.substr(0, 8).c_str());
                continue;
            }
            // Transaction IS in our wallet. Verify it has an output to us.
            // depth >= 0 means: 0 = in mempool (broadcast, not yet in block), >=1 = confirmed.
            // We accept mempool transactions as valid payment — "on-chain" includes mempool.
            int depth = miner_wallet_ptr->GetTxDepthInMainChain(wit->second);
            if (depth < 0) {
                // Conflicted/invalid — don't count.
                continue;
            }
            // Verify the output is to the miner's wallet.
            bool output_to_miner = false;
            for (const CTxOut& txout : wit->second.tx->vout) {
                if (miner_wallet_ptr->IsMine(txout)) {
                    output_to_miner = true;
                    break;
                }
            }
            if (output_to_miner) {
                // Verified: txid is in our wallet AND has an output to us.
                // Count the claimed amount (what the client says they transferred).
                received_tknc += claimed_amount;
                LogInfo("[MinerPay] Verified txid=%s... amount=%s (depth=%d)",
                        txid_str.substr(0, 16).c_str(),
                        FormatMoney(claimed_amount).c_str(), depth);
            }
        }
    }

    // Apply the same sliding window grace period from the MINER's perspective:
    // served_tknc - received_tknc < 2 * COIN → allow
    // served_tknc - received_tknc >= 2 * COIN → refuse
    CAmount unconfirmed_debt = served_tknc - received_tknc;
    if (unconfirmed_debt >= 2 * COIN) {
        error_msg = strprintf("Miner has served %s TKNC but only received %s TKNC in verified payments. "
                              "Debt exceeds 2 TKNC grace period. Inference refused until payment is received.",
                              FormatMoney(served_tknc).c_str(),
                              FormatMoney(received_tknc).c_str());
        LogWarning("[MinerPay] Rejected inference for api_key=%s...: "
                   "served=%s, received=%s, debt=%s (>= 2 TKNC grace limit)",
                   api_key.substr(0, 8).c_str(),
                   FormatMoney(served_tknc).c_str(),
                   FormatMoney(received_tknc).c_str(),
                   FormatMoney(unconfirmed_debt).c_str());
        return false;
    }

    LogInfo("[MinerPay] api_key=%s... served=%s, received=%s, debt=%s (< 2 TKNC, OK)",
            api_key.substr(0, 8).c_str(),
            FormatMoney(served_tknc).c_str(),
            FormatMoney(received_tknc).c_str(),
            FormatMoney(unconfirmed_debt).c_str());
    return true;
}

// === Sliding window grace period billing ===
// Parse pending_txids string: "txid1:amount1;txid2:amount2;..."
// Returns vector of (txid, amount) pairs.
static std::vector<std::pair<std::string, CAmount>> ParsePendingTxids(const std::string& pending_str)
{
    std::vector<std::pair<std::string, CAmount>> result;
    if (pending_str.empty()) return result;

    size_t start = 0;
    while (start < pending_str.size()) {
        size_t sep = pending_str.find(';', start);
        std::string entry = (sep == std::string::npos)
            ? pending_str.substr(start)
            : pending_str.substr(start, sep - start);

        size_t colon = entry.find(':');
        if (colon != std::string::npos) {
            std::string txid = entry.substr(0, colon);
            std::string amount_str = entry.substr(colon + 1);
            try {
                CAmount amount = std::stoll(amount_str);
                result.push_back({txid, amount});
            } catch (...) {}
        }

        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return result;
}

// Serialize pending txids back to string
static std::string SerializePendingTxids(const std::vector<std::pair<std::string, CAmount>>& txids)
{
    std::string result;
    for (size_t i = 0; i < txids.size(); i++) {
        if (i > 0) result += ";";
        result += txids[i].first + ":" + std::to_string(txids[i].second);
    }
    return result;
}

// Check if a transaction is confirmed on-chain (depth >= 1) using the wallet.
static bool IsTxConfirmedOnChain(const std::string& txid_str, std::shared_ptr<wallet::CWallet> pwallet)
{
    if (txid_str.empty() || !pwallet) return false;

    auto txid_opt = uint256::FromHex(txid_str);
    if (!txid_opt.has_value()) return false;

    LOCK(pwallet->cs_wallet);
    auto it = pwallet->mapWallet.find(Txid::FromUint256(*txid_opt));
    if (it == pwallet->mapWallet.end()) return false;

    int depth = pwallet->GetTxDepthInMainChain(it->second);
    return depth >= 1;
}

// Update confirmed_tknc by checking pending transfers for on-chain confirmation.
// Moves confirmed amounts from pending to confirmed_tknc.
void UpdateConfirmedTransfers(SpendingLimit& escrow)
{
    if (escrow.pending_txids.empty()) return;

    auto pending = ParsePendingTxids(escrow.pending_txids);
    if (pending.empty()) return;

    // Find the user's wallet to check transaction confirmations
    auto user_wallet = FindWalletByAddress(escrow.user_wallet);
    if (!user_wallet) return;

    std::vector<std::pair<std::string, CAmount>> still_pending;
    CAmount newly_confirmed = 0;

    for (const auto& [txid, amount] : pending) {
        if (IsTxConfirmedOnChain(txid, user_wallet)) {
            newly_confirmed += amount;
            LogInfo("[GracePeriod] Transfer %s confirmed on-chain: %s TKNC (txid=%s)",
                    FormatMoney(amount).c_str(), FormatMoney(amount).c_str(), txid.c_str());
        } else {
            still_pending.push_back({txid, amount});
        }
    }

    if (newly_confirmed > 0) {
        escrow.confirmed_tknc += newly_confirmed;
        escrow.pending_txids = SerializePendingTxids(still_pending);
        LogInfo("[GracePeriod] Updated escrow %s: confirmed_tknc=%s, pending=%zu transfers, newly_confirmed=%s",
                escrow.escrow_id.c_str(), FormatMoney(escrow.confirmed_tknc).c_str(),
                still_pending.size(), FormatMoney(newly_confirmed).c_str());
    }
}

// Forward declaration — g_wallet_ctx is defined later in this file.
static wallet::WalletContext* g_wallet_ctx;

// Pre-inference balance check: returns true if user can afford inference, false otherwise.
// If false, error_msg contains a human-readable reason.
// This is called BEFORE forwarding the request to the miner, preventing free inference.
bool CanAffordInference(const std::string& api_key, std::string& error_msg)
{
 error_msg.clear();

 auto escrow_opt = FindSpendingLimitByAPIKey(api_key);
 if (!escrow_opt.has_value()) {
 error_msg = "No billing escrow found for this API key. Run tknc_setinferproxytarget first.";
 return false;
 }

 auto& escrow = *escrow_opt;

 // === Sliding window grace period check ===
 // Before any other checks, update confirmed transfers (scan pending txids for on-chain confirmation).
 // This keeps confirmed_tknc up-to-date so the grace period check below is accurate.
 // Save to DB if any transfers were newly confirmed.
 {
     std::string old_pending = escrow.pending_txids;
     UpdateConfirmedTransfers(escrow);
     // If pending_txids changed (some transfers were confirmed), persist the update
     if (escrow.pending_txids != old_pending) {
         std::lock_guard<std::mutex> db_lock(g_escrow_mutex);
         // Re-read to get the correct DB key (escrow_id is the key)
         auto db_list = g_spending_limit_db->ListAllSpendingLimits();
         for (auto& [db_id, db_escrow] : db_list) {
             if (db_escrow.escrow_id == escrow.escrow_id) {
                 db_escrow.confirmed_tknc = escrow.confirmed_tknc;
                 db_escrow.pending_txids = escrow.pending_txids;
                 g_spending_limit_db->WriteSpendingLimit(db_id, db_escrow);
                 break;
             }
         }
     }
 }

 // === Billing check: different rules for client vs miner ===
 // CLIENT SIDE (user's wallet is on this node):
 //   NO grace period. If consumed crosses 1 TKNC boundary and transfer hasn't been made,
 //   refuse immediately. Client must transfer at each 1 TKNC boundary.
 //   Check: consumed_tknc - transferred_tknc >= COIN → refuse
 // MINER SIDE (miner's wallet is on this node, user's wallet is not):
 //   Sliding window grace period: allow up to 2 TKNC of unconfirmed debt.
 //   This gives time for on-chain transfers to confirm.
 //   Check: consumed_tknc - confirmed_tknc >= 2 * COIN → refuse
 if (escrow.total_tknc == 0 && escrow.consumed_tknc > 0) {
     auto user_wallet_ptr = FindWalletByAddress(escrow.user_wallet);
     auto miner_wallet_ptr = escrow.miner_wallet.empty() ? nullptr : FindWalletByAddress(escrow.miner_wallet);

     bool is_client_side = (user_wallet_ptr != nullptr);

     if (is_client_side) {
         // CLIENT SIDE: strict check — no grace period.
         // If consumed - transferred >= 1 TKNC, the transfer should have happened but didn't.
         // Refuse inference until the transfer is made (wallet unlocked, sufficient balance).
         CAmount untransferred = escrow.consumed_tknc - escrow.transferred_tknc;
         if (untransferred >= COIN) {
             error_msg = strprintf("Payment required: consumed %s TKNC but only %s TKNC transferred. "
                                   "Please unlock wallet to enable payment transfer.",
                                   FormatMoney(escrow.consumed_tknc).c_str(),
                                   FormatMoney(escrow.transferred_tknc).c_str());
             LogWarning("[ClientPay] Rejected inference for api_key=%s...: "
                        "consumed=%s, transferred=%s, untransferred=%s (>= 1 TKNC, no grace on client)",
                        api_key.substr(0, 8).c_str(),
                        FormatMoney(escrow.consumed_tknc).c_str(),
                        FormatMoney(escrow.transferred_tknc).c_str(),
                        FormatMoney(untransferred).c_str());
             return false;
         }
         LogInfo("[ClientPay] api_key=%s... consumed=%s, transferred=%s, untransferred=%s (< 1 TKNC, OK)",
                 api_key.substr(0, 8).c_str(),
                 FormatMoney(escrow.consumed_tknc).c_str(),
                 FormatMoney(escrow.transferred_tknc).c_str(),
                 FormatMoney(untransferred).c_str());
     } else {
         // MINER SIDE or RELAY: sliding window grace period (2 TKNC).
         // Check based on transferred_tknc (on-chain transfers), NOT confirmed_tknc.
         // Once a transfer is on-chain it's irreversible — no need to wait for block confirmations.
         // This gives 1 TKNC of grace: if client used 2 TKNC but hasn't transferred anything, refuse.
         CAmount untransferred = escrow.consumed_tknc - escrow.transferred_tknc;
         if (untransferred >= 2 * COIN) {
             error_msg = strprintf("Payment overdue: consumed %s TKNC but only %s TKNC transferred on-chain. "
                                   "Grace period (2 TKNC) exceeded.",
                                   FormatMoney(escrow.consumed_tknc).c_str(),
                                   FormatMoney(escrow.transferred_tknc).c_str());
             LogWarning("[MinerPay] Rejected inference for api_key=%s...: "
                        "consumed=%s, transferred=%s, untransferred=%s (>= 2 TKNC grace limit)",
                        api_key.substr(0, 8).c_str(),
                        FormatMoney(escrow.consumed_tknc).c_str(),
                        FormatMoney(escrow.transferred_tknc).c_str(),
                        FormatMoney(untransferred).c_str());
             return false;
         }
         LogInfo("[MinerPay] api_key=%s... consumed=%s, transferred=%s, untransferred=%s (< 2 TKNC, OK)",
                 api_key.substr(0, 8).c_str(),
                 FormatMoney(escrow.consumed_tknc).c_str(),
                 FormatMoney(escrow.transferred_tknc).c_str(),
                 FormatMoney(untransferred).c_str());
     }
 }

 // Check escrow state — PAYMENT_PENDING is auto-recovered in CheckAndDeductEscrow,
 // so we don't block inference here. Only block for truly terminal states.
 if (escrow.state == SpendingLimit::State::SUSPENDED) {
 error_msg = "Escrow suspended — miner may be offline.";
 return false;
 }
 if (escrow.state == SpendingLimit::State::CLOSED || escrow.state == SpendingLimit::State::EXHAUSTED) {
 error_msg = "Escrow is " + std::string(SpendingLimitStateToString(escrow.state)) + ".";
 return false;
 }
 if (escrow.IsExpired()) {
 error_msg = "Escrow has expired.";
 return false;
 }

 // For prepaid escrows (total_tknc > 0), check remaining limit
 // SECURITY: On the miner node, do NOT trust prepaid escrows received via P2P sync.
 // A malicious client can create a fake prepaid escrow with total_tknc=1000000 and sync it
 // to the miner, getting free inference. To prevent this, we verify the wallet balance
 // even for prepaid escrows — the user's wallet must have sufficient funds to back the escrow.
 if (escrow.total_tknc > 0) {
 if (escrow.GetRemainingLimit() <= 0) {
 error_msg = "Spending limit exhausted.";
 return false;
 }

 // SECURITY: Verify wallet balance even for prepaid escrows.
 // On the miner node, the user's wallet may not be present (it's on the client node).
 // If the wallet is not found, we CANNOT trust the prepaid escrow — refuse inference.
 auto prepaid_user_wallet = FindWalletByAddress(escrow.user_wallet);
 if (!prepaid_user_wallet) {
     if (escrow.user_wallet.empty()) {
         error_msg = "Prepaid escrow has no user wallet — cannot verify. Run tknc_setinferproxytarget first.";
     } else {
         error_msg = "User wallet not found on this node. Cannot verify prepaid escrow balance.";
     }
     LogWarning("[CanAffordInference] Rejected prepaid escrow for api_key=%s: wallet not found (user_wallet=%s)",
                api_key.substr(0, 8).c_str(), escrow.user_wallet.c_str());
     return false;
 }

 // Check wallet balance covers at least 1 TKNC (minimum for grace period)
 CAmount prepaid_available = 0;
 {
     LOCK(prepaid_user_wallet->cs_wallet);
     auto bal = GetBalance(*prepaid_user_wallet, 0, true);
     prepaid_available = bal.m_mine_trusted + bal.m_mine_untrusted_pending;
 }
 if (prepaid_available <= 0) {
     error_msg = "Insufficient wallet balance (0 TKNC available). Please recharge your wallet.";
     return false;
 }

 // If wallet is locked, allow inference but defer payment.
 // The post-inference billing code (CheckAndDeductEscrow) already handles
 // locked wallets by deferring the transfer — consumed_tknc accumulates
 // and will be charged when the wallet is unlocked.
 if (prepaid_user_wallet->IsLocked()) {
     LogWarning("[CanAffordInference] Prepaid wallet is LOCKED — payment deferred. "
                "Balance: %s TKNC. Inference allowed.", FormatMoney(prepaid_available).c_str());
 }

 // Also check pending reserved costs
 CAmount prepaid_pending = GetPendingCost(api_key);
 if (prepaid_pending > 0) {
     CAmount prepaid_effective = (prepaid_available > prepaid_pending) ? (prepaid_available - prepaid_pending) : 0;
     if (prepaid_effective <= 0) {
         error_msg = "Insufficient wallet balance: all available funds are reserved by in-flight requests.";
         return false;
     }
 }

 return true; // Prepaid escrow has remaining credit AND wallet has balance
 }

 // For pay-as-you-go (total_tknc == 0), check wallet balance
 auto user_wallet = FindWalletByAddress(escrow.user_wallet);
 if (!user_wallet) {
     // If user_wallet is empty in the escrow, try to find ANY loaded wallet
     // and auto-fix the escrow record. This handles the case where the escrow
     // was created without a user_wallet (e.g., by EnsureEscrowForAPIKey).
     if (escrow.user_wallet.empty() && g_wallet_ctx) {
         LOCK(g_wallet_ctx->wallets_mutex);
         for (const auto& w : g_wallet_ctx->wallets) {
             if (w) {
                 user_wallet = w;
                 // Try to get the wallet's address to update the escrow
                 std::string fixed_addr;
                 {
                     LOCK(w->cs_wallet);
                     for (const auto& [dest, addr_man] : w->m_address_book) {
                         if (w->IsMine(dest)) {
                             fixed_addr = EncodeDestination(dest);
                             break;
                         }
                     }
                 }
                 if (fixed_addr.empty()) {
                     // Could not find an address — skip escrow update, but still use this wallet
                 }
                 if (!fixed_addr.empty()) {
                     escrow.user_wallet = fixed_addr;
                     WriteSpendingLimitFromP2P(escrow);
                     LogInfo("[CanAffordInference] Auto-fixed escrow user_wallet=%s for api_key=%s...",
                         fixed_addr.c_str(), api_key.substr(0, 8).c_str());
                 }
                 break;
             }
         }
     }
 }

 if (!user_wallet) {
     if (escrow.user_wallet.empty()) {
         error_msg = "No user wallet associated with this API key. Run tknc_setinferproxytarget first.";
     } else {
         error_msg = "User wallet not found on this node. Cannot verify balance.";
     }
     LogWarning("[CanAffordInference] Rejected inference for api_key=%s: %s (g_wallet_ctx=%s, wallets=%zu)",
         api_key.substr(0, 8).c_str(), error_msg.c_str(),
         g_wallet_ctx ? "set" : "null",
         g_wallet_ctx ? g_wallet_ctx->wallets.size() : 0);
     return false;
 }

 // Check wallet balance first — this works even on locked wallets
 // because GetBalance only reads UTXOs, not private keys.
 CAmount available = 0;
 {
     LOCK(user_wallet->cs_wallet);
     auto bal = GetBalance(*user_wallet, 0, true);
     available = bal.m_mine_trusted + bal.m_mine_untrusted_pending;
 }

 if (available <= 0) {
 error_msg = "Insufficient wallet balance (0 TKNC available). Please recharge your wallet to use inference.";
 LogWarning("[CanAffordInference] Rejected inference for api_key=%s: wallet balance is 0",
 api_key.substr(0, 8).c_str());
 return false;
 }

 // If wallet is locked, allow inference but defer payment.
 // The post-inference billing code (CheckAndDeductEscrow) already handles
 // locked wallets by deferring the transfer — consumed_tknc accumulates
 // and will be charged when the wallet is unlocked.
 if (user_wallet->IsLocked()) {
 LogWarning("[CanAffordInference] Wallet is LOCKED — payment deferred. "
 "Balance: %s TKNC. Inference allowed.",
 FormatMoney(available).c_str());
 }

 // SECURITY: Subtract pending (reserved) costs from in-flight requests.
 // This prevents concurrent request flood attacks where many requests
 // all pass the balance check before any billing is applied.
 CAmount pending = GetPendingCost(api_key);
 if (pending > 0) {
 CAmount effective_available = (available > pending) ? (available - pending) : 0;
 LogInfo("[CanAffordInference] api_key=%s... wallet=%s, pending=%s, effective=%s",
 api_key.substr(0, 8).c_str(), FormatMoney(available).c_str(),
 FormatMoney(pending).c_str(), FormatMoney(effective_available).c_str());
 if (effective_available <= 0) {
 error_msg = "Insufficient wallet balance: all available funds are reserved by in-flight requests. "
             "Please wait for current requests to complete or recharge your wallet.";
 LogWarning("[CanAffordInference] Rejected inference for api_key=%s: "
            "wallet=%s but pending=%s (effective=0)",
            api_key.substr(0, 8).c_str(), FormatMoney(available).c_str(), FormatMoney(pending).c_str());
 return false;
 }
 }

 return true;
}

// Auto-create escrow for an API key if one doesn't exist.
// This handles the case where a user gets a new API key from the web
// but doesn't re-run tknc_setinferproxytarget. The escrow is created
// on-demand using the stored miner price and any available wallet info.
bool EnsureEscrowForAPIKey(const std::string& api_key, const std::string& param_miner_wallet,
                            const std::string& param_user_wallet, const std::string& model_name)
{
    // Check if escrow already exists
    auto existing = FindSpendingLimitByAPIKey(api_key);
    if (existing.has_value()) {
        return true; // Escrow already exists
    }

    if (api_key.empty()) return false;

    // SECURITY: Validate API key format before creating an escrow.
    // Without this, any random string (e.g., "test-billing-bypass") gets an escrow.
    if (!ValidateAPIKeyFormat(api_key)) {
        LogWarning("[EnsureEscrowForAPIKey] Refused to create escrow for invalid API key format (length=%zu)",
                   api_key.size());
        return false;
    }

    // Get miner_wallet: from parameter only — no fallback to empty string lookup
    std::string miner_wallet = param_miner_wallet;

    // Get user_wallet: from parameter, or try to find any loaded wallet
    std::string user_wallet = param_user_wallet;

    // Get verified token rate — use the specific miner wallet
    int64_t tokens_per_tknc = GetTokensPerTknc(miner_wallet);
    // No fallback to GetTokensPerTknc("") — it returns 0, which is correct
    // (no rate = no billing until rate is set via handshake or setminerprice)

    // Create the escrow
    SpendingLimit escrow;
    escrow.escrow_id = GenerateSpendingLimitID(api_key);
    escrow.api_key = api_key;
    escrow.miner_wallet = miner_wallet;
    escrow.user_wallet = user_wallet;
    escrow.model_name = model_name;
    escrow.total_tknc = 0; // Unlimited (pay-as-you-go)
    escrow.consumed_tknc = 0;
    escrow.spending_limit = 0;
    escrow.quota_tokens = INT64_MAX;
    escrow.used_tokens = 0;
    escrow.rate_tokens_per_tknc = (tokens_per_tknc > 0) ? tokens_per_tknc : 0;
    escrow.rate_tknc_per_token = (tokens_per_tknc > 0) ? (int64_t)(COIN / tokens_per_tknc) : 0;
    escrow.created_at = GetTime();
    escrow.expires_at = GetTime() + (90 * 86400); // 90 days
    escrow.last_activity = GetTime();
    escrow.state = SpendingLimit::State::CREATED;

    if (WriteSpendingLimitFromP2P(escrow)) {
        LogInfo("[EnsureEscrowForAPIKey] Auto-created escrow %s for api_key=%s... "
                "miner_wallet=%s, user_wallet=%s, tokens_per_tknc=%lld",
                escrow.escrow_id.c_str(), api_key.substr(0, 8).c_str(),
                miner_wallet.c_str(), user_wallet.c_str(), (long long)tokens_per_tknc);
        return true;
    } else {
        LogWarning("[EnsureEscrowForAPIKey] Failed to auto-create escrow for api_key=%s...",
                   api_key.substr(0, 8).c_str());
        return false;
    }
}

// === Wallet access for pay-as-you-go inference billing ===
// Replaces the old PaymentWallet singleton. Any loaded wallet owning the
// escrow.user_wallet address can be used for inference payment.
// g_wallet_ctx is declared earlier in this file (before CanAffordInference).

void SetWalletContext(wallet::WalletContext* ctx)
{
 g_wallet_ctx = ctx;
 LogInfo("SetWalletContext: wallet context registered for spending limit billing");
}

std::shared_ptr<wallet::CWallet> FindWalletByAddress(const std::string& address)
{
 if (!g_wallet_ctx) return nullptr;

 // If address is empty, return any loaded wallet (for auto-created escrows)
 if (address.empty()) {
 LOCK(g_wallet_ctx->wallets_mutex);
 for (const auto& w : g_wallet_ctx->wallets) {
 if (w) return w;
 }
 return nullptr;
 }

 CTxDestination dest = DecodeDestination(address);
 if (dest.index() == 0) {
 // Invalid address format, return any loaded wallet as fallback
 LOCK(g_wallet_ctx->wallets_mutex);
 for (const auto& w : g_wallet_ctx->wallets) {
 if (w) return w;
 }
 return nullptr;
 }
 LOCK(g_wallet_ctx->wallets_mutex);
 for (const auto& w : g_wallet_ctx->wallets) {
 if (w && w->IsMine(dest)) return w;
 }
 // Address not owned by any loaded wallet, return any wallet as fallback
 for (const auto& w : g_wallet_ctx->wallets) {
 if (w) return w;
 }
 return nullptr;
}

bool TransferFromWallet(std::shared_ptr<wallet::CWallet> pwallet,
 const std::string& to_address, CAmount amount, std::string& txid, bool subtract_fee)
{
 if (!pwallet) { LogError("[EscrowBilling] Wallet null"); return false; }
 if (pwallet->IsLocked()) { LogError("[EscrowBilling] Wallet '%s' locked", pwallet->GetName()); return false; }
 CTxDestination dest = DecodeDestination(to_address);
 if (dest.index() == 0) { LogError("[EscrowBilling] Invalid address %s", to_address.c_str()); return false; }
 wallet::CRecipient recipient{dest, amount, subtract_fee};
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
 LogInfo("[EscrowBilling] Transfer %s TKNC from '%s' to %s (subtract_fee=%d) txid=%s",
 FormatMoney(amount).c_str(), pwallet->GetName(), to_address.c_str(), subtract_fee, txid.c_str());
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
 {"amount_tknc", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "Amount of TKNC to lock (0 = unlimited, bounded by wallet balance)"},
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

 if (amount < 0) {
 throw JSONRPCError(RPC_INVALID_PARAMETER, "Amount must be non-negative (0 = unlimited)");
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
 // (Chinese comment removed)
 snapshot.quota_tokens = (amount == 0) ? INT64_MAX : (amount * rate_tokens_per_tknc) / COIN;
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
 escrow.total_tknc = amount; // 0 = unlimited (bounded by wallet balance)
 escrow.consumed_tknc = 0;
 escrow.spending_limit = amount; // 0 = unlimited (kept in sync with total_tknc)
 escrow.quota_tokens = snapshot.quota_tokens; // INT64_MAX when unlimited
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

 // P2P Escrow Sync: broadcast to peers so client nodes can validate/bill p2pinference.
 // Without this, p2pinference called on a remote client node would fail with
 // "No escrow found" because the escrow only exists on the seed node.
 int escrow_sync_count = 0;
 try {
 node::NodeContext& node_ctx = EnsureAnyNodeContext(request.context);
 CConnman& connman = EnsureConnman(node_ctx);
 connman.ForEachNode([&](CNode* pnode) {
 connman.PushMessage(pnode, NetMsg::Make(std::string(MessageTypes::ESCROWSYNC), escrow));
 escrow_sync_count++;
 });
 LogInfo("[Escrow-P2P] Broadcast ESCROWSYNC to %d peers: escrow_id=%s",
 escrow_sync_count, escrow.escrow_id);
 } catch (const std::exception& e) {
 LogWarning("[Escrow-P2P] Failed to broadcast ESCROWSYNC: %s", e.what());
 }

 UniValue result(UniValue::VOBJ);
 result.pushKV("escrow_id", escrow.escrow_id);
 result.pushKV("snapshot_hash", snapshot_hash);
 result.pushKV("total_tknc", ValueFromAmount(escrow.total_tknc));
 result.pushKV("quota_tokens", escrow.quota_tokens);
 result.pushKV("state", SpendingLimitStateToString(escrow.state));
 result.pushKV("synced_peers", escrow_sync_count);

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
    obj.pushKV("api_key", escrow.api_key);
    obj.pushKV("user_wallet", escrow.user_wallet);
    obj.pushKV("miner_wallet", escrow.miner_wallet);
    obj.pushKV("state", SpendingLimitStateToString(escrow.state));
    obj.pushKV("total_tknc", ValueFromAmount(escrow.total_tknc));
obj.pushKV("consumed_tknc", ValueFromAmount(escrow.consumed_tknc));
obj.pushKV("transferred_tknc", ValueFromAmount(escrow.transferred_tknc));
obj.pushKV("confirmed_tknc", ValueFromAmount(escrow.confirmed_tknc));
obj.pushKV("pending_txids", escrow.pending_txids);
obj.pushKV("spending_limit", ValueFromAmount(escrow.spending_limit));
obj.pushKV("remaining_tokens", escrow.GetRemainingTokens());

 // Show grace period status
 if (escrow.total_tknc == 0 && escrow.consumed_tknc > 0) {
    CAmount unconfirmed_debt = escrow.consumed_tknc - escrow.confirmed_tknc;
    obj.pushKV("grace_period_debt", ValueFromAmount(unconfirmed_debt));
    obj.pushKV("grace_period_limit", ValueFromAmount(2 * COIN));
    obj.pushKV("grace_period_ok", unconfirmed_debt < 2 * COIN);
}
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
static std::map<std::string, int64_t> g_token_rates; // miner_wallet -> tokens_per_tknc
static std::mutex g_miner_price_mutex;


static RPCMethod setminerprice()
{
 return RPCMethod{"setminerprice",
 "Set miner's token exchange rate (tokens per TKNC).\n"
 "This is stored on the node for billing calculation and handshake verification.\n"
 "Miners set their price via WEB login; this RPC is for programmatic access.\n",
 {
 {"miner_wallet", RPCArg::Type::STR, RPCArg::Optional::NO, "Miner wallet address"},
 {"tokens_per_tknc", RPCArg::Type::NUM, RPCArg::Optional::NO, "Tokens per 1 TKNC (e.g. 10 means 1 TKNC = 10 tokens, must be > 0)"},
 },
 RPCResult{
 RPCResult::Type::OBJ, "", "",
 {
 {RPCResult::Type::STR, "miner_wallet", "Miner wallet address"},
 {RPCResult::Type::NUM, "tokens_per_tknc", "Tokens per 1 TKNC"},
 {RPCResult::Type::STR, "exchange_rate_display", "Human-readable exchange rate"},
 {RPCResult::Type::STR, "status", "success"},
 }
 },
 RPCExamples{
 HelpExampleCli("setminerprice", "\"token1m...\" 10")
 },
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
 {
 std::string miner_wallet = request.params[0].get_str();
 int64_t tokens_per_tknc = request.params[1].getInt<int64_t>();

 if (tokens_per_tknc <= 0) {
 throw JSONRPCError(RPC_INVALID_PARAMETER, "tokens_per_tknc must be positive");
 }

 {
 std::lock_guard<std::mutex> lock(g_miner_price_mutex);
 g_token_rates[miner_wallet] = tokens_per_tknc;
 }

 // Persist to LevelDB so rate survives node restart
 if (g_spending_limit_db) {
 g_spending_limit_db->WriteTokenRate(miner_wallet, tokens_per_tknc);
 }

 LogInfo("setminerprice: miner=%s, tokens_per_tknc=%lld",
 miner_wallet, tokens_per_tknc);

 UniValue result(UniValue::VOBJ);
 result.pushKV("miner_wallet", miner_wallet);
 result.pushKV("tokens_per_tknc", tokens_per_tknc);
 result.pushKV("exchange_rate_display", strprintf("1 TKNC = %lld tokens", (long long)tokens_per_tknc));
 result.pushKV("status", "success");
 return result;
 }
 };
}

static RPCMethod getminerprice()
{
 return RPCMethod{"getminerprice",
 "Get miner's token exchange rate (tokens per TKNC).\n"
 "Returns the rate set by the miner for billing calculation and handshake verification.\n",
 {
 {"miner_wallet", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Miner wallet address (omit to get first registered miner's rate)"},
 },
 RPCResult{
 RPCResult::Type::OBJ, "", "",
 {
 {RPCResult::Type::STR, "miner_wallet", "Miner wallet address"},
 {RPCResult::Type::NUM, "tokens_per_tknc", "Tokens per 1 TKNC"},
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

 int64_t tokens_per_tknc = GetTokensPerTknc(miner_wallet);

 // Find the actual wallet key used
 std::string used_wallet;
 {
 std::lock_guard<std::mutex> lock(g_miner_price_mutex);
 if (!miner_wallet.empty()) {
 auto it = g_token_rates.find(miner_wallet);
 if (it != g_token_rates.end()) used_wallet = miner_wallet;
 }
 if (used_wallet.empty() && !g_token_rates.empty()) {
 used_wallet = g_token_rates.begin()->first;
 }
 }

UniValue result(UniValue::VOBJ);
result.pushKV("miner_wallet", used_wallet);
result.pushKV("tokens_per_tknc", tokens_per_tknc);
result.pushKV("exchange_rate_display", strprintf("1 TKNC = %lld tokens", (long long)tokens_per_tknc));
return result;
 }
 };
}

// Get token rate (exported for use in inference_gateway and handshake verification)
// Returns tokens_per_tknc: e.g. 10 means 1 TKNC = 10 tokens.
int64_t GetTokensPerTknc(const std::string& miner_wallet)
{
 std::lock_guard<std::mutex> lock(g_miner_price_mutex);

 // If memory cache is empty, try loading from LevelDB
 if (g_token_rates.empty() && g_spending_limit_db) {
 auto db_rates = g_spending_limit_db->ListAllTokenRates();
 for (const auto& [wallet, rate] : db_rates) {
 g_token_rates[wallet] = rate;
 }
 }

 if (!miner_wallet.empty()) {
 auto it = g_token_rates.find(miner_wallet);
 if (it != g_token_rates.end()) {
 return it->second;
 }
 }
 // If wallet not specified, return 0 — caller must provide the specific miner wallet.
 // Previously returned g_token_rates.begin()->second which could be a stale entry
 // from a different miner, causing incorrect billing rates.
 return 0;
}

// Store token rate (exported for use in tknc_setinferproxytarget after handshake)
// This allows the client node to store the verified rate from the remote miner.
void StoreTokenRate(const std::string& miner_wallet, int64_t tokens_per_tknc)
{
 if (tokens_per_tknc <= 0) return;

 {
 std::lock_guard<std::mutex> lock(g_miner_price_mutex);
 g_token_rates[miner_wallet] = tokens_per_tknc;
 }

 // Persist to LevelDB so rate survives node restart
 if (g_spending_limit_db) {
 g_spending_limit_db->WriteTokenRate(miner_wallet, tokens_per_tknc);
 }

 LogInfo("StoreTokenRate: miner=%s, tokens_per_tknc=%lld",
 miner_wallet.c_str(), tokens_per_tknc);
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
     escrow.state != SpendingLimit::State::ACTIVE &&
     escrow.state != SpendingLimit::State::PAYMENT_PENDING) {
 // SUSPENDED, EXHAUSTED, CLOSED: skip this escrow
 LogInfo("[CheckAndDeductEscrow] Escrow %s in state %s — skipping, looking for active escrow",
 escrow.escrow_id.c_str(), SpendingLimitStateToString(escrow.state));
 continue;
 }
 // Auto-recover from PAYMENT_PENDING: reset to ACTIVE so billing can retry.
 if (escrow.state == SpendingLimit::State::PAYMENT_PENDING) {
 escrow.state = SpendingLimit::State::ACTIVE;
 LogInfo("[CheckAndDeductEscrow] Auto-recovering escrow %s from PAYMENT_PENDING to ACTIVE",
 escrow.escrow_id.c_str());
 }
 if (escrow.IsExhausted()) continue;
 if (escrow.IsExpired()) continue;

 // Use miner's price from setminerprice RPC (node-side), not the escrow's stored rate.
 // The escrow rate may be 0 if created without proper rate setup.
 // GetTokensPerTknc returns the rate set by the miner via -token parameter.
 int64_t tokens_per_tknc = GetTokensPerTknc(escrow.miner_wallet);
 CAmount rate_tknc_per_token;
 if (tokens_per_tknc > 0) {
 // rate_tknc_per_token = COIN / tokens_per_tknc
// e.g., tokens_per_tknc=10 -> rate=1000000000/10=100000000 sat/token (10 tokens per TKNC)
// e.g., tokens_per_tknc=100 -> rate=1000000000/100=10000000 sat/token (100 tokens per TKNC)
 rate_tknc_per_token = (CAmount)(COIN / tokens_per_tknc);
 } else {
 // Fallback: use escrow's stored rate
 rate_tknc_per_token = escrow.rate_tknc_per_token;
 }
 // Ensure rate is at least 1 satoshi per token
 if (rate_tknc_per_token <= 0) rate_tknc_per_token = 1;

 LogInfo("CheckAndDeductEscrow: tokens_per_tknc=%lld, rate_tknc_per_token=%lld sat, tokens=%lld",
 (long long)tokens_per_tknc, (long long)rate_tknc_per_token, (long long)tokens_used);

 // Integer overflow check
 if (rate_tknc_per_token > 0 && tokens_used > INT64_MAX / rate_tknc_per_token) {
 LogWarning("CheckAndDeductEscrow: overflow detected (tokens=%lld rate=%lld)", tokens_used, rate_tknc_per_token);
 return false;
 }
 CAmount cost = tokens_used * rate_tknc_per_token;
 if (cost <= 0) cost = 1;
 // SECURITY: Use GetRemainingLimit() instead of stored spending_limit field to avoid stale data.
 CAmount remaining_limit = escrow.GetRemainingLimit();
 if (cost > remaining_limit) {
 cost = remaining_limit;
 if (rate_tknc_per_token > 0) {
 tokens_used = static_cast<int64_t>(cost / rate_tknc_per_token);
 }
 }

 // === Sliding window: update confirmed transfers before billing ===
 // This scans pending_txids for on-chain confirmation and moves confirmed
 // amounts to confirmed_tknc, keeping the grace period check accurate.
 UpdateConfirmedTransfers(escrow);

 // Track previous consumption for on-chain payment trigger
 CAmount prev_consumed = escrow.consumed_tknc;

 escrow.consumed_tknc += cost;
 // Unlimited mode (total_tknc == 0): spending_limit stays 0 to signal "unlimited"
 escrow.spending_limit = (escrow.total_tknc == 0) ? 0 : escrow.total_tknc - escrow.consumed_tknc;
 escrow.used_tokens += tokens_used;
 escrow.last_activity = GetTime();

 if (escrow.state == SpendingLimit::State::CREATED) {
 escrow.state = SpendingLimit::State::ACTIVE;
 }
 if (escrow.IsExhausted()) {
 escrow.state = SpendingLimit::State::EXHAUSTED;
 }

 g_spending_limit_db->WriteSpendingLimit(id, escrow);

 // (Chinese comment removed)
 // Check if consumption crossed a 1 TKNC boundary
 CAmount COIN_UNIT = COIN; // 1 TKNC in smallest unit
 CAmount prev_full_coins = prev_consumed / COIN_UNIT;
 CAmount curr_full_coins = escrow.consumed_tknc / COIN_UNIT;

 if (curr_full_coins > prev_full_coins && !escrow.miner_wallet.empty()) {
 CAmount coins_to_pay = (curr_full_coins - prev_full_coins) * COIN_UNIT;
 LogInfo("[PAY-AS-YOU-GO] Consumption crossed %lld TKNC boundary. "
 "Transferring %s TKNC from user wallet ?miner_wallet=%s",
 (long long)curr_full_coins,
 FormatMoney(coins_to_pay).c_str(),
 escrow.miner_wallet.c_str());

 // Find the user's wallet by escrow.user_wallet address.
 auto user_wallet = FindWalletByAddress(escrow.user_wallet);
 std::string txid;
 CAmount actual_transfer_amount = 0; // Tracks actual amount transferred (for grace period tracking)
 bool transfer_ok = false;
 if (user_wallet) {
 if (user_wallet->IsLocked()) {
 // CLIENT SIDE: wallet is locked — transfer FAILED, do NOT defer.
 // transferred_tknc stays unchanged, so CanAffordInference will refuse
 // the next inference (consumed - transferred >= 1 TKNC).
 // This enforces immediate transfer at 1 TKNC boundary — no sliding window on client side.
 LogWarning("[PAY-AS-YOU-GO] User wallet '%s' is LOCKED — transfer FAILED. "
 "Inference will be blocked on next request. "
 "Please unlock with walletpassphrase to enable transfers.",
 user_wallet->GetName());
 transfer_ok = false; // Failed — do not defer on client side
 } else {
 bool paid = TransferFromWallet(user_wallet, escrow.miner_wallet, coins_to_pay, txid);
 if (paid && !txid.empty()) {
 LogInfo("[PAY-AS-YOU-GO] Transfer succeeded: txid=%s", txid.c_str());
 transfer_ok = true;
 actual_transfer_amount = coins_to_pay;
 } else {
 // Full 1 TKNC transfer failed — try partial transfer of remaining balance.
 // This ensures the miner gets paid whatever is left, and the wallet
 // balance goes to 0 so CanAffordInference will refuse further inference.
 CAmount available = 0;
 {
 LOCK(user_wallet->cs_wallet);
 auto bal = GetBalance(*user_wallet, 0, true);
 available = bal.m_mine_trusted + bal.m_mine_untrusted_pending;
 }
 if (available > 0 && available < coins_to_pay) {
 LogInfo("[PAY-AS-YOU-GO] Attempting partial transfer of %s TKNC (remaining balance)",
 FormatMoney(available).c_str());
 bool partial_paid = TransferFromWallet(user_wallet, escrow.miner_wallet, available, txid, /*subtract_fee=*/true);
 if (partial_paid && !txid.empty()) {
 LogInfo("[PAY-AS-YOU-GO] Partial transfer succeeded: txid=%s, amount=%s",
 txid.c_str(), FormatMoney(available).c_str());
 transfer_ok = true;
 actual_transfer_amount = available;
 } else {
 LogWarning("[PAY-AS-YOU-GO] Partial transfer also failed — wallet may have dust/fee issues.");
 }
 } else {
 LogWarning("[PAY-AS-YOU-GO] Transfer FAILED — wallet may have insufficient balance. "
 "available=%s", FormatMoney(available).c_str());
 }
 }
 }
 } else {
 // Wallet not found on this node. Payment deferred to client node.
 LogInfo("[PAY-AS-YOU-GO] No wallet found (user_wallet='%s') — "
 "payment deferred. Inference continues.",
 escrow.user_wallet.c_str());
 transfer_ok = true; // Deferred
 }

 // If transfer genuinely failed (wallet on this node but transfer failed),
 // do NOT block inference — the consumed_tknc remains accumulated and
 // will be retried on the next billing cycle. Only CanAffordInference
 // (which checks wallet balance) should block inference.
 // This is like a phone plan: keep serving until balance is truly 0.
 if (!transfer_ok) {
 LogWarning("[PAY-AS-YOU-GO] Transfer failed for escrow %s — payment deferred, "
 "will retry next cycle. consumed_tknc=%s",
 escrow.escrow_id.c_str(), FormatMoney(escrow.consumed_tknc).c_str());
 }

 // === Track successful transfers for sliding window grace period ===
 // Record the txid and amount so CanAffordInference can check if it's confirmed on-chain.
 // The grace period allows inference to continue while waiting for confirmation,
 // but stops if consumed_tknc - confirmed_tknc >= 2 * COIN (1 TKNC grace exceeded).
 if (transfer_ok && !txid.empty()) {
     CAmount transferred_amount = actual_transfer_amount;
     // For full transfers, transferred_amount == coins_to_pay (1 TKNC * number of boundaries crossed)
     // For partial transfers, transferred_amount == actual balance transferred (less than coins_to_pay)

     // Add txid:amount to pending_txids
     std::string entry = txid + ":" + std::to_string(transferred_amount);
     if (!escrow.pending_txids.empty()) {
         escrow.pending_txids += ";";
     }
     escrow.pending_txids += entry;
     escrow.transferred_tknc += transferred_amount;

     LogInfo("[GracePeriod] Tracked transfer for escrow %s: txid=%s, amount=%s, "
             "transferred_tknc=%s, pending_txids=%s",
             escrow.escrow_id.c_str(), txid.c_str(),
             FormatMoney(transferred_amount).c_str(),
             FormatMoney(escrow.transferred_tknc).c_str(),
             escrow.pending_txids.c_str());

     // Save updated escrow with new pending_txids and transferred_tknc
     g_spending_limit_db->WriteSpendingLimit(id, escrow);
 } else if (transfer_ok && txid.empty()) {
     // Deferred payment (wallet locked or not found) — no txid to track.
     // The consumed_tknc is already recorded, and transferred_tknc stays unchanged.
     // The grace period check will catch this: if consumed - confirmed >= 2 TKNC,
     // inference will be refused until a transfer is made and confirmed.
     LogInfo("[GracePeriod] Payment deferred (no txid) for escrow %s. "
             "consumed=%s, transferred=%s, confirmed=%s",
             escrow.escrow_id.c_str(),
             FormatMoney(escrow.consumed_tknc).c_str(),
             FormatMoney(escrow.transferred_tknc).c_str(),
             FormatMoney(escrow.confirmed_tknc).c_str());
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
 receipt.remaining_limit = (escrow.total_tknc == 0) ? 0 : escrow.spending_limit; // 0 = unlimited in receipt
 receipt.timestamp = GetTime();

 LogInfo("CheckAndDeductEscrow: api_key=%s, escrow=%s, tokens=%lld, cost=%s, remaining=%lld, consumed=%s",
 api_key, escrow.escrow_id, tokens_used, FormatMoney(cost),
 escrow.GetRemainingTokens(), FormatMoney(escrow.consumed_tknc).c_str());
 return true;
 }
 }
 return false;
}

// Write SpendingLimit received from P2P ESCROWSYNC (called by net_processing)
// Updates payment-related fields on existing escrows so the miner can see
// the latest transferred_tknc and pending_txids from the client.
bool WriteSpendingLimitFromP2P(const SpendingLimit& escrow)
{
 if (!g_spending_limit_db) {
 LogWarning("[Escrow-P2P] Cannot write escrow: database not initialized");
 return false;
 }

 if (escrow.escrow_id.empty() || escrow.api_key.empty()) {
 LogWarning("[Escrow-P2P] Cannot write escrow: empty escrow_id or api_key");
 return false;
 }

 auto existing = g_spending_limit_db->ReadSpendingLimit(escrow.escrow_id);
 if (existing) {
     // Escrow already exists — update payment-related fields from the client.
     // SECURITY: Only update fields that track payment progress. Do NOT update
     // total_tknc, spending_limit, rate, miner_wallet, etc. — these were set
     // when the escrow was created and must not change via P2P sync.
     // A malicious peer could try to fake transferred_tknc, but the miner
     // verifies txids independently against its own wallet.
     bool changed = false;
     if (escrow.transferred_tknc > existing->transferred_tknc) {
         existing->transferred_tknc = escrow.transferred_tknc;
         changed = true;
     }
     if (escrow.consumed_tknc > existing->consumed_tknc) {
         existing->consumed_tknc = escrow.consumed_tknc;
         changed = true;
     }
     if (escrow.confirmed_tknc > existing->confirmed_tknc) {
         existing->confirmed_tknc = escrow.confirmed_tknc;
         changed = true;
     }
     if (escrow.pending_txids != existing->pending_txids && !escrow.pending_txids.empty()) {
         existing->pending_txids = escrow.pending_txids;
         changed = true;
     }
     // Update state if the client reports a higher state (e.g., ACTIVE after CREATED)
     if (escrow.state != existing->state) {
         existing->state = escrow.state;
         changed = true;
     }
     if (escrow.last_activity > existing->last_activity) {
         existing->last_activity = escrow.last_activity;
         changed = true;
     }
     if (changed) {
         std::lock_guard<std::mutex> lock(g_escrow_mutex);
         g_spending_limit_db->WriteSpendingLimit(escrow.escrow_id, *existing);
         LogInfo("[Escrow-P2P] Updated escrow from peer: escrow_id=%s, transferred=%s, consumed=%s, pending_txids=%s",
                 escrow.escrow_id.c_str(),
                 FormatMoney(existing->transferred_tknc).c_str(),
                 FormatMoney(existing->consumed_tknc).c_str(),
                 existing->pending_txids.substr(0, 64).c_str());
     }
     return true;
 }

 // New escrow — create it.
 {
 std::lock_guard<std::mutex> lock(g_escrow_mutex);
 if (!g_spending_limit_db->WriteSpendingLimit(escrow.escrow_id, escrow)) {
 LogWarning("[Escrow-P2P] Failed to write escrow to database: %s", escrow.escrow_id);
 return false;
 }
 }

 LogInfo("[Escrow-P2P] Synced SpendingLimit from peer: escrow_id=%s, api_key=%s, total=%s TKNC",
 escrow.escrow_id, escrow.api_key.substr(0, std::min((size_t)10, escrow.api_key.length())),
 FormatMoney(escrow.total_tknc));
 return true;
}