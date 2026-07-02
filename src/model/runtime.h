#ifndef TKN_MODEL_RUNTIME_H
#define TKN_MODEL_RUNTIME_H

#include <pow/transformerpow.h>
#include <string>
#include <vector>
#include <mutex>

class ModelRuntime {
private:
    TransformerWeights weights;
    bool is_loaded;
    std::string model_path;
    mutable std::mutex weights_mutex;

public:
    ModelRuntime();
    ~ModelRuntime();

    bool LoadModel(const std::string& path);
    bool IsLoaded() const;
    const std::string& GetModelPath() const;

    std::vector<float> ProcessTask(const std::vector<float>& input);

private:
    std::vector<float> Forward(const std::vector<int>& tokens);
};

#endif // TKN_MODEL_RUNTIME_H
