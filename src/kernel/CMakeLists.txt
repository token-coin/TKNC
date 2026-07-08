# Copyright (c) 2024-present The TKNC Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include(GNUInstallDirs)

# TODO: libtknckernel is a work in progress consensus engine
#       library, as more and more modules are decoupled from the
#       consensus engine, this list will shrink to only those
#       which are absolutely necessary.
add_library(tknckernel
  tknckernel.cpp
  chain.cpp
  checks.cpp
  ../tknc_chainparams.cpp
  coinstats.cpp
  context.cpp
  cs_main.cpp
  disconnected_transactions.cpp
  mempool_removal_reason.cpp
  ../arith_uint256.cpp
  ../chain.cpp
  ../coins.cpp
  ../compressor.cpp
  ../consensus/merkle.cpp
  ../consensus/tx_check.cpp
  ../consensus/tx_verify.cpp
  ../dbwrapper.cpp
  ../deploymentinfo.cpp
  ../deploymentstatus.cpp
  ../flatfile.cpp
  ../hash.cpp
  ../logging.cpp
  ../node/blockstorage.cpp
  ../node/chainstate.cpp
  ../node/utxo_snapshot.cpp
  ../policy/ephemeral_policy.cpp
  ../policy/feerate.cpp
  ../policy/packages.cpp
  ../policy/policy.cpp
  ../policy/rbf.cpp
  ../policy/settings.cpp
  ../policy/truc_policy.cpp
  ../pow.cpp
  ../primitives/block.cpp
  ../primitives/transaction.cpp
  ../pubkey.cpp
  ../random.cpp
  ../randomenv.cpp
  ../script/interpreter.cpp
  ../script/script.cpp
  ../script/script_error.cpp
  ../script/sigcache.cpp
  ../script/solver.cpp
  ../signet.cpp
  ../streams.cpp
  ../sync.cpp
  ../txdb.cpp
  ../txgraph.cpp
  ../txmempool.cpp
  ../uint256.cpp
  ../util/chaintype.cpp
  ../util/check.cpp
  ../util/expected.cpp
  ../util/feefrac.cpp
  ../util/fs.cpp
  ../util/fs_helpers.cpp
  ../util/hasher.cpp
  ../util/moneystr.cpp
  ../util/rbf.cpp
  ../util/signalinterrupt.cpp
  ../util/syserror.cpp
  ../util/threadnames.cpp
  ../util/time.cpp
  ../util/tokenpipe.cpp
  ../validation.cpp
  ../validationinterface.cpp
  ../versionbits.cpp
  $<TARGET_OBJECTS:tknc_clientversion>
  $<TARGET_OBJECTS:tknc_crypto>
  $<TARGET_OBJECTS:leveldb>
  $<TARGET_OBJECTS:crc32c>
)

# Compiler warnings that apply only to the kernel and its dependencies.
# These can be more strict and/or warnings that only apply to shared
# libs.
add_library(kernel_warn_interface INTERFACE)
if(NOT MSVC)
  try_append_cxx_flags("-Wunique-object-duplication" TARGET kernel_warn_interface SKIP_LINK)
endif()

# Also manually apply the warnings to the kernel's internal dependencies
target_link_libraries(tknc_clientversion PRIVATE kernel_warn_interface)
target_link_libraries(tknc_crypto PRIVATE kernel_warn_interface)
target_link_libraries(leveldb PRIVATE kernel_warn_interface)
target_link_libraries(crc32c PRIVATE kernel_warn_interface)

target_link_libraries(tknckernel
  PRIVATE
    core_interface
    kernel_warn_interface
    secp256k1_objs
    $<$<PLATFORM_ID:Windows>:bcrypt>
    $<TARGET_NAME_IF_EXISTS:USDT::headers>
    Boost::headers
)

target_include_directories(tknckernel PRIVATE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/leveldb/include>)

# Add a convenience libtknckernel target as a synonym for tknckernel.
add_custom_target(libtknckernel)
add_dependencies(libtknckernel tknckernel)

get_target_property(tknckernel_type tknckernel TYPE)
if(tknckernel_type STREQUAL "STATIC_LIBRARY")
  target_compile_definitions(tknckernel PUBLIC TKNCKERNEL_STATIC)
endif()

configure_file(${PROJECT_SOURCE_DIR}/libtknckernel.pc.in ${PROJECT_BINARY_DIR}/libtknckernel.pc @ONLY)
install(FILES ${PROJECT_BINARY_DIR}/libtknckernel.pc DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig" COMPONENT libtknckernel)

install(TARGETS tknckernel
  RUNTIME
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT libtknckernel
  LIBRARY
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT libtknckernel
  ARCHIVE
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT libtknckernel
)

install(FILES tknckernel.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT libtknckernel)
