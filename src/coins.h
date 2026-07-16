// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TKN_COINS_H
#define TKN_COINS_H

#include <attributes.h>
#include <compressor.h>
#include <core_memusage.h>
#include <memusage.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <support/allocators/pool.h>
#include <uint256.h>
#include <util/check.h>
#include <util/overflow.h>
#include <util/hasher.h>

#include <cassert>
#include <cstdint>

#include <functional>
#include <unordered_map>

/** A UTXO entry. Serialized format: VARINT((height << 1) | (coinbase ? 1 : 0)) followed by the non-spent CTxOut (via TxOutCompression). */
class Coin
{
public:
 //! unspent transaction output
 CTxOut out;

 //! whether containing transaction was a coinbase
 bool fCoinBase : 1;

 //! at which height this containing transaction was included in the active block chain
 uint32_t nHeight : 31;

 //! construct a Coin from a CTxOut and height/coinbase information.
 Coin(CTxOut&& outIn, int nHeightIn, bool fCoinBaseIn) : out(std::move(outIn)), fCoinBase(fCoinBaseIn), nHeight(nHeightIn) {}
 Coin(const CTxOut& outIn, int nHeightIn, bool fCoinBaseIn) : out(outIn), fCoinBase(fCoinBaseIn),nHeight(nHeightIn) {}

 void Clear() {
 out.SetNull();
 fCoinBase = false;
 nHeight = 0;
 }

 //! empty constructor
 Coin() : fCoinBase(false), nHeight(0) { }

 bool IsCoinBase() const {
 return fCoinBase;
 }

 template<typename Stream>
 void Serialize(Stream &s) const {
 assert(!IsSpent());
 uint32_t code{(uint32_t{nHeight} << 1) | uint32_t{fCoinBase}};
 ::Serialize(s, VARINT(code));
 ::Serialize(s, Using<TxOutCompression>(out));
 }

 template<typename Stream>
 void Unserialize(Stream &s) {
 uint32_t code = 0;
 ::Unserialize(s, VARINT(code));
 nHeight = code >> 1;
 fCoinBase = code & 1;
 ::Unserialize(s, Using<TxOutCompression>(out));
 }

 /** Either this coin never existed (see e.g. coinEmpty in coins.cpp), or it did exist and has been spent. */
 bool IsSpent() const {
 return out.IsNull();
 }

 size_t DynamicMemoryUsage() const {
 return memusage::DynamicUsage(out.scriptPubKey);
 }
};

struct CCoinsCacheEntry;
using CoinsCachePair = std::pair<const COutPoint, CCoinsCacheEntry>;

/** A Coin in one level of the coins database caching hierarchy. A coin can be: unspent or spent (spent means Coin::Clear() is applied); DIRTY or not DIRTY; FRESH or not FRESH. Only some of the 8 combinations are valid: unspent+FRESH+DIRTY (new coin in cache), unspent+not FRESH+DIRTY (reorg), unspent+not FRESH+not DIRTY (fetched from parent), spent+not FRESH+DIRTY (spentness must be flushed). */
struct CCoinsCacheEntry
{
private:
 /** Doubly linked list of flagged entries (DIRTY or FRESH), set in SetDirty/SetFresh, unset in SetClean. DIRTY entries are tracked so that only modified entries are passed to the parent cache for batch writing ?a performance optimization over scanning all entries. */
 CoinsCachePair* m_prev{nullptr};
 CoinsCachePair* m_next{nullptr};
 uint8_t m_flags{0};

 //! Adding a flag requires a reference to the sentinel of the flagged pair linked list.
 static void AddFlags(uint8_t flags, CoinsCachePair& pair, CoinsCachePair& sentinel) noexcept
 {
 Assume(flags & (DIRTY | FRESH));
 if (!pair.second.m_flags) {
 Assume(!pair.second.m_prev && !pair.second.m_next);
 pair.second.m_prev = sentinel.second.m_prev;
 pair.second.m_next = &sentinel;
 sentinel.second.m_prev = &pair;
 pair.second.m_prev->second.m_next = &pair;
 }
 Assume(pair.second.m_prev && pair.second.m_next);
 pair.second.m_flags |= flags;
 }

public:
 Coin coin; // The actual cached data.

