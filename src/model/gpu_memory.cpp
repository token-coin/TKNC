#include <model/gpu_memory.h>
#include <util/log.h>
#include <cinttypes>
#include <cstring>
#include <sstream>
#include <array>
#include <vector>
#ifdef WIN32
#include <windows.h>
#include <stdio.h>
#include <dxgi1_6.h>
#ifdef _MSC_VER
#pragma comment(lib, "dxgi.lib")
#endif
#else
#include <cstdio>
#include <memory>
#include <dlfcn.h>
#endif

bool GPUMemoryManager::DetectPrimaryGPU(GPUDeviceInfo& out_info) {
    out_info = GPUDeviceInfo();
#ifdef WIN32
    IDXGIFactory6* pFactory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), (void**)&pFactory))) {
        LogInfo("GPUMemoryManager: DirectX GPU detection - CreateDXGIFactory1 failed");
        return false;
    }

    UINT adapterIndex = 0;
    IDXGIAdapter4* pAdapter = nullptr;
    bool found = false;
    UINT64 best_vram = 0;  // Select GPU with most VRAM (prefer dGPU over iGPU)

    while (SUCCEEDED(pFactory->EnumAdapters1(adapterIndex, (IDXGIAdapter1**)&pAdapter))) {
        DXGI_ADAPTER_DESC3 desc;
        if (SUCCEEDED(pAdapter->GetDesc3(&desc))) {
            if (desc.DedicatedVideoMemory > 0 && !(static_cast<UINT>(desc.Flags) & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                // Pick the adapter with the largest VRAM (dGPU > iGPU)
                if (desc.DedicatedVideoMemory > best_vram) {
                    best_vram = desc.DedicatedVideoMemory;
                    char gpu_name[256];
                    WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, gpu_name, 256, NULL, NULL);
                    out_info.name = gpu_name;
                    out_info.total_memory_mb = desc.DedicatedVideoMemory / (1024 * 1024);
                    out_info.backend_type = GPUBackend::NONE;
                    out_info.is_available = true;
                    found = true;
                    LogInfo("GPUMemoryManager: DirectX detected GPU: %s (%" PRIu64 " MB)",
                            out_info.name.c_str(), out_info.total_memory_mb);
                }
            }
        }
        pAdapter->Release();
        adapterIndex++;
    }

    pFactory->Release();
    return found;
#else
    // Linux: use nvidia-smi for GPU detection
    LogInfo("GPUMemoryManager: Linux GPU detection via nvidia-smi...");
    FILE* pipe = popen("nvidia-smi --query-gpu=name,memory.total --format=csv,noheader,nounits 2>/dev/null", "r");
    if (pipe) {
        char buffer[512] = {0};
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            buffer[strcspn(buffer, "\r\n")] = 0;
            char* comma = strchr(buffer, ',');
            if (comma) {
                *comma = '\0';
                out_info.name = buffer;
                out_info.total_memory_mb = strtoull(comma + 1, nullptr, 10);
                out_info.backend_type = GPUBackend::CUDA;
                out_info.is_available = true;
                pclose(pipe);
                LogInfo("GPUMemoryManager: Linux detected GPU: %s (%" PRIu64 " MB)",
                        out_info.name.c_str(), out_info.total_memory_mb);
                return true;
            }
        }
        pclose(pipe);
    }
    LogInfo("GPUMemoryManager: No GPU detected on Linux");
    return false;
#endif
}

GPUMemoryManager::GPUMemoryManager()
    : model_memory(0), request_memory(0), total_memory(0), max_concurrent_requests(0)
    , active_backend(GPUBackend::NONE), multi_gpu_initialized(false) {
}

