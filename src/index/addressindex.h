// Copyright (c) 2024-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TKN_INDEX_ADDRESSINDEX_H
#define TKN_INDEX_ADDRESSINDEX_H

#include <index/base.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class uint160;

static constexpr bool DEFAULT_ADDRESSINDEX{false};

/**
 * UTXO data for a single address output.
 */
struct AddressUTXO {
    Txid txid;
    uint32_t vout{0};
    int64_t value{0};       // satoshis
    int height{0};
    bool coinbase{false};
};

/**
 * AddressIndex maintains a LevelDB index that maps scriptPubKey hashes to
 * their unspent transaction outputs (UTXOs). This enables O(k) address
 * balance lookups (where k is the number of UTXOs for that address)
 * instead of the O(n) full-UTXO-set scan performed by scantxoutset.
 *
 * The index stores two key spaces:
 *   1. UTXO index (prefix 'a'): key = Hash160(scriptPubKey) || txid || vout
 *      value = amount || height || coinbase_flag
 *   2. Outpoint index (prefix 'o'): key = txid || vout
 *      value = Hash160(scriptPubKey)  (used to find the scriptHash when
 *      an input spends a previously-indexed output)
 */
class AddressIndex final : public BaseIndex
{
private:
    std::unique_ptr<BaseIndex::DB> m_db;

    bool AllowPrune() const override { return false; }

protected:
    interfaces::Chain::NotifyOptions CustomOptions() override;

    bool CustomAppend(const interfaces::BlockInfo& block) override;

    bool CustomRemove(const interfaces::BlockInfo& block) override;

    BaseIndex::DB& GetDB() const override;

public:
    explicit AddressIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory = false, bool f_wipe = false);

    ~AddressIndex() override = default;

    /**
     * Look up all unspent outputs for the given scriptPubKey hash.
     *
     * @param[in]   script_hash  The Hash160 of the scriptPubKey to search for.
     * @param[out]  utxos        Vector to populate with found UTXOs.
     * @return  true on success, false on database error.
     */
    bool FindAddressUTXOs(const uint160& script_hash, std::vector<AddressUTXO>& utxos) const;
};

/// The global address index. May be null.
extern std::unique_ptr<AddressIndex> g_addressindex;

#endif // TKN_INDEX_ADDRESSINDEX_H
