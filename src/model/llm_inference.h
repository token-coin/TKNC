#ifndef TKN_LLM_INFERENCE_H
#define TKN_LLM_INFERENCE_H

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <functional>

struct llama_model;
struct llama_context;
struct llama_sampler;

class LLMInference {
public:
    struct Config {
        std::string model_path;
        std::string dll_path;
        int n_ctx;
        int n_predict;
        float temperature;
        float top_p;
        int n_threads;
        int n_gpu_layers;

        Config()
            : n_ctx(2048)
            , n_predict(128)
            , temperature(0.1f)
            , top_p(0.9f)
            , n_threads(4)
            , n_gpu_layers(999) {}
    };

    struct GenerationResult {
        std::string text;
        int prompt_tokens;
        int completion_tokens;
        bool success;
        std::string error;
    };

private:
    Config config;
    bool initialized;
    llama_model* m_model;
    llama_sampler* m_sampler;
    mutable std::mutex inference_mutex;

public:
    LLMInference();
    ~LLMInference();

    bool Initialize(const Config& cfg);
    bool IsInitialized() const;
    void Shutdown();

    GenerationResult Generate(const std::string& prompt, const std::string& system_prompt = "");

    void SetMaxTokens(int max_tokens);

    void GenerateAsync(const std::string& prompt,
                       std::function<void(GenerationResult)> callback,
                       const std::string& system_prompt = "");

    GenerationResult GenerateStream(const std::string& prompt,
                                    std::function<void(const std::string& token, int index)> on_token,
                                    const std::string& system_prompt = "");

private:
    std::string BuildChatPrompt(const std::string& user_message, const std::string& system_prompt);
};

#endif
