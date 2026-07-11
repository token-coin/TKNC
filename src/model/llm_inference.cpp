#include <model/llm_inference.h>
#include <model/llama_dll.h>
#include <util/fs_helpers.h>
#include <util/fs.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>
#include <numeric>

LLMInference::LLMInference()
    : initialized(false)
    , m_model(nullptr)
    , m_sampler(nullptr) {}

LLMInference::~LLMInference() {
    Shutdown();
}

void LLMInference::Shutdown() {
    if (m_sampler) {
        LLamaDLL::Instance().sampler_free(m_sampler);
        m_sampler = nullptr;
    }
    if (m_model) {
        LLamaDLL::Instance().model_free(m_model);
        m_model = nullptr;
    }
    initialized = false;
}

bool LLMInference::Initialize(const Config& cfg) {
    if (initialized) return true;

    config = cfg;

    std::string dll_path = config.dll_path;
    if (dll_path.empty()) {
        // Default: DLLs are in <exe_dir>/dll/ subdirectory — works on ANY machine, ANY folder
        dll_path = fs::PathToString(GetExeDir() / "dll" / "llama.dll");
    } else if (dll_path.find('/') == std::string::npos && dll_path.find('\\') == std::string::npos) {
        // Bare filename like "llama.dll" — resolve relative to exe's dll/ subdir
        // This handles config files that specify just the filename
        std::cerr << "[LLM] Bare filename '" << dll_path << "' detected, resolving to exe-relative path" << std::endl;
        dll_path = fs::PathToString(GetExeDir() / "dll" / fs::u8path(dll_path));
    } else if (!fs::u8path(dll_path).is_absolute()) {
        // Relative path with directory component (e.g., "dll/llama.dll") — resolve from exe dir
        dll_path = fs::PathToString(GetExeDir() / fs::u8path(dll_path));
    }
    // If already absolute path, use as-is

    std::cerr << "[LLM] Resolved llama.dll path: " << dll_path << std::endl;

    auto& dll = LLamaDLL::Instance();
    if (!dll.Load(dll_path)) {
        std::cerr << "[LLM] Failed to load llama.dll from: " << dll_path << std::endl;
        return false;
    }

    auto model_params = dll.model_default_params();
    model_params.n_gpu_layers = config.n_gpu_layers;

    // Multi-GPU offload: pass all GPU devices to llama.cpp for layer distribution via tensor_split.
    const auto& gpu_devices = dll.GetAllGPUDevices();
    if (!gpu_devices.empty() && config.n_gpu_layers > 0) {
        // Build NULL-terminated device array
        std::vector<void*> device_array;
        for (const auto& dev : gpu_devices) {
            device_array.push_back(dev.handle);
        }
        device_array.push_back(nullptr);  // NULL terminator

        model_params.devices = device_array.data();

        if (gpu_devices.size() == 1) {
            std::cerr << "[LLM] Single GPU: " << gpu_devices[0].name
                      << " (VRAM=" << gpu_devices[0].vram_total / (1024*1024) << " MB)" << std::endl;
        } else {
            // Multi-GPU: compute tensor_split proportional to VRAM
            std::vector<float> split(gpu_devices.size());
            size_t total_vram = 0;
            for (const auto& dev : gpu_devices) total_vram += dev.vram_total;

            if (total_vram > 0) {
                for (size_t i = 0; i < gpu_devices.size(); i++) {
                    split[i] = (float)((double)gpu_devices[i].vram_total / (double)total_vram);
                }
            } else {
                // VRAM info unavailable — equal split
                for (size_t i = 0; i < gpu_devices.size(); i++) {
                    split[i] = 1.0f / (float)gpu_devices.size();
                }
            }

            // Use ROW split mode for better load balancing across GPUs
            model_params.split_mode = LLAMA_SPLIT_MODE_ROW;
            model_params.tensor_split = split.data();
            model_params.main_gpu = 0;  // Primary GPU (first dGPU)

            std::cerr << "[LLM] Multi-GPU offload: " << gpu_devices.size() << " GPU(s)" << std::endl;
            for (size_t i = 0; i < gpu_devices.size(); i++) {
                const char* type_name = (gpu_devices[i].dev_type == 1) ? "dGPU" : "iGPU";
                std::cerr << "[LLM]   GPU[" << i << "]: " << gpu_devices[i].name
                          << " (" << type_name << ", VRAM=" << gpu_devices[i].vram_total / (1024*1024) << " MB"
                          << ", split=" << (int)(split[i] * 100) << "%)" << std::endl;
            }
        }
    } else if (config.n_gpu_layers > 0) {
        std::cerr << "[LLM] No GPU device detected — model will run on CPU only" << std::endl;
    }

    m_model = dll.model_load_from_file(config.model_path.c_str(), model_params);
    if (!m_model) {
        std::cerr << "[LLM] Failed to load model: " << config.model_path << std::endl;
        return false;
    }

    // GPU validation: best-effort check that model was offloaded to GPU (may be unavailable in some builds).
    if (config.n_gpu_layers > 0) {
        bool on_gpu = dll.IsModelOnGPU(m_model);
        if (on_gpu) {
            std::cerr << "[LLM] GPU offload verified: model running on GPU acceleration" << std::endl;
        } else {
            // Verification unavailable but model loaded successfully with n_gpu_layers > 0
            // → llama.cpp already handled GPU selection internally, trust it
            std::cerr << "[LLM] [INFO] GPU offload: n_gpu_layers=" << config.n_gpu_layers
                      << " requested, model loaded OK (verification unavailable, trusting llama.cpp)" << std::endl;
        }
    }

    char desc_buf[256];
    dll.model_desc(m_model, desc_buf, sizeof(desc_buf));

    m_sampler = dll.sampler_chain_init({false});
    if (!m_sampler) {
        std::cerr << "[LLM] Failed to create sampler chain" << std::endl;
        dll.model_free(m_model);
        m_model = nullptr;
        return false;
    }

    if (config.temperature <= 0.01f) {
        dll.sampler_chain_add(m_sampler, dll.sampler_init_greedy());
    } else {
        dll.sampler_chain_add(m_sampler, dll.sampler_init_penalties(-1, 1.1f, 0.0f, 0.0f));
        dll.sampler_chain_add(m_sampler, dll.sampler_init_top_k(40));
        dll.sampler_chain_add(m_sampler, dll.sampler_init_top_p(config.top_p, 1));
        dll.sampler_chain_add(m_sampler, dll.sampler_init_temp(config.temperature));
        dll.sampler_chain_add(m_sampler, dll.sampler_init_dist((uint32_t)time(nullptr)));
    }

    initialized = true;
    return true;
}

