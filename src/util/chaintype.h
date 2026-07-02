// Copyright (c) 2023-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TKN_UTIL_CHAINTYPE_H
#define TKN_UTIL_CHAINTYPE_H

#include <optional>
#include <string>
#include <string_view>

enum class ChainType {
    MAIN,
    TESTNET,
    SIGNET,
    REGTEST,
    TESTNET4,
};

std::string ChainTypeToString(ChainType chain);

std::optional<ChainType> ChainTypeFromString(std::string_view chain);

#endif // TKN_UTIL_CHAINTYPE_H
