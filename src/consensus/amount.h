// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TKN_CONSENSUS_AMOUNT_H
#define TKN_CONSENSUS_AMOUNT_H

#include <cstdint>

/** Amount in TKNC (Can be negative) */
typedef int64_t CAmount;

/** The smallest unit of one TKNC. */
static constexpr CAmount COIN = 100000000;

/** No amount larger than this (in TKNC) is valid.
 *
 * TKNC total supply is 1,000,000,000 (1 billion) TKNC.
 * This constant is consensus-critical and used by MoneyRange() validation.
 */
static constexpr CAmount MAX_MONEY = 1000000000 * COIN;
inline bool MoneyRange(const CAmount& nValue) { return (nValue >= 0 && nValue <= MAX_MONEY); }

#endif // TKN_CONSENSUS_AMOUNT_H
