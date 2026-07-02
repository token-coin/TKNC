#ifndef TKN_MODEL_LOADER_H
#define TKN_MODEL_LOADER_H

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <functional>
#include <model/llm_inference.h>

enum class ModelType {
    TEXT
};

struct ModelInfo {
    std::string name;
    ModelType type;
    std::string category;
    int64_t max_context_length;
    std::string quantization;
    bool gpu_required;
    int n_gpu_layers;
    std::string path;
    bool loaded;

    ModelInfo() : type(ModelType::TEXT), max_context_length(0),
                  gpu_required(false), n_gpu_layers(999), loaded(false) {}
};

struct InferenceRequest {
    std::string prompt;
    int max_tokens;
    float temperature;
    std::string model_name;

    InferenceRequest() : max_tokens(256), temperature(0.7f) {}
};

struct InferenceResponse {
    std::string generated_text;
    int tokens_used;
    bool success;
    std::string error_message;

    InferenceResponse() : tokens_used(0), success(false) {}
};

class ModelLoader {
private:
    mutable std::mutex load_mutex;
    std::vector<ModelInfo> loaded_models;

    bool inference_engine_initialized;
    std::unique_ptr<LLMInference> llm_engine;

public:
    ModelLoader();

    bool PreloadModel(const std::string& model_path, ModelType type);

    InferenceResponse RunInference(const InferenceRequest& request);

    void UnloadModel(const std::string& model_name);

    std::vector<ModelInfo> GetLoadedModels() const;

    bool IsModelLoaded(const std::string& model_name) const;

    ModelInfo GetModelInfo(const std::string& model_name) const;

    void ReleaseAllModels();

    bool IsInferenceEngineReady() const;
};

#endif // TKN_MODEL_LOADER_H