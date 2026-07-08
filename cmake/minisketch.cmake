# Copyright (c) 2023-present The TKNC Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Adds the minisketch library as a subdirectory, disabling tests/bench.
# Produces the `minisketch` static library target.
set(MINISKETCH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MINISKETCH_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(MINISKETCH_INSTALL OFF CACHE BOOL "" FORCE)

add_subdirectory(${PROJECT_SOURCE_DIR}/src/minisketch ${CMAKE_BINARY_DIR}/src/minisketch)
