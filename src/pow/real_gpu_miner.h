#ifndef TKN_POW_REAL_GPU_MINER_H
#define TKN_POW_REAL_GPU_MINER_H

#include <pow/tknchash.h>
#include <model/gpu_memory.h>
#include <primitives/block.h>
#include <arith_uint256.h>
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <mutex>

#ifdef _WIN32
#include <d3d11.h>
#include <dxgi1_2.h>
#include <pdh.h>
#endif

class RealGPUMiner {
public:
    enum class GPUMode {
        IDLE = 0,
        POW_MINING,
        INFERENCE
    };

    struct GPUCapabilities {
        uint64_t total_vram_bytes;
        uint64_t available_vram_bytes;
        uint32_t feature_level;
        std::string gpu_name;
        bool is_dedicated_gpu;
        uint32_t adaptive_batch_size;
        uint32_t adaptive_workload_multiplier;
        float target_utilization;
    };

    static bool IsAvailable();

    static uint32_t MineBlockGPU(const CBlockHeader& header);

    static bool SwitchToMode(GPUMode mode, int64_t blockHeight = -1);
    static GPUMode GetCurrentMode();
    static bool IsMiningMode();

    static float GetGPUUtilization();
    static float GetRealGPULoad();

    static uint64_t GetTotalDispatchedHashes();

    static std::string FormatHashrate(double hashrate);
    static const uint32_t* GetLookupTable() { return s_lookup_table; }

private:
    static std::atomic<bool> s_initialized;
    static std::atomic<bool> s_available;
    static std::string s_device_name;
    static std::atomic<float> s_gpu_utilization;
    static std::atomic<uint64_t> s_total_hashes;

    static std::atomic<GPUMode> s_current_mode;
    static std::mutex s_mode_mutex;
    static std::atomic<int> s_d3d11_usage_count;

#ifdef _WIN32
    static ID3D11Device* s_d3d_device;
    static ID3D11DeviceContext* s_device_context;
    static ID3D11ComputeShader* s_compute_shader;
    static ID3D11Buffer* s_lookup_buffer;
    static ID3D11Buffer* s_output_buffer;
    static ID3D11Buffer* s_constant_buffer;
    static IDXGIAdapter* s_dxgi_adapter;
    static ID3D11ShaderResourceView* s_lookup_srv;
    static ID3D11UnorderedAccessView* s_output_uav;
    static PDH_HQUERY s_pdh_query;
    static PDH_HCOUNTER s_pdh_counter;
#endif

    static uint32_t s_lookup_table[TKNC_HASH_TABLE_SIZE];

    static std::atomic<float> s_real_gpu_load;
    static std::atomic<uint32_t> s_workload_ratio;
    static std::atomic<uint32_t> s_current_batch_size;
    static std::atomic<uint32_t> s_gpu_parallel_capacity;

    static GPUCapabilities s_gpu_caps;

    static std::chrono::steady_clock::time_point s_last_gpu_check;
    static std::chrono::steady_clock::time_point s_mining_start_time;
    static std::atomic<uint64_t> s_total_dispatches;
    static std::chrono::duration<double> s_total_gpu_time;

    static bool InitializeDirectX11();
    static void ShutdownDirectX11();
    static bool CreateComputeShader();
    static bool InitializeHashResources();

    static uint32_t RunMiningLoop(const CBlockHeader& header);
    static bool DispatchComputeBatch(const CBlockHeader& header, uint32_t nonce_start, uint32_t batch_size);
    static bool ReadGPUOutput(std::vector<uint32_t>& results);

    static void ProbeGPUCapabilities();
    static void CalculateAdaptiveParameters();
    static void DetectRealGPULoad();
    static void AdjustWorkloadBasedOnLoad();
    static void InitializeGPUWorkload();
    static uint32_t DetectGPUParallelCapacity();
    static uint32_t CalculateOptimalBatchSize();
    static void UpdateGPUUtilizationStats();

    static bool InitializePdhCounter();
    static void ShutdownPdhCounter();
    static float QueryRealGPU3DUsage();

};

#endif
