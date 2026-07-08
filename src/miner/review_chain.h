#ifndef TKN_REVIEW_CHAIN_H
#define TKN_REVIEW_CHAIN_H

#include <consensus/amount.h>
#include <serialize.h>
#include <uint256.h>
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <map>

/**
 * TKNC On-Chain Review System
 * 
 * Security Design Principles:
 * 1. No private keys, mnemonics, or passwords stored or transmitted
 * 2. Identity verification via public key signatures only
 * 3. All data integrity verified through blockchain hashes
 * 4. Usage proof based on transaction history (immutable)
 * 
 * Anti-Fraud Mechanism:
 * - Reviews require on-chain usage proof (transaction hash)
 * - Like rewards create real blockchain transactions
 * - Short usage patterns automatically flagged as suspicious
 */

namespace ReviewChain {

// Review status enumeration
enum class ReviewStatus {
    PENDING_VERIFICATION = 0,
    VERIFIED_ON_CHAIN = 1,
    FLAGGED_SUSPICIOUS = 2,
    REJECTED_INVALID = 3
};

// Credibility levels for anti-fraud
enum class CredibilityLevel {
    TRUSTED = 80,        // Long-term user with high token consumption
    RELIABLE = 60,       // Regular user with moderate usage
    MODERATE = 40,       // New user with some usage history
    SUSPICIOUS = 20,     // Very short usage time, possible fake
    LIKELY_FAKE = 0      // No verifiable usage or obvious fraud
};

// Review data structure (stored off-chain, hash committed on-chain)
struct ReviewData {
    std::string review_id;              // Unique identifier
    std::string miner_id;               // Miner being reviewed
    std::string reviewer_pubkey_hash;   // Anonymized reviewer identifier (hash of pubkey)
    
    std::string comment;                // Review text content
    int rating;                         // 1-5 star rating
    
    // Usage verification data (from actual transactions)
    std::string usage_tx_hash;          // Transaction proving usage
    int64_t usage_duration_hours;       // How long user has used this miner
    int64_t tokens_consumed;            // Total tokens used (in millions)
    CAmount total_spent;                // Total TKNC spent on this miner
    
    // Timestamps
    int64_t created_at;                 // Review creation timestamp
    int64_t last_updated;               // Last update timestamp
    
    // On-chain commitment
    uint256 review_hash;                // SHA256 of all review data
    std::string commitment_tx_hash;     // TXID of OP_RETURN with review hash
    
    // Status and credibility
    ReviewStatus status;
    CredibilityLevel credibility;
    int credibility_score;              // 0-100
    
    // Social engagement
    int like_count;
    std::map<std::string, bool> likes_by_user;  // user_pubkey_hash -> liked
    
    // Serialization
    SERIALIZE_METHODS(ReviewData, obj) {
        READWRITE(obj.review_id);
        READWRITE(obj.miner_id);
        READWRITE(obj.reviewer_pubkey_hash);
        READWRITE(obj.comment);
        READWRITE(obj.rating);
        READWRITE(obj.usage_tx_hash);
        READWRITE(obj.usage_duration_hours);
        READWRITE(obj.tokens_consumed);
        READWRITE(obj.total_spent);
        READWRITE(obj.created_at);
        READWRITE(obj.last_updated);
        READWRITE(obj.review_hash);
        READWRITE(obj.commitment_tx_hash);
        READWRITE(obj.status);
        READWRITE(obj.credibility);
        READWRITE(obj.credibility_score);
        READWRITE(obj.like_count);
        READWRITE(obj.likes_by_user);
    }
};

// Like transaction record (on-chain)
struct LikeTransaction {
    std::string like_id;
    std::string review_id;
    std::string liker_pubkey_hash;
    std::string miner_id;
    
    CAmount reward_amount;              // 10 TKNC transferred
    std::string from_address;           // Miner's address
    std::string to_address;             // Reviewer's address
    
    std::string tx_hash;                // Actual blockchain transaction ID
    int64_t block_height;               // Block where tx was included
    int confirmations;                  // Number of confirmations
    
    int64_t timestamp;
    bool is_liked;                      // true = like, false = unlike
    
    SERIALIZE_METHODS(LikeTransaction, obj) {
        READWRITE(obj.like_id);
        READWRITE(obj.review_id);
        READWRITE(obj.liker_pubkey_hash);
        READWRITE(obj.miner_id);
        READWRITE(obj.reward_amount);
        READWRITE(obj.from_address);
        READWRITE(obj.to_address);
        READWRITE(obj.tx_hash);
        READWRITE(obj.block_height);
        READWRITE(obj.confirmations);
        READWRITE(obj.timestamp);
        READWRITE(obj.is_liked);
    }
};

// Usage proof structure (derived from actual inference transactions)
struct UsageProof {
    std::string user_pubkey_hash;
    std::string miner_id;
    
    std::vector<std::string> tx_hashes;     // All transaction IDs for this user-miner pair
    CAmount total_paid;                     // Total TKNC paid to miner
    int64_t total_tokens;                   // Total tokens processed
    int64_t first_interaction;              // Timestamp of first transaction
    int64_t last_interaction;               // Timestamp of most recent transaction
    
