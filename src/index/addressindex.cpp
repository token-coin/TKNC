// Copyright (c) 2024-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/addressindex.h>

#include <common/args.h>
#include <dbwrapper.h>
#include <hash.h>
#include <index/base.h>
#include <interfaces/chain.h>
#include <logging.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <streams.h>
#include <undo.h>
#include <uint256.h>
#include <util/fs.h>
#include <validation.h>

#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// LevelDB key prefixes.
constexpr uint8_t DB_ADDRESSINDEX_UTXO{'a'}; // (Chinese comment removed)
constexpr uint8_t DB_ADDRESSINDEX_OUTPOINT{'o'}; // (Chinese comment removed)

std::unique_ptr<AddressIndex> g_addressindex;

/**
 * UTXO index key: prefix || scriptHash || txid || vout
 * Used for range queries by scriptHash.
 */
struct DBAddressKey {
 uint160 script_hash;
 Txid txid;
 uint32_t vout{0};

 DBAddressKey() = default;
 DBAddressKey(const uint160& sh, const Txid& tx, uint32_t v) : script_hash(sh), txid(tx), vout(v) {}

 SERIALIZE_METHODS(DBAddressKey, obj)
 {
 uint8_t prefix{DB_ADDRESSINDEX_UTXO};
 READWRITE(prefix);
 if (prefix != DB_ADDRESSINDEX_UTXO) {
 throw std::ios_base::failure("Invalid format for address index UTXO key");
 }
 READWRITE(obj.script_hash);
 READWRITE(obj.txid);
 READWRITE(obj.vout);
 }
};

/**
 * UTXO index value: amount || height || coinbase_flag
 */
struct DBAddressValue {
 int64_t amount{0};
 int32_t height{0};
 uint8_t coinbase{0};

 DBAddressValue() = default;
 DBAddressValue(int64_t amt, int32_t h, bool cb) : amount(amt), height(h), coinbase(cb ? 1 : 0) {}

 SERIALIZE_METHODS(DBAddressValue, obj)
 {
 READWRITE(obj.amount);
 READWRITE(obj.height);
 READWRITE(obj.coinbase);
 }
};

/**
 * Outpoint index key: prefix || txid || vout
 * Used to look up the scriptHash when an input spends a previously-indexed output.
 */
struct DBOutpointKey {
 Txid txid;
 uint32_t vout{0};

 DBOutpointKey() = default;
 DBOutpointKey(const Txid& tx, uint32_t v) : txid(tx), vout(v) {}

 SERIALIZE_METHODS(DBOutpointKey, obj)
 {
 uint8_t prefix{DB_ADDRESSINDEX_OUTPOINT};
 READWRITE(prefix);
 if (prefix != DB_ADDRESSINDEX_OUTPOINT) {
 throw std::ios_base::failure("Invalid format for address index outpoint key");
 }
 READWRITE(obj.txid);
 READWRITE(obj.vout);
 }
};

// ---------------------------------------------------------------------------
// AddressIndex construction
// ---------------------------------------------------------------------------

AddressIndex::AddressIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory, bool f_wipe)
 : BaseIndex(std::move(chain), "addressindex"),
 m_db{std::make_unique<BaseIndex::DB>(gArgs.GetDataDirNet() / "indexes" / "addressindex", n_cache_size, f_memory, f_wipe)}
{
}

interfaces::Chain::NotifyOptions AddressIndex::CustomOptions()
{
 interfaces::Chain::NotifyOptions options;
 // Request undo data so CustomRemove can restore spent outputs during reorgs.
 options.disconnect_data = true;
 return options;
}

BaseIndex::DB& AddressIndex::GetDB() const { return *m_db; }

// ---------------------------------------------------------------------------
// Helper: compute Hash160 of a scriptPubKey
// ---------------------------------------------------------------------------

static uint160 ComputeScriptHash(const CScript& scriptPubKey)
{
 return Hash160(scriptPubKey);
}

// ---------------------------------------------------------------------------
// CustomAppend: index new outputs, remove spent outputs
// ---------------------------------------------------------------------------

