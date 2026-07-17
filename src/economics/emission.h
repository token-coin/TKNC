#ifndef TKN_ECONOMICS_EMISSION_H
#define TKN_ECONOMICS_EMISSION_H

#include <consensus/amount.h>
#include <cstdint>
#include <string>
#include <mutex>
#include <vector>

// TKNC economic model - Remaining Supply Decay
//
// Formula: reward(h) = remaining(h-1) / DIVISOR
// where remaining(0) = TOTAL_SUPPLY * COIN
// remaining(h) = remaining(h-1) - reward(h)
//
// This is a geometric series: remaining(h) = S * ((D-1)/D)^h
// Total emitted after h blocks: emitted(h) = S - remaining(h)
//
// SECURITY: All arithmetic is integer (int64_t). No floating point.
// PERFORMANCE: O(1) via closed-form integer approximation using bit-shift powers.

static const int64_t TKNC_TOTAL_SUPPLY = 1000000000; // Total supply 1 billion TKNC
static const int64_t TKNC_BLOCKS_PER_YEAR = 262800; // Blocks per year (120s/block * 365.25d)
static const int TKNC_EMISSION_COMPLETE_YEAR = 9; // Emission completes after 9 years
static const int64_t TKNC_EMISSION_DIVISOR = 300000; // Divisor (integer)

// Block reward distribution: 90% miner, 10% team (protocol-enforced)
static const int64_t TKNC_TEAM_SHARE_PERCENT = 10;
inline const char* TKNC_TEAM_WALLET_ADDRESS = "token1qjln2lhvqe49yv7f6jud0ms874ge2fu4e0e3fjv";

// Total blocks in the emission period
static const int64_t TKNC_TOTAL_EMISSION_BLOCKS = TKNC_BLOCKS_PER_YEAR * TKNC_EMISSION_COMPLETE_YEAR;

// Forward declaration
inline CAmount GetTKNCTotalEmitted(int nHeight);

// Calculate block reward for specified height:
// reward(h) = remaining(h-1) / DIVISOR
// where remaining(h-1) = TOTAL_SUPPLY*COIN - emitted(h-1)
inline CAmount GetTKNCBlockSubsidy(int nHeight) {
 if (nHeight <= 0) {
 return 0;
 }

 int current_year = nHeight / TKNC_BLOCKS_PER_YEAR;
 if (current_year >= TKNC_EMISSION_COMPLETE_YEAR) {
 return 0;
 }

 // remaining_before_h = total - GetTKNCTotalEmitted(h); reward = remaining / D.
 CAmount total_emitted_before = GetTKNCTotalEmitted(nHeight);
 int64_t total_supply_sat = TKNC_TOTAL_SUPPLY * COIN;
 int64_t remaining_sat = total_supply_sat - total_emitted_before;

 if (remaining_sat <= 0) {
 return 0;
 }

 int64_t reward_sat = remaining_sat / TKNC_EMISSION_DIVISOR;

 // Ensure minimum reward of 1 satoshi if supply remains
 if (reward_sat == 0 && remaining_sat > 0) {
 reward_sat = 1;
 }

 return static_cast<CAmount>(reward_sat);
}

// O(1) closed-form calculation of total emitted amount.
//
// The series is: remaining(h) = S * ((D-1)/D)^h
// In integer arithmetic, ((D-1)/D)^h is computed via repeated squaring
// using 128-bit intermediate values to avoid overflow.
//
// We compute: remaining_h = S * (D-1)^h / D^h
// Since (D-1)^h and D^h grow exponentially, we use a scaling approach:
// remaining_h = S * pow_int(D-1, h) / pow_int(D, h)
// where pow_int uses exponentiation by squaring with 128-bit intermediates.
//
// For h up to ~2.4M blocks, D=300000:
// D^h is astronomically large, so we use logarithmic scaling instead.
//
// Practical O(1) approach: use the recurrence relation but with
// exponential jumping. After k blocks:
// (Chinese comment removed)
// We precompute ((D-1)/D)^(2^i) for i=0..21 (covers up to 4M blocks)
// and use binary decomposition of h to compute remaining(h) in O(log h).

