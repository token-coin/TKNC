#include <apikey/api_key.h>
#include <random.h>
#include <util/strencodings.h>
#include <sstream>

// Generate random API Key
std::string GenerateAPIKey() {
    std::vector<uint8_t> random_bytes(API_KEY_RANDOM_BYTES);
    GetRandBytes(random_bytes);
    
    std::string hex_str = HexStr(random_bytes);
    return std::string(API_KEY_PREFIX) + hex_str;
}

// Validate API Key format
bool ValidateAPIKeyFormat(const std::string& key) {
    if (key.size() != API_KEY_TOTAL_LENGTH) {
        return false;
    }
    
    if (key.substr(0, 5) != API_KEY_PREFIX) {
        return false;
    }
    
    std::string hex_part = key.substr(5);
    for (char c : hex_part) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    
    return true;
}

// Parse API Key (returns miner_id)
std::string ParseAPIKeyMinerID(const std::string& key) {
    if (!ValidateAPIKeyFormat(key)) {
        return "";
    }
    
    return key.substr(5);
}
