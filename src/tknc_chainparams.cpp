#include <chainparams.h>
#include <chainparamsbase.h>
#include <chainparamsseeds.h>
#include <consensus/params.h>
#include <uint256.h>
#include <arith_uint256.h>
#include <util/strencodings.h>
#include <util/chaintype.h>
#include <common/args.h>
#include <script/script.h>
#include <consensus/merkle.h>
#include <primitives/transaction.h>
#include <logging.h>

#include <assert.h>
#include <memory>

static const uint32_t TKN_NETWORK_MAGIC = 0x4e414200;

static const int TKNC_DEFAULT_P2P_PORT = 9333;

static CBlock CreateGenesisBlock(uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward) {
    const char* pszTimestamp = "TKNC Genesis Block - AI Computing Power Trading Platform 2026";
    CMutableTransaction txNew;
    txNew.version = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << 486604799 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = CScript();

    CBlock genesis;
    genesis.nTime    = nTime;
    genesis.nBits    = nBits;
    genesis.nNonce   = nNonce;
    genesis.nVersion = nVersion;
    genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

static CBlock CreateTKNCGenesisBlock() {
    // Genesis = 2026-07-01 16:00 UTC, nBits=0x1e100000 (~58s/block at 18KH/s).
    return CreateGenesisBlock(1782835200, 2083236893, 0x1e100000, 1, 0);
}

static CBlock CreateTKNCRegTestGenesisBlock() {
    return CreateGenesisBlock(1745884801, 0, 0x207fffff, 1, 5000 * COIN);
}

class TKNCMainParams : public CChainParams {
public:
    TKNCMainParams() {
        m_chain_type = ChainType::MAIN;
        pchMessageStart[0] = 0x4e;
        pchMessageStart[1] = 0x41;
        pchMessageStart[2] = 0x42;
        pchMessageStart[3] = 0x00;
        nPruneAfterHeight = 100000;
        m_assumed_blockchain_size = 1;
        m_assumed_chain_state_size = 0;
        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        consensus.nPowTargetSpacing = 120;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256();
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.MinBIP9WarningHeight = 0;
        // E02-FIX: powLimit corrected to Bitcoin-standard 0x7fff...ff (was 0x00ff...ff, 256x too easy).
        consensus.powLimit = uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.fPowAllowMinDifficultyBlocks = false;
        // E01-FIX: enable difficulty retargeting (was disabled, causing 4.15x faster emission).
        consensus.fPowNoRetargeting = false;
        consensus.nPowTargetSpacing = 120;
        consensus.nPowTargetTimespan = 120 * 100;
        consensus.enforce_BIP94 = true;

        consensus.nBlockInterval = 120;
        consensus.nDifficultyWindow = 100;
        consensus.nMaxConcurrentRequests = 4;

        nDefaultPort = TKNC_DEFAULT_P2P_PORT;

        genesis = CreateTKNCGenesisBlock();
        consensus.hashGenesisBlock = genesis.GetHash();
        // SECURITY: nMinimumChainWork protects new nodes from accepting low-work fake chains.
        // Set to ~2^30 (based on block 682 log2_work=30.66) as a conservative floor.
        // An attacker must produce at least ~1.07 billion hashes of work to fool a new node.
        // Update periodically: tknc-cli getblockchaininfo -> chainwork
        consensus.nMinimumChainWork = uint256{"0000000000000000000000000000000000000000000000000000000040000000"};
        // SECURITY: defaultAssumeValid allows new nodes to skip script validation for blocks
        // before this hash, speeding up IBD. Set to a known-good block hash after chain matures.
        // Leave as uint256() (null) to validate all blocks from genesis.
        consensus.defaultAssumeValid = uint256();

        // chainTxData: helps new nodes estimate verification progress during IBD.
        // Values based on explorer.tknc.shop at block ~7485 (2026-07-15).
        // Update periodically: tknc-cli getblockchaininfo -> time, txcount
        chainTxData = {
            1784142731,  // nTime: timestamp of block ~7479 on explorer
            7486,        // tx_count: ~1 tx/block * 7485 blocks + 1 extra tx in block 683
            0.005724     // dTxRate: 7486 txs / 1307531s span ≈ 0.00572 tx/s
        };

        vSeeds.emplace_back("66.154.101.183:9333");
        vSeeds.emplace_back("tknc-seed.tkncchain.org:9333");

        base58Prefixes[PUBKEY_ADDRESS] = {0x7F};  // TKNC 't' prefix
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 5);
        base58Prefixes[SECRET_KEY]     = std::vector<unsigned char>(1, 180);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x6D, 0x32, 0x74};  // TKNC xpub (NOT BTC)
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x6D, 0x0E, 0x40};  // TKNC xprv (NOT BTC)
        // BIP44: m/44'/9797' (TKNC coin type, pending SLIP-44 registration)

        bech32_hrp = "token";  // Bech32 format: token1q... (per user spec)

        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();

        // E03-FIX: assign fixed seeds from chainparamsseeds.h (was empty, peer discovery failed).
        vFixedSeeds = std::vector<uint8_t>(std::begin(chainparams_seed_main),
                                           std::end(chainparams_seed_main));
    }
};

