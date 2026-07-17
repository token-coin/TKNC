// Copyright (c) 2022-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <headerssync.h>

#include <logging.h>
#include <pow.h>
#include <util/check.h>
#include <util/time.h>
#include <util/vector.h>

// Our memory analysis in headerssync-params.py assumes this many bytes for a
// CompressedHeader (we should re-calculate parameters if we compress further).
static_assert(sizeof(CompressedHeader) == 48);

HeadersSyncState::HeadersSyncState(NodeId id,
 const Consensus::Params& consensus_params,
 const HeadersSyncParams& params,
 const CBlockIndex& chain_start,
 const arith_uint256& minimum_required_work)
 : m_commit_offset((assert(params.commitment_period > 0), // HeadersSyncParams field must be initialized to non-zero.
 FastRandomContext().randrange(params.commitment_period))),
 m_id(id),
 m_consensus_params(consensus_params),
 m_params(params),
 m_chain_start(chain_start),
 m_minimum_required_work(minimum_required_work),
 m_current_chain_work(chain_start.nChainWork),
 m_last_header_received(m_chain_start.GetBlockHeader()),
 m_current_height(chain_start.nHeight)
{
 const auto max_seconds_since_start{(Ticks<std::chrono::seconds>(NodeClock::now() - NodeSeconds{std::chrono::seconds{chain_start.GetMedianTimePast()}}))
 + MAX_FUTURE_BLOCK_TIME};
 m_max_commitments = 6 * max_seconds_since_start / m_params.commitment_period;

 LogDebug(BCLog::NET, "Initial headers sync started with peer=%d: height=%i, max_commitments=%i, min_work=%s\n", m_id, m_current_height, m_max_commitments, m_minimum_required_work.ToString());
}

/** Free memory and mark object unusable; prevents reuse with same SaltedUint256Hasher. */
void HeadersSyncState::Finalize()
{
 Assume(m_download_state != State::FINAL);
 ClearShrink(m_header_commitments);
 m_last_header_received.SetNull();
 ClearShrink(m_redownloaded_headers);
 m_redownload_buffer_last_hash.SetNull();
 m_redownload_buffer_first_prev_hash.SetNull();
 m_process_all_remaining_headers = false;
 m_current_height = 0;

 m_download_state = State::FINAL;
}

/** Process next headers batch: validate/store commitments, switch to REDOWNLOAD on sufficient work. */
HeadersSyncState::ProcessingResult HeadersSyncState::ProcessNextHeaders(
 std::span<const CBlockHeader> received_headers, const bool full_headers_message)
{
 ProcessingResult ret;

 Assume(!received_headers.empty());
 if (received_headers.empty()) return ret;

 Assume(m_download_state != State::FINAL);
 if (m_download_state == State::FINAL) return ret;

 if (m_download_state == State::PRESYNC) {
 // PRESYNC: minimally validate headers and store commitments until work threshold reached.
 ret.success = ValidateAndStoreHeadersCommitments(received_headers);
 if (ret.success) {
 if (full_headers_message || m_download_state == State::REDOWNLOAD) {
 // Full message: peer may have more; or just switched to REDOWNLOAD: re-request from start.
 ret.request_more = true;
 } else {
 Assume(m_download_state == State::PRESYNC);
 // PRESYNC non-full message: peer's chain ended, insufficient work, stop sync.
 LogDebug(BCLog::NET, "Initial headers sync aborted with peer=%d: incomplete headers message at height=%i (presync phase)\n", m_id, m_current_height);
 }
 }
 } else if (m_download_state == State::REDOWNLOAD) {
 // REDOWNLOAD: verify commitments against received headers, buffer them, release batches.
 ret.success = true;
 for (const auto& hdr : received_headers) {
 if (!ValidateAndStoreRedownloadedHeader(hdr)) {
 // Peer gave unexpected chain; give up on sync (could punish peer in future).
 ret.success = false;
 break;
 }
 }

 if (ret.success) {
 // Return any headers that are ready for acceptance.
 ret.pow_validated_headers = PopHeadersReadyForAcceptance();

 // If we hit our target blockhash, then all remaining headers will be
 // returned and we can clear any leftover internal state.
 if (m_redownloaded_headers.empty() && m_process_all_remaining_headers) {
 LogDebug(BCLog::NET, "Initial headers sync complete with peer=%d: releasing all at height=%i (redownload phase)\n", m_id, m_redownload_buffer_last_height);
 } else if (full_headers_message) {
 // If the headers message is full, we need to request more.
 ret.request_more = true;
 } else {
 // Peer declines to re-serve high-work chain; give up. Headers already processed, return success.
 LogDebug(BCLog::NET, "Initial headers sync aborted with peer=%d: incomplete headers message at height=%i (redownload phase)\n", m_id, m_redownload_buffer_last_height);
 }
 }
 }

 if (!(ret.success && ret.request_more)) Finalize();
 return ret;
}

