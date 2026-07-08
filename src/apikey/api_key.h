#ifndef TKN_APIKEY_API_KEY_H
#define TKN_APIKEY_API_KEY_H

#include <consensus/amount.h>
#include <serialize.h>
#include <string>
#include <vector>
#include <cstdint>

// API Key configuration (TKNC system)
inline constexpr const char* API_KEY_PREFIX = "tknc_";  // TKNC API Key prefix
inline constexpr size_t API_KEY_RANDOM_BYTES = 16;
inline constexpr size_t API_KEY_TOTAL_LENGTH = 37;

// API Key structure
struct APIKey {
    std::string key;              // tknc_[32-char random hex]
    std::string miner_id;         // Miner node ID (Peer ID)
    int64_t balance;              // Prepaid balance (TKNC)
    int64_t locked_balance;       // Locked balance (pre-locked during inference)
    int64_t total_tokens_used;    // Total tokens used
    int64_t expiry_time;          // Expiry time (Unix timestamp)
    std::string model_name;       // Available model name
    std::string payment_tx_hash;  // Payment tx hash (on-chain payment proof at creation)
    bool active;                  // Active status

    APIKey() : balance(0), locked_balance(0), total_tokens_used(0), expiry_time(0), active(false) {}

    SERIALIZE_METHODS(APIKey, obj)
    {
        READWRITE(obj.key, obj.miner_id, obj.balance, obj.locked_balance, obj.total_tokens_used, obj.expiry_time, obj.model_name, obj.payment_tx_hash, obj.active);
    }
};

// Generate random API Key
std::string GenerateAPIKey();

// Validate API Key format
bool ValidateAPIKeyFormat(const std::string& key);

// Parse API Key (returns miner_id)
std::string ParseAPIKeyMinerID(const std::string& key);

// Write API Key received from P2P sync (called by net_processing when APIKEYSYNC message received)
// Returns true if written successfully, false if key already exists or format invalid
bool WriteAPIKeyFromP2P(const APIKey& key_data);

#endif // TKN_APIKEY_API_KEY_H