static GPUBackend DetectGPUBackend() {
    LogInfo("GPUMemoryManager: Detecting GPU backend (DLL probe)...");
    std::cerr << "[GPU-DIAG] === Starting GPU Backend Detection ===" << std::endl;

#ifdef WIN32
    // --- DXGI enumeration (informational only, for diagnostics) ---
    std::string vendor;
    IDXGIFactory6* pFactory = nullptr;
    HRESULT hrFactory = CreateDXGIFactory1(__uuidof(IDXGIFactory6), (void**)&pFactory);
    if (SUCCEEDED(hrFactory)) {
        IDXGIAdapter4* pAdapter = nullptr;
        UINT adapterIndex = 0;
        int totalAdapters = 0, skippedAdapters = 0;
        UINT64 best_vram = 0;  // Track best GPU for vendor determination
        while (SUCCEEDED(pFactory->EnumAdapters1(adapterIndex, (IDXGIAdapter1**)&pAdapter))) {
            totalAdapters++;
            DXGI_ADAPTER_DESC3 desc;
            if (SUCCEEDED(pAdapter->GetDesc3(&desc))) {
                char name[256] = {0};
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), NULL, NULL);
                std::cerr << "[GPU-DIAG] DXGI Adapter#" << adapterIndex << ": \"" << name
                          << "\" VendorID=0x" << std::hex << desc.VendorId << std::dec
                          << " VRAM=" << (desc.DedicatedVideoMemory / (1024*1024)) << "MB" << std::endl;

                if (desc.DedicatedVideoMemory > 0 && !(static_cast<UINT>(desc.Flags) & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    // Determine vendor from the GPU with most VRAM (prefer dGPU)
                    if (desc.DedicatedVideoMemory > best_vram) {
                        best_vram = desc.DedicatedVideoMemory;
                        if (desc.VendorId == 0x10DE) vendor = "NVIDIA";
                        else if (desc.VendorId == 0x1002 || desc.VendorId == 0x1022) vendor = "AMD";
                        else if (desc.VendorId != 0x8086)
                            vendor = "Unknown(0x" + std::to_string(desc.VendorId) + ")";
                    }
                } else {
                    skippedAdapters++;
                }
            }
            pAdapter->Release();
            adapterIndex++;
        }
        pFactory->Release();
        std::cerr << "[GPU-DIAG] DXGI: " << totalAdapters << " adapters, "
                  << skippedAdapters << " skipped, vendor=\"" << vendor << "\"" << std::endl;
    } else {
        std::cerr << "[GPU-DIAG] DXGI unavailable (old driver/VM)" << std::endl;
    }

    // Direct DLL probe: select order based on DXGI-detected vendor to match llama.cpp's auto-selection.
    const char* forced_backend = getenv("FORCE_GPU_BACKEND");
    if (forced_backend && (strcmp(forced_backend, "CUDA") == 0 || strcmp(forced_backend, "cuda") == 0)) {
        std::cerr << "[GPU-DIAG] FORCE_GPU_BACKEND=CUDA detected, forcing CUDA..." << std::endl;
        HMODULE hCuda = LoadLibraryA("ggml-cuda.dll");
        if (hCuda) {
            // Do NOT FreeLibrary — GGML's DllMain sets global state (abort handler)
            // that persists after FreeLibrary. If LLamaDLL::Load later re-initializes
            // GGML, it triggers: GGML_ASSERT(prev != ggml_uncaught_exception) failed.
            // Keeping the DLL resident avoids the double-initialization crash.
            LogInfo("GPUMemoryManager: Forced CUDA backend via env var (DLL kept resident)");
            std::cerr << "[GPU-DIAG] OK => CUDA backend selected (FORCED)" << std::endl;
            return GPUBackend::CUDA;
        }
        DWORD cudaErr = GetLastError();
        std::cerr << "[GPU-DIAG] FAILED: ggml-cuda.dll not found (err=" << cudaErr << "), falling back to auto-detect" << std::endl;
        // Continue with normal detection
    }

    bool is_nvidia = (vendor == "NVIDIA");
    bool is_amd = (vendor == "AMD");
    std::cerr << "[GPU-DIAG] Probing DLLs (vendor='" << vendor << "', prefer "
              << (is_nvidia ? "CUDA" : (is_amd ? "Vulkan" : "auto")) << ")..." << std::endl;

    DWORD cudaErr = 0;
    DWORD vulkanErr = 0;

    if (is_nvidia) {
        // NVIDIA: probe CUDA FIRST to match llama.cpp's auto-selection
        std::cerr << "[GPU-DIAG] LoadLibrary(ggml-cuda.dll)... ";
        HMODULE hCuda = LoadLibraryA("ggml-cuda.dll");
        if (hCuda) {
            // Do NOT FreeLibrary — see comment in FORCE_GPU_BACKEND block above.
            LogInfo("GPUMemoryManager: ggml-cuda.dll loaded -> CUDA backend (DLL kept resident)");
            std::cerr << "OK => CUDA backend selected" << std::endl;
            return GPUBackend::CUDA;
        }
        cudaErr = GetLastError();
        std::cerr << "FAILED (err=" << cudaErr << ")" << std::endl;

        // Fallback to Vulkan
        std::cerr << "[GPU-DIAG] LoadLibrary(ggml-vulkan.dll)... ";
        HMODULE hVulkan = LoadLibraryA("ggml-vulkan.dll");
        if (hVulkan) {
            // Do NOT FreeLibrary — same reason as above.
            LogInfo("GPUMemoryManager: ggml-vulkan.dll loaded -> Vulkan backend (DLL kept resident)");
            std::cerr << "OK => Vulkan backend selected" << std::endl;
            return GPUBackend::VULKAN;
        }
        vulkanErr = GetLastError();
        std::cerr << "FAILED (err=" << vulkanErr << ")" << std::endl;
    } else {
        // AMD or unknown: probe Vulkan FIRST (broader compatibility)
        std::cerr << "[GPU-DIAG] LoadLibrary(ggml-vulkan.dll)... ";
        HMODULE hVulkan = LoadLibraryA("ggml-vulkan.dll");
        if (hVulkan) {
            // Do NOT FreeLibrary — GGML DllMain global state crash, see above.
            LogInfo("GPUMemoryManager: ggml-vulkan.dll loaded -> Vulkan backend (DLL kept resident)");
            std::cerr << "OK => Vulkan backend selected" << std::endl;
            return GPUBackend::VULKAN;
        }
        vulkanErr = GetLastError();
        std::cerr << "FAILED (err=" << vulkanErr << ")" << std::endl;

        // Fallback to CUDA
        std::cerr << "[GPU-DIAG] LoadLibrary(ggml-cuda.dll)... ";
        HMODULE hCuda = LoadLibraryA("ggml-cuda.dll");
        if (hCuda) {
            // Do NOT FreeLibrary — same reason as above.
            LogInfo("GPUMemoryManager: ggml-cuda.dll loaded -> CUDA backend (DLL kept resident)");
            std::cerr << "OK => CUDA backend selected" << std::endl;
            return GPUBackend::CUDA;
        }
        cudaErr = GetLastError();
        std::cerr << "FAILED (err=" << cudaErr << ")" << std::endl;
    }

    // Both failed
    LogError("GPUMemoryManager: FATAL: No GPU backend DLL could be loaded!");
    LogError("GPUMemoryManager:   ggml-cuda.dll error=%lu  ggml-vulkan.dll error=%lu", cudaErr, vulkanErr);
    LogError("GPUMemoryManager: Supported GPUs: NVIDIA (CUDA), AMD (Vulkan). CPU NOT supported.");
    std::cerr << "[GPU-DIAG] FATAL: Neither DLL loaded! CUDA err=" << cudaErr
              << " Vulkan err=" << vulkanErr << std::endl;
    return GPUBackend::NONE;

