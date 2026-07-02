#include <pow/tknchash.h>
#include <pow/cn_slow_hash.hpp>
#include <pow/tokenhash.hxx>
#include <crypto/sha256.h>
#include <cstring>

static inline uint32_t ROTL32(uint32_t x, int n) {
    return (x << (n & 31)) | (x >> ((32 - n) & 31));
}

void TKNCGenerateTable(const uint256& seed, uint32_t table[TKNC_HASH_TABLE_SIZE]) {
    uint32_t s = 0;
    const uint8_t* seed_bytes = seed.begin();
    for (int i = 0; i < 32; i++) {
        s ^= (uint32_t)seed_bytes[i] << ((i * 3) & 31);
        s ^= (s << 13);
        s ^= (s >> 17);
        s ^= (s << 5);
    }
    if (s == 0) s = 0x9E3779B9;

    for (int i = 0; i < TKNC_HASH_TABLE_SIZE; i++) {
        s ^= (s << 13);
        s ^= (s >> 17);
        s ^= (s << 5);
        table[i] = s;
    }
}

uint256 TKNCComputeHashWithTable(const CBlockHeader& header, const uint32_t table[TKNC_HASH_TABLE_SIZE]) {
    // Hash raw serialized CBlockHeader (80 bytes). The keccak() function inside
    // cn_v4_hash_t::hash() adds its own 0x01 padding byte at offset 80, matching
    // the GPU kernel's State[10] = input[10] where input_data[80] = 0x01.
    // Appending an extra 0x01 here would cause double-padding (State[10] = 0x0101
    // on CPU vs 0x0001 on GPU), breaking PoW verification.
    thread_local cn_v4_hash_t ctx;
    uint8_t result[32] = {0};
    ctx.hash(&header, sizeof(CBlockHeader), result);

    uint256 hash;
    memcpy(hash.begin(), result, 32);
    return hash;
}

uint256 TKNCComputeHash(const CBlockHeader& header) {
    return TKNCComputeHashWithTable(header, nullptr);
}