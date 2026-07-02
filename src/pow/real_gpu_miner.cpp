#include <pow/real_gpu_miner.h>
#include <util/log.h>
#include <arith_uint256.h>
#include <uint256.h>
#include <crypto/sha256.h>
#include <util/time.h>

#include <iostream>
#include <sstream>
#include <iomanip>
#include <random>
#include <algorithm>
#include <cinttypes>
#include <thread>
#include <memory>
#include <map>
#include <string>

#ifdef _WIN32
#include <dxgi1_3.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <pdh.h>
#endif

std::atomic<bool> RealGPUMiner::s_initialized{false};
std::atomic<bool> RealGPUMiner::s_available{false};
std::string RealGPUMiner::s_device_name;
std::atomic<float> RealGPUMiner::s_gpu_utilization{0.0f};
std::atomic<uint64_t> RealGPUMiner::s_total_hashes{0};

std::atomic<RealGPUMiner::GPUMode> RealGPUMiner::s_current_mode{RealGPUMiner::GPUMode::IDLE};
std::mutex RealGPUMiner::s_mode_mutex;
std::atomic<int> RealGPUMiner::s_d3d11_usage_count{0};

#ifdef _WIN32
ID3D11Device* RealGPUMiner::s_d3d_device = nullptr;
ID3D11DeviceContext* RealGPUMiner::s_device_context = nullptr;
ID3D11ComputeShader* RealGPUMiner::s_compute_shader = nullptr;
ID3D11Buffer* RealGPUMiner::s_lookup_buffer = nullptr;
ID3D11Buffer* RealGPUMiner::s_output_buffer = nullptr;
ID3D11Buffer* RealGPUMiner::s_constant_buffer = nullptr;
IDXGIAdapter* RealGPUMiner::s_dxgi_adapter = nullptr;
ID3D11ShaderResourceView* RealGPUMiner::s_lookup_srv = nullptr;
ID3D11UnorderedAccessView* RealGPUMiner::s_output_uav = nullptr;
PDH_HQUERY RealGPUMiner::s_pdh_query = nullptr;
PDH_HCOUNTER RealGPUMiner::s_pdh_counter = nullptr;
#endif

uint32_t RealGPUMiner::s_lookup_table[TKNC_HASH_TABLE_SIZE] = {0};

std::atomic<float> RealGPUMiner::s_real_gpu_load{0.0f};
std::atomic<uint32_t> RealGPUMiner::s_workload_ratio{512};
std::atomic<uint32_t> RealGPUMiner::s_current_batch_size{0};
std::atomic<uint32_t> RealGPUMiner::s_gpu_parallel_capacity{0};

RealGPUMiner::GPUCapabilities RealGPUMiner::s_gpu_caps{};

std::chrono::steady_clock::time_point RealGPUMiner::s_last_gpu_check{};
std::chrono::steady_clock::time_point RealGPUMiner::s_mining_start_time{};
std::atomic<uint64_t> RealGPUMiner::s_total_dispatches{0};
std::chrono::duration<double> RealGPUMiner::s_total_gpu_time{0};

const char* TKNC_HASH_SHADER_HLSL = R"(
cbuffer PoWParams : register(b0) {
    uint4 block_header[20];
    uint start_nonce;
    uint num_nonces;
    uint target_hi;
    uint target_lo;
    uint padding[29];
};

StructuredBuffer<uint> lookup_table : register(t0);
RWStructuredBuffer<uint> output_hashes : register(u0);

#define TABLE_SIZE 4096
#define STATE_SIZE 8
#define HASH_ROUNDS 64

uint rotl(uint x, uint n) {
    return (x << (n & 31)) | (x >> ((32 - n) & 31));
}

