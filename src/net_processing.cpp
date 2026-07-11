// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <net_processing.h>

#include <net/message.h>  // MessageTypes::APIKEYSYNC
#include <future>
#include <addrman.h>
#include <apikey/api_key.h>
#include <rpc/escrow_rpc.h>  // WriteSpendingLimitFromP2P for ESCROWSYNC handler
#include <arith_uint256.h>
#include <banman.h>
#include <blockencodings.h>
#include <blockfilter.h>
#include <chain.h>
#include <chainparams.h>
#include <common/bloom.h>
#include <consensus/amount.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <core_memusage.h>
#include <crypto/siphash.h>
#include <deploymentstatus.h>
#include <flatfile.h>
#include <headerssync.h>
#include <index/blockfilterindex.h>
#include <kernel/types.h>
#include <logging.h>
#include <merkleblock.h>
#include <net.h>
#include <net/api_protocol.h>
#include <net/inference_engine.h>
#include <net/miner_registry.h>
#include <net/message.h>
#include <net_permissions.h>
#include <netaddress.h>
#include <netbase.h>
#include <netmessagemaker.h>
#include <node/blockstorage.h>
#include <node/connection_types.h>
#include <node/protocol_version.h>
#include <node/timeoffsets.h>
#include <node/txdownloadman.h>
#include <node/txorphanage.h>
#include <node/txreconciliation.h>
#include <node/warnings.h>
#include <policy/feerate.h>
#include <policy/fees/block_policy_estimator.h>
#include <policy/packages.h>
#include <policy/policy.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <private_broadcast.h>
#include <protocol.h>
#include <random.h>
#include <scheduler.h>
#include <script/script.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <sync.h>
#include <tinyformat.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/check.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <util/trace.h>
#include <validation.h>

#include <algorithm>
#include <array>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define closesocket close
#ifndef SOCKET
#define SOCKET int
#endif
#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#endif
#include <atomic>
#include <compare>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <ranges>
#include <ratio>
#include <set>
#include <span>
#include <typeinfo>
#include <utility>

using kernel::ChainstateRole;
using namespace util::hex_literals;

TRACEPOINT_SEMAPHORE(net, inbound_message);
TRACEPOINT_SEMAPHORE(net, misbehaving_connection);

/** Headers download timeout.
 *  Timeout = base + per_header * (expected number of headers) */
static constexpr auto HEADERS_DOWNLOAD_TIMEOUT_BASE = 15min;
static constexpr auto HEADERS_DOWNLOAD_TIMEOUT_PER_HEADER = 1ms;
/** How long to wait for a peer to respond to a getheaders request */
static constexpr auto HEADERS_RESPONSE_TIME{2min};
/** Protect at least this many outbound peers from disconnection due to slow/
 * behind headers chain.
 */
static constexpr int32_t MAX_OUTBOUND_PEERS_TO_PROTECT_FROM_DISCONNECT = 4;
/** Timeout for (unprotected) outbound peers to sync to our chainwork */
static constexpr auto CHAIN_SYNC_TIMEOUT{20min};
/** How frequently to check for stale tips */
static constexpr auto STALE_CHECK_INTERVAL{10min};
/** How frequently to check for extra outbound peers and disconnect */
static constexpr auto EXTRA_PEER_CHECK_INTERVAL{45s};
/** Minimum time an outbound-peer-eviction candidate must be connected for, in order to evict */
static constexpr auto MINIMUM_CONNECT_TIME{30s};
/** SHA256("main address relay")[0:8] */
static constexpr uint64_t RANDOMIZER_ID_ADDRESS_RELAY = 0x3cac0035b5866b90ULL;
/// Age after which a stale block will no longer be served if requested as
/// protection against fingerprinting. Set to one month, denominated in seconds.
static constexpr int STALE_RELAY_AGE_LIMIT = 30 * 24 * 60 * 60;
/// Age after which a block is considered historical for purposes of rate
/// limiting block relay. Set to one week, denominated in seconds.
static constexpr int HISTORICAL_BLOCK_AGE = 7 * 24 * 60 * 60;
/** Time between pings automatically sent out for latency probing and keepalive */
static constexpr auto PING_INTERVAL{2min};
/** The maximum number of entries in a locator */
static const unsigned int MAX_LOCATOR_SZ = 101;
/** The maximum number of entries in an 'inv' protocol message */
static const unsigned int MAX_INV_SZ = 50000;
/** Limit to avoid sending big packets. Not used in processing incoming GETDATA for compatibility */
static const unsigned int MAX_GETDATA_SZ = 1000;
/** Number of blocks that can be requested at any given time from a single peer. */
static const int MAX_BLOCKS_IN_TRANSIT_PER_PEER = 16;
/** Default time during which a peer must stall block download progress before being disconnected.
 * the actual timeout is increased temporarily if peers are disconnected for hitting the timeout */
static constexpr auto BLOCK_STALLING_TIMEOUT_DEFAULT{2s};
/** Maximum timeout for stalling block download. */
static constexpr auto BLOCK_STALLING_TIMEOUT_MAX{64s};
/** Maximum depth of blocks we're willing to serve as compact blocks to peers
 *  when requested. For older blocks, a regular BLOCK response will be sent. */
static const int MAX_CMPCTBLOCK_DEPTH = 5;
/** Maximum depth of blocks we're willing to respond to GETBLOCKTXN requests for. */
static const int MAX_BLOCKTXN_DEPTH = 10;
static_assert(MAX_BLOCKTXN_DEPTH <= MIN_BLOCKS_TO_KEEP, "MAX_BLOCKTXN_DEPTH too high");
/** Size of the "block download window": how far ahead of our current height do we fetch?
 *  Larger windows tolerate larger download speed differences between peer, but increase the potential
 *  degree of disordering of blocks on disk (which make reindexing and pruning harder). We'll probably
 *  want to make this a per-peer adaptive value at some point. */
static const unsigned int BLOCK_DOWNLOAD_WINDOW = 1024;
/** Block download timeout base, expressed in multiples of the block interval (i.e. 10 min) */
static constexpr double BLOCK_DOWNLOAD_TIMEOUT_BASE = 1;
/** Additional block download timeout per parallel downloading peer (i.e. 5 min) */
static constexpr double BLOCK_DOWNLOAD_TIMEOUT_PER_PEER = 0.5;
/** Maximum number of headers to announce when relaying blocks with headers message.*/
static const unsigned int MAX_BLOCKS_TO_ANNOUNCE = 8;
/** Minimum blocks required to signal NODE_NETWORK_LIMITED */
static const unsigned int NODE_NETWORK_LIMITED_MIN_BLOCKS = 288;
/** Window, in blocks, for connecting to NODE_NETWORK_LIMITED peers */
static const unsigned int NODE_NETWORK_LIMITED_ALLOW_CONN_BLOCKS = 144;
/** Average delay between local address broadcasts */
static constexpr auto AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL{24h};
/** Average delay between peer address broadcasts */
static constexpr auto AVG_ADDRESS_BROADCAST_INTERVAL{30s};
/** Delay between rotating the peers we relay a particular address to */
static constexpr auto ROTATE_ADDR_RELAY_DEST_INTERVAL{24h};
/** Average delay between trickled inventory transmissions for inbound peers.
 *  Blocks and peers with NetPermissionFlags::NoBan permission bypass this. */
static constexpr auto INBOUND_INVENTORY_BROADCAST_INTERVAL{5s};
/** Average delay between trickled inventory transmissions for outbound peers.
 *  Use a smaller delay as there is less privacy concern for them.
 *  Blocks and peers with NetPermissionFlags::NoBan permission bypass this. */
static constexpr auto OUTBOUND_INVENTORY_BROADCAST_INTERVAL{2s};
/** Maximum rate of inventory items to send per second.
 *  Limits the impact of low-fee transaction floods. */
static constexpr unsigned int INVENTORY_BROADCAST_PER_SECOND{14};
/** Target number of tx inventory items to send per transmission. */
static constexpr unsigned int INVENTORY_BROADCAST_TARGET = INVENTORY_BROADCAST_PER_SECOND * count_seconds(INBOUND_INVENTORY_BROADCAST_INTERVAL);
/** Maximum number of inventory items to send per transmission. */
static constexpr unsigned int INVENTORY_BROADCAST_MAX = 1000;
static_assert(INVENTORY_BROADCAST_MAX >= INVENTORY_BROADCAST_TARGET, "INVENTORY_BROADCAST_MAX too low");
static_assert(INVENTORY_BROADCAST_MAX <= node::MAX_PEER_TX_ANNOUNCEMENTS, "INVENTORY_BROADCAST_MAX too high");
/** Average delay between feefilter broadcasts in seconds. */
static constexpr auto AVG_FEEFILTER_BROADCAST_INTERVAL{10min};
/** Maximum feefilter broadcast delay after significant change. */
static constexpr auto MAX_FEEFILTER_CHANGE_DELAY{5min};
/** Maximum number of compact filters that may be requested with one getcfilters. See BIP 157. */
static constexpr uint32_t MAX_GETCFILTERS_SIZE = 1000;
/** Maximum number of cf hashes that may be requested with one getcfheaders. See BIP 157. */
static constexpr uint32_t MAX_GETCFHEADERS_SIZE = 2000;
/** the maximum percentage of addresses from our addrman to return in response to a getaddr message. */
static constexpr size_t MAX_PCT_ADDR_TO_SEND = 23;
/** The maximum number of address records permitted in an ADDR message. */
static constexpr size_t MAX_ADDR_TO_SEND{1000};
/** The maximum rate of address records we're willing to process on average. Can be bypassed using
 *  the NetPermissionFlags::Addr permission. */
static constexpr double MAX_ADDR_RATE_PER_SECOND{0.1};
/** The soft limit of the address processing token bucket (the regular MAX_ADDR_RATE_PER_SECOND
 *  based increments won't go above this, but the MAX_ADDR_TO_SEND increment following GETADDR
 *  is exempt from this limit). */
static constexpr size_t MAX_ADDR_PROCESSING_TOKEN_BUCKET{MAX_ADDR_TO_SEND};
/** The compactblocks version we support. See BIP 152. */
static constexpr uint64_t CMPCTBLOCKS_VERSION{2};
/** For private broadcast, send a transaction to this many peers. */
static constexpr size_t NUM_PRIVATE_BROADCAST_PER_TX{3};
/** Private broadcast connections must complete within this time. Disconnect the peer if it takes longer. */
static constexpr auto PRIVATE_BROADCAST_MAX_CONNECTION_LIFETIME{3min};

// Internal stuff
namespace {
/** Blocks that are in flight, and that are in the queue to be downloaded. */
struct QueuedBlock {
    /** BlockIndex. We must have this since we only request blocks when we've already validated the header. */
    const CBlockIndex* pindex;
    /** Optional, used for CMPCTBLOCK downloads */
    std::unique_ptr<PartiallyDownloadedBlock> partialBlock;
};

/**
 * Data structure for an individual peer. This struct is not protected by
 * cs_main since it does not contain validation-critical data.
 *
 * Memory is owned by shared pointers and this object is destructed when
 * the refcount drops to zero.
 *
 * Mutexes inside this struct must not be held when locking m_peer_mutex.
 *
 * TODO: move most members from CNodeState to this structure.
 * TODO: move remaining application-layer data members from CNode to this structure.
 */
struct Peer {
    /** Same id as the CNode object for this peer */
    const NodeId m_id{0};

    /** Services we offered to this peer.
     *
     *  This is supplied by CConnman during peer initialization. It's const
     *  because there is no protocol defined for renegotiating services
     *  initially offered to a peer. The set of local services we offer should
     *  not change after initialization.
     *
     *  An interesting example of this is NODE_NETWORK and initial block
     *  download: a node which starts up from scratch doesn't have any blocks
     *  to serve, but still advertises NODE_NETWORK because it will eventually
     *  fulfill this role after IBD completes. P2P code is written in such a
     *  way that it can gracefully handle peers who don't make good on their
     *  service advertisements. */
    const ServiceFlags m_our_services;
    /** Services this peer offered to us. */
    std::atomic<ServiceFlags> m_their_services{NODE_NONE};

    //! Whether this peer is an inbound connection
    const bool m_is_inbound;

    /** Protects misbehavior data members */
    Mutex m_misbehavior_mutex;
    /** Whether this peer should be disconnected and marked as discouraged (unless it has NetPermissionFlags::NoBan permission). */
    bool m_should_discourage GUARDED_BY(m_misbehavior_mutex){false};

    /** Protects block inventory data members */
    Mutex m_block_inv_mutex;
    /** List of blocks that we'll announce via an `inv` message.
     * There is no final sorting before sending, as they are always sent
     * immediately and in the order requested. */
    std::vector<uint256> m_blocks_for_inv_relay GUARDED_BY(m_block_inv_mutex);
    /** Unfiltered list of blocks that we'd like to announce via a `headers`
     * message. If we can't announce via a `headers` message, we'll fall back to
     * announcing via `inv`. */
    std::vector<uint256> m_blocks_for_headers_relay GUARDED_BY(m_block_inv_mutex);
    /** The final block hash that we sent in an `inv` message to this peer.
     * When the peer requests this block, we send an `inv` message to trigger
     * the peer to request the next sequence of block hashes.
     * Most peers use headers-first syncing, which doesn't use this mechanism */
    uint256 m_continuation_block GUARDED_BY(m_block_inv_mutex) {};

    /** Set to true once initial VERSION message was sent (only relevant for outbound peers). */
    bool m_outbound_version_message_sent GUARDED_BY(NetEventsInterface::g_msgproc_mutex){false};

    /** The pong reply we're expecting, or 0 if no pong expected. */
    std::atomic<uint64_t> m_ping_nonce_sent{0};
    /** When the last ping was sent, or 0 if no ping was ever sent */
    std::atomic<NodeClock::time_point> m_ping_start{NodeClock::epoch};
    /** Whether a ping has been requested by the user */
    std::atomic<bool> m_ping_queued{false};

    /** Whether this peer relays txs via wtxid */
    std::atomic<bool> m_wtxid_relay{false};
    /** The feerate in the most recent BIP133 `feefilter` message sent to the peer.
     *  It is *not* a p2p protocol violation for the peer to send us
     *  transactions with a lower fee rate than this. See BIP133. */
    CAmount m_fee_filter_sent GUARDED_BY(NetEventsInterface::g_msgproc_mutex){0};
    /** Timestamp after which we will send the next BIP133 `feefilter` message
      * to the peer. */
    std::chrono::microseconds m_next_send_feefilter GUARDED_BY(NetEventsInterface::g_msgproc_mutex){0};

    struct TxRelay {
        mutable RecursiveMutex m_bloom_filter_mutex;
        /** Whether we relay transactions to this peer. */
        bool m_relay_txs GUARDED_BY(m_bloom_filter_mutex){false};
        /** A bloom filter for which transactions to announce to the peer. See BIP37. */
        std::unique_ptr<CBloomFilter> m_bloom_filter PT_GUARDED_BY(m_bloom_filter_mutex) GUARDED_BY(m_bloom_filter_mutex){nullptr};

        mutable RecursiveMutex m_tx_inventory_mutex;
        /** A filter of all the (w)txids that the peer has announced to
         *  us or we have announced to the peer. We use this to avoid announcing
         *  the same (w)txid to a peer that already has the transaction. */
        CRollingBloomFilter m_tx_inventory_known_filter GUARDED_BY(m_tx_inventory_mutex){50000, 0.000001};
        /** Set of wtxids we still have to announce. For non-wtxid-relay peers,
         *  we retrieve the txid from the corresponding mempool transaction when
         *  constructing the `inv` message. We use the mempool to sort transactions
         *  in dependency order before relay, so this does not have to be sorted. */
        std::set<Wtxid> m_tx_inventory_to_send GUARDED_BY(m_tx_inventory_mutex);
        /** Whether the peer has requested us to send our complete mempool. Only
         *  permitted if the peer has NetPermissionFlags::Mempool or we advertise
         *  NODE_BLOOM. See BIP35. */
        bool m_send_mempool GUARDED_BY(m_tx_inventory_mutex){false};
        /** The next time after which we will send an `inv` message containing
         *  transaction announcements to this peer. */
        std::chrono::microseconds m_next_inv_send_time GUARDED_BY(m_tx_inventory_mutex){0};
        /** The mempool sequence num at which we sent the last `inv` message to this peer.
         *  Can relay txs with lower sequence numbers than this (see CTxMempool::info_for_relay). */
        uint64_t m_last_inv_sequence GUARDED_BY(m_tx_inventory_mutex){1};

        /** Minimum fee rate with which to filter transaction announcements to this node. See BIP133. */
        std::atomic<CAmount> m_fee_filter_received{0};
    };

    /* Initializes a TxRelay struct for this peer. Can be called at most once for a peer. */
    TxRelay* SetTxRelay() EXCLUSIVE_LOCKS_REQUIRED(!m_tx_relay_mutex)
    {
        LOCK(m_tx_relay_mutex);
        Assume(!m_tx_relay);
        m_tx_relay = std::make_unique<Peer::TxRelay>();
        return m_tx_relay.get();
    };

    TxRelay* GetTxRelay() EXCLUSIVE_LOCKS_REQUIRED(!m_tx_relay_mutex)
    {
        return WITH_LOCK(m_tx_relay_mutex, return m_tx_relay.get());
    };

    /** A vector of addresses to send to the peer, limited to MAX_ADDR_TO_SEND. */
    std::vector<CAddress> m_addrs_to_send GUARDED_BY(NetEventsInterface::g_msgproc_mutex);
    /** Probabilistic filter to track recent addr messages relayed with this
     *  peer. Used to avoid relaying redundant addresses to this peer.
     *
     *  We initialize this filter for outbound peers (other than
     *  block-relay-only connections) or when an inbound peer sends us an
     *  address related message (ADDR, ADDRV2, GETADDR).
     *
     *  Presence of this filter must correlate with m_addr_relay_enabled.
     **/
    std::unique_ptr<CRollingBloomFilter> m_addr_known GUARDED_BY(NetEventsInterface::g_msgproc_mutex);
    /** Whether we are participating in address relay with this connection.
     *
     *  We set this bool to true for outbound peers (other than
     *  block-relay-only connections), or when an inbound peer sends us an
     *  address related message (ADDR, ADDRV2, GETADDR).
     *
     *  We use this bool to decide whether a peer is eligible for gossiping
     *  addr messages. This avoids relaying to peers that are unlikely to
     *  forward them, effectively blackholing self announcements. Reasons
     *  peers might support addr relay on the link include that they connected
     *  to us as a block-relay-only peer or they are a light client.
     *
     *  This field must correlate with whether m_addr_known has been
     *  initialized.*/
    std::atomic_bool m_addr_relay_enabled{false};
    /** Whether a getaddr request to this peer is outstanding. */
    bool m_getaddr_sent GUARDED_BY(NetEventsInterface::g_msgproc_mutex){false};
    /** Guards address sending timers. */
    mutable Mutex m_addr_send_times_mutex;
    /** Time point to send the next ADDR message to this peer. */
    std::chrono::microseconds m_next_addr_send GUARDED_BY(m_addr_send_times_mutex){0};
    /** Time point to possibly re-announce our local address to this peer. */
    std::chrono::microseconds m_next_local_addr_send GUARDED_BY(m_addr_send_times_mutex){0};
    /** Whether the peer has signaled support for receiving ADDRv2 (BIP155)
     *  messages, indicating a preference to receive ADDRv2 instead of ADDR ones. */
    std::atomic_bool m_wants_addrv2{false};
    /** Whether this peer has already sent us a getaddr message. */
    bool m_getaddr_recvd GUARDED_BY(NetEventsInterface::g_msgproc_mutex){false};
    /** Number of addresses that can be processed from this peer. Start at 1 to
     *  permit self-announcement. */
    double m_addr_token_bucket GUARDED_BY(NetEventsInterface::g_msgproc_mutex){1.0};
    /** When m_addr_token_bucket was last updated */
    NodeClock::time_point m_addr_token_timestamp GUARDED_BY(NetEventsInterface::g_msgproc_mutex){NodeClock::now()};
    /** Total number of addresses that were dropped due to rate limiting. */
    std::atomic<uint64_t> m_addr_rate_limited{0};
    /** Total number of addresses that were processed (excludes rate-limited ones). */
    std::atomic<uint64_t> m_addr_processed{0};

    /** Whether we've sent this peer a getheaders in response to an inv prior to initial-headers-sync completing */
    bool m_inv_triggered_getheaders_before_sync GUARDED_BY(NetEventsInterface::g_msgproc_mutex){false};

    /** Protects m_getdata_requests **/
    Mutex m_getdata_requests_mutex;
    /** Work queue of items requested by this peer **/
    std::deque<CInv> m_getdata_requests GUARDED_BY(m_getdata_requests_mutex);

    /** Time of the last getheaders message to this peer */
    NodeClock::time_point m_last_getheaders_timestamp GUARDED_BY(NetEventsInterface::g_msgproc_mutex){};

    /** Protects m_headers_sync **/
    Mutex m_headers_sync_mutex;
    /** Headers-sync state for this peer (eg for initial sync, or syncing large
     * reorgs) **/
    std::unique_ptr<HeadersSyncState> m_headers_sync PT_GUARDED_BY(m_headers_sync_mutex) GUARDED_BY(m_headers_sync_mutex) {};

    /** Whether we've sent our peer a sendheaders message. **/
    std::atomic<bool> m_sent_sendheaders{false};

    /** When to potentially disconnect peer for stalling headers download */
    std::chrono::microseconds m_headers_sync_timeout GUARDED_BY(NetEventsInterface::g_msgproc_mutex){0us};

    /** Whether this peer wants invs or headers (when possible) for block announcements */
    bool m_prefers_headers GUARDED_BY(NetEventsInterface::g_msgproc_mutex){false};

    /** Time offset computed during the version handshake based on the
     * timestamp the peer sent in the version message. */
    std::atomic<std::chrono::seconds> m_time_offset{0s};

    explicit Peer(NodeId id, ServiceFlags our_services, bool is_inbound)
        : m_id{id}
        , m_our_services{our_services}
        , m_is_inbound{is_inbound}
    {}

private:
    mutable Mutex m_tx_relay_mutex;

    /** Transaction relay data. May be a nullptr. */
    std::unique_ptr<TxRelay> m_tx_relay GUARDED_BY(m_tx_relay_mutex);
};

using PeerRef = std::shared_ptr<Peer>;

/**
 * Maintain validation-specific state about nodes, protected by cs_main, instead
 * by CNode's own locks. This simplifies asynchronous operation, where
 * processing of incoming data is done after the ProcessMessage call returns,
 * and we're no longer holding the node's locks.
 */
struct CNodeState {
    //! The best known block we know this peer has announced.
    const CBlockIndex* pindexBestKnownBlock{nullptr};
    //! The hash of the last unknown block this peer has announced.
    uint256 hashLastUnknownBlock{};
    //! The last full block we both have.
    const CBlockIndex* pindexLastCommonBlock{nullptr};
    //! The best header we have sent our peer.
    const CBlockIndex* pindexBestHeaderSent{nullptr};
    //! Whether we've started headers synchronization with this peer.
    bool fSyncStarted{false};
    //! Since when we're stalling block download progress (in microseconds), or 0.
    std::chrono::microseconds m_stalling_since{0us};
    std::list<QueuedBlock> vBlocksInFlight;
    //! When the first entry in vBlocksInFlight started downloading. Don't care when vBlocksInFlight is empty.
    std::chrono::microseconds m_downloading_since{0us};
    //! Whether we consider this a preferred download peer.
    bool fPreferredDownload{false};
    /** Whether this peer wants invs or cmpctblocks (when possible) for block announcements. */
    bool m_requested_hb_cmpctblocks{false};
    /** Whether this peer will send us cmpctblocks if we request them. */
    bool m_provides_cmpctblocks{false};

    /** State used to enforce CHAIN_SYNC_TIMEOUT and EXTRA_PEER_CHECK_INTERVAL logic.
      *
      * Both are only in effect for outbound, non-manual, non-protected connections.
      * Any peer protected (m_protect = true) is not chosen for eviction. A peer is
      * marked as protected if all of these are true:
      *   - its connection type is IsBlockOnlyConn() == false
      *   - it gave us a valid connecting header
      *   - we haven't reached MAX_OUTBOUND_PEERS_TO_PROTECT_FROM_DISCONNECT yet
      *   - its chain tip has at least as much work as ours
      *
      * CHAIN_SYNC_TIMEOUT: if a peer's best known block has less work than our tip,
      * set a timeout CHAIN_SYNC_TIMEOUT in the future:
      *   - If at timeout their best known block now has more work than our tip
      *     when the timeout was set, then either reset the timeout or clear it
      *     (after comparing against our current tip's work)
      *   - If at timeout their best known block still has less work than our
      *     tip did when the timeout was set, then send a getheaders message,
      *     and set a shorter timeout, HEADERS_RESPONSE_TIME seconds in future.
      *     If their best known block is still behind when that new timeout is
      *     reached, disconnect.
      *
      * EXTRA_PEER_CHECK_INTERVAL: after each interval, if we have too many outbound peers,
      * drop the outbound one that least recently announced us a new block.
      */
    struct ChainSyncTimeoutState {
        //! A timeout used for checking whether our peer has sufficiently synced
        std::chrono::seconds m_timeout{0s};
        //! A header with the work we require on our peer's chain
        const CBlockIndex* m_work_header{nullptr};
        //! After timeout is reached, set to true after sending getheaders
        bool m_sent_getheaders{false};
        //! Whether this peer is protected from disconnection due to a bad/slow chain
        bool m_protect{false};
    };

    ChainSyncTimeoutState m_chain_sync;

    //! Time of last new block announcement
    int64_t m_last_block_announcement{0};
};

class PeerManagerImpl final : public PeerManager
{
public:
    PeerManagerImpl(CConnman& connman, AddrMan& addrman,
                    BanMan* banman, ChainstateManager& chainman,
                    CTxMemPool& pool, node::Warnings& warnings, Options opts);

    /** Overridden from CValidationInterface. */
    void ActiveTipChange(const CBlockIndex& new_tip, bool) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_tx_download_mutex);
    void BlockConnected(const ChainstateRole& role, const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindexConnected) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_tx_download_mutex);
    void BlockDisconnected(const std::shared_ptr<const CBlock> &block, const CBlockIndex* pindex) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_tx_download_mutex);
    void UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);
    void BlockChecked(const std::shared_ptr<const CBlock>& block, const BlockValidationState& state) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);
    void NewPoWValidBlock(const CBlockIndex *pindex, const std::shared_ptr<const CBlock>& pblock) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_most_recent_block_mutex);

    /** Implement NetEventsInterface */
    void InitializeNode(const CNode& node, ServiceFlags our_services) override EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex, !m_tx_download_mutex);
    void FinalizeNode(const CNode& node) override EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex, !m_headers_presync_mutex, !m_tx_download_mutex);
    bool HasAllDesirableServiceFlags(ServiceFlags services) const override;
    bool ProcessMessages(CNode& node, std::atomic<bool>& interrupt) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex, !m_most_recent_block_mutex, !m_headers_presync_mutex, g_msgproc_mutex, !m_tx_download_mutex);
    bool SendMessages(CNode& node) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex, !m_most_recent_block_mutex, g_msgproc_mutex, !m_tx_download_mutex);

    /** Implement PeerManager */
    void StartScheduledTasks(CScheduler& scheduler) override;
    void CheckForStaleTipAndEvictPeers() override;
    util::Expected<void, std::string> FetchBlock(NodeId peer_id, const CBlockIndex& block_index) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);
    bool GetNodeStateStats(NodeId nodeid, CNodeStateStats& stats) const override EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);
    std::vector<node::TxOrphanage::OrphanInfo> GetOrphanTransactions() override EXCLUSIVE_LOCKS_REQUIRED(!m_tx_download_mutex);
    PeerManagerInfo GetInfo() const override EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);
    std::vector<PrivateBroadcast::TxBroadcastInfo> GetPrivateBroadcastInfo() const override EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);
    std::vector<CTransactionRef> AbortPrivateBroadcast(const uint256& id) override EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);
    void SendPings() override EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);
    void InitiateTxBroadcastToAll(const Txid& txid, const Wtxid& wtxid) override EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);
    void InitiateTxBroadcastPrivate(const CTransactionRef& tx) override EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);
    void SetBestBlock(int height, std::chrono::seconds time) override
    {
        m_best_height = height;
        m_best_block_time = time;
    };
    void UnitTestMisbehaving(NodeId peer_id) override EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex) { Misbehaving(*Assert(GetPeerRef(peer_id)), ""); };
    void UpdateLastBlockAnnounceTime(NodeId node, int64_t time_in_seconds) override;
    ServiceFlags GetDesirableServiceFlags(ServiceFlags services) const override;
    std::future<APIResponse> SendInferenceRequest(NodeId peer_id, const APIRequest& request) override;

    // P2P Miner Advertisement — broadcast local miner info + query peer→miner mapping
    void BroadcastLocalMinerInfo();  // Probe 127.0.0.1:9332 → broadcast MINER_INFO to all peers
    std::vector<std::pair<NodeId, ::MinerAdvertisement>> GetMinerPeers() const override; // Return all known miner peers
    NodeId SelectBestMinerPeer(NodeId requester_id, const std::string& model_hint = "") const override; // Deterministic miner peer selection by score

