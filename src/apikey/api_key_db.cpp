#include <apikey/api_key_db.h>
#include <logging.h>

#include <algorithm>
#include <optional>
#include <utility>

static const std::string APIKEY_DB_PREFIX = "apikey_";

CAPIKeyDB::CAPIKeyDB(const fs::path& path, size_t cache_size, bool wipe_data)
{
    LogInfo("CAPIKeyDB: Initializing API Key database");
    
    DBParams params;
    params.path = path;
    params.cache_bytes = cache_size;
    params.wipe_data = wipe_data;
    params.obfuscate = false;
    params.memory_only = false;
    
    m_db = std::make_unique<CDBWrapper>(params);
    
    LogInfo("CAPIKeyDB: API Key database initialized");
}

bool CAPIKeyDB::WriteAPIKey(const std::string& key, const APIKey& api_key)
{
    std::string db_key = APIKEY_DB_PREFIX + key;
    LogInfo("CAPIKeyDB: Write API Key: %s", key.substr(0, 10).c_str());
    m_db->Write(db_key, api_key);
    return true;
}

std::optional<APIKey> CAPIKeyDB::ReadAPIKey(const std::string& key) const
{
    std::string db_key = APIKEY_DB_PREFIX + key;
    APIKey api_key;
    if (m_db->Read(db_key, api_key)) {
        return api_key;
    }
    return std::nullopt;
}

bool CAPIKeyDB::EraseAPIKey(const std::string& key)
{
    std::string db_key = APIKEY_DB_PREFIX + key;
    LogInfo("CAPIKeyDB: Erase API Key: %s", key.substr(0, 10).c_str());
    m_db->Erase(db_key);
    return true;
}

bool CAPIKeyDB::HasAPIKey(const std::string& key) const
{
    std::string db_key = APIKEY_DB_PREFIX + key;
    return m_db->Exists(db_key);
}

std::vector<APIKey> CAPIKeyDB::GetAllAPIKeys() const
{
    std::vector<APIKey> result;
    
    std::unique_ptr<CDBIterator> cursor(m_db->NewIterator());
    
    for (cursor->Seek(APIKEY_DB_PREFIX); cursor->Valid(); cursor->Next()) {
        std::string key;
        if (cursor->GetKey(key) && key.substr(0, APIKEY_DB_PREFIX.size()) == APIKEY_DB_PREFIX) {
            APIKey api_key;
            if (cursor->GetValue(api_key)) {
                result.push_back(api_key);
            }
        } else {
            break;
        }
    }
    
    LogInfo("CAPIKeyDB: GetAllAPIKeys, total %d keys", result.size());
    return result;
}

bool CAPIKeyDB::UpdateAPIKeyBalance(const std::string& key, CAmount new_balance)
{
    auto api_key_opt = ReadAPIKey(key);
    if (!api_key_opt) {
        LogInfo("CAPIKeyDB: Update balance failed, API Key not found: %s", key.substr(0, 10).c_str());
        return false;
    }
    
    APIKey api_key = *api_key_opt;
    api_key.balance = new_balance;
    
    LogInfo("CAPIKeyDB: Update API Key balance: %s, new balance: %d", key.substr(0, 10).c_str(), new_balance);
    return WriteAPIKey(key, api_key);
}

bool CAPIKeyDB::UpdateAPIKeyExpiry(const std::string& key, int64_t new_expiry)
{
    auto api_key_opt = ReadAPIKey(key);
    if (!api_key_opt) {
        LogInfo("CAPIKeyDB: Update expiry failed, API Key not found: %s", key.substr(0, 10).c_str());
        return false;
    }
    
    APIKey api_key = *api_key_opt;
    api_key.expiry_time = new_expiry;
    
    LogInfo("CAPIKeyDB: Update API Key expiry: %s, new expiry: %d", key.substr(0, 10).c_str(), new_expiry);
    return WriteAPIKey(key, api_key);
}

size_t CAPIKeyDB::GetAPIKeyCount() const
{
    size_t count = 0;
    
    std::unique_ptr<CDBIterator> cursor(m_db->NewIterator());
    
    for (cursor->Seek(APIKEY_DB_PREFIX); cursor->Valid(); cursor->Next()) {
        std::string key;
        if (cursor->GetKey(key) && key.substr(0, APIKEY_DB_PREFIX.size()) == APIKEY_DB_PREFIX) {
            count++;
        } else {
            break;
        }
    }
    
    return count;
}

std::vector<std::pair<std::string, APIKey>> CAPIKeyDB::ListAllAPIKeys() const
{
    std::vector<std::pair<std::string, APIKey>> result;
    
    std::unique_ptr<CDBIterator> cursor(m_db->NewIterator());
    
    for (cursor->Seek(APIKEY_DB_PREFIX); cursor->Valid(); cursor->Next()) {
        std::string key;
        if (cursor->GetKey(key) && key.substr(0, APIKEY_DB_PREFIX.size()) == APIKEY_DB_PREFIX) {
            APIKey api_key;
            if (cursor->GetValue(api_key)) {
                std::string actual_key = key.substr(APIKEY_DB_PREFIX.size());
                result.push_back({actual_key, api_key});
            }
        } else {
            break;
        }
    }
    
    LogInfo("CAPIKeyDB: ListAllAPIKeys, total %d keys", result.size());
    return result;
}
