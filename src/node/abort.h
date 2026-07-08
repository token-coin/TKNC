// Copyright (c) 2023-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TKN_NODE_ABORT_H
#define TKN_NODE_ABORT_H

#include <atomic>
#include <functional>

struct bilingual_str;

namespace node {
class Warnings;
void AbortNode(const std::function<bool()>& shutdown_request, std::atomic<int>& exit_status, const bilingual_str& message, node::Warnings* warnings);
} // namespace node

#endif // TKN_NODE_ABORT_H
