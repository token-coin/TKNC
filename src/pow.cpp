#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <logging.h>
#include <primitives/block.h>
#include <uint256.h>
#include <util/check.h>
#include <pow/tknchash.h>
#include <pow/difficulty.h>

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    if (params.fPowNoRetargeting) {
        return pindexLast->nBits;
    }

    if (params.fPowAllowMinDifficultyBlocks)
    {
        if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing * 2)
            return UintToArith256(params.powLimit).GetCompact();
    }

    arith_uint256 next_target = CalculateNextDifficultyTarget(pindexLast, params);
    return next_target.GetCompact();
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    arith_uint256 next_target = CalculateNextDifficultyTarget(pindexLast, params);
    return next_target.GetCompact();
}

bool PermittedDifficultyTransition(const Consensus::Params& params, int64_t height, uint32_t old_nbits, uint32_t new_nbits)
{
    if (params.fPowAllowMinDifficultyBlocks) return true;

    const arith_uint256 pow_limit = UintToArith256(params.powLimit);
    arith_uint256 new_target;
    new_target.SetCompact(new_nbits);

    if (new_target > pow_limit) {
        return false;
    }

    arith_uint256 old_target;
    old_target.SetCompact(old_nbits);

    arith_uint256 max_target = old_target * 4;
    if (max_target > pow_limit) {
        max_target = pow_limit;
    }

    arith_uint256 min_target = old_target / 4;
    if (min_target == 0) {
        min_target = arith_uint256(1);
    }

    if (new_target > max_target) return false;
    if (new_target < min_target) return false;

    return true;
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    if (hash == params.hashGenesisBlock) {
        return true;
    }

    if (EnableFuzzDeterminism()) return (hash.data()[31] & 0x80) == 0;

    return CheckProofOfWorkImpl(hash, nBits, params);
}

std::optional<arith_uint256> DeriveTarget(unsigned int nBits, const uint256 pow_limit)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(pow_limit))
        return {};

    return bnTarget;
}

bool CheckProofOfWorkImpl(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    auto bnTarget{DeriveTarget(nBits, params.powLimit)};
    if (!bnTarget) return false;

    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}

bool CheckProofOfWork(const CBlockHeader& header, const Consensus::Params& params)
{
    if (header.GetHash() == params.hashGenesisBlock) {
        return true;
    }

    if (params.fPowNoRetargeting && EnableFuzzDeterminism()) {
        return (header.GetHash().data()[31] & 0x80) == 0;
    }

    auto bnTarget{DeriveTarget(header.nBits, params.powLimit)};
    if (!bnTarget) {
        return false;
    }

    if (header.nTime == 0) {
        return false;
    }

    uint256 powHash = TKNCComputeHash(header);
    if (UintToArith256(powHash) > *bnTarget) {
        return false;
    }

    return true;
}