#else
# ifdef __APPLE__
    std::cerr << "[GPU-DIAG] Probing macOS GPU backends..." << std::endl;

    void* opencl_framework = dlopen("/System/Library/Frameworks/OpenCL.framework/OpenCL", RTLD_LAZY);
    if (opencl_framework) {
        dlclose(opencl_framework);
        LogInfo("GPUMemoryManager: OpenCL.framework loaded -> OpenCL backend (Apple Silicon/M2)");
        std::cerr << "[GPU-DIAG] OpenCL.framework OK => Apple Silicon GPU detected" << std::endl;
        return GPUBackend::VULKAN;
    }

    LogError("GPUMemoryManager: OpenCL.framework not found");
    return GPUBackend::NONE;
# else
    std::cerr << "[GPU-DIAG] Probing DLLs (non-Windows)..." << std::endl;
    void* hCuda = dlopen("./ggml-cuda.dll", RTLD_NOW | RTLD_LOCAL);
    if (hCuda) { dlclose(hCuda); return GPUBackend::CUDA; }
    void* hVulkan = dlopen("./ggml-vulkan.dll", RTLD_NOW | RTLD_LOCAL);
    if (hVulkan) { dlclose(hVulkan); return GPUBackend::VULKAN; }
    return GPUBackend::NONE;
# endif
#endif
}

bool GPUMemoryManager::InitializeGPU(int device_id) {
    std::lock_guard<std::mutex> lock(memory_mutex);

    LogInfo("GPUMemoryManager: Initializing GPU device %d...", device_id);

    GPUBackend backend = DetectGPUBackend();

    // FATAL: No supported GPU backend - exit immediately
    if (backend == GPUBackend::NONE) {
        LogError("GPUMemoryManager: FATAL ERROR: No supported GPU backend available!");
        LogError("GPUMemoryManager: TKNC Miner requires a NVIDIA (CUDA) or AMD (Vulkan) GPU.");
        LogError("GPUMemoryManager: CPU-only mode is NOT supported.");
        LogError("GPUMemoryManager: Please ensure:");
        LogError("GPUMemoryManager:   1. A NVIDIA or AMD GPU is installed with working drivers");
        LogError("GPUMemoryManager:   2. For NVIDIA: ggml-cuda.dll + cudart64_12.dll + cublas64_12.dll in miner directory");
        LogError("GPUMemoryManager:   3. For AMD: ggml-vulkan.dll + vulkan-1.dll in miner directory");
        return false;
    }

    const char* backend_name = "";
    switch (backend) {
        case GPUBackend::CUDA:   backend_name = "CUDA (NVIDIA)"; break;
        case GPUBackend::VULKAN: backend_name = "Vulkan (AMD)"; break;
        default:                 backend_name = "Unknown"; break;
    }
    LogInfo("GPUMemoryManager: GPU Backend: %s", backend_name);

    // Get VRAM via DXGI (no external tools needed)
#ifdef WIN32
    total_memory = 0;
    IDXGIFactory6* pFactory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), (void**)&pFactory))) {
        IDXGIAdapter4* pAdapter = nullptr;
        UINT adapterIndex = 0;
        UINT64 best_vram = 0;  // Select GPU with most VRAM (prefer dGPU over iGPU)
        while (SUCCEEDED(pFactory->EnumAdapters1(adapterIndex, (IDXGIAdapter1**)&pAdapter))) {
            DXGI_ADAPTER_DESC3 desc;
            if (SUCCEEDED(pAdapter->GetDesc3(&desc))) {
                if (desc.DedicatedVideoMemory > 0 && !(static_cast<UINT>(desc.Flags) & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    // Pick the adapter with the largest VRAM
                    if (desc.DedicatedVideoMemory > best_vram) {
                        best_vram = desc.DedicatedVideoMemory;
                        char name[256] = {0};
                        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), NULL, NULL);
                        total_memory = desc.DedicatedVideoMemory;

                        GPUDeviceInfo dev;
                        dev.device_id = adapterIndex;
                        dev.backend_type = backend;
                        dev.name = std::string(name);
                        dev.total_memory_mb = total_memory / (1024 * 1024);
                        dev.free_memory_mb = 0;  // Will be updated by llama.cpp
                        dev.is_available = true;
                        gpu_devices.clear();  // Replace previous entry
                        gpu_devices.push_back(dev);

                        LogInfo("GPUMemoryManager: GPU detected: %s (%" PRIu64 " MB VRAM)",
                                name, total_memory / (1024 * 1024));
                    }
                }
            }
            pAdapter->Release();
            adapterIndex++;
        }
        pFactory->Release();
    } else {
        LogWarning("GPUMemoryManager: DXGI factory creation failed, VRAM unknown");
        total_memory = 0;
    }