bool HeadersSyncState::ValidateAndStoreHeadersCommitments(std::span<const CBlockHeader> headers)
{
 // The caller should not give us an empty set of headers.
 Assume(headers.size() > 0);
 if (headers.size() == 0) return true;

 Assume(m_download_state == State::PRESYNC);
 if (m_download_state != State::PRESYNC) return false;

 if (headers[0].hashPrevBlock != m_last_header_received.GetHash()) {
 // Non-connecting header: peer may have reorged; give up (will restart with new start point).
 LogDebug(BCLog::NET, "Initial headers sync aborted with peer=%d: non-continuous headers at height=%i (presync phase)\n", m_id, m_current_height);
 return false;
 }

 // If it does connect, (minimally) validate and occasionally store
 // commitments.
 for (const auto& hdr : headers) {
 if (!ValidateAndProcessSingleHeader(hdr)) {
 return false;
 }
 }

 if (m_current_chain_work >= m_minimum_required_work) {
 m_redownloaded_headers.clear();
 m_redownload_buffer_last_height = m_chain_start.nHeight;
 m_redownload_buffer_first_prev_hash = m_chain_start.GetBlockHash();
 m_redownload_buffer_last_hash = m_chain_start.GetBlockHash();
 m_redownload_chain_work = m_chain_start.nChainWork;
 m_download_state = State::REDOWNLOAD;
 LogDebug(BCLog::NET, "Initial headers sync transition with peer=%d: reached sufficient work at height=%i, redownloading from height=%i\n", m_id, m_current_height, m_redownload_buffer_last_height);
 }
 return true;
}

bool HeadersSyncState::ValidateAndProcessSingleHeader(const CBlockHeader& current)
{
 Assume(m_download_state == State::PRESYNC);
 if (m_download_state != State::PRESYNC) return false;

 int next_height = m_current_height + 1;

 // Reject difficulty growing too fast: adversary could compress work into few blocks.
 if (!PermittedDifficultyTransition(m_consensus_params, next_height,
 m_last_header_received.nBits, current.nBits)) {
 LogDebug(BCLog::NET, "Initial headers sync aborted with peer=%d: invalid difficulty transition at height=%i (presync phase)\n", m_id, next_height);
 return false;
 }

 if (next_height % m_params.commitment_period == m_commit_offset) {
 // Add a commitment.
 m_header_commitments.push_back(m_hasher(current.GetHash()) & 1);
 if (m_header_commitments.size() > m_max_commitments) {
 // Chain too long (may have grown since sync start); retry later may succeed.
 LogDebug(BCLog::NET, "Initial headers sync aborted with peer=%d: exceeded max commitments at height=%i (presync phase)\n", m_id, next_height);
 return false;
 }
 }

 m_current_chain_work += GetBlockProof(current);
 m_last_header_received = current;
 m_current_height = next_height;

 return true;
}