bool LLMInference::IsInitialized() const {
    return initialized && m_model != nullptr;
}

std::string LLMInference::BuildChatPrompt(const std::string& user_message, const std::string& system_prompt) {
    std::string prompt;
    if (!system_prompt.empty()) {
        prompt += "<|im_start|>system\n" + system_prompt + "<|im_end|>\n";
    }
    prompt += "<|im_start|>user\n" + user_message + "<|im_end|>\n";
    prompt += "<|im_start|>assistant\n";
    return prompt;
}

void LLMInference::SetMaxTokens(int max_tokens) {
    // No artificial cap. The system is a bridge — the miner's hardware determines
    // how many tokens it can generate. A data-center miner with a 10000B model
    // should be able to generate as many tokens as it wants.
    // max_tokens > 0: generate up to N tokens
    // max_tokens < 0: unlimited (generate until EOS)
    // max_tokens == 0: keep current config unchanged
    if (max_tokens != 0) {
        config.n_predict = max_tokens;
    }
}

// Helper: create llama_context with adaptive n_ctx fallback.
// When configured n_ctx is very large (e.g. 131072) but GPU VRAM is insufficient,
// context creation fails silently. This function tries progressively smaller
// context sizes until one succeeds.
static llama_context* CreateContextAdaptive(LLamaDLL& dll, llama_model* model,
                                             int configured_n_ctx, int n_threads,
                                             int& actual_n_ctx_out) {
    int ctx_sizes_to_try[] = {configured_n_ctx, 32768, 16384, 8192, 4096, 2048};
    for (int try_ctx : ctx_sizes_to_try) {
        if (try_ctx > configured_n_ctx) continue;
        llama_context* ctx = dll.new_context_with_model_default(model, try_ctx, n_threads);
        if (ctx) {
            actual_n_ctx_out = try_ctx;
            if (try_ctx < configured_n_ctx) {
                std::cerr << "[LLM] Context created with n_ctx=" << try_ctx
                          << " (requested " << configured_n_ctx
                          << " failed — likely VRAM insufficient for KV cache)" << std::endl;
            } else {
                std::cerr << "[LLM] Context created with n_ctx=" << try_ctx << std::endl;
            }
            return ctx;
        }
        std::cerr << "[LLM] Context creation failed for n_ctx=" << try_ctx
                  << ", trying smaller..." << std::endl;
    }
    actual_n_ctx_out = 0;
    return nullptr;
}

