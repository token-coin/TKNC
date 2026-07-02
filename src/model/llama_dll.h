#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <iostream>

// CRITICAL: Structs must match MSVC layout exactly (pragma pack) — passed to/from llama.dll (MSVC-compiled).
// UPDATED: Matched to llama.cpp master (2025-06) used by Ollama
// ============================================================

#ifdef _WIN32
// Force MSVC-compatible struct packing for ABI compatibility
#pragma pack(push, 8)
#endif

// Forward declarations for opaque pointers (loaded from DLL)
struct llama_model {};
struct llama_context {};
struct llama_sampler {};
struct llama_vocab {};

// Token type
typedef int32_t llama_token;
typedef int32_t llama_pos;
typedef int32_t llama_seq_id;

// ============================================================
// Enum definitions matching llama.cpp master (2025-06)
// ============================================================

enum llama_split_mode {
    LLAMA_SPLIT_MODE_NONE = 0,
    LLAMA_SPLIT_MODE_LAYER = 1,
    LLAMA_SPLIT_MODE_ROW = 2,
    LLAMA_SPLIT_MODE_TENSOR = 3,
};

enum llama_rope_scaling_type {
    LLAMA_ROPE_SCALING_TYPE_UNSPECIFIED = -1,
    LLAMA_ROPE_SCALING_TYPE_NONE = 0,
    LLAMA_ROPE_SCALING_TYPE_LINEAR = 1,
    LLAMA_ROPE_SCALING_TYPE_YARN = 2,
    LLAMA_ROPE_SCALING_TYPE_LONGROPE = 3,
};

enum llama_pooling_type {
    LLAMA_POOLING_TYPE_UNSPECIFIED = -1,
    LLAMA_POOLING_TYPE_NONE = 0,
    LLAMA_POOLING_TYPE_MEAN = 1,
    LLAMA_POOLING_TYPE_CLS = 2,
    LLAMA_POOLING_TYPE_LAST = 3,
    LLAMA_POOLING_TYPE_RANK = 4,
};

enum llama_attention_type {
    LLAMA_ATTENTION_TYPE_UNSPECIFIED = -1,
    LLAMA_ATTENTION_TYPE_CAUSAL = 0,
    LLAMA_ATTENTION_TYPE_NON_CAUSAL = 1,
};

enum llama_flash_attn_type {
    LLAMA_FLASH_ATTENTION_TYPE_AUTO = -1,
    LLAMA_FLASH_ATTENTION_TYPE_DISABLED = 0,
    LLAMA_FLASH_ATTENTION_TYPE_ENABLED = 1,
};

// ============================================================
// COMPLETE struct definitions matching llama.cpp master (2025-06)
// Using MSVC-default packing (8-byte alignment)
// ============================================================

// Model parameters - FULL definition matching latest llama.dll layout
// Source: https://github.com/ggml-org/llama.cpp (master branch, 2025-06)
typedef struct {
    void* devices;                          // ggml_backend_dev_t* - NULL-terminated list
    void* tensor_buft_overrides;            // const struct llama_model_tensor_buft_override*
    int32_t n_gpu_layers;                   // number of layers to store in VRAM
    int32_t split_mode;                     // enum llama_split_mode
    int32_t main_gpu;                       // GPU used when split_mode is NONE
    int32_t _pad1;                          // padding to 8-byte align tensor_split
    const float* tensor_split;              // pointer to split proportions (was inline array)
    void* progress_callback;                // llama_progress_callback
    void* progress_callback_user_data;      // context for progress callback
    void* kv_overrides;                     // const struct llama_model_kv_override*
    bool vocab_only;                        // only load vocabulary
    bool use_mmap;                          // use mmap if possible
    bool use_direct_io;                     // use direct io
    bool use_mlock;                         // force system to keep model in RAM
    bool check_tensors;                     // validate model tensor data
    bool use_extra_bufts;                   // use extra buffer types
    bool no_host;                           // bypass host buffer
    bool no_alloc;                          // only load metadata
} llama_model_params;

