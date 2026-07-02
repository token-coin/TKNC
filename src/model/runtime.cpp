#include <model/runtime.h>
#include <pow/transformerpow.h>
#include <logging.h>
#include <vector>
#include <hash.h>

ModelRuntime::ModelRuntime() : is_loaded(false) {
}

ModelRuntime::~ModelRuntime() {
}

bool ModelRuntime::LoadModel(const std::string& path) {
    std::lock_guard<std::mutex> lock(weights_mutex);

    std::vector<unsigned char> path_bytes(path.begin(), path.end());
    uint256 seed = Hash(path_bytes);
    weights = GenerateWeights(seed);

    model_path = path;
    is_loaded = true;

    return true;
}

bool ModelRuntime::IsLoaded() const {
    return is_loaded;
}

const std::string& ModelRuntime::GetModelPath() const {
    return model_path;
}

std::vector<float> ModelRuntime::ProcessTask(const std::vector<float>& input) {
    std::vector<int> tokens(input.size());
    for (size_t i = 0; i < input.size(); i++) {
        tokens[i] = static_cast<int>(input[i] * 1000) % TRANSFORMER_VOCAB_SIZE;
    }

    return Forward(tokens);
}

std::vector<float> ModelRuntime::Forward(const std::vector<int>& tokens) {
    std::lock_guard<std::mutex> lock(weights_mutex);
    return TransformerForward(tokens, weights);
}