    bool verified_on_chain;                 // Whether blockchain confirms these transactions
    int min_confirmations;                  // Minimum confirmations across all txs
    
    // Calculated metrics
    int64_t duration_hours() const {
        if (last_interaction <= first_interaction) return 0;
        return (last_interaction - first_interaction) / 3600;
    }
    
    bool HasMinimumUsage() const {
        // Require at least 24 hours of usage history
        return duration_hours() >= 24 && total_tokens > 0;
    }
    
    SERIALIZE_METHODS(UsageProof, obj) {
        READWRITE(obj.user_pubkey_hash);
        READWRITE(obj.miner_id);
        READWRITE(obj.tx_hashes);
        READWRITE(obj.total_paid);
        READWRITE(obj.total_tokens);
        READWRITE(obj.first_interaction);
        READWRITE(obj.last_interaction);
        READWRITE(obj.verified_on_chain);
        READWRITE(obj.min_confirmations);
    }
};

/**
 * Review Chain Manager
 * Handles on-chain storage and verification of reviews and likes
 */
class ReviewChainManager {
public:
    /**
     * Create a new review with on-chain commitment
     * 
     * @param reviewer_key_hash Anonymized public key hash (no private key needed)
     * @param miner_id Target miner
     * @param comment Review content
     * @param rating 1-5 stars
     * @param usage_proof Verified usage from blockchain transactions
     * @return ReviewData if successful, nullopt if validation fails
     */
    static std::optional<ReviewData> CreateReview(
        const std::string& reviewer_key_hash,
        const std::string& miner_id,
        const std::string& comment,
        int rating,
        const UsageProof& usage_proof
    );
    
    /**
     * Toggle like on a review (creates blockchain transaction)
     * 
     * @param liker_key_hash Anonymized public key hash
     * @param review_id Target review
     * @param miner_id Miner being reviewed (pays reward)
     * @return LikeTransaction if successful
     * 
     * Security: This creates a REAL blockchain transaction transferring 10 TKNC
     * from miner to liker. Requires miner's signed authorization but NEVER
     * exposes private keys. Uses secure signing ceremony.
     */
    static std::optional<LikeTransaction> ToggleLike(
        const std::string& liker_key_hash,
        const std::string& review_id,
        const std::string& miner_id
    );
    
    /**
     * Verify user has actually used a specific miner
     * Scans blockchain for transactions between user and miner
     * 
     * @param user_key_hash User's public key hash
     * @param miner_id Target miner
     * @return UsageProof with transaction history if found
     */
    static std::optional<UsageProof> VerifyUsageOnChain(
        const std::string& user_key_hash,
        const std::string& miner_id
    );
    
    /**
     * Calculate credibility score for a review
     * Multi-factor analysis to detect fake reviews
     * 
     * Factors considered:
     * - Usage duration (longer = more credible)
     * - Token consumption (more = more credible)
     * - Rating consistency vs community average
     * - Account age and history
     * 
     * @param review The review to evaluate
     * @return Credibility score 0-100
     */
    static int CalculateCredibilityScore(const ReviewData& review);
    
    /**
     * Get all reviews for a miner
     * 
     * @param miner_id Target miner
     * @return Vector of reviews sorted by credibility
     */
    static std::vector<ReviewData> GetMinerReviews(const std::string& miner_id);
    
    /**
     * Get like count for a review
     * 
     * @param review_id Target review
     * @return Number of likes (confirmed on-chain)
     */
    static int GetConfirmedLikeCount(const std::string& review_id);
    
    /**
     * Validate review signature
     * Confirms review was created by the claimed user
     * WITHOUT requiring private key exposure
     * 
     * @param review The review to validate
     * @param signature DER-encoded signature
     * @param public_key User's public key
     * @return true if signature is valid
     */
    static bool ValidateReviewSignature(
        const ReviewData& review,
        const std::string& signature,
        const std::string& public_key
    );

private:
    /**
     * Generate commitment transaction (OP_RETURN with review hash)
     * This commits the review data to the blockchain immutably
     */
    static std::string CreateCommitmentTransaction(const ReviewData& review);
    
    /**
     * Generate reward transfer transaction for likes
     * Transfers 10 TKNC from miner to reviewer
     */
    static std::string CreateRewardTransaction(
        const std::string& from_address,
        const std::string& to_address,
        CAmount amount,
        const std::string& review_id
    );
    
    /**
     * Hash review data for on-chain commitment
     */
    static uint256 HashReviewData(const ReviewData& review);
    
    /**
     * Check if usage pattern indicates potential fraud
     */
    static bool IsSuspiciousUsagePattern(const UsageProof& proof);
};

// Constants
constexpr CAmount LIKE_REWARD_AMOUNT = 10 * COIN;  // 10 TKNC per like
constexpr int MINIMUM_USAGE_HOURS = 24;             // Minimum hours before reviewing
constexpr int MINIMUM_TOKEN_CONSUMPTION = 1000000;  // 1M minimum tokens
constexpr int SUSPICIOUS_USAGE_THRESHOLD = 10;      // Hours below this = suspicious

} // namespace ReviewChain

#endif // TKN_REVIEW_CHAIN_H