// Copyright (c) 2016-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <blockencodings.h>
#include <chainparams.h>
#include <common/system.h>
#include <consensus/consensus.h>
#include <consensus/validation.h>
#include <crypto/sha256.h>
#include <crypto/siphash.h>
#include <logging.h>
#include <random.h>
#include <streams.h>
#include <txmempool.h>
#include <validation.h>

#include <unordered_map>

CBlockHeaderAndShortTxIDs::CBlockHeaderAndShortTxIDs(const CBlock& block, uint64_t nonce)
    : nonce(nonce),
      shorttxids(block.vtx.size() - 1),
      prefilledtxn(1),
      header(block)
{
    FillShortTxIDSelector();
    // TODO: Use our mempool prior to block acceptance to predictively fill more than just the coinbase
    prefilledtxn[0] = {0, block.vtx[0]};
    for (size_t i = 1; i < block.vtx.size(); i++) {
        const CTransaction& tx = *block.vtx[i];
        shorttxids[i - 1] = GetShortID(tx.GetWitnessHash());
    }
}

void CBlockHeaderAndShortTxIDs::FillShortTxIDSelector() const
{
    DataStream stream{};
    stream << header << nonce;
    CSHA256 hasher;
    hasher.Write((unsigned char*)&(*stream.begin()), stream.end() - stream.begin());
    uint256 shorttxidhash;
    hasher.Finalize(shorttxidhash.begin());
    m_hasher.emplace(shorttxidhash.GetUint64(0), shorttxidhash.GetUint64(1));
}

uint64_t CBlockHeaderAndShortTxIDs::GetShortID(const Wtxid& wtxid) const
{
    static_assert(SHORTTXIDS_LENGTH == 6, "shorttxids calculation assumes 6-byte shorttxids");
    return (*Assert(m_hasher))(wtxid.ToUint256()) & 0xffffffffffffL;
}

/* Reconstructing a compact block is hot-path: iterate a vector of (wtxid, tx) pairs
 * for CPU cache friendliness (~40 bytes per tx). */
ReadStatus PartiallyDownloadedBlock::InitData(const CBlockHeaderAndShortTxIDs& cmpctblock, const std::vector<std::pair<Wtxid, CTransactionRef>>& extra_txn)
{
    LogDebug(BCLog::CMPCTBLOCK, "Initializing PartiallyDownloadedBlock for block %s using a cmpctblock of %u bytes\n", cmpctblock.header.GetHash().ToString(), GetSerializeSize(cmpctblock));
    if (cmpctblock.header.IsNull() || (cmpctblock.shorttxids.empty() && cmpctblock.prefilledtxn.empty()))
        return READ_STATUS_INVALID;
    if (cmpctblock.shorttxids.size() + cmpctblock.prefilledtxn.size() > MAX_BLOCK_WEIGHT / MIN_SERIALIZABLE_TRANSACTION_WEIGHT)
        return READ_STATUS_INVALID;

    if (!header.IsNull() || !txn_available.empty()) return READ_STATUS_INVALID;

    header = cmpctblock.header;
    txn_available.resize(cmpctblock.BlockTxCount());

    int32_t lastprefilledindex = -1;
    for (size_t i = 0; i < cmpctblock.prefilledtxn.size(); i++) {
        if (cmpctblock.prefilledtxn[i].tx->IsNull())
            return READ_STATUS_INVALID;

        lastprefilledindex += cmpctblock.prefilledtxn[i].index + 1; //index is a uint16_t, so can't overflow here
        if (lastprefilledindex > std::numeric_limits<uint16_t>::max())
            return READ_STATUS_INVALID;
        if ((uint32_t)lastprefilledindex > cmpctblock.shorttxids.size() + i) {
            // Index exceeds shorttxids + prefilled count: a tx with neither prefilled data nor shortid.
            return READ_STATUS_INVALID;
        }
        txn_available[lastprefilledindex] = cmpctblock.prefilledtxn[i].tx;
    }
    prefilled_count = cmpctblock.prefilledtxn.size();

    // Map shortids -> positions. Uneven short-ID distribution (valid blocks should be uniform) → READ_STATUS_FAILED.
    std::unordered_map<uint64_t, uint16_t> shorttxids(cmpctblock.shorttxids.size());
    uint16_t index_offset = 0;
    for (size_t i = 0; i < cmpctblock.shorttxids.size(); i++) {
        while (txn_available[i + index_offset])
            index_offset++;
        shorttxids[cmpctblock.shorttxids[i]] = i + index_offset;
        // Bucket overflow check: P(max_bucket > 12) <= S*(1-cdf(binomial(n=S,p=1/S),12)); ~1 failure per 1M transfers.
        if (shorttxids.bucket_size(shorttxids.bucket(cmpctblock.shorttxids[i])) > 12)
            return READ_STATUS_FAILED;
    }
    if (shorttxids.size() != cmpctblock.shorttxids.size())
        return READ_STATUS_FAILED; // Short ID collision

    std::vector<bool> have_txn(txn_available.size());
    {
    LOCK(pool->cs);
    for (const auto& [wtxid, txit] : pool->txns_randomized) {
        uint64_t shortid = cmpctblock.GetShortID(wtxid);
        std::unordered_map<uint64_t, uint16_t>::iterator idit = shorttxids.find(shortid);
        if (idit != shorttxids.end()) {
            if (!have_txn[idit->second]) {
                txn_available[idit->second] = txit->GetSharedTx();
                have_txn[idit->second]  = true;
                mempool_count++;
            } else {
                // Two mempool txn match the shortid: request it (rare; bandwidth < round-trip cost).
                if (txn_available[idit->second]) {
                    txn_available[idit->second].reset();
                    mempool_count--;
                }
            }
        }
        // Early-exit on full match: worth the small risk of missing a two-txn-match-shortid case.
        if (mempool_count == shorttxids.size())
            break;
    }
    }

    for (size_t i = 0; i < extra_txn.size(); i++) {
        uint64_t shortid = cmpctblock.GetShortID(extra_txn[i].first);
        std::unordered_map<uint64_t, uint16_t>::iterator idit = shorttxids.find(shortid);
        if (idit != shorttxids.end()) {
            if (!have_txn[idit->second]) {
                txn_available[idit->second] = extra_txn[i].second;
                have_txn[idit->second]  = true;
                mempool_count++;
                extra_count++;
            } else {
                // Two mempool/extra txn match shortid: request it. Compare witness hashes first so
                // extra/mempool duplication doesn't trigger this (rare; bandwidth < round-trip cost).
                if (txn_available[idit->second] &&
                        txn_available[idit->second]->GetWitnessHash() != extra_txn[i].second->GetWitnessHash()) {
                    txn_available[idit->second].reset();
                    mempool_count--;
                    extra_count--;
                }
            }
        }
        // Early-exit on full match: worth the small risk of missing a two-txn-match-shortid case.
        if (mempool_count == shorttxids.size())
            break;
    }

    LogDebug(BCLog::CMPCTBLOCK, "Initialized PartiallyDownloadedBlock for block %s using a cmpctblock of %u bytes\n", cmpctblock.header.GetHash().ToString(), GetSerializeSize(cmpctblock));

    return READ_STATUS_OK;
}