 enum Flags {
 /** DIRTY means the CCoinsCacheEntry is potentially different from the version in the parent cache. Failure to mark a coin as DIRTY when it is potentially different from the parent cache will cause a consensus failure, since the coin's state won't get written to the parent when the cache is flushed. */
 DIRTY = (1 << 0),
 /** FRESH means the parent cache does not have this coin or that it is a spent coin in the parent cache. If a FRESH coin in the cache is later spent, it can be deleted entirely and doesn't ever need to be flushed to the parent. This is a performance optimization. Marking a coin as FRESH when it exists unspent in the parent cache will cause a consensus failure, since it might not be deleted from the parent when this cache is flushed. */
 FRESH = (1 << 1),
 };

 CCoinsCacheEntry() noexcept = default;
 explicit CCoinsCacheEntry(Coin&& coin_) noexcept : coin(std::move(coin_)) {}
 ~CCoinsCacheEntry()
 {
 SetClean();
 }

 static void SetDirty(CoinsCachePair& pair, CoinsCachePair& sentinel) noexcept { AddFlags(DIRTY, pair, sentinel); }
 static void SetFresh(CoinsCachePair& pair, CoinsCachePair& sentinel) noexcept { AddFlags(FRESH, pair, sentinel); }

 void SetClean() noexcept
 {
 if (!m_flags) return;
 m_next->second.m_prev = m_prev;
 m_prev->second.m_next = m_next;
 m_flags = 0;
 m_prev = m_next = nullptr;
 }
 bool IsDirty() const noexcept { return m_flags & DIRTY; }
 bool IsFresh() const noexcept { return m_flags & FRESH; }

 //! Only call Next when this entry is DIRTY, FRESH, or both
 CoinsCachePair* Next() const noexcept
 {
 Assume(m_flags);
 return m_next;
 }

 //! Only call Prev when this entry is DIRTY, FRESH, or both
 CoinsCachePair* Prev() const noexcept
 {
 Assume(m_flags);
 return m_prev;
 }

 //! Only use this for initializing the linked list sentinel
 void SelfRef(CoinsCachePair& pair) noexcept
 {
 Assume(&pair.second == this);
 m_prev = &pair;
 m_next = &pair;
 // Set sentinel to DIRTY so we can call Next on it
 m_flags = DIRTY;
 }
};

/** PoolAllocator's MAX_BLOCK_SIZE_BYTES parameter uses sizeof the data plus 4 pointers. The exact unordered_node size is implementation defined; most implementations have 1-2 pointer overhead, so sizeof(void*)*4 should be sufficient for all implementations. */
using CCoinsMap = std::unordered_map<COutPoint,
 CCoinsCacheEntry,
 SaltedOutpointHasher,
 std::equal_to<COutPoint>,
 PoolAllocator<CoinsCachePair,
 sizeof(CoinsCachePair) + sizeof(void*) * 4>>;

using CCoinsMapMemoryResource = CCoinsMap::allocator_type::ResourceType;

/** Cursor for iterating over CoinsView state */
class CCoinsViewCursor
{
public:
 CCoinsViewCursor(const uint256& in_block_hash) : block_hash(in_block_hash) {}
 virtual ~CCoinsViewCursor() = default;

 virtual bool GetKey(COutPoint &key) const = 0;
 virtual bool GetValue(Coin &coin) const = 0;

 virtual bool Valid() const = 0;
 virtual void Next() = 0;

 //! Get best block at the time this cursor was created
 const uint256& GetBestBlock() const { return block_hash; }
private:
 uint256 block_hash;
};

/** Cursor for iterating over the linked list of flagged entries in CCoinsViewCache. This helper struct encapsulates the diverging logic between a non-erasing CCoinsViewCache::Sync and an erasing CCoinsViewCache::Flush, so the receiver of CCoinsView::BatchWrite can iterate flagged entries without knowing the caller's intent. The receiver can call CoinsViewCacheCursor::WillErase to see if the caller will erase the entry after BatchWrite returns; if so, it can move the coin out of the CCoinsCachEntry instead of copying. */
struct CoinsViewCacheCursor
{
 //! If will_erase is not set, iterating through the cursor will erase spent coins from the map and unflag other coins. If will_erase is set, the underlying map and linked list are not modified (caller wipes the map). Calling CCoinsMap::clear() afterwards is faster than erasing during iteration because a CoinsCachePair cannot be coerced back into a CCoinsMap::iterator to be erased and must be looked up by key.
 CoinsViewCacheCursor(size_t& dirty_count LIFETIMEBOUND,
 CoinsCachePair& sentinel LIFETIMEBOUND,
 CCoinsMap& map LIFETIMEBOUND,
 bool will_erase) noexcept
 : m_dirty_count(dirty_count), m_sentinel(sentinel), m_map(map), m_will_erase(will_erase) {}

