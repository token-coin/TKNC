#include <model/registry.h>
#include <model/registry_db.h>
#include <util/fs.h>
#include <util/time.h>
#include <algorithm>
#include <logging.h>

// Persistent model registry database (replaces in-memory vector)
static std::unique_ptr<CModelRegistryDB> g_model_registry_db;
static std::mutex g_model_registry_mutex;

void InitModelRegistryDB(const fs::path& path)
{
    g_model_registry_db = std::make_unique<CModelRegistryDB>(path, 1 << 20, false);
    LogInfo("InitModelRegistryDB: Model Registry database initialized at %s", PathToString(path));
}

bool HasModelRegistryDB()
{
    return g_model_registry_db != nullptr;
}

// Global registry instance (wrapper around DB)
static ModelRegistry g_model_registry;

ModelRegistry& GetModelRegistry()
{
    return g_model_registry;
}

bool ModelRegistry::RegisterModel(const ModelRegistryEntry& entry)
{
    if (entry.model_name.empty() || entry.model_hash.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_model_registry_mutex);

    ModelRegistryEntry new_entry = entry;
    if (new_entry.registered_at == 0) {
        new_entry.registered_at = GetTime();
    }
    new_entry.last_verified_at = GetTime();

    g_model_registry_db->WriteModel(new_entry.model_name, new_entry);
    return true;
}

bool ModelRegistry::UnregisterModel(const std::string& model_name)
{
    std::lock_guard<std::mutex> lock(g_model_registry_mutex);
    auto entry_opt = g_model_registry_db->ReadModel(model_name);
    if (entry_opt) {
        g_model_registry_db->EraseModel(model_name);
        return true;
    }
    return false;
}

bool ModelRegistry::IsModelAvailable(const std::string& model_name) const
{
    std::lock_guard<std::mutex> lock(g_model_registry_mutex);
    auto entry_opt = g_model_registry_db->ReadModel(model_name);
    if (entry_opt && entry_opt->loaded) {
        return true;
    }
    return false;
}

ModelRegistryEntry ModelRegistry::GetModelInfo(const std::string& model_name) const
{
    std::lock_guard<std::mutex> lock(g_model_registry_mutex);
    auto entry_opt = g_model_registry_db->ReadModel(model_name);
    if (entry_opt) {
        return *entry_opt;
    }
    return ModelRegistryEntry();
}

std::vector<ModelRegistryEntry> ModelRegistry::GetAllModels() const
{
    std::lock_guard<std::mutex> lock(g_model_registry_mutex);
    return g_model_registry_db->GetAllModels();
}

std::vector<ModelRegistryEntry> ModelRegistry::GetLoadedModels() const
{
    std::lock_guard<std::mutex> lock(g_model_registry_mutex);
    std::vector<ModelRegistryEntry> loaded;
    auto all = g_model_registry_db->GetAllModels();
    for (const auto& entry : all) {
        if (entry.loaded) {
            loaded.push_back(entry);
        }
    }
    return loaded;
}

bool ModelRegistry::VerifyModelHash(const std::string& model_name,
                                     const std::string& expected_hash) const
{
    if (expected_hash.empty()) return true; // No hash check required

    std::lock_guard<std::mutex> lock(g_model_registry_mutex);
    auto entry_opt = g_model_registry_db->ReadModel(model_name);
    if (entry_opt) {
        return entry_opt->model_hash == expected_hash;
    }
    return false; // Model not registered
}

bool ModelRegistry::SetModelLoaded(const std::string& model_name, bool loaded)
{
    std::lock_guard<std::mutex> lock(g_model_registry_mutex);
    auto entry_opt = g_model_registry_db->ReadModel(model_name);
    if (entry_opt) {
        ModelRegistryEntry entry = *entry_opt;
        entry.loaded = loaded;
        g_model_registry_db->WriteModel(model_name, entry);
        return true;
    }
    return false;
}

bool ModelRegistry::UpdateModelHash(const std::string& model_name,
                                     const std::string& new_hash)
{
    std::lock_guard<std::mutex> lock(g_model_registry_mutex);
    auto entry_opt = g_model_registry_db->ReadModel(model_name);
    if (entry_opt) {
        ModelRegistryEntry entry = *entry_opt;
        entry.model_hash = new_hash;
        entry.last_verified_at = GetTime();
        g_model_registry_db->WriteModel(model_name, entry);
        return true;
    }
    return false;
}

size_t ModelRegistry::GetModelCount() const
{
    std::lock_guard<std::mutex> lock(g_model_registry_mutex);
    return g_model_registry_db->GetModelCount();
}

void ModelRegistry::Clear()
{
    std::lock_guard<std::mutex> lock(g_model_registry_mutex);
    auto all = g_model_registry_db->GetAllModels();
    for (const auto& entry : all) {
        g_model_registry_db->EraseModel(entry.model_name);
    }
}