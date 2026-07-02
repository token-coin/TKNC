#ifndef TKN_ECONOMICS_DYNAMIC_PRICING_H
#define TKN_ECONOMICS_DYNAMIC_PRICING_H

#include <consensus/amount.h>
#include <cmath>
#include <cstdint>
#include <string>

static const double INFERENCE_PREMIUM_MULTIPLIER = 2.0;
static const int BLOCKS_PER_DAY = 144;
static const int TOKENS_PER_1M = 1000000;

struct PricingParams {
    double current_difficulty;
    int active_miners;
    CAmount block_reward;
    int64_t network_hashrate;
    
    PricingParams() : current_difficulty(10.0), active_miners(100), 
                      block_reward(50 * COIN), network_hashrate(0) {}
};

struct InferencePricing {
    CAmount minimum_price_per_1m;
    CAmount suggested_price_per_1m;
    CAmount market_average_price;
    double premium_multiplier;
    CAmount estimated_daily_pow_revenue;
    CAmount estimated_daily_inference_revenue;
    
    InferencePricing() : minimum_price_per_1m(0), suggested_price_per_1m(0),
                         market_average_price(0), premium_multiplier(2.0),
                         estimated_daily_pow_revenue(0), estimated_daily_inference_revenue(0) {}
};

inline CAmount CalculateMinerDailyPoWRevenue(const PricingParams& params) {
    if (params.active_miners == 0 || params.current_difficulty <= 0) {
        return 0;
    }

    double total_daily_pow_output = static_cast<double>(params.block_reward) * BLOCKS_PER_DAY;
    double per_miner_revenue = total_daily_pow_output / params.active_miners;

    double difficulty_adjustment = 10.0 / params.current_difficulty;
    difficulty_adjustment = std::min(difficulty_adjustment, 1000.0);

    per_miner_revenue = per_miner_revenue * difficulty_adjustment;

    if (per_miner_revenue < 0 || per_miner_revenue > static_cast<double>(MAX_MONEY)) {
        return 0;
    }

    return static_cast<CAmount>(per_miner_revenue);
}

inline InferencePricing CalculateInferencePricing(const PricingParams& params) {
    InferencePricing pricing;
    
    pricing.estimated_daily_pow_revenue = CalculateMinerDailyPoWRevenue(params);
    
    CAmount base_minimum = pricing.estimated_daily_pow_revenue * INFERENCE_PREMIUM_MULTIPLIER;
    pricing.minimum_price_per_1m = base_minimum;
    
    double miner_density_factor = 1.0 + (params.active_miners - 50) / 100.0;
    miner_density_factor = std::max(0.5, std::min(3.0, miner_density_factor));
    
    double demand_factor = 1.0 + (params.current_difficulty - 8.0) / 10.0;
    demand_factor = std::max(0.8, std::min(2.5, demand_factor));
    
    pricing.suggested_price_per_1m = static_cast<CAmount>(
        pricing.minimum_price_per_1m * miner_density_factor * demand_factor
    );
    
    pricing.market_average_price = static_cast<CAmount>(
        pricing.suggested_price_per_1m * (0.9 + (rand() % 20) / 100.0)
    );
    
    pricing.premium_multiplier = INFERENCE_PREMIUM_MULTIPLIER;
    
    int daily_inference_requests = 500 + (rand() % 2000);
    CAmount daily_inference_revenue = daily_inference_requests * 
                                     (pricing.market_average_price / TOKENS_PER_1M);
    pricing.estimated_daily_inference_revenue = daily_inference_revenue;
    
    return pricing;
}

inline bool IsValidInferencePrice(CAmount price, const PricingParams& params) {
    InferencePricing pricing = CalculateInferencePricing(params);
    return price >= pricing.minimum_price_per_1m;
}

inline std::string FormatPriceTKNC(CAmount price_tknc) {
    double tknc = static_cast<double>(price_tknc) / COIN;
    
    if (tknc >= 1000000) {
        return std::to_string(tknc / 1000000).substr(0, 4) + "M TKNC";
    } else if (tknc >= 1000) {
        return std::to_string(static_cast<int>(tknc / 1000)) + "K TKNC";
    } else {
        return std::to_string(static_cast<int>(tknc)) + " TKNC";
    }
}

inline std::string GetEconomicModelExplanation() {
    return 
        "=== TKNC Economic Model ===\n"
        "\nCore Principle: Inference Revenue > Mining Revenue\n"
        "\nMechanism:\n"
        "1. Mining is Red Ocean: Anyone can mine, intense competition, diminishing returns\n"
        "2. Inference is Blue Ocean: Only some can provide, high demand, high margins\n"
        "3. No Verification Needed: Market automatically filters quality\n"
        "\nPricing Formula:\n"
        "- Minimum Inference Price = Daily PoW Revenue × 2\n"
        "- Adjusted by: Miner density × Network difficulty\n"
        "- Result: Inference always more profitable than mining\n"
        "\nIncentive Structure:\n"
        "- Large model miners: Inference revenue >> Mining revenue → Scale up models\n"
        "- Small model miners: Inference revenue > Mining revenue → Willing to host models\n"
        "- No-model miners: Only mining → Lowest income → Forced to deploy models\n"
        "\nMarket Self-Regulation:\n"
        "- Users pay for quality (willing to pay more for better answers)\n"
        "- Market pricing, not system pricing\n"
        "- Miners optimize models to attract more users\n"
        "\nZero Supervision Cost:\n"
        "- Based on public mathematics (blockchain transparent)\n"
        "- Self-running system (no human intervention needed)\n"
        "- Innovation driven by competition (quality improvement)";
}

#endif // TKN_ECONOMICS_DYNAMIC_PRICING_H