private:
    void ProcessMessage(Peer& peer, CNode& pfrom, const std::string& msg_type, DataStream& vRecv, NodeClock::time_point time_received,
                        const std::atomic<bool>& interruptMsgProc)
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex, !m_most_recent_block_mutex, !m_headers_presync_mutex, g_msgproc_mutex, !m_tx_download_mutex);

    /** Consider evicting an outbound peer based on the amount of time they've been behind our tip */
    void ConsiderEviction(CNode& pto, Peer& peer, std::chrono::seconds time_in_seconds) EXCLUSIVE_LOCKS_REQUIRED(cs_main, g_msgproc_mutex);

    /** If we have extra outbound peers, try to disconnect the one with the oldest block announcement */
    void EvictExtraOutboundPeers(NodeClock::time_point now) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Retrieve unbroadcast transactions from the mempool and reattempt sending to peers */
    void ReattemptInitialBroadcast(CScheduler& scheduler) EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);

    /** Rebroadcast stale private transactions (already broadcast but not received back from the network). */
    void ReattemptPrivateBroadcast(CScheduler& scheduler);

    /** Get a shared pointer to the Peer object.
     *  May return an empty shared_ptr if the Peer object can't be found. */
    PeerRef GetPeerRef(NodeId id) const EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);

    /** Get a shared pointer to the Peer object and remove it from m_peer_map.
     *  May return an empty shared_ptr if the Peer object can't be found. */
    PeerRef RemovePeer(NodeId id) EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);

    /** Mark a peer as misbehaving, which will cause it to be disconnected and its
     *  address discouraged. */
    void Misbehaving(Peer& peer, const std::string& message);

    /**
     * Potentially mark a node discouraged based on the contents of a BlockValidationState object
     *
     * @param[in] via_compact_block this bool is passed in because net_processing should
     * punish peers differently depending on whether the data was provided in a compact
     * block message or not. If the compact block had a valid header, but contained invalid
     * txs, the peer should not be punished. See BIP 152.
     */
    void MaybePunishNodeForBlock(NodeId nodeid, const BlockValidationState& state,
                                 bool via_compact_block, const std::string& message = "")
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);

    /** Maybe disconnect a peer and discourage future connections from its address.
     *
     * @param[in]   pnode     The node to check.
     * @param[in]   peer      The peer object to check.
     * @return                True if the peer was marked for disconnection in this function
     */
    bool MaybeDiscourageAndDisconnect(CNode& pnode, Peer& peer);

    /** Handle a transaction whose result was not MempoolAcceptResult::ResultType::VALID.
     * @param[in]   first_time_failure            Whether we should consider inserting into vExtraTxnForCompact, adding
     *                                            a new orphan to resolve, or looking for a package to submit.
     *                                            Set to true for transactions just received over p2p.
     *                                            Set to false if the tx has already been rejected before,
     *                                            e.g. is already in the orphanage, to avoid adding duplicate entries.
     * Updates m_txrequest, m_lazy_recent_rejects, m_lazy_recent_rejects_reconsiderable, m_orphanage, and vExtraTxnForCompact.
     *
     * @returns a PackageToValidate if this transaction has a reconsiderable failure and an eligible package was found,
     * or std::nullopt otherwise.
     */
    std::optional<node::PackageToValidate> ProcessInvalidTx(NodeId nodeid, const CTransactionRef& tx, const TxValidationState& result,
                                                      bool first_time_failure)
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex, g_msgproc_mutex, m_tx_download_mutex);

    /** Handle a transaction whose result was MempoolAcceptResult::ResultType::VALID.
     * Updates m_txrequest, m_orphanage, and vExtraTxnForCompact. Also queues the tx for relay. */
    void ProcessValidTx(NodeId nodeid, const CTransactionRef& tx, const std::list<CTransactionRef>& replaced_transactions)
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex, g_msgproc_mutex, m_tx_download_mutex);

    /** Handle the results of package validation: calls ProcessValidTx and ProcessInvalidTx for
     * individual transactions, and caches rejection for the package as a group.
     */
    void ProcessPackageResult(const node::PackageToValidate& package_to_validate, const PackageMempoolAcceptResult& package_result)
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex, g_msgproc_mutex, m_tx_download_mutex);

    /**
     * Reconsider orphan transactions after a parent has been accepted to the mempool.
     *
     * @peer[in]  peer     The peer whose orphan transactions we will reconsider. Generally only
     *                     one orphan will be reconsidered on each call of this function. If an
     *                     accepted orphan has orphaned children, those will need to be
     *                     reconsidered, creating more work, possibly for other peers.
     * @return             True if meaningful work was done (an orphan was accepted/rejected).
     *                     If no meaningful work was done, then the work set for this peer
     *                     will be empty.
     */
    bool ProcessOrphanTx(Peer& peer)
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex, g_msgproc_mutex, !m_tx_download_mutex);

    /** Process a single headers message from a peer.
     *
     * @param[in]   pfrom     CNode of the peer
     * @param[in]   peer      The peer sending us the headers
     * @param[in]   headers   The headers received. Note that this may be modified within ProcessHeadersMessage.
     * @param[in]   via_compact_block   Whether this header came in via compact block handling.
    */
    void ProcessHeadersMessage(CNode& pfrom, Peer& peer,
                               std::vector<CBlockHeader>&& headers,
                               bool via_compact_block)
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex, !m_headers_presync_mutex, g_msgproc_mutex);
    /** Various helpers for headers processing, invoked by ProcessHeadersMessage() */
    /** Return true if headers are continuous and have valid proof-of-work (DoS points assigned on failure) */
    bool CheckHeadersPoW(const std::vector<CBlockHeader>& headers, Peer& peer);
    /** Calculate an anti-DoS work threshold for headers chains */
    arith_uint256 GetAntiDoSWorkThreshold();
    /** Deal with state tracking and headers sync for peers that send
     * non-connecting headers (this can happen due to BIP 130 headers
     * announcements for blocks interacting with the 2hr (MAX_FUTURE_BLOCK_TIME) rule). */
    void HandleUnconnectingHeaders(CNode& pfrom, Peer& peer, const std::vector<CBlockHeader>& headers) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);
    /** Return true if the headers connect to each other, false otherwise */
    bool CheckHeadersAreContinuous(const std::vector<CBlockHeader>& headers) const;
    /** Try to continue a low-work headers sync that has already begun.
     * Assumes the caller has already verified the headers connect, and has
     * checked that each header satisfies the proof-of-work target included in
     * the header.
     *  @param[in]  peer                            The peer we're syncing with.
     *  @param[in]  pfrom                           CNode of the peer
     *  @param[in,out] headers                      The headers to be processed.
     *  @return     True if the passed in headers were successfully processed
     *              as the continuation of a low-work headers sync in progress;
     *              false otherwise.
     *              If false, the passed in headers will be returned back to
     *              the caller.
     *              If true, the returned headers may be empty, indicating
     *              there is no more work for the caller to do; or the headers
     *              may be populated with entries that have passed anti-DoS
     *              checks (and therefore may be validated for block index
     *              acceptance by the caller).
     */
    bool IsContinuationOfLowWorkHeadersSync(Peer& peer, CNode& pfrom,
            std::vector<CBlockHeader>& headers)
        EXCLUSIVE_LOCKS_REQUIRED(peer.m_headers_sync_mutex, !m_headers_presync_mutex, g_msgproc_mutex);
    /** Check work on a headers chain to be processed, and if insufficient,
     * initiate our anti-DoS headers sync mechanism.
     *
     * @param[in]   peer                The peer whose headers we're processing.
     * @param[in]   pfrom               CNode of the peer
     * @param[in]   chain_start_header  Where these headers connect in our index.
     * @param[in,out]   headers             The headers to be processed.
     *
     * @return      True if chain was low work (headers will be empty after
     *              calling); false otherwise.
     */
    bool TryLowWorkHeadersSync(Peer& peer, CNode& pfrom,
                               const CBlockIndex& chain_start_header,
                               std::vector<CBlockHeader>& headers)
        EXCLUSIVE_LOCKS_REQUIRED(!peer.m_headers_sync_mutex, !m_peer_mutex, !m_headers_presync_mutex, g_msgproc_mutex);

    /** Return true if the given header is an ancestor of
     *  m_chainman.m_best_header or our current tip */
    bool IsAncestorOfBestHeaderOrTip(const CBlockIndex* header) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Request further headers from this peer with a given locator.
     * We don't issue a getheaders message if we have a recent one outstanding.
     * This returns true if a getheaders is actually sent, and false otherwise.
     */
    bool MaybeSendGetHeaders(CNode& pfrom, const CBlockLocator& locator, Peer& peer) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);
    /** Potentially fetch blocks from this peer upon receipt of a new headers tip */
    void HeadersDirectFetchBlocks(CNode& pfrom, const Peer& peer, const CBlockIndex& last_header);
    /** Update peer state based on received headers message */
    void UpdatePeerStateForReceivedHeaders(CNode& pfrom, Peer& peer, const CBlockIndex& last_header, bool received_new_header, bool may_have_more_headers)
        EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);

    void SendBlockTransactions(CNode& pfrom, Peer& peer, const CBlock& block, const BlockTransactionsRequest& req);

    /** Send a message to a peer */
    void PushMessage(CNode& node, CSerializedNetMsg&& msg) const { m_connman.PushMessage(&node, std::move(msg)); }
    template <typename... Args>
    void MakeAndPushMessage(CNode& node, std::string msg_type, Args&&... args) const
    {
        m_connman.PushMessage(&node, NetMsg::Make(std::move(msg_type), std::forward<Args>(args)...));
    }

    /** Send a version message to a peer */
    void PushNodeVersion(CNode& pnode, const Peer& peer);

    /** Send a ping message every PING_INTERVAL or if requested via RPC (peer.m_ping_queued is true).
     *  May mark the peer to be disconnected if a ping has timed out.
     *  We use mockable time for ping timeouts, so setmocktime may cause pings
     *  to time out. */
    void MaybeSendPing(CNode& node_to, Peer& peer, NodeClock::time_point now);

    /** Send `addr` messages on a regular schedule. */
    void MaybeSendAddr(CNode& node, Peer& peer, std::chrono::microseconds current_time) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);

    /** Send a single `sendheaders` message, after we have completed headers sync with a peer. */
    void MaybeSendSendHeaders(CNode& node, Peer& peer) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);

    /** Relay (gossip) an address to a few randomly chosen nodes.
     *
     * @param[in] originator   The id of the peer that sent us the address. We don't want to relay it back.
     * @param[in] addr         Address to relay.
     * @param[in] fReachable   Whether the address' network is reachable. We relay unreachable
     *                         addresses less.
     */
    void RelayAddress(NodeId originator, const CAddress& addr, bool fReachable) EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex, g_msgproc_mutex);

    /** Send `feefilter` message. */
    void MaybeSendFeefilter(CNode& node, Peer& peer, std::chrono::microseconds current_time) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);

    FastRandomContext m_rng GUARDED_BY(NetEventsInterface::g_msgproc_mutex);

    FeeFilterRounder m_fee_filter_rounder GUARDED_BY(NetEventsInterface::g_msgproc_mutex);

    const CChainParams& m_chainparams;
    CConnman& m_connman;
    AddrMan& m_addrman;
    /** Pointer to this node's banman. May be nullptr - check existence before dereferencing. */
    BanMan* const m_banman;
    ChainstateManager& m_chainman;
    CTxMemPool& m_mempool;

    /** Synchronizes tx download including TxRequestTracker, rejection filters, and TxOrphanage.
     * Lock invariants:
     * - A txhash (txid or wtxid) in m_txrequest is not also in m_orphanage.
     * - A txhash (txid or wtxid) in m_txrequest is not also in m_lazy_recent_rejects.
     * - A txhash (txid or wtxid) in m_txrequest is not also in m_lazy_recent_rejects_reconsiderable.
     * - A txhash (txid or wtxid) in m_txrequest is not also in m_lazy_recent_confirmed_transactions.
     * - Each data structure's limits hold (m_orphanage max size, m_txrequest per-peer limits, etc).
     */
    Mutex m_tx_download_mutex ACQUIRED_BEFORE(m_mempool.cs);
    node::TxDownloadManager m_txdownloadman GUARDED_BY(m_tx_download_mutex);

    std::unique_ptr<TxReconciliationTracker> m_txreconciliation;

    /** The height of the best chain */
    std::atomic<int> m_best_height{-1};
    /** The time of the best chain tip block */
    std::atomic<std::chrono::seconds> m_best_block_time{0s};

    /** Next time to check for stale tip */
    std::chrono::seconds m_stale_tip_check_time GUARDED_BY(cs_main){0s};

    node::Warnings& m_warnings;
    TimeOffsets m_outbound_time_offsets{m_warnings};

    const Options m_opts;

    // P2P LLM inference response matching
    mutable Mutex m_pending_api_mutex;
    std::map<uint64_t, std::shared_ptr<std::promise<APIResponse>>> m_pending_api_responses GUARDED_BY(m_pending_api_mutex);

    // P2P Miner Advertisement System: nodes broadcast local miner info via MINER_INFO messages;
    // other nodes store {peer_id -> miner_advert} mapping for deterministic routing.

    /** Local miner registry — node-centric discovery via IPC probing */
    MinerLocalRegistry m_local_miner_registry;
    int64_t m_last_local_probe_time GUARDED_BY(m_pending_api_mutex){0};

    std::map<NodeId, MinerAdvertisement> m_peer_miner_ads GUARDED_BY(m_pending_api_mutex);
    int64_t m_last_miner_broadcast_time{0};
    static constexpr int MINER_BROADCAST_INTERVAL_SECONDS = 60;

    bool RejectIncomingTxs(const CNode& peer) const;

    /** Whether we've completed initial sync yet, for determining when to turn
      * on extra block-relay-only peers. */
    bool m_initial_sync_finished GUARDED_BY(cs_main){false};

    /** Protects m_peer_map. This mutex must not be locked while holding a lock
     *  on any of the mutexes inside a Peer object. */
    mutable Mutex m_peer_mutex;
    /**
     * Map of all Peer objects, keyed by peer id. This map is protected
     * by the m_peer_mutex. Once a shared pointer reference is
     * taken, the lock may be released. Individual fields are protected by
     * their own locks.
     */
    std::map<NodeId, PeerRef> m_peer_map GUARDED_BY(m_peer_mutex);

    /** Map maintaining per-node state. */
    std::map<NodeId, CNodeState> m_node_states GUARDED_BY(cs_main);

    /** Get a pointer to a const CNodeState, used when not mutating the CNodeState object. */
    const CNodeState* State(NodeId pnode) const EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    /** Get a pointer to a mutable CNodeState. */
    CNodeState* State(NodeId pnode) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    uint32_t GetFetchFlags(const Peer& peer) const;

    std::map<uint64_t, std::chrono::microseconds> m_next_inv_to_inbounds_per_network_key GUARDED_BY(g_msgproc_mutex);

    /** Number of nodes with fSyncStarted. */
    int nSyncStarted GUARDED_BY(cs_main) = 0;

    /** Hash of the last block we received via INV */
    uint256 m_last_block_inv_triggering_headers_sync GUARDED_BY(g_msgproc_mutex){};

    /**
     * Sources of received blocks, saved to be able punish them when processing
     * happens afterwards.
     * Set mapBlockSource[hash].second to false if the node should not be
     * punished if the block is invalid.
     */
    std::map<uint256, std::pair<NodeId, bool>> mapBlockSource GUARDED_BY(cs_main);

    /** Number of peers with wtxid relay. */
    std::atomic<int> m_wtxid_relay_peers{0};

    /** Number of outbound peers with m_chain_sync.m_protect. */
    int m_outbound_peers_with_protect_from_disconnect GUARDED_BY(cs_main) = 0;

    /** Number of preferable block download peers. */
    int m_num_preferred_download_peers GUARDED_BY(cs_main){0};

    /** Stalling timeout for blocks in IBD */
    std::atomic<std::chrono::seconds> m_block_stalling_timeout{BLOCK_STALLING_TIMEOUT_DEFAULT};

    /**
     * For sending `inv`s to inbound peers, we use a single (exponentially
     * distributed) timer for all peers with the same network key. If we used a separate timer for each
     * peer, a spy node could make multiple inbound connections to us to
     * accurately determine when we received a transaction (and potentially
     * determine the transaction's origin). Each network key has its own timer
     * to make fingerprinting harder. */
    std::chrono::microseconds NextInvToInbounds(std::chrono::microseconds now,
                                                std::chrono::seconds average_interval,
                                                uint64_t network_key) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);


    // All of the following cache a recent block, and are protected by m_most_recent_block_mutex
    Mutex m_most_recent_block_mutex;
    std::shared_ptr<const CBlock> m_most_recent_block GUARDED_BY(m_most_recent_block_mutex);
    std::shared_ptr<const CBlockHeaderAndShortTxIDs> m_most_recent_compact_block GUARDED_BY(m_most_recent_block_mutex);
    uint256 m_most_recent_block_hash GUARDED_BY(m_most_recent_block_mutex);
    std::unique_ptr<const std::map<GenTxid, CTransactionRef>> m_most_recent_block_txs GUARDED_BY(m_most_recent_block_mutex);

    // Data about the low-work headers synchronization, aggregated from all peers' HeadersSyncStates.
    /** Mutex guarding the other m_headers_presync_* variables. */
    Mutex m_headers_presync_mutex;
    /** A type to represent statistics about a peer's low-work headers sync.
     *
     * - The first field is the total verified amount of work in that synchronization.
     * - The second is:
     *   - nullopt: the sync is in REDOWNLOAD phase (phase 2).
     *   - {height, timestamp}: the sync has the specified tip height and block timestamp (phase 1).
     */
    using HeadersPresyncStats = std::pair<arith_uint256, std::optional<std::pair<int64_t, uint32_t>>>;
    /** Statistics for all peers in low-work headers sync. */
    std::map<NodeId, HeadersPresyncStats> m_headers_presync_stats GUARDED_BY(m_headers_presync_mutex) {};
    /** The peer with the most-work entry in m_headers_presync_stats. */
    NodeId m_headers_presync_bestpeer GUARDED_BY(m_headers_presync_mutex) {-1};
    /** The m_headers_presync_stats improved, and needs signalling. */
    std::atomic_bool m_headers_presync_should_signal{false};

    /** Height of the highest block announced using BIP 152 high-bandwidth mode. */
    int m_highest_fast_announce GUARDED_BY(::cs_main){0};

    /** Have we requested this block from a peer */
    bool IsBlockRequested(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Have we requested this block from an outbound peer */
    bool IsBlockRequestedFromOutbound(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main, !m_peer_mutex);

    /** Remove this block from our tracked requested blocks. Called if:
     *  - the block has been received from a peer
     *  - the request for the block has timed out
     * If "from_peer" is specified, then only remove the block if it is in
     * flight from that peer (to avoid one peer's network traffic from
     * affecting another's state).
     */
    void RemoveBlockRequest(const uint256& hash, std::optional<NodeId> from_peer) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /* Mark a block as in flight
     * Returns false, still setting pit, if the block was already in flight from the same peer
     * pit will only be valid as long as the same cs_main lock is being held
     */
    bool BlockRequested(NodeId nodeid, const CBlockIndex& block, std::list<QueuedBlock>::iterator** pit = nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    bool TipMayBeStale() EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Update pindexLastCommonBlock and add not-in-flight missing successors to vBlocks, until it has
     *  at most count entries.
     */
    void FindNextBlocksToDownload(const Peer& peer, unsigned int count, std::vector<const CBlockIndex*>& vBlocks, NodeId& nodeStaller) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Request blocks for the background chainstate, if one is in use. */
    void TryDownloadingHistoricalBlocks(const Peer& peer, unsigned int count, std::vector<const CBlockIndex*>& vBlocks, const CBlockIndex* from_tip, const CBlockIndex* target_block) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
    * \brief Find next blocks to download from a peer after a starting block.
    *
    * \param vBlocks      Vector of blocks to download which will be appended to.
    * \param peer         Peer which blocks will be downloaded from.
    * \param state        Pointer to the state of the peer.
    * \param pindexWalk   Pointer to the starting block to add to vBlocks.
    * \param count        Maximum number of blocks to allow in vBlocks. No more
    *                     blocks will be added if it reaches this size.
    * \param nWindowEnd   Maximum height of blocks to allow in vBlocks. No
    *                     blocks will be added above this height.
    * \param activeChain  Optional pointer to a chain to compare against. If
    *                     provided, any next blocks which are already contained
    *                     in this chain will not be appended to vBlocks, but
    *                     instead will be used to update the
    *                     state->pindexLastCommonBlock pointer.
    * \param nodeStaller  Optional pointer to a NodeId variable that will receive
    *                     the ID of another peer that might be causing this peer
    *                     to stall. This is set to the ID of the peer which
    *                     first requested the first in-flight block in the
    *                     download window. It is only set if vBlocks is empty at
    *                     the end of this function call and if increasing
    *                     nWindowEnd by 1 would cause it to be non-empty (which
    *                     indicates the download might be stalled because every
    *                     block in the window is in flight and no other peer is
    *                     trying to download the next block).
    */
    void FindNextBlocks(std::vector<const CBlockIndex*>& vBlocks, const Peer& peer, CNodeState *state, const CBlockIndex *pindexWalk, unsigned int count, int nWindowEnd, const CChain* activeChain=nullptr, NodeId* nodeStaller=nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /* Multimap used to preserve insertion order */
    typedef std::multimap<uint256, std::pair<NodeId, std::list<QueuedBlock>::iterator>> BlockDownloadMap;
    BlockDownloadMap mapBlocksInFlight GUARDED_BY(cs_main);

    /** When our tip was last updated. */
    std::atomic<std::chrono::seconds> m_last_tip_update{0s};

    /** Determine whether or not a peer can request a transaction, and return it (or nullptr if not found or not allowed). */
    CTransactionRef FindTxForGetData(const Peer::TxRelay& tx_relay, const GenTxid& gtxid)
        EXCLUSIVE_LOCKS_REQUIRED(!m_most_recent_block_mutex, !tx_relay.m_tx_inventory_mutex);

    void ProcessGetData(CNode& pfrom, Peer& peer, const std::atomic<bool>& interruptMsgProc)
        EXCLUSIVE_LOCKS_REQUIRED(!m_most_recent_block_mutex, peer.m_getdata_requests_mutex, NetEventsInterface::g_msgproc_mutex)
        LOCKS_EXCLUDED(::cs_main);

    /** Process a new block. Perform any post-processing housekeeping */
    void ProcessBlock(CNode& node, const std::shared_ptr<const CBlock>& block, bool force_processing, bool min_pow_checked);

    /** Process compact block txns  */
    void ProcessCompactBlockTxns(CNode& pfrom, Peer& peer, const BlockTransactions& block_transactions)
        EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex, !m_most_recent_block_mutex);

    /**
     * Schedule an INV for a transaction to be sent to the given peer (via `PushMessage()`).
     * The transaction is picked from the list of transactions for private broadcast.
     * It is assumed that the connection to the peer is `ConnectionType::PRIVATE_BROADCAST`.
     * Avoid calling this for other peers since it will degrade privacy.
     */
    void PushPrivateBroadcastTx(CNode& node) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex, !m_most_recent_block_mutex);

    /**
     * When a peer sends us a valid block, instruct it to announce blocks to us
     * using CMPCTBLOCK if possible by adding its nodeid to the end of
     * lNodesAnnouncingHeaderAndIDs, and keeping that list under a certain size by
     * removing the first element if necessary.
     */
    void MaybeSetPeerAsAnnouncingHeaderAndIDs(NodeId nodeid) EXCLUSIVE_LOCKS_REQUIRED(cs_main, !m_peer_mutex);

    /** Stack of nodes which we have set to announce using compact blocks */
    std::list<NodeId> lNodesAnnouncingHeaderAndIDs GUARDED_BY(cs_main);

    /** Number of peers from which we're downloading blocks. */
    int m_peers_downloading_from GUARDED_BY(cs_main) = 0;

    void AddToCompactExtraTransactions(const CTransactionRef& tx) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);

    /** Orphan/conflicted/etc transactions that are kept for compact block reconstruction.
     *  The last -blockreconstructionextratxn/DEFAULT_BLOCK_RECONSTRUCTION_EXTRA_TXN of
     *  these are kept in a ring buffer */
    std::vector<std::pair<Wtxid, CTransactionRef>> vExtraTxnForCompact GUARDED_BY(g_msgproc_mutex);
    /** Offset into vExtraTxnForCompact to insert the next tx */
    size_t vExtraTxnForCompactIt GUARDED_BY(g_msgproc_mutex) = 0;

    /** Check whether the last unknown block a peer advertised is not yet known. */
    void ProcessBlockAvailability(NodeId nodeid) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    /** Update tracking information about which blocks a peer is assumed to have. */
    void UpdateBlockAvailability(NodeId nodeid, const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool CanDirectFetch() EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
     * Estimates the distance, in blocks, between the best-known block and the network chain tip.
     * Utilizes the best-block time and the chainparams blocks spacing to approximate it.
     */
    int64_t ApproximateBestBlockDepth() const;

    /**
     * To prevent fingerprinting attacks, only send blocks/headers outside of
     * the active chain if they are no more than a month older (both in time,
     * and in best equivalent proof of work) than the best header chain we know
     * about and we fully-validated them at some point.
     */
    bool BlockRequestAllowed(const CBlockIndex& block_index) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool AlreadyHaveBlock(const uint256& block_hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    void ProcessGetBlockData(CNode& pfrom, Peer& peer, const CInv& inv)
        EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex, !m_most_recent_block_mutex);

    /**
     * Validation logic for compact filters request handling.
     *
     * May disconnect from the peer in the case of a bad request.
     *
     * @param[in]   node            The node that we received the request from
     * @param[in]   peer            The peer that we received the request from
     * @param[in]   filter_type     The filter type the request is for. Must be basic filters.
     * @param[in]   start_height    The start height for the request
     * @param[in]   stop_hash       The stop_hash for the request
     * @param[in]   max_height_diff The maximum number of items permitted to request, as specified in BIP 157
     * @param[out]  stop_index      The CBlockIndex for the stop_hash block, if the request can be serviced.
     * @param[out]  filter_index    The filter index, if the request can be serviced.
     * @return                      True if the request can be serviced.
     */
    bool PrepareBlockFilterRequest(CNode& node, Peer& peer,
                                   BlockFilterType filter_type, uint32_t start_height,
                                   const uint256& stop_hash, uint32_t max_height_diff,
                                   const CBlockIndex*& stop_index,
                                   BlockFilterIndex*& filter_index);

    /**
     * Handle a cfilters request.
     *
     * May disconnect from the peer in the case of a bad request.
     *
     * @param[in]   node            The node that we received the request from
     * @param[in]   peer            The peer that we received the request from
     * @param[in]   vRecv           The raw message received
     */
    void ProcessGetCFilters(CNode& node, Peer& peer, DataStream& vRecv);

    /**
     * Handle a cfheaders request.
     *
     * May disconnect from the peer in the case of a bad request.
     *
     * @param[in]   node            The node that we received the request from
     * @param[in]   peer            The peer that we received the request from
     * @param[in]   vRecv           The raw message received
     */
    void ProcessGetCFHeaders(CNode& node, Peer& peer, DataStream& vRecv);

    /**
     * Handle a getcfcheckpt request.
     *
     * May disconnect from the peer in the case of a bad request.
     *
     * @param[in]   node            The node that we received the request from
     * @param[in]   peer            The peer that we received the request from
     * @param[in]   vRecv           The raw message received
     */
    void ProcessGetCFCheckPt(CNode& node, Peer& peer, DataStream& vRecv);

    void ProcessPong(CNode& pfrom, Peer& peer, NodeClock::time_point ping_end, DataStream& vRecv);

    /** Checks if address relay is permitted with peer. If needed, initializes
     * the m_addr_known bloom filter and sets m_addr_relay_enabled to true.
     *
     *  @return   True if address relay is enabled with peer
     *            False if address relay is disallowed
     */
    bool SetupAddressRelay(const CNode& node, Peer& peer) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);

    void ProcessAddrs(std::string_view msg_type, CNode& pfrom, Peer& peer, std::vector<CAddress>&& vAddr, const std::atomic<bool>& interruptMsgProc)
        EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex, !m_peer_mutex);

    void AddAddressKnown(Peer& peer, const CAddress& addr) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);
    void PushAddress(Peer& peer, const CAddress& addr) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);

    void LogBlockHeader(const CBlockIndex& index, const CNode& peer, bool via_compact_block);

    /// The transactions to be broadcast privately.
    PrivateBroadcast m_tx_for_private_broadcast;
};

const CNodeState* PeerManagerImpl::State(NodeId pnode) const
{
    std::map<NodeId, CNodeState>::const_iterator it = m_node_states.find(pnode);
    if (it == m_node_states.end())
        return nullptr;
    return &it->second;
}

CNodeState* PeerManagerImpl::State(NodeId pnode)
{
    return const_cast<CNodeState*>(std::as_const(*this).State(pnode));
}

/**
 * Whether the peer supports the address. For example, a peer that does not
 * implement BIP155 cannot receive Tor v3 addresses because it requires
 * ADDRv2 (BIP155) encoding.
 */
static bool IsAddrCompatible(const Peer& peer, const CAddress& addr)
{
    return peer.m_wants_addrv2 || addr.IsAddrV1Compatible();
}

void PeerManagerImpl::AddAddressKnown(Peer& peer, const CAddress& addr)
{
    assert(peer.m_addr_known);
    peer.m_addr_known->insert(addr.GetKey());
}

void PeerManagerImpl::PushAddress(Peer& peer, const CAddress& addr)
{
    // Known checking here saves space from duplicates; we'll filter again before sending.
    assert(peer.m_addr_known);
    if (addr.IsValid() && !peer.m_addr_known->contains(addr.GetKey()) && IsAddrCompatible(peer, addr)) {
        if (peer.m_addrs_to_send.size() >= MAX_ADDR_TO_SEND) {
            peer.m_addrs_to_send[m_rng.randrange(peer.m_addrs_to_send.size())] = addr;
        } else {
            peer.m_addrs_to_send.push_back(addr);
        }
    }
}

static void AddKnownTx(Peer& peer, const uint256& hash)
{
    auto tx_relay = peer.GetTxRelay();
    if (!tx_relay) return;

    LOCK(tx_relay->m_tx_inventory_mutex);
    tx_relay->m_tx_inventory_known_filter.insert(hash);
}

/** Whether this peer can serve us blocks. */
static bool CanServeBlocks(const Peer& peer)
{
    return peer.m_their_services & (NODE_NETWORK|NODE_NETWORK_LIMITED);
}

/** Whether this peer can only serve limited recent blocks (e.g. because
 *  it prunes old blocks) */
static bool IsLimitedPeer(const Peer& peer)
{
    return (!(peer.m_their_services & NODE_NETWORK) &&
             (peer.m_their_services & NODE_NETWORK_LIMITED));
}

/** Whether this peer can serve us witness data */
static bool CanServeWitnesses(const Peer& peer)
{
    return peer.m_their_services & NODE_WITNESS;
}

std::chrono::microseconds PeerManagerImpl::NextInvToInbounds(std::chrono::microseconds now,
                                                             std::chrono::seconds average_interval,
                                                             uint64_t network_key)
{
    auto [it, inserted] = m_next_inv_to_inbounds_per_network_key.try_emplace(network_key, 0us);
    auto& timer{it->second};
    if (timer < now) {
        timer = now + m_rng.rand_exp_duration(average_interval);
    }
    return timer;
}

bool PeerManagerImpl::IsBlockRequested(const uint256& hash)
{
    return mapBlocksInFlight.contains(hash);
}

bool PeerManagerImpl::IsBlockRequestedFromOutbound(const uint256& hash)
{
    for (auto range = mapBlocksInFlight.equal_range(hash); range.first != range.second; range.first++) {
        auto [nodeid, block_it] = range.first->second;
        PeerRef peer{GetPeerRef(nodeid)};
        if (peer && !peer->m_is_inbound) return true;
    }

    return false;
}

void PeerManagerImpl::RemoveBlockRequest(const uint256& hash, std::optional<NodeId> from_peer)
{
    auto range = mapBlocksInFlight.equal_range(hash);
    if (range.first == range.second) {
        // Block was not requested from any peer
        return;
    }

    // We should not have requested too many of this block
    Assume(mapBlocksInFlight.count(hash) <= MAX_CMPCTBLOCKS_INFLIGHT_PER_BLOCK);

    while (range.first != range.second) {
        const auto& [node_id, list_it]{range.first->second};

        if (from_peer && *from_peer != node_id) {
            range.first++;
            continue;
        }

        CNodeState& state = *Assert(State(node_id));

        if (state.vBlocksInFlight.begin() == list_it) {
            // First block on the queue was received, update the start download time for the next one
            state.m_downloading_since = std::max(state.m_downloading_since, GetTime<std::chrono::microseconds>());
        }
        state.vBlocksInFlight.erase(list_it);

        if (state.vBlocksInFlight.empty()) {
            // Last validated block on the queue for this peer was received.
            m_peers_downloading_from--;
        }
        state.m_stalling_since = 0us;

        range.first = mapBlocksInFlight.erase(range.first);
    }
}

bool PeerManagerImpl::BlockRequested(NodeId nodeid, const CBlockIndex& block, std::list<QueuedBlock>::iterator** pit)
{
    const uint256& hash{block.GetBlockHash()};

    CNodeState *state = State(nodeid);
    assert(state != nullptr);

    Assume(mapBlocksInFlight.count(hash) <= MAX_CMPCTBLOCKS_INFLIGHT_PER_BLOCK);

    // Short-circuit most stuff in case it is from the same node
    for (auto range = mapBlocksInFlight.equal_range(hash); range.first != range.second; range.first++) {
        if (range.first->second.first == nodeid) {
            if (pit) {
                *pit = &range.first->second.second;
            }
            return false;
        }
    }

    // Make sure it's not being fetched already from same peer.
    RemoveBlockRequest(hash, nodeid);

    std::list<QueuedBlock>::iterator it = state->vBlocksInFlight.insert(state->vBlocksInFlight.end(),
            {&block, std::unique_ptr<PartiallyDownloadedBlock>(pit ? new PartiallyDownloadedBlock(&m_mempool) : nullptr)});
    if (state->vBlocksInFlight.size() == 1) {
        // We're starting a block download (batch) from this peer.
        state->m_downloading_since = GetTime<std::chrono::microseconds>();
        m_peers_downloading_from++;
    }
    auto itInFlight = mapBlocksInFlight.insert(std::make_pair(hash, std::make_pair(nodeid, it)));
    if (pit) {
        *pit = &itInFlight->second.second;
    }
    return true;
}

void PeerManagerImpl::MaybeSetPeerAsAnnouncingHeaderAndIDs(NodeId nodeid)
{
    AssertLockHeld(cs_main);

    // In -blocksonly mode, never request high-bandwidth mode (mempool lacks txs for reconstruction).
    if (m_opts.ignore_incoming_txs) return;

    CNodeState* nodestate = State(nodeid);
    PeerRef peer{GetPeerRef(nodeid)};
    if (!nodestate || !nodestate->m_provides_cmpctblocks) {
        // Don't request compact blocks if the peer has not signalled support
        return;
    }

    int num_outbound_hb_peers = 0;
    for (std::list<NodeId>::iterator it = lNodesAnnouncingHeaderAndIDs.begin(); it != lNodesAnnouncingHeaderAndIDs.end(); it++) {
        if (*it == nodeid) {
            lNodesAnnouncingHeaderAndIDs.erase(it);
            lNodesAnnouncingHeaderAndIDs.push_back(nodeid);
            return;
        }
        PeerRef peer_ref{GetPeerRef(*it)};
        if (peer_ref && !peer_ref->m_is_inbound) ++num_outbound_hb_peers;
    }
    if (peer && peer->m_is_inbound) {
        // If we're adding an inbound HB peer, make sure we're not removing
        // our last outbound HB peer in the process.
        if (lNodesAnnouncingHeaderAndIDs.size() >= 3 && num_outbound_hb_peers == 1) {
            PeerRef remove_peer{GetPeerRef(lNodesAnnouncingHeaderAndIDs.front())};
            if (remove_peer && !remove_peer->m_is_inbound) {
                // Put the HB outbound peer in the second slot, so that it
                // doesn't get removed.
                std::swap(lNodesAnnouncingHeaderAndIDs.front(), *std::next(lNodesAnnouncingHeaderAndIDs.begin()));
            }
        }
    }
    m_connman.ForNode(nodeid, [this](CNode* pfrom) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        AssertLockHeld(::cs_main);
        if (lNodesAnnouncingHeaderAndIDs.size() >= 3) {
            // As per BIP152, we only get 3 of our peers to announce
            // blocks using compact encodings.
            m_connman.ForNode(lNodesAnnouncingHeaderAndIDs.front(), [this](CNode* pnodeStop){
                MakeAndPushMessage(*pnodeStop, NetMsgType::SENDCMPCT, /*high_bandwidth=*/false, /*version=*/CMPCTBLOCKS_VERSION);
                // save BIP152 bandwidth state: we select peer to be low-bandwidth
                pnodeStop->m_bip152_highbandwidth_to = false;
                return true;
            });
            lNodesAnnouncingHeaderAndIDs.pop_front();
        }
        MakeAndPushMessage(*pfrom, NetMsgType::SENDCMPCT, /*high_bandwidth=*/true, /*version=*/CMPCTBLOCKS_VERSION);
        // save BIP152 bandwidth state: we select peer to be high-bandwidth
        pfrom->m_bip152_highbandwidth_to = true;
        lNodesAnnouncingHeaderAndIDs.push_back(pfrom->GetId());
        return true;
    });
}

bool PeerManagerImpl::TipMayBeStale()
{
    AssertLockHeld(cs_main);
    const Consensus::Params& consensusParams = m_chainparams.GetConsensus();
    if (m_last_tip_update.load() == 0s) {
        m_last_tip_update = GetTime<std::chrono::seconds>();
    }
    return m_last_tip_update.load() < GetTime<std::chrono::seconds>() - std::chrono::seconds{consensusParams.nPowTargetSpacing * 3} && mapBlocksInFlight.empty();
}

int64_t PeerManagerImpl::ApproximateBestBlockDepth() const
{
    return (GetTime<std::chrono::seconds>() - m_best_block_time.load()).count() / m_chainparams.GetConsensus().nPowTargetSpacing;
}

bool PeerManagerImpl::CanDirectFetch()
{
    return m_chainman.ActiveChain().Tip()->Time() > NodeClock::now() - m_chainparams.GetConsensus().PowTargetSpacing() * 20;
}

static bool PeerHasHeader(CNodeState *state, const CBlockIndex *pindex) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    if (state->pindexBestKnownBlock && pindex == state->pindexBestKnownBlock->GetAncestor(pindex->nHeight))
        return true;
    if (state->pindexBestHeaderSent && pindex == state->pindexBestHeaderSent->GetAncestor(pindex->nHeight))
        return true;
    return false;
}

void PeerManagerImpl::ProcessBlockAvailability(NodeId nodeid) {
    CNodeState *state = State(nodeid);
    assert(state != nullptr);

    if (!state->hashLastUnknownBlock.IsNull()) {
        const CBlockIndex* pindex = m_chainman.m_blockman.LookupBlockIndex(state->hashLastUnknownBlock);
        if (pindex && pindex->nChainWork > 0) {
            if (state->pindexBestKnownBlock == nullptr || pindex->nChainWork >= state->pindexBestKnownBlock->nChainWork) {
                state->pindexBestKnownBlock = pindex;
            }
            state->hashLastUnknownBlock.SetNull();
        }
    }
}

void PeerManagerImpl::UpdateBlockAvailability(NodeId nodeid, const uint256 &hash) {
    CNodeState *state = State(nodeid);
    assert(state != nullptr);

    ProcessBlockAvailability(nodeid);

    const CBlockIndex* pindex = m_chainman.m_blockman.LookupBlockIndex(hash);
    if (pindex && pindex->nChainWork > 0) {
        // An actually better block was announced.
        if (state->pindexBestKnownBlock == nullptr || pindex->nChainWork >= state->pindexBestKnownBlock->nChainWork) {
            state->pindexBestKnownBlock = pindex;
        }
    } else {
        // An unknown block was announced; just assume that the latest one is the best one.
        state->hashLastUnknownBlock = hash;
    }
}

// Logic for calculating which blocks to download from a given peer, given our current tip.
void PeerManagerImpl::FindNextBlocksToDownload(const Peer& peer, unsigned int count, std::vector<const CBlockIndex*>& vBlocks, NodeId& nodeStaller)
{
    if (count == 0)
        return;

    vBlocks.reserve(vBlocks.size() + count);
    CNodeState *state = State(peer.m_id);
    assert(state != nullptr);

    // Make sure pindexBestKnownBlock is up to date, we'll need it.
    ProcessBlockAvailability(peer.m_id);

    if (state->pindexBestKnownBlock == nullptr || state->pindexBestKnownBlock->nChainWork < m_chainman.ActiveChain().Tip()->nChainWork || state->pindexBestKnownBlock->nChainWork < m_chainman.MinimumChainWork()) {
        // This peer has nothing interesting.
        return;
    }

    // Abort AssumeUtxo download from peers lacking the snapshot block (can't reorg without undo data).
    const CBlockIndex* snap_base{m_chainman.CurrentChainstate().SnapshotBase()};
    if (snap_base && m_chainman.CurrentChainstate().m_assumeutxo == Assumeutxo::UNVALIDATED &&
        state->pindexBestKnownBlock->GetAncestor(snap_base->nHeight) != snap_base) {
        LogDebug(BCLog::NET, "Not downloading blocks from peer=%d, which doesn't have the snapshot block in its best chain.\n", peer.m_id);
        return;
    }

    // Determine forking point; pindexLastCommonBlock must be ancestor of pindexBestKnownBlock.
    auto fork_point = LastCommonAncestor(state->pindexBestKnownBlock, m_chainman.ActiveTip());
    if (state->pindexLastCommonBlock == nullptr ||
        fork_point->nChainWork > state->pindexLastCommonBlock->nChainWork ||
        state->pindexBestKnownBlock->GetAncestor(state->pindexLastCommonBlock->nHeight) != state->pindexLastCommonBlock) {
        state->pindexLastCommonBlock = fork_point;
    }
    if (state->pindexLastCommonBlock == state->pindexBestKnownBlock)
        return;

    const CBlockIndex *pindexWalk = state->pindexLastCommonBlock;
    // Never fetch beyond peer's best known block or BLOCK_DOWNLOAD_WINDOW + 1 beyond last common block (+1 detects stalling).
    int nWindowEnd = state->pindexLastCommonBlock->nHeight + BLOCK_DOWNLOAD_WINDOW;

    FindNextBlocks(vBlocks, peer, state, pindexWalk, count, nWindowEnd, &m_chainman.ActiveChain(), &nodeStaller);
}

void PeerManagerImpl::TryDownloadingHistoricalBlocks(const Peer& peer, unsigned int count, std::vector<const CBlockIndex*>& vBlocks, const CBlockIndex *from_tip, const CBlockIndex* target_block)
{
    Assert(from_tip);
    Assert(target_block);

    if (vBlocks.size() >= count) {
        return;
    }

    vBlocks.reserve(count);
    CNodeState *state = Assert(State(peer.m_id));

    if (state->pindexBestKnownBlock == nullptr || state->pindexBestKnownBlock->GetAncestor(target_block->nHeight) != target_block) {
        // Peer can't provide complete series up to assumeutxo snapshot base (less work than our tip).
        return;
    }

    FindNextBlocks(vBlocks, peer, state, from_tip, count, std::min<int>(from_tip->nHeight + BLOCK_DOWNLOAD_WINDOW, target_block->nHeight));
}

void PeerManagerImpl::FindNextBlocks(std::vector<const CBlockIndex*>& vBlocks, const Peer& peer, CNodeState *state, const CBlockIndex *pindexWalk, unsigned int count, int nWindowEnd, const CChain* activeChain, NodeId* nodeStaller)
{
    std::vector<const CBlockIndex*> vToFetch;
    int nMaxHeight = std::min<int>(state->pindexBestKnownBlock->nHeight, nWindowEnd + 1);
    bool is_limited_peer = IsLimitedPeer(peer);
    NodeId waitingfor = -1;
    while (pindexWalk->nHeight < nMaxHeight) {
        // Read up to 128+ successors into vToFetch (GetAncestor is as expensive as iterating ~100 entries).
        int nToFetch = std::min(nMaxHeight - pindexWalk->nHeight, std::max<int>(count - vBlocks.size(), 128));
        vToFetch.resize(nToFetch);
        pindexWalk = state->pindexBestKnownBlock->GetAncestor(pindexWalk->nHeight + nToFetch);
        vToFetch[nToFetch - 1] = pindexWalk;
        for (unsigned int i = nToFetch - 1; i > 0; i--) {
            vToFetch[i - 1] = vToFetch[i]->pprev;
        }

        // Add undownloaded, non-in-flight blocks from vToFetch; update pindexLastCommonBlock as ancestors complete.
        for (const CBlockIndex* pindex : vToFetch) {
            if (!pindex->IsValid(BLOCK_VALID_TREE)) {
                // We consider the chain that this peer is on invalid.
                return;
            }

            if (!CanServeWitnesses(peer) && DeploymentActiveAt(*pindex, m_chainman, Consensus::DEPLOYMENT_SEGWIT)) {
                // We wouldn't download this block or its descendants from this peer.
                return;
            }

            if (pindex->nStatus & BLOCK_HAVE_DATA || (activeChain && activeChain->Contains(*pindex))) {
                if (activeChain && pindex->HaveNumChainTxs()) {
                    state->pindexLastCommonBlock = pindex;
                }
                continue;
            }

            // Is block in-flight?
            if (IsBlockRequested(pindex->GetBlockHash())) {
                if (waitingfor == -1) {
                    // This is the first already-in-flight block.
                    waitingfor = mapBlocksInFlight.lower_bound(pindex->GetBlockHash())->second.first;
                }
                continue;
            }

            // The block is not already downloaded, and not yet in flight.
            if (pindex->nHeight > nWindowEnd) {
                // We reached the end of the window.
                if (vBlocks.size() == 0 && waitingfor != peer.m_id) {
                    // We aren't able to fetch anything, but we would be if the download window was one larger.
                    if (nodeStaller) *nodeStaller = waitingfor;
                }
                return;
            }

            // Don't request blocks that go further than what limited peers can provide
            if (is_limited_peer && (state->pindexBestKnownBlock->nHeight - pindex->nHeight >= static_cast<int>(NODE_NETWORK_LIMITED_MIN_BLOCKS) - 2 /* two blocks buffer for possible races */)) {
                continue;
            }

            vBlocks.push_back(pindex);
            if (vBlocks.size() == count) {
                return;
            }
        }
    }
}

} // namespace

