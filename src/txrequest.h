// Copyright (c) 2020-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TKN_TXREQUEST_H
#define TKN_TXREQUEST_H

#include <primitives/transaction.h>
#include <net.h>
#include <uint256.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

/** Data structure to keep track of, and schedule, transaction downloads from peers. === Specification === Tracks which peers announced which transactions, and uses that to determine which requests should go to which peer, when, and in what order. Per peer/tx combination ("announcement"), the following information is tracked: peer (NodeId); txhash (txid or wtxid, see BIP339); tx vs wtx announcement; reqtime (earliest time it can be requested from that peer); preferred flag (for outbound/higher-trust peersticky even if preferredness changes); expiry (when a requested transaction times out); failure flag (timed out, invalid, or NOTFOUND received). Transaction requests are assigned to peers, following these rules: (1) No transaction requested while another request for the same txhash is outstanding (must fail by expiry, NOTFOUND, or invalid first). Rationale: avoid wasting bandwidth on multiple copies. Works per txhashf same tx announced via txid and wtxid, no way to prevent fetching both (caller can mitigate by delaying one). (2) Same transaction never requested twice from same peer unless forgotten and re-announced. Announcements are forgotten only: peer goes offline (all its); tx successfully received or no longer needed (caller calls ForgetTxHash, removes all across all peers with that txhash); for a given txhash, only already-failed remain (all forgotten). Rationale: giving a peer multiple chances would allow them to bias requests in their favor (worsening censoring attacks). Flip side: as long as attacker prevents us from receiving a tx, failed announcements (incl. honest peers) linger, increasing memorympact limited by cap on tracked announcements per peer; should be rare in practice. Censoring attacks: announce txs quickly, don't answer requests. See https://allquantor.at/blockchainbib/pdf/miller2015topology.pdf. (3) Transactions not requested from a peer until its reqtime has passed. Rationale: enable calling code to define a delay for less-than-ideal peers, so presumed-better peers have a chance to announce first. (4) If multiple viable candidate peers exist, pick: (a) preferred peers only if any are available. Rationale: more trusted, less likely attacker-controlled. (b) Uniformly random among candidates. Rationale: random assignments hard to influence. Together these rules balance fast in non-adversarial conditions and minimize susceptibility to censorship. Attacker that races the network: (a) unsuccessful if all preferred connections honest (>=1 preferred). (b) If P preferred connections with Ph>=1 honest, attacker can delay learning a tx by k expiration periods, k ~ 1 + NHG(N=P-1,K=P-Ph-1,r=1), mean P/(Ph+1) (NHG = Negative Hypergeometric). The "1 +" is because attacker can be the first to announce through a preferred connection in this scenario (very likely means first request). (c) If all P preferred connections to attacker, NP non-preferred of which NPh>=1 honest (attacker can disconnect/reconnect), distribution k ~ P + NB(p=1-NPh/NP,r=1) (NB = Negative Binomial), mean P-1+NP/NPh. Complexity: (a) Memory = O(total tracked announcements (Size()) + peers with non-zero count). (b) CPU generally logarithmic in total tracked, plus O(1) amortized per affected announcement. Context: Earlier version of the request logic allowed a peer to prevent us from seeing a specific tx. See https://bitcoincore.org/en/2024/07/03/disclose_already_asked_for. */
class TxRequestTracker {
 // Avoid littering this header file with implementation details.
 class Impl;
 const std::unique_ptr<Impl> m_impl;

public:
 //! Construct a TxRequestTracker.
 explicit TxRequestTracker(bool deterministic = false);
 ~TxRequestTracker();

 // Conceptually, the data structure consists of a collection of "announcements", one for each peer/txhash combination: (1) CANDIDATE announcements represent transactions that were announced by a peer, and that become available for download after their reqtime has passed. (2) REQUESTED announcements represent transactions that have been requested, and which we're awaiting a response for from that peer. Their expiry value determines when the request times out. (3) COMPLETED announcements represent transactions that have been requested from a peer, and a NOTFOUND or a transaction was received in response (valid or not), or they timed out. They're only kept around to prevent requesting them again. If only COMPLETED announcements for a given txhash remain (so no CANDIDATE or REQUESTED ones), all of them are deleted (invariant, maintained by all operations below).

 // The operations below manipulate the data structure.