[numthreads(256, 1, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID) {
    const uint global_id = DTid.x;
    if (global_id >= num_nonces) return;

    uint nonce = start_nonce + global_id;

    uint state[8];
    state[0] = block_header[0].x ^ nonce;
    state[1] = block_header[0].y;
    state[2] = block_header[0].z ^ rotl(nonce, 17);
    state[3] = block_header[0].w;
    state[4] = block_header[1].x + nonce;
    state[5] = block_header[1].y;
    state[6] = block_header[1].z + rotl(nonce, 7);
    state[7] = block_header[1].w;

    [unroll]
    for (uint round = 0; round < HASH_ROUNDS; round++) {
        [unroll]
        for (uint i = 0; i < STATE_SIZE; i++) {
            uint idx1 = (state[i] ^ (round * 0x9E3779B9u)) & 0xFFF;
            state[i] ^= lookup_table[idx1 & (TABLE_SIZE - 1)];

            state[i] += state[(i + 1) % STATE_SIZE];
            state[i] ^= state[(i + 3) % STATE_SIZE];
            state[i] = rotl(state[i], (round * 3 + i * 7) & 31);
            state[i] *= 0x9E3779B9u;

            uint idx2 = (state[i] >> 16) & 0xFFF;
            state[(i + 5) % STATE_SIZE] ^= lookup_table[idx2 & (TABLE_SIZE - 1)];
        }
    }

    [unroll]
    for (uint i = 0; i < STATE_SIZE; i++) {
        state[i] ^= state[(i + 2) % STATE_SIZE];
        state[i] = rotl(state[i], i * 4 + 7);
        state[i] *= 0x517CC1B7u;
    }

    uint is_valid = 0;
    if (state[7] < target_hi) {
        is_valid = 1;
    } else if (state[7] == target_hi && state[6] <= target_lo) {
        is_valid = 1;
    }

    output_hashes[global_id * 2] = nonce;
    output_hashes[global_id * 2 + 1] = is_valid;
}
)";

bool RealGPUMiner::InitializeDirectX11() {
#ifdef _WIN32
    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // First, find the best GPU (most VRAM) for PoW mining
    IDXGIAdapter* pBestAdapter = nullptr;
    {
        IDXGIFactory* pFactory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory), (void**)&pFactory))) {
            UINT adapterIndex = 0;
            IDXGIAdapter* pAdapter = nullptr;
            UINT64 best_vram = 0;
            while (SUCCEEDED(pFactory->EnumAdapters(adapterIndex, &pAdapter))) {
                DXGI_ADAPTER_DESC desc;
                if (SUCCEEDED(pAdapter->GetDesc(&desc))) {
                    // Software adapters have DedicatedVideoMemory=0, so > best_vram check suffices
                    if (desc.DedicatedVideoMemory > best_vram) {
                        best_vram = desc.DedicatedVideoMemory;
                        if (pBestAdapter) pBestAdapter->Release();
                        pBestAdapter = pAdapter;
                        pAdapter->AddRef();  // Keep reference
                    }
                }
                pAdapter->Release();
                adapterIndex++;
            }
            pFactory->Release();
        }
        if (!pBestAdapter) {
            std::cerr << "[RealGPU] ERROR: No suitable GPU found for mining!" << std::endl;
            return false;
        }
    }

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDevice(
        pBestAdapter,   // Explicitly use the best GPU (most VRAM), not nullptr (default=iGPU)
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        createDeviceFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &device,
        &featureLevel,
        &context
    );

    if (FAILED(hr)) {
        std::cerr << "[RealGPU] FAILED at Step 1 - D3D11CreateDevice error: 0x"
                  << std::hex << hr << std::dec << std::endl;

        if (hr == DXGI_ERROR_UNSUPPORTED) {
            std::cerr << "Error: GPU does not support DirectX 11\n";
        } else if (hr == E_INVALIDARG) {
            std::cerr << "Error: Invalid arguments\n";
        }
        if (pBestAdapter) pBestAdapter->Release();
        return false;
    }

    s_d3d_device = device;
    s_device_context = context;
    LogInfo("RealGPU: DX11 OK, FL=0x%x", featureLevel);

    // Use our pre-selected best adapter directly (already holds reference from enum loop)
    s_dxgi_adapter = pBestAdapter;
    pBestAdapter = nullptr;  // Transferred ownership to s_dxgi_adapter

    DXGI_ADAPTER_DESC desc;
    s_dxgi_adapter->GetDesc(&desc);
    char gpuName[256];
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpuName, 256, nullptr, nullptr);
    s_device_name = gpuName;
    LogInfo("RealGPU: %s (%" PRIu64 " MB VRAM)", s_device_name.c_str(), desc.DedicatedVideoMemory / (1024*1024));

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = 128 * sizeof(uint32_t);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    hr = s_d3d_device->CreateBuffer(&cbDesc, nullptr, &s_constant_buffer);
    if (FAILED(hr)) {
        std::cerr << "Error: Create constant buffer: 0x" << std::hex << hr << std::dec << "\n";
        return false;
    }

    InitializePdhCounter();
    
    return true;
