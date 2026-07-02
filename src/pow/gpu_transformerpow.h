#ifndef TKN_POW_GPU_TRANSFORMERPOW_H
#define TKN_POW_GPU_TRANSFORMERPOW_H

#include <pow/transformerpow.h>
#include <model/gpu_memory.h>
#include <cstdint>
#include <string>
#include <atomic>

class GPUTransformerPoW {
public:
    enum class AccelerationMode {
        NONE = 0,
        CUDA,
        VULKAN_OPENCL,
        ROCM
    };

    static uint256 ComputeProofGPU(const CBlockHeader& header, const TransformerWeights* weights);

    static void SetInterruptFlag(bool flag);
    static bool CheckInterrupt();

private:
    static std::atomic<AccelerationMode> s_mode;
    static std::atomic<bool> s_initialized;
    static std::string s_gpu_name;
    static float s_gpu_load_target;

    static bool InitCUDA();
    static bool InitROCm();
    static bool InitVulkanOpenCL();

    static uint256 RunCUDAPoW(const CBlockHeader& header, const TransformerWeights* weights);
    static uint256 RunROCmPoW(const CBlockHeader& header, const TransformerWeights* weights);
    static uint256 RunVulkanOpenCLPoW(const CBlockHeader& header, const TransformerWeights* weights);
};

#endif // TKN_POW_GPU_TRANSFORMERPOW_H
