#include <model/registry.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <univalue.h>
#include <util/log.h>
#include <util/time.h>

static RPCMethod registermodel()
{
    return RPCMethod{"registermodel",
        "Register a model in the node's Model Registry.\n"
        "Required before a miner can serve LLM inference requests.\n",
        {
            {"model_name", RPCArg::Type::STR, RPCArg::Optional::NO, "Model identifier"},
            {"model_hash", RPCArg::Type::STR, RPCArg::Optional::NO, "SHA256 hash of GGUF file"},
            {"file_path", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Local file path to GGUF"},
            {"size_mb", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Model file size in MB"},
            {"quantization", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Quantization type (e.g. Q4_K_M)"},
            {"gpu_capability", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "GPU name or \"CPU\""},
            {"publisher_wallet", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Publisher/miner wallet address"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "model_name", "Registered model name"},
                {RPCResult::Type::STR, "model_hash", "Registered model hash"},
                {RPCResult::Type::BOOL, "loaded", "Whether model is currently loaded"},
                {RPCResult::Type::NUM, "registered_at", "Registration timestamp"},
            }
        },
        RPCExamples{
            HelpExampleCli("registermodel",
                "\"qwen2.5-0.5b-instruct\" \"sha256:abc123...\" \"./models/model.gguf\" 468 \"Q4_K_M\" \"P106-100\" \"token1q...\"")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
        {
            ModelRegistryEntry entry;
            entry.model_name = request.params[0].get_str();
            entry.model_hash = request.params[1].get_str();
            entry.file_path = request.params.size() > 2 ? request.params[2].get_str() : "";
            entry.size_mb = request.params.size() > 3 ? request.params[3].getInt<int64_t>() : 0;
            entry.quantization = request.params.size() > 4 ? request.params[4].get_str() : "";
            entry.gpu_capability = request.params.size() > 5 ? request.params[5].get_str() : "CPU";
            entry.publisher_wallet = request.params.size() > 6 ? request.params[6].get_str() : "";
            entry.loaded = false;
            entry.registered_at = GetTime();

            if (entry.model_name.empty() || entry.model_hash.empty()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "model_name and model_hash are required");
            }

            ModelRegistry& registry = GetModelRegistry();
            if (!registry.RegisterModel(entry)) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to register model");
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("model_name", entry.model_name);
            result.pushKV("model_hash", entry.model_hash);
            result.pushKV("loaded", entry.loaded);
            result.pushKV("registered_at", entry.registered_at);

            LogInfo("RPC: registermodel - Registered model %s (hash=%s)",
                     entry.model_name, entry.model_hash);

            return result;
        },
    };
}

static RPCMethod getmodelinfo()
{
    return RPCMethod{"getmodelinfo",
        "Get information about a registered model.",
        {
            {"model_name", RPCArg::Type::STR, RPCArg::Optional::NO, "Model name"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "model_name", ""},
                {RPCResult::Type::STR, "model_hash", ""},
                {RPCResult::Type::STR, "file_path", ""},
                {RPCResult::Type::NUM, "size_mb", ""},
                {RPCResult::Type::STR, "quantization", ""},
                {RPCResult::Type::STR, "gpu_capability", ""},
                {RPCResult::Type::STR, "publisher_wallet", ""},
                {RPCResult::Type::BOOL, "loaded", ""},
                {RPCResult::Type::NUM, "registered_at", ""},
            }
        },
        RPCExamples{
            HelpExampleCli("getmodelinfo", "\"qwen2.5-0.5b-instruct\"")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string model_name = request.params[0].get_str();
            ModelRegistry& registry = GetModelRegistry();
            ModelRegistryEntry entry = registry.GetModelInfo(model_name);

            if (entry.model_name.empty()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Model not found in registry");
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("model_name", entry.model_name);
            result.pushKV("model_hash", entry.model_hash);
            result.pushKV("file_path", entry.file_path);
            result.pushKV("size_mb", entry.size_mb);
            result.pushKV("quantization", entry.quantization);
            result.pushKV("gpu_capability", entry.gpu_capability);
            result.pushKV("publisher_wallet", entry.publisher_wallet);
            result.pushKV("loaded", entry.loaded);
            result.pushKV("registered_at", entry.registered_at);

            return result;
        },
    };
}

static RPCMethod listmodels()
{
    return RPCMethod{"listmodels",
        "List all models registered in the node's Model Registry.",
        {},
        RPCResult{
            RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "model_name", ""},
                        {RPCResult::Type::STR, "model_hash", ""},
                        {RPCResult::Type::STR, "gpu_capability", ""},
                        {RPCResult::Type::BOOL, "loaded", ""},
                        {RPCResult::Type::NUM, "registered_at", ""},
                    }
                }
            }
        },
        RPCExamples{
            HelpExampleCli("listmodels", "")
        },
        [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
        {
            ModelRegistry& registry = GetModelRegistry();
            auto models = registry.GetAllModels();

            UniValue result(UniValue::VARR);
            for (const auto& model : models) {
                UniValue obj(UniValue::VOBJ);
                obj.pushKV("model_name", model.model_name);
                obj.pushKV("model_hash", model.model_hash);
                obj.pushKV("gpu_capability", model.gpu_capability);
                obj.pushKV("loaded", model.loaded);
                obj.pushKV("registered_at", model.registered_at);
                result.push_back(obj);
            }

            return result;
        },
    };
}

void RegisterModelRegistryRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"model", &registermodel},
        {"model", &getmodelinfo},
        {"model", &listmodels},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}