 inline CoinsCachePair* Begin() const noexcept { return m_sentinel.second.Next(); }
 inline CoinsCachePair* End() const noexcept { return &m_sentinel; }

 //! Return the next entry after current, possibly erasing current
 inline CoinsCachePair* NextAndMaybeErase(CoinsCachePair& current) noexcept
 {
 const auto next_entry{current.second.Next()};
 Assume(TrySub(m_dirty_count, current.second.IsDirty()));
 // If we are not going to erase the cache, we must still erase spent entries.
 // Otherwise, clear the state of the entry.
 if (!m_will_erase) {
 if (current.second.coin.IsSpent()) {
 assert(current.second.coin.DynamicMemoryUsage() == 0); // scriptPubKey was already cleared in SpendCoin
 m_map.erase(current.first);
 } else {
 current.second.SetClean();
 }
 }
 return next_entry;
 }

 inline bool WillErase(CoinsCachePair& current) const noexcept { return m_will_erase || current.second.coin.IsSpent(); }
 size_t GetDirtyCount() const noexcept { return m_dirty_count; }
 size_t GetTotalCount() const noexcept { return m_map.size(); }
private:
 size_t& m_dirty_count;
 CoinsCachePair& m_sentinel;
 CCoinsMap& m_map;
 bool m_will_erase;
};

/** Pure abstract view on the open txout dataset. */
class CCoinsView
{
public:
 //! As we use CCoinsViews polymorphically, have a virtual destructor
 virtual ~CCoinsView() = default;

 //! Retrieve the Coin (unspent transaction output) for a given outpoint.
 //! May populate the cache. Use PeekCoin() to perform a non-caching lookup.
 virtual std::optional<Coin> GetCoin(const COutPoint& outpoint) const = 0;

 //! Retrieve the Coin (unspent transaction output) for a given outpoint, without caching results.
 //! Does not populate the cache. Use GetCoin() to cache the result.
 virtual std::optional<Coin> PeekCoin(const COutPoint& outpoint) const = 0;

 //! Just check whether a given outpoint is unspent.
 //! May populate the cache. Use PeekCoin() to perform a non-caching lookup.
 virtual bool HaveCoin(const COutPoint& outpoint) const = 0;

 //! Retrieve the block hash whose state this CCoinsView currently represents
 virtual uint256 GetBestBlock() const = 0;

 //! Retrieve the range of blocks that may have been only partially written. Empty vector if the database is consistent; otherwise a two-element vector of (new, old) block hash.
 virtual std::vector<uint256> GetHeadBlocks() const = 0;

 //! Do a bulk modification (multiple Coin changes + BestBlock change).
 //! The passed cursor is used to iterate through the coins.
 virtual void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash) = 0;

 //! Get a cursor to iterate over the whole state. Implementations may return nullptr.
 virtual std::unique_ptr<CCoinsViewCursor> Cursor() const = 0;

 //! Estimate database size
 virtual size_t EstimateSize() const = 0;
};

/** Noop coins view. */
class CoinsViewEmpty : public CCoinsView
{
protected:
 CoinsViewEmpty() = default;

public:
 static CoinsViewEmpty& Get();

 CoinsViewEmpty(const CoinsViewEmpty&) = delete;
 CoinsViewEmpty& operator=(const CoinsViewEmpty&) = delete;

 std::optional<Coin> GetCoin(const COutPoint&) const override { return {}; }
 std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override { return GetCoin(outpoint); }
 bool HaveCoin(const COutPoint& outpoint) const override { return !!GetCoin(outpoint); }
 uint256 GetBestBlock() const override { return {}; }
 std::vector<uint256> GetHeadBlocks() const override { return {}; }
 void BatchWrite(CoinsViewCacheCursor& cursor, const uint256&) override
 {
 for (auto it{cursor.Begin()}; it != cursor.End(); it = cursor.NextAndMaybeErase(*it)) { }
 }
 std::unique_ptr<CCoinsViewCursor> Cursor() const override { return {}; }
 size_t EstimateSize() const override { return 0; }
};