#else
    LogError("RealGPU [DX11]: DirectX 11 not supported on this platform");
    return false;
#endif
}

void RealGPUMiner::ShutdownDirectX11() {
#ifdef _WIN32
    ShutdownPdhCounter();
    if (s_output_uav) { s_output_uav->Release(); s_output_uav = nullptr; }
    if (s_lookup_srv) { s_lookup_srv->Release(); s_lookup_srv = nullptr; }
    if (s_output_buffer) { s_output_buffer->Release(); s_output_buffer = nullptr; }
    if (s_lookup_buffer) { s_lookup_buffer->Release(); s_lookup_buffer = nullptr; }
    if (s_constant_buffer) { s_constant_buffer->Release(); s_constant_buffer = nullptr; }
    if (s_compute_shader) { s_compute_shader->Release(); s_compute_shader = nullptr; }
    if (s_device_context) { s_device_context->Release(); s_device_context = nullptr; }
    if (s_d3d_device) { s_d3d_device->Release(); s_d3d_device = nullptr; }
    if (s_dxgi_adapter) { s_dxgi_adapter->Release(); s_dxgi_adapter = nullptr; }
#endif
}

bool RealGPUMiner::CreateComputeShader() {
#ifdef _WIN32
    ID3DBlob* shaderBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    HRESULT hr = D3DCompile(
        TKNC_HASH_SHADER_HLSL,
        strlen(TKNC_HASH_SHADER_HLSL),
        "TKNCHash",
        nullptr,
        nullptr,
        "CSMain",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &shaderBlob,
        &errorBlob
    );

    if (FAILED(hr)) {
        std::cerr << "Error: Shader compile: 0x" << std::hex << hr << std::dec;
        if (errorBlob) {
            std::cerr << " - " << (const char*)errorBlob->GetBufferPointer();
            errorBlob->Release();
        }
        std::cerr << "\n";
        return false;
    }

    hr = s_d3d_device->CreateComputeShader(
        shaderBlob->GetBufferPointer(),
        shaderBlob->GetBufferSize(),
        nullptr,
        &s_compute_shader
    );

    shaderBlob->Release();

    if (FAILED(hr)) {
        std::cerr << "Error: CreateComputeShader: 0x" << std::hex << hr << std::dec << "\n";
        return false;
    }

    return true;
#else
    return false;
#endif
}

