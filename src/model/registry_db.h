#ifndef TKNC_MODEL_REGISTRY_DB_H
#define TKNC_MODEL_REGISTRY_DB_H

#include <model/registry.h>
#include <dbwrapper.h>
#include <util/fs.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class CModelRegistryDB
{
private:
    std::unique_ptr<CDBWrapper> m_db;
    static const std::string DB_PREFIX;

public:
    explicit CModelRegistryDB(const fs::path& path, size_t cache_size, bool wipe_data = false);

    bool WriteModel(const std::string& model_name, const ModelRegistryEntry& entry);
    std::optional<ModelRegistryEntry> ReadModel(const std::string& model_name) const;
    bool EraseModel(const std::string& model_name);
    std::vector<ModelRegistryEntry> GetAllModels() const;
    size_t GetModelCount() const;
};

#endif