#elif defined(__APPLE__)
    total_memory = 0;

    typedef unsigned int cl_uint;
    typedef int cl_int;
    typedef unsigned long cl_ulong;
    typedef size_t cl_device_info;
    typedef void* cl_platform_id;
    typedef void* cl_device_id;
    typedef unsigned int cl_device_type;

    const cl_int CL_SUCCESS = 0;
    const cl_device_type CL_DEVICE_TYPE_GPU = 1 << 1;
    const cl_device_info CL_DEVICE_NAME = 0x102B;
    const cl_device_info CL_DEVICE_GLOBAL_MEM_SIZE = 0x101F;

    void* opencl_framework = dlopen("/System/Library/Frameworks/OpenCL.framework/OpenCL", RTLD_LAZY);
    if (opencl_framework) {
        typedef cl_int (*clGetPlatformIDs_fn)(cl_uint, cl_platform_id*, cl_uint*);
        typedef cl_int (*clGetDeviceIDs_fn)(cl_platform_id, cl_device_type, cl_uint, cl_device_id*, cl_uint*);
        typedef cl_int (*clGetDeviceInfo_fn)(cl_device_id, cl_device_info, size_t, void*, size_t*);

        clGetPlatformIDs_fn clGetPlatformIDs = (clGetPlatformIDs_fn)dlsym(opencl_framework, "clGetPlatformIDs");
        clGetDeviceIDs_fn clGetDeviceIDs = (clGetDeviceIDs_fn)dlsym(opencl_framework, "clGetDeviceIDs");
        clGetDeviceInfo_fn clGetDeviceInfo = (clGetDeviceInfo_fn)dlsym(opencl_framework, "clGetDeviceInfo");

        if (clGetPlatformIDs && clGetDeviceIDs && clGetDeviceInfo) {
            cl_uint num_platforms = 0;
            if (clGetPlatformIDs(0, nullptr, &num_platforms) == CL_SUCCESS && num_platforms > 0) {
                cl_platform_id* platforms = new cl_platform_id[num_platforms];
                if (clGetPlatformIDs(num_platforms, platforms, nullptr) == CL_SUCCESS) {
                    for (cl_uint i = 0; i < num_platforms; i++) {
                        cl_uint num_devices = 0;
                        if (clGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_GPU, 0, nullptr, &num_devices) == CL_SUCCESS && num_devices > 0) {
                            cl_device_id* devices = new cl_device_id[num_devices];
                            if (clGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_GPU, num_devices, devices, nullptr) == CL_SUCCESS) {
                                for (cl_uint j = 0; j < num_devices; j++) {
                                    char name[256] = {0};
                                    size_t name_len = 0;
                                    clGetDeviceInfo(devices[j], CL_DEVICE_NAME, sizeof(name), name, &name_len);

                                    cl_ulong vram = 0;
                                    clGetDeviceInfo(devices[j], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(vram), &vram, nullptr);

                                    GPUDeviceInfo dev;
                                    dev.device_id = j;
                                    dev.backend_type = backend;
                                    dev.name = std::string(name);
                                    dev.total_memory_mb = vram / (1024 * 1024);
                                    dev.free_memory_mb = 0;
                                    dev.is_available = true;
                                    gpu_devices.push_back(dev);

                                    total_memory = vram;
                                    LogInfo("GPUMemoryManager: GPU detected: %s (%" PRIu64 " MB VRAM)",
                                            name, vram / (1024 * 1024));
                                }
                            }
                            delete[] devices;
                        }
                    }
                }
                delete[] platforms;
            }
        }
        dlclose(opencl_framework);
    }
#else
    total_memory = 0;
#endif

    active_backend = backend;
    multi_gpu_initialized = !gpu_devices.empty();

    LogInfo("GPUMemoryManager: GPU initialized successfully via %s", backend_name);
    if (total_memory > 0) {
        LogInfo("GPUMemoryManager: Total VRAM: %" PRIu64 " MB", total_memory / (1024 * 1024));
    }

    return true;
}

int GPUMemoryManager::CalculateMaxConcurrentRequests(uint64_t model_size, uint64_t per_request_size) {
    std::lock_guard<std::mutex> lock(memory_mutex);

    if (per_request_size == 0 || total_memory == 0) {
        return 0;
    }

    uint64_t available_memory = total_memory - model_size;
    int max_requests = static_cast<int>(available_memory / per_request_size);

    max_concurrent_requests = max_requests;

    LogInfo("GPUMemoryManager: Max concurrent requests: %d (Available VRAM: %" PRIu64 " MB, Per request: %" PRIu64 " MB)",
              max_requests, available_memory / (1024 * 1024), per_request_size / (1024 * 1024));

    return max_requests;
}

bool GPUMemoryManager::AllocateRequestMemory(uint64_t size) {
    std::lock_guard<std::mutex> lock(memory_mutex);

    if (request_memory + size > total_memory - model_memory) {
        LogInfo("GPUMemoryManager: Insufficient VRAM, cannot allocate %" PRIu64 " bytes", size);
        return false;
    }

    request_memory += size;
    LogInfo("GPUMemoryManager: Allocated VRAM %" PRIu64 " bytes, current usage: %" PRIu64 " bytes",
              size, request_memory);
    return true;
}

void GPUMemoryManager::FreeRequestMemory(uint64_t size) {
    std::lock_guard<std::mutex> lock(memory_mutex);

    if (request_memory >= size) {
        request_memory -= size;
        LogInfo("GPUMemoryManager: Freed VRAM %" PRIu64 " bytes, current usage: %" PRIu64 " bytes",
                  size, request_memory);
    }
}

