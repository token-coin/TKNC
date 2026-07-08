// Copyright (c) The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef TKN_COMMON_LICENSE_INFO_H
#define TKN_COMMON_LICENSE_INFO_H

#include <string>

std::string CopyrightHolders(const std::string& strPrefix);

/** Returns licensing information (for -version) */
std::string LicenseInfo();

#endif // TKN_COMMON_LICENSE_INFO_H
