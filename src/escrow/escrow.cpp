#include <escrow/escrow.h>
#include <hash.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <random.h>
#include <sstream>
#include <iomanip>

// Helper function to escape JSON strings (prevents injection)
static std::string JsonEscape(const std::string& s) {
    std::ostringstream oss;
    oss << '"';
    for (char c : s) {
        switch (c) {
            case '"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b"; break;
            case '\f': oss << "\\f"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if (c < 0x20) {
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)(unsigned char)c;
                } else {
                    oss << c;
                }
                break;
        }
    }
    oss << '"';
    return oss.str();
}

// PricingSnapshot implementation
std::string PricingSnapshot::ToJSON() const
{
 std::ostringstream oss;
 oss << "{"
 << "\"rate_tknc_per_token\":" << rate_tknc_per_token << ","
 << "\"rate_tokens_per_tknc\":" << rate_tokens_per_tknc << ","
 << "\"quota_tokens\":" << quota_tokens << ","
 << "\"total_tknc_paid\":" << total_tknc_paid << ","
 << "\"miner_wallet\":" << JsonEscape(miner_wallet) << ","
 << "\"user_wallet\":" << JsonEscape(user_wallet) << ","
 << "\"model_name\":" << JsonEscape(model_name) << ","
 << "\"model_hash\":" << JsonEscape(model_hash) << ","
 << "\"timestamp\":" << timestamp << ","
 << "\"version\":" << version
 << "}";
 return oss.str();
}

std::string PricingSnapshot::ComputeHash() const
{
 std::string json = ToJSON();
 std::vector<uint8_t> data(json.begin(), json.end());

 // Double SHA256
 uint256 hash1 = Hash(data);
 uint256 hash2 = Hash(hash1);

 return hash2.GetHex();
}

bool PricingSnapshot::Validate() const
{
 if (rate_tokens_per_tknc <= 0) return false;
 if (rate_tknc_per_token <= 0) return false;
 if (quota_tokens <= 0) return false;
 if (total_tknc_paid <= 0) return false;
 if (miner_wallet.empty()) return false;
 if (user_wallet.empty()) return false;
 if (model_name.empty()) return false;
 if (timestamp <= 0) return false;

 // Allow small payments (less than 1 TKNC)
 // For small amounts, quota_tokens = (amount * rate) / COIN

 // (Chinese comment removed)
 // Check for overflow: if rate_tokens_per_tknc > INT64_MAX / rate_tknc_per_token, overflow
 if (rate_tknc_per_token > 0 && rate_tokens_per_tknc > INT64_MAX / rate_tknc_per_token) {
 return false; // Would overflow
 }
 int64_t check = rate_tokens_per_tknc * rate_tknc_per_token;
 if (check <= 0) return false;

 // Verify quota matches payment: quota_tokens = total_tknc_paid * rate_tokens_per_tknc / COIN
 int64_t expected_quota = (total_tknc_paid * rate_tokens_per_tknc) / COIN;
 if (quota_tokens != expected_quota) return false;

 return true;
}

// SpendingLimit implementation
bool SpendingLimit::IsExpired() const
{
 if (expires_at <= 0) return false;
 return GetTime() > expires_at;
}

// Utility functions
std::string GenerateSpendingLimitID(const std::string& source_id)
{
 // Derive spending limit ID from source identifier + random suffix
 std::vector<uint8_t> random_bytes(8);
 GetRandBytes(random_bytes);
 std::string suffix = HexStr(random_bytes);

 // Take first 16 chars of source + suffix for uniqueness
 std::string base = source_id;
 if (base.length() > 16) {
 base = base.substr(base.length() - 16);
 }

 return "escrow_" + base + "_" + suffix.substr(0, 8);
}

const char* SpendingLimitStateToString(SpendingLimit::State state)
{
 switch (state) {
 case SpendingLimit::State::CREATED: return "CREATED";
 case SpendingLimit::State::ACTIVE: return "ACTIVE";
 case SpendingLimit::State::SUSPENDED: return "SUSPENDED";
 case SpendingLimit::State::EXHAUSTED: return "EXHAUSTED";
 case SpendingLimit::State::CLOSED: return "CLOSED";
 case SpendingLimit::State::PAYMENT_PENDING: return "PAYMENT_PENDING";
 default: return "UNKNOWN";
 }
}