bool GPUMemoryManager::EnumerateAllGPUs() {
    std::lock_guard<std::mutex> lock(memory_mutex);

    gpu_devices.clear();
    multi_gpu_initialized = false;

    LogInfo("GPUMemoryManager: Starting GPU enumeration...");

#ifdef WIN32
    HANDLE hReadPipe, hWritePipe;
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hReadPipe, &hWritePipe, &saAttr, 0)) {
        LogInfo("GPUMemoryManager: Failed to create pipe for enumeration");
        return false;
    }

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.dwFlags |= STARTF_USESTDHANDLES;
    ZeroMemory(&pi, sizeof(pi));

    std::string cmd = "nvidia-smi --query-gpu=index,name,memory.total,memory.free,uuid,compute_cap,multiprocessors,clocks.max.graphics,clocks.max.mem --format=csv,noheader";

    BOOL success = CreateProcessA(
        NULL,
        const_cast<char*>(cmd.c_str()),
        NULL,
        NULL,
        TRUE,
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &si,
        &pi
    );

    if (success) {
        CloseHandle(hWritePipe);

        std::vector<char> buffer(8192);  // Heap allocation to avoid stack overflow
        DWORD bytesRead;
        std::string output;

        while (ReadFile(hReadPipe, buffer.data(), static_cast<DWORD>(buffer.size()) - 1, &bytesRead, NULL) && bytesRead > 0) {
            output.append(buffer.data(), bytesRead);
        }

        CloseHandle(hReadPipe);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if (!output.empty() && output.find("ERROR") == std::string::npos) {
            active_backend = GPUBackend::CUDA;

            std::istringstream iss(output);
            std::string line;
            int device_index = 0;

            while (std::getline(iss, line)) {
                if (line.empty()) continue;

                GPUDeviceInfo device;
                device.device_id = device_index++;
                device.backend_type = GPUBackend::CUDA;
                device.is_available = true;

                std::vector<std::string> tokens;
                std::stringstream ss(line);
                std::string token;

                while (std::getline(ss, token, ',')) {
                    tokens.push_back(token);
                }

                if (tokens.size() >= 3) {
                    try { device.device_id = std::stoi(tokens[0]); } catch (...) {}
                    device.name = tokens[1];

                    try {
                        std::string mem_str = tokens[2];
                        mem_str.erase(std::remove_if(mem_str.begin(), mem_str.end(), ::isspace), mem_str.end());
                        device.total_memory_mb = std::stoull(mem_str);
                    } catch (...) { device.total_memory_mb = 0; }

                    try {
                        std::string free_mem_str = tokens[3];
                        free_mem_str.erase(std::remove_if(free_mem_str.begin(), free_mem_str.end(), ::isspace), free_mem_str.end());
                        device.free_memory_mb = std::stoull(free_mem_str);
                    } catch (...) { device.free_memory_mb = 0; }

                    if (tokens.size() >= 5) device.uuid = tokens[4];

                    if (tokens.size() >= 6) {
                        std::string cc = tokens[5];
                        size_t dot_pos = cc.find('.');
                        if (dot_pos != std::string::npos) {
                            try { device.compute_capability_major = std::stoi(cc.substr(0, dot_pos)); } catch (...) {}
                            try { device.compute_capability_minor = std::stoi(cc.substr(dot_pos + 1)); } catch (...) {}
                        }
                    }

                    if (tokens.size() >= 7) {
                        try { device.multiprocessor_count = std::stoi(tokens[6]); } catch (...) {}
                    }

                    if (tokens.size() >= 8) {
                        try {
                            std::string clock_str = tokens[7];
                            clock_str.erase(std::remove_if(clock_str.begin(), clock_str.end(), ::isspace), clock_str.end());
                            device.max_clock_rate_mhz = std::stoi(clock_str);
                        } catch (...) {}
                    }

                    if (tokens.size() >= 9) {
                        try {
                            std::string mem_clock_str = tokens[8];
                            mem_clock_str.erase(std::remove_if(mem_clock_str.begin(), mem_clock_str.end(), ::isspace), mem_clock_str.end());
                            device.memory_clock_rate_mhz = std::stoi(mem_clock_str);
                        } catch (...) {}
                    }

                    gpu_devices.push_back(device);

                    LogInfo("GPUMemoryManager: Found CUDA GPU #%d: %s (%" PRIu64 " MB VRAM)",
                              device.device_id, device.name.c_str(), device.total_memory_mb);
                }
            }
            LogInfo("GPUMemoryManager: Total NVIDIA GPUs detected: %zu", gpu_devices.size());
        }
    } else {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);

        cmd = "rocm-smi --showallinfo --json";
        success = CreateProcessA(
            NULL,
            const_cast<char*>(cmd.c_str()),
            NULL,
            NULL,
            TRUE,
            CREATE_NO_WINDOW,
            NULL,
            NULL,
            &si,
            &pi
        );

        if (success) {
            CloseHandle(hWritePipe);

            std::vector<char> buffer(8192);  // Heap allocation to avoid stack overflow
            DWORD bytesRead;
            std::string output;

            while (ReadFile(hReadPipe, buffer.data(), static_cast<DWORD>(buffer.size()) - 1, &bytesRead, NULL) && bytesRead > 0) {
                output.append(buffer.data(), bytesRead);
            }

            CloseHandle(hReadPipe);
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);

            active_backend = GPUBackend::ROCm;

            GPUDeviceInfo amd_device;
            amd_device.device_id = 0;
            amd_device.backend_type = GPUBackend::ROCm;
            amd_device.name = "AMD GPU (ROCm)";
            amd_device.total_memory_mb = 0;
            amd_device.free_memory_mb = 0;
            amd_device.is_available = true;

            gpu_devices.push_back(amd_device);

            LogInfo("GPUMemoryManager: Found AMD GPU via ROCm: %s (%" PRIu64 " MB VRAM)",
                      amd_device.name.c_str(), amd_device.total_memory_mb);
        }
    }
#else
    FILE* pipe = popen("nvidia-smi --query-gpu=index,name,memory.total,memory.free --format=csv,noheader", "r");
    if (pipe) {
        char buffer[1024];
        int idx = 0;

        active_backend = GPUBackend::CUDA;

        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            buffer[strcspn(buffer, "\r\n")] = 0;

            GPUDeviceInfo device;
            device.device_id = idx++;
            device.backend_type = GPUBackend::CUDA;
            device.is_available = true;

            std::vector<std::string> tokens;
            std::stringstream ss(buffer);
            std::string token;

            while (std::getline(ss, token, ',')) {
                tokens.push_back(token);
            }

            if (tokens.size() >= 3) {
                try { device.device_id = std::stoi(tokens[0]); } catch (...) {}
                device.name = tokens[1];
                try { device.total_memory_mb = std::stoull(tokens[2]); } catch (...) {}
                try { device.free_memory_mb = std::stoull(tokens[3]); } catch (...) {}

                gpu_devices.push_back(device);

                LogInfo("GPUMemoryManager: Found CUDA GPU #%d: %s (%" PRIu64 " MB VRAM)",
                          device.device_id, device.name.c_str(), device.total_memory_mb);
            }
        }

        pclose(pipe);
        LogInfo("GPUMemoryManager: Total NVIDIA GPUs detected: %zu", gpu_devices.size());
    }

    pipe = popen("rocm-smi --showmeminfo vram --unit MiB 2>/dev/null | head -5", "r");
    if (pipe && gpu_devices.empty()) {
        char buffer[512];
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            pclose(pipe);

            active_backend = GPUBackend::ROCm;

            GPUDeviceInfo amd_device;
            amd_device.device_id = 0;
            amd_device.backend_type = GPUBackend::ROCm;
            amd_device.name = "AMD GPU (ROCm)";
            amd_device.total_memory_mb = 0;
            amd_device.free_memory_mb = 0;
            amd_device.is_available = true;

            gpu_devices.push_back(amd_device);

            LogInfo("GPUMemoryManager: Found AMD GPU via ROCm: %s", amd_device.name.c_str());
        } else {
            pclose(pipe);
        }
    }