void PeerManagerImpl::PushNodeVersion(CNode& pnode, const Peer& peer)
{
    uint64_t my_services;
    int64_t my_time;
    uint64_t your_services;
    CService your_addr;
    std::string my_user_agent;
    int my_height;
    bool my_tx_relay;
    if (pnode.IsPrivateBroadcastConn()) {
        my_services = NODE_NONE;
        my_time = 0;
        your_services = NODE_NONE;
        your_addr = CService{};
        my_user_agent = "/tknc-node:1.0.0/"; // TKNC blockchain node
        my_height = 0;
        my_tx_relay = false;
    } else {
        const CAddress& addr{pnode.addr};
        my_services = peer.m_our_services;
        my_time = TicksSinceEpoch<std::chrono::seconds>(NodeClock::now());
        your_services = addr.nServices;
        your_addr = addr.IsRoutable() && !IsProxy(addr) && addr.IsAddrV1Compatible() ? CService{addr} : CService{};
        my_user_agent = strSubVersion;
        my_height = m_best_height;
        my_tx_relay = !RejectIncomingTxs(pnode);
    }

    MakeAndPushMessage(
        pnode,
        NetMsgType::VERSION,
        PROTOCOL_VERSION,
        my_services,
        my_time,
        // your_services + CNetAddr::V1(your_addr) is the pre-version-31402 serialization of your_addr (without nTime)
        your_services, CNetAddr::V1(your_addr),
        // same, for a dummy address
        my_services, CNetAddr::V1(CService{}),
        pnode.GetLocalNonce(),
        my_user_agent,
        my_height,
        my_tx_relay);

    LogDebug(
        BCLog::NET, "send version message: version=%d, blocks=%d%s, txrelay=%d, peer=%d\n",
        PROTOCOL_VERSION, my_height,
        fLogIPs ? strprintf(", them=%s", your_addr.ToStringAddrPort()) : "",
        my_tx_relay, pnode.GetId());
}

void PeerManagerImpl::UpdateLastBlockAnnounceTime(NodeId node, int64_t time_in_seconds)
{
    LOCK(cs_main);
    CNodeState *state = State(node);
    if (state) state->m_last_block_announcement = time_in_seconds;
}

void PeerManagerImpl::InitializeNode(const CNode& node, ServiceFlags our_services)
{
    NodeId nodeid = node.GetId();
    {
        LOCK(cs_main); // For m_node_states
        m_node_states.try_emplace(m_node_states.end(), nodeid);
    }
    WITH_LOCK(m_tx_download_mutex, m_txdownloadman.CheckIsEmpty(nodeid));

    if (NetPermissions::HasFlag(node.m_permission_flags, NetPermissionFlags::BloomFilter)) {
        our_services = static_cast<ServiceFlags>(our_services | NODE_BLOOM);
    }

    PeerRef peer = std::make_shared<Peer>(nodeid, our_services, node.IsInboundConn());
    {
        LOCK(m_peer_mutex);
        m_peer_map.emplace_hint(m_peer_map.end(), nodeid, peer);
    }
}

void PeerManagerImpl::ReattemptInitialBroadcast(CScheduler& scheduler)
{
    std::set<Txid> unbroadcast_txids = m_mempool.GetUnbroadcastTxs();

    for (const auto& txid : unbroadcast_txids) {
        CTransactionRef tx = m_mempool.get(txid);

        if (tx != nullptr) {
            InitiateTxBroadcastToAll(txid, tx->GetWitnessHash());
        } else {
            m_mempool.RemoveUnbroadcastTx(txid, true);
        }
    }

    // Schedule next run for 10-15 minutes in the future.
    // We add randomness on every cycle to avoid the possibility of P2P fingerprinting.
    const auto delta = 10min + FastRandomContext().randrange<std::chrono::milliseconds>(5min);
    scheduler.scheduleFromNow([&] { ReattemptInitialBroadcast(scheduler); }, delta);
}

void PeerManagerImpl::ReattemptPrivateBroadcast(CScheduler& scheduler)
{
    // Remove stale transactions that are no longer relevant (e.g. already in
    // the mempool or mined) and count the remaining ones.
    size_t num_for_rebroadcast{0};
    const auto stale_txs = m_tx_for_private_broadcast.GetStale();
    if (!stale_txs.empty()) {
        LOCK(cs_main);
        for (const auto& stale_tx : stale_txs) {
            auto mempool_acceptable = m_chainman.ProcessTransaction(stale_tx, /*test_accept=*/true);
            if (mempool_acceptable.m_result_type == MempoolAcceptResult::ResultType::VALID) {
                LogDebug(BCLog::PRIVBROADCAST,
                         "Reattempting broadcast of stale txid=%s wtxid=%s",
                         stale_tx->GetHash().ToString(), stale_tx->GetWitnessHash().ToString());
                ++num_for_rebroadcast;
            } else {
                LogDebug(BCLog::PRIVBROADCAST, "Giving up broadcast attempts for txid=%s wtxid=%s: %s",
                         stale_tx->GetHash().ToString(), stale_tx->GetWitnessHash().ToString(),
                         mempool_acceptable.m_state.ToString());
                m_tx_for_private_broadcast.Remove(stale_tx);
            }
        }

        // This could overshoot, but that is ok - we will open some private connections in vain.
        m_connman.m_private_broadcast.NumToOpenAdd(num_for_rebroadcast);
    }

    const auto delta{2min + FastRandomContext().randrange<std::chrono::milliseconds>(1min)};
    scheduler.scheduleFromNow([&] { ReattemptPrivateBroadcast(scheduler); }, delta);
}

void PeerManagerImpl::FinalizeNode(const CNode& node)
{
    NodeId nodeid = node.GetId();
    {
    LOCK(cs_main);
    {
        // PeerRef removed from g_peer_map; Peer may not destruct immediately (other threads may hold refs).
        PeerRef peer = RemovePeer(nodeid);
        assert(peer != nullptr);
        m_wtxid_relay_peers -= peer->m_wtxid_relay;
        assert(m_wtxid_relay_peers >= 0);
    }
    CNodeState *state = State(nodeid);
    assert(state != nullptr);

    if (state->fSyncStarted)
        nSyncStarted--;

    for (const QueuedBlock& entry : state->vBlocksInFlight) {
        auto range = mapBlocksInFlight.equal_range(entry.pindex->GetBlockHash());
        while (range.first != range.second) {
            auto [node_id, list_it] = range.first->second;
            if (node_id != nodeid) {
                range.first++;
            } else {
                range.first = mapBlocksInFlight.erase(range.first);
            }
        }
    }
    {
        LOCK(m_tx_download_mutex);
        m_txdownloadman.DisconnectedPeer(nodeid);
    }
    if (m_txreconciliation) m_txreconciliation->ForgetPeer(nodeid);
    m_num_preferred_download_peers -= state->fPreferredDownload;
    m_peers_downloading_from -= (!state->vBlocksInFlight.empty());
    assert(m_peers_downloading_from >= 0);
    m_outbound_peers_with_protect_from_disconnect -= state->m_chain_sync.m_protect;
    assert(m_outbound_peers_with_protect_from_disconnect >= 0);

    m_node_states.erase(nodeid);

    if (m_node_states.empty()) {
        // Do a consistency check after the last peer is removed.
        assert(mapBlocksInFlight.empty());
        assert(m_num_preferred_download_peers == 0);
        assert(m_peers_downloading_from == 0);
        assert(m_outbound_peers_with_protect_from_disconnect == 0);
        assert(m_wtxid_relay_peers == 0);
        WITH_LOCK(m_tx_download_mutex, m_txdownloadman.CheckIsEmpty());
    }
    } // cs_main
    if (node.fSuccessfullyConnected &&
        !node.IsBlockOnlyConn() && !node.IsPrivateBroadcastConn() && !node.IsInboundConn()) {
        // Only update addrman for full outbound peers (feeler/private-broadcast would leak info).
        m_addrman.Connected(node.addr);
    }
    {
        LOCK(m_headers_presync_mutex);
        m_headers_presync_stats.erase(nodeid);
    }
    if (node.IsPrivateBroadcastConn() &&
        !m_tx_for_private_broadcast.DidNodeConfirmReception(nodeid) &&
        m_tx_for_private_broadcast.HavePendingTransactions()) {

        m_connman.m_private_broadcast.NumToOpenAdd(1);
    }
    LogDebug(BCLog::NET, "Cleared nodestate for peer=%d\n", nodeid);
}

bool PeerManagerImpl::HasAllDesirableServiceFlags(ServiceFlags services) const
{
    // Shortcut for (services & GetDesirableServiceFlags(services)) == GetDesirableServiceFlags(services)
    return !(GetDesirableServiceFlags(services) & (~services));
}

ServiceFlags PeerManagerImpl::GetDesirableServiceFlags(ServiceFlags services) const
{
    if (services & NODE_NETWORK_LIMITED) {
        // Limited peers are desirable when we are close to the tip.
        if (ApproximateBestBlockDepth() < NODE_NETWORK_LIMITED_ALLOW_CONN_BLOCKS) {
            return ServiceFlags(NODE_NETWORK_LIMITED | NODE_WITNESS);
        }
    }
    return ServiceFlags(NODE_NETWORK | NODE_WITNESS);
}

PeerRef PeerManagerImpl::GetPeerRef(NodeId id) const
{
    LOCK(m_peer_mutex);
    auto it = m_peer_map.find(id);
    return it != m_peer_map.end() ? it->second : nullptr;
}

PeerRef PeerManagerImpl::RemovePeer(NodeId id)
{
    PeerRef ret;
    LOCK(m_peer_mutex);
    auto it = m_peer_map.find(id);
    if (it != m_peer_map.end()) {
        ret = std::move(it->second);
        m_peer_map.erase(it);
    }
    return ret;
}

bool PeerManagerImpl::GetNodeStateStats(NodeId nodeid, CNodeStateStats& stats) const
{
    {
        LOCK(cs_main);
        const CNodeState* state = State(nodeid);
        if (state == nullptr)
            return false;
        stats.nSyncHeight = state->pindexBestKnownBlock ? state->pindexBestKnownBlock->nHeight : -1;
        stats.nCommonHeight = state->pindexLastCommonBlock ? state->pindexLastCommonBlock->nHeight : -1;
        for (const QueuedBlock& queue : state->vBlocksInFlight) {
            if (queue.pindex)
                stats.vHeightInFlight.push_back(queue.pindex->nHeight);
        }
    }

    PeerRef peer = GetPeerRef(nodeid);
    if (peer == nullptr) return false;
    stats.their_services = peer->m_their_services;
    // Report lag signal: if a ping is unusually long in flight, caller can detect immediately (pingtime only updates on completion).
    NodeClock::duration ping_wait{0us};
    if ((0 != peer->m_ping_nonce_sent) && (peer->m_ping_start.load() > NodeClock::epoch)) {
        ping_wait = NodeClock::now() - peer->m_ping_start.load();
    }

    if (auto tx_relay = peer->GetTxRelay(); tx_relay != nullptr) {
        stats.m_relay_txs = WITH_LOCK(tx_relay->m_bloom_filter_mutex, return tx_relay->m_relay_txs);
        stats.m_fee_filter_received = tx_relay->m_fee_filter_received.load();
        LOCK(tx_relay->m_tx_inventory_mutex);
        stats.m_last_inv_seq = tx_relay->m_last_inv_sequence;
        stats.m_inv_to_send = tx_relay->m_tx_inventory_to_send.size();
    } else {
        stats.m_relay_txs = false;
        stats.m_fee_filter_received = 0;
        stats.m_inv_to_send = 0;
    }

    stats.m_ping_wait = ping_wait;
    stats.m_addr_processed = peer->m_addr_processed.load();
    stats.m_addr_rate_limited = peer->m_addr_rate_limited.load();
    stats.m_addr_relay_enabled = peer->m_addr_relay_enabled.load();
    {
        LOCK(peer->m_headers_sync_mutex);
        if (peer->m_headers_sync) {
            stats.presync_height = peer->m_headers_sync->GetPresyncHeight();
        }
    }
    stats.time_offset = peer->m_time_offset;

    return true;
}

std::vector<node::TxOrphanage::OrphanInfo> PeerManagerImpl::GetOrphanTransactions()
{
    LOCK(m_tx_download_mutex);
    return m_txdownloadman.GetOrphanTransactions();
}

PeerManagerInfo PeerManagerImpl::GetInfo() const
{
    return PeerManagerInfo{
        .median_outbound_time_offset = m_outbound_time_offsets.Median(),
        .ignores_incoming_txs = m_opts.ignore_incoming_txs,
    };
}

std::vector<PrivateBroadcast::TxBroadcastInfo> PeerManagerImpl::GetPrivateBroadcastInfo() const
{
    return m_tx_for_private_broadcast.GetBroadcastInfo();
}

std::vector<CTransactionRef> PeerManagerImpl::AbortPrivateBroadcast(const uint256& id)
{
    const auto snapshot{m_tx_for_private_broadcast.GetBroadcastInfo()};
    std::vector<CTransactionRef> removed_txs;

    size_t connections_cancelled{0};
    for (const auto& tx_info : snapshot) {
        const CTransactionRef& tx{tx_info.tx};
        if (tx->GetHash().ToUint256() != id && tx->GetWitnessHash().ToUint256() != id) continue;
        if (const auto peer_acks{m_tx_for_private_broadcast.Remove(tx)}) {
            removed_txs.push_back(tx);
            if (NUM_PRIVATE_BROADCAST_PER_TX > *peer_acks) {
                connections_cancelled += (NUM_PRIVATE_BROADCAST_PER_TX - *peer_acks);
            }
        }
    }
    m_connman.m_private_broadcast.NumToOpenSub(connections_cancelled);

    return removed_txs;
}

void PeerManagerImpl::AddToCompactExtraTransactions(const CTransactionRef& tx)
{
    if (m_opts.max_extra_txs <= 0)
        return;
    if (!vExtraTxnForCompact.size())
        vExtraTxnForCompact.resize(m_opts.max_extra_txs);
    vExtraTxnForCompact[vExtraTxnForCompactIt] = std::make_pair(tx->GetWitnessHash(), tx);
    vExtraTxnForCompactIt = (vExtraTxnForCompactIt + 1) % m_opts.max_extra_txs;
}

void PeerManagerImpl::Misbehaving(Peer& peer, const std::string& message)
{
    LOCK(peer.m_misbehavior_mutex);

    const std::string message_prefixed = message.empty() ? "" : (": " + message);
    peer.m_should_discourage = true;
    LogDebug(BCLog::NET, "Misbehaving: peer=%d%s\n", peer.m_id, message_prefixed);
    TRACEPOINT(net, misbehaving_connection,
        peer.m_id,
        message.c_str()
    );
}

void PeerManagerImpl::MaybePunishNodeForBlock(NodeId nodeid, const BlockValidationState& state,
                                              bool via_compact_block, const std::string& message)
{
    PeerRef peer{GetPeerRef(nodeid)};
    switch (state.GetResult()) {
    case BlockValidationResult::BLOCK_RESULT_UNSET:
        break;
    case BlockValidationResult::BLOCK_HEADER_LOW_WORK:
        // We didn't try to process the block because the header chain may have
        // too little work.
        break;
    // The node is providing invalid data:
    case BlockValidationResult::BLOCK_CONSENSUS:
    case BlockValidationResult::BLOCK_MUTATED:
        if (!via_compact_block) {
            if (peer) Misbehaving(*peer, message);
            return;
        }
        break;
    case BlockValidationResult::BLOCK_CACHED_INVALID:
        {
            // Discourage outbound (but not inbound) peers if on an invalid chain.
            // Exempt HB compact block peers. Manual connections are always protected from discouragement.
            if (peer && !via_compact_block && !peer->m_is_inbound) {
                if (peer) Misbehaving(*peer, message);
                return;
            }
            break;
        }
    case BlockValidationResult::BLOCK_INVALID_HEADER:
    case BlockValidationResult::BLOCK_INVALID_PREV:
        if (peer) Misbehaving(*peer, message);
        return;
    // Conflicting (but not necessarily invalid) data or different policy:
    case BlockValidationResult::BLOCK_MISSING_PREV:
        if (peer) Misbehaving(*peer, message);
        return;
    case BlockValidationResult::BLOCK_TIME_FUTURE:
        break;
    }
    if (message != "") {
        LogDebug(BCLog::NET, "peer=%d: %s\n", nodeid, message);
    }
}

bool PeerManagerImpl::BlockRequestAllowed(const CBlockIndex& block_index)
{
    AssertLockHeld(cs_main);
    if (m_chainman.ActiveChain().Contains(block_index)) return true;
    return block_index.IsValid(BLOCK_VALID_SCRIPTS) && (m_chainman.m_best_header != nullptr) &&
           (m_chainman.m_best_header->GetBlockTime() - block_index.GetBlockTime() < STALE_RELAY_AGE_LIMIT) &&
           (GetBlockProofEquivalentTime(*m_chainman.m_best_header, block_index, *m_chainman.m_best_header, m_chainparams.GetConsensus()) < STALE_RELAY_AGE_LIMIT);
}

util::Expected<void, std::string> PeerManagerImpl::FetchBlock(NodeId peer_id, const CBlockIndex& block_index)
{
    if (m_chainman.m_blockman.LoadingBlocks()) return util::Unexpected{"Loading blocks ..."};

    // Ensure this peer exists and hasn't been disconnected
    PeerRef peer = GetPeerRef(peer_id);
    if (peer == nullptr) return util::Unexpected{"Peer does not exist"};

    // Ignore pre-segwit peers
    if (!CanServeWitnesses(*peer)) return util::Unexpected{"Pre-SegWit peer"};

    LOCK(cs_main);

    // Forget about all prior requests
    RemoveBlockRequest(block_index.GetBlockHash(), std::nullopt);

    // Mark block as in-flight
    if (!BlockRequested(peer_id, block_index)) return util::Unexpected{"Already requested from this peer"};

    // Construct message to request the block
    const uint256& hash{block_index.GetBlockHash()};
    std::vector<CInv> invs{CInv(MSG_BLOCK | MSG_WITNESS_FLAG, hash)};

    // Send block request message to the peer
    bool success = m_connman.ForNode(peer_id, [this, &invs](CNode* node) {
        this->MakeAndPushMessage(*node, NetMsgType::GETDATA, invs);
        return true;
    });

    if (!success) return util::Unexpected{"Peer not fully connected"};

    LogDebug(BCLog::NET, "Requesting block %s from peer=%d\n",
                 hash.ToString(), peer_id);
    return {};
}

std::unique_ptr<PeerManager> PeerManager::make(CConnman& connman, AddrMan& addrman,
                                               BanMan* banman, ChainstateManager& chainman,
                                               CTxMemPool& pool, node::Warnings& warnings, Options opts)
{
    return std::make_unique<PeerManagerImpl>(connman, addrman, banman, chainman, pool, warnings, opts);
}

PeerManagerImpl::PeerManagerImpl(CConnman& connman, AddrMan& addrman,
                                 BanMan* banman, ChainstateManager& chainman,
                                 CTxMemPool& pool, node::Warnings& warnings, Options opts)
    : m_rng{opts.deterministic_rng},
      m_fee_filter_rounder{CFeeRate{DEFAULT_MIN_RELAY_TX_FEE}, m_rng},
      m_chainparams(chainman.GetParams()),
      m_connman(connman),
      m_addrman(addrman),
      m_banman(banman),
      m_chainman(chainman),
      m_mempool(pool),
      m_txdownloadman(node::TxDownloadOptions{pool, m_rng, opts.deterministic_rng}),
      m_warnings{warnings},
      m_opts{opts}
{
    // While Erlay support is incomplete, it must be enabled explicitly via -txreconciliation.
    // This argument can go away after Erlay support is complete.
    if (opts.reconcile_txs) {
        m_txreconciliation = std::make_unique<TxReconciliationTracker>(TXRECONCILIATION_VERSION);
    }
}

void PeerManagerImpl::StartScheduledTasks(CScheduler& scheduler)
{
    // Combine stale-tip check and peer eviction in one function to avoid scheduler drift.
    static_assert(EXTRA_PEER_CHECK_INTERVAL < STALE_CHECK_INTERVAL, "peer eviction timer should be less than stale tip check timer");
    scheduler.scheduleEvery([this] { this->CheckForStaleTipAndEvictPeers(); }, std::chrono::seconds{EXTRA_PEER_CHECK_INTERVAL});

    // schedule next run for 10-15 minutes in the future
    const auto delta = 10min + FastRandomContext().randrange<std::chrono::milliseconds>(5min);
    scheduler.scheduleFromNow([&] { ReattemptInitialBroadcast(scheduler); }, delta);

    if (m_opts.private_broadcast) {
        scheduler.scheduleFromNow([&] { ReattemptPrivateBroadcast(scheduler); }, 0min);
    }

    // P2P Miner Advertisement: broadcast local miner info every 60s
    scheduler.scheduleEvery([this] { this->BroadcastLocalMinerInfo(); }, std::chrono::seconds{MINER_BROADCAST_INTERVAL_SECONDS});
    // Also broadcast immediately after startup
    scheduler.scheduleFromNow([this] { this->BroadcastLocalMinerInfo(); }, 0s);
}

void PeerManagerImpl::ActiveTipChange(const CBlockIndex& new_tip, bool is_ibd)
{
    // Ensure mempool mutex was released, otherwise deadlock may occur if another thread holding
    // m_tx_download_mutex waits on the mempool mutex.
    AssertLockNotHeld(m_mempool.cs);
    AssertLockNotHeld(m_tx_download_mutex);

    if (!is_ibd) {
        LOCK(m_tx_download_mutex);
        // Reset rejection filters on chain tip change (previously rejected txs may now be valid, e.g. timelock).
        m_txdownloadman.ActiveTipChange();
    }
}

/**
 * Evict orphan txn pool entries based on a newly connected
 * block, remember the recently confirmed transactions, and delete tracked
 * announcements for them. Also save the time of the last tip update and
 * possibly reduce dynamic block stalling timeout.
 */
void PeerManagerImpl::BlockConnected(
    const ChainstateRole& role,
    const std::shared_ptr<const CBlock>& pblock,
    const CBlockIndex* pindex)
{
    // Update this for all chainstate roles so that we don't mistakenly see peers
    // helping us do background IBD as having a stale tip.
    m_last_tip_update = GetTime<std::chrono::seconds>();

    // In case the dynamic timeout was doubled once or more, reduce it slowly back to its default value
    auto stalling_timeout = m_block_stalling_timeout.load();
    Assume(stalling_timeout >= BLOCK_STALLING_TIMEOUT_DEFAULT);
    if (stalling_timeout != BLOCK_STALLING_TIMEOUT_DEFAULT) {
        const auto new_timeout = std::max(std::chrono::duration_cast<std::chrono::seconds>(stalling_timeout * 0.85), BLOCK_STALLING_TIMEOUT_DEFAULT);
        if (m_block_stalling_timeout.compare_exchange_strong(stalling_timeout, new_timeout)) {
            LogDebug(BCLog::NET, "Decreased stalling timeout to %d seconds\n", count_seconds(new_timeout));
        }
    }

    // Skip for historical chainstate (no mempool) and IBD (no incoming txs).
    if (!role.historical && !m_chainman.IsInitialBlockDownload()) {
        LOCK(m_tx_download_mutex);
        m_txdownloadman.BlockConnected(pblock);
    }
}

void PeerManagerImpl::BlockDisconnected(const std::shared_ptr<const CBlock> &block, const CBlockIndex* pindex)
{
    LOCK(m_tx_download_mutex);
    m_txdownloadman.BlockDisconnected();
}

/**
 * Maintain state about the best-seen block and fast-announce a compact block
 * to compatible peers.
 */
void PeerManagerImpl::NewPoWValidBlock(const CBlockIndex *pindex, const std::shared_ptr<const CBlock>& pblock)
{
    auto pcmpctblock = std::make_shared<const CBlockHeaderAndShortTxIDs>(*pblock, FastRandomContext().rand64());

    LOCK(cs_main);

    if (pindex->nHeight <= m_highest_fast_announce)
        return;
    m_highest_fast_announce = pindex->nHeight;

    if (!DeploymentActiveAt(*pindex, m_chainman, Consensus::DEPLOYMENT_SEGWIT)) return;

    uint256 hashBlock(pblock->GetHash());
    const std::shared_future<CSerializedNetMsg> lazy_ser{
        std::async(std::launch::deferred, [&] { return NetMsg::Make(NetMsgType::CMPCTBLOCK, *pcmpctblock); })};

    {
        auto most_recent_block_txs = std::make_unique<std::map<GenTxid, CTransactionRef>>();
        for (const auto& tx : pblock->vtx) {
            most_recent_block_txs->emplace(tx->GetHash(), tx);
            most_recent_block_txs->emplace(tx->GetWitnessHash(), tx);
        }

        LOCK(m_most_recent_block_mutex);
        m_most_recent_block_hash = hashBlock;
        m_most_recent_block = pblock;
        m_most_recent_compact_block = pcmpctblock;
        m_most_recent_block_txs = std::move(most_recent_block_txs);
    }

    m_connman.ForEachNode([this, pindex, &lazy_ser, &hashBlock](CNode* pnode) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        AssertLockHeld(::cs_main);

        if (pnode->GetCommonVersion() < INVALID_CB_NO_BAN_VERSION || pnode->fDisconnect)
            return;
        ProcessBlockAvailability(pnode->GetId());
        CNodeState &state = *State(pnode->GetId());
        // If the peer has, or we announced to them the previous block already,
        // but we don't think they have this one, go ahead and announce it
        if (state.m_requested_hb_cmpctblocks && !PeerHasHeader(&state, pindex) && PeerHasHeader(&state, pindex->pprev)) {

            LogDebug(BCLog::NET, "%s sending header-and-ids %s to peer=%d\n", "PeerManager::NewPoWValidBlock",
                    hashBlock.ToString(), pnode->GetId());

            const CSerializedNetMsg& ser_cmpctblock{lazy_ser.get()};
            PushMessage(*pnode, ser_cmpctblock.Copy());
            state.pindexBestHeaderSent = pindex;
        }
    });
}

/**
 * Update our best height and announce any block hashes which weren't previously
 * in m_chainman.ActiveChain() to our peers.
 */
void PeerManagerImpl::UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload)
{
    SetBestBlock(pindexNew->nHeight, std::chrono::seconds{pindexNew->GetBlockTime()});

    // Don't relay inventory during initial block download.
    if (fInitialDownload) return;

    // Find the hashes of all blocks that weren't previously in the best chain.
    std::vector<uint256> vHashes;
    const CBlockIndex *pindexToAnnounce = pindexNew;
    while (pindexToAnnounce != pindexFork) {
        vHashes.push_back(pindexToAnnounce->GetBlockHash());
        pindexToAnnounce = pindexToAnnounce->pprev;
        if (vHashes.size() == MAX_BLOCKS_TO_ANNOUNCE) {
            // Limit announcements in case of a huge reorganization.
            // Rely on the peer's synchronization mechanism in that case.
            break;
        }
    }

    {
        LOCK(m_peer_mutex);
        for (auto& it : m_peer_map) {
            Peer& peer = *it.second;
            LOCK(peer.m_block_inv_mutex);
            for (const uint256& hash : vHashes | std::views::reverse) {
                peer.m_blocks_for_headers_relay.push_back(hash);
            }
        }
    }

    m_connman.WakeMessageHandler();
}

/**
 * Handle invalid block rejection and consequent peer discouragement, maintain which
 * peers announce compact blocks.
 */
void PeerManagerImpl::BlockChecked(const std::shared_ptr<const CBlock>& block, const BlockValidationState& state)
{
    LOCK(cs_main);

    const uint256 hash(block->GetHash());
    std::map<uint256, std::pair<NodeId, bool>>::iterator it = mapBlockSource.find(hash);

    // If the block failed validation, we know where it came from and we're still connected
    // to that peer, maybe punish.
    if (state.IsInvalid() &&
        it != mapBlockSource.end() &&
        State(it->second.first)) {
            MaybePunishNodeForBlock(/*nodeid=*/ it->second.first, state, /*via_compact_block=*/ !it->second.second);
    }
    // Check: block valid, not IBD, and no other blocks in flight (tip not updated yet so we proxy via in-flight check).
    else if (state.IsValid() &&
             !m_chainman.IsInitialBlockDownload() &&
             mapBlocksInFlight.count(hash) == mapBlocksInFlight.size()) {
        if (it != mapBlockSource.end()) {
            MaybeSetPeerAsAnnouncingHeaderAndIDs(it->second.first);
        }
    }
    if (it != mapBlockSource.end())
        mapBlockSource.erase(it);
}

// Messages

bool PeerManagerImpl::AlreadyHaveBlock(const uint256& block_hash)
{
    return m_chainman.m_blockman.LookupBlockIndex(block_hash) != nullptr;
}

void PeerManagerImpl::SendPings()
{
    LOCK(m_peer_mutex);
    for(auto& it : m_peer_map) it.second->m_ping_queued = true;
}

void PeerManagerImpl::BroadcastLocalMinerInfo()
{
    // Probe local miner at 127.0.0.1:9332 to get wallet + model info
    LogInfo("[MINER-BCAST] Probing local miner at 127.0.0.1:9332...");
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        LogWarning("[MINER-BCAST] socket() failed, aborting broadcast");
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9332);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

#ifdef WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#endif
    int cr = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
#ifdef WIN32
    if (cr == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set ws; FD_ZERO(&ws); FD_SET(sock, &ws);
        timeval tv; tv.tv_sec = 2; tv.tv_usec = 0;
        if (select((int)sock + 1, nullptr, &ws, nullptr, &tv) > 0) cr = 0;
        mode = 0; ioctlsocket(sock, FIONBIO, &mode);
    }
#endif

    std::string wallet, model, gpu;

    if (cr != 0) {
        LogWarning("[MINER-BCAST] Failed to connect to 127.0.0.1:9332 (no local miner?), skipping broadcast");
        closesocket(sock);
    } else {

    // HTTP GET /api/v1/miners to get local miner info
    std::string req = "GET /api/v1/miners HTTP/1.1\r\nHost: 127.0.0.1:9332\r\nConnection: close\r\n\r\n";
    send(sock, req.c_str(), (int)req.size(), 0);

    char buf[4096] = {};
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int r = recv(sock, buf + total, sizeof(buf) - total - 1, 0);
        if (r <= 0) break;
        total += r;
    }
    closesocket(sock);

    std::string response(buf, total);

    // Extract JSON body (after \r\n\r\n)
    size_t hdr_end = response.find("\r\n\r\n");
    if (hdr_end == std::string::npos) { return; }
    std::string body = response.substr(hdr_end + 4);

    // Extract ONLINE miner's wallet: find "status":"online" object, fallback to last wallet in array.
    // Miner API JSON field order: miner_id, model_name, status, gpu_name, ..., wallet_address, ...
    // So wallet_address and gpu_name come AFTER status — search FORWARD.
    // model_name comes BEFORE status — search BACKWARD.
    std::string online_marker = "\"status\":\"online\"";
    size_t online_pos = body.find(online_marker);
    if (online_pos != std::string::npos) {
        // Find the end of this JSON object
        size_t obj_end = body.find('}', online_pos);
        if (obj_end == std::string::npos) obj_end = body.size();

        // wallet_address: search FORWARD from online_pos
        size_t obj_start = body.find("\"wallet_address\"", online_pos);
        if (obj_start == std::string::npos || obj_start > obj_end) {
            obj_start = body.find("\"wallet\"", online_pos);
        }
        if (obj_start != std::string::npos && obj_start <= obj_end) {
            size_t colon = body.find(":", obj_start);
            size_t start = colon + 1;
            while (start < body.size() && (body[start] == ' ' || body[start] == '"')) start++;
            size_t end = body.find("\"", start);
            if (end != std::string::npos) wallet = body.substr(start, end - start);
        }
        // model_name: search BACKWARD from online_pos (field is before status)
        size_t model_pos = body.rfind("\"model_name\"", online_pos);
        if (model_pos != std::string::npos) {
            size_t colon = body.find(":", model_pos);
            size_t start = colon + 1;
            while (start < body.size() && (body[start] == ' ' || body[start] == '"')) start++;
            size_t end = body.find("\"", start);
            if (end != std::string::npos) model = body.substr(start, end - start);
        }
        // gpu_name: search FORWARD from online_pos (field is after status)
        size_t gpu_pos = body.find("\"gpu_name\"", online_pos);
        if (gpu_pos != std::string::npos && gpu_pos <= obj_end) {
            size_t colon = body.find(":", gpu_pos);
            size_t start = colon + 1;
            while (start < body.size() && (body[start] == ' ' || body[start] == '"')) start++;
            size_t end = body.find("\"", start);
            if (end != std::string::npos) gpu = body.substr(start, end - start);
        }
    }
    // Fallback: find LAST wallet field in the entire response (our miner is usually last)
    // Support both "wallet_address" and "wallet" field names
    if (wallet.empty()) {
        size_t last_wallet = body.rfind("\"wallet_address\"");
        if (last_wallet == std::string::npos) {
            last_wallet = body.rfind("\"wallet\"");
        }
        if (last_wallet != std::string::npos) {
            size_t colon = body.find(":", last_wallet);
            size_t start = colon + 1;
            while (start < body.size() && (body[start] == ' ' || body[start] == '"')) start++;
            size_t end = body.find("\"", start);
            if (end != std::string::npos) wallet = body.substr(start, end - start);
        }
        }
    }

    // Strategy 2: Environment variable fallback (no HTTP dependency)
    if (wallet.empty()) {
        const char* env_wallet = getenv("MINER_WALLET");
        if (env_wallet && strlen(env_wallet) > 0) wallet = std::string(env_wallet);
        const char* env_model = getenv("MINER_MODEL");
        if (env_model && strlen(env_model) > 0) model = std::string(env_model);
    }

    // CRITICAL: Always broadcast if we have at least a wallet address
    // Previously: silent return on probe failure broke entire P2P discovery
    if (wallet.empty()) {
        LogWarning("[MINER-BCAST] No wallet address found after probe + env fallback, skipping broadcast");
        return;
    }

    LogInfo("[MINER-BCAST] Broadcasting MINER_INFO: wallet=%s model=%s gpu=%s", wallet.substr(0,16).c_str(), model.c_str(), gpu.c_str());

    // Serialize as null-delimited string: wallet\0model\0gpu\0port
    // Use 9313 (gateway port) so remote clients can connect via node's public API
    // CRITICAL: std::string + "\0" adds NOTHING (strlen("\0")==0), must use push_back('\0')
    std::string payload;
    payload.reserve(wallet.size() + model.size() + gpu.size() + 8);
    payload += wallet;
    payload.push_back('\0');
    payload += model;
    payload.push_back('\0');
    payload += gpu;
    payload.push_back('\0');
    payload += "9313";
    std::vector<uint8_t> data(payload.begin(), payload.end());

    // Broadcast MINER_INFO to all connected peers
    int peer_count = 0;
    m_connman.ForEachNode([&](CNode* node) {
        m_connman.PushMessage(node, NetMsg::Make(std::string(MessageTypes::MINER_INFO), data));
        peer_count++;
    });
    LogInfo("[MINER-BCAST] MINER_INFO sent to %d peers", peer_count);

    // Also feed into MinerSyncManager for rich miner info database
    #ifdef HAVE_MINER_SYNC
    {
        using namespace MinerSyncProtocol;
        MinerInfo rich_info;
        rich_info.miner_id = wallet;
        rich_info.node_id = "tknc-node";  // TODO: use actual node ID
        rich_info.model_name = model;
        rich_info.current_status = MinerStatus::MINING;
        rich_info.api_endpoint = "0.0.0.0:9313";
        rich_info.last_update_timestamp = GetTime();
        rich_info.first_seen_timestamp = GetTime();
        // Try to get a global reference to the static instance from init.cpp
        // For now, this is a compile-time toggle; full wiring needs global accessor
    }
    #endif
}

std::vector<std::pair<NodeId, ::MinerAdvertisement>> PeerManagerImpl::GetMinerPeers() const
{
    std::vector<std::pair<NodeId, ::MinerAdvertisement>> result;

    // Source 1: Local registry (highest priority — node-centric)
    auto local_miners = m_local_miner_registry.GetAll();
    for (const auto& lm : local_miners) {
        ::MinerAdvertisement ad;
        ad.wallet_address = lm.wallet_address;
        ad.model_name = lm.model_name;
        ad.gpu_name = lm.gpu_name;
        ad.last_seen = lm.last_seen;
        result.emplace_back(-1, ad);  // -1 = local (not a P2P peer)
    }

    // Source 2: Remote P2P advertisements (lower priority)
    LOCK(m_pending_api_mutex);
    int64_t now = GetTime();
    for (const auto& [peer_id, advert] : m_peer_miner_ads) {
        if (now - advert.last_seen < 300) {
            result.emplace_back(peer_id, advert);
        }
    }
    return result;
}

