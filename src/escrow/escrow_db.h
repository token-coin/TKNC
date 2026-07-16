#ifndef TKNC_ESCROW_DB_H
#define TKNC_ESCROW_DB_H

#include <escrow/escrow.h>
#include <dbwrapper.h>
#include <util/fs.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class CSpendingLimitDB
{
private:
    std::unique_ptr<CDBWrapper> m_db;
    static const std::string DB_PREFIX;

public:
    explicit CSpendingLimitDB(const fs::path& path, size_t cache_size, bool wipe_data = false);

    bool WriteSpendingLimit(const std::string& escrow_id, const SpendingLimit& entry);
    std::optional<SpendingLimit> ReadSpendingLimit(const std::string& escrow_id) const;
    bool EraseSpendingLimit(const std::string& escrow_id);
    bool HasSpendingLimit(const std::string& escrow_id) const;
    std::vector<SpendingLimit> GetAllSpendingLimits() const;
    std::vector<std::pair<std::string, SpendingLimit>> ListAllSpendingLimits() const;
    size_t GetEscrowCount() const;

    // Find spending limit by API key (scans all entries)
    std::optional<SpendingLimit> FindSpendingLimitByAPIKey(const std::string& api_key) const;

    // Token rate persistence (survives node restart)
    bool WriteTokenRate(const std::string& miner_wallet, int64_t tokens_per_tknc);
    std::vector<std::pair<std::string, int64_t>> ListAllTokenRates() const;
};

#endif