bool AddressIndex::CustomAppend(const interfaces::BlockInfo& block)
{
 // Exclude genesis block (outputs are not spendable in the same way).
 if (block.height == 0) return true;

 assert(block.data);

 CDBBatch batch(*m_db);

 // Phase 1: Add all new outputs to the index.
 for (const auto& tx : block.data->vtx) {
 const Txid txid = tx->GetHash();
 for (uint32_t vout = 0; vout < tx->vout.size(); ++vout) {
 const CTxOut& output = tx->vout[vout];
 // Skip unspendable outputs (OP_RETURN, oversized scripts).
 if (output.scriptPubKey.IsUnspendable()) continue;

 uint160 script_hash = ComputeScriptHash(output.scriptPubKey);

 // Write UTXO index entry.
 DBAddressKey addr_key(script_hash, txid, vout);
 DBAddressValue addr_value(output.nValue, block.height, tx->IsCoinBase());
 batch.Write(addr_key, addr_value);

 // Write outpoint index entry (for reverse lookup when spent).
 DBOutpointKey outpoint_key(txid, vout);
 batch.Write(outpoint_key, script_hash);
 }
 }

 // Phase 2: Remove outputs spent by inputs in this block.
 // Do this after adding new outputs so that same-block spend-and-respend works.
 for (const auto& tx : block.data->vtx) {
 if (tx->IsCoinBase()) continue;

 for (const auto& input : tx->vin) {
 const COutPoint& prevout = input.prevout;
 DBOutpointKey outpoint_key(prevout.hash, prevout.n);

 // Read the scriptHash from the outpoint index.
 uint160 script_hash;
 if (!m_db->Read(outpoint_key, script_hash)) {
 // The spent output might not be in our index if:
 // 1. It was created before the index was started (during initial sync gap).
 // 2. It is an unspendable output that was skipped.
 // In either case, there is nothing to remove.
 continue;
 }

 // Remove from UTXO index.
 DBAddressKey addr_key(script_hash, prevout.hash, prevout.n);
 batch.Erase(addr_key);

 // Remove from outpoint index.
 batch.Erase(outpoint_key);
 }
 }

 m_db->WriteBatch(batch);
 return true;
}

// ---------------------------------------------------------------------------
// CustomRemove: undo a block during reorg (reverse of CustomAppend)
// ---------------------------------------------------------------------------

bool AddressIndex::CustomRemove(const interfaces::BlockInfo& block)
{
 if (block.height == 0) return true;

 assert(block.data);
 // undo_data is required to restore spent outputs.
 assert(block.undo_data);

 CDBBatch batch(*m_db);

 // Phase 1: Restore outputs that were spent by inputs in this block.
 // This reverses Phase 2 of CustomAppend.
 // undo_data->vtxundo[i] corresponds to block.data->vtx[i+1] (skip coinbase).
 for (size_t tx_idx = 1; tx_idx < block.data->vtx.size(); ++tx_idx) {
 const auto& tx = block.data->vtx[tx_idx];
 const CTxUndo& txundo = block.undo_data->vtxundo[tx_idx - 1];
 assert(txundo.vprevout.size() == tx->vin.size());

 for (size_t vin_idx = 0; vin_idx < tx->vin.size(); ++vin_idx) {
 const COutPoint& prevout = tx->vin[vin_idx].prevout;
 const Coin& coin = txundo.vprevout[vin_idx];

 // Skip unspendable outputs.
 if (coin.out.scriptPubKey.IsUnspendable()) continue;

 uint160 script_hash = ComputeScriptHash(coin.out.scriptPubKey);

 // Restore UTXO index entry.
 DBAddressKey addr_key(script_hash, prevout.hash, prevout.n);
 DBAddressValue addr_value(coin.out.nValue, coin.nHeight, coin.IsCoinBase());
 batch.Write(addr_key, addr_value);

 // Restore outpoint index entry.
 DBOutpointKey outpoint_key(prevout.hash, prevout.n);
 batch.Write(outpoint_key, script_hash);
 }
 }

 // Phase 2: Remove outputs that were created by transactions in this block.
 // This reverses Phase 1 of CustomAppend.
 for (const auto& tx : block.data->vtx) {
 const Txid txid = tx->GetHash();
 for (uint32_t vout = 0; vout < tx->vout.size(); ++vout) {
 const CTxOut& output = tx->vout[vout];
 if (output.scriptPubKey.IsUnspendable()) continue;

 uint160 script_hash = ComputeScriptHash(output.scriptPubKey);

 // Remove from UTXO index.
 DBAddressKey addr_key(script_hash, txid, vout);
 batch.Erase(addr_key);

 // Remove from outpoint index.
 DBOutpointKey outpoint_key(txid, vout);
 batch.Erase(outpoint_key);
 }
 }

 m_db->WriteBatch(batch);
 return true;
}

// ---------------------------------------------------------------------------
// FindAddressUTXOs: range query by scriptHash
// ---------------------------------------------------------------------------

bool AddressIndex::FindAddressUTXOs(const uint160& script_hash, std::vector<AddressUTXO>& utxos) const
{
 utxos.clear();

 std::unique_ptr<CDBIterator> it(m_db->NewIterator());

 // Seek to the first key with our prefix + scriptHash.
 it->Seek(std::pair(DB_ADDRESSINDEX_UTXO, script_hash));

 DBAddressKey key;
 while (it->Valid() && it->GetKey(key) && key.script_hash == script_hash) {
 DBAddressValue value;
 if (!it->GetValue(value)) {
 LogError("AddressIndex: failed to read value for UTXO %s:%d", key.txid.GetHex(), key.vout);
 return false;
 }
 AddressUTXO utxo;
 utxo.txid = key.txid;
 utxo.vout = key.vout;
 utxo.value = value.amount;
 utxo.height = value.height;
 utxo.coinbase = value.coinbase != 0;
 utxos.push_back(std::move(utxo));
 it->Next();
 }

 return true;
}