NodeId PeerManagerImpl::SelectBestMinerPeer(NodeId requester_id, const std::string& model_hint) const
{
    struct Candidate {
        NodeId id;
        int score;
        const MinerAdvertisement* ad;
    };
    std::vector<Candidate> candidates;

    {
        LOCK(m_pending_api_mutex);
        int64_t now = GetTime();
        for (const auto& [peer_id, advert] : m_peer_miner_ads) {
            if (peer_id == requester_id) continue;
            // Skip stale ads (>5 min)
            if (now - advert.last_seen > 300) continue;

            int score = 0;
            // Factor 1: Model match (+50)
            if (!model_hint.empty() && !advert.model_name.empty()) {
                if (advert.model_name.find(model_hint) != std::string::npos) {
                    score += 50;
                }
            }
            // Factor 2: Freshness (+30, decays 1 point per minute after 30min)
            int age_secs = static_cast<int>(now - advert.last_seen);
            score += std::max(0, 30 - age_secs / 60);
            // Factor 3: Known wallet with non-empty address (+20)
            if (!advert.wallet_address.empty()) score += 20;

            candidates.push_back({peer_id, score, &advert});
        }
    }

    if (candidates.empty()) return -1;

    // Sort by score descending
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    LogInfo("[P2P-ROUTE] Selected peer=%d (score=%d, wallet=%s, model=%s) from %d candidates",
            candidates[0].id, candidates[0].score,
            candidates[0].ad->wallet_address.substr(0, 16).c_str(),
            candidates[0].ad->model_name.c_str(),
            (int)candidates.size());

    return candidates[0].id;
}

std::future<APIResponse> PeerManagerImpl::SendInferenceRequest(NodeId peer_id, const APIRequest& request)
{
    LogInfo("[P2P-API] SendInferenceRequest: peer=%d model=%s request_id=%llu",
            peer_id, request.model.c_str(), (unsigned long long)request.request_id);

    auto promise = std::make_shared<std::promise<APIResponse>>();
    std::future<APIResponse> future = promise->get_future();

    {
        LOCK(m_pending_api_mutex);
        m_pending_api_responses[request.request_id] = promise;
    }

    std::vector<uint8_t> request_data = request.Serialize();
    LogInfo("[P2P-API] Serialized APIREQ: %d bytes", (int)request_data.size());

    bool sent = m_connman.ForNode(peer_id, [&](CNode* node) {
        m_connman.PushMessage(node, NetMsg::Make(std::string(MessageTypes::APIREQ), std::move(request_data)));
        LogInfo("[P2P-API] APIREQ message pushed to peer=%d", peer_id);
        return true;
    });

    if (!sent) {
        LogWarning("[P2P-API] ForNode returned false — peer=%d not found or not connected", peer_id);
        LOCK(m_pending_api_mutex);
        auto it = m_pending_api_responses.find(request.request_id);
        if (it != m_pending_api_responses.end()) {
            APIResponse err_resp;
            err_resp.content = "Error: Peer not found or not connected";
            err_resp.request_id = request.request_id;
            err_resp.tokens_used = 0;
            err_resp.cost = 0;
            it->second->set_value(err_resp);
            m_pending_api_responses.erase(it);
        }
    }

    return future;
}

// P2PRelayToLocalMiner: Direct HTTP POST to local miner for P2P-relayed requests.
// Bypasses BackendRouter (rejects empty api_key); sends X-P2P-Relay header; auth validated at P2P level.
static InferenceResult P2PRelayToLocalMiner(const APIRequest& request)
{
    InferenceResult result;
    LogInfo("[P2PRelay] === START: connecting to 127.0.0.1:9332 for model=%s ===", request.model.c_str());

#ifdef WIN32
    // Ensure Winsock is initialized (required on Windows before any socket call)
    WSADATA wsaData;
    static bool wsa_initialized = false;
    if (!wsa_initialized) {
        int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (wsaResult != 0) {
            result.error_message = "P2PRelay: WSAStartup failed";
            return result;
        }
        wsa_initialized = true;
    }
#endif

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        result.error_message = "P2PRelay: socket() failed";
        return result;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9332);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

#ifdef WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#endif

    int cr = connect(sock, (struct sockaddr*)&addr, sizeof(addr));

#ifdef WIN32
    if (cr != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set ws; FD_ZERO(&ws); FD_SET(sock, &ws);
        timeval tv; tv.tv_sec = 10; tv.tv_usec = 0;
        int sr = select(0, NULL, &ws, NULL, &tv);
        if (sr > 0) {
            int so_error = 0; socklen_t len = sizeof(so_error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
            if (so_error == 0) cr = 0;
            else LogInfo("[P2PRelay] connect so_error=%d", so_error);
        } else {
            LogInfo("[P2PRelay] connect select returned %d (timeout/failure)", sr);
        }
    } else if (cr != 0) {
        LogInfo("[P2PRelay] connect failed immediately, err=%d", WSAGetLastError());
    }
    mode = 0;
    ioctlsocket(sock, FIONBIO, &mode);
#endif

    if (cr != 0) {
        closesocket(sock);
        result.error_message = "P2PRelay: Local miner not reachable at 127.0.0.1:9332";
        return result;
    }

    // Set recv/send timeout to 86400s (24h). The system is a bridge — no artificial
    // timeout should cut off long-running inference. Whether the miner runs a 0.5B
    // model on a laptop or a 10000B model on a data-center cluster, the bridge
    // simply waits for the miner to finish. The real bottleneck is the miner's
    // hardware and bandwidth, not this bridge.
#ifdef WIN32
    DWORD recv_timeout = 86400000;  // 86400s in milliseconds
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recv_timeout, sizeof(recv_timeout));
    DWORD snd_timeout = 86400000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&snd_timeout, sizeof(snd_timeout));
#else
    struct timeval tv;
    tv.tv_sec = 86400;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    // Build JSON body from request messages
    std::string combined_prompt;
    for (const auto& msg : request.messages) {
        if (!combined_prompt.empty()) combined_prompt += "\n";
        combined_prompt += msg.content;
    }
    // Simple JSON escaping
    std::string escaped_prompt;
    for (char c : combined_prompt) {
        if (c == '"') escaped_prompt += "\\\"";
        else if (c == '\n') escaped_prompt += "\\n";
        else if (c == '\\') escaped_prompt += "\\\\";
        else if (c == '\t') escaped_prompt += "\\t";
        else escaped_prompt += c;
    }
    std::string escaped_model;
    for (char c : request.model) {
        if (c == '"') escaped_model += "\\\"";
        else if (c == '\\') escaped_model += "\\\\";
        else escaped_model += c;
    }

    // Build JSON: include api_key if present, omit if empty (miner's localhost bypass handles it)
    // stream:true forces the miner to use GenerateStream (逐token流式生成模式),
    // which is more efficient and consistent with the HTTP Gateway path.
    // The P2P relay collects all SSE chunks and returns the full content via P2P.
    // max_tokens is passed through from the client — no artificial cap.
    std::string jsonBody;
    std::string max_tokens_str;
    if (request.max_tokens != 0) {
        max_tokens_str = ",\"max_tokens\":" + std::to_string(request.max_tokens);
    }
    if (!request.api_key.empty()) {
        jsonBody = "{\"api_key\":\"" + request.api_key
            + "\",\"model\":\"" + escaped_model
            + "\",\"messages\":[{\"role\":\"user\",\"content\":\"" + escaped_prompt + "\"}]"
            + ",\"stream\":true" + max_tokens_str + "}";
    } else {
        // No api_key — miner will allow via X-P2P-Relay localhost trust boundary
        jsonBody = "{\"model\":\"" + escaped_model
            + "\",\"messages\":[{\"role\":\"user\",\"content\":\"" + escaped_prompt + "\"}]"
            + ",\"stream\":true" + max_tokens_str + "}";
    }

    // Build HTTP request with P2P relay header
    std::string httpRequest = "POST /api/v1/chat HTTP/1.1\r\n";
    httpRequest += "Host: 127.0.0.1:9332\r\n";
    httpRequest += "Content-Type: application/json\r\n";
    httpRequest += "X-P2P-Relay: true\r\n";
    httpRequest += "Content-Length: " + std::to_string(jsonBody.length()) + "\r\n";
    httpRequest += "Connection: close\r\n\r\n";
    httpRequest += jsonBody;

    send(sock, httpRequest.c_str(), (int)httpRequest.length(), 0);

    // Receive response using dynamic buffer (no fixed size limit).
    // Uses the same pattern as HttpPostToMiner in inference_gateway.cpp:
    //   read into small stack buffer, append to dynamic std::string.
    // This handles any output size — from 256-token short answers to
    // 8192-token long essays, regardless of model size (0.5B or 744B).
    std::string response_data;
    char recv_buf[4096];
    int content_length = -1;  // -1 = unknown, will fall back to connection-close
    size_t header_end_pos = std::string::npos;

    while (true) {
        int r = recv(sock, recv_buf, sizeof(recv_buf) - 1, 0);
        if (r <= 0) break;
        recv_buf[r] = '\0';
        response_data.append(recv_buf, r);

        // Try to parse headers once we have them
        if (header_end_pos == std::string::npos) {
            header_end_pos = response_data.find("\r\n\r\n");
            if (header_end_pos != std::string::npos) {
                // Parse Content-Length from headers
                std::string headers = response_data.substr(0, header_end_pos);
                size_t cl_pos = headers.find("Content-Length:");
                if (cl_pos == std::string::npos)
                    cl_pos = headers.find("content-length:");
                if (cl_pos != std::string::npos) {
                    size_t colon = headers.find(":", cl_pos);
                    size_t val_start = colon + 1;
                    while (val_start < headers.size() && headers[val_start] == ' ') val_start++;
                    size_t val_end = headers.find("\r\n", val_start);
                    if (val_end == std::string::npos) val_end = headers.size();
                    try {
                        content_length = std::stoi(headers.substr(val_start, val_end - val_start));
                    } catch (...) {}
                }
            }
        }

        // If we know Content-Length, check if we have the full body
        if (content_length >= 0 && header_end_pos != std::string::npos) {
            int body_received = (int)response_data.size() - (int)header_end_pos - 4;  // subtract "\r\n\r\n"
            if (body_received >= content_length) {
                break;  // Full response received, no need to wait for connection close
            }
        }
    }
    closesocket(sock);

    size_t headerEnd = response_data.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        result.error_message = "P2PRelay: Invalid HTTP response from local miner";
        return result;
    }

    std::string jsonData = response_data.substr(headerEnd + 4);

    // === SSE Response Parsing ===
    // When stream:true is sent to the miner, the response is SSE format:
    //   data: {"choices":[{"delta":{"content":"Hello"}}]}\n\n
    //   data: {"choices":[{"delta":{"content":" world"}}]}\n\n
    //   data: {"choices":[{"delta":{},"finish_reason":"stop"}]}\n\n
    //   data: [DONE]\n\n
    // We parse all data: lines and extract delta.content to build the full response.
    if (jsonData.find("data: ") != std::string::npos || jsonData.find("data:") != std::string::npos) {
        std::string sse_content;
        int sse_token_count = 0;

        // Parse line by line
        size_t pos = 0;
        while (pos < jsonData.size()) {
            size_t line_start = jsonData.find("data:", pos);
            if (line_start == std::string::npos) break;

            // Skip "data:" and any spaces
            size_t val_start = line_start + 5;
            while (val_start < jsonData.size() && jsonData[val_start] == ' ') val_start++;

            // Find end of line
            size_t line_end = jsonData.find('\n', val_start);
            if (line_end == std::string::npos) line_end = jsonData.size();

            std::string data_str = jsonData.substr(val_start, line_end - val_start);
            // Trim trailing \r
            if (!data_str.empty() && data_str.back() == '\r') data_str.pop_back();

            pos = line_end + 1;

            if (data_str.empty() || data_str == "[DONE]") continue;

            // Parse JSON: extract choices[0].delta.content
            // Look for "content":"..." pattern within the data line
            size_t content_pos = data_str.find("\"content\":\"");
            if (content_pos != std::string::npos) {
                size_t c_start = content_pos + 10;  // skip "content":"
                size_t c_end = c_start;
                // Handle escaped characters
                while (c_end < data_str.size()) {
                    if (data_str[c_end] == '\\' && c_end + 1 < data_str.size()) {
                        c_end += 2;
                    } else if (data_str[c_end] == '"') {
                        break;
                    } else {
                        c_end++;
                    }
                }
                if (c_end <= data_str.size()) {
                    std::string token = data_str.substr(c_start, c_end - c_start);
                    // Unescape JSON strings
                    size_t ep = 0;
                    while ((ep = token.find("\\n", ep)) != std::string::npos) { token.replace(ep, 2, "\n"); ep++; }
                    while ((ep = token.find("\\\"", ep)) != std::string::npos) { token.replace(ep, 2, "\""); ep++; }
                    while ((ep = token.find("\\\\", ep)) != std::string::npos) { token.replace(ep, 2, "\\"); ep++; }
                    while ((ep = token.find("\\t", ep)) != std::string::npos) { token.replace(ep, 2, "\t"); ep++; }
                    sse_content += token;
                    sse_token_count++;
                }
            }

            // Also check for finish_reason to count tokens
            if (data_str.find("\"finish_reason\":\"stop\"") != std::string::npos) {
                // Stream finished
            }
        }

        if (!sse_content.empty()) {
            result.success = true;
            result.content = sse_content;
            result.tokens_used = sse_token_count;
            result.cost = sse_token_count;
            LogInfo("[P2PRelay] SSE parsing complete: tokens=%d, content_length=%zu",
                     sse_token_count, sse_content.size());
            return result;
        }

        // SSE format detected but no content extracted — fall through to error
        result.error_message = "P2PRelay: SSE response had no content. Raw: " + jsonData.substr(0, 200);
        return result;
    }

    // === Non-SSE (batch JSON) response parsing (fallback) ===
    if (jsonData.find("\"error\"") != std::string::npos) {
        size_t epos = jsonData.find("\"error\"");
        size_t colon = jsonData.find(":", epos);
        if (colon != std::string::npos) {
            size_t start = colon + 1;
            while (start < jsonData.size() && (jsonData[start] == ' ' || jsonData[start] == '"')) start++;
            size_t end = jsonData.find("\"", start);
            if (end != std::string::npos) {
                result.error_message = "P2PRelay: Miner error: " + jsonData.substr(start, end - start);
            }
        }
        return result;
    }

    // Extract content field
    std::string content;
    size_t cpos = std::string::npos;
    const char* fields[] = {"content", "response", "text"};
    for (auto f : fields) {
        std::string pattern = std::string("\"") + f + "\"";
        size_t pos = jsonData.find(pattern);
        if (pos != std::string::npos) { cpos = pos; break; }
    }

    if (cpos != std::string::npos) {
        size_t colonPos = jsonData.find(":", cpos);
        if (colonPos != std::string::npos) {
            size_t start = colonPos + 1;
            while (start < jsonData.size() && (jsonData[start] == ' ' || jsonData[start] == '"')) start++;
            size_t end = jsonData.find("\"", start);
            if (end != std::string::npos) {
                content = jsonData.substr(start, end - start);
                // Unescape JSON strings
                size_t ep = 0;
                while ((ep = content.find("\\n", ep)) != std::string::npos) { content.replace(ep, 2, "\n"); ep++; }
                while ((ep = content.find("\\\"", ep)) != std::string::npos) { content.replace(ep, 2, "\""); ep++; }
            }
        }
    }

    // Extract tokens_used if present
    size_t tpos = jsonData.find("\"tokens_used\"");
    if (tpos != std::string::npos) {
        size_t colon = jsonData.find(":", tpos);
        if (colon != std::string::npos) {
            size_t valEnd = jsonData.find(",", colon);
            if (valEnd == std::string::npos) valEnd = jsonData.find("}", colon);
            if (valEnd != std::string::npos) {
                try { result.tokens_used = std::stoll(jsonData.substr(colon + 1, valEnd - colon - 1)); } catch (...) {}
            }
        }
    }

    if (!content.empty()) {
        result.success = true;
        result.content = content;
        result.cost = result.tokens_used;
    } else {
        result.error_message = "P2PRelay: Miner returned empty/unparseable response. Raw: " + jsonData.substr(0, 200);
    }

    return result;
}

void PeerManagerImpl::InitiateTxBroadcastToAll(const Txid& txid, const Wtxid& wtxid)
{
    LOCK(m_peer_mutex);
    for(auto& it : m_peer_map) {
        Peer& peer = *it.second;
        auto tx_relay = peer.GetTxRelay();
        if (!tx_relay) continue;

        LOCK(tx_relay->m_tx_inventory_mutex);
        // Only announce after version handshake (else arrival time could leak to spy).
        if (tx_relay->m_next_inv_send_time == 0s) continue;

        const uint256& hash{peer.m_wtxid_relay ? wtxid.ToUint256() : txid.ToUint256()};
        if (!tx_relay->m_tx_inventory_known_filter.contains(hash)) {
            tx_relay->m_tx_inventory_to_send.insert(wtxid);
        }
    }
}

void PeerManagerImpl::InitiateTxBroadcastPrivate(const CTransactionRef& tx)
{
    const auto txstr{strprintf("txid=%s, wtxid=%s", tx->GetHash().ToString(), tx->GetWitnessHash().ToString())};
    if (m_tx_for_private_broadcast.Add(tx)) {
        LogDebug(BCLog::PRIVBROADCAST, "Requesting %d new connections due to %s", NUM_PRIVATE_BROADCAST_PER_TX, txstr);
        m_connman.m_private_broadcast.NumToOpenAdd(NUM_PRIVATE_BROADCAST_PER_TX);
    } else {
        LogDebug(BCLog::PRIVBROADCAST, "Ignoring unnecessary request to schedule an already scheduled transaction: %s", txstr);
    }
}

void PeerManagerImpl::RelayAddress(NodeId originator,
                                   const CAddress& addr,
                                   bool fReachable)
{
    // Choose same nodes within 24h window to relay each address once (prevents unjust propagation boost).
    if (!fReachable && !addr.IsRelayable()) return;

    // Relay to limited nodes using deterministic randomness (same nodes for 24h prevents repeats).
    const uint64_t hash_addr{CServiceHash(0, 0)(addr)};
    const auto current_time{GetTime<std::chrono::seconds>()};
    // Adding address hash makes exact rotation time different per address, while preserving periodicity.
    const uint64_t time_addr{(static_cast<uint64_t>(count_seconds(current_time)) + hash_addr) / count_seconds(ROTATE_ADDR_RELAY_DEST_INTERVAL)};
    const CSipHasher hasher{m_connman.GetDeterministicRandomizer(RANDOMIZER_ID_ADDRESS_RELAY)
                                .Write(hash_addr)
                                .Write(time_addr)};

    // Relay reachable addresses to 2 peers. Unreachable addresses are relayed randomly to 1 or 2 peers.
    unsigned int nRelayNodes = (fReachable || (hasher.Finalize() & 1)) ? 2 : 1;

    std::array<std::pair<uint64_t, Peer*>, 2> best{{{0, nullptr}, {0, nullptr}}};
    assert(nRelayNodes <= best.size());

    LOCK(m_peer_mutex);

    for (auto& [id, peer] : m_peer_map) {
        if (peer->m_addr_relay_enabled && id != originator && IsAddrCompatible(*peer, addr)) {
            uint64_t hashKey = CSipHasher(hasher).Write(id).Finalize();
            for (unsigned int i = 0; i < nRelayNodes; i++) {
                 if (hashKey > best[i].first) {
                     std::copy(best.begin() + i, best.begin() + nRelayNodes - 1, best.begin() + i + 1);
                     best[i] = std::make_pair(hashKey, peer.get());
                     break;
                 }
            }
        }
    };

    for (unsigned int i = 0; i < nRelayNodes && best[i].first != 0; i++) {
        PushAddress(*best[i].second, addr);
    }
}

void PeerManagerImpl::ProcessGetBlockData(CNode& pfrom, Peer& peer, const CInv& inv)
{
    std::shared_ptr<const CBlock> a_recent_block;
    std::shared_ptr<const CBlockHeaderAndShortTxIDs> a_recent_compact_block;
    {
        LOCK(m_most_recent_block_mutex);
        a_recent_block = m_most_recent_block;
        a_recent_compact_block = m_most_recent_compact_block;
    }

    bool need_activate_chain = false;
    {
        LOCK(cs_main);
        const CBlockIndex* pindex = m_chainman.m_blockman.LookupBlockIndex(inv.hash);
        if (pindex) {
            if (pindex->HaveNumChainTxs() && !pindex->IsValid(BLOCK_VALID_SCRIPTS) &&
                    pindex->IsValid(BLOCK_VALID_TREE)) {
                // Block + parents present but unvalidated: mid-connection, need ActivateBestChain before relay checks.
                need_activate_chain = true;
            }
        }
    } // release cs_main before calling ActivateBestChain
    if (need_activate_chain) {
        BlockValidationState state;
        if (!m_chainman.ActiveChainstate().ActivateBestChain(state, a_recent_block)) {
            LogDebug(BCLog::NET, "failed to activate chain (%s)\n", state.ToString());
        }
    }

    const CBlockIndex* pindex{nullptr};
    const CBlockIndex* tip{nullptr};
    bool can_direct_fetch{false};
    FlatFilePos block_pos{};
    {
        LOCK(cs_main);
        pindex = m_chainman.m_blockman.LookupBlockIndex(inv.hash);
        if (!pindex) {
            return;
        }
        if (!BlockRequestAllowed(*pindex)) {
            LogDebug(BCLog::NET, "%s: ignoring request from peer=%i for old block that isn't in the main chain\n", __func__, pfrom.GetId());
            return;
        }
        // disconnect node in case we have reached the outbound limit for serving historical blocks
        if (m_connman.OutboundTargetReached(true) &&
            (((m_chainman.m_best_header != nullptr) && (m_chainman.m_best_header->GetBlockTime() - pindex->GetBlockTime() > HISTORICAL_BLOCK_AGE)) || inv.IsMsgFilteredBlk()) &&
            !pfrom.HasPermission(NetPermissionFlags::Download) // nodes with the download permission may exceed target
        ) {
            LogDebug(BCLog::NET, "historical block serving limit reached, %s", pfrom.DisconnectMsg());
            pfrom.fDisconnect = true;
            return;
        }
        tip = m_chainman.ActiveChain().Tip();
        // Avoid leaking prune-height by never sending blocks below the NODE_NETWORK_LIMITED threshold
        if (!pfrom.HasPermission(NetPermissionFlags::NoBan) && (
                (((peer.m_our_services & NODE_NETWORK_LIMITED) == NODE_NETWORK_LIMITED) && ((peer.m_our_services & NODE_NETWORK) != NODE_NETWORK) && (tip->nHeight - pindex->nHeight > (int)NODE_NETWORK_LIMITED_MIN_BLOCKS + 2 /* add two blocks buffer extension for possible races */) )
           )) {
            LogDebug(BCLog::NET, "Ignore block request below NODE_NETWORK_LIMITED threshold, %s", pfrom.DisconnectMsg());
            //disconnect node and prevent it from stalling (would otherwise wait for the missing block)
            pfrom.fDisconnect = true;
            return;
        }
        // Pruned nodes may have deleted the block, so check whether
        // it's available before trying to send.
        if (!(pindex->nStatus & BLOCK_HAVE_DATA)) {
            return;
        }
        can_direct_fetch = CanDirectFetch();
        block_pos = pindex->GetBlockPos();
    }

    std::shared_ptr<const CBlock> pblock;
    if (a_recent_block && a_recent_block->GetHash() == inv.hash) {
        pblock = a_recent_block;
    } else if (inv.IsMsgWitnessBlk()) {
        // Fast-path: in this case it is possible to serve the block directly from disk,
        // as the network format matches the format on disk
        if (const auto block_data{m_chainman.m_blockman.ReadRawBlock(block_pos)}) {
            MakeAndPushMessage(pfrom, NetMsgType::BLOCK, std::span{*block_data});
        } else {
            if (WITH_LOCK(m_chainman.GetMutex(), return m_chainman.m_blockman.IsBlockPruned(*pindex))) {
                LogDebug(BCLog::NET, "Block was pruned before it could be read, %s", pfrom.DisconnectMsg());
            } else {
                LogError("Cannot load block from disk, %s", pfrom.DisconnectMsg());
            }
            pfrom.fDisconnect = true;
            return;
        }
        // Don't set pblock as we've sent the block
    } else {
        // Send block from disk
        std::shared_ptr<CBlock> pblockRead = std::make_shared<CBlock>();
        if (!m_chainman.m_blockman.ReadBlock(*pblockRead, block_pos, inv.hash)) {
            if (WITH_LOCK(m_chainman.GetMutex(), return m_chainman.m_blockman.IsBlockPruned(*pindex))) {
                LogDebug(BCLog::NET, "Block was pruned before it could be read, %s", pfrom.DisconnectMsg());
            } else {
                LogError("Cannot load block from disk, %s", pfrom.DisconnectMsg());
            }
            pfrom.fDisconnect = true;
            return;
        }
        pblock = pblockRead;
    }
    if (pblock) {
        if (inv.IsMsgBlk()) {
            MakeAndPushMessage(pfrom, NetMsgType::BLOCK, TX_NO_WITNESS(*pblock));
        } else if (inv.IsMsgWitnessBlk()) {
            MakeAndPushMessage(pfrom, NetMsgType::BLOCK, TX_WITH_WITNESS(*pblock));
        } else if (inv.IsMsgFilteredBlk()) {
            bool sendMerkleBlock = false;
            CMerkleBlock merkleBlock;
            if (auto tx_relay = peer.GetTxRelay(); tx_relay != nullptr) {
                LOCK(tx_relay->m_bloom_filter_mutex);
                if (tx_relay->m_bloom_filter) {
                    sendMerkleBlock = true;
                    merkleBlock = CMerkleBlock(*pblock, *tx_relay->m_bloom_filter);
                }
            }
            if (sendMerkleBlock) {
                MakeAndPushMessage(pfrom, NetMsgType::MERKLEBLOCK, merkleBlock);
                // CMerkleBlock only has hashes, so push matched txs too (no single-tx request protocol; duplicates allowed but must include all needed).
                for (const auto& [tx_idx, _] : merkleBlock.vMatchedTxn)
                    MakeAndPushMessage(pfrom, NetMsgType::TX, TX_NO_WITNESS(*pblock->vtx[tx_idx]));
            }
            // else
            // no response
        } else if (inv.IsMsgCmpctBlk()) {
            // For old blocks, peer likely lacks useful mempool for compact block; skip construction.
            // instead we respond with the full, non-compact block.
            if (can_direct_fetch && pindex->nHeight >= tip->nHeight - MAX_CMPCTBLOCK_DEPTH) {
                if (a_recent_compact_block && a_recent_compact_block->header.GetHash() == inv.hash) {
                    MakeAndPushMessage(pfrom, NetMsgType::CMPCTBLOCK, *a_recent_compact_block);
                } else {
                    CBlockHeaderAndShortTxIDs cmpctblock{*pblock, m_rng.rand64()};
                    MakeAndPushMessage(pfrom, NetMsgType::CMPCTBLOCK, cmpctblock);
                }
            } else {
                MakeAndPushMessage(pfrom, NetMsgType::BLOCK, TX_WITH_WITNESS(*pblock));
            }
        }
    }

    {
        LOCK(peer.m_block_inv_mutex);
        // Trigger the peer node to send a getblocks request for the next batch of inventory
        if (inv.hash == peer.m_continuation_block) {
            // Send immediately even if redundant, right after the last block (no waiting).
            std::vector<CInv> vInv;
            vInv.emplace_back(MSG_BLOCK, tip->GetBlockHash());
            MakeAndPushMessage(pfrom, NetMsgType::INV, vInv);
            peer.m_continuation_block.SetNull();
        }
    }
}

CTransactionRef PeerManagerImpl::FindTxForGetData(const Peer::TxRelay& tx_relay, const GenTxid& gtxid)
{
    // If a tx was in the mempool prior to the last INV for this peer, permit the request.
    auto txinfo{std::visit(
        [&](const auto& id) {
            return m_mempool.info_for_relay(id, WITH_LOCK(tx_relay.m_tx_inventory_mutex, return tx_relay.m_last_inv_sequence));
        },
        gtxid)};
    if (txinfo.tx) {
        return std::move(txinfo.tx);
    }

    // Or it might be from the most recent block
    {
        LOCK(m_most_recent_block_mutex);
        if (m_most_recent_block_txs != nullptr) {
            auto it = m_most_recent_block_txs->find(gtxid);
            if (it != m_most_recent_block_txs->end()) return it->second;
        }
    }

    return {};
}

void PeerManagerImpl::ProcessGetData(CNode& pfrom, Peer& peer, const std::atomic<bool>& interruptMsgProc)
{
    AssertLockNotHeld(cs_main);

    auto tx_relay = peer.GetTxRelay();

    std::deque<CInv>::iterator it = peer.m_getdata_requests.begin();
    std::vector<CInv> vNotFound;

    // Batch process TX items from front of getdata queue (common, efficient).
    while (it != peer.m_getdata_requests.end() && it->IsGenTxMsg()) {
        if (interruptMsgProc) return;
        // The send buffer provides backpressure. If there's no space in
        // the buffer, pause processing until the next call.
        if (pfrom.fPauseSend) break;

        const CInv &inv = *it++;

        if (tx_relay == nullptr) {
            // Ignore GETDATA requests for transactions from block-relay-only
            // peers and peers that asked us not to announce transactions.
            continue;
        }

        if (auto tx{FindTxForGetData(*tx_relay, ToGenTxid(inv))}) {
            // WTX and WITNESS_TX imply we serialize with witness
            const auto maybe_with_witness = (inv.IsMsgTx() ? TX_NO_WITNESS : TX_WITH_WITNESS);
            MakeAndPushMessage(pfrom, NetMsgType::TX, maybe_with_witness(*tx));
            m_mempool.RemoveUnbroadcastTx(tx->GetHash());
        } else {
            vNotFound.push_back(inv);
        }
    }

    // Only process one BLOCK item per call, since they're uncommon and can be
    // expensive to process.
    if (it != peer.m_getdata_requests.end() && !pfrom.fPauseSend) {
        const CInv &inv = *it++;
        if (inv.IsGenBlkMsg()) {
            ProcessGetBlockData(pfrom, peer, inv);
        }
        // Erase unknown-type queue items to avoid CPU spin (historical CVE: malformed GETDATA).
    }

    peer.m_getdata_requests.erase(peer.m_getdata_requests.begin(), it);

    if (!vNotFound.empty()) {
        // Send NOTFOUND so peer doesn't wait forever; SPV clients use it for dependency walking,
        // and nodes use it to skip waiting and request from other peers.
        MakeAndPushMessage(pfrom, NetMsgType::NOTFOUND, vNotFound);
    }
}

uint32_t PeerManagerImpl::GetFetchFlags(const Peer& peer) const
{
    uint32_t nFetchFlags = 0;
    if (CanServeWitnesses(peer)) {
        nFetchFlags |= MSG_WITNESS_FLAG;
    }
    return nFetchFlags;
}

void PeerManagerImpl::SendBlockTransactions(CNode& pfrom, Peer& peer, const CBlock& block, const BlockTransactionsRequest& req)
{
    BlockTransactions resp(req);
    for (size_t i = 0; i < req.indexes.size(); i++) {
        if (req.indexes[i] >= block.vtx.size()) {
            Misbehaving(peer, "getblocktxn with out-of-bounds tx indices");
            return;
        }
        resp.txn[i] = block.vtx[req.indexes[i]];
    }

    if (LogAcceptCategory(BCLog::CMPCTBLOCK, BCLog::Level::Debug)) {
        uint32_t tx_requested_size{0};
        for (const auto& tx : resp.txn) tx_requested_size += tx->ComputeTotalSize();
        LogDebug(BCLog::CMPCTBLOCK, "Peer %d sent us a GETBLOCKTXN for block %s, sending a BLOCKTXN with %u txns. (%u bytes)\n", pfrom.GetId(), block.GetHash().ToString(), resp.txn.size(), tx_requested_size);
    }
    MakeAndPushMessage(pfrom, NetMsgType::BLOCKTXN, resp);
}

bool PeerManagerImpl::CheckHeadersPoW(const std::vector<CBlockHeader>& headers, Peer& peer)
{
    // Do these headers have proof-of-work matching what's claimed?
    if (!HasValidProofOfWork(headers, m_chainparams.GetConsensus())) {
        Misbehaving(peer, "header with invalid proof of work");
        return false;
    }

    // Are these headers connected to each other?
    if (!CheckHeadersAreContinuous(headers)) {
        Misbehaving(peer, "non-continuous headers sequence");
        return false;
    }
    return true;
}

arith_uint256 PeerManagerImpl::GetAntiDoSWorkThreshold()
{
    arith_uint256 near_chaintip_work = 0;
    LOCK(cs_main);
    if (m_chainman.ActiveChain().Tip() != nullptr) {
        const CBlockIndex *tip = m_chainman.ActiveChain().Tip();
        // Use a 144 block buffer, so that we'll accept headers that fork from
        // near our tip.
        near_chaintip_work = tip->nChainWork - std::min<arith_uint256>(144*GetBlockProof(*tip), tip->nChainWork);
    }
    return std::max(near_chaintip_work, m_chainman.MinimumChainWork());
}

/**
 * Special handling for unconnecting headers that might be part of a block
 * announcement.
 *
 * We'll send a getheaders message in response to try to connect the chain.
 */
void PeerManagerImpl::HandleUnconnectingHeaders(CNode& pfrom, Peer& peer,
        const std::vector<CBlockHeader>& headers)
{
    // Try to fill in the missing headers.
    const CBlockIndex* best_header{WITH_LOCK(cs_main, return m_chainman.m_best_header)};
    if (MaybeSendGetHeaders(pfrom, GetLocator(best_header), peer)) {
        LogDebug(BCLog::NET, "received header %s: missing prev block %s, sending getheaders (%d) to end (peer=%d)\n",
            headers[0].GetHash().ToString(),
            headers[0].hashPrevBlock.ToString(),
            best_header->nHeight,
            pfrom.GetId());
    }

    // Set hashLastUnknownBlock so we can use this peer for download once headers arrive (from any peer).
    WITH_LOCK(cs_main, UpdateBlockAvailability(pfrom.GetId(), headers.back().GetHash()));
}

bool PeerManagerImpl::CheckHeadersAreContinuous(const std::vector<CBlockHeader>& headers) const
{
    uint256 hashLastBlock;
    for (const CBlockHeader& header : headers) {
        if (!hashLastBlock.IsNull() && header.hashPrevBlock != hashLastBlock) {
            return false;
        }
        hashLastBlock = header.GetHash();
    }
    return true;
}

bool PeerManagerImpl::IsContinuationOfLowWorkHeadersSync(Peer& peer, CNode& pfrom, std::vector<CBlockHeader>& headers)
{
    if (peer.m_headers_sync) {
        auto result = peer.m_headers_sync->ProcessNextHeaders(headers, headers.size() == m_opts.max_headers_result);
        // If it is a valid continuation, we should treat the existing getheaders request as responded to.
        if (result.success) peer.m_last_getheaders_timestamp = {};
        if (result.request_more) {
            auto locator = peer.m_headers_sync->NextHeadersRequestLocator();
            // If we were instructed to ask for a locator, it should not be empty.
            Assume(!locator.vHave.empty());
            // We can only be instructed to request more if processing was successful.
            Assume(result.success);
            if (!locator.vHave.empty()) {
                // It should be impossible for the getheaders request to fail,
                // because we just cleared the last getheaders timestamp.
                bool sent_getheaders = MaybeSendGetHeaders(pfrom, locator, peer);
                Assume(sent_getheaders);
                LogDebug(BCLog::NET, "more getheaders (from %s) to peer=%d\n",
                    locator.vHave.front().ToString(), pfrom.GetId());
            }
        }

        if (peer.m_headers_sync->GetState() == HeadersSyncState::State::FINAL) {
            peer.m_headers_sync.reset(nullptr);

            // Delete peer's presync stats entry; if best peer, replaced by next else{} trigger.
            LOCK(m_headers_presync_mutex);
            m_headers_presync_stats.erase(pfrom.GetId());
        } else {
            // Build statistics for this peer's sync.
            HeadersPresyncStats stats;
            stats.first = peer.m_headers_sync->GetPresyncWork();
            if (peer.m_headers_sync->GetState() == HeadersSyncState::State::PRESYNC) {
                stats.second = {peer.m_headers_sync->GetPresyncHeight(),
                                peer.m_headers_sync->GetPresyncTime()};
            }

            // Update statistics in stats.
            LOCK(m_headers_presync_mutex);
            m_headers_presync_stats[pfrom.GetId()] = stats;
            auto best_it = m_headers_presync_stats.find(m_headers_presync_bestpeer);
            bool best_updated = false;
            if (best_it == m_headers_presync_stats.end()) {
                // If the cached best peer is outdated, iterate over all remaining ones (including
                // newly updated one) to find the best one.
                NodeId peer_best{-1};
                const HeadersPresyncStats* stat_best{nullptr};
                for (const auto& [peer, stat] : m_headers_presync_stats) {
                    if (!stat_best || stat > *stat_best) {
                        peer_best = peer;
                        stat_best = &stat;
                    }
                }
                m_headers_presync_bestpeer = peer_best;
                best_updated = (peer_best == pfrom.GetId());
            } else if (best_it->first == pfrom.GetId() || stats > best_it->second) {
                // pfrom was and remains the best peer, or pfrom just became best.
                m_headers_presync_bestpeer = pfrom.GetId();
                best_updated = true;
            }
            if (best_updated && stats.second.has_value()) {
                // If the best peer updated, and it is in its first phase, signal.
                m_headers_presync_should_signal = true;
            }
        }

        if (result.success) {
            // We only overwrite the headers passed in if processing was
            // successful.
            headers.swap(result.pow_validated_headers);
        }

        return result.success;
    }
    // No sync in progress, sync failed, or returning headers to caller for processing.
    return false;
}