 /** Adds a new CANDIDATE announcement. Does nothing if one already exists for that (txhash, peer) combination (whether CANDIDATE, REQUESTED, or COMPLETED). Note: txid/wtxid property is ignored for determining uniqueness, so if an announcement is added for wtxid H, while one for txid H from the same peer already exists, it will be ignored. This is harmless as txhashes being equal implies a non-segwit tx, so it doesn't matter how it is fetched. The new announcement is given the specified preferred and reqtime values, and takes its is_wtxid from the specified gtxid. */
 void ReceivedInv(NodeId peer, const GenTxid& gtxid, bool preferred,
 std::chrono::microseconds reqtime);

 /** Deletes all announcements for a given peer. It should be called when a peer goes offline. */
 void DisconnectedPeer(NodeId peer);

 /** Deletes all announcements for a given txhash (both txid and wtxid ones). This should be called when a transaction is no longer needed. The caller should ensure that new announcements for the same txhash will not trigger new ReceivedInv calls, at least in the short term after this call. */
 void ForgetTxHash(const uint256& txhash);

 /** Find the txids to request now from peer. It does the following: (1) Convert all REQUESTED announcements (for all txhashes/peers) with (expiry <= now) to COMPLETED ones. These are returned in expired, if non-nullptr. (2) Requestable announcements are selected: CANDIDATE announcements from the specified peer with (reqtime <= now) for which no existing REQUESTED announcement with the same txhash from a different peer exists, and for which the specified peer is the best choice among all (reqtime <= now) CANDIDATE announcements with the same txhash (subject to preferredness rules, and tiebreaking using a deterministic salted hash of peer and txhash). (3) The selected announcements are returned in announcement order (even if multiple were added at the same time, or when the clock went backwards while they were being added). This is done to minimize disruption from dependent transactions being requested out of order: if multiple dependent transactions are announced simultaneously by one peer, and end up being requested from them, the requests will happen in announcement order. */
 std::vector<GenTxid> GetRequestable(NodeId peer, std::chrono::microseconds now,
 std::vector<std::pair<NodeId, GenTxid>>* expired = nullptr);

 /** Marks a transaction as requested, with a specified expiry. If no CANDIDATE announcement for the provided peer and txhash exists, this call has no effect. Otherwise: (1) That announcement is converted to REQUESTED. (2) If any other REQUESTED announcement for the same txhash already existed, it means an unexpected request was made (GetRequestable will never advise doing so). In this case it is converted to COMPLETED, as we're no longer waiting for a response to it. */
 void RequestedTx(NodeId peer, const uint256& txhash, std::chrono::microseconds expiry);

 /** Converts a CANDIDATE or REQUESTED announcement to a COMPLETED one. If no such announcement exists for the provided peer and txhash, nothing happens. It should be called whenever a transaction or NOTFOUND was received from a peer. When the transaction is not needed entirely anymore, ForgetTxhash should be called instead of, or in addition to, this call. */
 void ReceivedResponse(NodeId peer, const uint256& txhash);

 // The operations below inspect the data structure.

 /** Count how many REQUESTED announcements a peer has. */
 size_t CountInFlight(NodeId peer) const;

 /** Count how many CANDIDATE announcements a peer has. */
 size_t CountCandidates(NodeId peer) const;

 /** Count how many announcements a peer has (REQUESTED, CANDIDATE, and COMPLETED combined). */
 size_t Count(NodeId peer) const;

 /** Count how many announcements are being tracked in total across all peers and transaction hashes. */
 size_t Size() const;

 /** For some txhash (txid or wtxid), finds all peers with non-COMPLETED announcements and appends them to
 * result_peers. Does not try to ensure that result_peers contains no duplicates. */
 void GetCandidatePeers(const uint256& txhash, std::vector<NodeId>& result_peers) const;

 /** Access to the internal priority computation (testing only) */
 uint64_t ComputePriority(const uint256& txhash, NodeId peer, bool preferred) const;

 /** Run internal consistency check (testing only). */
 void SanityCheck() const;

 /** Run a time-dependent internal consistency check (testing only). This can only be called immediately after GetRequestable, with the same 'now' parameter. */
 void PostGetRequestableSanityCheck(std::chrono::microseconds now) const;
};

#endif // TKN_TXREQUEST_H
