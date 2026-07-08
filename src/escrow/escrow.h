#ifndef TKN_ESCROW_ESCROW_H
#define TKN_ESCROW_ESCROW_H

#include <consensus/amount.h>
#include <serialize.h>
#include <string>
#include <cstdint>
#include <vector>

// Spending limit tracking for API Key consumption
// API Key = spending limit credential (not lockup/escrow)
// Every 1 TKNC consumed → automatic on-chain transfer to miner wallet
// No lockup, no staking, no refund — pay-as-you-go model
struct SpendingLimit {
    std::string escrow_id;           // Unique identifier (field name kept for backward-compatible serialization)
    std::string user_wallet;         // User wallet address (payer)
    std::string miner_wallet;        // Miner wallet address (payee)
    std::string model_name;          // Target model name
    std::string model_hash;          // SHA256 hash of model GGUF file
    std::string snapshot_hash;       // SHA256 of pricing snapshot
    std::string api_key;             // Associated API Key

    CAmount total_tknc;              // Spending limit (user-declared max consumption)
    CAmount consumed_tknc;           // TKNC already consumed (paid on-chain)
    CAmount spending_limit;          // Remaining spending limit (total - consumed)

    int64_t quota_tokens;            // Total token quota based on spending limit
    int64_t used_tokens;             // Tokens already consumed

    int64_t rate_tokens_per_tknc;    // Exchange rate: tokens per 1 TKNC
    int64_t rate_tknc_per_token;     // Exchange rate: TKNC per 1 token (smallest unit)

    int64_t created_at;              // Creation timestamp (Unix)
    int64_t expires_at;              // Expiry timestamp (Unix)
    int64_t last_activity;           // Last inference activity timestamp

    // Spending limit states
    enum class State : uint8_t {
        CREATED = 0,        // API Key created, no inference yet
        ACTIVE = 1,         // Active, inference ongoing
        SUSPENDED = 2,      // Miner offline, paused
        EXHAUSTED = 3,      // Spending limit reached
        CLOSED = 4,         // Closed normally or expired
        PAYMENT_PENDING = 5 // Transfer to miner failed — inference blocked until payment succeeds
    };
    State state;

    SpendingLimit() : total_tknc(0), consumed_tknc(0), spending_limit(0),
                      quota_tokens(0), used_tokens(0),
                      rate_tokens_per_tknc(0), rate_tknc_per_token(0),
                      created_at(0), expires_at(0), last_activity(0),
                      state(State::CREATED) {}

    // Computed properties
    // total_tknc == 0 means unlimited spending (pay-as-you-go, bounded only by wallet balance)
    CAmount GetRemainingLimit() const { return total_tknc == 0 ? INT64_MAX : total_tknc - consumed_tknc; }
    int64_t GetRemainingTokens() const { return total_tknc == 0 ? INT64_MAX : quota_tokens - used_tokens; }
    bool IsExhausted() const { return total_tknc == 0 ? false : (consumed_tknc >= total_tknc || used_tokens >= quota_tokens); }
    bool IsExpired() const;

    SERIALIZE_METHODS(SpendingLimit, obj)
    {
        READWRITE(obj.escrow_id, obj.user_wallet, obj.miner_wallet,
                  obj.model_name, obj.model_hash, obj.snapshot_hash,
                  obj.api_key,
                  obj.total_tknc, obj.consumed_tknc, obj.spending_limit,
                  obj.quota_tokens, obj.used_tokens,
                  obj.rate_tokens_per_tknc, obj.rate_tknc_per_token,
                  obj.created_at, obj.expires_at, obj.last_activity);
        uint8_t state_uint = static_cast<uint8_t>(obj.state);
        READWRITE(state_uint);
        if constexpr (!std::is_const_v<std::remove_reference_t<decltype(obj)>>) {
            obj.state = static_cast<SpendingLimit::State>(state_uint);
        }
    }
};

// Pricing snapshot — written to OP_RETURN as SHA256 hash
struct PricingSnapshot {
    int64_t rate_tknc_per_token;     // TKNC per token (smallest unit)
    int64_t rate_tokens_per_tknc;    // Tokens per TKNC
    int64_t quota_tokens;            // Total token quota
    CAmount total_tknc_paid;         // Total TKNC paid
    std::string miner_wallet;        // Miner wallet
    std::string user_wallet;         // User wallet
    std::string model_name;          // Model name
    std::string model_hash;          // Model hash
    int64_t timestamp;               // Snapshot timestamp
    int32_t version;                 // Snapshot version

    PricingSnapshot() : rate_tknc_per_token(0), rate_tokens_per_tknc(0),
                        quota_tokens(0), total_tknc_paid(0),
                        timestamp(0), version(1) {}

    // Serialize to JSON string for hashing
    std::string ToJSON() const;

    // Compute SHA256 hash of the snapshot
    std::string ComputeHash() const;

    // Validate snapshot integrity
    bool Validate() const;
};

// Generate a unique spending limit ID
std::string GenerateSpendingLimitID(const std::string& source_id);

// State to string conversion
const char* SpendingLimitStateToString(SpendingLimit::State state);

#endif // TKN_ESCROW_ESCROW_H