/** CCoinsView backed by another CCoinsView */
class CCoinsViewBacked : public CCoinsView
{
protected:
 CCoinsView* base;

public:
 explicit CCoinsViewBacked(CCoinsView* in_view) : base{Assert(in_view)} {}

 void SetBackend(CCoinsView& in_view) { base = &in_view; }

 std::optional<Coin> GetCoin(const COutPoint& outpoint) const override { return base->GetCoin(outpoint); }
 std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override { return base->PeekCoin(outpoint); }
 bool HaveCoin(const COutPoint& outpoint) const override { return base->HaveCoin(outpoint); }
 uint256 GetBestBlock() const override { return base->GetBestBlock(); }
 std::vector<uint256> GetHeadBlocks() const override { return base->GetHeadBlocks(); }
 void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash) override { base->BatchWrite(cursor, block_hash); }
 std::unique_ptr<CCoinsViewCursor> Cursor() const override { return base->Cursor(); }
 size_t EstimateSize() const override { return base->EstimateSize(); }
};


/** CCoinsView that adds a memory cache for transactions to another CCoinsView */
class CCoinsViewCache : public CCoinsViewBacked
{
private:
 const bool m_deterministic;

protected:
 /** Make mutable so that we can "fill the cache" even from Get-methods declared as "const". */
 mutable uint256 m_block_hash;
 mutable CCoinsMapMemoryResource m_cache_coins_memory_resource{};
 /* The starting sentinel of the flagged entry circular doubly linked list. */
 mutable CoinsCachePair m_sentinel;
 mutable CCoinsMap cacheCoins;

 /* Cached dynamic memory usage for the inner Coin objects. */
 mutable size_t cachedCoinsUsage{0};
 /* Running count of dirty Coin cache entries. */
 mutable size_t m_dirty_count{0};

 /** Discard all modifications made to this cache without flushing to the base view. Used to efficiently reuse a cache instance across multiple operations. */
 void Reset() noexcept;

 /* Fetch the coin from base. Used for cache misses in FetchCoin. */
 virtual std::optional<Coin> FetchCoinFromBase(const COutPoint& outpoint) const;

public:
 CCoinsViewCache(CCoinsView* in_base, bool deterministic = false);

 /** By deleting the copy constructor, we prevent accidentally using it when one intends to create a cache on top of a base cache. */
 CCoinsViewCache(const CCoinsViewCache &) = delete;