#endif

    multi_gpu_initialized = !gpu_devices.empty();

    if (multi_gpu_initialized) {
        uint64_t total_vram = 0;
        for (const auto& gpu : gpu_devices) {
            total_vram += gpu.total_memory_mb;
        }
        LogInfo("GPUMemoryManager: Multi-GPU initialization complete");
        LogInfo("GPUMemoryManager:   Total GPUs: %zu", gpu_devices.size());
        LogInfo("GPUMemoryManager:   Total VRAM across all GPUs: %" PRIu64 " MB", total_vram);
        LogInfo("GPUMemoryManager:   Active Backend: %s",
                  active_backend == GPUBackend::CUDA ? "CUDA (NVIDIA)" :
                  active_backend == GPUBackend::ROCm ? "ROCm (AMD)" : "Unknown");
    } else {
        LogInfo("GPUMemoryManager: WARNING: No GPUs detected via enumeration");
        LogInfo("GPUMemoryManager: Will attempt single-GPU initialization as fallback");
    }

    return multi_gpu_initialized;
}

const GPUDeviceInfo* GPUMemoryManager::GetGPUDeviceInfo(int device_id) const {
    std::lock_guard<std::mutex> lock(memory_mutex);

    for (const auto& device : gpu_devices) {
        if (device.device_id == device_id) {
            return &device;
        }
    }

    return nullptr;
}

int GPUMemoryManager::CalculateOptimalGPULayers(int device_id, uint64_t model_size_mb, int total_layers) {
    std::lock_guard<std::mutex> lock(memory_mutex);

    const GPUDeviceInfo* device = nullptr;

    for (const auto& d : gpu_devices) {
        if (d.device_id == device_id) {
            device = &d;
            break;
        }
    }

    if (!device || !device->is_available) {
        LogInfo("GPUMemoryManager: Device %d not found or unavailable, using default calculation", device_id);
        if (total_memory > 0) {
            uint64_t available_mb = (total_memory / (1024 * 1024)) * 80 / 100;
            uint64_t per_layer_mb = model_size_mb / total_layers;
            if (per_layer_mb == 0) per_layer_mb = 100;
            int optimal_layers = static_cast<int>(available_mb / per_layer_mb);
            return std::min(optimal_layers, total_layers);
        }
        return std::min(total_layers, 24);
    }

    uint64_t available_mb = device->free_memory_mb * 80 / 100;

    uint64_t per_layer_mb = model_size_mb / total_layers;
    if (per_layer_mb == 0) per_layer_mb = 100;

    int optimal_layers = static_cast<int>(available_mb / per_layer_mb);
    optimal_layers = std::max(1, optimal_layers);
    optimal_layers = std::min(optimal_layers, total_layers);

    LogInfo("GPUMemoryManager: Optimal GPU layers for device %d (%s): %d/%d",
              device_id, device->name.c_str(), optimal_layers, total_layers);
    LogInfo("GPUMemoryManager:   Model size: %" PRIu64 " MB, Available VRAM: %" PRIu64 " MB (80%%)",
              model_size_mb, available_mb);
    LogInfo("GPUMemoryManager:   Estimated per-layer: %" PRIu64 " MB", per_layer_mb);

    return optimal_layers;
}

int GPUMemoryManager::CalculateOptimalGPULayersForAllGPUs(uint64_t model_size_mb, int total_layers) {
    std::lock_guard<std::mutex> lock(memory_mutex);

    if (gpu_devices.empty()) {
        LogInfo("GPUMemoryManager: No GPUs enumerated, using default value");
        return std::min(total_layers, 24);
    }

    int min_optimal_layers = total_layers;

    for (const auto& device : gpu_devices) {
        if (device.is_available) {
            uint64_t available_mb = device.free_memory_mb * 80 / 100;
            uint64_t per_layer_mb = model_size_mb / total_layers;
            if (per_layer_mb == 0) per_layer_mb = 100;

            int optimal_for_device = static_cast<int>(available_mb / per_layer_mb);
            optimal_for_device = std::max(1, optimal_for_device);
            optimal_for_device = std::min(optimal_for_device, total_layers);

            min_optimal_layers = std::min(min_optimal_layers, optimal_for_device);

            LogInfo("GPUMemoryManager: Device %d (%s): optimal layers = %d",
                      device.device_id, device.name.c_str(), optimal_for_device);
        }
    }

    LogInfo("GPUMemoryManager: Final optimal layers for all GPUs: %d", min_optimal_layers);

    return min_optimal_layers;
}

GPUStats GPUMemoryManager::GetGPUStats(int device_id) const {
    std::lock_guard<std::mutex> lock(memory_mutex);

    GPUStats stats;
    stats.device_id = device_id;

    if (!gpu_devices.empty() && device_id < static_cast<int>(gpu_devices.size())) {
        const auto& device = gpu_devices[device_id];
        stats.name = device.name;
        stats.memory.total = device.total_memory_mb * 1024 * 1024;
        stats.memory.free = device.free_memory_mb * 1024 * 1024;
        stats.memory.used = stats.memory.total - stats.memory.free;
        stats.memory.usage_percent = static_cast<float>(stats.memory.used) / static_cast<float>(stats.memory.total) * 100.0f;
    } else {
        stats.name = "GPU (auto-detected)";
        stats.usage_percent = static_cast<float>(request_memory) / static_cast<float>(total_memory) * 100.0f;
        stats.memory.total = total_memory;
        stats.memory.used = model_memory + request_memory;
        stats.memory.free = total_memory - stats.memory.used;
        stats.memory.usage_percent = static_cast<float>(stats.memory.used) / static_cast<float>(total_memory) * 100.0f;
    }

    stats.temperature = 0.0f;
    stats.power_usage = 0;

    return stats;
}

