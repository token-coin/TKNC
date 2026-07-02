# Copyright (c) 2023-present The TKNC Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

cmake_minimum_required(VERSION 3.22)

# Allow subprojects with older cmake_minimum_required to build
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

project(tknc
    VERSION 1.0.0
    DESCRIPTION "TKNC Core"
    HOMEPAGE_URL "https://tkncchain.org/"
    LANGUAGES C CXX
)

# Append cmake module path so include(AddWindowsResources) etc. resolve
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# Enable Windows resource compiler (.rc files)
if(WIN32)
    enable_language(RC)
endif()

#=============================
# Build options
#=============================
option(BUILD_DAEMON "Build tkncd executable." ON)
option(BUILD_CLI "Build tknc-cli executable." ON)
option(BUILD_TX "Build tknc-tx executable." ON)
option(BUILD_UTIL "Build tknc-util executable." ON)
option(BUILD_TKNC_BIN "Build tknc executable." ON)
option(BUILD_WALLET_TOOL "Build tknc-wallet tool." ON)
option(BUILD_GUI "Build tknc-qt executable." OFF)
option(BUILD_BENCH "Build bench_tknc executable." OFF)
option(BUILD_TESTS "Build test_tknc and other unit test executables." ON)
option(BUILD_FUZZ_BINARY "Build fuzz binary." OFF)
option(BUILD_FOR_FUZZING "Build for fuzzing." OFF)
option(BUILD_MINER "Build tknc-miner executable." ON)
option(BUILD_KERNEL_LIB "Build experimental tknckernel library." OFF)
option(BUILD_KERNEL_TEST "Build kernel tests." OFF)
option(BUILD_UTIL_CHAINSTATE "Build experimental tknc-chainstate executable." OFF)

option(ENABLE_WALLET "Enable wallet." ON)
option(ENABLE_IPC "Enable IPC." OFF)
option(WITH_ZMQ "Enable ZMQ." OFF)
option(WITH_EMBEDDED_ASMAP "Embed asmap data file." OFF)
option(WITH_EXTERNAL_LIBMULTIPROCESS "Use external libmultiprocess." OFF)

#=============================
# Version / copyright info
#=============================
set(CLIENT_VERSION_MAJOR 1)
set(CLIENT_VERSION_MINOR 0)
set(CLIENT_VERSION_BUILD 0)
set(CLIENT_VERSION_IS_RELEASE false)
set(CLIENT_VERSION_STRING "1.0.0")

set(COPYRIGHT_YEAR 2026)
set(COPYRIGHT_HOLDERS "The %s developers")
set(COPYRIGHT_HOLDERS_FINAL "The TKNC developers")
set(COPYRIGHT_HOLDERS_SUBSTITUTION "TKNC")

set(CLIENT_NAME "TKNC")
set(CLIENT_URL "https://tkncchain.org/")
set(CLIENT_BUGREPORT "https://github.com/tknc/tknc/issues")

#=============================
# Feature detection (Windows defaults — match previous successful build)
#=============================
set(HAVE_DECL_FORK 0)
set(HAVE_DECL_PIPE2 0)
set(HAVE_DECL_SETSID 0)
set(HAVE_O_CLOEXEC 0)
set(HAVE_SYSTEM 1)
set(ENABLE_EXTERNAL_SIGNER 1)
set(ENABLE_WALLET 1)

#=============================
# Output directories
#=============================
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
endif()
if(NOT CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
endif()

#=============================
# core_interface — hardening flags shared by all targets
#=============================
add_library(core_interface INTERFACE)
if(WIN32)
    target_compile_definitions(core_interface INTERFACE
        NOMINMAX
        WIN32
        WIN32_LEAN_AND_MEAN
        _MT
        _WIN32_IE=0x0A00
        _WIN32_WINNT=0x0A00
        _WINDOWS
    )
endif()
# TKNC source requires assertions to be enabled (see src/util/check.h).
# Strip off any NDEBUG that Release mode may have added.
target_compile_options(core_interface INTERFACE -UNDEBUG)
target_compile_options(core_interface INTERFACE
    -O2
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++20>
    -fmacro-prefix-map=${CMAKE_SOURCE_DIR}/src=.
    -fstack-reuse=none
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=3
    -Wstack-protector
    -fstack-protector-all
    -fcf-protection=full
    -Wall
    -Wextra
    -Wformat
    -Wformat-security
    -Wvla
    -Wredundant-decls
    -Wdate-time
    -Wduplicated-branches
    -Wduplicated-cond
    -Wlogical-op
    $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
    $<$<COMPILE_LANGUAGE:CXX>:-Wsuggest-override>
    -Wimplicit-fallthrough
    -Wunreachable-code
    -Wbidi-chars=any
    -Wundef
    -Wno-unused-parameter
    -Wno-deprecated-declarations
)
if(MINGW)
    target_compile_options(core_interface INTERFACE
        -Wa,-muse-unaligned-vector-move
        -fno-extended-identifiers
    )
endif()
if(WIN32)
    target_link_options(core_interface INTERFACE
        -O2
        -static
        -Wl,--major-subsystem-version,6
        -Wl,--minor-subsystem-version,2
        -fstack-reuse=none
        -fstack-protector-all
        -fcf-protection=full
        -Wl,--enable-reloc-section
        -Wl,--dynamicbase
        -Wl,--nxcompat
        -Wl,--high-entropy-va
    )
else()
    target_link_options(core_interface INTERFACE
        -O2
        -fstack-reuse=none
        -fstack-protector-all
        -fcf-protection=full
    )
endif()

#=============================
# System-include override directory (BEFORE)
#=============================
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/BEFORE)
    file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/BEFORE)
