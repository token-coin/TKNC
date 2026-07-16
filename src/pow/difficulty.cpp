#include <pow/difficulty.h>
#include <chain.h>
#include <consensus/params.h>
#include <arith_uint256.h>
#include <algorithm>
#include <cmath>

// TKNC per-block difficulty adjustment using a sliding-window proportional algorithm.
//
// BUG FIXED (difficulty never converged to 120s target):
// The old algorithm used x = 2 - (timespan / 60) with integer truncation.
// This created a 120-179s "dead zone" where blocks 25-50% slower than target
// triggered ZERO adjustment, while 60-119s blocks (slightly fast) still
// increased difficulty. Combined with single-block noise (Poisson variance),
// this caused a systematic upward bias on difficulty, making blocks
// permanently too slow.
//
// NEW ALGORITHM:
// 1. Compute the average block time over the last WINDOW blocks (smooths
// out single-block Poisson noise).
// 2. Compute a ratio = avg_timespan / target_spacing using 1024-bit fixed
// point for precision.
// (Chinese comment removed)
// (Chinese comment removed)

static constexpr int TKN_DIFF_WINDOW = 12; // ~24 min of history at 120s/block
static constexpr int64_t TKN_RATIO_SCALE = 1024; // fixed-point base
static constexpr int64_t TKN_RATIO_MIN = 819; // -20% (1024 * 0.80)
static constexpr int64_t TKN_RATIO_MAX = 1229; // +20% (1024 * 1.20)

arith_uint256 CalculateNextDifficultyTarget(const CBlockIndex* pindexLast, const Consensus::Params& params) {
 if (pindexLast == nullptr) {
 return UintToArith256(params.powLimit);
 }

 if (pindexLast->pprev == nullptr) {
 arith_uint256 genesis_target;
 genesis_target.SetCompact(pindexLast->nBits);
 return genesis_target;
 }

 arith_uint256 parent_target;
 parent_target.SetCompact(pindexLast->nBits);
 arith_uint256 pow_limit = UintToArith256(params.powLimit);

 const int64_t target_spacing = params.nPowTargetSpacing; // 120s

 // --- Sliding-window average block time ---
 int64_t total_timespan = 0;
 int count = 0;
 const CBlockIndex* p = pindexLast;
 while (p != nullptr && p->pprev != nullptr && count < TKN_DIFF_WINDOW) {
 total_timespan += p->GetBlockTime() - p->pprev->GetBlockTime();
 p = p->pprev;
 count++;
 }
 if (count == 0) {
 return parent_target;
 }

 int64_t avg_timespan = total_timespan / count;

 // --- Proportional adjustment via fixed-point ratio ---
 // ratio = avg_timespan * 1024 / target_spacing
 // (Chinese comment removed)
 // (Chinese comment removed)
 int64_t ratio = avg_timespan * TKN_RATIO_SCALE / target_spacing;

 // (Chinese comment removed)
 if (ratio < TKN_RATIO_MIN) ratio = TKN_RATIO_MIN;
 if (ratio > TKN_RATIO_MAX) ratio = TKN_RATIO_MAX;

 arith_uint256 new_target = parent_target * (uint64_t)ratio / TKN_RATIO_SCALE;

 // Final bounds check
 if (new_target > pow_limit) {
 new_target = pow_limit;
 }
 if (new_target == 0) {
 new_target = arith_uint256(1);
 }

 return new_target;
}

arith_uint256 ClampDifficulty(const arith_uint256& new_target, const arith_uint256& parent_target, const arith_uint256& pow_limit) {
 arith_uint256 clamped = new_target;
 if (clamped > pow_limit) clamped = pow_limit;
 if (clamped == 0) clamped = arith_uint256(1);
 return clamped;
}