// Context parameters - FULL definition matching latest llama.dll layout
// CRITICAL SIZE NOTE: Probed DLL's llama_context_default_params() memcpy size = 160 bytes (0xa0).
// Adding 16 bytes of trailing padding to prevent stack overflow when the DLL writes its
// default 160-byte struct into our local. The DLL's actual field layout may differ from ours
// for fields after n_threads_batch, but we only modify n_ctx, n_threads, n_threads_batch
// (which DO match the DLL's offsets 0, 16, 20). All other fields use the DLL's defaults as-is.
typedef struct {
    uint32_t n_ctx;                         // text context size (DLL offset 0)
    uint32_t n_batch;                       // logical max batch size (DLL offset 4)
    uint32_t n_ubatch;                      // physical max batch size (DLL offset 8)
    uint32_t n_seq_max;                     // max number of sequences (DLL offset 12)
    int32_t n_threads;                      // threads for generation (DLL offset 16)
    int32_t n_threads_batch;                // threads for batch processing (DLL offset 20)
    int32_t rope_scaling_type;              // enum llama_rope_scaling_type (DLL offset 24)
    int32_t pooling_type;                   // enum llama_pooling_type (DLL offset 28)
    int32_t attention_type;                 // enum llama_attention_type (DLL offset 32)
    int32_t flash_attn_type;                // enum llama_flash_attn_type (DLL offset 36)
    float rope_freq_base;                   // RoPE base frequency
    float rope_freq_scale;                  // RoPE frequency scaling
    float yarn_ext_factor;                  // YaRN extrapolation mix
    float yarn_attn_factor;                 // YaRN magnitude factor
    float yarn_beta_fast;                   // YaRN low correction dim
    float yarn_beta_slow;                   // YaRN high correction dim
    uint32_t yarn_orig_ctx;                 // YaRN original context size
    float defrag_thold;                     // KV cache defrag threshold (deprecated)
    void* cb_eval;                          // ggml_backend_sched_eval_callback
    void* cb_eval_user_data;                // user data for cb_eval
    int32_t type_k;                         // enum ggml_type for K cache
    int32_t type_v;                         // enum ggml_type for V cache
    void* abort_callback;                   // ggml_abort_callback
    void* abort_callback_data;              // user data for abort callback
    bool embeddings;                        // extract embeddings
    bool offload_kqv;                       // offload KQV ops to GPU
    bool no_perf;                           // measure performance timings
    bool op_offload;                        // offload host tensor ops
    bool swa_full;                          // use full-size SWA cache
    bool kv_unified;                        // unified buffer for attention
    int32_t _pad2;                          // padding to 8-byte align samplers
    void* samplers;                         // struct llama_sampler_seq_config*
    size_t n_samplers;                      // number of sampler chains
    // [CRITICAL PADDING] OLLAMA's llama.dll has 16 extra bytes in the struct.
    // Without this padding, the DLL's default_params() memcpy of 160 bytes overflows
    // our 144-byte stack allocation, corrupting the canary and producing
    // "*** stack smashing detected ***" at the end of llama_init_from_model.
    uint8_t _dll_extension_padding[16];     // DLL has 16 extra bytes (offsets 144-159)
} llama_context_params;

// Compile-time assertion: ensure struct is exactly 160 bytes to match DLL's memcpy size
static_assert(sizeof(llama_context_params) == 160,
    "llama_context_params must be 160 bytes to match OLLAMA's llama.dll memcpy size. "
    "If struct size changes, probe DLL's llama_context_default_params() to confirm new size.");

// Batch structure - FULL definition (contains pointers!)
// UPDATED: added embd field for latest llama.cpp
typedef struct {
    int32_t n_tokens;                       // number of tokens
    llama_token* token;                     // token pointers
    float* embd;                            // token embeddings (new in latest)
    llama_pos* pos;                         // position pointers
    int32_t* n_seq_id;                      // number of sequence ids per token
    llama_seq_id** seq_id;                  // sequence id pointers per token
    int8_t* logits;                         // logit output flags per position
} llama_batch;

// Sampler chain parameters (kept simple - likely unchanged in DLL)
typedef struct {
    int32_t no_perf;                        // disable performance checks
} llama_sampler_chain_params;

