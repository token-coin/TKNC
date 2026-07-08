# Copyright (c) 2025-present The TKN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include(ts_files.cmake)

if(Qt6_VERSION VERSION_GREATER_EQUAL 6.7)
  qt6_add_lrelease(TS_FILES ${ts_files} LRELEASE_TARGET tkncqt_lrelease OPTIONS -silent)
else()
  qt6_add_lrelease(tkncqt TS_FILES ${ts_files} OPTIONS -silent)
endif()

add_dependencies(tkncqt tkncqt_lrelease)
