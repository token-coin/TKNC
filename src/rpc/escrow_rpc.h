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

// Write SpendingLimit received from P2P ESCROWSYNC (called by net_processing)
// Returns true if written successfully, false if key already exists or format invalid
bool WriteSpendingLimitFromP2P(const SpendingLimit& escrow);

// Miner pricing (node-side, replaces miner-side set_price)
int64_t GetMinerPrice(const std::string& miner_wallet);

// Spending limit DB initialization
void InitEscrowDB(const fs::path& path);
bool HasEscrowDB();

// Wallet access for pay-as-you-go inference billing.
// Set once during init; allows CheckAndDeductEscrow to find the user's wallet
// by escrow.user_wallet address and transfer funds to miner.
void SetWalletContext(wallet::WalletContext* ctx);
std::shared_ptr<wallet::CWallet> FindWalletByAddress(const std::string& address);
bool TransferFromWallet(std::shared_ptr<wallet::CWallet> pwallet,
                        const std::string& to_address, CAmount amount, std::string& txid);

#endif