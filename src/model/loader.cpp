#include <model/loader.h>
#include <util/log.h>
#include <util/fs.h>
#include <algorithm>
#include <thread>

ModelLoader::ModelLoader() : inference_engine_initialized(false) {
    llm_engine = std::make_unique<LLMInference>();
}

bool ModelLoader::PreloadModel(const std::string& model_path, ModelType type) {
    LogInfo("ModelLoader: Preloading model %s (GPU mode)...", model_path.c_str());
    
    if (!fs::exists(fs::PathFromString(model_path))) {
        LogInfo("ModelLoader: Model file not found: %s", model_path.c_str());
        return false;
    }
    
    std::lock_guard<std::mutex> lock(load_mutex);
    
    if (!inference_engine_initialized) {
        LLMInference::Config llm_config;
        llm_config.model_path = model_path;
        llm_config.n_ctx = 4096;
        llm_config.n_predict = 128;
        llm_config.temperature = 0.1f;
        llm_config.top_p = 0.9f;
        int hw_threads = static_cast<int>(std::thread::hardware_concurrency());
        llm_config.n_threads = (hw_threads > 0) ? hw_threads : 4;
        llm_config.n_gpu_layers = 24;
        
        if (llm_engine->Initialize(llm_config)) {
            inference_engine_initialized = true;
            LogInfo("ModelLoader: LLM inference engine initialized with GPU support");
        } else {
            LogInfo("ModelLoader: Failed to initialize LLM engine - check GPU availability");
            return false;
        }
    }
    
    std::string short_name = model_path;
    size_t last_sep = short_name.find_last_of("\\/");
    if (last_sep != std::string::npos) {
        short_name = short_name.substr(last_sep + 1);
    }
    if (short_name.size() > 5 && short_name.substr(short_name.size() - 5) == ".gguf") {
        short_name = short_name.substr(0, short_name.size() - 5);
    }

    ModelInfo info;
    info.name = short_name;
    info.path = model_path;
    info.type = type;
    info.loaded = true;
    
    switch (type) {
        case ModelType::TEXT:
        default:
            info.category = "llm";
            info.max_context_length = 4096;
            info.quantization = "q4_0";
            info.gpu_required = true;
            info.n_gpu_layers = 999;
            break;
    }
    
    auto it = std::find_if(loaded_models.begin(), loaded_models.end(),
        [&model_path](const ModelInfo& m) { return m.path == model_path; });
    
    if (it != loaded_models.end()) {
        *it = info;
        LogInfo("ModelLoader: Model %s updated", model_path.c_str());
    } else {
        loaded_models.push_back(info);
        LogInfo("ModelLoader: Model %s loaded to GPU memory", model_path.c_str());
    }
    
    return true;
}

InferenceResponse ModelLoader::RunInference(const InferenceRequest& request) {
    InferenceResponse response;

    std::lock_guard<std::mutex> lock(load_mutex);

    if (!inference_engine_initialized || !llm_engine) {
        response.success = false;
        response.error_message = "Inference engine not initialized";
        LogInfo("ModelLoader: Inference failed - engine not ready");
        return response;
    }

    auto it = std::find_if(loaded_models.begin(), loaded_models.end(),
        [&request](const ModelInfo& m) { return m.name == request.model_name; });

    if (it == loaded_models.end() || !it->loaded) {
        response.success = false;
        response.error_message = "Model not loaded: " + request.model_name;
        LogInfo("ModelLoader: Inference failed - model %s not loaded", request.model_name.c_str());
        return response;
    }

    LogInfo("ModelLoader: Running inference for model: %s (mode: %s)",
            request.model_name.c_str(), it->gpu_required ? "GPU" : "CPU");
    
    llm_engine->SetMaxTokens(request.max_tokens);
    LLMInference::GenerationResult result = llm_engine->Generate(request.prompt, "");
    
    if (result.success) {
        response.generated_text = result.text;
        response.tokens_used = result.completion_tokens;
        response.success = true;
        
        LogInfo("ModelLoader: Inference completed - tokens: %d", result.completion_tokens);
    } else {
        response.success = false;
        response.error_message = result.error;
        LogInfo("ModelLoader: Inference failed - %s", result.error.c_str());
    }
    
    return response;
}

void ModelLoader::UnloadModel(const std::string& model_name) {
    std::lock_guard<std::mutex> lock(load_mutex);
    
    auto it = std::find_if(loaded_models.begin(), loaded_models.end(),
        [&model_name](const ModelInfo& m) { return m.name == model_name; });
    
    if (it != loaded_models.end()) {
        it->loaded = false;
        LogInfo("ModelLoader: Model %s unloaded from GPU", model_name.c_str());
    }
}

std::vector<ModelInfo> ModelLoader::GetLoadedModels() const {
    std::lock_guard<std::mutex> lock(load_mutex);
    
    std::vector<ModelInfo> result;
    for (const auto& model : loaded_models) {
        if (model.loaded) {
            result.push_back(model);
        }
    }
    return result;
}

bool ModelLoader::IsModelLoaded(const std::string& model_name) const {
    std::lock_guard<std::mutex> lock(load_mutex);
    
    auto it = std::find_if(loaded_models.begin(), loaded_models.end(),
        [&model_name](const ModelInfo& m) { return m.name == model_name; });
    
    return it != loaded_models.end() && it->loaded;
}

ModelInfo ModelLoader::GetModelInfo(const std::string& model_name) const {
    std::lock_guard<std::mutex> lock(load_mutex);
    
    auto it = std::find_if(loaded_models.begin(), loaded_models.end(),
        [&model_name](const ModelInfo& m) { return m.name == model_name; });
    
    if (it != loaded_models.end()) {
        return *it;
    }
    
    return ModelInfo();
}

void ModelLoader::ReleaseAllModels() {
    std::lock_guard<std::mutex> lock(load_mutex);
    
    for (auto& model : loaded_models) {
        model.loaded = false;
    }
    
    loaded_models.clear();
    
    if (llm_engine) {
        llm_engine.reset();
    }
    
    inference_engine_initialized = false;
    
    LogInfo("ModelLoader: All models released from GPU");
}

bool ModelLoader::IsInferenceEngineReady() const {
    std::lock_guard<std::mutex> lock(load_mutex);
    return inference_engine_initialized && llm_engine && llm_engine->IsInitialized();
}
