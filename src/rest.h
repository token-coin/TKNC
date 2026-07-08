// Copyright (c) 2015-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TKN_REST_H
#define TKN_REST_H

#include <string>

enum class RESTResponseFormat {
    UNDEF,
    BINARY,
    HEX,
    JSON,
};

// Parse a URI to get the data format and URI without data format string and query string. param[out]=strReq without format/query; strReq[in]=URI to parse; returns parsed RESTResponseFormat.
RESTResponseFormat ParseDataFormat(std::string& param, const std::string& strReq);

#endif // TKN_REST_H