bool PartiallyDownloadedBlock::IsTxAvailable(size_t index) const
{
    if (header.IsNull()) return false;

    assert(index < txn_available.size());
    return txn_available[index] != nullptr;
}

ReadStatus PartiallyDownloadedBlock::FillBlock(CBlock& block, const std::vector<CTransactionRef>& vtx_missing, bool segwit_active)
{
    if (header.IsNull()) return READ_STATUS_INVALID;

    block = header;
    block.vtx.resize(txn_available.size());

    size_t tx_missing_offset = 0;
    for (size_t i = 0; i < txn_available.size(); i++) {
        if (!txn_available[i]) {
            if (tx_missing_offset >= vtx_missing.size()) {
                return READ_STATUS_INVALID;
            }
            block.vtx[i] = vtx_missing[tx_missing_offset++];
        } else {
            block.vtx[i] = std::move(txn_available[i]);
        }
    }

    // Make sure we can't call FillBlock again.
    header.SetNull();
    txn_available.clear();

    if (vtx_missing.size() != tx_missing_offset) {
        return READ_STATUS_INVALID;
    }

    // Check for possible mutations early now that we have a seemingly good block
    IsBlockMutatedFn check_mutated{m_check_block_mutated_mock ? m_check_block_mutated_mock : IsBlockMutated};
    if (check_mutated(/*block=*/block, /*check_witness_root=*/segwit_active)) {
        return READ_STATUS_FAILED; // Possible Short ID collision
    }

    if (LogAcceptCategory(BCLog::CMPCTBLOCK, BCLog::Level::Debug)) {
        const uint256 hash{block.GetHash()};
        uint32_t tx_missing_size{0};
        for (const auto& tx : vtx_missing) tx_missing_size += tx->ComputeTotalSize();
        LogDebug(BCLog::CMPCTBLOCK, "Successfully reconstructed block %s with %u txn prefilled, %u txn from mempool (incl at least %u from extra pool) and %u txn (%u bytes) requested\n", hash.ToString(), prefilled_count, mempool_count, extra_count, vtx_missing.size(), tx_missing_size);
        if (vtx_missing.size() < 5) {
            for (const auto& tx : vtx_missing) {
                LogDebug(BCLog::CMPCTBLOCK, "Reconstructed block %s required tx %s\n", hash.ToString(), tx->GetHash().ToString());
            }
        }
    }

    return READ_STATUS_OK;
}
