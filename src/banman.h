// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef TKN_BANMAN_H
#define TKN_BANMAN_H

#include <addrdb.h>
#include <common/bloom.h>
#include <net_types.h>
#include <sync.h>
#include <util/fs.h>

#include <chrono>
#include <cstdint>
#include <memory>

// NOTE: When adjusting this, update rpcnet:setban's help ("24h")
static constexpr unsigned int DEFAULT_MISBEHAVING_BANTIME = 60 * 60 * 24; // Default 24-hour ban

/// How often to dump banned addresses/subnets to disk.
static constexpr std::chrono::minutes DUMP_BANS_INTERVAL{15};

class CClientUIInterface;
class CNetAddr;
class CSubNet;

// Banman manages two related but distinct concepts. (1) Banning: configured manually via setban RPC, we never accept incoming or create outgoing connections to a banned address/subnet, and we don't gossip its address; persisted to disk on shutdown and reloaded on startup; used to prevent connections with spy nodes or griefers. (2) Discouragement: if a peer misbehaves (see Misbehaving() in net_processing.cpp) we mark its address discouraged; incoming connections are still accepted but preferred for eviction, never used for outgoing, not gossiped; implemented as a bloom filter (probabilistic membership test, no list or unmark API); used to prevent limited connection slots being used up by incompatible/broken peers. Neither mechanism is a DoS protection: an attacker can always reconnect from a different IP. NOTE: previously misbehaving peers were banned (rather than discouraged), allowing unbounded growth of the in-memory banned-ips map and O(n) ADDR-message comparisons; see https://bitcoincore.org/en/2024/07/03/disclose-unbounded-banlist.

class BanMan
{
public:
    ~BanMan();
    BanMan(fs::path ban_file, CClientUIInterface* client_interface, int64_t default_ban_time);
    void Ban(const CNetAddr& net_addr, int64_t ban_time_offset = 0, bool since_unix_epoch = false) EXCLUSIVE_LOCKS_REQUIRED(!m_banned_mutex);
    void Ban(const CSubNet& sub_net, int64_t ban_time_offset = 0, bool since_unix_epoch = false) EXCLUSIVE_LOCKS_REQUIRED(!m_banned_mutex);
    void Discourage(const CNetAddr& net_addr) EXCLUSIVE_LOCKS_REQUIRED(!m_banned_mutex);
    void ClearBanned() EXCLUSIVE_LOCKS_REQUIRED(!m_banned_mutex);

    //! Return whether net_addr is banned
    bool IsBanned(const CNetAddr& net_addr) EXCLUSIVE_LOCKS_REQUIRED(!m_banned_mutex);

    //! Return whether sub_net is exactly banned
    bool IsBanned(const CSubNet& sub_net) EXCLUSIVE_LOCKS_REQUIRED(!m_banned_mutex);

    //! Return whether net_addr is discouraged.
    bool IsDiscouraged(const CNetAddr& net_addr) EXCLUSIVE_LOCKS_REQUIRED(!m_banned_mutex);

    bool Unban(const CNetAddr& net_addr) EXCLUSIVE_LOCKS_REQUIRED(!m_banned_mutex);
    bool Unban(const CSubNet& sub_net) EXCLUSIVE_LOCKS_REQUIRED(!m_banned_mutex);
    void GetBanned(banmap_t& banmap) EXCLUSIVE_LOCKS_REQUIRED(!m_banned_mutex);
    void DumpBanlist() EXCLUSIVE_LOCKS_REQUIRED(!m_banned_mutex);

private:
    void LoadBanlist() EXCLUSIVE_LOCKS_REQUIRED(!m_banned_mutex);
    //!clean unused entries (if bantime has expired)
    void SweepBanned() EXCLUSIVE_LOCKS_REQUIRED(m_banned_mutex);

    Mutex m_banned_mutex;
    banmap_t m_banned GUARDED_BY(m_banned_mutex);
    bool m_is_dirty GUARDED_BY(m_banned_mutex){false};
    CClientUIInterface* m_client_interface = nullptr;
    CBanDB m_ban_db;
    const int64_t m_default_ban_time;
    CRollingBloomFilter m_discouraged GUARDED_BY(m_banned_mutex) {50000, 0.000001};
};

#endif // TKN_BANMAN_H
