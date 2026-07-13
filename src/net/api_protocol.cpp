#include <net/api_protocol.h>
#include <apikey/api_key.h>
#include <hash.h>
#include <util/log.h>
#include <serialize.h>
#include <span.h>

#include <cstring>
#include <algorithm>

std::vector<uint8_t> APIRequest::Serialize() const {
    std::vector<uint8_t> data;
    
    size_t pos = 0;
    
    size_t key_len = api_key.size();
    data.resize(data.size() + sizeof(size_t) + key_len);
    std::memcpy(data.data() + pos, &key_len, sizeof(size_t));
    pos += sizeof(size_t);
    std::memcpy(data.data() + pos, api_key.data(), key_len);
    pos += key_len;
    
    size_t model_len = model.size();
    data.resize(data.size() + sizeof(size_t) + model_len);
    std::memcpy(data.data() + pos, &model_len, sizeof(size_t));
    pos += sizeof(size_t);
    std::memcpy(data.data() + pos, model.data(), model_len);
    pos += model_len;
    
    size_t model_hash_len = model_hash.size();
    data.resize(data.size() + sizeof(size_t) + model_hash_len);
    std::memcpy(data.data() + pos, &model_hash_len, sizeof(size_t));
    pos += sizeof(size_t);
    std::memcpy(data.data() + pos, model_hash.data(), model_hash_len);
    pos += model_hash_len;
    
    uint32_t msg_count = messages.size();
    data.resize(data.size() + sizeof(uint32_t));
    std::memcpy(data.data() + pos, &msg_count, sizeof(uint32_t));
    pos += sizeof(uint32_t);
    
    for (const auto& msg : messages) {
        size_t role_len = msg.role.size();
        data.resize(data.size() + sizeof(size_t) + role_len);
        std::memcpy(data.data() + pos, &role_len, sizeof(size_t));
        pos += sizeof(size_t);
        std::memcpy(data.data() + pos, msg.role.data(), role_len);
        pos += role_len;
        
        size_t content_len = msg.content.size();
        data.resize(data.size() + sizeof(size_t) + content_len);
        std::memcpy(data.data() + pos, &content_len, sizeof(size_t));
        pos += sizeof(size_t);
        std::memcpy(data.data() + pos, msg.content.data(), content_len);
        pos += content_len;
    }
    
    data.resize(data.size() + sizeof(uint64_t));
    std::memcpy(data.data() + pos, &nonce, sizeof(uint64_t));
    pos += sizeof(uint64_t);
    
    data.resize(data.size() + sizeof(uint64_t));
    std::memcpy(data.data() + pos, &request_id, sizeof(uint64_t));
    pos += sizeof(uint64_t);
    
    size_t sig_len = signature.size();
    data.resize(data.size() + sizeof(size_t) + sig_len);
    std::memcpy(data.data() + pos, &sig_len, sizeof(size_t));
    pos += sizeof(size_t);
    std::memcpy(data.data() + pos, signature.data(), sig_len);
    
    // max_tokens (appended for forward compatibility — old receivers ignore trailing bytes)
    data.resize(data.size() + sizeof(int));
    std::memcpy(data.data() + pos, &max_tokens, sizeof(int));
    
    return data;
}

bool APIRequest::Deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < sizeof(size_t) * 3 + sizeof(uint32_t) + sizeof(uint64_t)) {
        LogInfo("APIRequest: Data too short to deserialize");
        return false;
    }
    
    size_t pos = 0;
    
    size_t key_len = 0;
    std::memcpy(&key_len, data.data() + pos, sizeof(size_t));
    pos += sizeof(size_t);
    if (pos + key_len > data.size()) return false;
    api_key.assign(data.begin() + pos, data.begin() + pos + key_len);
    pos += key_len;
    
    size_t model_len = 0;
    std::memcpy(&model_len, data.data() + pos, sizeof(size_t));
    pos += sizeof(size_t);
    if (pos + model_len > data.size()) return false;
    model.assign(data.begin() + pos, data.begin() + pos + model_len);
    pos += model_len;
    
    size_t model_hash_len = 0;
    std::memcpy(&model_hash_len, data.data() + pos, sizeof(size_t));
    pos += sizeof(size_t);
    if (pos + model_hash_len > data.size()) return false;
    model_hash.assign(data.begin() + pos, data.begin() + pos + model_hash_len);
    pos += model_hash_len;
    
    uint32_t msg_count = 0;
    std::memcpy(&msg_count, data.data() + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t);
    
    messages.clear();
    for (uint32_t i = 0; i < msg_count; ++i) {
        ChatMessage msg;
        
        size_t role_len = 0;
        std::memcpy(&role_len, data.data() + pos, sizeof(size_t));
        pos += sizeof(size_t);
        if (pos + role_len > data.size()) return false;
        msg.role.assign(data.begin() + pos, data.begin() + pos + role_len);
        pos += role_len;
        
        size_t content_len = 0;
        std::memcpy(&content_len, data.data() + pos, sizeof(size_t));
        pos += sizeof(size_t);
        if (pos + content_len > data.size()) return false;
        msg.content.assign(data.begin() + pos, data.begin() + pos + content_len);
        pos += content_len;
        
        messages.push_back(msg);
    }
    
    std::memcpy(&nonce, data.data() + pos, sizeof(uint64_t));
    pos += sizeof(uint64_t);
    
    std::memcpy(&request_id, data.data() + pos, sizeof(uint64_t));
    pos += sizeof(uint64_t);
    
    size_t sig_len = 0;
    std::memcpy(&sig_len, data.data() + pos, sizeof(size_t));
    pos += sizeof(size_t);
    if (pos + sig_len > data.size()) return false;
    signature.assign(data.begin() + pos, data.begin() + pos + sig_len);
    pos += sig_len;
    
    // max_tokens (backward compatible: old senders don't have this field)
    max_tokens = 0;  // default: not specified
    if (pos + sizeof(int) <= data.size()) {
        std::memcpy(&max_tokens, data.data() + pos, sizeof(int));
    }
    
    return true;
}

