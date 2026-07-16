#include <escrow/escrow_db.h>
#include <logging.h>
#include <algorithm>

const std::string CSpendingLimitDB::DB_PREFIX = "escrow_";

CSpendingLimitDB::CSpendingLimitDB(const fs::path& path, size_t cache_size, bool wipe_data)
{
    LogInfo("CSpendingLimitDB: Initializing spending limit database");
    DBParams params;
    params.path = path;
    params.cache_bytes = cache_size;
    params.wipe_data = wipe_data;
    params.obfuscate = false;
    params.memory_only = false;
    m_db = std::make_unique<CDBWrapper>(params);
    LogInfo("CSpendingLimitDB: Spending limit database initialized");
}

bool CSpendingLimitDB::WriteSpendingLimit(const std::string& escrow_id, const SpendingLimit& entry)
{
    std::string db_key = DB_PREFIX + escrow_id;
    m_db->Write(db_key, entry);
    return true;
}

std::optional<SpendingLimit> CSpendingLimitDB::ReadSpendingLimit(const std::string& escrow_id) const
{
    std::string db_key = DB_PREFIX + escrow_id;
    SpendingLimit entry;
    if (m_db->Read(db_key, entry)) {
        return entry;
    }
    return std::nullopt;
}

bool CSpendingLimitDB::EraseSpendingLimit(const std::string& escrow_id)
{
    std::string db_key = DB_PREFIX + escrow_id;
    m_db->Erase(db_key);
    return true;
}

bool CSpendingLimitDB::HasSpendingLimit(const std::string& escrow_id) const
{
    std::string db_key = DB_PREFIX + escrow_id;
    return m_db->Exists(db_key);
}

std::vector<SpendingLimit> CSpendingLimitDB::GetAllSpendingLimits() const
{
    std::vector<SpendingLimit> result;
    std::unique_ptr<CDBIterator> cursor(m_db->NewIterator());
    for (cursor->SeekToFirst(); cursor->Valid(); cursor->Next()) {
        std::string key;
        if (cursor->GetKey(key) && key.substr(0, DB_PREFIX.size()) == DB_PREFIX) {
            SpendingLimit entry;
            if (cursor->GetValue(entry)) {
                result.push_back(entry);
            }
        }
    }
    return result;
}

std::vector<std::pair<std::string, SpendingLimit>> CSpendingLimitDB::ListAllSpendingLimits() const
{
    std::vector<std::pair<std::string, SpendingLimit>> result;
    std::unique_ptr<CDBIterator> cursor(m_db->NewIterator());
    for (cursor->SeekToFirst(); cursor->Valid(); cursor->Next()) {
        std::string key;
        if (cursor->GetKey(key) && key.substr(0, DB_PREFIX.size()) == DB_PREFIX) {
            SpendingLimit entry;
            if (cursor->GetValue(entry)) {
                std::string actual_key = key.substr(DB_PREFIX.size());
                result.push_back({actual_key, entry});
            }
        }
    }
    return result;
}

size_t CSpendingLimitDB::GetEscrowCount() const
{
    size_t count = 0;
    std::unique_ptr<CDBIterator> cursor(m_db->NewIterator());
    for (cursor->SeekToFirst(); cursor->Valid(); cursor->Next()) {
        std::string key;
        if (cursor->GetKey(key) && key.substr(0, DB_PREFIX.size()) == DB_PREFIX) {
            count++;
        }
    }
    return count;
}

std::optional<SpendingLimit> CSpendingLimitDB::FindSpendingLimitByAPIKey(const std::string& api_key) const
{
    auto all = ListAllSpendingLimits();
    // Helper: check if escrow is in an active state (usable for billing)
    auto is_active = [](const SpendingLimit& e) {
        return e.state == SpendingLimit::State::CREATED ||
               e.state == SpendingLimit::State::ACTIVE ||
               e.state == SpendingLimit::State::PAYMENT_PENDING;
    };
    // First pass: find an active escrow with non-empty user_wallet and miner_wallet
    for (const auto& [id, entry] : all) {
        if (entry.api_key == api_key && is_active(entry) &&
            !entry.user_wallet.empty() && !entry.miner_wallet.empty()) {
            return entry;
        }
    }
    // Second pass: find any active escrow (may have empty user_wallet)
    for (const auto& [id, entry] : all) {
        if (entry.api_key == api_key && is_active(entry)) {
            return entry;
        }
    }
    // Third pass: fall back to any matching escrow (for error reporting)
    for (const auto& [id, entry] : all) {
        if (entry.api_key == api_key) {
            return entry;
        }
    }
    return std::nullopt;
}

// === Token rate persistence ===
static const std::string TOKEN_RATE_PREFIX = "tokenrate_";

bool CSpendingLimitDB::WriteTokenRate(const std::string& miner_wallet, int64_t tokens_per_tknc)
{
    std::string db_key = TOKEN_RATE_PREFIX + miner_wallet;
    m_db->Write(db_key, tokens_per_tknc);
    return true;
}

std::vector<std::pair<std::string, int64_t>> CSpendingLimitDB::ListAllTokenRates() const
{
    std::vector<std::pair<std::string, int64_t>> result;
    std::unique_ptr<CDBIterator> cursor(m_db->NewIterator());
    for (cursor->SeekToFirst(); cursor->Valid(); cursor->Next()) {
        std::string key;
        if (cursor->GetKey(key) && key.substr(0, TOKEN_RATE_PREFIX.size()) == TOKEN_RATE_PREFIX) {
            int64_t rate = 0;
            if (cursor->GetValue(rate) && rate > 0) {
                std::string wallet = key.substr(TOKEN_RATE_PREFIX.size());
                result.push_back({wallet, rate});
            }
        }
    }
    return result;
}