// Log levels (matching ggml definition)
enum ggml_log_level {
    GGML_LOG_LEVEL_ERROR = 2,
    GGML_LOG_LEVEL_WARN  = 3,
    GGML_LOG_LEVEL_INFO  = 4,
    GGML_LOG_LEVEL_DEBUG = 5
};

// Log callback type
typedef void (*ggml_log_callback)(enum ggml_log_level level, const char* text, void* user_data);

#ifdef _WIN32
#pragma pack(pop)
#endif

class LLamaDLL {
public:
    static LLamaDLL& Instance();

    bool Load(const std::string& dll_path);
    bool IsLoaded() const;
    void Unload();

    llama_model_params model_default_params();
    llama_model* model_load_from_file(const char* path, llama_model_params params);
    void model_free(llama_model* model);
    llama_context* new_context_with_model_default(llama_model* model, int32_t n_ctx = 2048, int32_t n_threads = 4);
    void free(llama_context* ctx);

    int32_t n_ctx(const llama_context* ctx);
    int32_t n_vocab(const llama_model* model);

    llama_token token_bos(const llama_model* model);
    llama_token token_eos(const llama_model* model);

    int32_t tokenize(
        const llama_model* model,
        const char* text,
        int32_t text_len,
        llama_token* tokens,
        int32_t n_max_tokens,
        bool add_bos,
        bool special
    );

    int32_t token_to_piece(
        const llama_model* model,
        llama_token token,
        char* buf,
        int32_t length,
        int32_t lstrip,
        bool special
    );

    llama_batch batch_get_one(llama_token* tokens, int32_t n_tokens);
    void batch_free(llama_batch batch);

    int32_t decode(llama_context* ctx, llama_batch batch);

    float* get_logits(llama_context* ctx);

    llama_sampler* sampler_chain_init(llama_sampler_chain_params params);
    void sampler_chain_add(llama_sampler* chain, llama_sampler* sampler);
    void sampler_free(llama_sampler* sampler);
    void sampler_accept(llama_sampler* sampler, llama_token token);
    llama_token sampler_sample(llama_sampler* sampler, llama_context* ctx, int32_t idx);

    llama_sampler* sampler_init_temp(float temp);
    llama_sampler* sampler_init_top_k(int32_t k);
    llama_sampler* sampler_init_top_p(float p, size_t min_keep);
    llama_sampler* sampler_init_dist(uint32_t seed);
    llama_sampler* sampler_init_greedy();
    llama_sampler* sampler_init_penalties(int32_t penalty_last_n, float penalty_repeat, float penalty_freq, float penalty_present);

    void kv_cache_clear(llama_context* ctx);

    const char* model_desc(llama_model* model, char* buf, size_t size);

    // GPU device query — returns first non-CPU (GPU) backend device, or nullptr if none
    void* GetGPUDevice();

    // Multi-GPU: get all dGPU devices for multi-GPU offload
    struct GPUDevInfo {
        void*  handle;       // ggml_backend_dev_t
        int    dev_type;     // 0=CPU, 1=dGPU, 2=iGPU, 3=ACCEL, 4=META
        char   name[128];    // device name
        char   desc[256];    // device description
        size_t vram_total;   // total VRAM in bytes
        size_t vram_free;    // free VRAM in bytes
    };

    const std::vector<GPUDevInfo>& GetAllGPUDevices() const { return m_gpu_devices; }
    int GetGPUDeviceCount() const { return (int)m_gpu_devices.size(); }

    // Check if model has layers offloaded to GPU (call after model_load_from_file)
    bool IsModelOnGPU(llama_model* model);

    // Log functions
    void log_set(ggml_log_callback callback, void* user_data);

private:
    LLamaDLL();
    ~LLamaDLL() = default;
    LLamaDLL(const LLamaDLL&) = delete;
    LLamaDLL& operator=(const LLamaDLL&) = delete;

    bool m_loaded;
    void* m_gpu_device;  // Cached GPU backend device pointer (set during Load())
    std::vector<GPUDevInfo> m_gpu_devices;  // All dGPU devices for multi-GPU offload
};
