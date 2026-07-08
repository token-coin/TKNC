#ifndef TKN_POW_TRANSFORMERPOW_H
#define TKN_POW_TRANSFORMERPOW_H

#include <uint256.h>
#include <arith_uint256.h>
#include <primitives/block.h>
#include <vector>
#include <cstdint>
#include <memory>
#include <mutex>

// Transformer configuration parameters - Match GPU RealGPUMiner (12-layer, 32-dim)
inline constexpr int TRANSFORMER_NUM_LAYERS = 12;
inline constexpr int TRANSFORMER_NUM_HEADS = 8;
inline constexpr int TRANSFORMER_HIDDEN_DIM = 32;
inline constexpr int TRANSFORMER_SEQ_LENGTH = 32;
inline constexpr int TRANSFORMER_VOCAB_SIZE = 262144;
inline constexpr int TRANSFORMER_QUANT_BITS = 8;

struct TransformerWeights {
    std::vector<std::vector<float>> embedding;
    std::vector<float> layer_weights;
    std::vector<float> attention_weights;
    std::vector<float> output_weights;
};

TransformerWeights GenerateWeights(const uint256& seed);
uint256 ComputeTransformerProof(const CBlockHeader& header, const TransformerWeights* cached_weights = nullptr);
std::vector<int> EncodeBlockHeader(const CBlockHeader& header);
std::vector<float> TransformerForward(const std::vector<int>& tokens, const TransformerWeights& weights);
uint256 QuantizeOutput(const std::vector<float>& output);

class TransformerPoWCache {
private:
    mutable std::mutex cache_mutex;
    std::unique_ptr<TransformerWeights> cached_weights;
    uint256 cached_seed;
    bool is_valid;

public:
    TransformerPoWCache();

    const TransformerWeights* GetWeights(const uint256& seed);
    void Invalidate();
    bool IsValid() const;
};

TransformerPoWCache& GetTransformerPoWCache();

#endif // TKN_POW_TRANSFORMERPOW_H
