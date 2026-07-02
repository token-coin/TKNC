#include <pow/transformerpow.h>
#include <miner/mode_switcher.h>
#include <hash.h>
#include <arith_uint256.h>
#include <logging.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>

// Generate deterministic weights (consistent with GPU RealGPUMiner: mt19937-style PRNG)
TransformerWeights GenerateWeights(const uint256& seed) {
    TransformerWeights weights;

    uint32_t state = 42;
    for (size_t i = 0; i < 32; i++) {
        state ^= (state >> (i % 4 + 1)) * 0x9E3779B9u + (seed.begin()[i] << ((i * 3) % 24));
    }

    auto nextFloat = [&state]() -> float {
        state ^= (state << 13);
        state ^= (state >> 17);
        state ^= (state << 5);
        return (float)(state & 0xFFFFFF) / (float)16777216.0f - 0.5f;
    };

    weights.embedding.resize(TRANSFORMER_VOCAB_SIZE, std::vector<float>(TRANSFORMER_HIDDEN_DIM));
    for (int i = 0; i < TRANSFORMER_VOCAB_SIZE; i++) {
        for (int j = 0; j < TRANSFORMER_HIDDEN_DIM; j++) {
            weights.embedding[i][j] = nextFloat() * 2.0f;
        }
    }

    int total_attn_weights = 65536 * 16;
    weights.attention_weights.resize(total_attn_weights);
    for (int i = 0; i < total_attn_weights; i++) {
        weights.attention_weights[i] = nextFloat() * 1.5f;
    }

    int layer_size = TRANSFORMER_HIDDEN_DIM * TRANSFORMER_HIDDEN_DIM;
    weights.layer_weights.resize(TRANSFORMER_NUM_LAYERS * layer_size);
    for (int i = 0; i < TRANSFORMER_NUM_LAYERS * layer_size; i++) {
        weights.layer_weights[i] = nextFloat();
    }

    int output_size = TRANSFORMER_HIDDEN_DIM * 16;
    weights.output_weights.resize(output_size);
    for (int i = 0; i < output_size; i++) {
        weights.output_weights[i] = nextFloat();
    }

    return weights;
}

// Encode block header as token sequence (consistent with GPU RealGPUMiner)
std::vector<int> EncodeBlockHeader(const CBlockHeader& header) {
    std::vector<int> tokens(TRANSFORMER_SEQ_LENGTH, 0);
    uint32_t nonce = header.nNonce;

    const uint8_t* data = reinterpret_cast<const uint8_t*>(&header);
    for (int ti = 0; ti < TRANSFORMER_SEQ_LENGTH; ti++) {
        int byte_idx = (ti / 4) % 80;
        if (byte_idx < 80) {
            uint32_t header_val = static_cast<uint32_t>(data[byte_idx]);
            uint32_t idx = (header_val + nonce + ti * 7 + (nonce >> (ti % 16))) % (1024 * 256);
            tokens[ti] = static_cast<int>(idx);
        }
    }
    return tokens;
}

