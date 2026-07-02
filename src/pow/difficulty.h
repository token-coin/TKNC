#ifndef TKN_POW_DIFFICULTY_H
#define TKN_POW_DIFFICULTY_H

#include <consensus/params.h>
#include <chain.h>
#include <arith_uint256.h>
#include <cstdint>
#include <vector>

inline constexpr int TKN_DIFFICULTY_WINDOW = 100;
inline constexpr int TKN_MIN_DIFFICULTY_WINDOW = 10;
inline constexpr int TKN_TARGET_BLOCK_TIME = 120;

arith_uint256 CalculateNextDifficultyTarget(const CBlockIndex* pindexLast, const Consensus::Params& params);
arith_uint256 ClampDifficulty(const arith_uint256& new_target, const arith_uint256& parent_target, const arith_uint256& pow_limit);

#endif // TKN_POW_DIFFICULTY_H