LLMInference::GenerationResult LLMInference::Generate(const std::string& prompt, const std::string& system_prompt) {
    GenerationResult result;
    result.success = false;
    result.prompt_tokens = 0;
    result.completion_tokens = 0;

    if (!initialized || !m_model) {
        result.error = "LLM not initialized";
        return result;
    }

    std::lock_guard<std::mutex> lock(inference_mutex);

    auto& dll = LLamaDLL::Instance();

    int actual_n_ctx = 0;
    llama_context* ctx = CreateContextAdaptive(dll, m_model, config.n_ctx, config.n_threads, actual_n_ctx);
    if (!ctx) {
        result.error = "Failed to create context (tried n_ctx from " + std::to_string(config.n_ctx) + " down to 2048)";
        std::cerr << "[LLM] ERROR: " << result.error << std::endl;
        return result;
    }

    // Clear KV cache from any previous context (safety measure)
    dll.kv_cache_clear(ctx);

    std::string full_prompt = BuildChatPrompt(prompt, system_prompt);

    // Two-pass tokenization: use a large buffer first, then retry if needed.
    // special=true: parse ChatML markers (<|im_start|>, <|im_end|>) as special tokens.
    // This is CRITICAL — with special=false, the model doesn't recognize ChatML format
    // and may generate <|im_end|> immediately, producing empty output.
    size_t tok_buf_size = std::max((size_t)actual_n_ctx, (size_t)8192);
    std::vector<llama_token> tokens(tok_buf_size);
    int n_tokens = dll.tokenize(m_model, full_prompt.c_str(), (int32_t)full_prompt.size(),
                                tokens.data(), (int32_t)tokens.size(), true, true);
    if (n_tokens < 0) {
        // Buffer too small — resize to required size and retry
        size_t required = static_cast<size_t>(-n_tokens) + 16;
        tokens.resize(required);
        n_tokens = dll.tokenize(m_model, full_prompt.c_str(), (int32_t)full_prompt.size(),
                                tokens.data(), (int32_t)tokens.size(), true, true);
        if (n_tokens < 0) {
            result.error = "Tokenization failed (required=" + std::to_string(-n_tokens) + ")";
            dll.free(ctx);
            return result;
        }
        std::cerr << "[LLM] Tokenization buffer resized to " << required << " tokens (n_ctx=" << actual_n_ctx << ")" << std::endl;
    }
    tokens.resize(n_tokens);
    result.prompt_tokens = n_tokens;

    // Truncate to n_ctx if prompt exceeds context window
    if (n_tokens > actual_n_ctx) {
        std::cerr << "[LLM] Prompt tokens (" << n_tokens << ") exceed n_ctx (" << actual_n_ctx << ") — truncating" << std::endl;
        tokens.resize(actual_n_ctx);
        n_tokens = actual_n_ctx;
    }

    llama_token eos_token = dll.token_eos(m_model);

    // Also detect <|im_end|> token — for Qwen2.5, EOS may be <|endoftext|> while
    // <|im_end|> is the actual ChatML turn-ending marker.
    llama_token im_end_token = -1;
    {
        llama_token tmp_tokens[8];
        int n = dll.tokenize(m_model, "<|im_end|>", 10, tmp_tokens, 8, false, true);
        if (n == 1) im_end_token = tmp_tokens[0];
    }

    std::cerr << "[LLM] Generate: prompt_tokens=" << n_tokens
              << " n_ctx=" << actual_n_ctx
              << " eos_id=" << eos_token
              << " im_end_id=" << im_end_token << std::endl;

    int32_t ret = dll.decode(ctx, dll.batch_get_one(tokens.data(), n_tokens));
    if (ret != 0) {
        result.error = "Initial decode failed, ret=" + std::to_string(ret);
        std::cerr << "[LLM] ERROR: " << result.error << std::endl;
        dll.free(ctx);
        return result;
    }

    std::string response;
    int n_decoded = 0;

    // n_predict < 0 means unlimited (generate until EOS or context full)
    for (int i = 0; config.n_predict < 0 || i < config.n_predict; i++) {
        llama_token new_token = dll.sampler_sample(m_sampler, ctx, -1);

        // Log first 5 tokens for debugging
        if (i < 5) {
            char dbg_buf[32];
            int dbg_len = dll.token_to_piece(m_model, new_token, dbg_buf, sizeof(dbg_buf), 0, true);
            std::string dbg_text(dbg_len > 0 ? std::string(dbg_buf, dbg_len) : std::string("(empty)"));
            std::cerr << "[LLM] Token[" << i << "] id=" << new_token << " text=\"" << dbg_text << "\"" << std::endl;
        }

        if (new_token == eos_token) {
            std::cerr << "[LLM] EOS token generated at position " << i << " — stopping" << std::endl;
            break;
        }

        // Also break on <|im_end|> token (ChatML turn-ending marker)
        if (im_end_token >= 0 && new_token == im_end_token) {
            std::cerr << "[LLM] im_end token generated at position " << i << " — stopping" << std::endl;
            break;
        }

        char piece_buf[32];
        int piece_len = dll.token_to_piece(m_model, new_token, piece_buf, sizeof(piece_buf), 0, false);
        if (piece_len > 0) {
            response.append(piece_buf, piece_len);
        }

        dll.sampler_accept(m_sampler, new_token);

        n_decoded++;

        ret = dll.decode(ctx, dll.batch_get_one(&new_token, 1));
        if (ret != 0) {
            std::cerr << "[LLM] Decode failed at token " << i << ", ret=" << ret << " — stopping" << std::endl;
            break;
        }
    }

    std::cerr << "[LLM] Generate done: n_decoded=" << n_decoded << std::endl;
    result.completion_tokens = n_decoded;
    
    std::string clean_response = response;
    
    std::vector<std::string> stop_tokens = {
        "<|im_end|>", "||im_end|>", "<|im_start|>", "||im_start|>",
        "<|endoftext|>", "<|end|>", "[/INST]", "</s>"
    };
    for (const auto& stop : stop_tokens) {
        size_t pos = clean_response.find(stop);
        if (pos != std::string::npos) {
            clean_response = clean_response.substr(0, pos);
        }
    }
    
    size_t human_pos = clean_response.find("Human:");
    if (human_pos != std::string::npos) {
        clean_response = clean_response.substr(0, human_pos);
    }
    
    while (!clean_response.empty() && (clean_response.back() == '\n' || clean_response.back() == ' ' || clean_response.back() == '\r')) {
        clean_response.pop_back();
    }
    while (!clean_response.empty() && (clean_response.front() == '\n' || clean_response.front() == ' ' || clean_response.front() == '\r')) {
        clean_response.erase(clean_response.begin());
    }
    
    result.text = clean_response;
    result.success = true;

    dll.free(ctx);

    return result;
}

