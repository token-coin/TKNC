#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif

#include "llama_dll.h"
#include <util/fs_helpers.h>
#include <iostream>
#include <cstring>
#include <algorithm>

// Log macros (simplified for compilation)
#define LogError(fmt, ...) do { fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LogWarning(fmt, ...) do { fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LogInfo(fmt, ...) do { fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__); } while(0)

// fs namespace already defined in fs_helpers.h

// ============================================================
// Function pointer types for dynamic library loading
// These types are used for both Windows (GetProcAddress) and Unix (dlsym)
// ============================================================

// Log callback types
typedef void (*fn_llama_log_set)(ggml_log_callback, void*);
typedef void (*fn_llama_log_get)(ggml_log_callback*, void**);

// Model functions
typedef llama_model_params (*fn_llama_model_default_params)(void);
typedef llama_model* (*fn_llama_model_load_from_file)(const char*, llama_model_params);
typedef void (*fn_llama_model_free)(llama_model*);
typedef const char* (*fn_llama_model_desc)(const llama_model*, char*, size_t);
typedef int32_t (*fn_llama_model_n_gpu_layers)(const llama_model*);

// Context functions
typedef llama_context_params (*fn_llama_context_default_params)(void);
typedef llama_context* (*fn_llama_init_from_model)(const llama_model*, llama_context_params);
typedef void (*fn_llama_free)(llama_context*);
typedef int32_t (*fn_llama_n_ctx)(const llama_context*);
typedef int32_t (*fn_llama_n_batch)(const llama_context*);

// Vocab functions
typedef const llama_vocab* (*fn_llama_model_get_vocab)(const llama_model*);
typedef int32_t (*fn_llama_vocab_n_tokens)(const llama_vocab*);
typedef llama_token (*fn_llama_vocab_bos)(const llama_vocab*);
typedef llama_token (*fn_llama_vocab_eos)(const llama_vocab*);
typedef int32_t (*fn_llama_tokenize)(const llama_vocab*, const char*, int, llama_token*, int, bool, bool);
typedef int32_t (*fn_llama_token_to_piece)(const llama_vocab*, llama_token, char*, int, int, bool);

// Batch functions
typedef llama_batch (*fn_llama_batch_get_one)(llama_token*, int32_t);
typedef void (*fn_llama_batch_free)(llama_batch);
typedef int32_t (*fn_llama_decode)(llama_context*, llama_batch);
typedef float* (*fn_llama_get_logits)(llama_context*);

// Sampler functions
typedef llama_sampler* (*fn_llama_sampler_chain_init)(llama_sampler_chain_params);
typedef void (*fn_llama_sampler_chain_add)(llama_sampler*, llama_sampler*);
typedef void (*fn_llama_sampler_free)(llama_sampler*);
typedef void (*fn_llama_sampler_accept)(llama_sampler*, llama_token);
typedef llama_token (*fn_llama_sampler_sample)(llama_sampler*, llama_context*, int32_t);
typedef llama_sampler* (*fn_llama_sampler_init_temp)(float);
typedef llama_sampler* (*fn_llama_sampler_init_top_k)(int32_t);
typedef llama_sampler* (*fn_llama_sampler_init_top_p)(float, size_t);
typedef llama_sampler* (*fn_llama_sampler_init_dist)(uint32_t);
typedef llama_sampler* (*fn_llama_sampler_init_greedy)(void);
typedef llama_sampler* (*fn_llama_sampler_init_penalties)(int32_t, float, float, float);

// KV cache
typedef void (*fn_llama_kv_cache_clear)(llama_context*);

// Backend functions
typedef void (*fn_ggml_backend_load_all)(void);
typedef void (*fn_ggml_backend_load_all_from_path)(const char*);
typedef void (*fn_llama_backend_init)(void);
typedef void* (*fn_ggml_backend_cpu_reg)(void);
typedef int (*fn_ggml_backend_reg_count)(void);

// GPU backend functions
typedef void (*fn_ggml_backend_register)(void* reg);
typedef void* (*fn_ggml_backend_dev_by_type)(int dev_type);
typedef size_t (*fn_ggml_backend_dev_count)(void);
typedef void* (*fn_ggml_backend_dev_get)(size_t index);
typedef const char* (*fn_ggml_backend_dev_name)(void* dev);
typedef const char* (*fn_ggml_backend_dev_description)(void* dev);
typedef int (*fn_ggml_backend_dev_type_fn)(void* dev);
typedef void (*fn_ggml_backend_dev_memory)(void* dev, size_t* free, size_t* total);

#ifdef _WIN32
#include <windows.h>

// Static function pointers (loaded from DLL)
static HMODULE s_llama_dll = nullptr;
static fn_llama_log_set pfn_llama_log_set = nullptr;
static fn_ggml_backend_load_all pfn_ggml_backend_load_all = nullptr;
static fn_llama_model_default_params pfn_llama_model_default_params = nullptr;
static fn_llama_model_load_from_file pfn_llama_model_load_from_file = nullptr;
static fn_llama_model_free pfn_llama_model_free = nullptr;
static fn_llama_model_desc pfn_llama_model_desc = nullptr;
static fn_llama_model_n_gpu_layers pfn_llama_model_n_gpu_layers = nullptr;
static fn_llama_model_get_vocab pfn_llama_model_get_vocab = nullptr;
static fn_llama_context_default_params pfn_llama_context_default_params = nullptr;
static fn_llama_init_from_model pfn_llama_init_from_model = nullptr;
static fn_llama_free pfn_llama_free = nullptr;
static fn_llama_n_ctx pfn_llama_n_ctx = nullptr;
static fn_llama_n_batch pfn_llama_n_batch = nullptr;
static fn_llama_vocab_n_tokens pfn_llama_vocab_n_tokens = nullptr;
static fn_llama_vocab_bos pfn_llama_vocab_bos = nullptr;
static fn_llama_vocab_eos pfn_llama_vocab_eos = nullptr;
static fn_llama_tokenize pfn_llama_tokenize = nullptr;
static fn_llama_token_to_piece pfn_llama_token_to_piece = nullptr;
static fn_llama_batch_get_one pfn_llama_batch_get_one = nullptr;
static fn_llama_batch_free pfn_llama_batch_free = nullptr;
static fn_llama_decode pfn_llama_decode = nullptr;
static fn_llama_get_logits pfn_llama_get_logits = nullptr;
static fn_llama_sampler_chain_init pfn_llama_sampler_chain_init = nullptr;
static fn_llama_sampler_chain_add pfn_llama_sampler_chain_add = nullptr;
static fn_llama_sampler_free pfn_llama_sampler_free = nullptr;
static fn_llama_sampler_accept pfn_llama_sampler_accept = nullptr;
static fn_llama_sampler_sample pfn_llama_sampler_sample = nullptr;
static fn_llama_sampler_init_temp pfn_llama_sampler_init_temp = nullptr;
static fn_llama_sampler_init_top_k pfn_llama_sampler_init_top_k = nullptr;
static fn_llama_sampler_init_top_p pfn_llama_sampler_init_top_p = nullptr;
static fn_llama_sampler_init_dist pfn_llama_sampler_init_dist = nullptr;
static fn_llama_sampler_init_greedy pfn_llama_sampler_init_greedy = nullptr;
static fn_llama_sampler_init_penalties pfn_llama_sampler_init_penalties = nullptr;
static fn_llama_kv_cache_clear pfn_llama_kv_cache_clear = nullptr;
static fn_llama_backend_init pfn_llama_backend_init = nullptr;
static fn_ggml_backend_load_all_from_path pfn_ggml_backend_load_all_from_path = nullptr;
static fn_ggml_backend_reg_count pfn_ggml_backend_reg_count = nullptr;
static fn_ggml_backend_register pfn_ggml_backend_register = nullptr;
static fn_ggml_backend_dev_by_type pfn_ggml_backend_dev_by_type = nullptr;

// llama.dll's own ggml backend registry functions (CRITICAL for dual-registry fix)
static fn_ggml_backend_register pfn_llama_ggml_backend_register = nullptr;
static fn_ggml_backend_dev_by_type pfn_llama_ggml_backend_dev_by_type = nullptr;
static fn_ggml_backend_reg_count pfn_llama_ggml_backend_reg_count = nullptr;

// Multi-GPU enumeration function pointers (loaded from llama.dll)
static fn_ggml_backend_dev_count pfn_llama_ggml_backend_dev_count = nullptr;
static fn_ggml_backend_dev_get pfn_llama_ggml_backend_dev_get = nullptr;
static fn_ggml_backend_dev_name pfn_llama_ggml_backend_dev_name = nullptr;
static fn_ggml_backend_dev_description pfn_llama_ggml_backend_dev_description = nullptr;
static fn_ggml_backend_dev_type_fn pfn_llama_ggml_backend_dev_type_fn = nullptr;
static fn_ggml_backend_dev_memory pfn_llama_ggml_backend_dev_memory = nullptr;

