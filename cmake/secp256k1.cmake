# Copyright (c) 2023-present The TKNC Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Defines add_secp256k1(name) function that adds the secp256k1 library
# as a subdirectory under a separate binary directory name.
# Produces the `secp256k1` static library target.
function(add_secp256k1 name)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libs" FORCE)
    set(SECP256K1_DISABLE_TESTS ON CACHE BOOL "" FORCE)
    set(SECP256K1_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
    set(SECP256K1_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SECP256K1_BUILD_EXHAUSTIVE_TESTS OFF CACHE BOOL "" FORCE)
    set(SECP256K1_INSTALL OFF CACHE BOOL "" FORCE)
    set(SECP256K1_ENABLE_MODULE_RECOVERY ON CACHE BOOL "" FORCE)
    set(SECP256K1_ENABLE_MODULE_SCHNORRSIG ON CACHE BOOL "" FORCE)
    set(SECP256K1_ENABLE_MODULE_ELLSWIFT ON CACHE BOOL "" FORCE)
    set(SECP256K1_ENABLE_MODULE_EXTRAKEYS ON CACHE BOOL "" FORCE)
    set(SECP256K1_ENABLE_MODULE_MUSIG ON CACHE BOOL "" FORCE)

    add_subdirectory(${PROJECT_SOURCE_DIR}/src/secp256k1 ${CMAKE_BINARY_DIR}/src/${name})
endfunction()