std::vector<uint8_t> APIResponse::Serialize() const {
    std::vector<uint8_t> data;
    
    size_t pos = 0;
    
    size_t content_len = content.size();
    data.resize(data.size() + sizeof(size_t) + content_len);
    std::memcpy(data.data() + pos, &content_len, sizeof(size_t));
    pos += sizeof(size_t);
    std::memcpy(data.data() + pos, content.data(), content_len);
    pos += content_len;
    
    data.resize(data.size() + sizeof(int));
    std::memcpy(data.data() + pos, &prompt_tokens, sizeof(int));
    pos += sizeof(int);

    data.resize(data.size() + sizeof(int));
    std::memcpy(data.data() + pos, &completion_tokens, sizeof(int));
    pos += sizeof(int);

    data.resize(data.size() + sizeof(int));
    std::memcpy(data.data() + pos, &tokens_used, sizeof(int));
    pos += sizeof(int);

    data.resize(data.size() + sizeof(int64_t));
    std::memcpy(data.data() + pos, &cost, sizeof(int64_t));
    pos += sizeof(int64_t);
    
    data.resize(data.size() + sizeof(uint64_t));
    std::memcpy(data.data() + pos, &request_id, sizeof(uint64_t));
    pos += sizeof(uint64_t);
    
    size_t sig_len = signature.size();
    data.resize(data.size() + sizeof(size_t) + sig_len);
    std::memcpy(data.data() + pos, &sig_len, sizeof(size_t));
    pos += sizeof(size_t);
    std::memcpy(data.data() + pos, signature.data(), sig_len);
    
    return data;
}

bool APIResponse::Deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < sizeof(size_t) + sizeof(int) * 3 + sizeof(int64_t)) {
        LogInfo("APIResponse: Data too short to deserialize");
        return false;
    }

    size_t pos = 0;

    size_t content_len = 0;
    std::memcpy(&content_len, data.data() + pos, sizeof(size_t));
    pos += sizeof(size_t);
    if (pos + content_len > data.size()) return false;
    content.assign(data.begin() + pos, data.begin() + pos + content_len);
    pos += content_len;

    // Backward-compatible format: supports both old (tokens_used) and new (prompt+completion) layouts.
    size_t remaining = data.size() - pos;
    if (remaining >= sizeof(int) * 3 + sizeof(int64_t) + sizeof(uint64_t) + sizeof(size_t)) {
        // New format with prompt_tokens + completion_tokens
        std::memcpy(&prompt_tokens, data.data() + pos, sizeof(int));
        pos += sizeof(int);
        std::memcpy(&completion_tokens, data.data() + pos, sizeof(int));
        pos += sizeof(int);
        std::memcpy(&tokens_used, data.data() + pos, sizeof(int));
        pos += sizeof(int);
    } else if (remaining >= sizeof(int) + sizeof(int64_t) + sizeof(uint64_t) + sizeof(size_t)) {
        // Old format: only tokens_used
        prompt_tokens = 0;
        completion_tokens = 0;
        std::memcpy(&tokens_used, data.data() + pos, sizeof(int));
        pos += sizeof(int);
    } else {
        return false;
    }

    std::memcpy(&cost, data.data() + pos, sizeof(int64_t));
    pos += sizeof(int64_t);
    
    std::memcpy(&request_id, data.data() + pos, sizeof(uint64_t));
    pos += sizeof(uint64_t);
    
    size_t sig_len = 0;
    std::memcpy(&sig_len, data.data() + pos, sizeof(size_t));
    pos += sizeof(size_t);
    if (pos + sig_len > data.size()) return false;
    signature.assign(data.begin() + pos, data.begin() + pos + sig_len);
    
    return true;
}

bool ValidateAPIRequest(const APIRequest& request) {
    bool has_api_key = !request.api_key.empty();
    
    if (has_api_key && !ValidateAPIKeyFormat(request.api_key)) {
        LogInfo("APIProtocol: Invalid API Key format");
        return false;
    }
    
    if (request.model.empty()) {
        LogInfo("APIProtocol: Model name is empty");
        return false;
    }
    
    // model_hash is optional, but if provided must be 64 hex characters (SHA256)
    if (!request.model_hash.empty()) {
        if (request.model_hash.size() != 64) {
            LogInfo("APIProtocol: model_hash must be 64 hex characters");
            return false;
        }
        for (char c : request.model_hash) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                LogInfo("APIProtocol: model_hash contains invalid hex characters");
                return false;
            }
        }
    }
    
    if (request.messages.empty()) {
        LogInfo("APIProtocol: Message list is empty");
        return false;
    }
    
    for (const auto& msg : request.messages) {
        if (msg.role.empty() || msg.content.empty()) {
            LogInfo("APIProtocol: Invalid message format");
            return false;
        }
    }
    
    // Client-side billing model: no signature validation, billing handled by payer.
    
    return true;
}
