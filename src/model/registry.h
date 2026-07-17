#ifndef TKN_MODEL_REGISTRY_H
#define TKN_MODEL_REGISTRY_H

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <serialize.h>
#include <util/fs.h>

// Target node verifies model hash before forwarding inference requests
struct ModelRegistryEntry {
 std::string model_name; // Model identifier (e.g., "qwen2.5-0.5b-instruct")
 std::string model_hash; // SHA256 hash of GGUF file
 std::string file_path; // Local file path to GGUF
 int64_t size_mb; // Model file size in MB
 std::string quantization; // Quantization type (e.g., "Q4_K_M")
 std::string gpu_capability; // GPU name or "CPU"
 std::string publisher_wallet; // Publisher/miner wallet address
 bool loaded; // Currently loaded in memory
 int64_t registered_at; // Registration timestamp (Unix)
 int64_t last_verified_at; // Last hash verification timestamp

 ModelRegistryEntry() : size_mb(0), loaded(false),
 registered_at(0), last_verified_at(0) {}

 SERIALIZE_METHODS(ModelRegistryEntry, obj)
 {
 READWRITE(obj.model_name, obj.model_hash, obj.file_path, obj.size_mb,
 obj.quantization, obj.gpu_capability, obj.publisher_wallet,
 obj.loaded, obj.registered_at, obj.last_verified_at);
 }
};

// Used by target node to validate model hash before inference
class ModelRegistry
{
private:
 mutable std::mutex m_mutex;
 std::vector<ModelRegistryEntry> m_entries;

public:
 ModelRegistry() = default;

 // Register a new model or update existing entry
 bool RegisterModel(const ModelRegistryEntry& entry);

 // Unregister a model by name
 bool UnregisterModel(const std::string& model_name);

 // Check if a model is registered and loaded
 bool IsModelAvailable(const std::string& model_name) const;

 // Get model info by name
 ModelRegistryEntry GetModelInfo(const std::string& model_name) const;

 // Get all registered models
 std::vector<ModelRegistryEntry> GetAllModels() const;

 // Get all currently loaded models
 std::vector<ModelRegistryEntry> GetLoadedModels() const;

 // Verify model hash matches registered hash
 bool VerifyModelHash(const std::string& model_name,
 const std::string& expected_hash) const;

 // Update model loaded status
 bool SetModelLoaded(const std::string& model_name, bool loaded);

 // Update model hash (after file change)
 bool UpdateModelHash(const std::string& model_name,
 const std::string& new_hash);

 // Get count of registered models
 size_t GetModelCount() const;

 // Clear all entries
 void Clear();
};

// Global model registry instance
ModelRegistry& GetModelRegistry();

// Model Registry DB initialization
void InitModelRegistryDB(const fs::path& path);
bool HasModelRegistryDB();

#endif // TKN_MODEL_REGISTRY_H