bool RealGPUMiner::InitializeHashResources() {
#ifdef _WIN32
    const size_t MAX_OUTPUT = s_gpu_caps.adaptive_batch_size;

    D3D11_BUFFER_DESC lookupDesc = {};
    lookupDesc.ByteWidth = TKNC_HASH_TABLE_SIZE * sizeof(uint32_t);
    lookupDesc.Usage = D3D11_USAGE_DEFAULT;
    lookupDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA lookupData = {};
    lookupData.pSysMem = s_lookup_table;

    HRESULT hr = s_d3d_device->CreateBuffer(&lookupDesc, &lookupData, &s_lookup_buffer);
    if (FAILED(hr)) {
        std::cerr << "Error: Create lookup buffer: 0x" << std::hex << hr << std::dec << "\n";
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC lookupSrvDesc = {};
    lookupSrvDesc.Format = DXGI_FORMAT_R32_UINT;
    lookupSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    lookupSrvDesc.Buffer.FirstElement = 0;
    lookupSrvDesc.Buffer.NumElements = TKNC_HASH_TABLE_SIZE;

    hr = s_d3d_device->CreateShaderResourceView(s_lookup_buffer, &lookupSrvDesc, &s_lookup_srv);
    if (FAILED(hr)) {
        std::cerr << "Error: Create lookup SRV: 0x" << std::hex << hr << std::dec << "\n";
        return false;
    }

    D3D11_BUFFER_DESC outputDesc = {};
    outputDesc.ByteWidth = MAX_OUTPUT * sizeof(uint32_t) * 2;
    outputDesc.Usage = D3D11_USAGE_DEFAULT;
    outputDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    outputDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    outputDesc.StructureByteStride = sizeof(uint32_t);

    hr = s_d3d_device->CreateBuffer(&outputDesc, nullptr, &s_output_buffer);
    if (FAILED(hr)) {
        std::cerr << "Error: Create output buffer: 0x" << std::hex << hr << std::dec << "\n";
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = MAX_OUTPUT * 2;

    hr = s_d3d_device->CreateUnorderedAccessView(s_output_buffer, &uavDesc, &s_output_uav);
    if (FAILED(hr)) {
        std::cerr << "Error: Create output UAV: 0x" << std::hex << hr << std::dec << "\n";
        return false;
    }
#endif

    return true;
}

bool RealGPUMiner::IsAvailable() {
    return s_available.load();
}

bool RealGPUMiner::SwitchToMode(GPUMode mode, int64_t blockHeight) {
    std::lock_guard<std::mutex> lock(s_mode_mutex);

    GPUMode current = s_current_mode.load();
    if (current == mode) return true;

    LogInfo("RealGPU [ModeSwitch]: %s → %s, Height=%d, s_initialized=%d",
            current == GPUMode::POW_MINING ? "PoW_Mining" :
            current == GPUMode::INFERENCE ? "Inference" : "Idle",
            mode == GPUMode::POW_MINING ? "PoW_Mining" :
            mode == GPUMode::INFERENCE ? "Inference" : "Idle",
            (int)blockHeight,
            s_initialized.load());

    switch (mode) {
        case GPUMode::POW_MINING:
            if (current == GPUMode::INFERENCE && s_initialized.load()) {
                InitializeDirectX11();
                CreateComputeShader();
                InitializeHashResources();
            }
            break;
            
        case GPUMode::INFERENCE:
            if (current == GPUMode::POW_MINING) {
                s_current_mode.store(mode);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                int wait_ms = 0;
                const int max_wait_ms = 5000;
                while (s_d3d11_usage_count.load() > 0 && wait_ms < max_wait_ms) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    wait_ms += 10;
                }
                if (s_d3d11_usage_count.load() > 0) {
                    LogWarning("RealGPU [ModeSwitch]: Timeout waiting for D3D11 usage count=%d after %dms",
                               s_d3d11_usage_count.load(), wait_ms);
                }
                if (s_initialized.load()) {
                    ShutdownDirectX11();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                } else {
                }
                return true;
            }
            break;
            
        case GPUMode::IDLE:
            if (s_initialized.load()) {
                ShutdownDirectX11();
            }
            break;
    }

    s_current_mode.store(mode);
    return true;
}

RealGPUMiner::GPUMode RealGPUMiner::GetCurrentMode() {
    return s_current_mode.load();
}

bool RealGPUMiner::IsMiningMode() {
    return s_current_mode.load() == GPUMode::POW_MINING;
}

uint32_t RealGPUMiner::MineBlockGPU(const CBlockHeader& header) {
    if (!s_initialized.load()) {
        LogError("RealGPU Miner: Not initialized!");
        return 0;
    }

    if (!IsMiningMode()) {
        LogWarning("RealGPU Miner: Not in mining mode (current mode: %d)", 
                   static_cast<int>(s_current_mode.load()));
        return 0;
    }

    auto mining_start = std::chrono::high_resolution_clock::now();
    
    LogInfo("RealGPU: Starting continuous GPU mining loop");

    uint32_t winning_nonce = RunMiningLoop(header);

    auto mining_end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(mining_end - mining_start).count();

    UpdateGPUUtilizationStats();

    if (winning_nonce == 0) {
        LogInfo("RealGPU: Round complete (%.2fms) - no valid proof yet", total_ms);
    }

    return winning_nonce;
}

uint32_t RealGPUMiner::RunMiningLoop(const CBlockHeader& header) {
    CBlockHeader work_header = header;
    work_header.nNonce = 0;

    CBlockHeader seed_header = header;
    seed_header.nNonce = 0;
    uint256 seed = seed_header.GetHash();
    TKNCGenerateTable(seed, s_lookup_table);

#ifdef _WIN32
    if (s_lookup_buffer && s_d3d_device && s_device_context) {
        s_d3d11_usage_count.fetch_add(1);
        s_device_context->UpdateSubresource(s_lookup_buffer, 0, nullptr, s_lookup_table, 0, 0);
        s_d3d11_usage_count.fetch_sub(1);
    }
#endif

    uint64_t total_hashes = 0;
    const uint64_t MAX_HASHES_PER_ROUND = UINT64_MAX;
    
    bool found_block = false;
    uint32_t winning_nonce = 0;
    
    while (total_hashes < MAX_HASHES_PER_ROUND && !found_block && IsMiningMode()) {
            AdjustWorkloadBasedOnLoad();
            
            if (!IsMiningMode()) {
                LogInfo("RealGPU [Mining]: Immediate mode check - GPU released for inference (PRE-DISPATCH)");
                break;
            }
            
            uint32_t batch_size = s_current_batch_size.load();
            uint32_t nonce_start = work_header.nNonce;

            auto dispatch_start = std::chrono::high_resolution_clock::now();

            if (!DispatchComputeBatch(header, nonce_start, batch_size)) {
                LogError("RealGPU [Mining]: Dispatch failed at nonce %u", nonce_start);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (!IsMiningMode()) {
                LogInfo("RealGPU [Mining]: Post-compute mode check - GPU released (POST-COMPUTE)");
                break;
            }

            std::vector<uint32_t> results(batch_size * 2);
            if (!ReadGPUOutput(results)) {
                LogError("RealGPU [Mining]: Readback failed at nonce %u", nonce_start);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            auto dispatch_end = std::chrono::high_resolution_clock::now();
            s_total_gpu_time += std::chrono::duration<double>(dispatch_end - dispatch_start);
            s_total_dispatches.fetch_add(1);

            arith_uint256 target;
            target.SetCompact(header.nBits);

            static int debug_counter = 0;
            uint32_t max_valid_seen = 0;

            (void)debug_counter;

            for (uint64_t i = 0; i < batch_size; i++) {
                uint32_t candidate_nonce = results[i * 2];
                uint32_t is_valid_flag = results[i * 2 + 1];

                if (is_valid_flag > max_valid_seen) {
                    max_valid_seen = is_valid_flag;
                }

                if (is_valid_flag == 0) {
                    continue;
                }

                total_hashes++;

                CBlockHeader verify_header = header;
                verify_header.nNonce = candidate_nonce;
                uint256 computed_hash = TKNCComputeHash(verify_header);

                if (UintToArith256(computed_hash) <= target) {
                    found_block = true;
                    winning_nonce = candidate_nonce;
                    work_header.nNonce = candidate_nonce;
                    LogInfo("RealGPU [Mining]: BLOCK FOUND! Nonce=%u (GPU+CPU verified)", candidate_nonce);
                    break;
                } else {
                    LogWarning("RealGPU [Mining]: GPU false positive at nonce=%u, rechecking...", candidate_nonce);
                }
            }

            (void)max_valid_seen;

            if (!found_block) {
                work_header.nNonce += batch_size;
            }

            s_total_hashes.fetch_add(batch_size);

            if (!IsMiningMode()) {
                LogInfo("RealGPU [Mining]: GPU released for inference (mode switched)");
                break;
            }
    }

    return found_block ? winning_nonce : 0;
}

bool RealGPUMiner::DispatchComputeBatch(const CBlockHeader& header, uint32_t nonce_start, uint32_t batch_size) {
#ifdef _WIN32
    if (!s_d3d_device || !s_device_context || !s_compute_shader || !s_constant_buffer) {
        return false;
    }

    s_d3d11_usage_count.fetch_add(1);
    struct UsageGuard { ~UsageGuard() { s_d3d11_usage_count.fetch_sub(1); } } guard;
    (void)guard;

    struct PoWConstants {
        uint32_t header[80];
        uint32_t start_nonce;
        uint32_t num_nonces;
        uint32_t target_hi;
        uint32_t target_lo;
        uint32_t padding[29];
    };

    PoWConstants constants = {};
    memcpy(constants.header, &header, sizeof(CBlockHeader));
    constants.start_nonce = nonce_start;
    constants.num_nonces = batch_size;

    arith_uint256 hashTarget;
    hashTarget.SetCompact(header.nBits);
    uint256 target256 = ArithToUint256(hashTarget);
    const unsigned char* targetBytes = target256.begin();
    constants.target_hi = (static_cast<uint32_t>(targetBytes[31]) << 24) |
                          (static_cast<uint32_t>(targetBytes[30]) << 16) |
                          (static_cast<uint32_t>(targetBytes[29]) << 8) |
                          static_cast<uint32_t>(targetBytes[28]);
    constants.target_lo = (static_cast<uint32_t>(targetBytes[27]) << 24) |
                          (static_cast<uint32_t>(targetBytes[26]) << 16) |
                          (static_cast<uint32_t>(targetBytes[25]) << 8) |
                          static_cast<uint32_t>(targetBytes[24]);

    s_device_context->UpdateSubresource(s_constant_buffer, 0, nullptr, &constants, 0, 0);

    s_device_context->CSSetConstantBuffers(0, 1, &s_constant_buffer);
    s_device_context->CSSetShader(s_compute_shader, nullptr, 0);
    ID3D11ShaderResourceView* srvs[1] = { s_lookup_srv };
    s_device_context->CSSetShaderResources(0, 1, srvs);
    s_device_context->CSSetUnorderedAccessViews(0, 1, &s_output_uav, nullptr);

    uint32_t threadGroups = (batch_size + 255) / 256;
    s_device_context->Dispatch(threadGroups, 1, 1);

    ID3D11UnorderedAccessView* nullUAV = nullptr;
    s_device_context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    s_device_context->CSSetShaderResources(0, 1, nullSRVs);
    ID3D11Buffer* nullBuffer = nullptr;
    s_device_context->CSSetConstantBuffers(0, 1, &nullBuffer);
    s_device_context->CSSetShader(nullptr, nullptr, 0);

    return true;
#else
    return false;
#endif
}

bool RealGPUMiner::ReadGPUOutput(std::vector<uint32_t>& results) {
#ifdef _WIN32
    if (!s_d3d_device || !s_device_context || !s_output_buffer) {
        return false;
    }

    s_d3d11_usage_count.fetch_add(1);
    struct UsageGuard { ~UsageGuard() { s_d3d11_usage_count.fetch_sub(1); } } guard;
    (void)guard;

    size_t requested_size = results.size();
    size_t max_buffer_size = s_gpu_caps.adaptive_batch_size;

    size_t output_elements = max_buffer_size * 2;
    if (requested_size < output_elements) {
        results.resize(output_elements);
    }

    D3D11_BUFFER_DESC stagingDesc = {};
    stagingDesc.ByteWidth = output_elements * sizeof(uint32_t);
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;

    ID3D11Buffer* stagingBuffer = nullptr;
    HRESULT hr = s_d3d_device->CreateBuffer(&stagingDesc, nullptr, &stagingBuffer);
    if (FAILED(hr)) {
        return false;
    }

    s_device_context->CopyResource(stagingBuffer, s_output_buffer);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = s_device_context->Map(stagingBuffer, 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(hr)) {
        const uint32_t* gpu_data = reinterpret_cast<const uint32_t*>(mapped.pData);
        for (size_t i = 0; i < results.size() && i < max_buffer_size; i++) {
            results[i * 2] = gpu_data[i * 2];
            results[i * 2 + 1] = gpu_data[i * 2 + 1];
        }
        s_device_context->Unmap(stagingBuffer, 0);
        stagingBuffer->Release();
        return true;
    }

    stagingBuffer->Release();
    return false;
#else
    return false;
#endif
}

void RealGPUMiner::DetectRealGPULoad() {
#ifdef _WIN32
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_last_gpu_check).count();
    
    if (elapsed < 500) return;

    float realUtilization = QueryRealGPU3DUsage();
    static float last_known_pdh = 0.0f;

    if (realUtilization < 0.0f) {
        realUtilization = last_known_pdh;
    } else {
        last_known_pdh = realUtilization;
    }

    s_real_gpu_load.store(realUtilization);
    s_last_gpu_check = now;
#endif
}

void RealGPUMiner::AdjustWorkloadBasedOnLoad() {
    DetectRealGPULoad();
}

void RealGPUMiner::InitializeGPUWorkload() {
    static bool initialized = false;
    if (initialized) return;

    uint32_t gpu_capacity = DetectGPUParallelCapacity();
    s_gpu_parallel_capacity.store(gpu_capacity);

    uint32_t ratio = s_workload_ratio.load();
    uint32_t batch_size = gpu_capacity * ratio;

    if (batch_size < 1024) batch_size = 1024;

    s_current_batch_size.store(batch_size);

    initialized = true;

    LogInfo("RealGPU: GPU Capacity: %u, Workload Ratio: %u, Batch Size: %u",
            gpu_capacity, ratio, batch_size);
}

uint32_t RealGPUMiner::DetectGPUParallelCapacity() {
#ifdef _WIN32
    if (s_d3d_device && s_device_context) {
        uint64_t vram_mb = s_gpu_caps.total_vram_bytes / (1024 * 1024);

        uint32_t base_capacity = 0;

        if (vram_mb >= 16000) {
            base_capacity = 262144;
        } else if (vram_mb >= 8000) {
            base_capacity = 131072;
        } else if (vram_mb >= 4000) {
            base_capacity = 65536;
        } else {
            base_capacity = 32768;
        }

        LogInfo("RealGPU [GPU-Capacity]: VRAM: %lluMB, Base capacity: %u threads",
                static_cast<unsigned long long>(vram_mb), base_capacity);

        return base_capacity;
    }
#endif

    uint64_t vram_mb = s_gpu_caps.total_vram_bytes / (1024 * 1024);

    if (vram_mb >= 16000) {
        return 262144;
    } else if (vram_mb >= 8000) {
        return 131072;
    } else if (vram_mb >= 4000) {
        return 65536;
    } else {
        return 32768;
    }
}

uint32_t RealGPUMiner::CalculateOptimalBatchSize() {
    uint32_t batch = s_current_batch_size.load();
    if (batch == 0) {
        InitializeGPUWorkload();
        batch = s_current_batch_size.load();
    }
    return batch;
}

void RealGPUMiner::UpdateGPUUtilizationStats() {
    float load = s_real_gpu_load.load();
    s_gpu_utilization.store(load);
}

bool RealGPUMiner::InitializePdhCounter() {
#ifdef _WIN32
    if (s_pdh_query) return true;
    
    PDH_STATUS status = PdhOpenQuery(nullptr, 0, &s_pdh_query);
    if (status != ERROR_SUCCESS) {
        s_pdh_query = nullptr;
        return false;
    }
    
    status = PdhAddEnglishCounterA(s_pdh_query,
        "\\GPU Engine(*)\\Utilization Percentage",
        0, &s_pdh_counter);
    
    if (status != ERROR_SUCCESS) {
        PdhCloseQuery(s_pdh_query);
        s_pdh_query = nullptr;
        s_pdh_counter = nullptr;
        return false;
    }
    
    PdhCollectQueryData(s_pdh_query);
    return true;
#else
    return false;
#endif
}

void RealGPUMiner::ShutdownPdhCounter() {
#ifdef _WIN32
    if (s_pdh_query) {
        PdhCloseQuery(s_pdh_query);
        s_pdh_query = nullptr;
        s_pdh_counter = nullptr;
    }
#endif
}

float RealGPUMiner::QueryRealGPU3DUsage() {
#ifdef _WIN32
    if (!s_pdh_query || !s_pdh_counter) {
        if (!InitializePdhCounter()) {
            return -1.0f;
        }
    }

    PDH_STATUS status = PdhCollectQueryData(s_pdh_query);
    if (status != ERROR_SUCCESS) {
        return -1.0f;
    }

    DWORD bufSize = 0;
    DWORD itemCount = 0;
    status = PdhGetFormattedCounterArrayW(s_pdh_counter, PDH_FMT_DOUBLE, &bufSize, &itemCount, nullptr);
    if ((DWORD)status != 0x800007D2) {
        PDH_FMT_COUNTERVALUE value;
        status = PdhGetFormattedCounterValue(s_pdh_counter, PDH_FMT_DOUBLE, nullptr, &value);
        if (status != ERROR_SUCCESS) return -1.0f;
        float v = static_cast<float>(value.doubleValue);
        return std::max(0.0f, std::min(100.0f, v));
    }

    auto buffer = std::make_unique<BYTE[]>(bufSize);
    status = PdhGetFormattedCounterArrayW(s_pdh_counter, PDH_FMT_DOUBLE, &bufSize, &itemCount,
                                          reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.get()));
    if (status != ERROR_SUCCESS) return -1.0f;

    std::map<std::wstring, double> engtype_totals;
    auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.get());
    for (DWORD i = 0; i < itemCount; i++) {
        std::wstring name(items[i].szName);
        size_t pos = name.find(L"engtype_");
        if (pos != std::wstring::npos) {
            size_t end = name.find(L'_', pos + 8);
            std::wstring etype = (end != std::wstring::npos) ? name.substr(pos, end - pos) : name.substr(pos);
            engtype_totals[etype] += items[i].FmtValue.doubleValue;
        }
    }

    double max_eng = 0.0;
    for (auto& [et, val] : engtype_totals) {
        if (val > max_eng) max_eng = val;
    }

    float usage = static_cast<float>(max_eng);
    usage = std::max(0.0f, std::min(100.0f, usage));

    return usage;
#else
    return 0.0f;
#endif
}

