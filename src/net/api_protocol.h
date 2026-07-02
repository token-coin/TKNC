#ifndef TKN_NET_API_PROTOCOL_H
#define TKN_NET_API_PROTOCOL_H

#include <apikey/api_key.h>
#include <net/message.h>
#include <consensus/amount.h>
#include <string>
#include <vector>

// Chat message
struct ChatMessage {
    std::string role;    // "user", "assistant", "system"
    std::string content;
};

// API request structure
struct APIRequest {
    std::string api_key;                    // tkn_[32-char hex]
    std::string model;                      // Model name
    std::string model_hash;                 // SHA256 hash of model GGUF file
    std::vector<ChatMessage> messages;      // Chat history
    uint64_t nonce;                         // Anti-replay
    uint64_t request_id;                    // P2P request tracking ID
    std::vector<uint8_t> signature;         // Signature

    APIRequest() : nonce(0), request_id(0) {}

    // Serialize
    std::vector<uint8_t> Serialize() const;

    // Deserialize
    bool Deserialize(const std::vector<uint8_t>& data);
};

// API response structure
struct APIResponse {
    std::string content;                    // Model output
    int prompt_tokens;                      // Prompt tokens (from miner's LLM engine)
    int completion_tokens;                  // Completion tokens (from miner's LLM engine)
    int tokens_used;                        // Total tokens = prompt_tokens + completion_tokens (computed by node)
    int64_t cost;                           // Cost (Token)
    uint64_t request_id;                    // P2P request tracking ID (echo back)
    std::vector<uint8_t> signature;         // Signature

    APIResponse() : prompt_tokens(0), completion_tokens(0), tokens_used(0), cost(0), request_id(0) {}

    // Serialize
    std::vector<uint8_t> Serialize() const;

    // Deserialize
    bool Deserialize(const std::vector<uint8_t>& data);
};

// Validate API request
bool ValidateAPIRequest(const APIRequest& request);

#endif // TKN_NET_API_PROTOCOL_H