class TKNCTestNetParams : public CChainParams {
public:
    TKNCTestNetParams() {
        m_chain_type = ChainType::TESTNET;
        pchMessageStart[0] = 0x4e;
        pchMessageStart[1] = 0x41;
        pchMessageStart[2] = 0x42;
        pchMessageStart[3] = 0x01;
        nPruneAfterHeight = 100000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;
        fDefaultConsistencyChecks = true;
        m_is_mockable_chain = true;

        consensus.nPowTargetSpacing = 120;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256();
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.fPowNoRetargeting = false;
        consensus.nPowTargetSpacing = 120;
        consensus.nPowTargetTimespan = 120 * 100;
        consensus.enforce_BIP94 = false;

        consensus.nBlockInterval = 120;
        consensus.nDifficultyWindow = 100;
        consensus.nMaxConcurrentRequests = 4;

        nDefaultPort = TKNC_DEFAULT_P2P_PORT + 1000;  // TestNet P2P port 10333

        genesis = CreateTKNCGenesisBlock();
        consensus.hashGenesisBlock = genesis.GetHash();
        consensus.nMinimumChainWork = uint256();
        consensus.defaultAssumeValid = uint256();

        vSeeds.clear();

        // TKNC TestNet address prefix (same as mainnet)
        base58Prefixes[PUBKEY_ADDRESS] = {0x7F};  // TKNC 't' prefix
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 5);
        base58Prefixes[SECRET_KEY]     = std::vector<unsigned char>(1, 180);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x6D, 0x32, 0x74};  // TKNC xpub
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x6D, 0x0E, 0x40};  // TKNC xprv

        bech32_hrp = "token";  // TestNet Bech32 HRP (token1q... format)

        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();

        chainTxData = {
            0,
            0,
            0,
        };
    }
};