// Transformer forward pass (fully consistent with GPU RealGPUMiner HLSL)
std::vector<float> TransformerForward(const std::vector<int>& tokens, const TransformerWeights& weights) {
    int seq_len = TRANSFORMER_SEQ_LENGTH;
    int hidden_dim = TRANSFORMER_HIDDEN_DIM;
    uint32_t nonce = 0;

    float hidden[32] = {0};
    const std::vector<std::vector<float>>& emb_table = weights.embedding;
    const std::vector<float>& attn_w = weights.attention_weights;

    for (int ti = 0; ti < seq_len && ti < 32; ti++) {
        uint32_t idx = (tokens[ti] % (1024 * 256)) % emb_table.size();
        float token_val = (idx < emb_table.size() && !emb_table[idx].empty()) ? emb_table[idx][0] : 0.0f;

        for (int pre = 0; pre < 4; pre++) {
            token_val += std::sin((float)nonce * 0.001f + (float)ti * 0.01f + (float)pre) * 0.01f;
            token_val *= std::cos(token_val + (float)pre * 0.5f);
        }
        hidden[ti % 32] = token_val;
    }

    for (int hi = 0; hi < hidden_dim; hi++) {
        float sum = 0.0f;
        for (int hj = 0; hj < hidden_dim; hj++) {
            uint32_t weight_idx_base = ((nonce % 512) * 1024 + hi * hidden_dim + hj);
            uint32_t weight_idx1 = weight_idx_base % (65536 * 8);
            uint32_t weight_idx2 = (weight_idx_base + nonce) % (65536 * 8);

            float w1 = (weight_idx1 < attn_w.size()) ? attn_w[weight_idx1] : 0.0f;
            float w2 = (weight_idx2 < attn_w.size()) ? attn_w[weight_idx2] : 0.0f;

            sum += hidden[hj] * w1;
            sum += hidden[hj] * w2 * 0.5f;
            sum = std::sin(sum) * std::cos(sum * 0.7f) + std::tanh(sum);
        }
        hidden[hi] = sum;

        for (int head = 0; head < 8; head++) {
            float q = hidden[hi] * (0.1f + 0.005f * head);
            float k = hidden[(hi + head * 4) % 32] * (0.15f + 0.01f * (nonce % 100));
            float v = hidden[(hi + head * 7 + nonce) % 32] * (0.12f + 0.008f * head);
            float attn_val = q * k * v;
            attn_val += std::sin(attn_val) * std::cos(attn_val * 0.3f);
            if (head == 0) hidden[hi] = 0;
            hidden[hi] += attn_val;
        }
        hidden[hi] *= 0.125f;

        float ff1 = hidden[hi] * 2.0f + 1.0f;
        float ff2 = std::max(0.0f, ff1);
        hidden[hi] = std::tanh(ff2 * 0.5f + hidden[hi]);
    }

    for (int layer = 0; layer < TRANSFORMER_NUM_LAYERS; layer++) {
        float new_hidden[32] = {0};
        float mean_val = 0.0f;
        for (int mi = 0; mi < 32; mi++) mean_val += hidden[mi];
        mean_val /= 32.0f;
        for (int ni = 0; ni < 32; ni++) {
            hidden[ni] -= mean_val;
            hidden[ni] *= (1.0f / std::sqrt(std::abs(hidden[ni]) + 0.0001f));
        }

        for (int ni = 0; ni < 32; ni++) {
            float layer_sum = 0.0f;
            for (int nj = 0; nj < 32; nj++) {
                uint32_t weight_idx = (layer * 131072 + nonce * 32 + ni * 32 + nj) % (65536 * 16);
                float w1 = (weight_idx < attn_w.size()) ? attn_w[weight_idx] : 0.0f;
                float w2 = ((weight_idx + 65536) % (65536 * 16) < attn_w.size()) ? attn_w[(weight_idx + 65536) % (65536 * 16)] : 0.0f;

                layer_sum += hidden[nj] * w1;
                layer_sum += hidden[nj] * w2 * 0.3f;
                layer_sum = std::sin(layer_sum + (float)(nonce + layer)) * std::cos(layer_sum * 0.5f);
                layer_sum += std::tanh(layer_sum) * std::exp(-std::abs(layer_sum) * 0.1f);
            }

            float layer_attn = 0.0f;
            for (int lh = 0; lh < 4; lh++) {
                float lq = layer_sum * (0.2f + 0.01f * lh);
                float lk = hidden[(ni + lh * 8) % 32] * (0.18f + 0.015f * layer);
                layer_attn += lq * lk;
                layer_attn = std::sin(layer_attn) * std::cos(layer_attn * 0.4f);
            }
            new_hidden[ni] = std::tanh(layer_sum + layer_attn * 0.25f);

            float pff = new_hidden[ni] * 3.0f - 1.0f;
            new_hidden[ni] += std::max(0.0f, pff) * 0.5f;
        }

        for (int ci = 0; ci < 32; ci++) {
            hidden[ci] = hidden[ci] + new_hidden[ci] * 0.7f;
            hidden[ci] = std::tanh(hidden[ci]);
        }
    }

    return std::vector<float>(hidden, hidden + 32);
}

