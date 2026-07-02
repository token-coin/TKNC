#ifndef TKNC_MINER_MINER_H
#define TKNC_MINER_MINER_H

#include <memory>
#include <string>
#include <univalue.h>

class CChainParams;
class ModelLoader;
class ModeSwitcher;

bool GenerateTKNC(bool fGenerate, int nThreads, const CChainParams& chainparams);
bool GetMiningInfo(const CChainParams& chainparams, UniValue& result);

void SetRPCConfig(const std::string& rpcUser, const std::string& rpcPassword,
                  const std::string& rpcConnect, int rpcPort,
                  const std::string& miningAddress = "");
bool SubmitBlockToNode(const std::string& hexBlock);

// Global access functions
ModelLoader& GetModelLoader();
ModeSwitcher& GetModeSwitcher();

// Cookie-based RPC authentication (reads <exe>/data/.cookie)
bool TryReadCookieAuth(std::string& outUser, std::string& outPass);

#endif // TKNC_MINER_MINER_H