uint64_t GPUMemoryManager::GetFreeMemory(int device_id) const {
    std::lock_guard<std::mutex> lock(memory_mutex);

    if (!gpu_devices.empty() && device_id < static_cast<int>(gpu_devices.size())) {
        return gpu_devices[device_id].free_memory_mb * 1024 * 1024;
    }

    return total_memory - model_memory - request_memory;
}

bool GPUMemoryManager::HasEnoughMemory(uint64_t required_size, int device_id) const {
    std::lock_guard<std::mutex> lock(memory_mutex);

    if (!gpu_devices.empty() && device_id < static_cast<int>(gpu_devices.size())) {
        return gpu_devices[device_id].free_memory_mb * 1024 * 1024 >= required_size;
    }

    return (total_memory - model_memory - request_memory) >= required_size;
}

uint64_t GPUMemoryManager::GetTotalVRAMAcrossAllGPUs() const {
    std::lock_guard<std::mutex> lock(memory_mutex);

    uint64_t total_vram = 0;
    for (const auto& gpu : gpu_devices) {
        total_vram += gpu.total_memory_mb;
    }

    return total_vram * 1024 * 1024;
}

void GPUMemoryManager::PrintGPUInfoSummary() const {
    std::lock_guard<std::mutex> lock(memory_mutex);

    LogDebug(BCLog::ALL, "TKNC GPU: backend=%s gpus=%zu",
              active_backend == GPUBackend::CUDA ? "CUDA" :
              active_backend == GPUBackend::ROCm ? "ROCm" :
              active_backend == GPUBackend::METAL ? "Metal" : "None",
              gpu_devices.size());

    for (size_t i = 0; i < gpu_devices.size(); i++) {
        const auto& gpu = gpu_devices[i];
        LogDebug(BCLog::ALL, "  GPU #%d: %s %" PRIu64 "MB free",
                  i, gpu.name.c_str(), gpu.free_memory_mb);
    }

    LogDebug(BCLog::ALL, "Total VRAM: %" PRIu64 " MB", GetTotalVRAMAcrossAllGPUs() / (1024 * 1024));
}

bool GPUMemoryManager::nvml_initialized = false;

bool GPUMemoryManager::InitializeNVML() {
    if (nvml_initialized) {
        return true;
    }

#ifdef WIN32
    HMODULE nvml_dll = LoadLibraryA("nvml.dll");
    if (!nvml_dll) {
        LogInfo("GPUMemoryManager: NVML library not found (nvml.dll)");
        return false;
    }

    typedef unsigned int (*nvmlInit_t)(void);

    union { FARPROC proc; nvmlInit_t fn; } u1, u2;
    u1.proc = GetProcAddress(nvml_dll, "nvmlInit_v2");
    nvmlInit_t nvmlInit = u1.fn;
    if (!nvmlInit) {
        u2.proc = GetProcAddress(nvml_dll, "nvmlInit");
        nvmlInit = u2.fn;
    }

    if (!nvmlInit) {
        FreeLibrary(nvml_dll);
        LogInfo("GPUMemoryManager: Failed to get nvmlInit function");
        return false;
    }

    unsigned int result = nvmlInit();
    if (result != 0) {
        FreeLibrary(nvml_dll);
        LogInfo("GPUMemoryManager: nvmlInit failed with error %u", result);
        return false;
    }

    nvml_initialized = true;
    LogInfo("GPUMemoryManager: NVML initialized successfully");
    return true;
#else
    LogInfo("GPUMemoryManager: NVML control only supported on Windows");
    return false;
#endif
}

void GPUMemoryManager::ShutdownNVML() {
    if (!nvml_initialized) {
        return;
    }

    for (auto& state : gpu_control_states) {
        if (state.is_controlled) {
            ResetGPUClocks(state.device_id);
        }
    }

#ifdef WIN32
    HMODULE nvml_dll = GetModuleHandleA("nvml.dll");
    if (nvml_dll) {
        typedef unsigned int (*nvmlShutdown_t)(void);
        union { FARPROC proc; nvmlShutdown_t fn; } u;
        u.proc = GetProcAddress(nvml_dll, "nvmlShutdown");
        nvmlShutdown_t nvmlShutdown = u.fn;
        if (nvmlShutdown) {
            nvmlShutdown();
        }
    }
#endif

    nvml_initialized = false;
    gpu_control_states.clear();
    LogInfo("GPUMemoryManager: NVML shutdown complete");
}