// LAYER 1: ggml.dll handle for backend functions
static HMODULE s_ggml_dll = nullptr;

// Macro to load function
#define LOAD_FUNC(name) \
 pfn_##name = (fn_##name)GetProcAddress(s_llama_dll, #name); \
 if (!pfn_##name) { \
 LogError("Failed to load " #name " from llama.dll"); \
 return false; \
 }

static bool LoadLLamaFunctions(const std::string& dll_path) {
 LOAD_FUNC(llama_log_set);
 // ggml_backend_load_all is in ggml.dll, loaded separately below
 LOAD_FUNC(llama_model_default_params);
 LOAD_FUNC(llama_model_load_from_file);
 LOAD_FUNC(llama_model_free);
 LOAD_FUNC(llama_model_desc);
 // llama_model_n_gpu_layers may not exist in all llama.dll versions (e.g., Ollama builds)
 pfn_llama_model_n_gpu_layers = (fn_llama_model_n_gpu_layers)GetProcAddress(s_llama_dll, "llama_model_n_gpu_layers");
 if (!pfn_llama_model_n_gpu_layers) {
 LogInfo("llama_model_n_gpu_layers not found (non-fatal, GPU layer check unavailable)");
 }
 LOAD_FUNC(llama_model_get_vocab);
 LOAD_FUNC(llama_context_default_params);
 LOAD_FUNC(llama_init_from_model);
 LOAD_FUNC(llama_free);
 LOAD_FUNC(llama_n_ctx);
 LOAD_FUNC(llama_n_batch);
 LOAD_FUNC(llama_vocab_n_tokens);
 LOAD_FUNC(llama_vocab_bos);
 LOAD_FUNC(llama_vocab_eos);
 LOAD_FUNC(llama_tokenize);
 LOAD_FUNC(llama_token_to_piece);
 LOAD_FUNC(llama_batch_get_one);
 LOAD_FUNC(llama_batch_free);
 LOAD_FUNC(llama_decode);
 LOAD_FUNC(llama_get_logits);
 LOAD_FUNC(llama_sampler_chain_init);
 LOAD_FUNC(llama_sampler_chain_add);
 LOAD_FUNC(llama_sampler_free);
 LOAD_FUNC(llama_sampler_accept);
 LOAD_FUNC(llama_sampler_sample);
 LOAD_FUNC(llama_sampler_init_temp);
 LOAD_FUNC(llama_sampler_init_top_k);
 LOAD_FUNC(llama_sampler_init_top_p);
 LOAD_FUNC(llama_sampler_init_dist);
 LOAD_FUNC(llama_sampler_init_greedy);
 LOAD_FUNC(llama_sampler_init_penalties);
 // llama_kv_cache_clear may not exist in all llama.dll versions - make optional
 pfn_llama_kv_cache_clear = (fn_llama_kv_cache_clear)GetProcAddress(s_llama_dll, "llama_kv_cache_clear");
 if (!pfn_llama_kv_cache_clear) {
 LogInfo("llama_kv_cache_clear not found (non-fatal, using fallback)");
 }

 // llama_backend_init may not exist in all llama.dll versions - make optional (needed for newer llama.cpp)
 pfn_llama_backend_init = (fn_llama_backend_init)GetProcAddress(s_llama_dll, "llama_backend_init");
 if (!pfn_llama_backend_init) {
 // Retry by ordinal 28 (observed in objdump for this llama.dll)
 pfn_llama_backend_init = (fn_llama_backend_init)GetProcAddress(s_llama_dll, (LPCSTR)28);
 if (pfn_llama_backend_init) {
 LogInfo("llama_backend_init loaded by ordinal 28 (GetProcAddress by name failed, err=%lu)", GetLastError());
 } else {
 LogInfo("llama_backend_init not found by name or ordinal 28 (non-fatal, older llama.dll)");
 }
 } else {
 LogInfo("Loaded llama_backend_init from llama.dll");
 }

 // Load llama.dll's own ggml backend registry functions (CRITICAL for dual-registry fix)
 // llama.dll statically links ggml-backend-reg.cpp, so it has its own global registry.
 // ggml.dll also has its own. Registering in ggml.dll's registry does NOT affect llama.dll.
 // We MUST use llama.dll's ggml_backend_register to register CPU in llama.dll's registry.
 pfn_llama_ggml_backend_register = (fn_ggml_backend_register)GetProcAddress(s_llama_dll, "ggml_backend_register");
 if (pfn_llama_ggml_backend_register) {
 LogInfo("Loaded ggml_backend_register from llama.dll (for llama.dll's own registry)");
 }
 pfn_llama_ggml_backend_dev_by_type = (fn_ggml_backend_dev_by_type)GetProcAddress(s_llama_dll, "ggml_backend_dev_by_type");
 if (pfn_llama_ggml_backend_dev_by_type) {
 LogInfo("Loaded ggml_backend_dev_by_type from llama.dll");
 }
 pfn_llama_ggml_backend_reg_count = (fn_ggml_backend_reg_count)GetProcAddress(s_llama_dll, "ggml_backend_reg_count");
 if (pfn_llama_ggml_backend_reg_count) {
 LogInfo("Loaded ggml_backend_reg_count from llama.dll");
 }

 // (Chinese comment removed)
 pfn_llama_ggml_backend_dev_count = (fn_ggml_backend_dev_count)GetProcAddress(s_llama_dll, "ggml_backend_dev_count");
 pfn_llama_ggml_backend_dev_get = (fn_ggml_backend_dev_get)GetProcAddress(s_llama_dll, "ggml_backend_dev_get");
 pfn_llama_ggml_backend_dev_name = (fn_ggml_backend_dev_name)GetProcAddress(s_llama_dll, "ggml_backend_dev_name");
 pfn_llama_ggml_backend_dev_description = (fn_ggml_backend_dev_description)GetProcAddress(s_llama_dll, "ggml_backend_dev_description");
 pfn_llama_ggml_backend_dev_type_fn = (fn_ggml_backend_dev_type_fn)GetProcAddress(s_llama_dll, "ggml_backend_dev_type");
 pfn_llama_ggml_backend_dev_memory = (fn_ggml_backend_dev_memory)GetProcAddress(s_llama_dll, "ggml_backend_dev_memory");
 if (pfn_llama_ggml_backend_dev_count && pfn_llama_ggml_backend_dev_get) {
 LogInfo("Multi-GPU enumeration APIs loaded from llama.dll (dev_count, dev_get, dev_name=%d, dev_desc=%d, dev_type=%d, dev_memory=%d)",
 pfn_llama_ggml_backend_dev_name ? 1 : 0,
 pfn_llama_ggml_backend_dev_description ? 1 : 0,
 pfn_llama_ggml_backend_dev_type_fn ? 1 : 0,
 pfn_llama_ggml_backend_dev_memory ? 1 : 0);
 } else {
 LogInfo("Multi-GPU enumeration APIs not available in llama.dll (will use dev_by_type fallback)");
 }

 // Load ggml_backend_load_all from the right DLL
 // Priority: llama.dll > ggml-base.dll > ggml.dll
 // llama.dll may re-export ggml_backend_load_all; ggml-base.dll (807KB) is the real backend
 {
 std::string base_dir = dll_path;
 size_t pos = base_dir.rfind('\\');
 if (pos != std::string::npos) {
 base_dir = base_dir.substr(0, pos + 1);
 }

 // Try 1: llama.dll itself (may re-export ggml_backend_load_all)
 pfn_ggml_backend_load_all = (fn_ggml_backend_load_all)GetProcAddress(s_llama_dll, "ggml_backend_load_all");
 if (pfn_ggml_backend_load_all) {
 LogInfo("Loaded ggml_backend_load_all from llama.dll");
 } else {
 // Try 2: ggml-base.dll (actual backend library, 807KB)
 std::string ggml_base_path = base_dir + "ggml-base.dll";
 HMODULE h_ggml_base = LoadLibraryA(ggml_base_path.c_str());
 if (h_ggml_base) {
 LogInfo("Loaded ggml-base.dll from: %s", ggml_base_path.c_str());
 pfn_ggml_backend_load_all = (fn_ggml_backend_load_all)GetProcAddress(h_ggml_base, "ggml_backend_load_all");
 if (pfn_ggml_backend_load_all) {
 LogInfo("Loaded ggml_backend_load_all from ggml-base.dll");
 }
 }

 // Try 3: ggml.dll (fallback, may be a stub)
 if (!pfn_ggml_backend_load_all) {
 std::string ggml_path = base_dir + "ggml.dll";
 s_ggml_dll = LoadLibraryA(ggml_path.c_str());
 if (s_ggml_dll) {
 LogInfo("Loaded ggml.dll from: %s", ggml_path.c_str());
 pfn_ggml_backend_load_all = (fn_ggml_backend_load_all)GetProcAddress(s_ggml_dll, "ggml_backend_load_all");
 if (pfn_ggml_backend_load_all) {
 LogInfo("Loaded ggml_backend_load_all from ggml.dll");
 } else {
 LogInfo("ggml_backend_load_all not found in ggml.dll (non-fatal)");
 }
 // Load path-based backend loader (preferred over ggml_backend_load_all)
 pfn_ggml_backend_load_all_from_path = (fn_ggml_backend_load_all_from_path)GetProcAddress(s_ggml_dll, "ggml_backend_load_all_from_path");
 if (pfn_ggml_backend_load_all_from_path) {
 LogInfo("Loaded ggml_backend_load_all_from_path from ggml.dll");
 }
 // Load diagnostic function
 pfn_ggml_backend_reg_count = (fn_ggml_backend_reg_count)GetProcAddress(s_ggml_dll, "ggml_backend_reg_count");
 if (pfn_ggml_backend_reg_count) {
 LogInfo("Loaded ggml_backend_reg_count from ggml.dll");
 }
 // Load manual registration functions (for DLLs missing ggml_backend_init)
 pfn_ggml_backend_register = (fn_ggml_backend_register)GetProcAddress(s_ggml_dll, "ggml_backend_register");
 if (pfn_ggml_backend_register) {
 LogInfo("Loaded ggml_backend_register from ggml.dll");
 }
 pfn_ggml_backend_dev_by_type = (fn_ggml_backend_dev_by_type)GetProcAddress(s_ggml_dll, "ggml_backend_dev_by_type");
 if (pfn_ggml_backend_dev_by_type) {
 LogInfo("Loaded ggml_backend_dev_by_type from ggml.dll");
 }
 } else {
 LogInfo("Failed to load ggml.dll (non-fatal)");
 }
 }
 }
 }
 return true;
}

static void tknc_llama_log_callback(enum ggml_log_level level, const char * text, void * user_data) {
 (void)user_data;
 if (!text || text[0] == '\0' || text[0] == '.' || text[0] == '\n') return;

 // Only log critical errors and final results.
 const char* keep_keywords[] = {
 "FATAL", "ERROR", "error", "fail", "crash",
 "SUCCESS", "Response:", "Tokens:", "Time:",
 nullptr
 };

 bool should_output = (level >= GGML_LOG_LEVEL_ERROR);
 if (!should_output) {
 for (int i = 0; keep_keywords[i]; i++) {
 if (strstr(text, keep_keywords[i])) {
 should_output = true;
 break;
 }
 }
 }

 if (should_output) {
 fprintf(stderr, "[LLM] %s", text);
 }
}

LLamaDLL::LLamaDLL() : m_loaded(false), m_gpu_device(nullptr) {}

bool LLamaDLL::Load(const std::string& dll_path) {
 if (m_loaded) return true;

 // LAYER 1: Pre-load ALL transitive dependencies in dependency order (leaf-first).
 // Windows DLL loader resolves imports at load time. If a dependency isn't already
 // in the process address space AND can't be found via standard search paths,
 // LoadLibrary fails with ERROR_PROC_NOT_FOUND (127) even though the file exists.
 //
 // (Chinese comment removed)
 // Solution: Load from leaf to root so each dep is already in memory when needed.
 //
 // Also set PATH/DllDirectory as belt-and-suspenders for any other deps we may have missed.
 // Use a local mutable copy so we can fix up the path if needed
 std::string resolved_dll_path = dll_path;
 std::string dll_dir = resolved_dll_path;
 {
 size_t pos = dll_dir.rfind('\\');
 if (pos != std::string::npos) {
 dll_dir = dll_dir.substr(0, pos);
 } else {
 // (Chinese comment removed)
 pos = dll_dir.rfind('/');
 if (pos != std::string::npos) {
 dll_dir = dll_dir.substr(0, pos);
 }
 }

 // SAFETY: If resolved dir looks wrong (contains .dll or is too short),
 // fall back to exe-relative dll/ directory
 if (dll_dir.find(".dll") != std::string::npos || dll_dir.length() < 3) {
 LogWarning("[DLL-PATH] Resolved dll_dir='%s' looks invalid, falling back to exe-relative path", dll_dir.c_str());
 fs::path exe_dir = GetExeDir();
 dll_dir = PathToString(exe_dir / "dll");
 // Also fix the actual llama.dll path
 resolved_dll_path = PathToString(exe_dir / "dll" / "llama.dll");
 LogInfo("[DLL-PATH] Fallback: dll_path='%s' dll_dir='%s'", resolved_dll_path.c_str(), dll_dir.c_str());
 }
 // Belt-and-suspenders: set all possible search mechanisms
 SetDllDirectoryA(dll_dir.c_str());
 std::string current_path = getenv("PATH") ? getenv("PATH") : "";
 _putenv(("PATH=" + dll_dir + ";" + current_path).c_str());

 // CRITICAL: Pre-load dependencies in leaf-first order
 // Each successful LoadLibrary puts the DLL into the process address space.
 // When llama.dll is loaded later, Windows finds these deps already loaded.
 const char* dep_order[] = {
 "libomp140.x86_64.dll", // LLVM OpenMP runtime (leaf dependency)
 "ggml-base.dll", // ggml base library
 "ggml.dll", // ggml main library
 nullptr // sentinel
 };
 LogInfo("[DLL-PATH] dll_dir='%s' (resolved from dll_path='%s')", dll_dir.c_str(), resolved_dll_path.c_str());
 for (int i = 0; dep_order[i]; i++) {
 std::string dep_path = dll_dir + "\\" + dep_order[i];
 // Check file existence BEFORE attempting LoadLibrary
 DWORD fattr = GetFileAttributesA(dep_path.c_str());
 bool file_exists = (fattr != INVALID_FILE_ATTRIBUTES && !(fattr & FILE_ATTRIBUTE_DIRECTORY));
 HMODULE hDep = LoadLibraryA(dep_path.c_str());
 if (hDep) {
 LogInfo("Pre-loaded dependency: %s (handle=%p) [file_exists=%s]", dep_order[i], (void*)hDep, file_exists ? "YES" : "NO");
 } else {
 DWORD dep_err = GetLastError();
 LogWarning("Pre-load %s failed (err=%lu) [path='%s' file_exists=%s], continuing...", dep_order[i], dep_err, dep_path.c_str(), file_exists ? "YES" : "NO(!)");
 // (Chinese comment removed)
 }
 }

 // CRITICAL: Pre-load GPU backend DLL BEFORE llama.dll initialization.
 // Without this, llama_backend_init() may not find the Vulkan/CUDA backend,
 // causing model to silently fall back to CPU even with n_gpu_layers > 0.
 //
 // Probe order matches gpu_memory.cpp vendor-aware logic:
 // (Chinese comment removed)
 // (Chinese comment removed)

 // Detect local GPU vendor via registry (lightweight, no DXGI header dependency)
 // [GPU-FIX] Collect ALL vendors first, then prefer NVIDIA over AMD/iGPU.
 // This fixes the "first-one-wins" bug where AMD iGPU (registry index 0000)
 // would shadow a later NVIDIA dGPU (index 0001), causing Vulkan backend
 // to load instead of CUDA, and GetGPUDevice() to pick the iGPU.
 std::string llm_vendor = "Unknown";
 bool has_nvidia = false;
 bool has_amd = false;
#ifdef _WIN32
 {
 HKEY hKey;
 if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
 "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}",
 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
 char name[256] = {0};
 DWORD idx = 0;
 while (true) {
 DWORD nameLen = sizeof(name);
 if (RegEnumKeyExA(hKey, idx++, name, &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;

 HKEY hSubKey;
 if (RegOpenKeyExA(hKey, name, 0, KEY_READ, &hSubKey) == ERROR_SUCCESS) {
 char desc[256] = {0}; DWORD descLen = sizeof(desc);
 char vendorId[32] = {0}; DWORD vendorIdLen = sizeof(vendorId);
 RegQueryValueExA(hSubKey, "DriverDesc", 0, nullptr, (LPBYTE)desc, &descLen);
 RegQueryValueExA(hSubKey, "MatchingDeviceId", 0, nullptr, (LPBYTE)vendorId, &vendorIdLen);
 // Look for NVIDIA or AMD in vendor ID string: "PCI\\VEN_10DE" or "PCI\\VEN_1002"
 std::string vid(vendorId);
 if (vid.find("VEN_10DE") != std::string::npos)
 has_nvidia = true;
 else if (vid.find("VEN_1002") != std::string::npos || vid.find("VEN_1022") != std::string::npos)
 has_amd = true;
 RegCloseKey(hSubKey);
 }
 }
 RegCloseKey(hKey);
 // (Chinese comment removed)
 if (has_nvidia) llm_vendor = "NVIDIA";
 else if (has_amd) llm_vendor = "AMD";
 }
 }
#endif

 bool is_nvidia = (llm_vendor == "NVIDIA");
 LogInfo("[GPU] LLM GPU vendor detection: %s (prefer %s)",
 llm_vendor.c_str(), is_nvidia ? "CUDA" : "Vulkan");

 const char* gpu_backend_primary = is_nvidia ? "ggml-cuda.dll" : "ggml-vulkan.dll";
 const char* gpu_backend_fallback = is_nvidia ? "ggml-vulkan.dll" : "ggml-cuda.dll";

 bool gpu_backend_loaded = false;
 for (const char* dll_name : {gpu_backend_primary, gpu_backend_fallback}) {
 if (!dll_name) continue;
 std::string gpu_dll_path = dll_dir + "\\" + dll_name;
 HMODULE hGpu = LoadLibraryA(gpu_dll_path.c_str());
 if (hGpu) {
 LogInfo("[GPU] Pre-loaded GPU backend: %s (handle=%p) ?will stay resident",
 dll_name, (void*)hGpu);
 gpu_backend_loaded = true;
 break; // Only need one GPU backend
 } else {
 DWORD gpu_err = GetLastError();
 LogInfo("[GPU] %s not found (err=%lu), trying next...", dll_name, gpu_err);
 }
 }
 if (!gpu_backend_loaded) {
 LogWarning("[GPU] WARNING: No GPU backend DLL found! Inference will use CPU only.");
 LogWarning("[GPU] Expected: ggml-vulkan.dll (AMD) or ggml-cuda.dll (NVIDIA) in %s", dll_dir.c_str());
 }

 LogInfo("DLL search path set to: %s (pre-loaded transitive deps)", dll_dir.c_str());
 }

 // Load llama.dll
 s_llama_dll = LoadLibraryA(resolved_dll_path.c_str());
 if (!s_llama_dll) {
 DWORD err = GetLastError();
 LogError("Failed to load llama.dll from: %s (GetLastError=%lu/0x%lX)", resolved_dll_path.c_str(), err, err);

 // [DIAGNOSTIC] Enumerate all possible transitive dependencies and test each one
 // to identify exactly which DLL is missing (err=126 = ERROR_MOD_NOT_FOUND)
 LogError("[DLL-DEPS] === DIAGNOSING MISSING TRANSITIVE DEPENDENCIES FOR llama.dll ===");
 const char* candidate_deps[] = {
 // MSVC C++ runtime extensions
 "vccorlib140.dll",
 "concrt140.dll",
 "msvcp140_1.dll",
 "msvcp140_2.dll",
 "msvcp140_atomic_wait.dll",
 "msvcp140_codecvt_ids.dll",
 "vcruntime140_1.dll",
 "vcruntime140_threads.dll",
 // llama.cpp sub-modules
 "llama-common.dll",
 "llama-server-impl.dll",
 // ggml CPU variants (may be needed)
 "ggml-cpu-x64.dll",
 "ggml-rpc.dll",
 // AMD/ROCm specific
 "amd_comgr0701.dll",
 "amd_comgr_3.dll",
 "rocblas.dll",
 "libhipblas.dll",
 "libhipblaslt.dll",
 nullptr
 };
 int found_count = 0, missing_count = 0;
 for (int i = 0; candidate_deps[i]; i++) {
 std::string dep_path = dll_dir + "\\" + candidate_deps[i];
 HMODULE hDep = LoadLibraryA(dep_path.c_str());
 if (hDep) {
 FreeLibrary(hDep); // Don't keep it loaded, just testing
 if (++found_count <= 10) {
 LogInfo("[DLL-DEPS] [OK] %s", candidate_deps[i]);
 }
 } else {
 DWORD dep_err = GetLastError();
 if (++missing_count <= 15) {
 LogError("[DLL-DEPS] [MISSING] %s (err=%lu)", candidate_deps[i], dep_err);
 }
 }
 }
 LogError("[DLL-DEPS] Summary: %d found, %d missing ?fix the MISSING ones above", found_count, missing_count);
 return false;
 }

 // Load all function pointers
 if (!LoadLLamaFunctions(resolved_dll_path)) {
 FreeLibrary(s_llama_dll);
 s_llama_dll = nullptr;
 return false;
 }

 // Set log callback
 if (pfn_llama_log_set) {
 pfn_llama_log_set(tknc_llama_log_callback, nullptr);
 }

 // ====================================================================
 // (Chinese comment removed)
 // 1. Call ggml_backend_load_all_from_path() via ggml.dll to load
 // GPU backends (CUDA, Vulkan) into ggml.dll's registry
 // (Chinese comment removed)
 // ggml_backend_load_all() which loads GPU backends into llama.dll's
 // own registry. But CPU backend may still be missing.
 // 3. Check llama.dll's registry for CPU backend. If missing, manually
 // register via llama.dll's ggml_backend_register() + ggml-cpu.dll's
 // ggml_backend_cpu_reg(). This is the KEY fix for the dual-registry
 // problem: we MUST use llama.dll's ggml_backend_register, not
 // ggml.dll's, because llama_model_load checks llama.dll's registry.
 // ====================================================================

 LogInfo("[LLM] Initializing llama backend...");
 LogInfo("[LLM] DEBUG: pfn_ggml_backend_load_all_from_path=%p, pfn_ggml_backend_load_all=%p",
 (void*)pfn_ggml_backend_load_all_from_path, (void*)pfn_ggml_backend_load_all);
 LogInfo("[LLM] DEBUG: pfn_ggml_backend_dev_by_type=%p, pfn_ggml_backend_register=%p",
 (void*)pfn_ggml_backend_dev_by_type, (void*)pfn_ggml_backend_register);
 LogInfo("[LLM] DEBUG: pfn_llama_ggml_backend_register=%p, pfn_llama_ggml_backend_dev_by_type=%p",
 (void*)pfn_llama_ggml_backend_register, (void*)pfn_llama_ggml_backend_dev_by_type);

 // Step 1: Load backends via ggml.dll's standard path (for ggml.dll's registry)
 if (pfn_ggml_backend_load_all_from_path) {
 LogInfo("[LLM] Calling ggml_backend_load_all_from_path('%s')...", dll_dir.c_str());
 pfn_ggml_backend_load_all_from_path(dll_dir.c_str());
 LogInfo("[LLM] ggml_backend_load_all_from_path('%s') completed", dll_dir.c_str());
 } else if (pfn_ggml_backend_load_all) {
 pfn_ggml_backend_load_all();
 LogInfo("[LLM] ggml_backend_load_all() completed (fallback)");
 } else {
 LogWarning("[LLM] No backend loading function available");
 }

 // (Chinese comment removed)
 if (pfn_llama_backend_init) {
 LogInfo("[LLM] DEBUG: llama.dll reg_count before llama_backend_init=%d",
 pfn_llama_ggml_backend_reg_count ? pfn_llama_ggml_backend_reg_count() : -1);
 pfn_llama_backend_init();
 LogInfo("[LLM] llama_backend_init() completed");
 LogInfo("[LLM] DEBUG: llama.dll reg_count after llama_backend_init=%d",
 pfn_llama_ggml_backend_reg_count ? pfn_llama_ggml_backend_reg_count() : -1);
 }

 // Step 3: Check llama.dll's registry for CPU backend
 // GGML_BACKEND_DEVICE_TYPE_CPU = 0
 bool cpu_in_llama = false;
 if (pfn_llama_ggml_backend_dev_by_type) {
 void* cpu_dev = pfn_llama_ggml_backend_dev_by_type(0);
 LogInfo("[LLM] DEBUG: llama.dll cpu_dev=%p", cpu_dev);
 if (cpu_dev) {
 cpu_in_llama = true;
 LogInfo("[LLM] CPU backend found in llama.dll's registry");
 } else {
 LogInfo("[LLM] CPU backend NOT in llama.dll's registry ?manual registration needed");
 }
 }

 // Also check ggml.dll's registry for diagnostic comparison
 if (pfn_ggml_backend_dev_by_type) {
 void* cpu_dev_ggml = pfn_ggml_backend_dev_by_type(0);
 LogInfo("[LLM] DEBUG: ggml.dll cpu_dev=%p (for comparison)", cpu_dev_ggml);
 }

 // Step 4: Manual CPU backend registration into llama.dll's registry
 // This is the KEY fix: use llama.dll's ggml_backend_register, not ggml.dll's
 if (!cpu_in_llama && pfn_llama_ggml_backend_register) {
 LogInfo("[LLM] Registering CPU backend into llama.dll's registry...");

 std::string cpu_dll_path = dll_dir + "\\ggml-cpu.dll";
 HMODULE h_cpu = LoadLibraryA(cpu_dll_path.c_str());
 if (h_cpu) {
 fn_ggml_backend_cpu_reg pfn_cpu_reg =
 (fn_ggml_backend_cpu_reg)GetProcAddress(h_cpu, "ggml_backend_cpu_reg");
 if (pfn_cpu_reg) {
 void* reg = pfn_cpu_reg();
 if (reg) {
 LogInfo("[LLM] DEBUG: ggml_backend_cpu_reg() returned reg=%p", reg);

 // CRITICAL: Register in llama.dll's registry, not ggml.dll's!
 pfn_llama_ggml_backend_register(reg);
 LogInfo("[LLM] CPU backend registered into llama.dll's registry via ggml_backend_register()");

 // Verify in llama.dll's registry
 if (pfn_llama_ggml_backend_dev_by_type) {
 void* cpu_dev_after = pfn_llama_ggml_backend_dev_by_type(0);
 LogInfo("[LLM] DEBUG: llama.dll cpu_dev after registration=%p", cpu_dev_after);
 if (cpu_dev_after) {
 cpu_in_llama = true;
 }
 }
 } else {
 LogError("[LLM] ggml_backend_cpu_reg() returned NULL");
 }
 } else {
 LogError("[LLM] ggml_backend_cpu_reg not found in ggml-cpu.dll");
 }
 // (Chinese comment removed)
 } else {
 LogError("[LLM] Failed to load ggml-cpu.dll from: %s", cpu_dll_path.c_str());
 }
 }

 // Fallback: also try ggml.dll's register if llama.dll's is not available
 if (!cpu_in_llama && pfn_ggml_backend_register && !pfn_llama_ggml_backend_register) {
 LogWarning("[LLM] llama.dll's ggml_backend_register not available, trying ggml.dll's (may not work for model loading)");
 std::string cpu_dll_path = dll_dir + "\\ggml-cpu.dll";
 HMODULE h_cpu = LoadLibraryA(cpu_dll_path.c_str());
 if (h_cpu) {
 fn_ggml_backend_cpu_reg pfn_cpu_reg =
 (fn_ggml_backend_cpu_reg)GetProcAddress(h_cpu, "ggml_backend_cpu_reg");
 if (pfn_cpu_reg) {
 void* reg = pfn_cpu_reg();
 if (reg) {
 pfn_ggml_backend_register(reg);
 LogInfo("[LLM] CPU backend registered via ggml.dll's ggml_backend_register (fallback)");
 cpu_in_llama = true; // hope for the best

 // Verify: check if ggml.dll's dev_by_type can now find CPU
 if (pfn_ggml_backend_dev_by_type) {
 void* cpu_dev_after = pfn_ggml_backend_dev_by_type(0);
 LogInfo("[LLM] DEBUG: ggml.dll cpu_dev AFTER fallback registration=%p", cpu_dev_after);
 }
 }
 }
 }
 }

 if (!cpu_in_llama) {
 LogWarning("[LLM] CPU backend not available in llama.dll's registry ?model loading will likely fail");
 }

 // Final verification
 if (pfn_llama_ggml_backend_dev_by_type) {
 void* cpu_dev_final = pfn_llama_ggml_backend_dev_by_type(0);
 LogInfo("[LLM] DEBUG: llama.dll cpu_dev FINAL=%p", cpu_dev_final);
 }

 // Diagnostic: check registered backend counts in both registries
 if (pfn_llama_ggml_backend_reg_count) {
 LogInfo("[LLM] llama.dll registry backend count: %d", pfn_llama_ggml_backend_reg_count());
 }
 if (pfn_ggml_backend_reg_count) {
 LogInfo("[LLM] ggml.dll registry backend count: %d", pfn_ggml_backend_reg_count());
 }

 // ================================================================
 // (Chinese comment removed)
 //
 // [GPU-FIX v2] Corrected device type priority based on ggml-backend.h:
 // GGML_BACKEND_DEVICE_TYPE_CPU = 0
 // (Chinese comment removed)
 // (Chinese comment removed)
 // GGML_BACKEND_DEVICE_TYPE_ACCEL = 3
 // GGML_BACKEND_DEVICE_TYPE_META = 4
 // ================================================================
 m_gpu_device = nullptr;
 m_gpu_devices.clear();

 if (pfn_llama_ggml_backend_reg_count && pfn_llama_ggml_backend_dev_by_type) {
 int reg_count = pfn_llama_ggml_backend_reg_count();
 LogInfo("[GPU] Enumerating %d registered backends in llama.dll...", reg_count);

 // Strategy 1: Use full enumeration API if available (ggml_backend_dev_count + dev_get)
 if (pfn_llama_ggml_backend_dev_count && pfn_llama_ggml_backend_dev_get) {
 size_t dev_count = pfn_llama_ggml_backend_dev_count();
 LogInfo("[GPU] Total devices in registry: %zu", dev_count);

 for (size_t i = 0; i < dev_count; i++) {
 void* dev = pfn_llama_ggml_backend_dev_get(i);
 if (!dev) continue;

 int dev_type = 0;
 if (pfn_llama_ggml_backend_dev_type_fn) {
 dev_type = pfn_llama_ggml_backend_dev_type_fn(dev);
 }

 // Skip CPU devices (type=0)
 if (dev_type == 0) continue;

 GPUDevInfo info;
 info.handle = dev;
 info.dev_type = dev_type;
 memset(info.name, 0, sizeof(info.name));
 memset(info.desc, 0, sizeof(info.desc));
 info.vram_total = 0;
 info.vram_free = 0;

 if (pfn_llama_ggml_backend_dev_name) {
 const char* name = pfn_llama_ggml_backend_dev_name(dev);
 if (name) strncpy(info.name, name, sizeof(info.name) - 1);
 }
 if (pfn_llama_ggml_backend_dev_description) {
 const char* desc = pfn_llama_ggml_backend_dev_description(dev);
 if (desc) strncpy(info.desc, desc, sizeof(info.desc) - 1);
 }
 if (pfn_llama_ggml_backend_dev_memory) {
 pfn_llama_ggml_backend_dev_memory(dev, &info.vram_free, &info.vram_total);
 }

 const char* type_name = (dev_type == 1) ? "dGPU" :
 (dev_type == 2) ? "iGPU" :
 (dev_type == 3) ? "ACCEL" : "META";
 LogInfo("[GPU] Device[%zu]: %s | type=%d (%s) | VRAM=%zu MB (free=%zu MB)",
 i, info.name, dev_type, type_name,
 info.vram_total / (1024*1024), info.vram_free / (1024*1024));

 m_gpu_devices.push_back(info);
 }
 }

 // (Chinese comment removed)
 if (m_gpu_devices.empty()) {
 // Try dGPU first, then iGPU
 const int types[] = { 1, 2 }; // dGPU, iGPU
 for (int t = 0; t < 2; t++) {
 void* dev = pfn_llama_ggml_backend_dev_by_type(types[t]);
 if (dev) {
 GPUDevInfo info;
 info.handle = dev;
 info.dev_type = types[t];
 memset(info.name, 0, sizeof(info.name));
 memset(info.desc, 0, sizeof(info.desc));
 info.vram_total = 0;
 info.vram_free = 0;
 snprintf(info.name, sizeof(info.name), "GPU(type=%d)", types[t]);
 m_gpu_devices.push_back(info);
 }
 }
 }

 // (Chinese comment removed)
 std::sort(m_gpu_devices.begin(), m_gpu_devices.end(),
 [](const GPUDevInfo& a, const GPUDevInfo& b) {
 if (a.dev_type != b.dev_type) return a.dev_type < b.dev_type; // dGPU < iGPU
 return a.vram_total > b.vram_total; // larger VRAM first
 });

 // Set primary GPU device (first dGPU, or first iGPU if no dGPU)
 if (!m_gpu_devices.empty()) {
 m_gpu_device = m_gpu_devices[0].handle;
 int dgpu_count = 0, igpu_count = 0;
 for (const auto& d : m_gpu_devices) {
 if (d.dev_type == 1) dgpu_count++;
 else if (d.dev_type == 2) igpu_count++;
 }
 LogInfo("[GPU] Found %d GPU device(s): %d dGPU + %d iGPU", (int)m_gpu_devices.size(), dgpu_count, igpu_count);
 for (size_t i = 0; i < m_gpu_devices.size(); i++) {
 const char* type_name = (m_gpu_devices[i].dev_type == 1) ? "dGPU" : "iGPU";
 LogInfo("[GPU] [%zu] %s (%s) VRAM=%zu MB", i, m_gpu_devices[i].name, type_name, m_gpu_devices[i].vram_total / (1024*1024));
 }
 }

 if (!m_gpu_device) {
 LogWarning("[GPU] No GPU device found in llama.dll registry!");
 LogWarning("[GPU] n_gpu_layers > 0 will be IGNORED ?model will load on CPU only.");
 LogWarning("[GPU] Ensure ggml-vulkan.dll / ggml-cuda.dll is loaded AND compatible.");
 } else {
 LogInfo("[GPU] Primary GPU: %s ?model will use GPU acceleration", m_gpu_devices[0].name);
 }
 }

 m_loaded = true;
 LogInfo("[LLM] llama.dll loaded successfully");
 return true;
}

LLamaDLL& LLamaDLL::Instance() {
 static LLamaDLL instance;
 return instance;
}

bool LLamaDLL::IsLoaded() const { return m_loaded; }

void LLamaDLL::Unload() {
 if (s_llama_dll) {
 FreeLibrary(s_llama_dll);
 s_llama_dll = nullptr;
 }
 m_loaded = false;
}

llama_model_params LLamaDLL::model_default_params() {
 // Use DLL internal default params to prevent struct layout mismatch crashes.
 llama_model_params params = {};
 memset(&params, 0, sizeof(params));

 if (pfn_llama_model_default_params) {
 params = pfn_llama_model_default_params();
 }
 return params;
}

llama_model* LLamaDLL::model_load_from_file(const char* path, llama_model_params params) {
 return pfn_llama_model_load_from_file ? pfn_llama_model_load_from_file(path, params) : nullptr;
}

void LLamaDLL::model_free(llama_model* model) {
 if (pfn_llama_model_free) pfn_llama_model_free(model);
}

llama_context* LLamaDLL::new_context_with_model_default(llama_model* model, int32_t n_ctx, int32_t n_threads) {
 if (!pfn_llama_init_from_model || !pfn_llama_context_default_params) return nullptr;
 // [CRITICAL FIX v3] The struct is now 160 bytes (with _dll_extension_padding)
 // to match the DLL's actual memcpy size in llama_context_default_params().
 // Without this, the previous 144-byte struct caused 16 bytes of stack overflow
 // that corrupted the canary and triggered "*** stack smashing detected ***"
 // at the end of llama_init_from_model.
 llama_context_params ctx_params = pfn_llama_context_default_params();
 ctx_params.n_ctx = n_ctx;
 ctx_params.n_batch = n_ctx;  // Set batch = context window so any prompt that fits in context also fits in one batch
 ctx_params.n_threads = n_threads;
 ctx_params.n_threads_batch = n_threads;
 ctx_params.flash_attn_type = LLAMA_FLASH_ATTENTION_TYPE_DISABLED;
 return pfn_llama_init_from_model(model, ctx_params);
}

void LLamaDLL::free(llama_context* ctx) {
 if (pfn_llama_free) pfn_llama_free(ctx);
}

int32_t LLamaDLL::n_ctx(const llama_context* ctx) {
 return pfn_llama_n_ctx ? pfn_llama_n_ctx(ctx) : 0;
}

int32_t LLamaDLL::n_batch(const llama_context* ctx) {
 return pfn_llama_n_batch ? pfn_llama_n_batch(ctx) : 0;
}

int32_t LLamaDLL::n_vocab(const llama_model* model) {
 if (!pfn_llama_model_get_vocab || !pfn_llama_vocab_n_tokens) return 0;
 const llama_vocab* vocab = pfn_llama_model_get_vocab(model);
 return vocab ? pfn_llama_vocab_n_tokens(vocab) : 0;
}

llama_token LLamaDLL::token_bos(const llama_model* model) {
 if (!pfn_llama_model_get_vocab || !pfn_llama_vocab_bos) return 0;
 return pfn_llama_vocab_bos(pfn_llama_model_get_vocab(model));
}

llama_token LLamaDLL::token_eos(const llama_model* model) {
 if (!pfn_llama_model_get_vocab || !pfn_llama_vocab_eos) return 0;
 return pfn_llama_vocab_eos(pfn_llama_model_get_vocab(model));
}

int32_t LLamaDLL::tokenize(const llama_model* model, const char* text, int32_t text_len,
 llama_token* tokens, int32_t n_max_tokens, bool add_bos, bool special) {
 if (!pfn_llama_model_get_vocab || !pfn_llama_tokenize) return 0;
 return pfn_llama_tokenize(pfn_llama_model_get_vocab(model), text, text_len, tokens, n_max_tokens, add_bos, special);
}

int32_t LLamaDLL::token_to_piece(const llama_model* model, llama_token token, char* buf,
 int32_t length, int32_t lstrip, bool special) {
 if (!pfn_llama_model_get_vocab || !pfn_llama_token_to_piece) return 0;
 return pfn_llama_token_to_piece(pfn_llama_model_get_vocab(model), token, buf, length, lstrip, special);
}

llama_batch LLamaDLL::batch_get_one(llama_token* tokens, int32_t n_tokens) {
 llama_batch batch = {};
 if (pfn_llama_batch_get_one) {
 batch = pfn_llama_batch_get_one(tokens, n_tokens);
 }
 return batch;
}

void LLamaDLL::batch_free(llama_batch batch) {
 if (pfn_llama_batch_free) pfn_llama_batch_free(batch);
}

int32_t LLamaDLL::decode(llama_context* ctx, llama_batch batch) {
 return pfn_llama_decode ? pfn_llama_decode(ctx, batch) : -1;
}

float* LLamaDLL::get_logits(llama_context* ctx) {
 return pfn_llama_get_logits ? pfn_llama_get_logits(ctx) : nullptr;
}

llama_sampler* LLamaDLL::sampler_chain_init(llama_sampler_chain_params params) {
 return pfn_llama_sampler_chain_init ? pfn_llama_sampler_chain_init(params) : nullptr;
}

void LLamaDLL::sampler_chain_add(llama_sampler* chain, llama_sampler* sampler) {
 if (pfn_llama_sampler_chain_add) pfn_llama_sampler_chain_add(chain, sampler);
}

void LLamaDLL::sampler_free(llama_sampler* sampler) {
 if (pfn_llama_sampler_free) pfn_llama_sampler_free(sampler);
}

void LLamaDLL::sampler_accept(llama_sampler* sampler, llama_token token) {
 if (pfn_llama_sampler_accept) pfn_llama_sampler_accept(sampler, token);
}

llama_token LLamaDLL::sampler_sample(llama_sampler* sampler, llama_context* ctx, int32_t idx) {
 return pfn_llama_sampler_sample ? pfn_llama_sampler_sample(sampler, ctx, idx) : 0;
}

llama_sampler* LLamaDLL::sampler_init_temp(float temp) {
 return pfn_llama_sampler_init_temp ? pfn_llama_sampler_init_temp(temp) : nullptr;
}

llama_sampler* LLamaDLL::sampler_init_top_k(int32_t k) {
 return pfn_llama_sampler_init_top_k ? pfn_llama_sampler_init_top_k(k) : nullptr;
}

llama_sampler* LLamaDLL::sampler_init_top_p(float p, size_t min_keep) {
 return pfn_llama_sampler_init_top_p ? pfn_llama_sampler_init_top_p(p, min_keep) : nullptr;
}

llama_sampler* LLamaDLL::sampler_init_dist(uint32_t seed) {
 return pfn_llama_sampler_init_dist ? pfn_llama_sampler_init_dist(seed) : nullptr;
}

llama_sampler* LLamaDLL::sampler_init_greedy() {
 return pfn_llama_sampler_init_greedy ? pfn_llama_sampler_init_greedy() : nullptr;
}

llama_sampler* LLamaDLL::sampler_init_penalties(int32_t penalty_last_n, float penalty_repeat, float penalty_freq, float penalty_present) {
 return pfn_llama_sampler_init_penalties ? pfn_llama_sampler_init_penalties(penalty_last_n, penalty_repeat, penalty_freq, penalty_present) : nullptr;
}

void LLamaDLL::kv_cache_clear(llama_context* ctx) {
 if (pfn_llama_kv_cache_clear) pfn_llama_kv_cache_clear(ctx);
}

const char* LLamaDLL::model_desc(llama_model* model, char* buf, size_t buf_size) {
 if (pfn_llama_model_desc) {
 pfn_llama_model_desc(model, buf, buf_size);
 } else {
 strncpy(buf, "Unknown model", buf_size);
 }
 return buf;
}

void LLamaDLL::log_set(ggml_log_callback callback, void* user_data) {
 if (pfn_llama_log_set) {
 pfn_llama_log_set(callback, user_data);
 }
}

void* LLamaDLL::GetGPUDevice() {
 return m_gpu_device;
}

bool LLamaDLL::IsModelOnGPU(llama_model* model) {
 if (!model) return false;
 if (!pfn_llama_model_n_gpu_layers) {
 LogWarning("[GPU] llama_model_n_gpu_layers not available ?cannot verify GPU offload");
 return false;
 }
 int32_t n_gpu = pfn_llama_model_n_gpu_layers(model);
 LogInfo("[GPU] Model GPU layer check: n_gpu_layers=%d", n_gpu);
 return n_gpu > 0;
}

#else

// macOS/Linux implementation using dlopen
#ifdef __APPLE__
#include <dlfcn.h>
#define LIB_HANDLE void*
#define LOAD_LIB(path) dlopen(path, RTLD_LAZY)
#define GET_SYM(handle, name) dlsym(handle, name)
#define FREE_LIB(handle) dlclose(handle)
#define LIB_EXT ".dylib"
#else
#include <dlfcn.h>
#define LIB_HANDLE void*
#define LOAD_LIB(path) dlopen(path, RTLD_LAZY)
#define GET_SYM(handle, name) dlsym(handle, name)
#define FREE_LIB(handle) dlclose(handle)
#define LIB_EXT ".so"
#endif

static LIB_HANDLE s_llama_lib = nullptr;

// Function pointers (same types as Windows version)
static fn_llama_log_set pfn_llama_log_set = nullptr;
static fn_llama_model_default_params pfn_llama_model_default_params = nullptr;
static fn_llama_model_load_from_file pfn_llama_model_load_from_file = nullptr;
static fn_llama_model_free pfn_llama_model_free = nullptr;
static fn_llama_model_desc pfn_llama_model_desc = nullptr;
static fn_llama_model_get_vocab pfn_llama_model_get_vocab = nullptr;
static fn_llama_context_default_params pfn_llama_context_default_params = nullptr;
static fn_llama_init_from_model pfn_llama_init_from_model = nullptr;
static fn_llama_free pfn_llama_free = nullptr;
static fn_llama_n_ctx pfn_llama_n_ctx = nullptr;
static fn_llama_n_batch pfn_llama_n_batch = nullptr;
static fn_llama_vocab_n_tokens pfn_llama_vocab_n_tokens = nullptr;
static fn_llama_vocab_bos pfn_llama_vocab_bos = nullptr;
static fn_llama_vocab_eos pfn_llama_vocab_eos = nullptr;
static fn_llama_tokenize pfn_llama_tokenize = nullptr;
static fn_llama_token_to_piece pfn_llama_token_to_piece = nullptr;
static fn_llama_batch_get_one pfn_llama_batch_get_one = nullptr;
static fn_llama_batch_free pfn_llama_batch_free = nullptr;
static fn_llama_decode pfn_llama_decode = nullptr;
static fn_llama_get_logits pfn_llama_get_logits = nullptr;
static fn_llama_sampler_chain_init pfn_llama_sampler_chain_init = nullptr;
static fn_llama_sampler_chain_add pfn_llama_sampler_chain_add = nullptr;
static fn_llama_sampler_free pfn_llama_sampler_free = nullptr;
static fn_llama_sampler_accept pfn_llama_sampler_accept = nullptr;
static fn_llama_sampler_sample pfn_llama_sampler_sample = nullptr;
static fn_llama_sampler_init_temp pfn_llama_sampler_init_temp = nullptr;
static fn_llama_sampler_init_top_k pfn_llama_sampler_init_top_k = nullptr;
static fn_llama_sampler_init_top_p pfn_llama_sampler_init_top_p = nullptr;
static fn_llama_sampler_init_dist pfn_llama_sampler_init_dist = nullptr;
static fn_llama_sampler_init_greedy pfn_llama_sampler_init_greedy = nullptr;
static fn_llama_sampler_init_penalties pfn_llama_sampler_init_penalties = nullptr;
static fn_llama_kv_cache_clear pfn_llama_kv_cache_clear = nullptr;
static fn_ggml_backend_load_all pfn_ggml_backend_load_all = nullptr;
static fn_llama_backend_init pfn_llama_backend_init = nullptr;

#define LOAD_FUNC_DLSYM(name) \
 pfn_##name = (fn_##name)GET_SYM(s_llama_lib, #name); \
 if (!pfn_##name) { \
 LogError("Failed to load " #name " from llama library"); \
 return false; \
 }

static bool LoadLLamaLibrary(const std::string& lib_path) {
 s_llama_lib = LOAD_LIB(lib_path.c_str());
 if (!s_llama_lib) {
 LogError("Failed to load llama library from: %s (%s)", lib_path.c_str(), dlerror());
 return false;
 }
 LogInfo("Loaded llama library from: %s", lib_path.c_str());
 
 LOAD_FUNC_DLSYM(llama_log_set);
 LOAD_FUNC_DLSYM(llama_model_default_params);
 LOAD_FUNC_DLSYM(llama_model_load_from_file);
 LOAD_FUNC_DLSYM(llama_model_free);
 LOAD_FUNC_DLSYM(llama_model_desc);
 LOAD_FUNC_DLSYM(llama_model_get_vocab);
 LOAD_FUNC_DLSYM(llama_context_default_params);
 LOAD_FUNC_DLSYM(llama_init_from_model);
 LOAD_FUNC_DLSYM(llama_free);
 LOAD_FUNC_DLSYM(llama_n_ctx);
 LOAD_FUNC_DLSYM(llama_n_batch);
 LOAD_FUNC_DLSYM(llama_vocab_n_tokens);
 LOAD_FUNC_DLSYM(llama_vocab_bos);
 LOAD_FUNC_DLSYM(llama_vocab_eos);
 LOAD_FUNC_DLSYM(llama_tokenize);
 LOAD_FUNC_DLSYM(llama_token_to_piece);
 LOAD_FUNC_DLSYM(llama_batch_get_one);
 LOAD_FUNC_DLSYM(llama_batch_free);
 LOAD_FUNC_DLSYM(llama_decode);
 LOAD_FUNC_DLSYM(llama_get_logits);
 LOAD_FUNC_DLSYM(llama_sampler_chain_init);
 LOAD_FUNC_DLSYM(llama_sampler_chain_add);
 LOAD_FUNC_DLSYM(llama_sampler_free);
 LOAD_FUNC_DLSYM(llama_sampler_accept);
 LOAD_FUNC_DLSYM(llama_sampler_sample);
 LOAD_FUNC_DLSYM(llama_sampler_init_temp);
 LOAD_FUNC_DLSYM(llama_sampler_init_top_k);
 LOAD_FUNC_DLSYM(llama_sampler_init_top_p);
 LOAD_FUNC_DLSYM(llama_sampler_init_dist);
 LOAD_FUNC_DLSYM(llama_sampler_init_greedy);
 
 pfn_llama_kv_cache_clear = (fn_llama_kv_cache_clear)GET_SYM(s_llama_lib, "llama_kv_cache_clear");
 if (!pfn_llama_kv_cache_clear) {
 LogInfo("llama_kv_cache_clear not found (non-fatal, using fallback)");
 }
 
 pfn_llama_sampler_init_penalties = (fn_llama_sampler_init_penalties)GET_SYM(s_llama_lib, "llama_sampler_init_penalties");
 if (!pfn_llama_sampler_init_penalties) {
 LogInfo("llama_sampler_init_penalties not found (non-fatal)");
 }
 
 pfn_ggml_backend_load_all = (fn_ggml_backend_load_all)GET_SYM(s_llama_lib, "ggml_backend_load_all");
 if (!pfn_ggml_backend_load_all) {
 LogInfo("ggml_backend_load_all not found (non-fatal)");
 }
 
 pfn_llama_backend_init = (fn_llama_backend_init)GET_SYM(s_llama_lib, "llama_backend_init");
 if (pfn_llama_backend_init) {
 pfn_llama_backend_init();
 LogInfo("llama_backend_init called");
 }
 
 if (pfn_ggml_backend_load_all) {
 pfn_ggml_backend_load_all();
 LogInfo("ggml_backend_load_all called");
 }
 
 return true;
}

LLamaDLL::LLamaDLL() : m_loaded(false), m_gpu_device(nullptr) {}

bool LLamaDLL::Load(const std::string& dll_path) {
 std::string lib_path = dll_path;
 
 // Convert .dll to platform extension
 if (lib_path.find(".dll") != std::string::npos) {
#ifdef __APPLE__
 lib_path.replace(lib_path.find(".dll"), 4, ".dylib");
#else
 lib_path.replace(lib_path.find(".dll"), 4, ".so");
#endif
 }
 
 // Try multiple paths with absolute path resolution
 std::vector<std::string> search_paths = {
 lib_path,
 "/Users/a/Desktop/tknc/TKNC/build/bin/dll/libllama" LIB_EXT,
 "/opt/homebrew/lib/libllama" LIB_EXT,
 "/usr/local/lib/libllama" LIB_EXT,
 "/usr/lib/libllama" LIB_EXT,
 "./libllama" LIB_EXT,
 "./dll/libllama" LIB_EXT
 };
 
 for (const auto& path : search_paths) {
 if (LoadLLamaLibrary(path)) {
 m_loaded = true;
 LogInfo("LLamaDLL loaded successfully from: %s", path.c_str());
 return true;
 }
 }
 
 LogError("LLamaDLL: Failed to load from any path");
 m_loaded = false;
 return false;
}

LLamaDLL& LLamaDLL::Instance() {
 static LLamaDLL instance;
 return instance;
}

bool LLamaDLL::IsLoaded() const { return m_loaded; }

void LLamaDLL::Unload() {
 if (s_llama_lib) {
 FREE_LIB(s_llama_lib);
 s_llama_lib = nullptr;
 }
 m_loaded = false;
}

llama_model_params LLamaDLL::model_default_params() {
 if (pfn_llama_model_default_params) return pfn_llama_model_default_params();
 return {};
}

llama_model* LLamaDLL::model_load_from_file(const char* path, llama_model_params params) {
 if (pfn_llama_model_load_from_file) return pfn_llama_model_load_from_file(path, params);
 return nullptr;
}

void LLamaDLL::model_free(llama_model* model) {
 if (pfn_llama_model_free) pfn_llama_model_free(model);
}

llama_context* LLamaDLL::new_context_with_model_default(llama_model* model, int32_t n_ctx, int32_t n_threads) {
 if (!pfn_llama_init_from_model || !model) return nullptr;
 llama_context_params params = pfn_llama_context_default_params();
 params.n_ctx = n_ctx;
 params.n_batch = n_ctx;  // Set batch = context window
 params.n_threads = n_threads;
 params.n_threads_batch = n_threads;
 return pfn_llama_init_from_model(model, params);
}

void LLamaDLL::free(llama_context* ctx) {
 if (pfn_llama_free) pfn_llama_free(ctx);
}

int32_t LLamaDLL::n_ctx(const llama_context* ctx) {
 if (pfn_llama_n_ctx) return pfn_llama_n_ctx(ctx);
 return 0;
}

int32_t LLamaDLL::n_batch(const llama_context* ctx) {
 if (pfn_llama_n_batch) return pfn_llama_n_batch(ctx);
 return 0;
}

int32_t LLamaDLL::n_vocab(const llama_model* model) {
 if (pfn_llama_vocab_n_tokens && pfn_llama_model_get_vocab) {
 const llama_vocab* vocab = pfn_llama_model_get_vocab(model);
 if (vocab) return pfn_llama_vocab_n_tokens(vocab);
 }
 return 0;
}

llama_token LLamaDLL::token_bos(const llama_model* model) {
 if (pfn_llama_vocab_bos && pfn_llama_model_get_vocab) {
 const llama_vocab* vocab = pfn_llama_model_get_vocab(model);
 if (vocab) return pfn_llama_vocab_bos(vocab);
 }
 return 0;
}

llama_token LLamaDLL::token_eos(const llama_model* model) {
 if (pfn_llama_vocab_eos && pfn_llama_model_get_vocab) {
 const llama_vocab* vocab = pfn_llama_model_get_vocab(model);
 if (vocab) return pfn_llama_vocab_eos(vocab);
 }
 return 0;
}

int32_t LLamaDLL::tokenize(const llama_model* model, const char* text, int32_t text_len, llama_token* tokens, int32_t n_max_tokens, bool add_bos, bool special) {
 if (pfn_llama_tokenize && pfn_llama_model_get_vocab) {
 const llama_vocab* vocab = pfn_llama_model_get_vocab(model);
 if (vocab) return pfn_llama_tokenize(vocab, text, text_len, tokens, n_max_tokens, add_bos, special);
 }
 return 0;
}

int32_t LLamaDLL::token_to_piece(const llama_model* model, llama_token token, char* buf, int32_t len, int32_t lstrip, bool special) {
 if (pfn_llama_token_to_piece && pfn_llama_model_get_vocab) {
 const llama_vocab* vocab = pfn_llama_model_get_vocab(model);
 if (vocab) return pfn_llama_token_to_piece(vocab, token, buf, len, lstrip, special);
 }
 return 0;
}

llama_batch LLamaDLL::batch_get_one(llama_token* tokens, int32_t n_tokens) {
 if (pfn_llama_batch_get_one) return pfn_llama_batch_get_one(tokens, n_tokens);
 return {};
}

void LLamaDLL::batch_free(llama_batch batch) {
 if (pfn_llama_batch_free) pfn_llama_batch_free(batch);
}

int32_t LLamaDLL::decode(llama_context* ctx, llama_batch batch) {
 if (pfn_llama_decode) return pfn_llama_decode(ctx, batch);
 return -1;
}

float* LLamaDLL::get_logits(llama_context* ctx) {
 if (pfn_llama_get_logits) return pfn_llama_get_logits(ctx);
 return nullptr;
}

llama_sampler* LLamaDLL::sampler_chain_init(llama_sampler_chain_params params) {
 if (pfn_llama_sampler_chain_init) return pfn_llama_sampler_chain_init(params);
 return nullptr;
}

void LLamaDLL::sampler_chain_add(llama_sampler* chain, llama_sampler* sampler) {
 if (pfn_llama_sampler_chain_add) pfn_llama_sampler_chain_add(chain, sampler);
}

void LLamaDLL::sampler_free(llama_sampler* sampler) {
 if (pfn_llama_sampler_free) pfn_llama_sampler_free(sampler);
}

void LLamaDLL::sampler_accept(llama_sampler* sampler, llama_token token) {
 if (pfn_llama_sampler_accept) pfn_llama_sampler_accept(sampler, token);
}

llama_token LLamaDLL::sampler_sample(llama_sampler* sampler, llama_context* ctx, int32_t idx) {
 if (pfn_llama_sampler_sample) return pfn_llama_sampler_sample(sampler, ctx, idx);
 return 0;
}

llama_sampler* LLamaDLL::sampler_init_temp(float temp) {
 if (pfn_llama_sampler_init_temp) return pfn_llama_sampler_init_temp(temp);
 return nullptr;
}

llama_sampler* LLamaDLL::sampler_init_top_k(int32_t k) {
 if (pfn_llama_sampler_init_top_k) return pfn_llama_sampler_init_top_k(k);
 return nullptr;
}

llama_sampler* LLamaDLL::sampler_init_top_p(float p, size_t min_keep) {
 if (pfn_llama_sampler_init_top_p) return pfn_llama_sampler_init_top_p(p, min_keep);
 return nullptr;
}

llama_sampler* LLamaDLL::sampler_init_dist(uint32_t seed) {
 if (pfn_llama_sampler_init_dist) return pfn_llama_sampler_init_dist(seed);
 return nullptr;
}

llama_sampler* LLamaDLL::sampler_init_greedy() {
 if (pfn_llama_sampler_init_greedy) return pfn_llama_sampler_init_greedy();
 return nullptr;
}

llama_sampler* LLamaDLL::sampler_init_penalties(int32_t n_prev, float penalty_repeat, float penalty_freq, float penalty_present) {
 if (pfn_llama_sampler_init_penalties) return pfn_llama_sampler_init_penalties(n_prev, penalty_repeat, penalty_freq, penalty_present);
 return nullptr;
}

void LLamaDLL::kv_cache_clear(llama_context* ctx) {
 if (pfn_llama_kv_cache_clear) pfn_llama_kv_cache_clear(ctx);
}

const char* LLamaDLL::model_desc(llama_model* model, char* buf, size_t buf_size) {
 if (pfn_llama_model_desc) return pfn_llama_model_desc(model, buf, buf_size);
 strncpy(buf, "N/A", buf_size - 1);
 buf[buf_size - 1] = '\0';
 return buf;
}

void* LLamaDLL::GetGPUDevice() {
 return nullptr; // TODO: implement GPU detection for macOS
}

bool LLamaDLL::IsModelOnGPU(llama_model* model) {
 (void)model;
 return false; // TODO: implement GPU layer check for macOS
}

#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
