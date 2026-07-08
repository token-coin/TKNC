#ifndef TKN_MINER_MODE_H
#define TKN_MINER_MODE_H

#include <atomic>
#include <cstdint>

enum class MiningMode {
    MODE_TASK = 0,
    MODE_POW = 1,
    MODE_LLM = 2
};

inline const char* GetModeName(MiningMode mode) {
    switch (mode) {
        case MiningMode::MODE_TASK: return "task";
        case MiningMode::MODE_POW: return "pow";
        case MiningMode::MODE_LLM: return "LLM";
        default: return "unknown";
    }
}

#endif // TKN_MINER_MODE_H