void LLMInference::GenerateAsync(const std::string& prompt,
                                  std::function<void(GenerationResult)> callback,
                                  const std::string& system_prompt) {
    std::thread([this, prompt, callback, system_prompt]() {
        auto result = Generate(prompt, system_prompt);
        callback(result);
    }).detach();
}

LLMInference::GenerationResult LLMInference::GenerateStream(const std::string& prompt,
    std::function<void(const std::string& token, int index)> on_token,
    const std::string& system_prompt) {
    GenerationResult result;
    result.success = false;
    result.prompt_tokens = 0;
    result.completion_tokens = 0;

    if (!initialized || !m_model) {
        result.error = "LLM not initialized";
        return result;
    }

    std::lock_guard<std::mutex> lock(inference_mutex);

    auto& dll = LLamaDLL::Instance();

    int actual_n_ctx = 0;
    llama_context* ctx = CreateContextAdaptive(dll, m_model, config.n_ctx, config.n_threads, actual_n_ctx);
    if (!ctx) {
        result.error = "Failed to create context (tried n_ctx from " + std::to_string(config.n_ctx) + " down to 2048)";
        std::cerr << "[LLM] ERROR: " << result.error << std::endl;
        return result;
    }

    // Clear KV cache from any previous context (safety measure)
    dll.kv_cache_clear(ctx);

    std::string full_prompt = BuildChatPrompt(prompt, system_prompt);

    // Two-pass tokenization: use a large buffer first, then retry if needed.
    // special=true: parse ChatML markers (<|im_start|>, <|im_end|>) as special tokens.
    // This is CRITICAL — with special=false, the model doesn't recognize ChatML format
    // and may generate <|im_end|> immediately, producing empty output.
    size_t tok_buf_size = std::max((size_t)actual_n_ctx, (size_t)8192);
    std::vector<llama_token> tokens(tok_buf_size);
    int n_tokens = dll.tokenize(m_model, full_prompt.c_str(), (int32_t)full_prompt.size(),
                                tokens.data(), (int32_t)tokens.size(), true, true);
    if (n_tokens < 0) {
        // Buffer too small — resize to required size and retry
        size_t required = static_cast<size_t>(-n_tokens) + 16;
        tokens.resize(required);
        n_tokens = dll.tokenize(m_model, full_prompt.c_str(), (int32_t)full_prompt.size(),
                                tokens.data(), (int32_t)tokens.size(), true, true);
        if (n_tokens < 0) {
            result.error = "Tokenization failed (required=" + std::to_string(-n_tokens) + ")";
            dll.free(ctx);
            return result;
        }
        std::cerr << "[LLM] Tokenization buffer resized to " << required << " tokens (n_ctx=" << actual_n_ctx << ")" << std::endl;
    }
    tokens.resize(n_tokens);
    result.prompt_tokens = n_tokens;

    // Truncate to n_ctx if prompt exceeds context window
    if (n_tokens > actual_n_ctx) {
        std::cerr << "[LLM] Prompt tokens (" << n_tokens << ") exceed n_ctx (" << actual_n_ctx << ") — truncating" << std::endl;
        tokens.resize(actual_n_ctx);
        n_tokens = actual_n_ctx;
    }

    llama_token eos_token = dll.token_eos(m_model);

    // Also detect <|im_end|> token — for Qwen2.5, EOS may be <|endoftext|> while
    // <|im_end|> is the actual ChatML turn-ending marker. Without this check,
    // the model would continue generating after <|im_end|>.
    // Get <|im_end|> token ID by tokenizing the string with special=true.
    llama_token im_end_token = -1;
    {
        llama_token tmp_tokens[8];
        int n = dll.tokenize(m_model, "<|im_end|>", 10, tmp_tokens, 8, false, true);
        if (n == 1) im_end_token = tmp_tokens[0];
    }

    std::cerr << "[LLM] GenerateStream: prompt_tokens=" << n_tokens
              << " n_ctx=" << actual_n_ctx
              << " eos_id=" << eos_token
              << " im_end_id=" << im_end_token << std::endl;

    int32_t ret = dll.decode(ctx, dll.batch_get_one(tokens.data(), n_tokens));
    if (ret != 0) {
        result.error = "Initial decode failed, ret=" + std::to_string(ret);
        std::cerr << "[LLM] ERROR: " << result.error << std::endl;
        dll.free(ctx);
        return result;
    }

    std::string response;
    int n_decoded = 0;

    // n_predict < 0 means unlimited (generate until EOS or context full)
    for (int i = 0; config.n_predict < 0 || i < config.n_predict; i++) {
        llama_token new_token = dll.sampler_sample(m_sampler, ctx, -1);

        // Log first 5 tokens for debugging
        if (i < 5) {
            char dbg_buf[32];
            int dbg_len = dll.token_to_piece(m_model, new_token, dbg_buf, sizeof(dbg_buf), 0, true);
            std::string dbg_text(dbg_len > 0 ? std::string(dbg_buf, dbg_len) : std::string("(empty)"));
            std::cerr << "[LLM] StreamToken[" << i << "] id=" << new_token << " text=\"" << dbg_text << "\"" << std::endl;
        }

        if (new_token == eos_token) {
            std::cerr << "[LLM] EOS token generated at position " << i << " — stopping" << std::endl;
            break;
        }

        // Also break on <|im_end|> token (ChatML turn-ending marker)
        if (im_end_token >= 0 && new_token == im_end_token) {
            std::cerr << "[LLM] im_end token generated at position " << i << " — stopping" << std::endl;
            break;
        }

        char piece_buf[32];
        int piece_len = dll.token_to_piece(m_model, new_token, piece_buf, sizeof(piece_buf), 0, false);
        if (piece_len > 0) {
            std::string token_text(piece_buf, piece_len);
            response.append(token_text);
            if (on_token) {
                on_token(token_text, n_decoded);
            }
        }

        dll.sampler_accept(m_sampler, new_token);

        n_decoded++;

        ret = dll.decode(ctx, dll.batch_get_one(&new_token, 1));
        if (ret != 0) {
            std::cerr << "[LLM] Decode failed at token " << i << ", ret=" << ret << " — stopping" << std::endl;
            break;
        }
    }

    std::cerr << "[LLM] GenerateStream done: n_decoded=" << n_decoded << std::endl;
    result.completion_tokens = n_decoded;

    std::string clean_response = response;

    std::vector<std::string> stop_tokens = {
        "<|im_end|>", "||im_end|>", "<|im_start|>", "||im_start|>",
        "<|endoftext|>", "<|end|>", "[/INST]", "</s>"
    };
    for (const auto& stop : stop_tokens) {
        size_t pos = clean_response.find(stop);
        if (pos != std::string::npos) {
            clean_response = clean_response.substr(0, pos);
        }
    }

    size_t human_pos = clean_response.find("Human:");
    if (human_pos != std::string::npos) {
        clean_response = clean_response.substr(0, human_pos);
    }

    while (!clean_response.empty() && (clean_response.back() == '\n' || clean_response.back() == ' ' || clean_response.back() == '\r')) {
        clean_response.pop_back();
    }
    while (!clean_response.empty() && (clean_response.front() == '\n' || clean_response.front() == ' ' || clean_response.front() == '\r')) {
        clean_response.erase(clean_response.begin());
    }

    result.text = clean_response;
    result.success = true;

    dll.free(ctx);

    return result;
}
