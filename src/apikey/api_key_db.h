#ifndef TKN_APIKEY_API_KEY_DB_H
#define TKN_APIKEY_API_KEY_DB_H

#include <apikey/api_key.h>
#include <dbwrapper.h>
#include <util/fs.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

/** API Key database storage. Uses LevelDB for persistent API Key data storage. */
class CAPIKeyDB
{
private:
    std::unique_ptr<CDBWrapper> m_db;

public:
    explicit CAPIKeyDB(const fs::path& path, size_t cache_size, bool wipe_data = false);

    /** Write API Key. */
    bool WriteAPIKey(const std::string& key, const APIKey& api_key);

    /** Read API Key. */
    std::optional<APIKey> ReadAPIKey(const std::string& key) const;

    /** Erase API Key. */
    bool EraseAPIKey(const std::string& key);

    /** Check if API Key exists. */
    bool HasAPIKey(const std::string& key) const;

    /** Get all API Keys. */
    std::vector<APIKey> GetAllAPIKeys() const;

    /** List all API Keys (with key map). */
    std::vector<std::pair<std::string, APIKey>> ListAllAPIKeys() const;

    /** Update API Key balance. */
    bool UpdateAPIKeyBalance(const std::string& key, CAmount new_balance);

    /** Update API Key expiry time. */
    bool UpdateAPIKeyExpiry(const std::string& key, int64_t new_expiry);

    /** Get API Key count. */
    size_t GetAPIKeyCount() const;
};

#endif // TKN_APIKEY_API_KEY_DB_H