 // Standard CCoinsView methods
 std::optional<Coin> GetCoin(const COutPoint& outpoint) const override;
 std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override;
 bool HaveCoin(const COutPoint& outpoint) const override;
 uint256 GetBestBlock() const override;
 void SetBestBlock(const uint256& block_hash);
 void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash) override;
 std::unique_ptr<CCoinsViewCursor> Cursor() const override {
 throw std::logic_error("CCoinsViewCache cursor iteration not supported.");
 }

 /** Check if we have the given utxo already loaded in this cache. The semantics are the same as HaveCoin(), but no calls to the backing CCoinsView are made. */
 bool HaveCoinInCache(const COutPoint &outpoint) const;

 /** Return a reference to Coin in the cache, or coinEmpty if not found. More efficient than GetCoin. Do not hold the reference for more than a short scope; the current implementation allows modifications during the hold, but this should not be relied upon. */
 const Coin& AccessCoin(const COutPoint &output) const;

 /** Add a coin. Set possible_overwrite to true if an unspent version may already exist in the cache. */
 void AddCoin(const COutPoint& outpoint, Coin&& coin, bool possible_overwrite);

 /** Emplace a coin into cacheCoins without performing any checks, marking the emplaced coin as dirty. NOT FOR GENERAL USE. Used only when loading coins from a UTXO snapshot. @sa ChainstateManager::PopulateAndValidateSnapshot() */
 void EmplaceCoinInternalDANGER(COutPoint&& outpoint, Coin&& coin);

 /** Spend a coin. Pass moveto to get the deleted data. If no unspent output exists for the passed outpoint, this call has no effect. */
 bool SpendCoin(const COutPoint &outpoint, Coin* moveto = nullptr);

 /** Push the modifications applied to this cache to its base and wipe local state. Failure to call this method or Sync() before destruction will cause the changes to be forgotten. If reallocate_cache is false, the cache will retain the same memory footprint after flushing and should be destroyed to deallocate. */
 void Flush(bool reallocate_cache = true);

 /** Push the modifications applied to this cache to its base while retaining the contents of this cache (except for spent coins, which we erase). Failure to call this method or Flush() before destruction will cause the changes to be forgotten. */
 void Sync();

 /** Removes the UTXO with the given outpoint from the cache, if it is not modified. */
 void Uncache(const COutPoint &outpoint);

 //! Size of the cache (in number of transaction outputs)
 unsigned int GetCacheSize() const;

 //! Number of dirty cache entries (transaction outputs)
 size_t GetDirtyCount() const noexcept { return m_dirty_count; }

 //! Calculate the size of the cache (in bytes)
 size_t DynamicMemoryUsage() const;

 //! Check whether all prevouts of the transaction are present in the UTXO set represented by this view
 bool HaveInputs(const CTransaction& tx) const;

 //! Force a reallocation of the cache map. Required when downsizing the cache because the map's allocator may hold onto memory despite .clear() having been called. See: https://stackoverflow.com/questions/42114044/how-to-release-unordered-map-memory
 void ReallocateCache();

 //! Run an internal sanity check on the cache data structure.
 void SanityCheck() const;

 class ResetGuard
 {
 private:
 friend CCoinsViewCache;
 CCoinsViewCache& m_cache;
 explicit ResetGuard(CCoinsViewCache& cache LIFETIMEBOUND) noexcept : m_cache{cache} {}

 public:
 ResetGuard(const ResetGuard&) = delete;
 ResetGuard& operator=(const ResetGuard&) = delete;
 ResetGuard(ResetGuard&&) = delete;
 ResetGuard& operator=(ResetGuard&&) = delete;

 ~ResetGuard() { m_cache.Reset(); }
 };

 //! Create a scoped guard that will call `Reset()` on this cache when it goes out of scope.
 [[nodiscard]] ResetGuard CreateResetGuard() noexcept { return ResetGuard{*this}; }

private:
 /** @note this is marked const, but may actually append to `cacheCoins`, increasing memory usage. */
 CCoinsMap::iterator FetchCoin(const COutPoint &outpoint) const;
};

/** CCoinsViewCache overlay that avoids populating/mutating parent cache layers on cache misses. Achieved by fetching coins from the base view using PeekCoin() instead of GetCoin(), so intermediate CCoinsViewCache layers are not filled. Used during ConnectBlock() as an ephemeral, resettable top-level view flushed only on success, so invalid blocks don't pollute the underlying cache. */
class CoinsViewOverlay : public CCoinsViewCache
{
private:
 std::optional<Coin> FetchCoinFromBase(const COutPoint& outpoint) const override
 {
 return base->PeekCoin(outpoint);
 }

public:
 using CCoinsViewCache::CCoinsViewCache;
};

//! Utility function to add all of a transaction's outputs to a cache. When check is false, assumes overwrites are only possible for coinbase transactions. When check is true, the underlying view may be queried to determine whether an addition is an overwrite. TODO: pass in a boolean to limit these possible overwrites to known (pre-BIP34) cases.
void AddCoins(CCoinsViewCache& cache, const CTransaction& tx, int nHeight, bool check = false);

// (Chinese comment removed)
const Coin& AccessByTxid(const CCoinsViewCache& cache, const Txid& txid);

/** Minimally invasive approach to shutdown on LevelDB read errors from the chainstate, while keeping user interface out of the common library shared between tkncd, tknc-qt, and non-server tools. Writes do not need similar protection ?failure to write is handled by the caller. */
class CCoinsViewErrorCatcher final : public CCoinsViewBacked
{
public:
 explicit CCoinsViewErrorCatcher(CCoinsView* view) : CCoinsViewBacked(view) {}

 void AddReadErrCallback(std::function<void()> f) {
 m_err_callbacks.emplace_back(std::move(f));
 }

 std::optional<Coin> GetCoin(const COutPoint& outpoint) const override;
 bool HaveCoin(const COutPoint& outpoint) const override;
 std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override;

private:
 /** A list of callbacks to execute upon leveldb read error. */
 std::vector<std::function<void()>> m_err_callbacks;

};

#endif // TKN_COINS_H
