# Copyright (c) 2023-present The TKNC Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Defines add_libmultiprocess(target) function.
# Only used when ENABLE_IPC AND NOT WITH_EXTERNAL_LIBMULTIPROCESS.
function(add_libmultiprocess target)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/ipc/libmultiprocess ${CMAKE_BINARY_DIR}/src/${target})
endfunction()