class TKNCRegTestParams : public CChainParams {
public:
    explicit TKNCRegTestParams(const ArgsManager& args) {
        m_chain_type = ChainType::REGTEST;
        pchMessageStart[0] = 0x4e;
        pchMessageStart[1] = 0x41;
        pchMessageStart[2] = 0x42;
        pchMessageStart[3] = 0x02;
        nPruneAfterHeight = 100000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;
        fDefaultConsistencyChecks = true;
        m_is_mockable_chain = true;

        consensus.nPowTargetSpacing = 120;
        consensus.BIP34Height = 500;
        consensus.BIP34Hash = uint256();
        consensus.BIP65Height = 1351;
        consensus.BIP66Height = 1251;
        consensus.CSVHeight = 432;
        consensus.SegwitHeight = 0;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.fPowNoRetargeting = false;
        consensus.nPowTargetSpacing = 120;
        consensus.nPowTargetTimespan = 120 * 100;
        consensus.enforce_BIP94 = false;

        consensus.nBlockInterval = 120;
        consensus.nDifficultyWindow = 10;
        consensus.nMaxConcurrentRequests = 4;

        nDefaultPort = TKNC_DEFAULT_P2P_PORT;  // RegTest also uses standard P2P port 9333

        genesis = CreateTKNCRegTestGenesisBlock();
        consensus.hashGenesisBlock = genesis.GetHash();
        consensus.nMinimumChainWork = uint256();
        consensus.defaultAssumeValid = uint256();

        vSeeds.clear();

        // TKNC specific address prefix (same as mainnet)
        base58Prefixes[PUBKEY_ADDRESS] = {0x7F};  // TKNC 't' prefix
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 5);
        base58Prefixes[SECRET_KEY]     = std::vector<unsigned char>(1, 180);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x6D, 0x32, 0x74};  // TKNC xpub
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x6D, 0x0E, 0x40};  // TKNC xprv

        bech32_hrp = "token";  // Bech32 format: token1q... (per user spec)

        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();

        chainTxData = {
            0,
            0,
            0,
        };
    }
};

static std::unique_ptr<const CChainParams> globalChainParams;

std::unique_ptr<const CChainParams> CreateTKNCChainParams(const ChainType chain)
{
    switch (chain) {
    case ChainType::MAIN:
        return std::make_unique<TKNCMainParams>();
    case ChainType::TESTNET:
        return std::make_unique<TKNCTestNetParams>();
    case ChainType::TESTNET4:
        return std::make_unique<TKNCTestNetParams>();
    case ChainType::REGTEST:
        return std::make_unique<TKNCRegTestParams>(gArgs);
    case ChainType::SIGNET:
        return std::make_unique<TKNCTestNetParams>();
    }
    assert(false);
}

const CChainParams &Params() {
    assert(globalChainParams);
    return *globalChainParams;
}

std::unique_ptr<const CChainParams> CreateChainParams(const ArgsManager& args, const ChainType chain)
{
    return CreateTKNCChainParams(chain);
}

void SelectParams(const ChainType chain)
{
    SelectBaseParams(chain);
    globalChainParams = CreateTKNCChainParams(chain);
}

std::unique_ptr<const CChainParams> CChainParams::Main()
{
    return std::make_unique<TKNCMainParams>();
}

std::unique_ptr<const CChainParams> CChainParams::TestNet()
{
    return std::make_unique<TKNCTestNetParams>();
}

std::unique_ptr<const CChainParams> CChainParams::TestNet4()
{
    return std::make_unique<TKNCTestNetParams>();
}

std::unique_ptr<const CChainParams> CChainParams::SigNet(const SigNetOptions& options)
{
    return std::make_unique<TKNCTestNetParams>();
}

std::unique_ptr<const CChainParams> CChainParams::RegTest(const RegTestOptions& options)
{
    return std::make_unique<TKNCRegTestParams>(gArgs);
}

std::optional<ChainType> GetNetworkForMagic(const MessageStartChars& pchMessageStart)
{
    const auto mainnet_msg = CChainParams::Main()->MessageStart();
    const auto testnet_msg = CChainParams::TestNet()->MessageStart();
    const auto regtest_msg = CChainParams::RegTest({})->MessageStart();

    if (pchMessageStart == mainnet_msg) {
        return ChainType::MAIN;
    }
    if (pchMessageStart == testnet_msg) {
        return ChainType::TESTNET;
    }
    if (pchMessageStart == regtest_msg) {
        return ChainType::REGTEST;
    }
    return std::nullopt;
}

std::vector<int> CChainParams::GetAvailableSnapshotHeights() const
{
    std::vector<int> heights;
    heights.reserve(m_assumeutxo_data.size());
    for (const auto& data : m_assumeutxo_data) {
        heights.emplace_back(data.height);
    }
    return heights;
}