float RealGPUMiner::GetGPUUtilization() {
    return s_gpu_utilization.load();
}

float RealGPUMiner::GetRealGPULoad() {
    DetectRealGPULoad();
    float val = s_real_gpu_load.load();
    static float last_good = 0.0f;
    if (val >= 1.0f) { last_good = val; return val; }
    return last_good > 0.0f ? last_good : 0.0f;
}

uint64_t RealGPUMiner::GetTotalDispatchedHashes() {
    return s_total_hashes.load();
}

void RealGPUMiner::ProbeGPUCapabilities() {
#ifdef _WIN32
    s_gpu_caps = {};
    
    s_gpu_caps.gpu_name = s_device_name;
    s_gpu_caps.feature_level = 0;

    if (s_dxgi_adapter) {
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(s_dxgi_adapter->GetDesc(&desc))) {
            s_gpu_caps.total_vram_bytes = desc.DedicatedVideoMemory;
            s_gpu_caps.is_dedicated_gpu = (desc.DedicatedVideoMemory > 0);
            
            float usage_estimate = 0.15f;
            s_gpu_caps.available_vram_bytes = static_cast<uint64_t>(
                (s_gpu_caps.total_vram_bytes * (1.0f - usage_estimate)));
        }
        
        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };
        
        for (size_t i = 0; i < ARRAYSIZE(featureLevels); i++) {
            HRESULT hr = s_dxgi_adapter->CheckInterfaceSupport(
                __uuidof(ID3D11Device), nullptr);
            (void)hr;
            
            if (s_d3d_device) {
                D3D_FEATURE_LEVEL fl = s_d3d_device->GetFeatureLevel();
                s_gpu_caps.feature_level = static_cast<uint32_t>(fl);
                break;
            }
        }
    }