bool PeerManagerImpl::TryLowWorkHeadersSync(Peer& peer, CNode& pfrom, const CBlockIndex& chain_start_header, std::vector<CBlockHeader>& headers)
{
    // Calculate the claimed total work on this chain.
    arith_uint256 total_work = chain_start_header.nChainWork + CalculateClaimedHeadersWork(headers);

    // Our dynamic anti-DoS threshold (minimum work required on a headers chain
    // before we'll store it)
    arith_uint256 minimum_chain_work = GetAntiDoSWorkThreshold();

    // Avoid DoS via low-difficulty-headers by only processing if the headers
    // are part of a chain with sufficient work.
    if (total_work < minimum_chain_work) {
        // Only sync if peer's headers message was full (else no more headers to sync).
        if (headers.size() == m_opts.max_headers_result) {
            // Could advance to last known header, but unlikely to matter (ProcessHeadersMessage handles known headers).
            LOCK(peer.m_headers_sync_mutex);
            peer.m_headers_sync.reset(new HeadersSyncState(peer.m_id, m_chainparams.GetConsensus(),
                m_chainparams.HeadersSync(), chain_start_header, minimum_chain_work));

            // HeadersSyncState created; process headers (failures handled in IsContinuationOfLowWorkHeadersSync).
            (void)IsContinuationOfLowWorkHeadersSync(peer, pfrom, headers);
        } else {
            LogDebug(BCLog::NET, "Ignoring low-work chain (height=%u) from peer=%d\n", chain_start_header.nHeight + headers.size(), pfrom.GetId());
        }

        // The peer has not yet given us a chain that meets our work threshold,
        // so we want to prevent further processing of the headers in any case.
        headers = {};
        return true;
    }

    return false;
}

bool PeerManagerImpl::IsAncestorOfBestHeaderOrTip(const CBlockIndex* header)
{
    if (header == nullptr) {
        return false;
    } else if (m_chainman.m_best_header != nullptr && header == m_chainman.m_best_header->GetAncestor(header->nHeight)) {
        return true;
    } else if (m_chainman.ActiveChain().Contains(*header)) {
        return true;
    }
    return false;
}

bool PeerManagerImpl::MaybeSendGetHeaders(CNode& pfrom, const CBlockLocator& locator, Peer& peer)
{
    const auto current_time = NodeClock::now();

    // Only allow a new getheaders message to go out if we don't have a recent
    // one already in-flight
    if (current_time - peer.m_last_getheaders_timestamp > HEADERS_RESPONSE_TIME) {
        MakeAndPushMessage(pfrom, NetMsgType::GETHEADERS, locator, uint256());
        peer.m_last_getheaders_timestamp = current_time;
        return true;
    }
    return false;
}

/*
 * Given a new headers tip ending in last_header, potentially request blocks towards that tip.
 * We require that the given tip have at least as much work as our tip, and for
 * our current tip to be "close to synced" (see CanDirectFetch()).
 */
void PeerManagerImpl::HeadersDirectFetchBlocks(CNode& pfrom, const Peer& peer, const CBlockIndex& last_header)
{
    LOCK(cs_main);
    CNodeState *nodestate = State(pfrom.GetId());

    if (CanDirectFetch() && last_header.IsValid(BLOCK_VALID_TREE) && m_chainman.ActiveChain().Tip()->nChainWork <= last_header.nChainWork) {
        std::vector<const CBlockIndex*> vToFetch;
        const CBlockIndex* pindexWalk{&last_header};
        // Calculate all the blocks we'd need to switch to last_header, up to a limit.
        while (pindexWalk && !m_chainman.ActiveChain().Contains(*pindexWalk) && vToFetch.size() <= MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
            if (!(pindexWalk->nStatus & BLOCK_HAVE_DATA) &&
                    !IsBlockRequested(pindexWalk->GetBlockHash()) &&
                    (!DeploymentActiveAt(*pindexWalk, m_chainman, Consensus::DEPLOYMENT_SEGWIT) || CanServeWitnesses(peer))) {
                // We don't have this block, and it's not yet in flight.
                vToFetch.push_back(pindexWalk);
            }
            pindexWalk = pindexWalk->pprev;
        }
        // Large reorg near tip: bail out of direct fetch, use parallel download (common ancestor must exist).
        if (!m_chainman.ActiveChain().Contains(*Assert(pindexWalk))) {
            LogDebug(BCLog::NET, "Large reorg, won't direct fetch to %s (%d)\n",
                     last_header.GetBlockHash().ToString(),
                     last_header.nHeight);
        } else {
            std::vector<CInv> vGetData;
            // Download as much as possible, from earliest to latest.
            for (const CBlockIndex* pindex : vToFetch | std::views::reverse) {
                if (nodestate->vBlocksInFlight.size() >= MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
                    // Can't download any more from this peer
                    break;
                }
                uint32_t nFetchFlags = GetFetchFlags(peer);
                vGetData.emplace_back(MSG_BLOCK | nFetchFlags, pindex->GetBlockHash());
                BlockRequested(pfrom.GetId(), *pindex);
                LogDebug(BCLog::NET, "Requesting block %s from peer=%d",
                         pindex->GetBlockHash().ToString(), pfrom.GetId());
            }
            if (vGetData.size() > 1) {
                LogDebug(BCLog::NET, "Downloading blocks toward %s (%d) via headers direct fetch\n",
                         last_header.GetBlockHash().ToString(),
                         last_header.nHeight);
            }
            if (vGetData.size() > 0) {
                if (!m_opts.ignore_incoming_txs &&
                        nodestate->m_provides_cmpctblocks &&
                        vGetData.size() == 1 &&
                        mapBlocksInFlight.size() == 1 &&
                        last_header.pprev->IsValid(BLOCK_VALID_CHAIN)) {
                    // In any case, we want to download using a compact block, not a regular one
                    vGetData[0] = CInv(MSG_CMPCT_BLOCK, vGetData[0].hash);
                }
                MakeAndPushMessage(pfrom, NetMsgType::GETDATA, vGetData);
            }
        }
    }
}

/**
 * Given receipt of headers from a peer ending in last_header, along with
 * whether that header was new and whether the headers message was full,
 * update the state we keep for the peer.
 */
void PeerManagerImpl::UpdatePeerStateForReceivedHeaders(CNode& pfrom, Peer& peer,
        const CBlockIndex& last_header, bool received_new_header, bool may_have_more_headers)
{
    LOCK(cs_main);
    CNodeState *nodestate = State(pfrom.GetId());

    UpdateBlockAvailability(pfrom.GetId(), last_header.GetBlockHash());

    // pindexBestKnownBlock should be non-null (set in UpdateBlockAvailability); nullptr checks are belt-and-suspenders.

    if (received_new_header && last_header.nChainWork > m_chainman.ActiveChain().Tip()->nChainWork) {
        nodestate->m_last_block_announcement = GetTime();
    }

    // If we're in IBD, we want outbound peers that will serve us a useful
    // chain. Disconnect peers that are on chains with insufficient work.
    if (m_chainman.IsInitialBlockDownload() && !may_have_more_headers) {
        // If the peer has no more headers to give us, then we know we have
        // their tip.
        if (nodestate->pindexBestKnownBlock && nodestate->pindexBestKnownBlock->nChainWork < m_chainman.MinimumChainWork()) {
            // Peer has too little work; disconnect if outbound candidate (compare to MinimumChainWork, not ActiveTip, as anti-DoS).
            if (pfrom.IsOutboundOrBlockRelayConn()) {
                LogInfo("outbound peer headers chain has insufficient work, %s", pfrom.DisconnectMsg());
                pfrom.fDisconnect = true;
            }
        }
    }

    // Protect outbound full-relay peers from bad/lagging chain logic (block-relay peers excluded; see ChainSyncTimeoutState).
    if (!pfrom.fDisconnect && pfrom.IsFullOutboundConn() && nodestate->pindexBestKnownBlock != nullptr) {
        if (m_outbound_peers_with_protect_from_disconnect < MAX_OUTBOUND_PEERS_TO_PROTECT_FROM_DISCONNECT && nodestate->pindexBestKnownBlock->nChainWork >= m_chainman.ActiveChain().Tip()->nChainWork && !nodestate->m_chain_sync.m_protect) {
            LogDebug(BCLog::NET, "Protecting outbound peer=%d from eviction\n", pfrom.GetId());
            nodestate->m_chain_sync.m_protect = true;
            ++m_outbound_peers_with_protect_from_disconnect;
        }
    }
}

void PeerManagerImpl::ProcessHeadersMessage(CNode& pfrom, Peer& peer,
                                            std::vector<CBlockHeader>&& headers,
                                            bool via_compact_block)
{
    size_t nCount = headers.size();

    if (nCount == 0) {
        // Empty headers: stop asking; peer has nothing to give (may have lost chain during sync).
        // (perhaps it reorged to our chain). Clear download state for this peer.
        LOCK(peer.m_headers_sync_mutex);
        if (peer.m_headers_sync) {
            peer.m_headers_sync.reset(nullptr);
            LOCK(m_headers_presync_mutex);
            m_headers_presync_stats.erase(pfrom.GetId());
        }
        // A headers message with no headers cannot be an announcement, so assume
        // it is a response to our last getheaders request, if there is one.
        peer.m_last_getheaders_timestamp = {};
        return;
    }

    // Sanity check: headers must pass PoW (anti-DoS, required before HeadersSyncState).
    if (!CheckHeadersPoW(headers, peer)) {
        // Misbehaving handled in CheckHeadersPoW; header errors always punishable.
        return;
    }

    const CBlockIndex *pindexLast = nullptr;

    // already_validated_work: headers processed in low-work sync can skip anti-DoS checks on REDOWNLOAD.
    bool already_validated_work = false;

    // If we're in the middle of headers sync, let it do its magic.
    bool have_headers_sync = false;
    {
        LOCK(peer.m_headers_sync_mutex);

        already_validated_work = IsContinuationOfLowWorkHeadersSync(peer, pfrom, headers);

        // Headers may be untouched/erased/replaced by sync; check if we still need to process.
        if (headers.empty()) {
            return;
        }

        have_headers_sync = !!peer.m_headers_sync;
    }

    // Do these headers connect to something in our block index?
    const CBlockIndex *chain_start_header{WITH_LOCK(::cs_main, return m_chainman.m_blockman.LookupBlockIndex(headers[0].hashPrevBlock))};
    bool headers_connect_blockindex{chain_start_header != nullptr};

    if (!headers_connect_blockindex) {
        // BIP 130 block announcement: handle non-connecting headers (may be benign).
        HandleUnconnectingHeaders(pfrom, peer, headers);
        return;
    }

    // Connecting headers = response to getheaders; clear last request time (non-connecting can't be response).
    peer.m_last_getheaders_timestamp = {};

    // Skip anti-DoS for known headers (ancestor of best header/tip; no memory/fingerprint leak).
    const CBlockIndex *last_received_header{nullptr};
    {
        LOCK(cs_main);
        last_received_header = m_chainman.m_blockman.LookupBlockIndex(headers.back().GetHash());
        already_validated_work = already_validated_work || IsAncestorOfBestHeaderOrTip(last_received_header);
    }

    // NoBan peers bypass anti-DoS logic (saves bandwidth for trusted startup peers).
    if (pfrom.HasPermission(NetPermissionFlags::NoBan)) {
        already_validated_work = true;
    }

    // Headers connect to block index; run anti-DoS checks to decide process vs. store.
    if (!already_validated_work && TryLowWorkHeadersSync(peer, pfrom,
                                                         *chain_start_header, headers)) {
        // If we successfully started a low-work headers sync, then there
        // should be no headers to process any further.
        Assume(headers.empty());
        return;
    }

    // At this point, we have a set of headers with sufficient work on them
    // which can be processed.

    // If we don't have the last header, then this peer will have given us
    // something new (if these headers are valid).
    bool received_new_header{last_received_header == nullptr};

    // Now process all the headers.
    BlockValidationState state;
    const bool processed{m_chainman.ProcessNewBlockHeaders(headers,
                                                           /*min_pow_checked=*/true,
                                                           state, &pindexLast)};
    if (!processed) {
        if (state.IsInvalid()) {
            if (!pfrom.IsInboundConn() && state.GetResult() == BlockValidationResult::BLOCK_CACHED_INVALID) {
                // Warn user if outgoing peers send us headers of blocks that we previously marked as invalid.
                LogWarning("%s (received from peer=%i). "
                           "If this happens with all peers, consider database corruption (that -reindex may fix) "
                           "or a potential consensus incompatibility.",
                           state.GetDebugMessage(), pfrom.GetId());
            }
            MaybePunishNodeForBlock(pfrom.GetId(), state, via_compact_block, "invalid header received");
            return;
        }
    }
    assert(pindexLast);

    if (processed && received_new_header) {
        LogBlockHeader(*pindexLast, pfrom, /*via_compact_block=*/false);
    }

    // Consider fetching more headers if we are not using our headers-sync mechanism.
    if (nCount == m_opts.max_headers_result && !have_headers_sync) {
        // Headers message had its maximum size; the peer may have more headers.
        if (MaybeSendGetHeaders(pfrom, GetLocator(pindexLast), peer)) {
            LogDebug(BCLog::NET, "more getheaders (%d) to end to peer=%d", pindexLast->nHeight, pfrom.GetId());
        }
    }

    UpdatePeerStateForReceivedHeaders(pfrom, peer, *pindexLast, received_new_header, nCount == m_opts.max_headers_result);

    // Consider immediately downloading blocks.
    HeadersDirectFetchBlocks(pfrom, peer, *pindexLast);

    return;
}

std::optional<node::PackageToValidate> PeerManagerImpl::ProcessInvalidTx(NodeId nodeid, const CTransactionRef& ptx, const TxValidationState& state,
                                       bool first_time_failure)
{
    AssertLockNotHeld(m_peer_mutex);
    AssertLockHeld(g_msgproc_mutex);
    AssertLockHeld(m_tx_download_mutex);

    PeerRef peer{GetPeerRef(nodeid)};

    LogDebug(BCLog::MEMPOOLREJ, "%s (wtxid=%s) from peer=%d was not accepted: %s\n",
        ptx->GetHash().ToString(),
        ptx->GetWitnessHash().ToString(),
        nodeid,
        state.ToString());

    const auto& [add_extra_compact_tx, unique_parents, package_to_validate] = m_txdownloadman.MempoolRejectedTx(ptx, state, nodeid, first_time_failure);

    if (add_extra_compact_tx && RecursiveDynamicUsage(*ptx) < 100000) {
        AddToCompactExtraTransactions(ptx);
    }
    for (const Txid& parent_txid : unique_parents) {
        if (peer) AddKnownTx(*peer, parent_txid.ToUint256());
    }

    return package_to_validate;
}

void PeerManagerImpl::ProcessValidTx(NodeId nodeid, const CTransactionRef& tx, const std::list<CTransactionRef>& replaced_transactions)
{
    AssertLockNotHeld(m_peer_mutex);
    AssertLockHeld(g_msgproc_mutex);
    AssertLockHeld(m_tx_download_mutex);

    m_txdownloadman.MempoolAcceptedTx(tx);

    LogDebug(BCLog::MEMPOOL, "AcceptToMemoryPool: peer=%d: accepted %s (wtxid=%s) (poolsz %u txn, %u kB)\n",
             nodeid,
             tx->GetHash().ToString(),
             tx->GetWitnessHash().ToString(),
             m_mempool.size(), m_mempool.DynamicMemoryUsage() / 1000);

    InitiateTxBroadcastToAll(tx->GetHash(), tx->GetWitnessHash());

    for (const CTransactionRef& removedTx : replaced_transactions) {
        AddToCompactExtraTransactions(removedTx);
    }
}

void PeerManagerImpl::ProcessPackageResult(const node::PackageToValidate& package_to_validate, const PackageMempoolAcceptResult& package_result)
{
    AssertLockNotHeld(m_peer_mutex);
    AssertLockHeld(g_msgproc_mutex);
    AssertLockHeld(m_tx_download_mutex);

    const auto& package = package_to_validate.m_txns;
    const auto& senders = package_to_validate.m_senders;

    if (package_result.m_state.IsInvalid()) {
        m_txdownloadman.MempoolRejectedPackage(package);
    }
    // We currently only expect to process 1-parent-1-child packages. Remove if this changes.
    if (!Assume(package.size() == 2)) return;

    // Iterate backwards to erase in-package descendants from the orphanage before they become
    // relevant in AddChildrenToWorkSet.
    auto package_iter = package.rbegin();
    auto senders_iter = senders.rbegin();
    while (package_iter != package.rend()) {
        const auto& tx = *package_iter;
        const NodeId nodeid = *senders_iter;
        const auto it_result{package_result.m_tx_results.find(tx->GetWitnessHash())};

        // It is not guaranteed that a result exists for every transaction.
        if (it_result != package_result.m_tx_results.end()) {
            const auto& tx_result = it_result->second;
            switch (tx_result.m_result_type) {
                case MempoolAcceptResult::ResultType::VALID:
                {
                    ProcessValidTx(nodeid, tx, tx_result.m_replaced_transactions);
                    break;
                }
                case MempoolAcceptResult::ResultType::INVALID:
                case MempoolAcceptResult::ResultType::DIFFERENT_WITNESS:
                {
                    // Skip vExtraTxnForCompact (already added via orphanage/TX_RECONSIDERABLE); revisit if package submission changes.
                    ProcessInvalidTx(nodeid, tx, tx_result.m_state, /*first_time_failure=*/false);
                    break;
                }
                case MempoolAcceptResult::ResultType::MEMPOOL_ENTRY:
                {
                    // AlreadyHaveTx() should be catching transactions that are already in mempool.
                    Assume(false);
                    break;
                }
            }
        }
        package_iter++;
        senders_iter++;
    }
}

// NOTE: the orphan processing used to be uninterruptible and quadratic, which could allow a peer to stall the node for
// hours with specially crafted transactions. See https://tkncore.org/en/2024/07/03/disclose-orphan-dos.
bool PeerManagerImpl::ProcessOrphanTx(Peer& peer)
{
    AssertLockHeld(g_msgproc_mutex);
    LOCK2(::cs_main, m_tx_download_mutex);

    CTransactionRef porphanTx = nullptr;

    while (CTransactionRef porphanTx = m_txdownloadman.GetTxToReconsider(peer.m_id)) {
        const MempoolAcceptResult result = m_chainman.ProcessTransaction(porphanTx);
        const TxValidationState& state = result.m_state;
        const Txid& orphanHash = porphanTx->GetHash();
        const Wtxid& orphan_wtxid = porphanTx->GetWitnessHash();

        if (result.m_result_type == MempoolAcceptResult::ResultType::VALID) {
            LogDebug(BCLog::TXPACKAGES, "   accepted orphan tx %s (wtxid=%s)\n", orphanHash.ToString(), orphan_wtxid.ToString());
            ProcessValidTx(peer.m_id, porphanTx, result.m_replaced_transactions);
            return true;
        } else if (state.GetResult() != TxValidationResult::TX_MISSING_INPUTS) {
            LogDebug(BCLog::TXPACKAGES, "   invalid orphan tx %s (wtxid=%s) from peer=%d. %s\n",
                orphanHash.ToString(),
                orphan_wtxid.ToString(),
                peer.m_id,
                state.ToString());

            if (Assume(state.IsInvalid() &&
                       state.GetResult() != TxValidationResult::TX_UNKNOWN &&
                       state.GetResult() != TxValidationResult::TX_NO_MEMPOOL &&
                       state.GetResult() != TxValidationResult::TX_RESULT_UNSET)) {
                ProcessInvalidTx(peer.m_id, porphanTx, state, /*first_time_failure=*/false);
            }
            return true;
        }
    }

    return false;
}

bool PeerManagerImpl::PrepareBlockFilterRequest(CNode& node, Peer& peer,
                                                BlockFilterType filter_type, uint32_t start_height,
                                                const uint256& stop_hash, uint32_t max_height_diff,
                                                const CBlockIndex*& stop_index,
                                                BlockFilterIndex*& filter_index)
{
    const bool supported_filter_type =
        (filter_type == BlockFilterType::BASIC &&
         (peer.m_our_services & NODE_COMPACT_FILTERS));
    if (!supported_filter_type) {
        LogDebug(BCLog::NET, "peer requested unsupported block filter type: %d, %s",
                 static_cast<uint8_t>(filter_type), node.DisconnectMsg());
        node.fDisconnect = true;
        return false;
    }

    {
        LOCK(cs_main);
        stop_index = m_chainman.m_blockman.LookupBlockIndex(stop_hash);

        // Check that the stop block exists and the peer would be allowed to fetch it.
        if (!stop_index || !BlockRequestAllowed(*stop_index)) {
            LogDebug(BCLog::NET, "peer requested invalid block hash: %s, %s",
                     stop_hash.ToString(), node.DisconnectMsg());
            node.fDisconnect = true;
            return false;
        }
    }

    uint32_t stop_height = stop_index->nHeight;
    if (start_height > stop_height) {
        LogDebug(BCLog::NET, "peer sent invalid getcfilters/getcfheaders with "
                 "start height %d and stop height %d, %s",
                 start_height, stop_height, node.DisconnectMsg());
        node.fDisconnect = true;
        return false;
    }
    if (stop_height - start_height >= max_height_diff) {
        LogDebug(BCLog::NET, "peer requested too many cfilters/cfheaders: %d / %d, %s",
                 stop_height - start_height + 1, max_height_diff, node.DisconnectMsg());
        node.fDisconnect = true;
        return false;
    }

    filter_index = GetBlockFilterIndex(filter_type);
    if (!filter_index) {
        LogDebug(BCLog::NET, "Filter index for supported type %s not found\n", BlockFilterTypeName(filter_type));
        return false;
    }

    return true;
}

void PeerManagerImpl::ProcessGetCFilters(CNode& node, Peer& peer, DataStream& vRecv)
{
    uint8_t filter_type_ser;
    uint32_t start_height;
    uint256 stop_hash;

    vRecv >> filter_type_ser >> start_height >> stop_hash;

    const BlockFilterType filter_type = static_cast<BlockFilterType>(filter_type_ser);

    const CBlockIndex* stop_index;
    BlockFilterIndex* filter_index;
    if (!PrepareBlockFilterRequest(node, peer, filter_type, start_height, stop_hash,
                                   MAX_GETCFILTERS_SIZE, stop_index, filter_index)) {
        return;
    }

    std::vector<BlockFilter> filters;
    if (!filter_index->LookupFilterRange(start_height, stop_index, filters)) {
        LogDebug(BCLog::NET, "Failed to find block filter in index: filter_type=%s, start_height=%d, stop_hash=%s\n",
                     BlockFilterTypeName(filter_type), start_height, stop_hash.ToString());
        return;
    }

    for (const auto& filter : filters) {
        MakeAndPushMessage(node, NetMsgType::CFILTER, filter);
    }
}

void PeerManagerImpl::ProcessGetCFHeaders(CNode& node, Peer& peer, DataStream& vRecv)
{
    uint8_t filter_type_ser;
    uint32_t start_height;
    uint256 stop_hash;

    vRecv >> filter_type_ser >> start_height >> stop_hash;

    const BlockFilterType filter_type = static_cast<BlockFilterType>(filter_type_ser);

    const CBlockIndex* stop_index;
    BlockFilterIndex* filter_index;
    if (!PrepareBlockFilterRequest(node, peer, filter_type, start_height, stop_hash,
                                   MAX_GETCFHEADERS_SIZE, stop_index, filter_index)) {
        return;
    }

    uint256 prev_header;
    if (start_height > 0) {
        const CBlockIndex* const prev_block =
            stop_index->GetAncestor(static_cast<int>(start_height - 1));
        if (!filter_index->LookupFilterHeader(prev_block, prev_header)) {
            LogDebug(BCLog::NET, "Failed to find block filter header in index: filter_type=%s, block_hash=%s\n",
                         BlockFilterTypeName(filter_type), prev_block->GetBlockHash().ToString());
            return;
        }
    }

    std::vector<uint256> filter_hashes;
    if (!filter_index->LookupFilterHashRange(start_height, stop_index, filter_hashes)) {
        LogDebug(BCLog::NET, "Failed to find block filter hashes in index: filter_type=%s, start_height=%d, stop_hash=%s\n",
                     BlockFilterTypeName(filter_type), start_height, stop_hash.ToString());
        return;
    }

    MakeAndPushMessage(node, NetMsgType::CFHEADERS,
              filter_type_ser,
              stop_index->GetBlockHash(),
              prev_header,
              filter_hashes);
}

void PeerManagerImpl::ProcessGetCFCheckPt(CNode& node, Peer& peer, DataStream& vRecv)
{
    uint8_t filter_type_ser;
    uint256 stop_hash;

    vRecv >> filter_type_ser >> stop_hash;

    const BlockFilterType filter_type = static_cast<BlockFilterType>(filter_type_ser);

    const CBlockIndex* stop_index;
    BlockFilterIndex* filter_index;
    if (!PrepareBlockFilterRequest(node, peer, filter_type, /*start_height=*/0, stop_hash,
                                   /*max_height_diff=*/std::numeric_limits<uint32_t>::max(),
                                   stop_index, filter_index)) {
        return;
    }

    std::vector<uint256> headers(stop_index->nHeight / CFCHECKPT_INTERVAL);

    // Populate headers.
    const CBlockIndex* block_index = stop_index;
    for (int i = headers.size() - 1; i >= 0; i--) {
        int height = (i + 1) * CFCHECKPT_INTERVAL;
        block_index = block_index->GetAncestor(height);

        if (!filter_index->LookupFilterHeader(block_index, headers[i])) {
            LogDebug(BCLog::NET, "Failed to find block filter header in index: filter_type=%s, block_hash=%s\n",
                         BlockFilterTypeName(filter_type), block_index->GetBlockHash().ToString());
            return;
        }
    }

    MakeAndPushMessage(node, NetMsgType::CFCHECKPT,
              filter_type_ser,
              stop_index->GetBlockHash(),
              headers);
}

void PeerManagerImpl::ProcessBlock(CNode& node, const std::shared_ptr<const CBlock>& block, bool force_processing, bool min_pow_checked)
{
    bool new_block{false};
    m_chainman.ProcessNewBlock(block, force_processing, min_pow_checked, &new_block);
    if (new_block) {
        node.m_last_block_time = GetTime<std::chrono::seconds>();
        // Erase block request even if from different peer (block now on disk).
        LOCK(cs_main);
        RemoveBlockRequest(block->GetHash(), std::nullopt);
    } else {
        LOCK(cs_main);
        mapBlockSource.erase(block->GetHash());
    }
}

void PeerManagerImpl::ProcessCompactBlockTxns(CNode& pfrom, Peer& peer, const BlockTransactions& block_transactions)
{
    std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
    bool fBlockRead{false};
    {
        LOCK(cs_main);

        auto range_flight = mapBlocksInFlight.equal_range(block_transactions.blockhash);
        size_t already_in_flight = std::distance(range_flight.first, range_flight.second);
        bool requested_block_from_this_peer{false};

        // Multimap ensures ordering of outstanding requests. It's either empty or first in line.
        bool first_in_flight = already_in_flight == 0 || (range_flight.first->second.first == pfrom.GetId());

        while (range_flight.first != range_flight.second) {
            auto [node_id, block_it] = range_flight.first->second;
            if (node_id == pfrom.GetId() && block_it->partialBlock) {
                requested_block_from_this_peer = true;
                break;
            }
            range_flight.first++;
        }

        if (!requested_block_from_this_peer) {
            LogDebug(BCLog::NET, "Peer %d sent us block transactions for block we weren't expecting\n", pfrom.GetId());
            return;
        }

        PartiallyDownloadedBlock& partialBlock = *range_flight.first->second.second->partialBlock;

        if (partialBlock.header.IsNull()) {
            // Header may be empty if FillBlock wiped it but left PartiallyDownloadedBlock (skip LookupBlockIndex).
            RemoveBlockRequest(block_transactions.blockhash, pfrom.GetId());
            Misbehaving(peer, "previous compact block reconstruction attempt failed");
            LogDebug(BCLog::NET, "Peer %d sent compact block transactions multiple times", pfrom.GetId());
            return;
        }

        // We should not have gotten this far in compact block processing unless it's attached to a known header
        const CBlockIndex* prev_block{Assume(m_chainman.m_blockman.LookupBlockIndex(partialBlock.header.hashPrevBlock))};
        ReadStatus status = partialBlock.FillBlock(*pblock, block_transactions.txn,
                                                   /*segwit_active=*/DeploymentActiveAfter(prev_block, m_chainman, Consensus::DEPLOYMENT_SEGWIT));
        if (status == READ_STATUS_INVALID) {
            RemoveBlockRequest(block_transactions.blockhash, pfrom.GetId()); // Reset in-flight state in case Misbehaving does not result in a disconnect
            Misbehaving(peer, "invalid compact block/non-matching block transactions");
            return;
        } else if (status == READ_STATUS_FAILED) {
            if (first_in_flight) {
                // Collision: fall back to getdata; keep failed partialBlock to block re-announcement from same peer.
                std::vector<CInv> invs;
                invs.emplace_back(MSG_BLOCK | GetFetchFlags(peer), block_transactions.blockhash);
                MakeAndPushMessage(pfrom, NetMsgType::GETDATA, invs);
            } else {
                RemoveBlockRequest(block_transactions.blockhash, pfrom.GetId());
                LogDebug(BCLog::NET, "Peer %d sent us a compact block but it failed to reconstruct, waiting on first download to complete\n", pfrom.GetId());
                return;
            }
        } else {
            // Block is okay for further processing
            RemoveBlockRequest(block_transactions.blockhash, pfrom.GetId()); // it is now an empty pointer
            fBlockRead = true;
            // mapBlockSource race with cs_main is fine; BIP 152 allows compact block relay after header validation (don't punish invalid blocks).
            mapBlockSource.emplace(block_transactions.blockhash, std::make_pair(pfrom.GetId(), false));
        }
    } // Don't hold cs_main when we call into ProcessNewBlock
    if (fBlockRead) {
        // Force-process requested block (bypasses AcceptBlock anti-DoS; safe due to compact block protections).
        ProcessBlock(pfrom, pblock, /*force_processing=*/true, /*min_pow_checked=*/true);
    }
    return;
}

void PeerManagerImpl::LogBlockHeader(const CBlockIndex& index, const CNode& peer, bool via_compact_block) {
    // Log new valid headers (call only after validation to prevent spam; detects selfish mining and non-followup peers).
    const auto msg = strprintf(
        "Saw new %sheader hash=%s height=%d %s",
        via_compact_block ? "cmpctblock " : "",
        index.GetBlockHash().ToString(),
        index.nHeight,
        peer.LogPeer()
    );
    if (m_chainman.IsInitialBlockDownload()) {
        LogDebug(BCLog::VALIDATION, "%s", msg);
    } else {
        LogInfo("%s", msg);
    }
}

void PeerManagerImpl::PushPrivateBroadcastTx(CNode& node)
{
    Assume(node.IsPrivateBroadcastConn());

    const auto opt_tx{m_tx_for_private_broadcast.PickTxForSend(node.GetId(), CService{node.addr})};
    if (!opt_tx) {
        LogDebug(BCLog::PRIVBROADCAST, "Disconnecting: no more transactions for private broadcast (connected in vain), %s", node.LogPeer());
        node.fDisconnect = true;
        return;
    }
    const CTransactionRef& tx{*opt_tx};

    LogDebug(BCLog::PRIVBROADCAST, "P2P handshake completed, sending INV for txid=%s%s, %s",
             tx->GetHash().ToString(), tx->HasWitness() ? strprintf(", wtxid=%s", tx->GetWitnessHash().ToString()) : "",
             node.LogPeer());

    MakeAndPushMessage(node, NetMsgType::INV, std::vector<CInv>{{CInv{MSG_TX, tx->GetHash().ToUint256()}}});
}

