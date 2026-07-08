// Copyright (c) 2016-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TKN_WALLET_RPC_WALLET_H
#define TKN_WALLET_RPC_WALLET_H

#include <span.h>

class CRPCCommand;

namespace wallet {
std::span<const CRPCCommand> GetWalletRPCCommands();
} // namespace wallet

#endif // TKN_WALLET_RPC_WALLET_H
