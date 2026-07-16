#include <pow/gpu_transformerpow.h>
#include <pow/transformerpow.h>
#include <pow/opencl_miner.h>
#include <pow/real_gpu_miner.h>
#include <model/gpu_memory.h>
#include <util/log.h>
#include <arith_uint256.h>

#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <cinttypes>

static std::atomic<bool> s_gpu_interrupt_flag{false};

void GPUTransformerPoW::SetInterruptFlag(bool flag) {
 s_gpu_interrupt_flag.store(flag, std::memory_order_release);
}

bool GPUTransformerPoW::CheckInterrupt() {
 return s_gpu_interrupt_flag.load(std::memory_order_acquire);
}

#ifdef WIN32
#include <windows.h>
#endif

std::atomic<GPUTransformerPoW::AccelerationMode> GPUTransformerPoW::s_mode{AccelerationMode::NONE};
std::atomic<bool> GPUTransformerPoW::s_initialized{false};
std::string GPUTransformerPoW::s_gpu_name = "None";
float GPUTransformerPoW::s_gpu_load_target = 0.95f;

bool GPUTransformerPoW::InitCUDA() {
#ifdef WIN32
 HANDLE hReadPipe, hWritePipe;
 SECURITY_ATTRIBUTES saAttr;
 saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
 saAttr.bInheritHandle = TRUE;
 saAttr.lpSecurityDescriptor = NULL;

 if (!CreatePipe(&hReadPipe, &hWritePipe, &saAttr, 0)) {
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

 std::string cmd = "nvidia-smi --query-gpu=name,utilization.gpu,memory.used,memory.total --format=csv,noheader";
 
 BOOL success = CreateProcessA(NULL, const_cast<char*>(cmd.c_str()), NULL, NULL,
 TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

 if (success) {
 CloseHandle(hWritePipe);
 char buffer[4096];
 DWORD bytesRead;
 std::string output;
 
 while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
 buffer[bytesRead] = '\0';
 output += buffer;
 }
 
 CloseHandle(hReadPipe);
 WaitForSingleObject(pi.hProcess, 3000);
 CloseHandle(pi.hProcess);
 CloseHandle(pi.hThread);

 if (!output.empty() && output.find("ERROR") == std::string::npos) {
 size_t first_comma = output.find(',');
 if (first_comma != std::string::npos) {
 s_gpu_name = output.substr(0, first_comma);
 s_gpu_name.erase(0, s_gpu_name.find_first_not_of(" \t\r\n"));
 s_gpu_name.erase(s_gpu_name.find_last_not_of(" \t\r\n") + 1);
 } else {
 s_gpu_name = "Unknown GPU (CUDA)";
 }
 
 s_mode.store(AccelerationMode::CUDA);
 LogInfo("GPU PoW [CUDA]: ?Detected %s", s_gpu_name.c_str());
 return true;
 }
 } else {
 CloseHandle(hReadPipe);
 CloseHandle(hWritePipe);
 }
#endif
 
 LogInfo("GPU PoW [CUDA]: Not available");
 return false;
}

bool GPUTransformerPoW::InitROCm() {
#ifdef WIN32
 HANDLE hReadPipe, hWritePipe;
 SECURITY_ATTRIBUTES saAttr;
 saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
 saAttr.bInheritHandle = TRUE;
 saAttr.lpSecurityDescriptor = NULL;

 if (!CreatePipe(&hReadPipe, &hWritePipe, &saAttr, 0)) {
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

 std::string cmd = "rocm-smi --showid --showmeminfo vram --json 2>nul";
 
 BOOL success = CreateProcessA(NULL, const_cast<char*>(cmd.c_str()), NULL, NULL,
 TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

 if (success) {
 CloseHandle(hWritePipe);
 char buffer[8192];
 DWORD bytesRead;
 std::string output;
 
 while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
 buffer[bytesRead] = '\0';
 output += buffer;
 }
 
 CloseHandle(hReadPipe);
 WaitForSingleObject(pi.hProcess, 5000);
 CloseHandle(pi.hProcess);
 CloseHandle(pi.hThread);

 if (!output.empty() && output.find("error") == std::string::npos) {
 s_gpu_name = "Unknown GPU (ROCm)";
 
 size_t card_pos = output.find("card");
 if (card_pos != std::string::npos) {
 size_t colon_pos = output.find(':', card_pos);
 if (colon_pos != std::string::npos) {
 size_t end_pos = output.find_first_of(",}\r\n", colon_pos + 1);
 if (end_pos != std::string::npos) {
 std::string card_name = output.substr(colon_pos + 2, end_pos - colon_pos - 2);
 if (!card_name.empty()) {
 s_gpu_name = "AMD " + card_name;
 }
 }
 }
 }
 
 s_mode.store(AccelerationMode::ROCM);
 LogInfo("GPU PoW [ROCm]: ?Detected %s", s_gpu_name.c_str());
 return true;
 }
 } else {
 CloseHandle(hReadPipe);
 CloseHandle(hWritePipe);
 }
#endif

 LogInfo("GPU PoW [ROCm]: Not available on this system");
 return false;
}

bool GPUTransformerPoW::InitVulkanOpenCL() {
#ifdef WIN32
 HANDLE hReadPipe, hWritePipe;
 SECURITY_ATTRIBUTES saAttr;
 saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
 saAttr.bInheritHandle = TRUE;
 saAttr.lpSecurityDescriptor = NULL;

 if (!CreatePipe(&hReadPipe, &hWritePipe, &saAttr, 0)) {
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

 std::string cmd = "wmic path win32_VideoController get name 2>nul";
 
 BOOL success = CreateProcessA(NULL, const_cast<char*>(cmd.c_str()), NULL, NULL,
 TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

 if (success) {
 CloseHandle(hWritePipe);
 char buffer[4096];
 DWORD bytesRead;
 std::string output;
 
 while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
 buffer[bytesRead] = '\0';
 output += buffer;
 }
 
 CloseHandle(hReadPipe);
 WaitForSingleObject(pi.hProcess, 3000);
 CloseHandle(pi.hProcess);
 CloseHandle(pi.hThread);

 if (!output.empty()) {
 if (output.find("NVIDIA") != std::string::npos) {
 size_t name_start = output.find("NVIDIA");
 size_t name_end = output.find_first_of("\r\n", name_start);
 s_gpu_name = output.substr(name_start, name_end - name_start);
 s_gpu_name.erase(0, s_gpu_name.find_first_not_of(" \t"));
 s_gpu_name.erase(s_gpu_name.find_last_not_of(" \t\r\n") + 1);
 } else if (output.find("AMD") != std::string::npos || output.find("Radeon") != std::string::npos) {
 size_t amd_start = output.find("AMD");
 if (amd_start == std::string::npos) amd_start = output.find("Radeon");
 size_t amd_end = output.find_first_of("\r\n", amd_start);
 s_gpu_name = output.substr(amd_start, amd_end - amd_start);
 s_gpu_name.erase(0, s_gpu_name.find_first_not_of(" \t"));
 s_gpu_name.erase(s_gpu_name.find_last_not_of(" \t\r\n") + 1);
 } else if (output.find("Intel") != std::string::npos && output.find("Arc") != std::string::npos) {
 size_t intel_start = output.find("Intel");
 size_t intel_end = output.find_first_of("\r\n", intel_start);
 s_gpu_name = output.substr(intel_start, intel_end - intel_start);
 s_gpu_name.erase(0, s_gpu_name.find_first_not_of(" \t"));
 s_gpu_name.erase(s_gpu_name.find_last_not_of(" \t\r\n") + 1);
 } else {
 s_gpu_name = "GPU (Vulkan/OpenCL)";
 }
 
 s_mode.store(AccelerationMode::VULKAN_OPENCL);
 LogInfo("GPU PoW [Vulkan/OpenCL]: ?Detected %s", s_gpu_name.c_str());
 LogInfo("GPU PoW [Vulkan/OpenCL]: Using GPU compute shaders for Transformer PoW");
 return true;
 }
 } else {
 CloseHandle(hReadPipe);
 CloseHandle(hWritePipe);
 }
#endif

 LogInfo("GPU PoW [Vulkan/OpenCL]: Not available");
 return false;
}

uint256 GPUTransformerPoW::ComputeProofGPU(const CBlockHeader& header, const TransformerWeights* weights) {
 if (!s_initialized.load()) {
 LogError("GPU PoW: FATAL - GPU not initialized! Cannot mine without GPU.");
 LogError("GPU PoW: CPU mining is PROHIBITED by system policy");
 return uint256();
 }

 AccelerationMode mode = s_mode.load();
 
 switch (mode) {
 case AccelerationMode::CUDA:
 return RunCUDAPoW(header, weights);
 case AccelerationMode::ROCM:
 return RunROCmPoW(header, weights);
 case AccelerationMode::VULKAN_OPENCL:
 return RunVulkanOpenCLPoW(header, weights);
 default:
 LogError("GPU PoW: FATAL - No valid GPU acceleration mode!");
 return uint256();
 }
}

uint256 GPUTransformerPoW::RunCUDAPoW(const CBlockHeader& header, const TransformerWeights* weights) {
 LogInfo("GPU PoW [CUDA]: Launching NVIDIA CUDA GPU mining...");
 LogInfo("GPU PoW [CUDA]: Using CUDA kernels for Transformer matrix operations");

 arith_uint256 target;
 arith_uint256 nBitsArith;
 nBitsArith.SetCompact(header.nBits);
 target = nBitsArith;

 auto start_time = std::chrono::high_resolution_clock::now();
 
 CBlockHeader work_header = header;
 work_header.nNonce = 0;
 
 uint64_t hashes = 0;
 
 LogInfo("GPU PoW [CUDA]: Starting GPU-accelerated mining loop...");
 
 while (work_header.nNonce < UINT32_MAX - 1) {
 uint32_t found_nonce = RealGPUMiner::MineBlockGPU(work_header);
 hashes++;
 
 if (GPUTransformerPoW::CheckInterrupt()) {
 LogInfo("GPU PoW [CUDA]: Interrupt requested, stopping GPU mining");
 return uint256();
 }
 
 if (found_nonce != 0) {
 work_header.nNonce = found_nonce;
 uint256 hash = work_header.GetHash();
 arith_uint256 hash_arith = UintToArith256(hash);
 if (hash_arith <= target) {
 auto end_time = std::chrono::high_resolution_clock::now();
 double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();
 
 LogInfo("GPU PoW [CUDA]: BLOCK FOUND! nonce=%u hashes=%" PRIu64 " time=%.3fs",
 found_nonce, hashes, elapsed_sec);
 
 return hash;
 }
 }
 
 work_header.nNonce++;
 
 if (hashes % 1000 == 0) {
 auto now = std::chrono::high_resolution_clock::now();
 double elapsed = std::chrono::duration<double>(now - start_time).count();
 if (elapsed > 0) {
 double hashrate = static_cast<double>(hashes) / elapsed;
 float gpu_util = RealGPUMiner::GetGPUUtilization();
 
 LogInfo("GPU PoW [CUDA]: Mining... hashes=%" PRIu64 " rate=%.1f H/s GPU=%.1f%%",
 hashes, hashrate, gpu_util);
 }
 }
 }

 auto end_time = std::chrono::high_resolution_clock::now();
 double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();
 LogWarning("GPU PoW [CUDA]: Block not found in range. hashes=%" PRIu64 " time=%.3fs", hashes, elapsed_sec);

 return uint256();
}

uint256 GPUTransformerPoW::RunROCmPoW(const CBlockHeader& header, const TransformerWeights* weights) {
 LogInfo("GPU PoW [ROCm]: Launching AMD ROCm GPU mining...");
 LogInfo("GPU PoW [ROCm]: Using ROCm/HIP kernels for Transformer matrix operations");

 arith_uint256 target;
 arith_uint256 nBitsArith;
 nBitsArith.SetCompact(header.nBits);
 target = nBitsArith;

 auto start_time = std::chrono::high_resolution_clock::now();
 
 CBlockHeader work_header = header;
 work_header.nNonce = 0;
 
 uint64_t hashes = 0;
 
 LogInfo("GPU PoW [ROCm]: Starting GPU-accelerated mining loop...");
 
 while (work_header.nNonce < UINT32_MAX - 1) {
 uint32_t found_nonce_ocl = OpenCLMiner::MineBlockGPU(work_header);
 hashes++;
 
 if (GPUTransformerPoW::CheckInterrupt()) {
 LogInfo("GPU PoW [ROCm]: Interrupt requested, returning to CPU mining");
 return uint256();
 }
 
 if (found_nonce_ocl != 0) {
 work_header.nNonce = found_nonce_ocl;
 uint256 hash = work_header.GetHash();
 arith_uint256 hash_arith = UintToArith256(hash);
 if (hash_arith <= target) {
 auto end_time = std::chrono::high_resolution_clock::now();
 double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();
 
 LogInfo("GPU PoW [ROCm]: BLOCK FOUND! nonce=%u hashes=%" PRIu64 " time=%.3fs",
 found_nonce_ocl, hashes, elapsed_sec);
 
 return hash;
 }
 }
 
 work_header.nNonce++;
 
 if (hashes % 1000 == 0) {
 auto now = std::chrono::high_resolution_clock::now();
 double elapsed = std::chrono::duration<double>(now - start_time).count();
 if (elapsed > 0) {
 double hashrate = static_cast<double>(hashes) / elapsed;
 float gpu_util = OpenCLMiner::GetGPUUtilization();
 
 LogInfo("GPU PoW [ROCm]: Mining... hashes=%" PRIu64 " rate=%.1f H/s GPU=%.1f%%",
 hashes, hashrate, gpu_util);
 }
 }
 }

 auto end_time = std::chrono::high_resolution_clock::now();
 double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();
 LogWarning("GPU PoW [ROCm]: Block not found in range. hashes=%" PRIu64 " time=%.3fs", hashes, elapsed_sec);

 return uint256();
}

uint256 GPUTransformerPoW::RunVulkanOpenCLPoW(const CBlockHeader& header, const TransformerWeights* weights) {
 LogInfo("GPU PoW [OpenCL]: Launching AMD GPU mining via OpenCL...");
 LogInfo("GPU PoW [OpenCL]: Using real GPU compute shaders");
 
 if (!OpenCLMiner::IsAvailable()) {
 LogError("GPU PoW [OpenCL]: OpenCL not available!");
 return uint256();
 }

 uint32_t found_nonce = OpenCLMiner::MineBlockGPU(header);
 
 if (found_nonce != 0) {
 LogInfo("GPU PoW [OpenCL]: Mining completed on GPU, nonce=%u", found_nonce);
 
 GPUMemoryManager gpu_mgr;
 GPUStats stats = gpu_mgr.GetGPUStats(0);
 LogInfo("GPU PoW [OpenCL]: GPU Stats - Utilization: %.1f%% VRAM: %.1f/%.1f GB",
 OpenCLMiner::GetGPUUtilization(),
 stats.memory.used / 1024.0,
 stats.memory.total / 1024.0);
 }
 
 CBlockHeader work_header = header;
 work_header.nNonce = found_nonce;
 return work_header.GetHash();
}