void PeerManagerImpl::ProcessMessage(Peer& peer, CNode& pfrom, const std::string& msg_type, DataStream& vRecv,
                                     const NodeClock::time_point time_received,
                                     const std::atomic<bool>& interruptMsgProc)
{
    AssertLockHeld(g_msgproc_mutex);

    LogDebug(BCLog::NET, "received: %s (%u bytes) peer=%d\n", SanitizeString(msg_type), vRecv.size(), pfrom.GetId());


    if (msg_type == NetMsgType::VERSION) {
        if (pfrom.nVersion != 0) {
            LogDebug(BCLog::NET, "redundant version message from peer=%d\n", pfrom.GetId());
            return;
        }

        int64_t nTime;
        CService addrMe;
        uint64_t nNonce = 1;
        ServiceFlags nServices;
        int nVersion;
        std::string cleanSubVer;
        int starting_height = -1;
        bool fRelay = true;

        vRecv >> nVersion >> Using<CustomUintFormatter<8>>(nServices) >> nTime;
        if (nTime < 0) {
            nTime = 0;
        }
        vRecv.ignore(8); // Ignore the addrMe service bits sent by the peer
        vRecv >> CNetAddr::V1(addrMe);
        if (!pfrom.IsInboundConn() && !pfrom.IsPrivateBroadcastConn())
        {
            // Overwrites existing services (unlike gossip-relayed services which can only be added).
            m_addrman.SetServices(pfrom.addr, nServices);
        }
        if (pfrom.ExpectServicesFromConn() && !HasAllDesirableServiceFlags(nServices))
        {
            LogDebug(BCLog::NET, "peer does not offer the expected services (%08x offered, %08x expected), %s",
                     nServices,
                     GetDesirableServiceFlags(nServices),
                     pfrom.DisconnectMsg());
            pfrom.fDisconnect = true;
            return;
        }

        if (nVersion < MIN_PEER_PROTO_VERSION) {
            // disconnect from peers older than this proto version
            LogDebug(BCLog::NET, "peer using obsolete version %i, %s", nVersion, pfrom.DisconnectMsg());
            pfrom.fDisconnect = true;
            return;
        }

        if (!vRecv.empty()) {
            // Skip unused sender info: 8 bytes services + 16 bytes addr + 2 bytes port.
            vRecv.ignore(26);
            vRecv >> nNonce;
        }
        if (!vRecv.empty()) {
            std::string strSubVer;
            vRecv >> LIMITED_STRING(strSubVer, MAX_SUBVERSION_LENGTH);
            cleanSubVer = SanitizeString(strSubVer);
        }
        if (!vRecv.empty()) {
            vRecv >> starting_height;
        }
        if (!vRecv.empty())
            vRecv >> fRelay;
        // Disconnect if we connected to ourself
        if (pfrom.IsInboundConn() && !m_connman.CheckIncomingNonce(nNonce))
        {
            LogInfo("connected to self at %s, disconnecting\n", pfrom.addr.ToStringAddrPort());
            pfrom.fDisconnect = true;
            return;
        }

        if (pfrom.IsInboundConn() && addrMe.IsRoutable())
        {
            SeenLocal(addrMe);
        }

        // Inbound peers send us their version message when they connect.
        // We send our version message in response.
        if (pfrom.IsInboundConn()) {
            PushNodeVersion(pfrom, peer);
        }

        // Change version
        const int greatest_common_version = std::min(nVersion, PROTOCOL_VERSION);
        pfrom.SetCommonVersion(greatest_common_version);
        pfrom.nVersion = nVersion;

        pfrom.m_has_all_wanted_services = HasAllDesirableServiceFlags(nServices);
        peer.m_their_services = nServices;
        pfrom.SetAddrLocal(addrMe);
        {
            LOCK(pfrom.m_subver_mutex);
            pfrom.cleanSubVer = cleanSubVer;
        }

        // Init m_relay_txs only for non-block-relay, non-feeler conns with fRelay or NODE_BLOOM offered.
        if (!pfrom.IsBlockOnlyConn() &&
            !pfrom.IsFeelerConn() &&
            (fRelay || (peer.m_our_services & NODE_BLOOM))) {
            auto* const tx_relay = peer.SetTxRelay();
            {
                LOCK(tx_relay->m_bloom_filter_mutex);
                tx_relay->m_relay_txs = fRelay; // set to true after we get the first filter* message
            }
            if (fRelay) pfrom.m_relays_txs = true;
        }

        const auto mapped_as{m_connman.GetMappedAS(pfrom.addr)};
        LogDebug(BCLog::NET, "receive version message: %s: version %d, blocks=%d, us=%s, txrelay=%d, %s%s",
                  cleanSubVer.empty() ? "<no user agent>" : cleanSubVer, pfrom.nVersion,
                  starting_height, addrMe.ToStringAddrPort(), fRelay, pfrom.LogPeer(),
                  (mapped_as ? strprintf(", mapped_as=%d", mapped_as) : ""));

        if (pfrom.IsPrivateBroadcastConn()) {
            if (fRelay) {
                MakeAndPushMessage(pfrom, NetMsgType::VERACK);
            } else {
                LogDebug(BCLog::PRIVBROADCAST, "Disconnecting: does not support transaction relay (connected in vain), %s",
                         pfrom.LogPeer());
                pfrom.fDisconnect = true;
            }
            return;
        }

        if (greatest_common_version >= WTXID_RELAY_VERSION) {
            MakeAndPushMessage(pfrom, NetMsgType::WTXIDRELAY);
        }

        // Signal ADDRv2 support (BIP155).
        if (greatest_common_version >= 70016) {
            // Don't send SENDADDRV2 to peers < 70016 (courtesy; no known BIP155 software below that).
            MakeAndPushMessage(pfrom, NetMsgType::SENDADDRV2);
        }

        if (greatest_common_version >= WTXID_RELAY_VERSION && m_txreconciliation) {
            // BIP-330 txreconciliation: requires WTXID_RELAY, tx relay, non-block-relay/feeler/addr-fetch, non-blocksonly.
            const auto* tx_relay = peer.GetTxRelay();
            if (tx_relay && WITH_LOCK(tx_relay->m_bloom_filter_mutex, return tx_relay->m_relay_txs) &&
                !pfrom.IsAddrFetchConn() && !m_opts.ignore_incoming_txs) {
                const uint64_t recon_salt = m_txreconciliation->PreRegisterPeer(pfrom.GetId());
                MakeAndPushMessage(pfrom, NetMsgType::SENDTXRCNCL,
                                   TXRECONCILIATION_VERSION, recon_salt);
            }
        }

        MakeAndPushMessage(pfrom, NetMsgType::VERACK);

        // Potentially mark this peer as a preferred download peer.
        {
            LOCK(cs_main);
            CNodeState* state = State(pfrom.GetId());
            state->fPreferredDownload = (!pfrom.IsInboundConn() || pfrom.HasPermission(NetPermissionFlags::NoBan)) && !pfrom.IsAddrFetchConn() && CanServeBlocks(peer);
            m_num_preferred_download_peers += state->fPreferredDownload;
        }

        // Initialize address relay for outbound peers to decide GETADDR sending.
        bool send_getaddr{false};
        if (!pfrom.IsInboundConn()) {
            send_getaddr = SetupAddressRelay(pfrom, peer);
        }
        if (send_getaddr) {
            // One-time address fetch to populate addrman; skipped for block-relay-only peers to avoid addr leaks.
            MakeAndPushMessage(pfrom, NetMsgType::GETADDR);
            peer.m_getaddr_sent = true;
            // When requesting a getaddr, accept an additional MAX_ADDR_TO_SEND addresses in response
            // (bypassing the MAX_ADDR_PROCESSING_TOKEN_BUCKET limit).
            peer.m_addr_token_bucket += MAX_ADDR_TO_SEND;
        }

        if (!pfrom.IsInboundConn()) {
            // Update addrman for non-inbound connections to record success; skip AddrMan::Connected() for block-relay-only peers to mitigate info leaks.
            m_addrman.Good(pfrom.addr);
        }

        peer.m_time_offset = NodeSeconds{std::chrono::seconds{nTime}} - Now<NodeSeconds>();
        if (!pfrom.IsInboundConn()) {
            // Don't use timedata samples from inbound peers to make it
            // harder for others to create false warnings about our clock being out of sync.
            m_outbound_time_offsets.Add(peer.m_time_offset);
            m_outbound_time_offsets.WarnIfOutOfSync();
        }

        // If the peer is old enough to have the old alert system, send it the final alert.
        if (greatest_common_version <= 70012) {
            constexpr auto finalAlert{"60010000000000000000000000ffffff7f00000000ffffff7ffeffff7f01ffffff7f00000000ffffff7f00ffffff7f002f555247454e543a20416c657274206b657920636f6d70726f6d697365642c2075706772616465207265717569726564004630440220653febd6410f470f6bae11cad19c48413becb1ac2c17f908fd0fd53bdc3abd5202206d0e9c96fe88d4a0f01ed9dedae2b6f9e00da94cad0fecaae66ecf689bf71b50"_hex};
            MakeAndPushMessage(pfrom, "alert", finalAlert);
        }

        // Feeler connections exist only to verify if address is online.
        if (pfrom.IsFeelerConn()) {
            LogDebug(BCLog::NET, "feeler connection completed, %s", pfrom.DisconnectMsg());
            pfrom.fDisconnect = true;
        }
        return;
    }

    if (pfrom.nVersion == 0) {
        // Must have a version message before anything else
        LogDebug(BCLog::NET, "non-version message before version handshake. Message \"%s\" from peer=%d\n", SanitizeString(msg_type), pfrom.GetId());
        return;
    }

    if (msg_type == NetMsgType::VERACK) {
        if (pfrom.fSuccessfullyConnected) {
            LogDebug(BCLog::NET, "ignoring redundant verack message from peer=%d\n", pfrom.GetId());
            return;
        }

        auto new_peer_msg = [&]() {
            const auto mapped_as{m_connman.GetMappedAS(pfrom.addr)};
            return strprintf("New %s peer connected: transport: %s, version: %d, %s%s",
                pfrom.ConnectionTypeAsString(),
                TransportTypeAsString(pfrom.m_transport->GetInfo().transport_type),
                pfrom.nVersion.load(), pfrom.LogPeer(),
                (mapped_as ? strprintf(", mapped_as=%d", mapped_as) : ""));
        };

        // Log successful connections unconditionally for outbound, but not for inbound as those
        // can be triggered by an attacker at high rate.
        if (pfrom.IsInboundConn()) {
            LogDebug(BCLog::NET, "%s", new_peer_msg());
        } else {
            LogInfo("%s", new_peer_msg());
        }

        if (auto tx_relay = peer.GetTxRelay()) {
            // m_tx_inventory_to_send must be empty before handshake completion to avoid leaking arrival time.
            Assume(WITH_LOCK(
                tx_relay->m_tx_inventory_mutex,
                return tx_relay->m_tx_inventory_to_send.empty() &&
                       tx_relay->m_next_inv_send_time == 0s));
        }

        if (pfrom.IsPrivateBroadcastConn()) {
            pfrom.fSuccessfullyConnected = true;
            // We may send tx below peer's FEEFILTER threshold; relay logic tolerates peer dropping tx.
            PushPrivateBroadcastTx(pfrom);
            return;
        }

        if (pfrom.GetCommonVersion() >= SHORT_IDS_BLOCKS_VERSION) {
            // Announce willingness to provide v2 cmpctblocks (non-NODE_NETWORK peers may wish to request them).
            MakeAndPushMessage(pfrom, NetMsgType::SENDCMPCT, /*high_bandwidth=*/false, /*version=*/CMPCTBLOCKS_VERSION);
        }

        if (m_txreconciliation) {
            if (!peer.m_wtxid_relay || !m_txreconciliation->IsPeerRegistered(pfrom.GetId())) {
                // Forget reconciliation state if WTXIDRELAY wasn't sent (can't be announced later).
                m_txreconciliation->ForgetPeer(pfrom.GetId());
            }
        }

        {
            LOCK2(::cs_main, m_tx_download_mutex);
            const CNodeState* state = State(pfrom.GetId());
            m_txdownloadman.ConnectedPeer(pfrom.GetId(), node::TxDownloadConnectionInfo {
                .m_preferred = state->fPreferredDownload,
                .m_relay_permissions = pfrom.HasPermission(NetPermissionFlags::Relay),
                .m_wtxid_relay = peer.m_wtxid_relay,
            });
        }

        pfrom.fSuccessfullyConnected = true;
        return;
    }

    if (msg_type == NetMsgType::SENDHEADERS) {
        peer.m_prefers_headers = true;
        return;
    }

    if (msg_type == NetMsgType::SENDCMPCT) {
        bool sendcmpct_hb{false};
        uint64_t sendcmpct_version{0};
        vRecv >> sendcmpct_hb >> sendcmpct_version;

        // Only support compact block relay with witnesses
        if (sendcmpct_version != CMPCTBLOCKS_VERSION) return;

        LOCK(cs_main);
        CNodeState* nodestate = State(pfrom.GetId());
        nodestate->m_provides_cmpctblocks = true;
        nodestate->m_requested_hb_cmpctblocks = sendcmpct_hb;
        // save whether peer selects us as BIP152 high-bandwidth peer
        // (receiving sendcmpct(1) signals high-bandwidth, sendcmpct(0) low-bandwidth)
        pfrom.m_bip152_highbandwidth_from = sendcmpct_hb;
        return;
    }

    // BIP339 defines feature negotiation of wtxidrelay, which must happen between
    // VERSION and VERACK to avoid relay problems from switching after a connection is up.
    if (msg_type == NetMsgType::WTXIDRELAY) {
        if (pfrom.fSuccessfullyConnected) {
            // Disconnect peers that send a wtxidrelay message after VERACK.
            LogDebug(BCLog::NET, "wtxidrelay received after verack, %s", pfrom.DisconnectMsg());
            pfrom.fDisconnect = true;
            return;
        }
        if (pfrom.GetCommonVersion() >= WTXID_RELAY_VERSION) {
            if (!peer.m_wtxid_relay) {
                peer.m_wtxid_relay = true;
                m_wtxid_relay_peers++;
            } else {
                LogDebug(BCLog::NET, "ignoring duplicate wtxidrelay from peer=%d\n", pfrom.GetId());
            }
        } else {
            LogDebug(BCLog::NET, "ignoring wtxidrelay due to old common version=%d from peer=%d\n", pfrom.GetCommonVersion(), pfrom.GetId());
        }
        return;
    }

    // BIP155 defines feature negotiation of addrv2 and sendaddrv2, which must happen
    // between VERSION and VERACK.
    if (msg_type == NetMsgType::SENDADDRV2) {
        if (pfrom.fSuccessfullyConnected) {
            // Disconnect peers that send a SENDADDRV2 message after VERACK.
            LogDebug(BCLog::NET, "sendaddrv2 received after verack, %s", pfrom.DisconnectMsg());
            pfrom.fDisconnect = true;
            return;
        }
        peer.m_wants_addrv2 = true;
        return;
    }

    // SENDTXRCNCL must be negotiated between VERSION and VERACK to avoid relay issues.
    if (msg_type == NetMsgType::SENDTXRCNCL) {
        if (!m_txreconciliation) {
            LogDebug(BCLog::NET, "sendtxrcncl from peer=%d ignored, as our node does not have txreconciliation enabled\n", pfrom.GetId());
            return;
        }

        if (pfrom.fSuccessfullyConnected) {
            LogDebug(BCLog::NET, "sendtxrcncl received after verack, %s", pfrom.DisconnectMsg());
            pfrom.fDisconnect = true;
            return;
        }

        // Peer must not offer us reconciliations if we specified no tx relay support in VERSION.
        if (RejectIncomingTxs(pfrom)) {
            LogDebug(BCLog::NET, "sendtxrcncl received to which we indicated no tx relay, %s", pfrom.DisconnectMsg());
            pfrom.fDisconnect = true;
            return;
        }

        // After RejectIncomingTxs above, m_relay_txs fully represents peer's tx relay support.
        const auto* tx_relay = peer.GetTxRelay();
        if (!tx_relay || !WITH_LOCK(tx_relay->m_bloom_filter_mutex, return tx_relay->m_relay_txs)) {
            LogDebug(BCLog::NET, "sendtxrcncl received which indicated no tx relay to us, %s", pfrom.DisconnectMsg());
            pfrom.fDisconnect = true;
            return;
        }

        uint32_t peer_txreconcl_version;
        uint64_t remote_salt;
        vRecv >> peer_txreconcl_version >> remote_salt;

        const ReconciliationRegisterResult result = m_txreconciliation->RegisterPeer(pfrom.GetId(), pfrom.IsInboundConn(),
                                                                                     peer_txreconcl_version, remote_salt);
        switch (result) {
        case ReconciliationRegisterResult::NOT_FOUND:
            LogDebug(BCLog::NET, "Ignore unexpected txreconciliation signal from peer=%d\n", pfrom.GetId());
            break;
        case ReconciliationRegisterResult::SUCCESS:
            break;
        case ReconciliationRegisterResult::ALREADY_REGISTERED:
            LogDebug(BCLog::NET, "txreconciliation protocol violation (sendtxrcncl received from already registered peer), %s", pfrom.DisconnectMsg());
            pfrom.fDisconnect = true;
            return;
        case ReconciliationRegisterResult::PROTOCOL_VIOLATION:
            LogDebug(BCLog::NET, "txreconciliation protocol violation, %s", pfrom.DisconnectMsg());
            pfrom.fDisconnect = true;
            return;
        }
        return;
    }

    if (!pfrom.fSuccessfullyConnected) {
        LogDebug(BCLog::NET, "Unsupported message \"%s\" prior to verack from peer=%d\n", SanitizeString(msg_type), pfrom.GetId());
        return;
    }

    if (pfrom.IsPrivateBroadcastConn()) {
        if (msg_type != NetMsgType::PONG && msg_type != NetMsgType::GETDATA) {
            LogDebug(BCLog::PRIVBROADCAST, "Ignoring incoming message '%s', %s", msg_type, pfrom.LogPeer());
            return;
        }
    }

    if (msg_type == NetMsgType::ADDR || msg_type == NetMsgType::ADDRV2) {
        const auto ser_params{
            msg_type == NetMsgType::ADDRV2 ?
            // Set V2 param so that the CNetAddr and CAddress
            // unserialize methods know that an address in v2 format is coming.
            CAddress::V2_NETWORK :
            CAddress::V1_NETWORK,
        };

        std::vector<CAddress> vAddr;
        vRecv >> ser_params(vAddr);
        ProcessAddrs(msg_type, pfrom, peer, std::move(vAddr), interruptMsgProc);
        return;
    }

    if (msg_type == NetMsgType::INV) {
        std::vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            Misbehaving(peer, strprintf("inv message size = %u", vInv.size()));
            return;
        }

        const bool reject_tx_invs{RejectIncomingTxs(pfrom)};

        LOCK2(cs_main, m_tx_download_mutex);

        const auto current_time{GetTime<std::chrono::microseconds>()};
        uint256* best_block{nullptr};

        for (CInv& inv : vInv) {
            if (interruptMsgProc) return;

            // Ignore INVs not matching wtxidrelay setting; orphan parent fetching uses no INV.
            if (peer.m_wtxid_relay) {
                if (inv.IsMsgTx()) continue;
            } else {
                if (inv.IsMsgWtx()) continue;
            }

            if (inv.IsMsgBlk()) {
                const bool fAlreadyHave = AlreadyHaveBlock(inv.hash);
                LogDebug(BCLog::NET, "got inv: %s %s peer=%d", inv.ToString(), fAlreadyHave ? "have" : "new", pfrom.GetId());

                UpdateBlockAvailability(pfrom.GetId(), inv.hash);
                if (!fAlreadyHave && !m_chainman.m_blockman.LoadingBlocks() && !IsBlockRequested(inv.hash)) {
                    // Fallback to inv may indicate re-org or incomplete headers sync; fetch via getheaders.
                    best_block = &inv.hash;
                }
            } else if (inv.IsGenTxMsg()) {
                if (reject_tx_invs) {
                    LogDebug(BCLog::NET, "transaction (%s) inv sent in violation of protocol, %s", inv.hash.ToString(), pfrom.DisconnectMsg());
                    pfrom.fDisconnect = true;
                    return;
                }
                const GenTxid gtxid = ToGenTxid(inv);
                AddKnownTx(peer, inv.hash);

                if (!m_chainman.IsInitialBlockDownload()) {
                    const bool fAlreadyHave{m_txdownloadman.AddTxAnnouncement(pfrom.GetId(), gtxid, current_time)};
                    LogDebug(BCLog::NET, "got inv: %s %s peer=%d", inv.ToString(), fAlreadyHave ? "have" : "new", pfrom.GetId());
                }
            } else {
                LogDebug(BCLog::NET, "Unknown inv type \"%s\" received from peer=%d\n", inv.ToString(), pfrom.GetId());
            }
        }

        if (best_block != nullptr) {
            // Consider getheaders if headers-sync not started; limit new sync peers per block to balance reliability vs bandwidth.
            CNodeState& state{*Assert(State(pfrom.GetId()))};
            if (state.fSyncStarted || (!peer.m_inv_triggered_getheaders_before_sync && *best_block != m_last_block_inv_triggering_headers_sync)) {
                if (MaybeSendGetHeaders(pfrom, GetLocator(m_chainman.m_best_header), peer)) {
                    LogDebug(BCLog::NET, "getheaders (%d) %s to peer=%d\n",
                            m_chainman.m_best_header->nHeight, best_block->ToString(),
                            pfrom.GetId());
                }
                if (!state.fSyncStarted) {
                    peer.m_inv_triggered_getheaders_before_sync = true;
                    // Track last block triggering headers sync to limit new sync peers per block.
                    m_last_block_inv_triggering_headers_sync = *best_block;
                }
            }
        }

        return;
    }

    if (msg_type == NetMsgType::GETDATA) {
        std::vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            Misbehaving(peer, strprintf("getdata message size = %u", vInv.size()));
            return;
        }

        LogDebug(BCLog::NET, "received getdata (%u invsz) peer=%d\n", vInv.size(), pfrom.GetId());

        if (vInv.size() > 0) {
            LogDebug(BCLog::NET, "received getdata for: %s peer=%d\n", vInv[0].ToString(), pfrom.GetId());
        }

        if (pfrom.IsPrivateBroadcastConn()) {
            const auto pushed_tx_opt{m_tx_for_private_broadcast.GetTxForNode(pfrom.GetId())};
            if (!pushed_tx_opt) {
                LogDebug(BCLog::PRIVBROADCAST, "Disconnecting: got GETDATA without sending an INV, %s",
                         pfrom.LogPeer());
                pfrom.fDisconnect = true;
                return;
            }

            const CTransactionRef& pushed_tx{*pushed_tx_opt};

            // The GETDATA request must contain exactly one inv and it must be for the transaction
            // that we INVed to the peer earlier.
            if (vInv.size() == 1 && vInv[0].IsMsgTx() && vInv[0].hash == pushed_tx->GetHash().ToUint256()) {

                MakeAndPushMessage(pfrom, NetMsgType::TX, TX_WITH_WITNESS(*pushed_tx));

                peer.m_ping_queued = true; // Ensure a ping will be sent: mimic a request via RPC.
                MaybeSendPing(pfrom, peer, NodeClock::now());
            } else {
                LogDebug(BCLog::PRIVBROADCAST, "Disconnecting: got an unexpected GETDATA message, %s",
                         pfrom.LogPeer());
                pfrom.fDisconnect = true;
            }
            return;
        }

        {
            LOCK(peer.m_getdata_requests_mutex);
            peer.m_getdata_requests.insert(peer.m_getdata_requests.end(), vInv.begin(), vInv.end());
            ProcessGetData(pfrom, peer, interruptMsgProc);
        }

        return;
    }

    if (msg_type == NetMsgType::GETBLOCKS) {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        if (locator.vHave.size() > MAX_LOCATOR_SZ) {
            LogDebug(BCLog::NET, "getblocks locator size %lld > %d, %s", locator.vHave.size(), MAX_LOCATOR_SZ, pfrom.DisconnectMsg());
            pfrom.fDisconnect = true;
            return;
        }

        // Verify best chain before responding to getblocks to handle compact-block-announced tip.
        {
            std::shared_ptr<const CBlock> a_recent_block;
            {
                LOCK(m_most_recent_block_mutex);
                a_recent_block = m_most_recent_block;
            }
            BlockValidationState state;
            if (!m_chainman.ActiveChainstate().ActivateBestChain(state, a_recent_block)) {
                LogDebug(BCLog::NET, "failed to activate chain (%s)\n", state.ToString());
            }
        }

        LOCK(cs_main);

        // Find the last block the caller has in the main chain
        const CBlockIndex* pindex = m_chainman.ActiveChainstate().FindForkInGlobalIndex(locator);

        // Send the rest of the chain
        if (pindex)
            pindex = m_chainman.ActiveChain().Next(*pindex);
        int nLimit = 500;
        LogDebug(BCLog::NET, "getblocks %d to %s limit %d from peer=%d\n", (pindex ? pindex->nHeight : -1), hashStop.IsNull() ? "end" : hashStop.ToString(), nLimit, pfrom.GetId());
        for (; pindex; pindex = m_chainman.ActiveChain().Next(*pindex))
        {
            if (pindex->GetBlockHash() == hashStop)
            {
                LogDebug(BCLog::NET, " getblocks stopping at %d %s", pindex->nHeight, pindex->GetBlockHash().ToString());
                break;
            }
            // If pruning, don't inv blocks unless we have on disk and are likely to still have
            // for some reasonable time window (1 hour) that block relay might require.
            const int nPrunedBlocksLikelyToHave = MIN_BLOCKS_TO_KEEP - 3600 / m_chainparams.GetConsensus().nPowTargetSpacing;
            if (m_chainman.m_blockman.IsPruneMode() && (!(pindex->nStatus & BLOCK_HAVE_DATA) || pindex->nHeight <= m_chainman.ActiveChain().Tip()->nHeight - nPrunedBlocksLikelyToHave)) {
                LogDebug(BCLog::NET, " getblocks stopping, pruned or too old block at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                break;
            }
            WITH_LOCK(peer.m_block_inv_mutex, peer.m_blocks_for_inv_relay.push_back(pindex->GetBlockHash()));
            if (--nLimit <= 0) {
                // When this block is requested, we'll send an inv that'll
                // trigger the peer to getblocks the next batch of inventory.
                LogDebug(BCLog::NET, " getblocks stopping at limit %d %s", pindex->nHeight, pindex->GetBlockHash().ToString());
                WITH_LOCK(peer.m_block_inv_mutex, {peer.m_continuation_block = pindex->GetBlockHash();});
                break;
            }
        }
        return;
    }

    if (msg_type == NetMsgType::GETBLOCKTXN) {
        BlockTransactionsRequest req;
        vRecv >> req;
        // Verify differential encoding invariant: indexes must be strictly increasing
        // DifferenceFormatter should guarantee this property during deserialization
        for (size_t i = 1; i < req.indexes.size(); ++i) {
            Assume(req.indexes[i] > req.indexes[i-1]);
        }

        std::shared_ptr<const CBlock> recent_block;
        {
            LOCK(m_most_recent_block_mutex);
            if (m_most_recent_block_hash == req.blockhash)
                recent_block = m_most_recent_block;
            // Unlock m_most_recent_block_mutex to avoid cs_main lock inversion
        }
        if (recent_block) {
            SendBlockTransactions(pfrom, peer, *recent_block, req);
            return;
        }

        FlatFilePos block_pos{};
        {
            LOCK(cs_main);

            const CBlockIndex* pindex = m_chainman.m_blockman.LookupBlockIndex(req.blockhash);
            if (!pindex || !(pindex->nStatus & BLOCK_HAVE_DATA)) {
                LogDebug(BCLog::NET, "Peer %d sent us a getblocktxn for a block we don't have\n", pfrom.GetId());
                return;
            }

            if (pindex->nHeight >= m_chainman.ActiveChain().Height() - MAX_BLOCKTXN_DEPTH) {
                block_pos = pindex->GetBlockPos();
            }
        }

        if (!block_pos.IsNull()) {
            CBlock block;
            const bool ret{m_chainman.m_blockman.ReadBlock(block, block_pos, req.blockhash)};
            // If height is above MAX_BLOCKTXN_DEPTH then this block cannot get
            // pruned after we release cs_main above, so this read should never fail.
            assert(ret);

            SendBlockTransactions(pfrom, peer, block, req);
            return;
        }

        // For older blocks, send full block response to deter getblocktxn spam causing disk reads.
        LogDebug(BCLog::NET, "Peer %d sent us a getblocktxn for a block > %i deep\n", pfrom.GetId(), MAX_BLOCKTXN_DEPTH);
        CInv inv{MSG_WITNESS_BLOCK, req.blockhash};
        WITH_LOCK(peer.m_getdata_requests_mutex, peer.m_getdata_requests.push_back(inv));
        // The message processing loop will go around again (without pausing) and we'll respond then
        return;
    }

    if (msg_type == NetMsgType::GETHEADERS) {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        if (locator.vHave.size() > MAX_LOCATOR_SZ) {
            LogDebug(BCLog::NET, "getheaders locator size %lld > %d, %s", locator.vHave.size(), MAX_LOCATOR_SZ, pfrom.DisconnectMsg());
            pfrom.fDisconnect = true;
            return;
        }

        if (m_chainman.m_blockman.LoadingBlocks()) {
            LogDebug(BCLog::NET, "Ignoring getheaders from peer=%d while importing/reindexing\n", pfrom.GetId());
            return;
        }

        LOCK(cs_main);

        // Don't serve headers below minimum chain work to prevent aborted low-work sync.
        if (m_chainman.ActiveTip() == nullptr ||
                (m_chainman.ActiveTip()->nChainWork < m_chainman.MinimumChainWork() && !pfrom.HasPermission(NetPermissionFlags::Download))) {
            LogDebug(BCLog::NET, "Ignoring getheaders from peer=%d because active chain has too little work; sending empty response\n", pfrom.GetId());
            // Just respond with an empty headers message, to tell the peer to
            // go away but not treat us as unresponsive.
            MakeAndPushMessage(pfrom, NetMsgType::HEADERS, std::vector<CBlockHeader>());
            return;
        }

        CNodeState *nodestate = State(pfrom.GetId());
        const CBlockIndex* pindex = nullptr;
        if (locator.IsNull())
        {
            // If locator is null, return the hashStop block
            pindex = m_chainman.m_blockman.LookupBlockIndex(hashStop);
            if (!pindex) {
                return;
            }
            if (!BlockRequestAllowed(*pindex)) {
                LogDebug(BCLog::NET, "%s: ignoring request from peer=%i for old block header that isn't in the main chain\n", __func__, pfrom.GetId());
                return;
            }
        }
        else
        {
            // Find the last block the caller has in the main chain
            pindex = m_chainman.ActiveChainstate().FindForkInGlobalIndex(locator);
            if (pindex)
                pindex = m_chainman.ActiveChain().Next(*pindex);
        }

        // we must use CBlocks, as CBlockHeaders won't include the 0x00 nTx count at the end
        std::vector<CBlock> vHeaders;
        int nLimit = m_opts.max_headers_result;
        LogDebug(BCLog::NET, "getheaders %d to %s from peer=%d\n", (pindex ? pindex->nHeight : -1), hashStop.IsNull() ? "end" : hashStop.ToString(), pfrom.GetId());
        for (; pindex; pindex = m_chainman.ActiveChain().Next(*pindex))
        {
            vHeaders.emplace_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
        }
        // Safe to update pindexBestHeaderSent to tip when pindex is nullptr (empty headers message).
        // Reset (not max) BestHeaderSent: compact-block-announced tip may have caused headers request without new block; reset ensures re-announcement.
        nodestate->pindexBestHeaderSent = pindex ? pindex : m_chainman.ActiveChain().Tip();
        MakeAndPushMessage(pfrom, NetMsgType::HEADERS, TX_WITH_WITNESS(vHeaders));
        return;
    }

    if (msg_type == NetMsgType::TX) {
        if (RejectIncomingTxs(pfrom)) {
            LogDebug(BCLog::NET, "transaction sent in violation of protocol, %s", pfrom.DisconnectMsg());
            pfrom.fDisconnect = true;
            return;
        }

        // Skip tx processing in IBD; unsolicited tx is not a protocol violation.
        if (m_chainman.IsInitialBlockDownload()) return;

        CTransactionRef ptx;
        vRecv >> TX_WITH_WITNESS(ptx);

        const Txid& txid = ptx->GetHash();
        const Wtxid& wtxid = ptx->GetWitnessHash();

        const uint256& hash = peer.m_wtxid_relay ? wtxid.ToUint256() : txid.ToUint256();
        AddKnownTx(peer, hash);

        if (const auto num_broadcasted{m_tx_for_private_broadcast.Remove(ptx)}) {
            LogDebug(BCLog::PRIVBROADCAST, "Received our privately broadcast transaction (txid=%s) from the "
                                           "network from %s; stopping private broadcast attempts",
                     txid.ToString(), pfrom.LogPeer());
            if (NUM_PRIVATE_BROADCAST_PER_TX > num_broadcasted.value()) {
                // Not all of the initial NUM_PRIVATE_BROADCAST_PER_TX connections were needed.
                // Tell CConnman it does not need to start the remaining ones.
                m_connman.m_private_broadcast.NumToOpenSub(NUM_PRIVATE_BROADCAST_PER_TX - num_broadcasted.value());
            }
        }

        LOCK2(cs_main, m_tx_download_mutex);

        const auto& [should_validate, package_to_validate] = m_txdownloadman.ReceivedTx(pfrom.GetId(), ptx);
        if (!should_validate) {
            if (pfrom.HasPermission(NetPermissionFlags::ForceRelay)) {
                // Always relay forcerelay peers' tx (even if in mempool) to act as gateway.
                if (!m_mempool.exists(txid)) {
                    LogInfo("Not relaying non-mempool transaction %s (wtxid=%s) from forcerelay peer=%d\n",
                              txid.ToString(), wtxid.ToString(), pfrom.GetId());
                } else {
                    LogInfo("Force relaying tx %s (wtxid=%s) from peer=%d\n",
                              txid.ToString(), wtxid.ToString(), pfrom.GetId());
                    InitiateTxBroadcastToAll(txid, wtxid);
                }
            }

            if (package_to_validate) {
                const auto package_result{ProcessNewPackage(m_chainman.ActiveChainstate(), m_mempool, package_to_validate->m_txns, /*test_accept=*/false, /*client_maxfeerate=*/std::nullopt)};
                LogDebug(BCLog::TXPACKAGES, "package evaluation for %s: %s\n", package_to_validate->ToString(),
                         package_result.m_state.IsValid() ? "package accepted" : "package rejected");
                ProcessPackageResult(package_to_validate.value(), package_result);
            }
            return;
        }

        // ReceivedTx should not be telling us to validate the tx and a package.
        Assume(!package_to_validate.has_value());

        const MempoolAcceptResult result = m_chainman.ProcessTransaction(ptx);
        const TxValidationState& state = result.m_state;

        if (result.m_result_type == MempoolAcceptResult::ResultType::VALID) {
            ProcessValidTx(pfrom.GetId(), ptx, result.m_replaced_transactions);
            pfrom.m_last_tx_time = GetTime<std::chrono::seconds>();
        }
        if (state.IsInvalid()) {
            if (auto package_to_validate{ProcessInvalidTx(pfrom.GetId(), ptx, state, /*first_time_failure=*/true)}) {
                const auto package_result{ProcessNewPackage(m_chainman.ActiveChainstate(), m_mempool, package_to_validate->m_txns, /*test_accept=*/false, /*client_maxfeerate=*/std::nullopt)};
                LogDebug(BCLog::TXPACKAGES, "package evaluation for %s: %s\n", package_to_validate->ToString(),
                         package_result.m_state.IsValid() ? "package accepted" : "package rejected");
                ProcessPackageResult(package_to_validate.value(), package_result);
            }
        }

        return;
    }

    if (msg_type == NetMsgType::CMPCTBLOCK)
    {
        // Ignore cmpctblock received while importing
        if (m_chainman.m_blockman.LoadingBlocks()) {
            LogDebug(BCLog::NET, "Unexpected cmpctblock message received from peer %d\n", pfrom.GetId());
            return;
        }

        CBlockHeaderAndShortTxIDs cmpctblock;
        vRecv >> cmpctblock;

        bool received_new_header = false;
        const auto blockhash = cmpctblock.header.GetHash();

        {
        LOCK(cs_main);

        const CBlockIndex* prev_block = m_chainman.m_blockman.LookupBlockIndex(cmpctblock.header.hashPrevBlock);
        if (!prev_block) {
            // Doesn't connect (or is genesis), instead of DoSing in AcceptBlockHeader, request deeper headers
            if (!m_chainman.IsInitialBlockDownload()) {
                MaybeSendGetHeaders(pfrom, GetLocator(m_chainman.m_best_header), peer);
            }
            return;
        } else if (prev_block->nChainWork + GetBlockProof(cmpctblock.header) < GetAntiDoSWorkThreshold()) {
            // If we get a low-work header in a compact block, we can ignore it.
            LogDebug(BCLog::NET, "Ignoring low-work compact block from peer %d\n", pfrom.GetId());
            return;
        }

        if (!m_chainman.m_blockman.LookupBlockIndex(blockhash)) {
            received_new_header = true;
        }
        }

        const CBlockIndex *pindex = nullptr;
        BlockValidationState state;
        if (!m_chainman.ProcessNewBlockHeaders({{cmpctblock.header}}, /*min_pow_checked=*/true, state, &pindex)) {
            if (state.IsInvalid()) {
                MaybePunishNodeForBlock(pfrom.GetId(), state, /*via_compact_block=*/true, "invalid header via cmpctblock");
                return;
            }
        }

        // If AcceptBlockHeader returned true, it set pindex
        Assert(pindex);
        if (received_new_header) {
            LogBlockHeader(*pindex, pfrom, /*via_compact_block=*/true);
        }

        bool fProcessBLOCKTXN = false;

        // If we end up treating this as a plain headers message, call that as well
        // without cs_main.
        bool fRevertToHeaderProcessing = false;

        // Keep a CBlock for "optimistic" compactblock reconstructions (see
        // below)
        std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
        bool fBlockReconstructed = false;

        {
        LOCK(cs_main);
        UpdateBlockAvailability(pfrom.GetId(), pindex->GetBlockHash());

        CNodeState *nodestate = State(pfrom.GetId());

        // If this was a new header with more work than our tip, update the
        // peer's last block announcement time
        if (received_new_header && pindex->nChainWork > m_chainman.ActiveChain().Tip()->nChainWork) {
            nodestate->m_last_block_announcement = GetTime();
        }

        if (pindex->nStatus & BLOCK_HAVE_DATA) // Nothing to do here
            return;

        auto range_flight = mapBlocksInFlight.equal_range(pindex->GetBlockHash());
        size_t already_in_flight = std::distance(range_flight.first, range_flight.second);
        bool requested_block_from_this_peer{false};

        // Multimap ensures ordering of outstanding requests. It's either empty or first in line.
        bool first_in_flight = already_in_flight == 0 || (range_flight.first->second.first == pfrom.GetId());

        while (range_flight.first != range_flight.second) {
            if (range_flight.first->second.first == pfrom.GetId()) {
                requested_block_from_this_peer = true;
                break;
            }
            range_flight.first++;
        }

        if (pindex->nChainWork <= m_chainman.ActiveChain().Tip()->nChainWork || // We know something better
                pindex->nTx != 0) { // We had this block at some point, but pruned it
            if (requested_block_from_this_peer) {
                // We requested this block for some reason, but our mempool will probably be useless
                // so we just grab the block via normal getdata
                std::vector<CInv> vInv(1);
                vInv[0] = CInv(MSG_BLOCK | GetFetchFlags(peer), blockhash);
                MakeAndPushMessage(pfrom, NetMsgType::GETDATA, vInv);
            }
            return;
        }

        // If we're not close to tip yet, give up and let parallel block fetch work its magic
        if (!already_in_flight && !CanDirectFetch()) {
            return;
        }

        // We want to be a bit conservative just to be extra careful about DoS
        // possibilities in compact block processing...
        if (pindex->nHeight <= m_chainman.ActiveChain().Height() + 2) {
            if ((already_in_flight < MAX_CMPCTBLOCKS_INFLIGHT_PER_BLOCK && nodestate->vBlocksInFlight.size() < MAX_BLOCKS_IN_TRANSIT_PER_PEER) ||
                 requested_block_from_this_peer) {
                std::list<QueuedBlock>::iterator* queuedBlockIt = nullptr;
                if (!BlockRequested(pfrom.GetId(), *pindex, &queuedBlockIt)) {
                    if (!(*queuedBlockIt)->partialBlock)
                        (*queuedBlockIt)->partialBlock.reset(new PartiallyDownloadedBlock(&m_mempool));
                    else {
                        // The block was already in flight using compact blocks from the same peer
                        LogDebug(BCLog::NET, "Peer sent us compact block we were already syncing!\n");
                        return;
                    }
                }

                PartiallyDownloadedBlock& partialBlock = *(*queuedBlockIt)->partialBlock;
                ReadStatus status = partialBlock.InitData(cmpctblock, vExtraTxnForCompact);
                if (status == READ_STATUS_INVALID) {
                    RemoveBlockRequest(pindex->GetBlockHash(), pfrom.GetId()); // Reset in-flight state in case Misbehaving does not result in a disconnect
                    Misbehaving(peer, "invalid compact block");
                    return;
                } else if (status == READ_STATUS_FAILED) {
                    if (first_in_flight)  {
                        // Duplicate txindexes, the block is now in-flight, so just request it
                        std::vector<CInv> vInv(1);
                        vInv[0] = CInv(MSG_BLOCK | GetFetchFlags(peer), blockhash);
                        MakeAndPushMessage(pfrom, NetMsgType::GETDATA, vInv);
                    } else {
                        // Give up for this peer and wait for other peer(s)
                        RemoveBlockRequest(pindex->GetBlockHash(), pfrom.GetId());
                    }
                    return;
                }

                BlockTransactionsRequest req;
                for (size_t i = 0; i < cmpctblock.BlockTxCount(); i++) {
                    if (!partialBlock.IsTxAvailable(i))
                        req.indexes.push_back(i);
                }
                if (req.indexes.empty()) {
                    fProcessBLOCKTXN = true;
                } else if (first_in_flight) {
                    // We will try to round-trip any compact blocks we get on failure,
                    // as long as it's first...
                    req.blockhash = pindex->GetBlockHash();
                    MakeAndPushMessage(pfrom, NetMsgType::GETBLOCKTXN, req);
                } else if (pfrom.m_bip152_highbandwidth_to &&
                    (!pfrom.IsInboundConn() ||
                    IsBlockRequestedFromOutbound(blockhash) ||
                    already_in_flight < MAX_CMPCTBLOCKS_INFLIGHT_PER_BLOCK - 1)) {
                    // hb relay peer when: outbound, or outbound attempt in flight, or non-final parallel slot.
                    req.blockhash = pindex->GetBlockHash();
                    MakeAndPushMessage(pfrom, NetMsgType::GETBLOCKTXN, req);
                } else {
                    // Give up for this peer and wait for other peer(s)
                    RemoveBlockRequest(pindex->GetBlockHash(), pfrom.GetId());
                }
            } else {
                // Block in flight from another peer or too many outstanding; try optimistic reconstruction.
                PartiallyDownloadedBlock tempBlock(&m_mempool);
                ReadStatus status = tempBlock.InitData(cmpctblock, vExtraTxnForCompact);
                if (status != READ_STATUS_OK) {
                    // TODO: don't ignore failures
                    return;
                }
                std::vector<CTransactionRef> dummy;
                const CBlockIndex* prev_block{Assume(m_chainman.m_blockman.LookupBlockIndex(cmpctblock.header.hashPrevBlock))};
                status = tempBlock.FillBlock(*pblock, dummy,
                                             /*segwit_active=*/DeploymentActiveAfter(prev_block, m_chainman, Consensus::DEPLOYMENT_SEGWIT));
                if (status == READ_STATUS_OK) {
                    fBlockReconstructed = true;
                }
            }
        } else {
            if (requested_block_from_this_peer) {
                // We requested this block, but its far into the future, so our
                // mempool will probably be useless - request the block normally
                std::vector<CInv> vInv(1);
                vInv[0] = CInv(MSG_BLOCK | GetFetchFlags(peer), blockhash);
                MakeAndPushMessage(pfrom, NetMsgType::GETDATA, vInv);
                return;
            } else {
                // If this was an announce-cmpctblock, we want the same treatment as a header message
                fRevertToHeaderProcessing = true;
            }
        }
        } // cs_main

        if (fProcessBLOCKTXN) {
            BlockTransactions txn;
            txn.blockhash = blockhash;
            return ProcessCompactBlockTxns(pfrom, peer, txn);
        }

        if (fRevertToHeaderProcessing) {
            // BIP 152: HB compact block peers' headers may be relayed before full validation; invalid-chain builds are detected later.
            return ProcessHeadersMessage(pfrom, peer, {cmpctblock.header}, /*via_compact_block=*/true);
        }

        if (fBlockReconstructed) {
            // If we got here, we were able to optimistically reconstruct a
            // block that is in flight from some other peer.
            {
                LOCK(cs_main);
                mapBlockSource.emplace(pblock->GetHash(), std::make_pair(pfrom.GetId(), false));
            }
            // force_processing bypasses anti-DoS unrequested-block filters; safe here due to CanDirectFetch + min-work checks.
            ProcessBlock(pfrom, pblock, /*force_processing=*/true, /*min_pow_checked=*/true);
            LOCK(cs_main); // hold cs_main for CBlockIndex::IsValid()
            if (pindex->IsValid(BLOCK_VALID_TRANSACTIONS)) {
                // Clear download state after ProcessNewBlock to prevent malleated cmpctblock interference.
                RemoveBlockRequest(pblock->GetHash(), std::nullopt);
            }
        }
        return;
    }

    if (msg_type == NetMsgType::BLOCKTXN)
    {
        // Ignore blocktxn received while importing
        if (m_chainman.m_blockman.LoadingBlocks()) {
            LogDebug(BCLog::NET, "Unexpected blocktxn message received from peer %d\n", pfrom.GetId());
            return;
        }

        BlockTransactions resp;
        vRecv >> resp;

        return ProcessCompactBlockTxns(pfrom, peer, resp);
    }

    if (msg_type == NetMsgType::HEADERS)
    {
        // Ignore headers received while importing
        if (m_chainman.m_blockman.LoadingBlocks()) {
            LogDebug(BCLog::NET, "Unexpected headers message received from peer %d\n", pfrom.GetId());
            return;
        }

        std::vector<CBlockHeader> headers;

        // Bypass the normal CBlock deserialization, as we don't want to risk deserializing 2000 full blocks.
        unsigned int nCount = ReadCompactSize(vRecv);
        if (nCount > m_opts.max_headers_result) {
            Misbehaving(peer, strprintf("headers message size = %u", nCount));
            return;
        }
        headers.resize(nCount);
        for (unsigned int n = 0; n < nCount; n++) {
            vRecv >> headers[n];
            ReadCompactSize(vRecv); // ignore tx count; assume it is 0.
        }

        ProcessHeadersMessage(pfrom, peer, std::move(headers), /*via_compact_block=*/false);

        // Check if the headers presync progress needs to be reported to validation.
        // This needs to be done without holding the m_headers_presync_mutex lock.
        if (m_headers_presync_should_signal.exchange(false)) {
            HeadersPresyncStats stats;
            {
                LOCK(m_headers_presync_mutex);
                auto it = m_headers_presync_stats.find(m_headers_presync_bestpeer);
                if (it != m_headers_presync_stats.end()) stats = it->second;
            }
            if (stats.second) {
                m_chainman.ReportHeadersPresync(stats.second->first, stats.second->second);
            }
        }

        return;
    }

    if (msg_type == NetMsgType::BLOCK)
    {
        // Ignore block received while importing
        if (m_chainman.m_blockman.LoadingBlocks()) {
            LogDebug(BCLog::NET, "Unexpected block message received from peer %d\n", pfrom.GetId());
            return;
        }

        std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
        vRecv >> TX_WITH_WITNESS(*pblock);

        LogDebug(BCLog::NET, "received block %s peer=%d\n", pblock->GetHash().ToString(), pfrom.GetId());

        const CBlockIndex* prev_block{WITH_LOCK(m_chainman.GetMutex(), return m_chainman.m_blockman.LookupBlockIndex(pblock->hashPrevBlock))};

        // Check for possible mutation if it connects to something we know so we can check for DEPLOYMENT_SEGWIT being active
        if (prev_block && IsBlockMutated(/*block=*/*pblock,
                           /*check_witness_root=*/DeploymentActiveAfter(prev_block, m_chainman, Consensus::DEPLOYMENT_SEGWIT))) {
            LogDebug(BCLog::NET, "Received mutated block from peer=%d\n", peer.m_id);
            Misbehaving(peer, "mutated block");
            WITH_LOCK(cs_main, RemoveBlockRequest(pblock->GetHash(), peer.m_id));
            return;
        }

        bool forceProcessing = false;
        const uint256 hash(pblock->GetHash());
        bool min_pow_checked = false;
        {
            LOCK(cs_main);
            // Always process the block if we requested it, since we may
            // need it even when it's not a candidate for a new best tip.
            forceProcessing = IsBlockRequested(hash);
            RemoveBlockRequest(hash, pfrom.GetId());
            // mapBlockSource race with ProcessNewBlock is fine (used only for peer punishment and compact-block tracking).
            mapBlockSource.emplace(hash, std::make_pair(pfrom.GetId(), true));

            // Check claimed work on this block against our anti-dos thresholds.
            if (prev_block && prev_block->nChainWork + GetBlockProof(*pblock) >= GetAntiDoSWorkThreshold()) {
                min_pow_checked = true;
            }
        }
        ProcessBlock(pfrom, pblock, forceProcessing, min_pow_checked);
        return;
    }

    if (msg_type == NetMsgType::GETADDR) {
        // Asymmetric behavior: NAT-bound outbound nodes ignore getaddr to mitigate fingerprinting.
        if (!pfrom.IsInboundConn()) {
            LogDebug(BCLog::NET, "Ignoring \"getaddr\" from %s connection. peer=%d\n", pfrom.ConnectionTypeAsString(), pfrom.GetId());
            return;
        }

        // Since this must be an inbound connection, SetupAddressRelay will
        // never fail.
        Assume(SetupAddressRelay(pfrom, peer));

        // Only send one GetAddr response per connection to reduce resource waste
        // and discourage addr stamping of INV announcements.
        if (peer.m_getaddr_recvd) {
            LogDebug(BCLog::NET, "Ignoring repeated \"getaddr\". peer=%d\n", pfrom.GetId());
            return;
        }
        peer.m_getaddr_recvd = true;

        peer.m_addrs_to_send.clear();
        std::vector<CAddress> vAddr;
        if (pfrom.HasPermission(NetPermissionFlags::Addr)) {
            vAddr = m_connman.GetAddressesUnsafe(MAX_ADDR_TO_SEND, MAX_PCT_ADDR_TO_SEND, /*network=*/std::nullopt);
        } else {
            vAddr = m_connman.GetAddresses(pfrom, MAX_ADDR_TO_SEND, MAX_PCT_ADDR_TO_SEND);
        }
        for (const CAddress &addr : vAddr) {
            PushAddress(peer, addr);
        }
        return;
    }

    if (msg_type == NetMsgType::MEMPOOL) {
        // Only process received mempool messages if we advertise NODE_BLOOM
        // or if the peer has mempool permissions.
        if (!(peer.m_our_services & NODE_BLOOM) && !pfrom.HasPermission(NetPermissionFlags::Mempool))
        {
            if (!pfrom.HasPermission(NetPermissionFlags::NoBan))
            {
                LogDebug(BCLog::NET, "mempool request with bloom filters disabled, %s", pfrom.DisconnectMsg());
                pfrom.fDisconnect = true;
            }
            return;
        }

        if (m_connman.OutboundTargetReached(false) && !pfrom.HasPermission(NetPermissionFlags::Mempool))
        {
            if (!pfrom.HasPermission(NetPermissionFlags::NoBan))
            {
                LogDebug(BCLog::NET, "mempool request with bandwidth limit reached, %s", pfrom.DisconnectMsg());
                pfrom.fDisconnect = true;
            }
            return;
        }

        if (auto tx_relay = peer.GetTxRelay(); tx_relay != nullptr) {
            LOCK(tx_relay->m_tx_inventory_mutex);
            tx_relay->m_send_mempool = true;
        }
        return;
    }

    if (msg_type == NetMsgType::PING) {
        if (pfrom.GetCommonVersion() > BIP0031_VERSION) {
            uint64_t nonce = 0;
            vRecv >> nonce;
            // Echo nonce back: enables connection liveness check and latency measurement; nonce prevents ping confusion.
            MakeAndPushMessage(pfrom, NetMsgType::PONG, nonce);
        }
        return;
    }

    if (msg_type == NetMsgType::PONG) {
        ProcessPong(pfrom, peer, /*ping_end=*/time_received, vRecv);
        return;
    }

    if (msg_type == NetMsgType::FILTERLOAD) {
        if (!(peer.m_our_services & NODE_BLOOM)) {
            LogDebug(BCLog::NET, "filterload received despite not offering bloom services, %s", pfrom.DisconnectMsg());
            pfrom.fDisconnect = true;
            return;
        }
        CBloomFilter filter;
        vRecv >> filter;

        if (!filter.IsWithinSizeConstraints())
        {
            // There is no excuse for sending a too-large filter
            Misbehaving(peer, "too-large bloom filter");
        } else if (auto tx_relay = peer.GetTxRelay(); tx_relay != nullptr) {
            {
                LOCK(tx_relay->m_bloom_filter_mutex);
                tx_relay->m_bloom_filter.reset(new CBloomFilter(filter));
                tx_relay->m_relay_txs = true;
            }
            pfrom.m_bloom_filter_loaded = true;
            pfrom.m_relays_txs = true;
        }
        return;
    }

    if (msg_type == NetMsgType::FILTERADD) {
        if (!(peer.m_our_services & NODE_BLOOM)) {
            LogDebug(BCLog::NET, "filteradd received despite not offering bloom services, %s", pfrom.DisconnectMsg());
            pfrom.fDisconnect = true;
            return;
        }
        std::vector<unsigned char> vData;
        vRecv >> vData;

        // Nodes must NEVER send a data item > MAX_SCRIPT_ELEMENT_SIZE bytes (the max size for a script data object,
        // and thus, the maximum size any matched object can have) in a filteradd message
        bool bad = false;
        if (vData.size() > MAX_SCRIPT_ELEMENT_SIZE) {
            bad = true;
        } else if (auto tx_relay = peer.GetTxRelay(); tx_relay != nullptr) {
            LOCK(tx_relay->m_bloom_filter_mutex);
            if (tx_relay->m_bloom_filter) {
                tx_relay->m_bloom_filter->insert(vData);
            } else {
                bad = true;
            }
        }
        if (bad) {
            Misbehaving(peer, "bad filteradd message");
        }
        return;
    }

    if (msg_type == NetMsgType::FILTERCLEAR) {
        if (!(peer.m_our_services & NODE_BLOOM)) {
            LogDebug(BCLog::NET, "filterclear received despite not offering bloom services, %s", pfrom.DisconnectMsg());
            pfrom.fDisconnect = true;
            return;
        }
        auto tx_relay = peer.GetTxRelay();
        if (!tx_relay) return;

        {
            LOCK(tx_relay->m_bloom_filter_mutex);
            tx_relay->m_bloom_filter = nullptr;
            tx_relay->m_relay_txs = true;
        }
        pfrom.m_bloom_filter_loaded = false;
        pfrom.m_relays_txs = true;
        return;
    }

    if (msg_type == NetMsgType::FEEFILTER) {
        CAmount newFeeFilter = 0;
        vRecv >> newFeeFilter;
        if (MoneyRange(newFeeFilter)) {
            if (auto tx_relay = peer.GetTxRelay(); tx_relay != nullptr) {
                tx_relay->m_fee_filter_received = newFeeFilter;
            }
            LogDebug(BCLog::NET, "received: feefilter of %s from peer=%d\n", CFeeRate(newFeeFilter).ToString(), pfrom.GetId());
        }
        return;
    }

    if (msg_type == NetMsgType::GETCFILTERS) {
        ProcessGetCFilters(pfrom, peer, vRecv);
        return;
    }

    if (msg_type == NetMsgType::GETCFHEADERS) {
        ProcessGetCFHeaders(pfrom, peer, vRecv);
        return;
    }

    if (msg_type == NetMsgType::GETCFCHECKPT) {
        ProcessGetCFCheckPt(pfrom, peer, vRecv);
        return;
    }

    if (msg_type == NetMsgType::NOTFOUND) {
        std::vector<CInv> vInv;
        vRecv >> vInv;
        std::vector<GenTxid> tx_invs;
        if (vInv.size() <= node::MAX_PEER_TX_ANNOUNCEMENTS + MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
            for (CInv &inv : vInv) {
                if (inv.IsGenTxMsg()) {
                    tx_invs.emplace_back(ToGenTxid(inv));
                }
            }
        }
        LOCK(m_tx_download_mutex);
        m_txdownloadman.ReceivedNotFound(pfrom.GetId(), tx_invs);
        return;
    }

    // TKNC: API request message handling
    if (msg_type == MessageTypes::APIREQ) {
        std::vector<uint8_t> request_data;
        vRecv >> request_data;

        APIRequest request;
        if (!request.Deserialize(request_data)) {
            LogDebug(BCLog::NET, "Invalid API request from peer=%d", pfrom.GetId());
            return;
        }

        if (!ValidateAPIRequest(request)) {
            APIResponse err_response;
            err_response.request_id = request.request_id;
            err_response.content = "Error: Invalid API key";
            err_response.tokens_used = 0;
            err_response.cost = 0;
            std::vector<uint8_t> response_data = err_response.Serialize();
            MakeAndPushMessage(pfrom, MessageTypes::APIRESP, response_data);
            return;
        }

        LogInfo("[P2P-API] Received API request: model=%s, peer=%d - relaying to LOCAL miner (direct HTTP)",
                 request.model.c_str(), pfrom.GetId());

        // P2P-RELAY: direct HTTP POST to local miner (bypass BackendRouter; auth already validated; localhost trust via X-P2P-Relay header).
        std::string relay_error_msg;  // Capture actual error for better diagnostics
        {
            InferenceResult local_result = P2PRelayToLocalMiner(request);

            if (local_result.success) {
                LogInfo("[P2P-API] Local miner inference OK (direct HTTP): tokens=%lld cost=%lld",
                         (long long)local_result.tokens_used, (long long)local_result.cost);
                APIResponse local_response;
                local_response.request_id = request.request_id;
                local_response.content = local_result.content;
                local_response.tokens_used = local_result.tokens_used;
                local_response.cost = local_result.cost;
                std::vector<uint8_t> response_data = local_response.Serialize();
                MakeAndPushMessage(pfrom, MessageTypes::APIRESP, response_data);
                return;
            } else {
                relay_error_msg = local_result.error_message;
                LogInfo("[P2P-API] Local miner direct HTTP failed (%s), will try P2P peer forwarding",
                    local_result.error_message.c_str());
            }
        }

        // Node-Centric routing: LOCAL registry FIRST, then P2P peers
        const LocalMinerEntry* local_miner = m_local_miner_registry.SelectBest(request.model);
        if (local_miner && local_miner->online) {
            // Local miner is registered but P2PRelayToLocalMiner already failed above.
            // The relay failure means the miner couldn't process the request (e.g., LLM not loaded).
            // Return the actual error to the requester instead of a generic message.
            LogInfo("[P2P-API] Local miner registered but relay failed: wallet=%s model=%s error=%s",
                    local_miner->wallet_address.substr(0, 16).c_str(),
                    local_miner->model_name.c_str(),
                    relay_error_msg.c_str());
            APIResponse err_response;
            err_response.request_id = request.request_id;
            if (relay_error_msg.empty()) {
                err_response.content = "Error: Local miner relay failed (unknown reason)";
            } else {
                err_response.content = "Error: " + relay_error_msg;
            }
            err_response.tokens_used = 0;
            err_response.cost = 0;
            std::vector<uint8_t> response_data = err_response.Serialize();
            MakeAndPushMessage(pfrom, MessageTypes::APIRESP, response_data);
            return;
        } else {
            // No local miner — try P2P remote peer selection
            NodeId target_miner_peer = SelectBestMinerPeer(pfrom.GetId(), request.model);

        if (target_miner_peer >= 0 && target_miner_peer != pfrom.GetId()) {
            // Forward APIREQ to miner peer and wait for response
            LogInfo("[P2P-API] Forwarding API request to miner peer=%d (from peer=%d)",
                     target_miner_peer, pfrom.GetId());

            auto promise = std::make_shared<std::promise<APIResponse>>();
            std::future<APIResponse> future = promise->get_future();

            {
                LOCK(m_pending_api_mutex);
                m_pending_api_responses[request.request_id] = promise;
            }

            m_connman.ForNode(target_miner_peer, [&](CNode* node) {
                m_connman.PushMessage(node, NetMsg::Make(std::string(MessageTypes::APIREQ), request_data));
                return true;
            });

            // Wait for response from miner (24h timeout — no artificial limit, system is a bridge)
            auto status = future.wait_for(std::chrono::seconds(86400));
            if (status == std::future_status::timeout) {
                LogWarning("[P2P-API] Forwarded request timed out, req_id=%llu",
                          static_cast<unsigned long long>(request.request_id));

                LOCK(m_pending_api_mutex);
                m_pending_api_responses.erase(request.request_id);

                APIResponse timeout_resp;
                timeout_resp.request_id = request.request_id;
                timeout_resp.content = "Error: Miner inference timeout";
                timeout_resp.tokens_used = 0;
                timeout_resp.cost = 0;
                std::vector<uint8_t> timeout_data = timeout_resp.Serialize();
                MakeAndPushMessage(pfrom, MessageTypes::APIRESP, timeout_data);
                return;
            }

            APIResponse miner_response = future.get();

            // Relay miner's response back to original requester
            LogInfo("[P2P-API] Relaying response from miner: tokens=%d, cost=%d",
                     miner_response.tokens_used, miner_response.cost);

            std::vector<uint8_t> relay_data = miner_response.Serialize();
            MakeAndPushMessage(pfrom, MessageTypes::APIRESP, relay_data);

            LogDebug(BCLog::NET, "[P2P-API] Relayed API response to peer=%d, tokens=%d, cost=%d",
                     pfrom.GetId(), miner_response.tokens_used, miner_response.cost);
        } else {
            // No miner peer available - return error with actual diagnostic info
            LogWarning("[P2P-API] No miner peer available for forwarding (connected peers check failed)");

            APIResponse err_response;
            err_response.request_id = request.request_id;
            // Include the actual relay error if available (e.g., "Inference unavailable: LLM not loaded")
            if (!relay_error_msg.empty()) {
                err_response.content = "Error: " + relay_error_msg;
            } else {
                err_response.content = "Error: No online miner available. Ensure at least one miner is connected.";
            }
            err_response.tokens_used = 0;
            err_response.cost = 0;
            std::vector<uint8_t> response_data = err_response.Serialize();
            MakeAndPushMessage(pfrom, MessageTypes::APIRESP, response_data);
        }
        }  // end else (no local miner — P2P fallback)
        return;
    }

    // TKNC: API response message handling
    if (msg_type == MessageTypes::APIRESP) {
        std::vector<uint8_t> response_data;
        vRecv >> response_data;
        
        APIResponse response;
        if (!response.Deserialize(response_data)) {
            LogDebug(BCLog::NET, "Invalid API response from peer=%d", pfrom.GetId());
            return;
        }
        
        LogDebug(BCLog::NET, "Received API response from peer=%d, tokens=%d, cost=%d, req_id=%llu", 
                 pfrom.GetId(), response.tokens_used, response.cost,
                 static_cast<unsigned long long>(response.request_id));

        if (response.request_id != 0) {
            LOCK(m_pending_api_mutex);
            auto it = m_pending_api_responses.find(response.request_id);
            if (it != m_pending_api_responses.end()) {
                it->second->set_value(response);
                m_pending_api_responses.erase(it);
                LogDebug(BCLog::NET, "API response delivered to waiting caller, req_id=%llu",
                         static_cast<unsigned long long>(response.request_id));
            }
        }

        // Bridge to InferenceEngine's RemoteBackend promise/future path
        // Handles requests sent via BackendRouter → RemoteBackend → SendCallback
        ::InferenceEngine::HandleAPIResponse(
            response.request_id,
            response.content,
            response.tokens_used,
            response.cost);

        return;
    }

    // TKNC: APIKEYSYNC broadcasts new API keys to peers; receivers store in LevelDB for miner validation.
    if (msg_type == MessageTypes::APIKEYSYNC) {
        APIKey key_data;
        try {
            vRecv >> key_data;
        } catch (const std::exception& e) {
            LogWarning("[APIKey-P2P] Failed to deserialize APIKEYSYNC from peer=%d: %s",
                        pfrom.GetId(), e.what());
            return;
        }

        LogInfo("[APIKey-P2P] Received APIKEYSYNC from peer=%d: key=%s, balance=%lld, model=%s",
                 pfrom.GetId(), key_data.key, (long long)key_data.balance, key_data.model_name);

        // Write to local LevelDB (idempotent — skips if already exists)
        WriteAPIKeyFromP2P(key_data);

        return;
    }

    // TKNC: ESCROWSYNC broadcasts SpendingLimit (escrow) to peers; receivers store in LevelDB
    // so client nodes can validate/bill p2pinference locally.
    if (msg_type == MessageTypes::ESCROWSYNC) {
        SpendingLimit escrow_data;
        try {
            vRecv >> escrow_data;
        } catch (const std::exception& e) {
            LogWarning("[Escrow-P2P] Failed to deserialize ESCROWSYNC from peer=%d: %s",
                        pfrom.GetId(), e.what());
            return;
        }

        LogInfo("[Escrow-P2P] Received ESCROWSYNC from peer=%d: escrow_id=%s, api_key=%s",
                 pfrom.GetId(), escrow_data.escrow_id,
                 escrow_data.api_key.substr(0, std::min((size_t)10, escrow_data.api_key.length())));

        // Write to local LevelDB (idempotent — skips if already exists)
        WriteSpendingLimitFromP2P(escrow_data);

        return;
    }

    // === P2P Miner Advertisement: Receive and store miner info from peers ===
    if (msg_type == MessageTypes::MINER_INFO) {
        LogInfo("[MINER-BCAST] Received MINER_INFO from peer=%d (size=%d bytes)", pfrom.GetId(), (int)vRecv.size());
        std::vector<uint8_t> advert_data;
        try {
            vRecv >> advert_data;
        } catch (const std::exception& e) {
            LogWarning("[P2P-MINER-ADV] Failed to deserialize MINER_INFO from peer=%d: %s (vRecv size=%d)",
                        pfrom.GetId(), e.what(), (int)vRecv.size());
            return;
        }

        // Deserialize: format is "wallet_address\0model_name\0gpu_name\0api_port"
        std::string raw(advert_data.begin(), advert_data.end());
        ::MinerAdvertisement advert;
        advert.last_seen = GetTime();

        // Parse delimiter-separated fields
        size_t pos = 0;
        auto extract = [&]() -> std::string {
            if (pos >= raw.size()) return "";  // boundary guard
            size_t end = raw.find('\0', pos);
            if (end == std::string::npos) end = raw.size();
            std::string val = raw.substr(pos, end - pos);
            pos = end + 1;
            return val;
        };
        advert.wallet_address = extract();
        advert.model_name = extract();
        advert.gpu_name = extract();
        try { advert.api_port = std::stoi(extract()); } catch (...) { advert.api_port = 9332; }

        if (!advert.wallet_address.empty()) {
            LOCK(m_pending_api_mutex);
            m_peer_miner_ads[pfrom.GetId()] = advert;
        }
        return;
    }

    // === Node-Centric: Probe local miner registry (IPC, not P2P) ===
    {
        int64_t now = GetTime();
        if (now - m_last_local_probe_time >= MinerLocalRegistry::PROBE_INTERVAL_SECONDS) {
            m_last_local_probe_time = now;
            m_local_miner_registry.Probe();
            LogInfo("[LOCAL-REG] Probed local miners: %d online", (int)m_local_miner_registry.OnlineCount());
        }
    }

    // Ignore unknown message types for extensibility
    LogDebug(BCLog::NET, "Unknown message type \"%s\" from peer=%d", SanitizeString(msg_type), pfrom.GetId());
    return;
}

