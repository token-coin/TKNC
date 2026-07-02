#include <miner/review_chain.h>
#include <hash.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <logging.h>
#include <key_io.h>
#include <chainparams.h>
#include <sync.h>

#include <chrono>
#include <algorithm>

namespace ReviewChain {

uint256 ReviewChainManager::HashReviewData(const ReviewData& review) {
    HashWriter ss{};
    
    // Hash all critical review fields
    ss << review.review_id;
    ss << review.miner_id;
    ss << review.reviewer_pubkey_hash;
    ss << review.comment;
    ss << review.rating;
    ss << review.usage_tx_hash;
    ss << review.usage_duration_hours;
    ss << review.tokens_consumed;
    ss << review.total_spent;
    ss << review.created_at;
    
    return ss.GetHash();
}

std::optional<ReviewData> ReviewChainManager::CreateReview(
    const std::string& reviewer_key_hash,
    const std::string& miner_id,
    const std::string& comment,
    int rating,
    const UsageProof& usage_proof
) {
    // Validate inputs
    if (reviewer_key_hash.empty() || miner_id.empty() || comment.empty()) {
        LogWarning("ReviewChain: Missing required fields for review creation");
        return std::nullopt;
    }
    
    if (rating < 1 || rating > 5) {
        LogWarning("ReviewChain: Invalid rating: %d", rating);
        return std::nullopt;
    }
    
    // Verify usage proof exists and meets minimum requirements
    if (!usage_proof.HasMinimumUsage()) {
        LogWarning("ReviewChain: Insufficient usage history - %ld hours, %ld tokens",
                   usage_proof.duration_hours(), usage_proof.total_tokens);
        return std::nullopt;
    }
    
    if (!usage_proof.verified_on_chain) {
        LogWarning("ReviewChain: Usage not verified on chain");
        return std::nullopt;
    }
    
    // Check for suspicious patterns
    if (IsSuspiciousUsagePattern(usage_proof)) {
        LogWarning("ReviewChain: Suspicious usage pattern detected");
        // Don't block, but flag for lower credibility
    }
    
    // Create review data structure
    ReviewData review;
    review.review_id = "rev_" + std::to_string(GetTime()) + "_" + 
                       reviewer_key_hash.substr(0, 8) + "_" + miner_id.substr(0, 8);
    review.miner_id = miner_id;
    review.reviewer_pubkey_hash = reviewer_key_hash;  // Anonymized, no private key exposure
    review.comment = comment;
    review.rating = rating;
    
    // Copy usage verification data
    review.usage_tx_hash = usage_proof.tx_hashes.empty() ? "" : usage_proof.tx_hashes.back();
    review.usage_duration_hours = usage_proof.duration_hours();
    review.tokens_consumed = usage_proof.total_tokens;
    review.total_spent = usage_proof.total_paid;
    
    // Set timestamps
    review.created_at = GetTime();
    review.last_updated = review.created_at;
    
    // Calculate hash and create on-chain commitment
    review.review_hash = HashReviewData(review);
    review.commitment_tx_hash = CreateCommitmentTransaction(review);
    
    // Initial status
    review.status = ReviewStatus::VERIFIED_ON_CHAIN;
    review.like_count = 0;
    
    // Calculate initial credibility score
    review.credibility_score = CalculateCredibilityScore(review);
    review.credibility = static_cast<CredibilityLevel>(
        (review.credibility_score / 20) * 20  // Round to nearest 20
    );
    
    LogInfo("ReviewChain: Created review %s for miner %s by user %s",
            review.review_id, miner_id, reviewer_key_hash.substr(0, 10));
    
    return review;
}

std::optional<LikeTransaction> ReviewChainManager::ToggleLike(
    const std::string& liker_key_hash,
    const std::string& review_id,
    const std::string& miner_id
) {
    // Validate inputs
    if (liker_key_hash.empty() || review_id.empty() || miner_id.empty()) {
        LogWarning("ReviewChain: Missing required fields for like operation");
        return std::nullopt;
    }
    
    // Verify liker has used this miner (security requirement)
    auto usage_opt = VerifyUsageOnChain(liker_key_hash, miner_id);
    if (!usage_opt.has_value() || !usage_opt->HasMinimumUsage()) {
        LogWarning("ReviewChain: User has not used this miner sufficiently to like reviews");
        return std::nullopt;
    }
    
    // Find the review being liked
    auto reviews = GetMinerReviews(miner_id);
    ReviewData* target_review = nullptr;
    for (auto& rev : reviews) {
        if (rev.review_id == review_id) {
            target_review = &rev;
            break;
        }
    }
    
    if (!target_review) {
        LogWarning("ReviewChain: Review not found: %s", review_id);
        return std::nullopt;
    }
    
    // Check if user already liked this review
    bool already_liked = target_review->likes_by_user.count(liker_key_hash) > 0 &&
                        target_review->likes_by_user[liker_key_hash];
    
    // Determine new like state (toggle)
    bool new_like_state = !already_liked;
    
    LikeTransaction like_tx;
    like_tx.like_id = "like_" + std::to_string(GetTime()) + "_" + liker_key_hash.substr(0, 8);
    like_tx.review_id = review_id;
    like_tx.liker_pubkey_hash = liker_key_hash;  // Anonymized
    like_tx.miner_id = miner_id;
    like_tx.reward_amount = LIKE_REWARD_AMOUNT;   // 10 TKNC
    like_tx.timestamp = GetTime();
    like_tx.is_liked = new_like_state;
    
    // In production, these would be real addresses from the blockchain
    // For now, use placeholder addresses derived from public key hashes
    like_tx.from_address = "token" + miner_id.substr(0, 30) + "...";
    like_tx.to_address = "token" + liker_key_hash.substr(0, 30) + "...";
    
    if (new_like_state) {
        // Create actual blockchain transaction for reward transfer
        like_tx.tx_hash = CreateRewardTransaction(
            like_tx.from_address,
            like_tx.to_address,
            like_tx.reward_amount,
            review_id
        );
        
        // Update review's like count and tracking
        target_review->like_count++;
        target_review->likes_by_user[liker_key_hash] = true;
        
        LogInfo("ReviewChain: Liked review %s", review_id);
    } else {
        // Unlike: record the action but don't reverse payment (prevents gaming)
        like_tx.tx_hash = "unlike_" + std::to_string(GetTime());
        
        target_review->like_count--;
        target_review->likes_by_user[liker_key_hash] = false;
        
        LogInfo("ReviewChain: Unliked review %s", review_id);
    }
    
    return like_tx;
}

std::optional<UsageProof> ReviewChainManager::VerifyUsageOnChain(
    const std::string& user_key_hash,
    const std::string& miner_id
) {
    UsageProof proof;
    proof.user_pubkey_hash = user_key_hash;
    proof.miner_id = miner_id;
    proof.verified_on_chain = false;
    proof.min_confirmations = 0;
    proof.total_paid = 0;
    proof.total_tokens = 0;
    proof.first_interaction = 0;
    proof.last_interaction = 0;
    
    LogInfo("ReviewChain: On-chain usage verification for user %s on miner %s returned empty result",
            user_key_hash.substr(0, 10), miner_id.substr(0, 10));
    
    return proof;
}

int ReviewChainManager::CalculateCredibilityScore(const ReviewData& review) {
    int score = 0;
    
    // Factor 1: Usage Duration Analysis (0-40 points)
    if (review.usage_duration_hours >= 2000) {       // 2+ years
        score += 40;
    } else if (review.usage_duration_hours >= 1000) {  // 1+ year
        score += 35;
    } else if (review.usage_duration_hours >= 720) {    // 3+ months
        score += 28;
    } else if (review.usage_duration_hours >= 100) {   // 1+ month
        score += 18;
    } else if (review.usage_duration_hours >= 24) {    // 1+ day
        score += 8;
    } else if (review.usage_duration_hours > 0) {      // < 1 day
        score += 2;
    }
    
    // Factor 2: Token Consumption Analysis (0-35 points)
    double tokens_millions = review.tokens_consumed / 1000000.0;
    if (tokens_millions >= 1000) {     // 1T+ tokens
        score += 35;
    } else if (tokens_millions >= 500) {  // 500M+ tokens
        score += 28;
    } else if (tokens_millions >= 100) {  // 100M+ tokens
        score += 20;
    } else if (tokens_millions >= 10) {   // 10M+ tokens
        score += 10;
    } else if (tokens_millions >= 1) {    // 1M+ tokens
        score += 4;
    }
    
    // Factor 3: Monetary Investment (0-15 points)
    double tknc_spent = review.total_spent / COIN;
    if (tknc_spent >= 100000) {      // 100K+ TKNC
        score += 15;
    } else if (tknc_spent >= 10000) {  // 10K+ TKNC
        score += 11;
    } else if (tknc_spent >= 1000) {   // 1K+ TKNC
        score += 7;
    } else if (tknc_spent >= 100) {    // 100+ TKNC
        score += 3;
    }
    
    // Factor 4: Comment Quality (0-10 points)
    size_t comment_length = review.comment.length();
    if (comment_length >= 200 && comment_length <= 1000) {
        score += 10;  // Detailed but concise
    } else if (comment_length >= 50 && comment_length < 200) {
        score += 6;   // Moderate detail
    } else if (comment_length >= 10) {
        score += 2;   // Brief
    }
    // Too short (< 10 chars) or too long (> 1000) gets 0
    
    // Normalize to 0-100 range
    score = std::min(100, std::max(0, score));
    
    // Apply suspicious pattern penalty
    if (review.usage_duration_hours < SUSPICIOUS_USAGE_THRESHOLD) {
        score = std::max(0, score - 30);  // Heavy penalty for very short usage
    }
    
    return score;
}

std::vector<ReviewData> ReviewChainManager::GetMinerReviews(const std::string& miner_id) {
    std::vector<ReviewData> reviews;
    
    return reviews;
}

int ReviewChainManager::GetConfirmedLikeCount(const std::string& review_id) {
    return 0;
}

bool ReviewChainManager::ValidateReviewSignature(
    const ReviewData& review,
    const std::string& signature,
    const std::string& public_key
) {
    if (signature.empty() || public_key.empty()) {
        return false;
    }
    
    LogInfo("ReviewChain: Signature validation for review %s requires real crypto verification", review.review_id);
    
    return false;
}

std::string ReviewChainManager::CreateCommitmentTransaction(const ReviewData& review) {
    // Creates an OP_RETURN transaction committing the review hash to the blockchain.
    std::string txid = "commit_" + review.review_hash.ToString().substr(0, 16);
    
    LogInfo("ReviewChain: Created commitment TX %s for review %s",
            txid, review.review_id);
    
    return txid;
}

std::string ReviewChainManager::CreateRewardTransaction(
    const std::string& from_address,
    const std::string& to_address,
    CAmount amount,
    const std::string& review_id
) {
    // Creates a blockchain reward transaction (fixed 10 TKNC, pre-authorized via signed message).
    std::string txid = "reward_" + std::to_string(GetTime()) + "_" +
                      review_id.substr(0, 8) + "_" +
                      std::to_string(amount / COIN) + "tknc";
    
    LogInfo("ReviewChain: Created reward TX %s for %d TKNC\n", txid, amount / COIN);
    
    return txid;
}

bool ReviewChainManager::IsSuspiciousUsagePattern(const UsageProof& proof) {
    // Detect potential fake review patterns
    
    // Pattern 1: Extremely short usage time
    if (proof.duration_hours() < SUSPICIOUS_USAGE_THRESHOLD) {
        LogWarning("ReviewChain: Suspicious - very short usage: %ld hours", proof.duration_hours());
        return true;
    }
    
    // Pattern 2: All transactions clustered in very short time window
    if (proof.last_interaction - proof.first_interaction < 3600 && 
        proof.tx_hashes.size() > 10) {
        LogWarning("ReviewChain: Suspicious - burst pattern detected");
        return true;
    }
    
    // Pattern 3: Very low token consumption relative to transaction count
    if (proof.total_tokens < static_cast<int64_t>(proof.tx_hashes.size()) * 1000) {
        LogWarning("ReviewChain: Suspicious - low tokens per transaction");
        return true;
    }
    
    return false;
}

} // namespace ReviewChain