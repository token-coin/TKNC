#include <model/registry_db.h>
#include <logging.h>

const std::string CModelRegistryDB::DB_PREFIX = "model_";

CModelRegistryDB::CModelRegistryDB(const fs::path& path, size_t cache_size, bool wipe_data)
{
    LogInfo("CModelRegistryDB: Initializing Model Registry database");
    DBParams params;
    params.path = path;
    params.cache_bytes = cache_size;
    params.wipe_data = wipe_data;
    params.obfuscate = false;
    params.memory_only = false;
    m_db = std::make_unique<CDBWrapper>(params);
    LogInfo("CModelRegistryDB: Model Registry database initialized");
}

bool CModelRegistryDB::WriteModel(const std::string& model_name, const ModelRegistryEntry& entry)
{
    std::string db_key = DB_PREFIX + model_name;
    m_db->Write(db_key, entry);
    return true;
}

std::optional<ModelRegistryEntry> CModelRegistryDB::ReadModel(const std::string& model_name) const
{
    std::string db_key = DB_PREFIX + model_name;
    ModelRegistryEntry entry;
    if (m_db->Read(db_key, entry)) {
        return entry;
    }
    return std::nullopt;
}

bool CModelRegistryDB::EraseModel(const std::string& model_name)
{
    std::string db_key = DB_PREFIX + model_name;
    m_db->Erase(db_key);
    return true;
}

std::vector<ModelRegistryEntry> CModelRegistryDB::GetAllModels() const
{
    std::vector<ModelRegistryEntry> result;
    std::unique_ptr<CDBIterator> cursor(m_db->NewIterator());
    for (cursor->Seek(DB_PREFIX); cursor->Valid(); cursor->Next()) {
        std::string key;
        if (cursor->GetKey(key) && key.substr(0, DB_PREFIX.size()) == DB_PREFIX) {
            ModelRegistryEntry entry;
            if (cursor->GetValue(entry)) {
                result.push_back(entry);
            }
        } else {
            break;
        }
    }
    return result;
}

size_t CModelRegistryDB::GetModelCount() const
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