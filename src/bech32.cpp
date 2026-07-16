// Copyright (c) 2017, 2021 Pieter Wuille
// Copyright (c) 2021-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bech32.h>
#include <util/vector.h>

#include <array>
#include <cassert>
#include <numeric>
#include <optional>

namespace bech32
{

namespace
{

typedef std::vector<uint8_t> data;

/** The Bech32 and Bech32m character set for encoding. */
const char* CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

/** The Bech32 and Bech32m character set for decoding. */
const int8_t CHARSET_REV[128] = {
 -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
 -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
 -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
 15, -1, 10, 17, 21, 20, 26, 30, 7, 5, -1, -1, -1, -1, -1, -1,
 -1, 29, -1, 24, 13, 25, 9, 8, 23, -1, 18, 22, 31, 27, 19, -1,
 1, 0, 3, 16, 11, 28, 12, 14, 6, 4, 2, -1, -1, -1, -1, -1,
 -1, 29, -1, 24, 13, 25, 9, 8, 23, -1, 18, 22, 31, 27, 19, -1,
 1, 0, 3, 16, 11, 28, 12, 14, 6, 4, 2, -1, -1, -1, -1, -1
};

/** GF(1024) = GF(32)[x]/(x^2 + 9x + 23); (e) is a primitive element. GF1024_EXP[k]=(e)^k, GF1024_LOG is its inverse. */
constexpr std::pair<std::array<int16_t, 1023>, std::array<int16_t, 1024>> GenerateGFTables()
{
 // GF(32) tables, used to construct GF(1024) tables below.
 std::array<int8_t, 31> GF32_EXP{};
 std::array<int8_t, 32> GF32_LOG{};

 // fmod = 41 (binary 101001) encodes GF(32) defining polynomial x^5 + x^3 + 1 over GF(2).
 const int fmod = 41;

 // GF(32) element = 5-bit vector (b_4..b_0) encoding polynomial b_4*x^4 + ... + b_0 mod fmod.
 GF32_EXP[0] = 1;
 GF32_LOG[0] = -1;
 GF32_LOG[1] = 0;
 int v = 1;
 for (int i = 1; i < 31; ++i) {
 // Multiply by x = left shift by 1.
 v = v << 1;
 // Reduce modulo fmod via XOR if x^5 term appears (GF(2) subtraction = XOR).
 if (v & 32) v ^= fmod;
 GF32_EXP[i] = v;
 GF32_LOG[v] = i;
 }

 // Build table for GF(1024)
 std::array<int16_t, 1023> GF1024_EXP{};
 std::array<int16_t, 1024> GF1024_LOG{};

 GF1024_EXP[0] = 1;
 GF1024_LOG[0] = -1;
 GF1024_LOG[1] = 0;

 // GF(1024) element v = v1||v0 (two GF(32) halves). (e)*v computed as v0'=23*v1, v1'=9*v1+v0.
 // GF(32) multiplication uses log/exp: a*b = EXP[LOG[a]+LOG[b]] for non-zero a,b.

 v = 1;
 for (int i = 1; i < 1023; ++i) {
 int v0 = v & 31;
 int v1 = v >> 5;

 int v0n = v1 ? GF32_EXP.at((GF32_LOG.at(v1) + GF32_LOG.at(23)) % 31) : 0;
 int v1n = (v1 ? GF32_EXP.at((GF32_LOG.at(v1) + GF32_LOG.at(9)) % 31) : 0) ^ v0;

 v = v1n << 5 | v0n;
 GF1024_EXP[i] = v;
 GF1024_LOG[v] = i;
 }

 return std::make_pair(GF1024_EXP, GF1024_LOG);
}

constexpr auto tables = GenerateGFTables();
constexpr const std::array<int16_t, 1023>& GF1024_EXP = tables.first;
constexpr const std::array<int16_t, 1024>& GF1024_LOG = tables.second;

/* Determine the final constant to use for the specified encoding. */
uint32_t EncodingConstant(Encoding encoding) {
 assert(encoding == Encoding::BECH32 || encoding == Encoding::BECH32M);
 return encoding == Encoding::BECH32 ? 1 : 0x2bc830a3;
}

/** Compute 6 5-bit values to XOR into last 6 inputs to make checksum 0. Packed in 30-bit int,
 * higher bits = earlier values. */
uint32_t PolyMod(const data& v)
{
 // Input = GF(32) polynomial v(x) coeffs (implicit leading 1); output = 30-bit int of v(x) mod g(x).
 // (Chinese comment removed)

 // `c` holds bitpacked coefficients of (processed input) mod g(x); starts at 1 (since 1 mod g(x)=1).

 uint32_t c = 1;
 for (const auto v_i : v) {
 // Update c to c'(x) = (c(x)*x + v_i) mod g(x). With k(x) = x^6 mod g(x), this becomes
 // c'(x) = (lower 5 coeffs * x + v_i) + c0*k(x), where c0 = top 5-bit group of c.

 // First, determine the value of c0:
 uint8_t c0 = c >> 25;

 // Then compute c1*x^5 + c2*x^4 + c3*x^3 + c4*x^2 + c5*x + v_i:
 c = ((c & 0x1ffffff) << 5) ^ v_i;

 // For each set bit n in c0, conditionally XOR {2^n}*k(x):
 if (c0 & 1) c ^= 0x3b6a57b2; // k(x) = {29}x^5 + {22}x^4 + {20}x^3 + {21}x^2 + {29}x + {18}
 if (c0 & 2) c ^= 0x26508e6d; // {2}k(x) = {19}x^5 + {5}x^4 + x^3 + {3}x^2 + {19}x + {13}
 if (c0 & 4) c ^= 0x1ea119fa; // {4}k(x) = {15}x^5 + {10}x^4 + {2}x^3 + {6}x^2 + {15}x + {26}
 if (c0 & 8) c ^= 0x3d4233dd; // {8}k(x) = {30}x^5 + {20}x^4 + {4}x^3 + {12}x^2 + {30}x + {29}
 if (c0 & 16) c ^= 0x2a1462b3; // {16}k(x) = {21}x^5 + x^4 + {8}x^3 + {24}x^2 + {21}x + {19}

 }
 return c;
}

/** Syndrome computes s_j = R(e^j) for j in [997,998,999], packed in 30-bit int (10 bits each).
 * R((e)^j) = E((e)^j) since C(x) is a valid codeword; each coeff bit contributes a precomputed constant. */
constexpr std::array<uint32_t, 25> GenerateSyndromeConstants() {
 std::array<uint32_t, 25> SYNDROME_CONSTS{};
 for (int k = 1; k < 6; ++k) {
 for (int shift = 0; shift < 5; ++shift) {
 int16_t b = GF1024_LOG.at(size_t{1} << shift);
 int16_t c0 = GF1024_EXP.at((997*k + b) % 1023);
 int16_t c1 = GF1024_EXP.at((998*k + b) % 1023);
 int16_t c2 = GF1024_EXP.at((999*k + b) % 1023);
 uint32_t c = c2 << 20 | c1 << 10 | c0;
 int ind = 5*(k-1) + shift;
 SYNDROME_CONSTS[ind] = c;
 }
 }
 return SYNDROME_CONSTS;
}
constexpr std::array<uint32_t, 25> SYNDROME_CONSTS = GenerateSyndromeConstants();

/** Syndrome returns s_997, s_998, s_999 packed in 30-bit int (10 bits each, as described above). */
uint32_t Syndrome(const uint32_t residue) {
 // low = first 5 bits = r6 (constant term of residue polynomial).
 uint32_t low = residue & 0x1f;

 // s_j starts at r6 (unconditional); XOR = addition in GF(2^k).
 uint32_t result = low ^ (low << 10) ^ (low << 20);

 // For each subsequent bit, XOR the precomputed constant (a^999||a^998||a^997 packed) if bit is set.
 for (int i = 0; i < 25; ++i) {
 result ^= ((residue >> (5+i)) & 1 ? SYNDROME_CONSTS.at(i) : 0);
 }
 return result;
}

/** Convert to lower case. */
inline unsigned char LowerCase(unsigned char c)
{
 return (c >= 'A' && c <= 'Z') ? (c - 'A') + 'a' : c;
}

/** Return indices of invalid characters in a Bech32 string. */
bool CheckCharacters(const std::string& str, std::vector<int>& errors)
{
 bool lower = false, upper = false;
 for (size_t i = 0; i < str.size(); ++i) {
 unsigned char c{(unsigned char)(str[i])};
 if (c >= 'a' && c <= 'z') {
 if (upper) {
 errors.push_back(i);
 } else {
 lower = true;
 }
 } else if (c >= 'A' && c <= 'Z') {
 if (lower) {
 errors.push_back(i);
 } else {
 upper = true;
 }
 } else if (c < 33 || c > 126) {
 errors.push_back(i);
 }
 }
 return errors.empty();
}

std::vector<unsigned char> PreparePolynomialCoefficients(const std::string& hrp, const data& values)
{
 data ret;
 ret.reserve(hrp.size() + 1 + hrp.size() + values.size() + CHECKSUM_SIZE);

 /** Expand a HRP for use in checksum computation. */
 for (size_t i = 0; i < hrp.size(); ++i) ret.push_back(hrp[i] >> 5);
 ret.push_back(0);
 for (size_t i = 0; i < hrp.size(); ++i) ret.push_back(hrp[i] & 0x1f);

 ret.insert(ret.end(), values.begin(), values.end());

 return ret;
}

/** Verify a checksum. */
Encoding VerifyChecksum(const std::string& hrp, const data& values)
{
 // Bech32 requires checksum=1 (not 0) so appending 0 can't create a new valid list; Bech32m amended this constant.
 // Ref: https://gist.github.com/sipa/14c248c288c3880a3b191f978a34508e
 auto enc = PreparePolynomialCoefficients(hrp, values);
 const uint32_t check = PolyMod(enc);
 if (check == EncodingConstant(Encoding::BECH32)) return Encoding::BECH32;
 if (check == EncodingConstant(Encoding::BECH32M)) return Encoding::BECH32M;
 return Encoding::INVALID;
}

/** Create a checksum. */
data CreateChecksum(Encoding encoding, const std::string& hrp, const data& values)
{
 auto enc = PreparePolynomialCoefficients(hrp, values);
 enc.insert(enc.end(), CHECKSUM_SIZE, 0x00);
 uint32_t mod = PolyMod(enc) ^ EncodingConstant(encoding); // Determine what to XOR into those 6 zeroes.
 data ret(CHECKSUM_SIZE);
 for (size_t i = 0; i < CHECKSUM_SIZE; ++i) {
 // Convert the 5-bit groups in mod to checksum values.
 ret[i] = (mod >> (5 * (5 - i))) & 31;
 }
 return ret;
}

} // namespace

/** Encode a Bech32 or Bech32m string. */
std::string Encode(Encoding encoding, const std::string& hrp, const data& values) {
 // HRP must be lowercase: BIP-173/BIP350 require lowercase output, uppercase HRP is always invalid.
 for (const char& c : hrp) assert(c < 'A' || c > 'Z');

 std::string ret;
 ret.reserve(hrp.size() + 1 + values.size() + CHECKSUM_SIZE);
 ret += hrp;
 ret += SEPARATOR;
 for (const uint8_t& i : values) ret += CHARSET[i];
 for (const uint8_t& i : CreateChecksum(encoding, hrp, values)) ret += CHARSET[i];
 return ret;
}

/** Decode a Bech32 or Bech32m string. */
DecodeResult Decode(const std::string& str, CharLimit limit) {
 std::vector<int> errors;
 if (!CheckCharacters(str, errors)) return {};
 size_t pos = str.rfind(SEPARATOR);
 if (str.size() > limit) return {};
 if (pos == str.npos || pos == 0 || pos + CHECKSUM_SIZE >= str.size()) {
 return {};
 }
 data values(str.size() - 1 - pos);
 for (size_t i = 0; i < str.size() - 1 - pos; ++i) {
 unsigned char c = str[i + pos + 1];
 int8_t rev = CHARSET_REV[c];

 if (rev == -1) {
 return {};
 }
 values[i] = rev;
 }
 std::string hrp;
 hrp.reserve(pos);
 for (size_t i = 0; i < pos; ++i) {
 hrp += LowerCase(str[i]);
 }
 Encoding result = VerifyChecksum(hrp, values);
 if (result == Encoding::INVALID) return {};
 return {result, std::move(hrp), data(values.begin(), values.end() - CHECKSUM_SIZE)};
}

/** Find index of an incorrect character in a Bech32 string. */
std::pair<std::string, std::vector<int>> LocateErrors(const std::string& str, CharLimit limit) {
 std::vector<int> error_locations{};

 if (str.size() > limit) {
 error_locations.resize(str.size() - limit);
 std::iota(error_locations.begin(), error_locations.end(), static_cast<int>(limit));
 return std::make_pair("Bech32 string too long", std::move(error_locations));
 }

 if (!CheckCharacters(str, error_locations)){
 return std::make_pair("Invalid character or mixed case", std::move(error_locations));
 }

 size_t pos = str.rfind(SEPARATOR);
 if (pos == str.npos) {
 return std::make_pair("Missing separator", std::vector<int>{});
 }
 if (pos == 0 || pos + CHECKSUM_SIZE >= str.size()) {
 error_locations.push_back(pos);
 return std::make_pair("Invalid separator position", std::move(error_locations));
 }

 std::string hrp;
 hrp.reserve(pos);
 for (size_t i = 0; i < pos; ++i) {
 hrp += LowerCase(str[i]);
 }

 size_t length = str.size() - 1 - pos; // length of data part
 data values(length);
 for (size_t i = pos + 1; i < str.size(); ++i) {
 unsigned char c = str[i];
 int8_t rev = CHARSET_REV[c];
 if (rev == -1) {
 error_locations.push_back(i);
 return std::make_pair("Invalid Base 32 character", std::move(error_locations));
 }
 values[i - pos - 1] = rev;
 }

 // We attempt error detection with both bech32 and bech32m, and choose the one with the fewest errors
 // We can't simply use the segwit version, because that may be one of the errors
 std::optional<Encoding> error_encoding;
 for (Encoding encoding : {Encoding::BECH32, Encoding::BECH32M}) {
 std::vector<int> possible_errors;
 // Recall that (expanded hrp + values) is interpreted as a list of coefficients of a polynomial
 // over GF(32). PolyMod computes the "remainder" of this polynomial modulo the generator G(x).
 auto enc = PreparePolynomialCoefficients(hrp, values);
 uint32_t residue = PolyMod(enc) ^ EncodingConstant(encoding);

 // All valid codewords should be multiples of G(x), so this remainder (after XORing with the encoding
 // constant) should be 0 - hence 0 indicates there are no errors present.
 if (residue != 0) {
 // If errors are present, our polynomial must be of the form C(x) + E(x) where C is the valid
 // codeword (a multiple of G(x)), and E encodes the errors.
 uint32_t syn = Syndrome(residue);

 // Unpack the three 10-bit syndrome values
 int s0 = syn & 0x3FF;
 int s1 = (syn >> 10) & 0x3FF;
 int s2 = syn >> 20;

 // Get the discrete logs of these values in GF1024 for more efficient computation
 int l_s0 = GF1024_LOG.at(s0);
 int l_s1 = GF1024_LOG.at(s1);
 int l_s2 = GF1024_LOG.at(s2);

 // Single error: E(x) = e1*x^p1. Then s1/s0 = s2/s1 = (e)^p1, so check s1^2 == s0*s2:
 if (l_s0 != -1 && l_s1 != -1 && l_s2 != -1 && (2 * l_s1 - l_s2 - l_s0 + 2046) % 1023 == 0) {
 // p1 = l_s1 - l_s0 (mod 1023); e1 = s0/((e)^(997*p1)), using (e)^1023=1 so 1/(e)^997=(e)^(1023-997).
 size_t p1 = (l_s1 - l_s0 + 1023) % 1023; // +1023 ensures positive
 int l_e1 = l_s0 + (1023 - 997) * p1;
 // Sanity: p1 within length, e1 in GF(32) (e1=(e)^(33k); GF(32)* is index-33 subgroup of GF(1024)*).
 if (p1 < length && !(l_e1 % 33)) {
 // (Chinese comment removed)
 possible_errors.push_back(str.size() - p1 - 1);
 }
 // Otherwise, suppose there are two errors. Then E(x) = e1*x^p1 + e2*x^p2.
 } else {
 // For all possible first error positions p1
 for (size_t p1 = 0; p1 < length; ++p1) {
 // Two errors: E(x) = e1*x^p1 + e2*x^p2. Guess p1, solve for p2.
 // In char 2: s2+s1*(e)^p1 = e2*(e)^(998*p2)*((e)^p2+(e)^p1), and s1+s0*(e)^p1 = e2*(e)^(997*p2)*((e)^p2+(e)^p1).
 int s2_s1p1 = s2 ^ (s1 == 0 ? 0 : GF1024_EXP.at((l_s1 + p1) % 1023));
 if (s2_s1p1 == 0) continue;
 int l_s2_s1p1 = GF1024_LOG.at(s2_s1p1);

 int s1_s0p1 = s1 ^ (s0 == 0 ? 0 : GF1024_EXP.at((l_s0 + p1) % 1023));
 if (s1_s0p1 == 0) continue;
 int l_s1_s0p1 = GF1024_LOG.at(s1_s0p1);

 // (e)^p2 = (s2 + s1^p1)/(s1 + s0^p1); p2 = log of that.
 size_t p2 = (l_s2_s1p1 - l_s1_s0p1 + 1023) % 1023;

 // Sanity: p2 valid and distinct from p1.
 if (p2 >= length || p1 == p2) continue;

 // Recover e1/e2 via s1+s0*(e)^p2 = e1*(e)^(997*p1)*((e)^p1+(e)^p2).
 int s1_s0p2 = s1 ^ (s0 == 0 ? 0 : GF1024_EXP.at((l_s0 + p2) % 1023));
 if (s1_s0p2 == 0) continue;
 int l_s1_s0p2 = GF1024_LOG.at(s1_s0p2);

 int inv_p1_p2 = 1023 - GF1024_LOG.at(GF1024_EXP.at(p1) ^ GF1024_EXP.at(p2));

 // e2 = (s1+s0*(e)^p1)/((e)^(997*p2)*((e)^p1+(e)^p2)); e1 analogous with p1.
 int l_e2 = l_s1_s0p1 + inv_p1_p2 + (1023 - 997) * p2;
 if (l_e2 % 33) continue; // e2 must be in GF(32).

 int l_e1 = l_s1_s0p2 + inv_p1_p2 + (1023 - 997) * p1;
 if (l_e1 % 33) continue; // e1 must be in GF(32).

 // Again, we do not return e1 or e2 for safety.
 // Order the error positions from the left of the string and return them
 if (p1 > p2) {
 possible_errors.push_back(str.size() - p1 - 1);
 possible_errors.push_back(str.size() - p2 - 1);
 } else {
 possible_errors.push_back(str.size() - p2 - 1);
 possible_errors.push_back(str.size() - p1 - 1);
 }
 break;
 }
 }
 } else {
 // No errors
 return std::make_pair("", std::vector<int>{});
 }

 if (error_locations.empty() || (!possible_errors.empty() && possible_errors.size() < error_locations.size())) {
 error_locations = std::move(possible_errors);
 if (!error_locations.empty()) error_encoding = encoding;
 }
 }
 std::string error_message = error_encoding == Encoding::BECH32M ? "Invalid Bech32m checksum"
 : error_encoding == Encoding::BECH32 ? "Invalid Bech32 checksum"
 : "Invalid checksum";

 return std::make_pair(error_message, std::move(error_locations));
}

} // namespace bech32
