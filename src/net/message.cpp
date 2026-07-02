#include <net/message.h>
#include <hash.h>
#include <util/log.h>
#include <serialize.h>
#include <span.h>

#include <cstring>
#include <algorithm>

void P2PMessage::CalculateChecksum() {
    if (payload.empty()) {
        std::memset(checksum, 0, sizeof(checksum));
        return;
    }
    
    uint256 hash = Hash(payload);
    std::memcpy(checksum, hash.begin(), 4);
}

bool P2PMessage::VerifyChecksum() const {
    if (payload.empty()) {
        uint8_t empty_checksum[4] = {0, 0, 0, 0};
        return std::memcmp(checksum, empty_checksum, 4) == 0;
    }
    
    uint256 hash = Hash(payload);
    uint8_t expected[4];
    std::memcpy(expected, hash.begin(), 4);
    
    return std::memcmp(checksum, expected, 4) == 0;
}

std::vector<uint8_t> P2PMessage::Serialize() const {
    std::vector<uint8_t> data;
    data.reserve(24 + payload.size());
    
    const uint8_t* magic_bytes = reinterpret_cast<const uint8_t*>(&magic);
    data.insert(data.end(), magic_bytes, magic_bytes + 4);
    
    data.insert(data.end(), command, command + 12);
    
    const uint8_t* length_bytes = reinterpret_cast<const uint8_t*>(&length);
    data.insert(data.end(), length_bytes, length_bytes + 4);
    
    data.insert(data.end(), checksum, checksum + 4);
    
    data.insert(data.end(), payload.begin(), payload.end());
    
    return data;
}

bool P2PMessage::Deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 24) {
        LogInfo("P2PMessage: Data too short to deserialize (%zu bytes)", data.size());
        return false;
    }
    
    std::memcpy(&magic, data.data(), 4);
    if (magic != TKN_NETWORK_MAGIC) {
        LogInfo("P2PMessage: Magic number mismatch (0x%x != 0x%x)", magic, TKN_NETWORK_MAGIC);
        return false;
    }
    
    std::memcpy(command, data.data() + 4, 12);
    std::memcpy(&length, data.data() + 16, 4);
    std::memcpy(checksum, data.data() + 20, 4);
    
    if (data.size() < 24 + length) {
        LogInfo("P2PMessage: Insufficient data length (need %u bytes, got %zu bytes)", 
                  length, data.size() - 24);
        return false;
    }
    
    payload.assign(data.begin() + 24, data.begin() + 24 + length);
    
    if (!VerifyChecksum()) {
        LogInfo("P2PMessage: Checksum verification failed");
        return false;
    }
    
    return true;
}
