#ifndef TKN_POW_TKNCHASH_H
#define TKN_POW_TKNCHASH_H

#include <uint256.h>
#include <arith_uint256.h>
#include <primitives/block.h>
#include <cstdint>
#include <vector>

inline constexpr int TKNC_HASH_TABLE_SIZE = 4096;
inline constexpr int TKNC_HASH_STATE_SIZE = 8;
inline constexpr int TKNC_HASH_ROUNDS = 64;

void TKNCGenerateTable(const uint256& seed, uint32_t table[TKNC_HASH_TABLE_SIZE]);
uint256 TKNCComputeHash(const CBlockHeader& header);
uint256 TKNCComputeHashWithTable(const CBlockHeader& header, const uint32_t table[TKNC_HASH_TABLE_SIZE]);

#endif
