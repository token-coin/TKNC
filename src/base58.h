// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Why base-58 instead of standard base-64 encoding? We avoid 0OIl characters that look the same in some fonts and could create visually identical data. A string with non-alphanumeric characters is not as easily accepted as input, email won't line-break without punctuation, and double-clicking selects the whole string as one word if it's all alphanumeric.
#ifndef TKN_BASE58_H
#define TKN_BASE58_H

#include <span.h>

#include <string>
#include <vector>

/** Encode a byte span as a base58-encoded string. */
std::string EncodeBase58(std::span<const unsigned char> input);

/** Decode a base58-encoded string (str) into a byte vector (vchRet). Return true if decoding is successful. */
[[nodiscard]] bool DecodeBase58(const std::string& str, std::vector<unsigned char>& vchRet, int max_ret_len);

/** Encode a byte span into a base58-encoded string, including checksum. */
std::string EncodeBase58Check(std::span<const unsigned char> input);

/** Decode a base58-encoded string (str) that includes a checksum into a byte vector (vchRet). Return true if decoding is successful. */
[[nodiscard]] bool DecodeBase58Check(const std::string& str, std::vector<unsigned char>& vchRet, int max_ret_len);

#endif // TKN_BASE58_H