#else
    s_gpu_caps.total_vram_bytes = 1024 * 1024 * 1024ULL;
    s_gpu_caps.available_vram_bytes = 800 * 1024 * 1024ULL;
    s_gpu_caps.feature_level = 0xB000;
    s_gpu_caps.gpu_name = "Unknown GPU";
    s_gpu_caps.is_dedicated_gpu = true;
#endif
}

void RealGPUMiner::CalculateAdaptiveParameters() {
    uint64_t vram_bytes = s_gpu_caps.available_vram_bytes;
    if (vram_bytes == 0) vram_bytes = s_gpu_caps.total_vram_bytes * 80 / 100;

    float vram_gb = static_cast<float>(vram_bytes) / (1024.0f * 1024.0f * 1024.0f);

    if (vram_gb <= 2.0f) {
        s_gpu_caps.adaptive_batch_size = 16384;
        s_gpu_caps.adaptive_workload_multiplier = 32;
        s_gpu_caps.target_utilization = 0.75f;
    } else if (vram_gb <= 4.0f) {
        s_gpu_caps.adaptive_batch_size = 65536;
        s_gpu_caps.adaptive_workload_multiplier = 64;
        s_gpu_caps.target_utilization = 0.80f;
    } else if (vram_gb <= 8.0f) {
        s_gpu_caps.adaptive_batch_size = 262144;
        s_gpu_caps.adaptive_workload_multiplier = 128;
        s_gpu_caps.target_utilization = 0.85f;
    } else if (vram_gb <= 12.0f) {
        s_gpu_caps.adaptive_batch_size = 524288;
        s_gpu_caps.adaptive_workload_multiplier = 192;
        s_gpu_caps.target_utilization = 0.88f;
    } else if (vram_gb <= 16.0f) {
        s_gpu_caps.adaptive_batch_size = 786432;
        s_gpu_caps.adaptive_workload_multiplier = 224;
        s_gpu_caps.target_utilization = 0.90f;
    } else {
        s_gpu_caps.adaptive_batch_size = 1048576;
        s_gpu_caps.adaptive_workload_multiplier = 256;
        s_gpu_caps.target_utilization = 0.92f;
    }

    InitializeGPUWorkload();
}

std::string RealGPUMiner::FormatHashrate(double hashrate) {
    const char* units[] = {"H/s", "KH/s", "MH/s", "GH/s", "TH/s", "PH/s"};
    int unitIndex = 0;
    
    while (hashrate >= 1000.0 && unitIndex < 5) {
        hashrate /= 1000.0;
        unitIndex++;
    }
    
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << hashrate << " " << units[unitIndex];
    return ss.str();
}
