# Copyright (c) 2023-present The TKNC Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# GenerateBuildInfo.cmake
#
# Invoked via:
#   cmake -DBUILD_INFO_HEADER_PATH=<path> -DSOURCE_DIR=<dir> -P GenerateBuildInfo.cmake
#
# Writes a header containing:
#   #define BUILD_GIT_COMMIT "<hash>-dirty?"
# derived from `git rev-parse HEAD` and `git status --porcelain` in SOURCE_DIR.
# If SOURCE_DIR is not a git repo or git is unavailable, falls back to
# "unknown".

if(NOT DEFINED BUILD_INFO_HEADER_PATH)
    message(FATAL_ERROR "BUILD_INFO_HEADER_PATH must be defined")
endif()
if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR must be defined")
endif()

set(_commit "unknown")
set(_dirty "")

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE _commit_out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _res
    )
    if(_res EQUAL 0 AND _commit_out)
        set(_commit "${_commit_out}")
    endif()

    execute_process(
        COMMAND ${GIT_EXECUTABLE} status --porcelain
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE _status_out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _status_res
    )
    if(_status_res EQUAL 0 AND _status_out)
        set(_dirty "-dirty")
    endif()
endif()

set(_content "")
string(APPEND _content "#define BUILD_GIT_COMMIT \"${_commit}${_dirty}\"\n")
file(WRITE "${BUILD_INFO_HEADER_PATH}" "${_content}")

message(STATUS "Generated build info: BUILD_GIT_COMMIT=\"${_commit}${_dirty}\"")
