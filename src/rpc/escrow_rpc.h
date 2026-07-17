#ifndef TKNC_ESCROW_RPC_H
#define TKNC_ESCROW_RPC_H

#include <escrow/escrow.h>
#include <billing/billing_receipt.h>
#include <optional>
#include <string>
#include <util/fs.h>
#include <wallet/context.h>
#include <wallet/wallet.h>

// Exported spending limit access for cross-module use (e.g., p2pinference)
std::optional<SpendingLimit> FindSpendingLimitByAPIKey(const std::string& api_key);
bool CheckAndDeductEscrow(const std::string& api_key, int64_t tokens_used, BillingReceipt& receipt);

// Pre-inference balance check. Returns true if user can afford inference.
// If false, error_msg contains the reason. Called before forwarding to miner.
// This function also considers pending (reserved) costs from concurrent in-flight requests.
bool CanAffordInference(const std::string& api_key, std::string& error_msg);

// Reserve estimated cost for an in-flight inference request.
// This prevents concurrent request flood attacks where many requests
// pass the balance check before any billing is applied.
// Returns the reserved amount (same as estimated_cost on success).
// The reserved amount is released by ReleaseEscrowCost after billing completes.
CAmount ReserveEscrowCost(const std::string& api_key, CAmount estimated_cost);

// Release a previously reserved cost after billing is complete (or on error).
// This must be called for every successful ReserveEscrowCost call.
void ReleaseEscrowCost(const std::string& api_key, CAmount reserved_cost);

// Get the total pending (reserved) cost for an API key.
CAmount GetPendingCost(const std::string& api_key);

// === Sliding window grace period billing ===
// Check and update confirmed transfer amounts for an escrow.
// Scans pending_txids for on-chain confirmations and moves confirmed amounts
// from "pending" to "confirmed_tknc". Returns the total confirmed_tknc.
// This implements the 1 TKNC grace period: inference continues as long as
// consumed_tknc - confirmed_tknc < 2 * COIN (1 TKNC of grace beyond current period).
void UpdateConfirmedTransfers(SpendingLimit& escrow);

// === Miner-side independent payment verification ===
// The miner is the ONLY reliable gatekeeper. Client-side checks are advisory —
// a malicious client can modify its own node code to skip billing entirely.
// The miner must independently verify it has been paid.
//
// The miner tracks served_tknc locally (via TrackMinerServedTokens) — this
// counter cannot be faked by the client.
//
// To verify payment, the miner checks txids (provided by the client via HTTP
// headers or from the P2P-synced escrow) against its OWN wallet. Only txids
// that actually appear in the miner's wallet are counted as received.
// A malicious client can fake the txid list, but cannot fake a transaction
// in the miner's wallet.
//
// Grace period: served_tknc - received_tknc < 2 * COIN → allow
//                served_tknc - received_tknc >= 2 * COIN → refuse
//
// pending_txids_override: from HTTP header X-TKNC-Pending-Txids (more current
//   than P2P-synced escrow data). If empty, falls back to escrow.pending_txids.
bool CheckMinerReceivedPayment(const std::string& api_key, std::string& error_msg,
                                const std::string& pending_txids_override = "");

// Track tokens served by this miner for an API key (called after successful inference).
// This updates the miner-side served_tknc counter used by CheckMinerReceivedPayment.
void TrackMinerServedTokens(const std::string& api_key, int64_t tokens_used);

// Auto-create escrow for an API key if one doesn't exist.
// Uses stored miner price and proxy target info to create a pay-as-you-go escrow.
// Returns true if escrow exists (or was created), false on failure.
bool EnsureEscrowForAPIKey(const std::string& api_key, const std::string& miner_wallet = "",
                            const std::string& user_wallet = "", const std::string& model_name = "");

// Write SpendingLimit received from P2P ESCROWSYNC (called by net_processing)
// Returns true if written successfully, false if key already exists or format invalid
bool WriteSpendingLimitFromP2P(const SpendingLimit& escrow);

// Token rate (node-side, replaces miner-side set_price)
// Returns tokens_per_tknc: e.g. 10 means 1 TKNC = 10 tokens.
int64_t GetTokensPerTknc(const std::string& miner_wallet);
void StoreTokenRate(const std::string& miner_wallet, int64_t tokens_per_tknc);

// Spending limit DB initialization
void InitEscrowDB(const fs::path& path);
bool HasEscrowDB();

// Wallet access for pay-as-you-go inference billing.
// Set once during init; allows CheckAndDeductEscrow to find the user's wallet
// by escrow.user_wallet address and transfer funds to miner.
void SetWalletContext(wallet::WalletContext* ctx);
std::shared_ptr<wallet::CWallet> FindWalletByAddress(const std::string& address);
bool TransferFromWallet(std::shared_ptr<wallet::CWallet> pwallet,
                        const std::string& to_address, CAmount amount, std::string& txid,
                        bool subtract_fee = false,
                        const std::string& from_address = "");

#endif