bool PeerManagerImpl::MaybeDiscourageAndDisconnect(CNode& pnode, Peer& peer)
{
    {
        LOCK(peer.m_misbehavior_mutex);

        // There's nothing to do if the m_should_discourage flag isn't set
        if (!peer.m_should_discourage) return false;

        peer.m_should_discourage = false;
    } // peer.m_misbehavior_mutex

    if (pnode.HasPermission(NetPermissionFlags::NoBan)) {
        // We never disconnect or discourage peers for bad behavior if they have NetPermissionFlags::NoBan permission
        LogWarning("Not punishing noban peer %d!", peer.m_id);
        return false;
    }

    if (pnode.IsManualConn()) {
        // We never disconnect or discourage manual peers for bad behavior
        LogWarning("Not punishing manually connected peer %d!", peer.m_id);
        return false;
    }

    if (pnode.addr.IsLocal()) {
        // We disconnect local peers for bad behavior but don't discourage (since that would discourage
        // all peers on the same local address)
        LogDebug(BCLog::NET, "Warning: disconnecting but not discouraging %s peer %d!\n",
                 pnode.m_inbound_onion ? "inbound onion" : "local", peer.m_id);
        pnode.fDisconnect = true;
        return true;
    }

    // Normal case: Disconnect the peer and discourage all nodes sharing the address
    LogDebug(BCLog::NET, "Disconnecting and discouraging peer %d!\n", peer.m_id);
    if (m_banman) m_banman->Discourage(pnode.addr);
    m_connman.DisconnectNode(pnode.addr);
    return true;
}

bool PeerManagerImpl::ProcessMessages(CNode& node, std::atomic<bool>& interruptMsgProc)
{
    AssertLockNotHeld(m_tx_download_mutex);
    AssertLockHeld(g_msgproc_mutex);

    PeerRef maybe_peer{GetPeerRef(node.GetId())};
    if (maybe_peer == nullptr) return false;
    Peer& peer{*maybe_peer};

    // For outbound connections, ensure that the initial VERSION message
    // has been sent first before processing any incoming messages
    if (!node.IsInboundConn() && !peer.m_outbound_version_message_sent) return false;

    {
        LOCK(peer.m_getdata_requests_mutex);
        if (!peer.m_getdata_requests.empty()) {
            ProcessGetData(node, peer, interruptMsgProc);
        }
    }

    const bool processed_orphan = ProcessOrphanTx(peer);

    if (node.fDisconnect)
        return false;

    if (processed_orphan) return true;

    // this maintains the order of responses
    // and prevents m_getdata_requests to grow unbounded
    {
        LOCK(peer.m_getdata_requests_mutex);
        if (!peer.m_getdata_requests.empty()) return true;
    }

    // Don't bother if send buffer is too full to respond anyway
    if (node.fPauseSend) return false;

    auto poll_result{node.PollMessage()};
    if (!poll_result) {
        // No message to process
        return false;
    }

    CNetMessage& msg{poll_result->first};
    bool fMoreWork = poll_result->second;

    TRACEPOINT(net, inbound_message,
        node.GetId(),
        node.m_addr_name.c_str(),
        node.ConnectionTypeAsString().c_str(),
        msg.m_type.c_str(),
        msg.m_recv.size(),
        msg.m_recv.data()
    );

    if (m_opts.capture_messages) {
        CaptureMessage(node.addr, msg.m_type, MakeUCharSpan(msg.m_recv), /*is_incoming=*/true);
    }

    try {
        ProcessMessage(peer, node, msg.m_type, msg.m_recv, msg.m_time, interruptMsgProc);
        if (interruptMsgProc) return false;
        {
            LOCK(peer.m_getdata_requests_mutex);
            if (!peer.m_getdata_requests.empty()) fMoreWork = true;
        }
        // Check for orphan to reconsider; cross-peer parent may cause up to 100ms extra delay.
        LOCK(m_tx_download_mutex);
        if (m_txdownloadman.HaveMoreWork(peer.m_id)) fMoreWork = true;
    } catch (const std::exception& e) {
        LogDebug(BCLog::NET, "%s(%s, %u bytes): Exception '%s' (%s) caught\n", __func__, SanitizeString(msg.m_type), msg.m_message_size, e.what(), typeid(e).name());
    } catch (...) {
        LogDebug(BCLog::NET, "%s(%s, %u bytes): Unknown exception caught\n", __func__, SanitizeString(msg.m_type), msg.m_message_size);
    }

    return fMoreWork;
}

void PeerManagerImpl::ConsiderEviction(CNode& pto, Peer& peer, std::chrono::seconds time_in_seconds)
{
    AssertLockHeld(cs_main);

    CNodeState &state = *State(pto.GetId());

    if (!state.m_chain_sync.m_protect && pto.IsOutboundOrBlockRelayConn() && state.fSyncStarted) {
        // Disconnect outbound peer if no equal-work block within CHAIN_SYNC_TIMEOUT + HEADERS_RESPONSE_TIME.
        if (state.pindexBestKnownBlock != nullptr && state.pindexBestKnownBlock->nChainWork >= m_chainman.ActiveChain().Tip()->nChainWork) {
            // The outbound peer has sent us a block with at least as much work as our current tip, so reset the timeout if it was set
            if (state.m_chain_sync.m_timeout != 0s) {
                state.m_chain_sync.m_timeout = 0s;
                state.m_chain_sync.m_work_header = nullptr;
                state.m_chain_sync.m_sent_getheaders = false;
            }
        } else if (state.m_chain_sync.m_timeout == 0s || (state.m_chain_sync.m_work_header != nullptr && state.pindexBestKnownBlock != nullptr && state.pindexBestKnownBlock->nChainWork >= state.m_chain_sync.m_work_header->nChainWork)) {
            // Outbound peer behind our tip (or never sent block/header); set new timeout based on current tip.
            state.m_chain_sync.m_timeout = time_in_seconds + CHAIN_SYNC_TIMEOUT;
            state.m_chain_sync.m_work_header = m_chainman.ActiveChain().Tip();
            state.m_chain_sync.m_sent_getheaders = false;
        } else if (state.m_chain_sync.m_timeout > 0s && time_in_seconds > state.m_chain_sync.m_timeout) {
            // Peer hasn't synced to our tip's work; send single getheaders to give them a chance.
            if (state.m_chain_sync.m_sent_getheaders) {
                // They've run out of time to catch up!
                LogInfo("Outbound peer has old chain, best known block = %s, %s", state.pindexBestKnownBlock != nullptr ? state.pindexBestKnownBlock->GetBlockHash().ToString() : "<none>", pto.DisconnectMsg());
                pto.fDisconnect = true;
            } else {
                assert(state.m_chain_sync.m_work_header);
                // Assume getheaders goes out (or skipped due to in-flight); peer should still respond with sufficient-work tip.
                MaybeSendGetHeaders(pto,
                        GetLocator(state.m_chain_sync.m_work_header->pprev),
                        peer);
                LogDebug(BCLog::NET, "sending getheaders to outbound peer=%d to verify chain work (current best known block:%s, benchmark blockhash: %s)\n", pto.GetId(), state.pindexBestKnownBlock != nullptr ? state.pindexBestKnownBlock->GetBlockHash().ToString() : "<none>", state.m_chain_sync.m_work_header->GetBlockHash().ToString());
                state.m_chain_sync.m_sent_getheaders = true;
                // Bump timeout to allow response (may clear, reset, or trigger disconnect).
                state.m_chain_sync.m_timeout = time_in_seconds + HEADERS_RESPONSE_TIME;
            }
        }
    }
}

void PeerManagerImpl::EvictExtraOutboundPeers(NodeClock::time_point now)
{
    // Disconnect youngest extra block-relay-only peer (or second-youngest if youngest gave a block); higher nodeid = more recent.
    if (m_connman.GetExtraBlockRelayCount() > 0) {
        std::pair<NodeId, std::chrono::seconds> youngest_peer{-1, 0}, next_youngest_peer{-1, 0};

        m_connman.ForEachNode([&](CNode* pnode) {
            if (!pnode->IsBlockOnlyConn() || pnode->fDisconnect) return;
            if (pnode->GetId() > youngest_peer.first) {
                next_youngest_peer = youngest_peer;
                youngest_peer.first = pnode->GetId();
                youngest_peer.second = pnode->m_last_block_time;
            }
        });
        NodeId to_disconnect = youngest_peer.first;
        if (youngest_peer.second > next_youngest_peer.second) {
            // Our newest block-relay-only peer gave us a block more recently;
            // disconnect our second youngest.
            to_disconnect = next_youngest_peer.first;
        }
        m_connman.ForNode(to_disconnect, [&](CNode* pnode) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
            AssertLockHeld(::cs_main);
            // Evict only if not receiving a block and connected long enough; blocks requested only from sufficient-work headers chain.
            CNodeState *node_state = State(pnode->GetId());
            if (node_state == nullptr ||
                (now - pnode->m_connected >= MINIMUM_CONNECT_TIME && node_state->vBlocksInFlight.empty())) {
                pnode->fDisconnect = true;
                LogDebug(BCLog::NET, "disconnecting extra block-relay-only peer=%d (last block received at time %d)\n",
                         pnode->GetId(), count_seconds(pnode->m_last_block_time));
                return true;
            } else {
                LogDebug(BCLog::NET, "keeping block-relay-only peer=%d chosen for eviction (connect time: %d, blocks_in_flight: %d)\n",
                         pnode->GetId(), TicksSinceEpoch<std::chrono::seconds>(pnode->m_connected), node_state->vBlocksInFlight.size());
            }
            return false;
        });
    }

    // Check whether we have too many outbound-full-relay peers
    if (m_connman.GetExtraFullOutboundCount() > 0) {
        // Disconnect outbound-full-relay peer that least recently announced a block (ties: higher nodeid); protect peers with no alternative network connection.
        NodeId worst_peer = -1;
        int64_t oldest_block_announcement = std::numeric_limits<int64_t>::max();

        m_connman.ForEachNode([&](CNode* pnode) EXCLUSIVE_LOCKS_REQUIRED(::cs_main, m_connman.GetNodesMutex()) {
            AssertLockHeld(::cs_main);

            // Only consider outbound-full-relay peers that are not already
            // marked for disconnection
            if (!pnode->IsFullOutboundConn() || pnode->fDisconnect) return;
            CNodeState *state = State(pnode->GetId());
            if (state == nullptr) return; // shouldn't be possible, but just in case
            // Don't evict our protected peers
            if (state->m_chain_sync.m_protect) return;
            // If this is the only connection on a particular network that is
            // OUTBOUND_FULL_RELAY or MANUAL, protect it.
            if (!m_connman.MultipleManualOrFullOutboundConns(pnode->addr.GetNetwork())) return;
            if (state->m_last_block_announcement < oldest_block_announcement || (state->m_last_block_announcement == oldest_block_announcement && pnode->GetId() > worst_peer)) {
                worst_peer = pnode->GetId();
                oldest_block_announcement = state->m_last_block_announcement;
            }
        });
        if (worst_peer != -1) {
            bool disconnected = m_connman.ForNode(worst_peer, [&](CNode* pnode) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
                AssertLockHeld(::cs_main);

                // Only disconnect peer connected long enough and not currently downloading a block.
                CNodeState &state = *State(pnode->GetId());
                if (now - pnode->m_connected > MINIMUM_CONNECT_TIME && state.vBlocksInFlight.empty()) {
                    LogDebug(BCLog::NET, "disconnecting extra outbound peer=%d (last block announcement received at time %d)\n", pnode->GetId(), oldest_block_announcement);
                    pnode->fDisconnect = true;
                    return true;
                } else {
                    LogDebug(BCLog::NET, "keeping outbound peer=%d chosen for eviction (connect time: %d, blocks_in_flight: %d)\n",
                             pnode->GetId(), TicksSinceEpoch<std::chrono::seconds>(pnode->m_connected), state.vBlocksInFlight.size());
                    return false;
                }
            });
            if (disconnected) {
                // Stop extra-peer attempts until next stale tip to limit network load.
                m_connman.SetTryNewOutboundPeer(false);
            }
        }
    }
}

void PeerManagerImpl::CheckForStaleTipAndEvictPeers()
{
    LOCK(cs_main);

    const auto current_time{NodeClock::now()};
    auto now{GetTime<std::chrono::seconds>()};

    EvictExtraOutboundPeers(current_time);

    if (now > m_stale_tip_check_time) {
        // Check whether our tip is stale, and if so, allow using an extra
        // outbound peer
        if (!m_chainman.m_blockman.LoadingBlocks() && m_connman.GetNetworkActive() && m_connman.GetUseAddrmanOutgoing() && TipMayBeStale()) {
            LogInfo("Potential stale tip detected, will try using extra outbound peer (last tip update: %d seconds ago)\n",
                      count_seconds(now - m_last_tip_update.load()));
            m_connman.SetTryNewOutboundPeer(true);
        } else if (m_connman.GetTryNewOutboundPeer()) {
            m_connman.SetTryNewOutboundPeer(false);
        }
        m_stale_tip_check_time = now + STALE_CHECK_INTERVAL;
    }

    if (!m_initial_sync_finished && CanDirectFetch()) {
        m_connman.StartExtraBlockRelayPeers();
        m_initial_sync_finished = true;
    }
}