// Lookup-table accelerated computation of remaining supply.
// Precomputes checkpoints every 1000 blocks, then iterates
// from the nearest checkpoint. Reduces worst-case from O(2.36M) to O(1000).
// Thread-safe via C++17 "magic statics" (guarantees single initialization
// of static locals in inline functions across all translation units).
static constexpr int EMISSION_LOOKUP_INTERVAL = 1000;

// Global lookup table (initialized once, thread-safe via magic statics)
struct EmissionLookupTable {
    std::vector<int64_t> checkpoints;
    EmissionLookupTable() {
        int64_t S = TKNC_TOTAL_SUPPLY * COIN;
        int64_t D = TKNC_EMISSION_DIVISOR;
        int num_checkpoints = (int)(TKNC_TOTAL_EMISSION_BLOCKS / EMISSION_LOOKUP_INTERVAL) + 2;
        checkpoints.resize(num_checkpoints);
        int64_t remaining = S;
        checkpoints[0] = remaining;
        for (int i = 1; i < num_checkpoints; i++) {
            for (int j = 0; j < EMISSION_LOOKUP_INTERVAL; j++) {
                int64_t reward = remaining / D;
                if (reward == 0) { remaining = 0; break; }
                remaining -= reward;
                if (remaining <= 0) { remaining = 0; break; }
            }
            checkpoints[i] = remaining;
            if (remaining == 0) {
                for (int k = i + 1; k < num_checkpoints; k++) {
                    checkpoints[k] = 0;
                }
                break;
            }
        }
    }
};

inline int64_t ComputeRemainingO1(int64_t S, int64_t D, int h) {
    if (h <= 0) return S;
    if (h > TKNC_TOTAL_EMISSION_BLOCKS) h = TKNC_TOTAL_EMISSION_BLOCKS;

    static EmissionLookupTable lookup;

    // Find nearest checkpoint at or below h
    int cp_idx = h / EMISSION_LOOKUP_INTERVAL;
    if (cp_idx >= (int)lookup.checkpoints.size()) cp_idx = lookup.checkpoints.size() - 1;
    int64_t remaining = lookup.checkpoints[cp_idx];
    if (remaining == 0) return 0;

    // Iterate from checkpoint to h (at most EMISSION_LOOKUP_INTERVAL - 1 steps)
    int start_h = cp_idx * EMISSION_LOOKUP_INTERVAL;
    for (int i = start_h; i < h; i++) {
        int64_t reward = remaining / D;
        if (reward == 0) { remaining = 0; break; }
        remaining -= reward;
        if (remaining <= 0) { remaining = 0; break; }
    }
    return remaining;
}

// Calculate total emitted amount up to specified height.
// (Chinese comment removed)
inline CAmount GetTKNCTotalEmitted(int nHeight) {
 if (nHeight <= 0) {
 return 0;
 }

 int current_year = nHeight / TKNC_BLOCKS_PER_YEAR;
 if (current_year >= TKNC_EMISSION_COMPLETE_YEAR) {
 return static_cast<CAmount>(TKNC_TOTAL_SUPPLY * COIN);
 }

 int effective_height = nHeight;
 if (effective_height > TKNC_TOTAL_EMISSION_BLOCKS) {
 effective_height = TKNC_TOTAL_EMISSION_BLOCKS;
 }

 int64_t total_supply_sat = TKNC_TOTAL_SUPPLY * COIN;
 int64_t remaining = ComputeRemainingO1(total_supply_sat, TKNC_EMISSION_DIVISOR, effective_height);

 int64_t emitted = total_supply_sat - remaining;

 if (emitted > total_supply_sat) {
 emitted = total_supply_sat;
 }

 return static_cast<CAmount>(emitted);
}

// Calculate remaining supply
inline CAmount GetTKNCRemainingSupply(int nHeight) {
 CAmount total_emitted = GetTKNCTotalEmitted(nHeight);
 CAmount total_supply = TKNC_TOTAL_SUPPLY * COIN;
 CAmount remaining = total_supply - total_emitted;

 return (remaining > 0) ? remaining : 0;
}

#endif // TKN_ECONOMICS_EMISSION_H
