#ifndef TKN_MODEL_GPU_MEMORY_H
#define TKN_MODEL_GPU_MEMORY_H

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>

struct GPUMemoryInfo {
    uint64_t total;
    uint64_t used;
    uint64_t free;
    float usage_percent;

    GPUMemoryInfo() : total(0), used(0), free(0), usage_percent(0.0f) {}
};

struct GPUStats {
    int device_id;
    std::string name;
    float usage_percent;
    GPUMemoryInfo memory;
    float temperature;
    int power_usage;

    GPUStats() : device_id(0), usage_percent(0.0f), temperature(0.0f), power_usage(0) {}
};

enum class GPUBackend {
    NONE = 0,
    CUDA,
    ROCm,
    VULKAN,
    METAL,
    OPENCL
};

struct GPUDeviceInfo {
    int device_id;
    std::string name;
    std::string uuid;
    GPUBackend backend_type;
    uint64_t total_memory_mb;
    uint64_t free_memory_mb;
    int compute_capability_major;
    int compute_capability_minor;
    int multiprocessor_count;
    int max_clock_rate_mhz;
    int memory_clock_rate_mhz;
    bool is_available;

    GPUDeviceInfo()
        : device_id(-1)
        , backend_type(GPUBackend::NONE)
        , total_memory_mb(0)
        , free_memory_mb(0)
        , compute_capability_major(0)
        , compute_capability_minor(0)
        , multiprocessor_count(0)
        , max_clock_rate_mhz(0)
        , memory_clock_rate_mhz(0)
        , is_available(false) {}
};

class GPUMemoryManager {
private:
    mutable std::mutex memory_mutex;
    uint64_t model_memory;
    uint64_t request_memory;
    uint64_t total_memory;
    int max_concurrent_requests;

    std::vector<GPUDeviceInfo> gpu_devices;
    GPUBackend active_backend;
    bool multi_gpu_initialized;

public:
    GPUMemoryManager();

    bool InitializeGPU(int device_id = 0);

    bool EnumerateAllGPUs();

    static bool DetectPrimaryGPU(GPUDeviceInfo& out_info);

    const std::vector<GPUDeviceInfo>& GetAllGPUs() const { return gpu_devices; }

    int GetGPUCount() const { return static_cast<int>(gpu_devices.size()); }

    const GPUDeviceInfo* GetGPUDeviceInfo(int device_id) const;

    GPUBackend GetActiveBackend() const { return active_backend; }

    bool IsMultiGPUAvailable() const { return gpu_devices.size() > 1; }

    int CalculateOptimalGPULayers(int device_id, uint64_t model_size_mb, int total_layers = 32);

    int CalculateOptimalGPULayersForAllGPUs(uint64_t model_size_mb, int total_layers = 32);

    GPUStats GetGPUStats(int device_id = 0) const;

    int CalculateMaxConcurrentRequests(uint64_t model_size, uint64_t per_request_size);

    bool AllocateRequestMemory(uint64_t size);

    void FreeRequestMemory(uint64_t size);

    uint64_t GetFreeMemory(int device_id = 0) const;

    bool HasEnoughMemory(uint64_t required_size, int device_id = 0) const;

    uint64_t GetTotalVRAMAcrossAllGPUs() const;

    void PrintGPUInfoSummary() const;

    bool SetGPUFrequency(int device_id, int core_clock_mhz, int mem_clock_mhz);

    bool SetGPUPowerLimit(int device_id, uint32_t power_watts);

    bool ResetGPUClocks(int device_id);

    struct GPUControlState {
        int device_id;
        uint32_t original_power_limit;
        int original_core_clock;
        int original_mem_clock;
        bool is_controlled;

        GPUControlState()
            : device_id(-1)
            , original_power_limit(0)
            , original_core_clock(0)
            , original_mem_clock(0)
            , is_controlled(false) {}
    };

    std::vector<GPUControlState> gpu_control_states;

    bool InitializeNVML();

    void ShutdownNVML();

    static bool nvml_initialized;
};

#endif // TKN_MODEL_GPU_MEMORY_H