void PeerManagerImpl::MaybeSendPing(CNode& node_to, Peer& peer, NodeClock::time_point now)
{
    if (m_connman.ShouldRunInactivityChecks(node_to, now) &&
        peer.m_ping_nonce_sent &&
        now > peer.m_ping_start.load() + TIMEOUT_INTERVAL)
    {
        // The ping timeout is using mocktime. To disable the check during
        // testing, increase -peertimeout.
        LogDebug(BCLog::NET, "ping timeout: %fs, %s", Ticks<SecondsDouble>(now - peer.m_ping_start.load()), node_to.DisconnectMsg());
        node_to.fDisconnect = true;
        return;
    }

    bool pingSend = false;

    if (peer.m_ping_queued) {
        // RPC ping request by user
        pingSend = true;
    }

    if (peer.m_ping_nonce_sent == 0 && now > peer.m_ping_start.load() + PING_INTERVAL) {
        // Ping automatically sent as a latency probe & keepalive.
        pingSend = true;
    }

    if (pingSend) {
        uint64_t nonce;
        do {
            nonce = FastRandomContext().rand64();
        } while (nonce == 0);
        peer.m_ping_queued = false;
        peer.m_ping_start = now;
        if (node_to.GetCommonVersion() > BIP0031_VERSION) {
            peer.m_ping_nonce_sent = nonce;
            MakeAndPushMessage(node_to, NetMsgType::PING, nonce);
        } else {
            // Peer is too old to support ping message type with nonce, pong will never arrive.
            peer.m_ping_nonce_sent = 0;
            MakeAndPushMessage(node_to, NetMsgType::PING);
        }
    }
}

void PeerManagerImpl::MaybeSendAddr(CNode& node, Peer& peer, std::chrono::microseconds current_time)
{
    // Nothing to do for non-address-relay peers
    if (!peer.m_addr_relay_enabled) return;

    LOCK(peer.m_addr_send_times_mutex);
    // Periodically advertise our local address to the peer.
    if (fListen && !m_chainman.IsInitialBlockDownload() &&
        peer.m_next_local_addr_send < current_time) {
        // Clear bloom filter on repeat send to ensure self-announcement goes out (small daily bandwidth cost).
        if (peer.m_next_local_addr_send != 0us) {
            peer.m_addr_known->reset();
        }
        if (std::optional<CService> local_service = GetLocalAddrForPeer(node)) {
            CAddress local_addr{*local_service, peer.m_our_services, Now<NodeSeconds>()};
            if (peer.m_next_local_addr_send == 0us) {
                // Send initial self-announcement separately to bypass rate-limiting with limited start-tokens.
                if (IsAddrCompatible(peer, local_addr)) {
                    std::vector<CAddress> self_announcement{local_addr};
                    if (peer.m_wants_addrv2) {
                        MakeAndPushMessage(node, NetMsgType::ADDRV2, CAddress::V2_NETWORK(self_announcement));
                    } else {
                        MakeAndPushMessage(node, NetMsgType::ADDR, CAddress::V1_NETWORK(self_announcement));
                    }
                }
            } else {
                // All later self-announcements are sent together with the other addresses.
                PushAddress(peer, local_addr);
            }
        }
        peer.m_next_local_addr_send = current_time + m_rng.rand_exp_duration(AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL);
    }

    // We sent an `addr` message to this peer recently. Nothing more to do.
    if (current_time <= peer.m_next_addr_send) return;

    peer.m_next_addr_send = current_time + m_rng.rand_exp_duration(AVG_ADDRESS_BROADCAST_INTERVAL);

    if (!Assume(peer.m_addrs_to_send.size() <= MAX_ADDR_TO_SEND)) {
        // Should be impossible since we always check size before adding to
        // m_addrs_to_send. Recover by trimming the vector.
        peer.m_addrs_to_send.resize(MAX_ADDR_TO_SEND);
    }

    // Remove addr records that the peer already knows about, and add new
    // addrs to the m_addr_known filter on the same pass.
    auto addr_already_known = [&peer](const CAddress& addr) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex) {
        bool ret = peer.m_addr_known->contains(addr.GetKey());
        if (!ret) peer.m_addr_known->insert(addr.GetKey());
        return ret;
    };
    peer.m_addrs_to_send.erase(std::remove_if(peer.m_addrs_to_send.begin(), peer.m_addrs_to_send.end(), addr_already_known),
                           peer.m_addrs_to_send.end());

    // No addr messages to send
    if (peer.m_addrs_to_send.empty()) return;

    if (peer.m_wants_addrv2) {
        MakeAndPushMessage(node, NetMsgType::ADDRV2, CAddress::V2_NETWORK(peer.m_addrs_to_send));
    } else {
        MakeAndPushMessage(node, NetMsgType::ADDR, CAddress::V1_NETWORK(peer.m_addrs_to_send));
    }
    peer.m_addrs_to_send.clear();

    // we only send the big addr message once
    if (peer.m_addrs_to_send.capacity() > 40) {
        peer.m_addrs_to_send.shrink_to_fit();
    }
}

void PeerManagerImpl::MaybeSendSendHeaders(CNode& node, Peer& peer)
{
    // Delay SENDHEADERS (BIP 130) until initial-headers-sync completes to avoid state-tracking issues.
    if (!peer.m_sent_sendheaders && node.GetCommonVersion() >= SENDHEADERS_VERSION) {
        LOCK(cs_main);
        CNodeState &state = *State(node.GetId());
        if (state.pindexBestKnownBlock != nullptr &&
                state.pindexBestKnownBlock->nChainWork > m_chainman.MinimumChainWork()) {
            // Send SENDHEADERS to all peers (incl. non-NODE_NETWORK like pruning nodes) preferring headers over inv.
            MakeAndPushMessage(node, NetMsgType::SENDHEADERS);
            peer.m_sent_sendheaders = true;
        }
    }
}

void PeerManagerImpl::MaybeSendFeefilter(CNode& pto, Peer& peer, std::chrono::microseconds current_time)
{
    if (m_opts.ignore_incoming_txs) return;
    if (pto.GetCommonVersion() < FEEFILTER_VERSION) return;
    // peers with the forcerelay permission should not filter txs to us
    if (pto.HasPermission(NetPermissionFlags::ForceRelay)) return;
    // Don't send feefilter messages to outbound block-relay-only peers since they should never announce
    // transactions to us, regardless of feefilter state.
    if (pto.IsBlockOnlyConn()) return;

    CAmount currentFilter = m_mempool.GetMinFee().GetFeePerK();

    if (m_chainman.IsInitialBlockDownload()) {
        // Received tx-inv messages are discarded when the active
        // chainstate is in IBD, so tell the peer to not send them.
        currentFilter = MAX_MONEY;
    } else {
        static const CAmount MAX_FILTER{m_fee_filter_rounder.round(MAX_MONEY)};
        if (peer.m_fee_filter_sent == MAX_FILTER) {
            // Send the current filter if we sent MAX_FILTER previously
            // and made it out of IBD.
            peer.m_next_send_feefilter = 0us;
        }
    }
    if (current_time > peer.m_next_send_feefilter) {
        CAmount filterToSend = m_fee_filter_rounder.round(currentFilter);
        // We always have a fee filter of at least the min relay fee
        filterToSend = std::max(filterToSend, m_mempool.m_opts.min_relay_feerate.GetFeePerK());
        if (filterToSend != peer.m_fee_filter_sent) {
            MakeAndPushMessage(pto, NetMsgType::FEEFILTER, filterToSend);
            peer.m_fee_filter_sent = filterToSend;
        }
        peer.m_next_send_feefilter = current_time + m_rng.rand_exp_duration(AVG_FEEFILTER_BROADCAST_INTERVAL);
    }
    // If the fee filter has changed substantially and it's still more than MAX_FEEFILTER_CHANGE_DELAY
    // until scheduled broadcast, then move the broadcast to within MAX_FEEFILTER_CHANGE_DELAY.
    else if (current_time + MAX_FEEFILTER_CHANGE_DELAY < peer.m_next_send_feefilter &&
                (currentFilter < 3 * peer.m_fee_filter_sent / 4 || currentFilter > 4 * peer.m_fee_filter_sent / 3)) {
        peer.m_next_send_feefilter = current_time + m_rng.randrange<std::chrono::microseconds>(MAX_FEEFILTER_CHANGE_DELAY);
    }
}

namespace {
class CompareInvMempoolOrder
{
    const CTxMemPool* m_mempool;
public:
    explicit CompareInvMempoolOrder(CTxMemPool* mempool) : m_mempool{mempool} {}

    bool operator()(std::set<Wtxid>::iterator a, std::set<Wtxid>::iterator b)
    {
        /* As std::make_heap produces a max-heap, we want the entries with the
         * higher mining score to sort later. */
        return m_mempool->CompareMiningScoreWithTopology(*b, *a);
    }
};
} // namespace

bool PeerManagerImpl::RejectIncomingTxs(const CNode& peer) const
{
    // block-relay-only peers may never send txs to us
    if (peer.IsBlockOnlyConn()) return true;
    if (peer.IsFeelerConn()) return true;
    // In -blocksonly mode, peers need the 'relay' permission to send txs to us
    if (m_opts.ignore_incoming_txs && !peer.HasPermission(NetPermissionFlags::Relay)) return true;
    return false;
}

void PeerManagerImpl::ProcessPong(CNode& pfrom, Peer& peer, const NodeClock::time_point ping_end, DataStream& vRecv)
{
    uint64_t nonce = 0;
    const size_t nAvail{vRecv.size()};
    bool bPingFinished = false;
    std::string sProblem;

    if (nAvail >= sizeof(nonce)) {
        vRecv >> nonce;

        // Only process pong message if there is an outstanding ping (old ping without nonce should never pong)
        if (peer.m_ping_nonce_sent != 0) {
            if (nonce == peer.m_ping_nonce_sent) {
                // Matching pong received, this ping is no longer outstanding
                bPingFinished = true;
                const auto ping_time = ping_end - peer.m_ping_start.load();
                if (ping_time.count() >= 0) {
                    // Let connman know about this successful ping-pong
                    pfrom.PongReceived(ping_time);
                    if (pfrom.IsPrivateBroadcastConn()) {
                        m_tx_for_private_broadcast.NodeConfirmedReception(pfrom.GetId());
                        LogDebug(BCLog::PRIVBROADCAST, "Got a PONG (the transaction will probably reach the network), marking for disconnect, %s",
                                 pfrom.LogPeer());
                        pfrom.fDisconnect = true;
                    }
                } else {
                    // This should never happen
                    sProblem = "Timing mishap";
                }
            } else {
                // Nonce mismatches are normal when pings are overlapping
                sProblem = "Nonce mismatch";
                if (nonce == 0) {
                    // This is most likely a bug in another implementation somewhere; cancel this ping
                    bPingFinished = true;
                    sProblem = "Nonce zero";
                }
            }
        } else {
            sProblem = "Unsolicited pong without ping";
        }
    } else {
        // This is most likely a bug in another implementation somewhere; cancel this ping
        bPingFinished = true;
        sProblem = "Short payload";
    }

    if (!(sProblem.empty())) {
        LogDebug(BCLog::NET, "pong peer=%d: %s, %x expected, %x received, %u bytes\n",
                 pfrom.GetId(),
                 sProblem,
                 peer.m_ping_nonce_sent,
                 nonce,
                 nAvail);
    }
    if (bPingFinished) {
        peer.m_ping_nonce_sent = 0;
    }
}

bool PeerManagerImpl::SetupAddressRelay(const CNode& node, Peer& peer)
{
    // Skip addr relay with block-relay-only connections to prevent link inference via addr traffic.
    if (node.IsBlockOnlyConn()) return false;

    if (!peer.m_addr_relay_enabled.exchange(true)) {
        // Initialize m_addr_known on first addr relay setup (version msg for outbound, first addr msg for inbound).
        peer.m_addr_known = std::make_unique<CRollingBloomFilter>(5000, 0.001);
    }

    return true;
}

void PeerManagerImpl::ProcessAddrs(std::string_view msg_type, CNode& pfrom, Peer& peer, std::vector<CAddress>&& vAddr, const std::atomic<bool>& interruptMsgProc)
{
    AssertLockNotHeld(m_peer_mutex);
    AssertLockHeld(g_msgproc_mutex);

    if (!SetupAddressRelay(pfrom, peer)) {
        LogDebug(BCLog::NET, "ignoring %s message from %s peer=%d\n", msg_type, pfrom.ConnectionTypeAsString(), pfrom.GetId());
        return;
    }

    if (vAddr.size() > MAX_ADDR_TO_SEND)
    {
        Misbehaving(peer, strprintf("%s message size = %u", msg_type, vAddr.size()));
        return;
    }

    // Store the new addresses
    std::vector<CAddress> vAddrOk;

    // Update/increment addr rate limiting bucket.
    const auto current_time{NodeClock::now()};
    if (peer.m_addr_token_bucket < MAX_ADDR_PROCESSING_TOKEN_BUCKET) {
        // Don't increment bucket if it's already full
        const auto time_diff{current_time - peer.m_addr_token_timestamp};
        const double increment{std::max(Ticks<SecondsDouble>(time_diff), 0.0) * MAX_ADDR_RATE_PER_SECOND};
        peer.m_addr_token_bucket = std::min<double>(peer.m_addr_token_bucket + increment, MAX_ADDR_PROCESSING_TOKEN_BUCKET);
    }
    peer.m_addr_token_timestamp = current_time;

    const bool rate_limited = !pfrom.HasPermission(NetPermissionFlags::Addr);
    uint64_t num_proc = 0;
    uint64_t num_rate_limit = 0;
    std::shuffle(vAddr.begin(), vAddr.end(), m_rng);
    for (CAddress& addr : vAddr)
    {
        if (interruptMsgProc)
            return;

        // Apply rate limiting.
        if (peer.m_addr_token_bucket < 1.0) {
            if (rate_limited) {
                ++num_rate_limit;
                continue;
            }
        } else {
            peer.m_addr_token_bucket -= 1.0;
        }
        // Store full nodes only (may include feeler-only candidates).
        if (!MayHaveUsefulAddressDB(addr.nServices) && !HasAllDesirableServiceFlags(addr.nServices))
            continue;

        if (addr.nTime <= NodeSeconds{100000000s} || addr.nTime > current_time + 10min) {
            addr.nTime = std::chrono::time_point_cast<std::chrono::seconds>(current_time - 5 * 24h);
        }
        AddAddressKnown(peer, addr);
        if (m_banman && (m_banman->IsDiscouraged(addr) || m_banman->IsBanned(addr))) {
            // Do not process banned/discouraged addresses beyond remembering we received them
            continue;
        }
        ++num_proc;
        const bool reachable{g_reachable_nets.Contains(addr)};
        if (addr.nTime > current_time - 10min && !peer.m_getaddr_sent && vAddr.size() <= 10 && addr.IsRoutable()) {
            // Relay to a limited number of other nodes
            RelayAddress(pfrom.GetId(), addr, reachable);
        }
        // Do not store addresses outside our network
        if (reachable) {
            vAddrOk.push_back(addr);
        }
    }
    peer.m_addr_processed += num_proc;
    peer.m_addr_rate_limited += num_rate_limit;
    LogDebug(BCLog::NET, "Received addr: %u addresses (%u processed, %u rate-limited) from peer=%d\n",
             vAddr.size(), num_proc, num_rate_limit, pfrom.GetId());

    m_addrman.Add(vAddrOk, pfrom.addr, /*time_penalty=*/2h);
    if (vAddr.size() < 1000) peer.m_getaddr_sent = false;

    // AddrFetch: Require multiple addresses to avoid disconnecting on self-announcements
    if (pfrom.IsAddrFetchConn() && vAddr.size() > 1) {
        LogDebug(BCLog::NET, "addrfetch connection completed, %s", pfrom.DisconnectMsg());
        pfrom.fDisconnect = true;
    }
}

bool PeerManagerImpl::SendMessages(CNode& node)
{
    AssertLockNotHeld(m_tx_download_mutex);
    AssertLockHeld(g_msgproc_mutex);

    PeerRef maybe_peer{GetPeerRef(node.GetId())};
    if (!maybe_peer) return false;
    Peer& peer{*maybe_peer};
    const Consensus::Params& consensusParams = m_chainparams.GetConsensus();

    // We must call MaybeDiscourageAndDisconnect first, to ensure that we'll
    // disconnect misbehaving peers even before the version handshake is complete.
    if (MaybeDiscourageAndDisconnect(node, peer)) return true;

    // Initiate version handshake for outbound connections
    if (!node.IsInboundConn() && !peer.m_outbound_version_message_sent) {
        PushNodeVersion(node, peer);
        peer.m_outbound_version_message_sent = true;
    }

    // Don't send anything until the version handshake is complete
    if (!node.fSuccessfullyConnected || node.fDisconnect)
        return true;

    const auto now{NodeClock::now()};
    const auto current_time{GetTime<std::chrono::microseconds>()};

    // Skip private broadcast peers (PushMessage also filters; this is an optimization).
    if (node.IsPrivateBroadcastConn()) {
        if (node.m_connected + PRIVATE_BROADCAST_MAX_CONNECTION_LIFETIME < now) {
            LogDebug(BCLog::PRIVBROADCAST, "Disconnecting: did not complete the transaction send within %d seconds, %s",
                     count_seconds(PRIVATE_BROADCAST_MAX_CONNECTION_LIFETIME), node.LogPeer());
            node.fDisconnect = true;
        }
        return true;
    }

    if (node.IsAddrFetchConn() && now - node.m_connected > 10 * AVG_ADDRESS_BROADCAST_INTERVAL) {
        LogDebug(BCLog::NET, "addrfetch connection timeout, %s", node.DisconnectMsg());
        node.fDisconnect = true;
        return true;
    }

    MaybeSendPing(node, peer, now);

    // MaybeSendPing may have marked peer for disconnection
    if (node.fDisconnect) return true;

    MaybeSendAddr(node, peer, current_time);

    MaybeSendSendHeaders(node, peer);

    {
        LOCK(cs_main);

        CNodeState &state = *State(node.GetId());

        // Start block sync
        if (m_chainman.m_best_header == nullptr) {
            m_chainman.m_best_header = m_chainman.ActiveChain().Tip();
        }

        // Determine whether to try initial headers sync or parallel block download (mainly affects IBD).
        bool sync_blocks_and_headers_from_peer = false;
        if (state.fPreferredDownload) {
            sync_blocks_and_headers_from_peer = true;
        } else if (CanServeBlocks(peer) && !node.IsAddrFetchConn()) {
            // Allow inbound peer block download when no outbound preferred peers or no in-flight blocks.
            // Prefer outbound to avoid loading inbound home users; fall back to inbound to ensure progress.
            if (m_num_preferred_download_peers == 0 || mapBlocksInFlight.empty()) {
                sync_blocks_and_headers_from_peer = true;
            }
        }

        if (!state.fSyncStarted && CanServeBlocks(peer) && !m_chainman.m_blockman.LoadingBlocks()) {
            // Only actively request headers from a single peer, unless we're close to today.
            if ((nSyncStarted == 0 && sync_blocks_and_headers_from_peer) || m_chainman.m_best_header->Time() > NodeClock::now() - 24h) {
                const CBlockIndex* pindexStart = m_chainman.m_best_header;
                /* If possible, start at the block preceding the currently
                   best known header.  This ensures that we always get a
                   non-empty list of headers back as long as the peer
                   is up-to-date.  With a non-empty response, we can initialise
                   the peer's known best block.  This wouldn't be possible
                   if we requested starting at m_chainman.m_best_header and
                   got back an empty response.  */
                if (pindexStart->pprev)
                    pindexStart = pindexStart->pprev;
                if (MaybeSendGetHeaders(node, GetLocator(pindexStart), peer)) {
                    LogDebug(BCLog::NET, "initial getheaders (%d) to peer=%d", pindexStart->nHeight, node.GetId());

                    state.fSyncStarted = true;
                    peer.m_headers_sync_timeout = current_time + HEADERS_DOWNLOAD_TIMEOUT_BASE +
                        (
                         // Convert HEADERS_DOWNLOAD_TIMEOUT_PER_HEADER to microseconds before scaling
                         // to maintain precision
                         std::chrono::microseconds{HEADERS_DOWNLOAD_TIMEOUT_PER_HEADER} *
                         Ticks<std::chrono::seconds>(NodeClock::now() - m_chainman.m_best_header->Time()) / consensusParams.nPowTargetSpacing
                        );
                    nSyncStarted++;
                }
            }
        }

        //
        // Try sending block announcements via headers
        //
        {
            // Send headers for connectable unknown blocks (≤MAX_BLOCKS_TO_ANNOUNCE); else fallback to inv.
            LOCK(peer.m_block_inv_mutex);
            std::vector<CBlock> vHeaders;
            bool fRevertToInv = ((!peer.m_prefers_headers &&
                                 (!state.m_requested_hb_cmpctblocks || peer.m_blocks_for_headers_relay.size() > 1)) ||
                                 peer.m_blocks_for_headers_relay.size() > MAX_BLOCKS_TO_ANNOUNCE);
            const CBlockIndex *pBestIndex = nullptr; // last header queued for delivery
            ProcessBlockAvailability(node.GetId()); // ensure pindexBestKnownBlock is up-to-date

            if (!fRevertToInv) {
                bool fFoundStartingHeader = false;
                // Find first unknown header; send all subsequent. Bail if any block not on active chain.
                for (const uint256& hash : peer.m_blocks_for_headers_relay) {
                    const CBlockIndex* pindex = m_chainman.m_blockman.LookupBlockIndex(hash);
                    assert(pindex);
                    if (m_chainman.ActiveChain()[pindex->nHeight] != pindex) {
                        // Bail out if we reorged away from this block
                        fRevertToInv = true;
                        break;
                    }
                    if (pBestIndex != nullptr && pindex->pprev != pBestIndex) {
                        // Blocks to announce don't connect; rare (invalidateblock/reconsiderblock loops). Revert to inv.
                        fRevertToInv = true;
                        break;
                    }
                    pBestIndex = pindex;
                    if (fFoundStartingHeader) {
                        // add this to the headers message
                        vHeaders.emplace_back(pindex->GetBlockHeader());
                    } else if (PeerHasHeader(&state, pindex)) {
                        continue; // keep looking for the first new block
                    } else if (pindex->pprev == nullptr || PeerHasHeader(&state, pindex->pprev)) {
                        // Peer doesn't have this header but they do have the prior one.
                        // Start sending headers.
                        fFoundStartingHeader = true;
                        vHeaders.emplace_back(pindex->GetBlockHeader());
                    } else {
                        // Peer doesn't have this header or the prior one -- nothing will
                        // connect, so bail out.
                        fRevertToInv = true;
                        break;
                    }
                }
            }
            if (!fRevertToInv && !vHeaders.empty()) {
                if (vHeaders.size() == 1 && state.m_requested_hb_cmpctblocks) {
                    // We only send up to 1 block as header-and-ids, as otherwise
                    // probably means we're doing an initial-ish-sync or they're slow
                    LogDebug(BCLog::NET, "%s sending header-and-ids %s to peer=%d\n", __func__,
                            vHeaders.front().GetHash().ToString(), node.GetId());

                    std::optional<CSerializedNetMsg> cached_cmpctblock_msg;
                    {
                        LOCK(m_most_recent_block_mutex);
                        if (m_most_recent_block_hash == pBestIndex->GetBlockHash()) {
                            cached_cmpctblock_msg = NetMsg::Make(NetMsgType::CMPCTBLOCK, *m_most_recent_compact_block);
                        }
                    }
                    if (cached_cmpctblock_msg.has_value()) {
                        PushMessage(node, std::move(cached_cmpctblock_msg.value()));
                    } else {
                        CBlock block;
                        const bool ret{m_chainman.m_blockman.ReadBlock(block, *pBestIndex)};
                        assert(ret);
                        CBlockHeaderAndShortTxIDs cmpctblock{block, m_rng.rand64()};
                        MakeAndPushMessage(node, NetMsgType::CMPCTBLOCK, cmpctblock);
                    }
                    state.pindexBestHeaderSent = pBestIndex;
                } else if (peer.m_prefers_headers) {
                    if (vHeaders.size() > 1) {
                        LogDebug(BCLog::NET, "%s: %u headers, range (%s, %s), to peer=%d\n", __func__,
                                vHeaders.size(),
                                vHeaders.front().GetHash().ToString(),
                                vHeaders.back().GetHash().ToString(), node.GetId());
                    } else {
                        LogDebug(BCLog::NET, "%s: sending header %s to peer=%d\n", __func__,
                                vHeaders.front().GetHash().ToString(), node.GetId());
                    }
                    MakeAndPushMessage(node, NetMsgType::HEADERS, TX_WITH_WITNESS(vHeaders));
                    state.pindexBestHeaderSent = pBestIndex;
                } else
                    fRevertToInv = true;
            }
            if (fRevertToInv) {
                // Fallback: inv the tip (last entry was our tip at some past point).
                if (!peer.m_blocks_for_headers_relay.empty()) {
                    const uint256& hashToAnnounce = peer.m_blocks_for_headers_relay.back();
                    const CBlockIndex* pindex = m_chainman.m_blockman.LookupBlockIndex(hashToAnnounce);
                    assert(pindex);

                    // Warn if announcing off-main-chain block (rare; logged only).
                    if (m_chainman.ActiveChain()[pindex->nHeight] != pindex) {
                        LogDebug(BCLog::NET, "Announcing block %s not on main chain (tip=%s)\n",
                            hashToAnnounce.ToString(), m_chainman.ActiveChain().Tip()->GetBlockHash().ToString());
                    }

                    // If the peer's chain has this block, don't inv it back.
                    if (!PeerHasHeader(&state, pindex)) {
                        peer.m_blocks_for_inv_relay.push_back(hashToAnnounce);
                        LogDebug(BCLog::NET, "%s: sending inv peer=%d hash=%s\n", __func__,
                            node.GetId(), hashToAnnounce.ToString());
                    }
                }
            }
            peer.m_blocks_for_headers_relay.clear();
        }

        //
        // Message: inventory
        //
        std::vector<CInv> vInv;
        {
            LOCK(peer.m_block_inv_mutex);
            vInv.reserve(std::max<size_t>(peer.m_blocks_for_inv_relay.size(), INVENTORY_BROADCAST_TARGET));

            // Add blocks
            for (const uint256& hash : peer.m_blocks_for_inv_relay) {
                vInv.emplace_back(MSG_BLOCK, hash);
                if (vInv.size() == MAX_INV_SZ) {
                    MakeAndPushMessage(node, NetMsgType::INV, vInv);
                    vInv.clear();
                }
            }
            peer.m_blocks_for_inv_relay.clear();
        }

        if (auto tx_relay = peer.GetTxRelay(); tx_relay != nullptr) {
                LOCK(tx_relay->m_tx_inventory_mutex);
                // Check whether periodic sends should happen
                bool fSendTrickle = node.HasPermission(NetPermissionFlags::NoBan);
                if (tx_relay->m_next_inv_send_time < current_time) {
                    fSendTrickle = true;
                    if (node.IsInboundConn()) {
                        tx_relay->m_next_inv_send_time = NextInvToInbounds(current_time, INBOUND_INVENTORY_BROADCAST_INTERVAL, node.m_network_key);
                    } else {
                        tx_relay->m_next_inv_send_time = current_time + m_rng.rand_exp_duration(OUTBOUND_INVENTORY_BROADCAST_INTERVAL);
                    }
                }

                // Time to send but the peer has requested we not relay transactions.
                if (fSendTrickle) {
                    LOCK(tx_relay->m_bloom_filter_mutex);
                    if (!tx_relay->m_relay_txs) tx_relay->m_tx_inventory_to_send.clear();
                }

                // Respond to BIP35 mempool requests
                if (fSendTrickle && tx_relay->m_send_mempool) {
                    auto vtxinfo = m_mempool.infoAll();
                    tx_relay->m_send_mempool = false;
                    const CFeeRate filterrate{tx_relay->m_fee_filter_received.load()};

                    LOCK(tx_relay->m_bloom_filter_mutex);

                    for (const auto& txinfo : vtxinfo) {
                        const Txid& txid{txinfo.tx->GetHash()};
                        const Wtxid& wtxid{txinfo.tx->GetWitnessHash()};
                        const auto inv = peer.m_wtxid_relay ?
                                             CInv{MSG_WTX, wtxid.ToUint256()} :
                                             CInv{MSG_TX, txid.ToUint256()};
                        tx_relay->m_tx_inventory_to_send.erase(wtxid);

                        // Don't send transactions that peers will not put into their mempool
                        if (txinfo.fee < filterrate.GetFee(txinfo.vsize)) {
                            continue;
                        }
                        if (tx_relay->m_bloom_filter) {
                            if (!tx_relay->m_bloom_filter->IsRelevantAndUpdate(*txinfo.tx)) continue;
                        }
                        tx_relay->m_tx_inventory_known_filter.insert(inv.hash);
                        vInv.push_back(inv);
                        if (vInv.size() == MAX_INV_SZ) {
                            MakeAndPushMessage(node, NetMsgType::INV, vInv);
                            vInv.clear();
                        }
                    }
                }

                // Determine transactions to relay
                if (fSendTrickle) {
                    // Produce a vector with all candidates for sending
                    std::vector<std::set<Wtxid>::iterator> vInvTx;
                    vInvTx.reserve(tx_relay->m_tx_inventory_to_send.size());
                    for (std::set<Wtxid>::iterator it = tx_relay->m_tx_inventory_to_send.begin(); it != tx_relay->m_tx_inventory_to_send.end(); it++) {
                        vInvTx.push_back(it);
                    }
                    const CFeeRate filterrate{tx_relay->m_fee_filter_received.load()};
                    // Topologically and fee-rate sort the inventory we send for privacy and priority reasons.
                    // A heap is used so that not all items need sorting if only a few are being sent.
                    CompareInvMempoolOrder compareInvMempoolOrder(&m_mempool);
                    std::make_heap(vInvTx.begin(), vInvTx.end(), compareInvMempoolOrder);
                    // No reason to drain out at many times the network's capacity,
                    // especially since we have many peers and some will draw much shorter delays.
                    unsigned int nRelayedTransactions = 0;
                    LOCK(tx_relay->m_bloom_filter_mutex);
                    size_t broadcast_max{INVENTORY_BROADCAST_TARGET + (tx_relay->m_tx_inventory_to_send.size()/1000)*5};
                    broadcast_max = std::min<size_t>(INVENTORY_BROADCAST_MAX, broadcast_max);
                    while (!vInvTx.empty() && nRelayedTransactions < broadcast_max) {
                        // Fetch the top element from the heap
                        std::pop_heap(vInvTx.begin(), vInvTx.end(), compareInvMempoolOrder);
                        std::set<Wtxid>::iterator it = vInvTx.back();
                        vInvTx.pop_back();
                        auto wtxid = *it;
                        // Remove it from the to-be-sent set
                        tx_relay->m_tx_inventory_to_send.erase(it);
                        // Not in the mempool anymore? don't bother sending it.
                        auto txinfo = m_mempool.info(wtxid);
                        if (!txinfo.tx) {
                            continue;
                        }
                        // Build inv first (txid or wtxid based on peer's wtxid-relay) for filter check.
                        const auto inv = peer.m_wtxid_relay ?
                                             CInv{MSG_WTX, wtxid.ToUint256()} :
                                             CInv{MSG_TX, txinfo.tx->GetHash().ToUint256()};
                        // Check if not in the filter already
                        if (tx_relay->m_tx_inventory_known_filter.contains(inv.hash)) {
                            continue;
                        }
                        // Peer told you to not send transactions at that feerate? Don't bother sending it.
                        if (txinfo.fee < filterrate.GetFee(txinfo.vsize)) {
                            continue;
                        }
                        if (tx_relay->m_bloom_filter && !tx_relay->m_bloom_filter->IsRelevantAndUpdate(*txinfo.tx)) continue;
                        // Send
                        vInv.push_back(inv);
                        nRelayedTransactions++;
                        if (vInv.size() == MAX_INV_SZ) {
                            MakeAndPushMessage(node, NetMsgType::INV, vInv);
                            vInv.clear();
                        }
                        tx_relay->m_tx_inventory_known_filter.insert(inv.hash);
                    }

                    // Ensure we'll respond to GETDATA requests for anything we've just announced
                    LOCK(m_mempool.cs);
                    tx_relay->m_last_inv_sequence = m_mempool.GetSequence();
                }
        }
        if (!vInv.empty())
            MakeAndPushMessage(node, NetMsgType::INV, vInv);

        // Detect whether we're stalling
        auto stalling_timeout = m_block_stalling_timeout.load();
        if (state.m_stalling_since.count() && state.m_stalling_since < current_time - stalling_timeout) {
            // Stalling indicates download window stuck; disconnection mainly happens during IBD.
            LogInfo("Peer is stalling block download, %s", node.DisconnectMsg());
            node.fDisconnect = true;
            // Increase timeout for the next peer so that we don't disconnect multiple peers if our own
            // bandwidth is insufficient.
            const auto new_timeout = std::min(2 * stalling_timeout, BLOCK_STALLING_TIMEOUT_MAX);
            if (stalling_timeout != new_timeout && m_block_stalling_timeout.compare_exchange_strong(stalling_timeout, new_timeout)) {
                LogDebug(BCLog::NET, "Increased stalling timeout temporarily to %d seconds\n", count_seconds(new_timeout));
            }
            return true;
        }
        // Disconnect on in-flight block timeout (block_interval * (1 + 0.5*N)); count only validated blocks to prevent timeout inflation.
        if (state.vBlocksInFlight.size() > 0) {
            QueuedBlock &queuedBlock = state.vBlocksInFlight.front();
            int nOtherPeersWithValidatedDownloads = m_peers_downloading_from - 1;
            if (current_time > state.m_downloading_since + std::chrono::seconds{consensusParams.nPowTargetSpacing} * (BLOCK_DOWNLOAD_TIMEOUT_BASE + BLOCK_DOWNLOAD_TIMEOUT_PER_PEER * nOtherPeersWithValidatedDownloads)) {
                LogInfo("Timeout downloading block %s, %s", queuedBlock.pindex->GetBlockHash().ToString(), node.DisconnectMsg());
                node.fDisconnect = true;
                return true;
            }
        }
        // Check for headers sync timeouts
        if (state.fSyncStarted && peer.m_headers_sync_timeout < std::chrono::microseconds::max()) {
            // Detect whether this is a stalling initial-headers-sync peer
            if (m_chainman.m_best_header->Time() <= NodeClock::now() - 24h) {
                if (current_time > peer.m_headers_sync_timeout && nSyncStarted == 1 && (m_num_preferred_download_peers - state.fPreferredDownload >= 1)) {
                    // Disconnect non-NoBan only-sync peer when alternatives exist; inbound-only won't disconnect (bigger problem).
                    if (!node.HasPermission(NetPermissionFlags::NoBan)) {
                        LogInfo("Timeout downloading headers, %s", node.DisconnectMsg());
                        node.fDisconnect = true;
                        return true;
                    } else {
                        LogInfo("Timeout downloading headers from noban peer, not %s", node.DisconnectMsg());
                        // Reset sync state to try a different peer (triggers ≥1 more getheaders to this peer).
                        state.fSyncStarted = false;
                        nSyncStarted--;
                        peer.m_headers_sync_timeout = 0us;
                    }
                }
            } else {
                // After we've caught up once, reset the timeout so we can't trigger
                // disconnect later.
                peer.m_headers_sync_timeout = std::chrono::microseconds::max();
            }
        }

        // Check that outbound peers have reasonable chains
        // GetTime() is used by this anti-DoS logic so we can test this using mocktime
        ConsiderEviction(node, peer, GetTime<std::chrono::seconds>());

        //
        // Message: getdata (blocks)
        //
        std::vector<CInv> vGetData;
        if (CanServeBlocks(peer) && ((sync_blocks_and_headers_from_peer && !IsLimitedPeer(peer)) || !m_chainman.IsInitialBlockDownload()) && state.vBlocksInFlight.size() < MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
            std::vector<const CBlockIndex*> vToDownload;
            NodeId staller = -1;
            auto get_inflight_budget = [&state]() {
                return std::max(0, MAX_BLOCKS_IN_TRANSIT_PER_PEER - static_cast<int>(state.vBlocksInFlight.size()));
            };

            // Prioritize current chainstate blocks over historical to reach network tip faster.
            FindNextBlocksToDownload(peer, get_inflight_budget(), vToDownload, staller);
            auto historical_blocks{m_chainman.GetHistoricalBlockRange()};
            if (historical_blocks && !IsLimitedPeer(peer)) {
                // If the first needed historical block is not an ancestor of the last,
                // we need to start requesting blocks from their last common ancestor.
                const CBlockIndex* from_tip = LastCommonAncestor(historical_blocks->first, historical_blocks->second);
                TryDownloadingHistoricalBlocks(
                    peer,
                    get_inflight_budget(),
                    vToDownload, from_tip, historical_blocks->second);
            }
            for (const CBlockIndex *pindex : vToDownload) {
                uint32_t nFetchFlags = GetFetchFlags(peer);
                vGetData.emplace_back(MSG_BLOCK | nFetchFlags, pindex->GetBlockHash());
                BlockRequested(node.GetId(), *pindex);
                LogDebug(BCLog::NET, "Requesting block %s (%d) peer=%d\n", pindex->GetBlockHash().ToString(),
                    pindex->nHeight, node.GetId());
            }
            if (state.vBlocksInFlight.empty() && staller != -1) {
                if (State(staller)->m_stalling_since == 0us) {
                    State(staller)->m_stalling_since = current_time;
                    LogDebug(BCLog::NET, "Stall started peer=%d\n", staller);
                }
            }
        }

        //
        // Message: getdata (transactions)
        //
        {
            LOCK(m_tx_download_mutex);
            for (const GenTxid& gtxid : m_txdownloadman.GetRequestsToSend(node.GetId(), current_time)) {
                vGetData.emplace_back(gtxid.IsWtxid() ? MSG_WTX : (MSG_TX | GetFetchFlags(peer)), gtxid.ToUint256());
                if (vGetData.size() >= MAX_GETDATA_SZ) {
                    MakeAndPushMessage(node, NetMsgType::GETDATA, vGetData);
                    vGetData.clear();
                }
            }
        }

        if (!vGetData.empty())
            MakeAndPushMessage(node, NetMsgType::GETDATA, vGetData);
    } // release cs_main
    MaybeSendFeefilter(node, peer, current_time);
    return true;
}
