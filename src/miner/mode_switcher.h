#ifndef TKN_MINER_MODE_SWITCHER_H
#define TKN_MINER_MODE_SWITCHER_H

#include <miner/mode.h>
#include <atomic>
#include <mutex>
#include <map>
#include <chrono>
#include <pow/real_gpu_miner.h>
#include <pow/gpu_transformerpow.h>

class ModeSwitcher {
private:
    std::atomic<MiningMode> current_mode{MiningMode::MODE_POW};
    std::atomic<uint64_t> active_requests{0};
    std::atomic<int64_t> current_block_height_{0};
    std::mutex request_mutex;
    std::map<uint64_t, std::chrono::steady_clock::time_point> request_timestamps;

    static constexpr int REQUEST_TIMEOUT_SECONDS = 30;

    void SyncGPUState(MiningMode mode) {
        int64_t height = current_block_height_.load(std::memory_order_relaxed);
        if (mode == MiningMode::MODE_LLM || mode == MiningMode::MODE_TASK) {
            RealGPUMiner::SwitchToMode(RealGPUMiner::GPUMode::INFERENCE, height);
        } else {
            RealGPUMiner::SwitchToMode(RealGPUMiner::GPUMode::POW_MINING, height);
        }
    }

public:
    void SetBlockHeight(int64_t height) {
        current_block_height_.store(height, std::memory_order_relaxed);
    }
    void OnRequestStart(uint64_t request_id) {
        active_requests.fetch_add(1, std::memory_order_acq_rel);
        current_mode.store(MiningMode::MODE_LLM, std::memory_order_release);
        GPUTransformerPoW::SetInterruptFlag(true);
        SyncGPUState(MiningMode::MODE_LLM);
        std::lock_guard<std::mutex> lock(request_mutex);
        request_timestamps[request_id] = std::chrono::steady_clock::now();
    }

    void OnRequestEnd(uint64_t request_id) {
        active_requests.fetch_sub(1, std::memory_order_acq_rel);
        std::lock_guard<std::mutex> lock(request_mutex);
        request_timestamps.erase(request_id);
        if (active_requests.load(std::memory_order_acquire) == 0) {
            current_mode.store(MiningMode::MODE_POW, std::memory_order_release);
            SyncGPUState(MiningMode::MODE_POW);
        }
    }

    void CleanupTimeoutRequests() {
        std::lock_guard<std::mutex> lock(request_mutex);
        auto now = std::chrono::steady_clock::now();
        auto it = request_timestamps.begin();
        while (it != request_timestamps.end()) {
            if (now - it->second > std::chrono::seconds(REQUEST_TIMEOUT_SECONDS)) {
                active_requests.fetch_sub(1, std::memory_order_acq_rel);
                it = request_timestamps.erase(it);
            } else {
                ++it;
            }
        }
        if (active_requests.load(std::memory_order_acquire) == 0) {
            current_mode.store(MiningMode::MODE_POW, std::memory_order_release);
        }
    }

    MiningMode GetCurrentMode() const {
        return current_mode.load(std::memory_order_acquire);
    }

    uint64_t GetActiveRequestCount() const {
        return active_requests.load(std::memory_order_acquire);
    }
};

class RequestGuard {
private:
    ModeSwitcher& switcher;
    uint64_t request_id;

public:
    RequestGuard(ModeSwitcher& s, uint64_t id)
        : switcher(s), request_id(id) {
        switcher.OnRequestStart(id);
    }

    ~RequestGuard() {
        switcher.OnRequestEnd(request_id);
    }

    RequestGuard(const RequestGuard&) = delete;
    RequestGuard& operator=(const RequestGuard&) = delete;
};

#endif // TKN_MINER_MODE_SWITCHER_H