// Quantize output to 256-bit hash (consistent with GPU RealGPUMiner: 16-dim projection -> uint32 hash -> expand to uint256)
uint256 QuantizeOutput(const std::vector<float>& output) {
    const std::vector<float>& attn_w = GetTransformerPoWCache().IsValid() ?
        GetTransformerPoWCache().GetWeights(uint256())->attention_weights :
        std::vector<float>();
    uint32_t nonce = 0;
    float hidden[32];
    for (int i = 0; i < 32 && i < (int)output.size(); i++) hidden[i] = output[i];

    float output_proj[16] = {0};
    for (int oi = 0; oi < 16; oi++) {
        float proj_sum = 0.0f;
        for (int oj = 0; oj < 32; oj++) {
            uint32_t proj_idx = (oi * 32 + oj + nonce * 17) % (65536 * 4);
            float w = (proj_idx < attn_w.size()) ? attn_w[proj_idx] : 0.0f;
            proj_sum += hidden[oj] * w;
            proj_sum = std::sin(proj_sum + (float)oi) * std::cos(proj_sum * 0.3f);
        }
        output_proj[oi] = std::tanh(proj_sum);
    }

    uint32_t hash_result = 0;
    for (int qi = 0; qi < 16; qi++) {
        uint32_t quantized = (uint32_t)((output_proj[qi] + 1.0f) * 127.5f) & 0xFF;
        hash_result ^= (quantized << ((qi * 8) % 32));
        hash_result = (hash_result << 7) | (hash_result >> 25);

        if (qi < 16 && qi < 32) {
            uint32_t hidden_quant = (uint32_t)((hidden[qi] + 1.0f) * 63.75f) & 0x7F;
            hash_result ^= (hidden_quant << ((qi * 4 + 16) % 32));
        }
    }

    hash_result ^= (nonce * 2654435761u);
    hash_result ^= (uint32_t)((float)nonce * 0.123456789f) & 0xFFFF;

    uint8_t hash_bytes[32] = {0};
    for (int i = 0; i < 32; i++) {
        hash_bytes[i] = (uint8_t)((hash_result >> ((i * 8) % 32)) & 0xFF);
        hash_result ^= (hash_result * 2654435769u) + 0x9E3779B9u;
    }

    return uint256(hash_bytes);
}

// Compute Transformer PoW proof
uint256 ComputeTransformerProof(const CBlockHeader& header, const TransformerWeights* cached_weights) {
    // 1. Generate or use cached weights
    const TransformerWeights* weights_ptr = nullptr;
    TransformerWeights local_weights;
    
    if (cached_weights != nullptr) {
        weights_ptr = cached_weights;
    } else {
        // Use block header hash without nonce as seed, ensuring mining and verification use same weights
        uint256 seed = header.GetHashWithoutNonce();
        const TransformerWeights* cached = GetTransformerPoWCache().GetWeights(seed);
        if (cached != nullptr) {
            weights_ptr = cached;
        } else {
            local_weights = GenerateWeights(seed);
            weights_ptr = &local_weights;
        }
    }
    
    // 2. Encode block header as token sequence
    std::vector<int> tokens = EncodeBlockHeader(header);
    
    // 3. Transformer forward pass
    std::vector<float> output = TransformerForward(tokens, *weights_ptr);
    
    // 4. Quantize output to 256-bit hash
    uint256 proof = QuantizeOutput(output);
    
    return proof;
}

// TransformerPoWCache implementation
TransformerPoWCache::TransformerPoWCache() : is_valid(false) {
}

const TransformerWeights* TransformerPoWCache::GetWeights(const uint256& seed) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    
    if (is_valid && cached_seed == seed) {
        return cached_weights.get();
    }
    
    // Cache miss, generate new weights
    cached_weights = std::make_unique<TransformerWeights>(GenerateWeights(seed));
    cached_seed = seed;
    is_valid = true;
    
    return cached_weights.get();
}

void TransformerPoWCache::Invalidate() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    is_valid = false;
    cached_weights.reset();
}

bool TransformerPoWCache::IsValid() const {
    std::lock_guard<std::mutex> lock(cache_mutex);
    return is_valid;
}

TransformerPoWCache& GetTransformerPoWCache() {
    static TransformerPoWCache cache;
    return cache;
}
