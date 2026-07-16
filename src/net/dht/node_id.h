#ifndef TKN_NET_DHT_NODE_ID_H
#define TKN_NET_DHT_NODE_ID_H

#include <uint256.h>
#include <hash.h>
#include <pubkey.h>
#include <cstring>
#include <vector>

// (Chinese comment removed)
class NodeID {
private:
 uint8_t id[20];
 
public:
 NodeID() {
 std::memset(id, 0, sizeof(id));
 }
 
 // Construct from byte array
 explicit NodeID(const uint8_t* data) {
 std::memcpy(id, data, 20);
 }
 
 // Generate node ID from public key (SHA-256 hash first 20 bytes)
 static NodeID FromPublicKey(const CPubKey& pubkey) {
 std::vector<unsigned char> pubkey_data(pubkey.data(), pubkey.data() + pubkey.size());
 uint256 hash = Hash(pubkey_data);
 NodeID node_id;
 std::memcpy(node_id.id, hash.begin(), 20);
 return node_id;
 }
 
 // Parse from string (hex format)
 static NodeID FromString(const std::string& hex) {
 NodeID node_id;
 std::vector<unsigned char> data = ParseHex(hex);
 if (data.size() >= 20) {
 std::memcpy(node_id.id, data.data(), 20);
 }
 return node_id;
 }
 
 // Calculate XOR distance
 uint256 Distance(const NodeID& other) const {
 uint256 result;
 for (int i = 0; i < 20; i++) {
 result.begin()[i] = id[i] ^ other.id[i];
 }
 return result;
 }
 
 // Get raw ID
 const uint8_t* GetData() const { return id; }
 
 // Convert to hex string
 std::string ToString() const {
 return HexStr(std::vector<uint8_t>(id, id + 20));
 }
 
 // Comparison operator
 bool operator==(const NodeID& other) const {
 return std::memcmp(id, other.id, 20) == 0;
 }
 
 bool operator!=(const NodeID& other) const {
 return !(*this == other);
 }
 
 bool operator<(const NodeID& other) const {
 return std::memcmp(id, other.id, 20) < 0;
 }
};

#endif // TKN_NET_DHT_NODE_ID_H