bool GPUMemoryManager::SetGPUFrequency(int device_id, int core_clock_mhz, int mem_clock_mhz) {
    std::lock_guard<std::mutex> lock(memory_mutex);

    if (!InitializeNVML()) {
        LogInfo("GPUMemoryManager: Cannot set GPU frequency - NVML not available");
        return false;
    }

#ifdef WIN32
    HMODULE nvml_dll = GetModuleHandleA("nvml.dll");
    if (!nvml_dll) return false;

    typedef unsigned int (*nvmlSetApplicationsClocks_t)(unsigned int, unsigned int, unsigned int);
    union { FARPROC proc; nvmlSetApplicationsClocks_t fn; } u3;
    u3.proc = GetProcAddress(nvml_dll, "nvmlSetApplicationsClocks");
    nvmlSetApplicationsClocks_t nvmlSetApplicationsClocks = u3.fn;

    if (!nvmlSetApplicationsClocks) {
        LogInfo("GPUMemoryManager: nvmlSetApplicationsClocks not available");
        return false;
    }

    unsigned int result = nvmlSetApplicationsClocks(
        device_id,
        core_clock_mhz * 1000,
        mem_clock_mhz * 1000
    );

    if (result != 0) {
        LogInfo("GPUMemoryManager: Failed to set GPU %d frequency (error %u)", device_id, result);
        return false;
    }

    bool found_state = false;
    for (auto& state : gpu_control_states) {
        if (state.device_id == device_id) {
            state.is_controlled = true;
            found_state = true;
            break;
        }
    }

    if (!found_state) {
        GPUControlState new_state;
        new_state.device_id = device_id;
        new_state.is_controlled = true;
        gpu_control_states.push_back(new_state);
    }

    LogInfo("GPUMemoryManager: GPU %d frequency set - Core: %d MHz, Memory: %d MHz",
              device_id, core_clock_mhz, mem_clock_mhz);
    return true;
#else
    LogInfo("GPUMemoryManager: GPU frequency control not supported on this platform");
    return false;
#endif
}

bool GPUMemoryManager::SetGPUPowerLimit(int device_id, uint32_t power_watts) {
    std::lock_guard<std::mutex> lock(memory_mutex);

    if (!InitializeNVML()) {
        LogInfo("GPUMemoryManager: Cannot set GPU power limit - NVML not available");
        return false;
    }

#ifdef WIN32
    HMODULE nvml_dll = GetModuleHandleA("nvml.dll");
    if (!nvml_dll) return false;

    typedef unsigned int (*nvmlDeviceGetHandleByIndex_t)(unsigned int, void*);
    typedef unsigned int (*nvmlPowerManagementGetLimit_t)(void*, unsigned int*);
    typedef unsigned int (*nvmlPowerManagementSetLimit_t)(void*, unsigned int);

    union { FARPROC proc; nvmlDeviceGetHandleByIndex_t fn; } u4a, u4b;
    u4a.proc = GetProcAddress(nvml_dll, "nvmlDeviceGetHandleByIndex_v2");
    nvmlDeviceGetHandleByIndex_t nvmlDeviceGetHandleByIndex = u4a.fn;
    if (!nvmlDeviceGetHandleByIndex) {
        u4b.proc = GetProcAddress(nvml_dll, "nvmlDeviceGetHandleByIndex");
        nvmlDeviceGetHandleByIndex = u4b.fn;
    }

    union { FARPROC proc; nvmlPowerManagementGetLimit_t fn; } u5;
    u5.proc = GetProcAddress(nvml_dll, "nvmlPowerManagementGetLimit");
    nvmlPowerManagementGetLimit_t nvmlPowerManagementGetLimit = u5.fn;

    union { FARPROC proc; nvmlPowerManagementSetLimit_t fn; } u6;
    u6.proc = GetProcAddress(nvml_dll, "nvmlPowerManagementSetLimit");
    nvmlPowerManagementSetLimit_t nvmlPowerManagementSetLimit = u6.fn;

    if (!nvmlDeviceGetHandleByIndex || !nvmlPowerManagementGetLimit || !nvmlPowerManagementSetLimit) {
        LogInfo("GPUMemoryManager: Required NVML functions not available");
        return false;
    }

    void* device_handle = nullptr;
    unsigned int result = nvmlDeviceGetHandleByIndex(device_id, &device_handle);
    if (result != 0) {
        LogInfo("GPUMemoryManager: Failed to get handle for GPU %d (error %u)", device_id, result);
        return false;
    }

    uint32_t original_limit = 0;
    result = nvmlPowerManagementGetLimit(device_handle, &original_limit);
    if (result == 0) {
        bool found_state = false;
        for (auto& state : gpu_control_states) {
            if (state.device_id == device_id) {
                if (state.original_power_limit == 0) {
                    state.original_power_limit = original_limit;
                }
                found_state = true;
                break;
            }
        }

        if (!found_state) {
            GPUControlState new_state;
            new_state.device_id = device_id;
            new_state.original_power_limit = original_limit;
            gpu_control_states.push_back(new_state);
        }
    }

    result = nvmlPowerManagementSetLimit(device_handle, power_watts * 1000);
    if (result != 0) {
        LogInfo("GPUMemoryManager: Failed to set power limit for GPU %d (error %u)", device_id, result);
        return false;
    }

    LogInfo("GPUMemoryManager: GPU %d power limit set to %u W (was %u W)",
              device_id, power_watts, original_limit / 1000);
    return true;
#else
    LogInfo("GPUMemoryManager: Power limit control not supported on this platform");
    return false;
#endif
}

bool GPUMemoryManager::ResetGPUClocks(int device_id) {
    std::lock_guard<std::mutex> lock(memory_mutex);

    if (!nvml_initialized) {
        return true;
    }

#ifdef WIN32
    HMODULE nvml_dll = GetModuleHandleA("nvml.dll");
    if (!nvml_dll) return true;

    typedef unsigned int (*nvmlResetApplicationsClocks_t)(unsigned int);
    union { FARPROC proc; nvmlResetApplicationsClocks_t fn; } u7;
    u7.proc = GetProcAddress(nvml_dll, "nvmlResetApplicationsClocks");
    nvmlResetApplicationsClocks_t nvmlResetApplicationsClocks = u7.fn;

    if (nvmlResetApplicationsClocks) {
        unsigned int result = nvmlResetApplicationsClocks(device_id);
        if (result == 0) {
            LogInfo("GPUMemoryManager: GPU %d clocks reset to default", device_id);

            for (auto& state : gpu_control_states) {
                if (state.device_id == device_id) {
                    state.is_controlled = false;
                    break;
                }
            }

            return true;
        } else {
            LogInfo("GPUMemoryManager: Failed to reset GPU %d clocks (error %u)", device_id, result);
            return false;
        }
    }
#endif

    return true;
}
