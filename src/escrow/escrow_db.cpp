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
    for (cursor->Seek(DB_PREFIX); cursor->Valid(); cursor->Next()) {
        std::string key;
        if (cursor->GetKey(key) && key.substr(0, DB_PREFIX.size()) == DB_PREFIX) {
            SpendingLimit entry;
            if (cursor->GetValue(entry)) {
                result.push_back(entry);
            }
        } else {
            break;
        }
    }
    return result;
}

std::vector<std::pair<std::string, SpendingLimit>> CSpendingLimitDB::ListAllSpendingLimits() const
{
    std::vector<std::pair<std::string, SpendingLimit>> result;
    std::unique_ptr<CDBIterator> cursor(m_db->NewIterator());
    for (cursor->Seek(DB_PREFIX); cursor->Valid(); cursor->Next()) {
        std::string key;
        if (cursor->GetKey(key) && key.substr(0, DB_PREFIX.size()) == DB_PREFIX) {
            SpendingLimit entry;
            if (cursor->GetValue(entry)) {
                std::string actual_key = key.substr(DB_PREFIX.size());
                result.push_back({actual_key, entry});
            }
        } else {
            break;
        }
    }
    return result;
}

size_t CSpendingLimitDB::GetEscrowCount() const
{
    size_t count = 0;
    std::unique_ptr<CDBIterator> cursor(m_db->NewIterator());
    for (cursor->Seek(DB_PREFIX); cursor->Valid(); cursor->Next()) {
        std::string key;
        if (cursor->GetKey(key) && key.substr(0, DB_PREFIX.size()) == DB_PREFIX) {
            count++;
        } else {
            break;
        }
    }
    return count;
}

std::optional<SpendingLimit> CSpendingLimitDB::FindSpendingLimitByAPIKey(const std::string& api_key) const
{
    auto all = ListAllSpendingLimits();
    for (const auto& [id, entry] : all) {
        if (entry.api_key == api_key) {
            return entry;
        }
    }
    return std::nullopt;
}

// === Miner price persistence ===
static const std::string MINER_PRICE_PREFIX = "miner_price_";

bool CSpendingLimitDB::WriteMinerPrice(const std::string& miner_wallet, int64_t price_per_1m_tknc)
{
    std::string db_key = MINER_PRICE_PREFIX + miner_wallet;
    m_db->Write(db_key, price_per_1m_tknc);
    return true;
}

std::vector<std::pair<std::string, int64_t>> CSpendingLimitDB::ListAllMinerPrices() const
{
    std::vector<std::pair<std::string, int64_t>> result;
    std::unique_ptr<CDBIterator> cursor(m_db->NewIterator());
    for (cursor->Seek(MINER_PRICE_PREFIX); cursor->Valid(); cursor->Next()) {
        std::string key;
        if (cursor->GetKey(key) && key.substr(0, MINER_PRICE_PREFIX.size()) == MINER_PRICE_PREFIX) {
            int64_t price = 0;
            if (cursor->GetValue(price) && price > 0) {
                std::string wallet = key.substr(MINER_PRICE_PREFIX.size());
                result.push_back({wallet, price});
            }
        } else {
            break;
        }
    }
    return result;
}
