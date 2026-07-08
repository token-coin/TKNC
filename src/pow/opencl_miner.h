#ifndef TKN_POW_OPENCL_MINER_H
#define TKN_POW_OPENCL_MINER_H

#include <pow/transformerpow.h>
#include <model/gpu_memory.h>
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>

#ifdef USE_OPENCL
  #ifdef __APPLE__
    #include <OpenCL/opencl.h>
  #else
    #include <CL/cl.h>
  #endif
#else
  typedef void* cl_platform_id;
  typedef void* cl_device_id;
  typedef void* cl_context;
  typedef void* cl_command_queue;
  typedef void* cl_program;
  typedef void* cl_kernel;
  typedef void* cl_mem;
  typedef int cl_int;
  typedef size_t cl_uint;
  typedef size_t cl_mem_flags;
  typedef uint64_t cl_ulong;
  #define CL_SUCCESS 0
  #define CL_DEVICE_TYPE_ALL (0xFFFFFFFF)
  #define CL_DEVICE_TYPE_GPU (1 << 2)
  typedef cl_ulong cl_device_type;
  #define CL_DEVICE_GLOBAL_MEM_SIZE 0x101F
  #define CL_DEVICE_LOCAL_MEM_SIZE 0x101B
  #define CL_DEVICE_MAX_COMPUTE_UNITS 0x1002
  #define CL_DEVICE_MAX_WORK_GROUP_SIZE 0x1004
  #define CL_MEM_READ_ONLY (1 << 0)
  #define CL_MEM_READ_WRITE (1 << 1)
  #define CL_MEM_WRITE_ONLY (1 << 2)
  #define CL_MEM_COPY_HOST_PTR (1 << 3)
  #define CL_PROGRAM_BUILD_LOG 0x11B3
  #define CL_OUT_OF_RESOURCES -4
  #define CL_BUILD_ERROR -11
  typedef uintptr_t cl_context_properties;
  #define CL_CONTEXT_PLATFORM 0x1084
  
  typedef cl_int (*fn_clGetPlatformIDs)(cl_uint, cl_platform_id*, cl_uint*);
  typedef cl_int (*fn_clGetPlatformInfo)(cl_platform_id, cl_uint, size_t, void*, size_t*);
  typedef cl_int (*fn_clGetDeviceIDs)(cl_platform_id, cl_uint, cl_uint, cl_device_id*, cl_uint*);
  typedef cl_context (*fn_clCreateContext)(const void*, cl_uint, const cl_device_id*, void (*)(const char*, const void*, size_t, void*), void*, cl_int*);
  typedef cl_command_queue (*fn_clCreateCommandQueue)(cl_context, cl_device_id, cl_int, cl_int*);
  typedef cl_program (*fn_clCreateProgramWithSource)(cl_context, cl_uint, const char**, const size_t*, cl_int*);
  typedef cl_int (*fn_clBuildProgram)(cl_program, cl_uint, const cl_device_id*, const char*, void (*)(cl_program, void*), void*);
  typedef cl_kernel (*fn_clCreateKernel)(cl_program, const char*, cl_int*);
  typedef cl_mem (*fn_clCreateBuffer)(cl_context, cl_mem_flags, size_t, void*, cl_int*);
  typedef cl_int (*fn_clEnqueueWriteBuffer)(cl_command_queue, cl_mem, cl_int, size_t, size_t, const void*, cl_uint, const void*, void**);
  typedef cl_int (*fn_clSetKernelArg)(cl_kernel, cl_uint, size_t, const void*);
  typedef cl_int (*fn_clEnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const size_t*, const size_t*, const size_t*, cl_uint, const void*, void**);
  typedef cl_int (*fn_clEnqueueReadBuffer)(cl_command_queue, cl_mem, cl_int, size_t, size_t, void*, cl_uint, const void*, void**);
  typedef cl_int (*fn_clFinish)(cl_command_queue);
  typedef cl_int (*fn_clReleaseMemObject)(cl_mem);
  typedef cl_int (*fn_clReleaseKernel)(cl_kernel);
  typedef cl_int (*fn_clReleaseProgram)(cl_program);
  typedef cl_int (*fn_clReleaseCommandQueue)(cl_command_queue);
  typedef cl_int (*fn_clReleaseContext)(cl_context);
  typedef cl_int (*fn_clGetDeviceInfo)(cl_device_id, cl_uint, size_t, void*, size_t*);
  typedef cl_int (*fn_clGetProgramBuildInfo)(cl_program, cl_device_id, cl_uint, size_t, void*, size_t*);
  typedef void* cl_event;
  typedef uint32_t cl_profiling_info;
  typedef cl_int (*fn_clGetEventProfilingInfo)(cl_event, cl_profiling_info, size_t, void*, size_t*);
  typedef cl_int (*fn_clReleaseEvent)(cl_event);
  #define CL_QUEUE_PROFILING_ENABLE (1 << 1)
  #define CL_PROFILING_COMMAND_START 0x0800
  #define CL_PROFILING_COMMAND_END 0x0801
  #define CL_TRUE 1
  #define CL_FALSE 0
#endif

class OpenCLMiner {
public:
    static bool Initialize();
    static bool IsAvailable();
    static std::string GetDeviceInfo();
    
    static uint32_t MineBlockGPU(const CBlockHeader& header);
    static uint32_t MineBlockCPU(const CBlockHeader& header);
    
    static float GetGPUUtilization();
    static float GetGPULoad();
    static GPUMemoryInfo GetGPUMemoryInfo();
    
    static bool IsGPUMiningActive();
    static uint64_t GetTotalHashes();

private:
    static std::atomic<bool> s_initialized;
    static std::atomic<bool> s_available;
    static std::string s_device_name;
    static std::atomic<float> s_gpu_utilization;
    static std::atomic<uint64_t> s_total_hashes;
    
    static bool LoadOpenCLRuntime();
    static bool InitOpenCLDevice();
    static void UpdateRealUtilization(int64_t gpu_compute_time_us, int64_t profiled_busy_ns = 0);
};

#endif // TKN_POW_OPENCL_MINER_H
