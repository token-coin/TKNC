#ifndef TKN_NET_MESSAGE_H
#define TKN_NET_MESSAGE_H

#include <cstdint>
#include <vector>
#include <string>
#include <cstring>

inline constexpr uint32_t TKN_NETWORK_MAGIC = 0x4e414200;

struct P2PMessage {
    uint32_t magic;
    char command[12];
    uint32_t length;
    uint8_t checksum[4];
    std::vector<uint8_t> payload;

    P2PMessage() : magic(TKN_NETWORK_MAGIC), length(0) {
        std::memset(command, 0, sizeof(command));
        std::memset(checksum, 0, sizeof(checksum));
    }

    void SetCommand(const std::string& cmd) {
        std::memset(command, 0, sizeof(command));
        std::strncpy(command, cmd.c_str(), sizeof(command) - 1);
    }

    void CalculateChecksum();
    bool VerifyChecksum() const;
    std::vector<uint8_t> Serialize() const;
    bool Deserialize(const std::vector<uint8_t>& data);
};

namespace MessageTypes {
    inline constexpr const char* VERSION = "version";
    inline constexpr const char* VERACK = "verack";
    inline constexpr const char* ADDR = "addr";
    inline constexpr const char* GETDATA = "getdata";
    inline constexpr const char* BLOCK = "block";
    inline constexpr const char* TX = "tx";
    inline constexpr const char* APIREQ = "apireq";
    inline constexpr const char* APIRESP = "apiresp";
    inline constexpr const char* FINDNODE = "findnode";
    inline constexpr const char* NODES = "nodes";
    inline constexpr const char* PING = "ping";
    inline constexpr const char* PONG = "pong";

    // Miner Explorer Protocol - New message types for miner info synchronization
    inline constexpr const char* MINER_INFO = "minerinfo";      // Broadcast miner status
    inline constexpr const char* GET_MINERS = "getminers";       // Request miner list
    inline constexpr const char* MINER_LIST = "minerlist";       // Response with miner list
    inline constexpr const char* MINER_STATS = "minerstats";     // Real-time statistics
    inline constexpr const char* REVIEW_BCAST = "reviewbcast";   // Broadcast new review
    inline constexpr const char* LIKE_BCAST = "likebcast";       // Broadcast like event
    inline constexpr const char* APIKEYSYNC = "apikeysync";      // Broadcast API Key sync (created via Web payment)
}

#endif // TKN_NET_MESSAGE_H