bool HeadersSyncState::ValidateAndStoreRedownloadedHeader(const CBlockHeader& header)
{
 Assume(m_download_state == State::REDOWNLOAD);
 if (m_download_state != State::REDOWNLOAD) return false;

 int64_t next_height = m_redownload_buffer_last_height + 1;

 // Ensure that we're working on a header that connects to the chain we're
 // downloading.
 if (header.hashPrevBlock != m_redownload_buffer_last_hash) {
 LogDebug(BCLog::NET, "Initial headers sync aborted with peer=%d: non-continuous headers at height=%i (redownload phase)\n", m_id, next_height);
 return false;
 }

 // Check that the difficulty adjustments are within our tolerance:
 uint32_t previous_nBits{0};
 if (!m_redownloaded_headers.empty()) {
 previous_nBits = m_redownloaded_headers.back().nBits;
 } else {
 previous_nBits = m_chain_start.nBits;
 }

 if (!PermittedDifficultyTransition(m_consensus_params, next_height,
 previous_nBits, header.nBits)) {
 LogDebug(BCLog::NET, "Initial headers sync aborted with peer=%d: invalid difficulty transition at height=%i (redownload phase)\n", m_id, next_height);
 return false;
 }

 // Track work on the redownloaded chain
 m_redownload_chain_work += GetBlockProof(header);

 if (m_redownload_chain_work >= m_minimum_required_work) {
 m_process_all_remaining_headers = true;
 }

 // Verify stored commitment at commitment-period headers; skip after target reached (peer may have extended chain).
 if (!m_process_all_remaining_headers && next_height % m_params.commitment_period == m_commit_offset) {
 if (m_header_commitments.size() == 0) {
 LogDebug(BCLog::NET, "Initial headers sync aborted with peer=%d: commitment overrun at height=%i (redownload phase)\n", m_id, next_height);
 // Peer fed a different chain; commitments exhausted.
 return false;
 }
 bool commitment = m_hasher(header.GetHash()) & 1;
 bool expected_commitment = m_header_commitments.front();
 m_header_commitments.pop_front();
 if (commitment != expected_commitment) {
 LogDebug(BCLog::NET, "Initial headers sync aborted with peer=%d: commitment mismatch at height=%i (redownload phase)\n", m_id, next_height);
 return false;
 }
 }

 // Store this header for later processing.
 m_redownloaded_headers.emplace_back(header);
 m_redownload_buffer_last_height = next_height;
 m_redownload_buffer_last_hash = header.GetHash();

 return true;
}

std::vector<CBlockHeader> HeadersSyncState::PopHeadersReadyForAcceptance()
{
 std::vector<CBlockHeader> ret;

 Assume(m_download_state == State::REDOWNLOAD);
 if (m_download_state != State::REDOWNLOAD) return ret;

 while (m_redownloaded_headers.size() > m_params.redownload_buffer_size ||
 (m_redownloaded_headers.size() > 0 && m_process_all_remaining_headers)) {
 ret.emplace_back(m_redownloaded_headers.front().GetFullHeader(m_redownload_buffer_first_prev_hash));
 m_redownloaded_headers.pop_front();
 m_redownload_buffer_first_prev_hash = ret.back().GetHash();
 }
 return ret;
}

CBlockLocator HeadersSyncState::NextHeadersRequestLocator() const
{
 Assume(m_download_state != State::FINAL);
 if (m_download_state == State::FINAL) return {};

 auto chain_start_locator = LocatorEntries(&m_chain_start);
 std::vector<uint256> locator;

 if (m_download_state == State::PRESYNC) {
 // During pre-synchronization, we continue from the last header received.
 locator.push_back(m_last_header_received.GetHash());
 }

 if (m_download_state == State::REDOWNLOAD) {
 // During redownload, we will download from the last received header that we stored.
 locator.push_back(m_redownload_buffer_last_hash);
 }

 locator.insert(locator.end(), chain_start_locator.begin(), chain_start_locator.end());

 return CBlockLocator{std::move(locator)};
}
