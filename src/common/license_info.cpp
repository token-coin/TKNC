// Copyright (c) The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <tknc-build-config.h> // IWYU pragma: keep

#include <common/license_info.h>

#include <tinyformat.h>
#include <util/translation.h>

#include <string>

std::string CopyrightHolders(const std::string& strPrefix)
{
    const auto copyright_devs = strprintf(_(COPYRIGHT_HOLDERS), COPYRIGHT_HOLDERS_SUBSTITUTION).translated;
    std::string strCopyrightHolders = strPrefix + copyright_devs;

    if (copyright_devs.find(CLIENT_NAME) == std::string::npos) {
        strCopyrightHolders += "\n" + strPrefix + "The " CLIENT_NAME " developers";
    }
    return strCopyrightHolders;
}

std::string LicenseInfo()
{
    const std::string URL_SOURCE_CODE = "<https://github.com/tknc/tknc>";

    return CopyrightHolders(strprintf(_("Copyright (C) %i-%i"), 2009, COPYRIGHT_YEAR).translated + " ") + "\n" +
           "\n" +
           strprintf(_("Please contribute if you find %s useful. "
                       "Visit %s for further information about the software."),
                     CLIENT_NAME, "<" CLIENT_URL ">")
               .translated +
           "\n" +
           strprintf(_("The source code is available from %s."), URL_SOURCE_CODE).translated +
           "\n" +
           "\n" +
           _("This is experimental software.") + "\n" +
           strprintf(_("Distributed under the MIT software license, see the accompanying file %s or %s"), "COPYING", "<https://opensource.org/license/MIT>").translated +
           "\n";
}