endif()

#=============================
# Boost (header-only target)
#=============================
find_package(Boost QUIET)
if(TARGET Boost::headers)
    # Use upstream Boost::headers
else()
    add_library(Boost_headers INTERFACE)
    target_include_directories(Boost_headers SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/BEFORE)
    add_library(Boost::headers ALIAS Boost_headers)
endif()

#=============================
# libevent targets
#=============================
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBEVENT QUIET libevent_core libevent_extra)
endif()

# Fallback: create libevent::core / libevent::extra / libevent::pthreads
# using mingw64 system libs if find_package didn't provide them.
# Only create libevent::pthreads if the library actually exists.
#
# On Windows with static MinGW libevent, event_extra.a depends on symbols in
# event_core.a. We declare a LINK_GROUP so CMake wraps both libs in
# --start-group/--end-group, allowing the linker to resolve circular refs.
set(CMAKE_LINK_GROUP_USING_libevent "LINKER:--start-group" "LINKER:--end-group")
set(CMAKE_LINK_GROUP_USING_libevent_SUPPORTED TRUE)
if(NOT TARGET libevent::core)
    add_library(libevent_core_iface INTERFACE)
    target_link_libraries(libevent_core_iface INTERFACE
        "$<LINK_GROUP:libevent,event_core;event_extra>"
    )
    add_library(libevent::core ALIAS libevent_core_iface)
endif()
if(NOT TARGET libevent::extra)
    add_library(libevent_extra_iface INTERFACE)
    target_link_libraries(libevent_extra_iface INTERFACE
        "$<LINK_GROUP:libevent,event_extra;event_core>"
    )
    add_library(libevent::extra ALIAS libevent_extra_iface)
endif()
# Only create libevent::pthreads target if libevent_pthreads library exists
# (it doesn't ship with MSYS2 mingw64 libevent package).
find_library(LIBEVENT_PTHREADS_LIB NAMES event_pthreads libevent_pthreads)
if(LIBEVENT_PTHREADS_LIB AND NOT TARGET libevent::pthreads)
    add_library(libevent_pthreads_iface INTERFACE)
    target_link_libraries(libevent_pthreads_iface INTERFACE ${LIBEVENT_PTHREADS_LIB})
    add_library(libevent::pthreads ALIAS libevent_pthreads_iface)
endif()

#=============================
# SQLite3 target
#=============================
find_package(SQLite3 QUIET)
if(NOT TARGET SQLite3::SQLite3)
    add_library(sqlite3_iface INTERFACE)
    target_link_libraries(sqlite3_iface INTERFACE sqlite3)
    add_library(SQLite3::SQLite3 ALIAS sqlite3_iface)
endif()

#=============================
# USDT headers (optional, no-op stub)
#=============================
if(NOT TARGET USDT::headers)
    add_library(usdt_headers_iface INTERFACE)
    add_library(USDT::headers ALIAS usdt_headers_iface)
endif()

#=============================
# VCPKG defaults (empty if not set)
#=============================
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "" CACHE STRING "Vcpkg triplet")
endif()
if(NOT DEFINED VCPKG_INSTALLED_DIR)
    set(VCPKG_INSTALLED_DIR "" CACHE PATH "Vcpkg installed dir")
endif()
if(NOT DEFINED VCPKG_INSTALLED)
    set(VCPKG_INSTALLED "" CACHE PATH "Vcpkg installed root")
endif()

#=============================
# Subdirectories
#=============================
add_subdirectory(src)
