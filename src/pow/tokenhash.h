#ifndef TKN_POW_TOKENHASH_H
#define TKN_POW_TOKENHASH_H

#include <uint256.h>
#include <arith_uint256.h>
#include <primitives/block.h>
#include <cstdint>
#include <vector>
#include <array>
#include <memory>
#include <atomic>

namespace tokenhash {

constexpr size_t EPOCH_LENGTH = 30000;
constexpr int64_t ALLOWED_FUTURE_BLOCK_TIME_SECONDS = 15;
constexpr uint64_t DIFFICULTY_BOUND_DIVISOR = 2048;

}

#endif
