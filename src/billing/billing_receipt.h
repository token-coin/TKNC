#ifndef TKN_BILLING_BILLING_RECEIPT_H
#define TKN_BILLING_BILLING_RECEIPT_H

#include <consensus/amount.h>
#include <string>
#include <vector>
#include <cstdint>

// Verifiable billing receipt for pay-as-you-go inference
// Every 1 TKNC consumed triggers an on-chain transfer to miner wallet
// No lockup, no escrow — receipt tracks consumption against spending limit
struct BillingReceipt {
    std::string api_key;             // API Key used
    std::string escrow_id;           // Associated spending limit ID (field name kept for backward-compatible serialization)
    std::string miner_wallet;        // Miner receiving payment
    std::string model_name;          // Model used

    int64_t input_tokens;            // Input tokens (prompt)
    int64_t output_tokens;           // Output tokens (generated)
    int64_t total_tokens;            // Total tokens this inference

    CAmount cost_tknc;               // Cost in smallest TKNC unit
    CAmount consumed_tknc;           // Cumulative TKNC consumed after this
    CAmount remaining_limit;         // Remaining spending limit after this

    int64_t timestamp;               // Timestamp of billing event
    uint64_t request_id;             // P2P request tracking ID
    uint64_t nonce;                  // Anti-replay nonce

    std::vector<uint8_t> node_signature; // HMAC-SHA256d MAC (32 bytes)

    BillingReceipt() : input_tokens(0), output_tokens(0), total_tokens(0),
                       cost_tknc(0), consumed_tknc(0), remaining_limit(0),
                       timestamp(0), request_id(0), nonce(0) {}
};

#endif // TKN_BILLING_BILLING_RECEIPT_H