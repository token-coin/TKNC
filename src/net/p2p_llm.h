#ifndef TKN_P2P_LLM_H
#define TKN_P2P_LLM_H

#include <string>
#include <cstdint>
#include <vector>
#include <functional>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
struct bufferevent;
#else
#include <sys/socket.h>
#include <event2/bufferevent.h>
#endif

inline constexpr uint32_t P2P_LLM_MAGIC = 0x544b4e4c;
inline constexpr size_t P2P_LLM_HEADER_SIZE = 10;

enum P2PLLMMessageType : uint16_t {
    P2P_LLM_INFERENCE_REQUEST = 1001,
    P2P_LLM_INFERENCE_TOKEN    = 1002,
    P2P_LLM_INFERENCE_ERROR    = 1003,
    P2P_LLM_INFERENCE_DONE     = 1004,
    P2P_LLM_API_KEY_VALIDATE  = 1005,
    P2P_LLM_HANDSHAKE_REQ     = 1006,  // Client node → Miner node: request handshake verification
    P2P_LLM_HANDSHAKE_RESP    = 1007   // Miner node → Client node: handshake response with verified pricing
};

struct P2PLLMInferenceRequest {
    std::string api_key;
    std::string system_prompt;
    std::string user_message;

    std::vector<uint8_t> Serialize() const;
    bool Deserialize(const std::vector<uint8_t>& data);
};

struct P2PLLMInferenceToken {
    std::string token_text;
    int token_index;

    P2PLLMInferenceToken() : token_index(0) {}

    std::vector<uint8_t> Serialize() const;
    bool Deserialize(const std::vector<uint8_t>& data);
};

struct P2PLLMInferenceError {
    int error_code;
    std::string error_message;

    P2PLLMInferenceError() : error_code(0) {}

    std::vector<uint8_t> Serialize() const;
    bool Deserialize(const std::vector<uint8_t>& data);
};

struct P2PLLMInferenceDone {
    std::string full_response;
    int total_tokens;

    P2PLLMInferenceDone() : total_tokens(0) {}

    std::vector<uint8_t> Serialize() const;
    bool Deserialize(const std::vector<uint8_t>& data);
};

// Handshake request: client node sends to miner node before inference
// Purpose: verify token counting accuracy + verify real exchange rate
struct P2PLLMHandshakeReq {
    std::string api_key;
    std::string model_name;
    std::string test_prompt;      // Short prompt for micro-inference verification
    int64_t web_price_per_1m;     // Price shown on WEB (for comparison)

    P2PLLMHandshakeReq() : web_price_per_1m(0) {}

    std::vector<uint8_t> Serialize() const;
    bool Deserialize(const std::vector<uint8_t>& data);
};

// Handshake response: miner node returns to client node
// Contains: test tokens + node-verified pricing + miner wallet
struct P2PLLMHandshakeResp {
    bool accepted;                // true = handshake passed, ready for inference
    std::string test_tokens;      // Output from micro-inference
    int node_token_count;         // Node's token count for test output
    int64_t verified_price_per_1m;// Node-verified real exchange rate (TKNC/1M tokens)
    int64_t tokens_per_tknc;      // Calculated: tokens per 1 TKNC
    std::string miner_wallet;     // Miner wallet address for payment
    std::string rejection_reason; // If rejected, why

    P2PLLMHandshakeResp() : accepted(false), node_token_count(0),
                             verified_price_per_1m(0), tokens_per_tknc(0) {}

    std::vector<uint8_t> Serialize() const;
    bool Deserialize(const std::vector<uint8_t>& data);
};

struct P2PLLMMessageHeader {
    uint32_t magic;
    uint16_t message_type;
    uint32_t payload_length;

    P2PLLMMessageHeader()
        : magic(P2P_LLM_MAGIC)
        , message_type(0)
        , payload_length(0) {}

    std::vector<uint8_t> Serialize() const;
    bool Deserialize(const std::vector<uint8_t>& data);
    bool IsValid() const;
};

bool SendLLMMessage(int socket_fd, P2PLLMMessageType msg_type, const std::vector<uint8_t>& payload);

bool SendInferenceRequest(int socket_fd, const P2PLLMInferenceRequest& req);
bool SendInferenceToken(int socket_fd, const P2PLLMInferenceToken& token);
bool SendInferenceError(int socket_fd, const P2PLLMInferenceError& error);
bool SendInferenceDone(int socket_fd, const P2PLLMInferenceDone& done);
bool SendHandshakeReq(int socket_fd, const P2PLLMHandshakeReq& req);
bool SendHandshakeResp(int socket_fd, const P2PLLMHandshakeResp& resp);

bool ParseLLMMessage(const std::vector<uint8_t>& raw_data,
                      P2PLLMMessageHeader& header,
                      std::vector<uint8_t>& payload);

using LLMTokenCallback = std::function<void(const std::string& token_text, int index)>;
using LLMDoneCallback = std::function<void(const std::string& full_response, int total_tokens)>;
using LLMErrorCallback = std::function<void(int error_code, const std::string& error_msg)>;

class P2PLLMPeerHandler {
public:
    void HandleIncomingMessage(const std::string& peer_id,
                                const std::vector<uint8_t>& message_data,
                                int socket_fd);

    void SetTokenCallback(LLMTokenCallback cb) { token_callback = cb; }
    void SetDoneCallback(LLMDoneCallback cb) { done_callback = cb; }
    void SetErrorCallback(LLMErrorCallback cb) { error_callback = cb; }

private:
    LLMTokenCallback token_callback;
    LLMDoneCallback done_callback;
    LLMErrorCallback error_callback;

    void ProcessInferenceRequest(const std::string& peer_id,
                                  const P2PLLMInferenceRequest& req,
                                  int socket_fd);
    void ProcessInferenceToken(const std::string& peer_id,
                                const P2PLLMInferenceToken& token);
    void ProcessInferenceDone(const std::string& peer_id,
                               const P2PLLMInferenceDone& done);
    void ProcessInferenceError(const std::string& peer_id,
                                const P2PLLMInferenceError& error);
    void ProcessHandshakeReq(const std::string& peer_id,
                              const P2PLLMHandshakeReq& req,
                              int socket_fd);
    void ProcessHandshakeResp(const std::string& peer_id,
                               const P2PLLMHandshakeResp& resp);
};

#endif // TKN_P2P_LLM_H
