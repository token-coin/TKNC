# Copyright (c) 2023-present The TKNC Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include(AddWindowsResources)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/tknc-build-config.h.in tknc-build-config.h USE_SOURCE_PERMISSIONS @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})

if(VCPKG_TARGET_TRIPLET MATCHES "-static")
 set(VCPKG_TARGET_TRIPLET_MATCHES_STATIC ON CACHE INTERNAL "")
 if(NOT VCPKG_INSTALLED_DIR)
 set(VCPKG_INSTALLED_DIR "C:/vcpkg/installed")
 endif()
 set(VCPKG_STATIC_LIB_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
else()
 set(VCPKG_TARGET_TRIPLET_MATCHES_STATIC OFF CACHE INTERNAL "")
 set(VCPKG_STATIC_LIB_DIR "")
endif()

#=============================
# Subprojects
#=============================
# Subprojects include subdirectories that do or could have tests
# and/or benchmark binaries, such as all subtrees and univalue.
# These need to be included before CMAKE_*_OUTPUT_DIRECTORY variables
# are set, so output locations of subproject tests and libraries
# are not overridden.
include(../cmake/crc32c.cmake)
include(../cmake/leveldb.cmake)
include(../cmake/minisketch.cmake)
add_subdirectory(univalue)
if (ENABLE_IPC AND NOT WITH_EXTERNAL_LIBMULTIPROCESS)
 include(../cmake/libmultiprocess.cmake)
 add_libmultiprocess(ipc/libmultiprocess)
endif()
include(../cmake/secp256k1.cmake)
add_secp256k1(secp256k1)

# Set top-level target output locations.
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
 set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
 set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
endif()
if(NOT CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
 set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
endif()

add_custom_target(generate_build_info
 BYPRODUCTS ${PROJECT_BINARY_DIR}/src/tknc-build-info.h
 COMMAND ${CMAKE_COMMAND} -DBUILD_INFO_HEADER_PATH=${PROJECT_BINARY_DIR}/src/tknc-build-info.h -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/script/GenerateBuildInfo.cmake
 COMMENT "Generating tknc-build-info.h"
 VERBATIM
)
add_library(tknc_clientversion STATIC
 clientversion.cpp
)
target_link_libraries(tknc_clientversion
 PRIVATE
 core_interface
)
add_dependencies(tknc_clientversion generate_build_info)

add_subdirectory(crypto)
add_subdirectory(util)
if(ENABLE_IPC)
 add_subdirectory(ipc)
else()
 add_library(tknc_ipc STATIC EXCLUDE_FROM_ALL ipc/stub.cpp)
endif()

add_library(tknc_consensus STATIC EXCLUDE_FROM_ALL
 arith_uint256.cpp
 consensus/merkle.cpp
 consensus/tx_check.cpp
 hash.cpp
 primitives/block.cpp
 primitives/transaction.cpp
 pubkey.cpp
 script/interpreter.cpp
 script/script.cpp
 script/script_error.cpp
 uint256.cpp
)
target_link_libraries(tknc_consensus
 PRIVATE
 core_interface
 tknc_crypto
 secp256k1
)

if(WITH_ZMQ)
 add_subdirectory(zmq)
endif()

# Home for common functionality shared by different executables and libraries.
# Similar to `tknc_util` library, but higher-level.
add_library(tknc_common STATIC EXCLUDE_FROM_ALL
 addresstype.cpp
 base58.cpp
 bech32.cpp
 chain.cpp
 chainparamsbase.cpp
 coins.cpp
 common/args.cpp
 common/bloom.cpp
 common/config.cpp
 common/init.cpp
 common/interfaces.cpp
 common/license_info.cpp
 common/messages.cpp
 common/netif.cpp
 common/pcp.cpp
 common/run_command.cpp
 common/settings.cpp
 common/signmessage.cpp
 common/system.cpp
 common/url.cpp
 compressor.cpp
 core_io.cpp
 deploymentinfo.cpp
 external_signer.cpp
 init/common.cpp
 key.cpp
 key_io.cpp
 merkleblock.cpp
 musig.cpp
 net_permissions.cpp
 net_types.cpp
 netaddress.cpp
 netbase.cpp
 outputtype.cpp
 policy/feerate.cpp
 policy/policy.cpp
 pow.cpp
 pow/difficulty.cpp
 pow/cn_slow_hash_soft.cpp
 $<$<NOT:$<PLATFORM_ID:Darwin>>:pow/cn_slow_hash_hard_intel.cpp>
 $<$<NOT:$<PLATFORM_ID:Darwin>>:pow/cn_slow_hash_intel_avx2.cpp>
 pow/keccak.c
 pow/aux_hash.c
 protocol.cpp
 psbt.cpp
 rpc/rawtransaction_util.cpp
 rpc/request.cpp
 rpc/util.cpp
 scheduler.cpp
 script/descriptor.cpp
 script/miniscript.cpp
 script/parsing.cpp
 script/sign.cpp
 script/signingprovider.cpp
 script/solver.cpp
)
target_link_libraries(tknc_common
 PRIVATE
 core_interface
 tknc_consensus
 tknc_util
 univalue
 secp256k1
 $<TARGET_NAME_IF_EXISTS:USDT::headers>
 $<$<PLATFORM_ID:Windows>:ws2_32>
)

# Enable AES/AVX2 intrinsics for Intel-optimized hash functions on non-Darwin x86 targets
if(NOT APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
 set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/pow/cn_slow_hash_hard_intel.cpp PROPERTIES COMPILE_OPTIONS "-maes;-mavx2")
 set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/pow/cn_slow_hash_intel_avx2.cpp PROPERTIES COMPILE_OPTIONS "-maes;-mavx2")
endif()

include(InstallBinaryComponent)

if(ENABLE_WALLET)
 add_subdirectory(wallet)

 if(BUILD_WALLET_TOOL)
 add_executable(tknc-wallet
 tknc-wallet.cpp
 init/tknc-wallet.cpp
 wallet/wallettool.cpp
 )
 add_windows_resources(tknc-wallet tknc-wallet-res.rc)
 add_windows_application_manifest(tknc-wallet)
 target_link_libraries(tknc-wallet
 core_interface
 tknc_wallet
 tknc_common
 tknc_node
 tknc_util
 )
 install_binary_component(tknc-wallet HAS_MANPAGE)
 endif()
endif()


# P2P and RPC server functionality used by `tkncd` and `tknc-qt` executables.
add_library(tknc_node STATIC EXCLUDE_FROM_ALL
 addrdb.cpp
 addrman.cpp
 banman.cpp
 bip324.cpp
 blockencodings.cpp
 blockfilter.cpp
 consensus/tx_verify.cpp
 dbwrapper.cpp
 deploymentstatus.cpp
 flatfile.cpp
 headerssync.cpp
 httprpc.cpp
 httpserver.cpp
 inference_gateway.cpp
 i2p.cpp
 index/base.cpp
 index/blockfilterindex.cpp
 index/coinstatsindex.cpp
 index/txindex.cpp
 index/txospenderindex.cpp
 index/addressindex.cpp
 init.cpp
 kernel/chain.cpp
 kernel/checks.cpp
 kernel/coinstats.cpp
 kernel/context.cpp
 kernel/cs_main.cpp
 kernel/disconnected_transactions.cpp
 kernel/mempool_removal_reason.cpp
 mapport.cpp
 seed_register.cpp
 ws_proxy.cpp
 net.cpp
 net_processing.cpp
 netgroup.cpp
 node/abort.cpp
 node/blockmanager_args.cpp
 node/blockstorage.cpp
 node/caches.cpp
 node/chainstate.cpp
 node/chainstatemanager_args.cpp
 node/coin.cpp
 node/coins_view_args.cpp
 node/connection_types.cpp
 node/context.cpp
 node/database_args.cpp
 node/eviction.cpp
 node/interface_ui.cpp
 node/interfaces.cpp
 node/kernel_notifications.cpp
 node/mempool_args.cpp
 node/mempool_persist.cpp
 node/mempool_persist_args.cpp
 node/miner.cpp
 node/miner_registry.cpp
 node/mini_miner.cpp
 node/minisketchwrapper.cpp
 node/peerman_args.cpp
 node/psbt.cpp
 node/timeoffsets.cpp
 node/transaction.cpp
 node/txdownloadman_impl.cpp
 node/txorphanage.cpp
 node/txreconciliation.cpp
 node/utxo_snapshot.cpp
 node/warnings.cpp
 noui.cpp
 policy/ephemeral_policy.cpp
 policy/fees/block_policy_estimator.cpp
 policy/fees/block_policy_estimator_args.cpp
 policy/packages.cpp
 policy/rbf.cpp
 policy/settings.cpp
 policy/truc_policy.cpp
 private_broadcast.cpp
 rest.cpp
 rpc/blockchain.cpp
 rpc/external_signer.cpp
 rpc/fees.cpp
 rpc/mempool.cpp
 rpc/mining.cpp
 rpc/net.cpp
 rpc/node.cpp
 rpc/output_script.cpp
 rpc/rawtransaction.cpp
 rpc/server.cpp
 rpc/server_util.cpp
 rpc/signmessage.cpp
 rpc/txoutproof.cpp
 script/sigcache.cpp
 signet.cpp
 torcontrol.cpp
 txdb.cpp
 txgraph.cpp
 txmempool.cpp
 txrequest.cpp
 validation.cpp
 validationinterface.cpp
 versionbits.cpp
 tknc_chainparams.cpp
 economics/emission.h
 pow/transformerpow.cpp
 pow/tknchash.cpp
 pow/gpu_transformerpow.cpp
 pow/opencl_miner.cpp
 pow/real_gpu_miner.cpp
 net/message.cpp
 net/p2p_discovery.cpp
 net/api_protocol.cpp
 net/p2p_llm.cpp
 net/nat_traversal.cpp
 net/inference_engine.cpp
 net/miner_registry.cpp
 net/backend_router.cpp
 net/local_backend.cpp
 net/remote_backend.cpp
 net/p2p_reachability.cpp
 net/peer_session.cpp
 net/endpoint_registry.cpp
 miner/miner.cpp
 miner/mode_switcher.cpp
 model/loader.cpp
 model/gpu_memory.cpp
 model/llm_inference.cpp
 model/llama_dll.cpp
 model/runtime.cpp
 apikey/api_key.cpp
 apikey/api_key_db.cpp
 rpc/tknc_apikey.cpp
 escrow/escrow_db.cpp
 rpc/escrow.cpp
 rpc/model_registry.cpp
 escrow/escrow.cpp
 billing/billing_receipt.cpp
 model/registry_db.cpp
 model/registry.cpp
 $<$<TARGET_EXISTS:tknc_wallet>:wallet/init.cpp>
 $<$<NOT:$<TARGET_EXISTS:tknc_wallet>>:dummywallet.cpp>
)
target_link_libraries(tknc_node
 PRIVATE
 core_interface
 tknc_common
 tknc_util
 $<TARGET_NAME_IF_EXISTS:tknc_zmq>
 leveldb
 minisketch
 univalue
 Boost::headers
 $<IF:$<BOOL:${VCPKG_TARGET_TRIPLET_MATCHES_STATIC}>,,$<TARGET_NAME_IF_EXISTS:libevent::extra>>
 $<IF:$<BOOL:${VCPKG_TARGET_TRIPLET_MATCHES_STATIC}>,,$<TARGET_NAME_IF_EXISTS:libevent::core>>
 $<IF:$<BOOL:${VCPKG_TARGET_TRIPLET_MATCHES_STATIC}>,,$<TARGET_NAME_IF_EXISTS:libevent::pthreads>>
 $<TARGET_NAME_IF_EXISTS:USDT::headers>
)

# Use DLL build (/MD) instead of static build (/MT) to match VCPKG CRT.
# The llama-cpp-build produces import .libs + runtime .dlls, all /MD.
set(LLAMA_CPP_BUILD_DIR "${CMAKE_BINARY_DIR}/llama-cpp-build")
set(LLAMA_CPP_SRC_DIR "${CMAKE_BINARY_DIR}/llama-cpp-source")

target_include_directories(tknc_node PRIVATE
 ${LLAMA_CPP_SRC_DIR}/include
 ${LLAMA_CPP_SRC_DIR}/ggml/include
 ${CMAKE_CURRENT_BINARY_DIR} # For generated headers (e.g. pow/tokenhash_cl.h)
)

if(WIN32)
 if(VCPKG_TARGET_TRIPLET MATCHES "-static")
 if(NOT VCPKG_INSTALLED_DIR)
 set(VCPKG_INSTALLED_DIR "C:/vcpkg/installed")
 endif()
 set(VCPKG_STATIC_LIB_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
 set(EVENT_STATIC_LIBS
 ${VCPKG_STATIC_LIB_DIR}/event.lib
 ${VCPKG_STATIC_LIB_DIR}/event_core.lib
 ${VCPKG_STATIC_LIB_DIR}/event_extra.lib
 )
 endif()
 # Note: llama.dll loaded dynamically via GetProcAddress (no static link needed)
 target_link_libraries(tknc_node PRIVATE
 ${EVENT_STATIC_LIBS}
 )
endif()
if(WITH_EMBEDDED_ASMAP)
 target_compile_definitions(tknc_node PRIVATE ENABLE_EMBEDDED_ASMAP=1)
 include(TargetDataSources)
 target_raw_data_sources(tknc_node NAMESPACE node::data
 node/data/ip_asn.dat
 )
endif()

# TKNC wrapper executable that can call other executables.
if(BUILD_TKNC_BIN)
 add_executable(tknc tknc.cpp)
 add_windows_resources(tknc tknc-res.rc)
 add_windows_application_manifest(tknc)
 target_link_libraries(tknc core_interface tknc_common tknc_util)
 install_binary_component(tknc HAS_MANPAGE)
endif()

# TKNC Core daemon.
if(BUILD_DAEMON)
 add_executable(tkncd
 tkncd.cpp
 init/tkncd.cpp
 )
 add_windows_resources(tkncd tknc-res.rc)
 add_windows_application_manifest(tkncd)
 target_link_libraries(tkncd
 core_interface
 tknc_node
 $<TARGET_NAME_IF_EXISTS:tknc_wallet>
 )
 if(WIN32)
 target_link_libraries(tkncd pdh d3d11 d3dcompiler dxgi)
 endif()
 install_binary_component(tkncd HAS_MANPAGE)
endif()
if(ENABLE_IPC AND BUILD_DAEMON)
 add_executable(tknc-node
 tkncd.cpp
 init/tknc-node.cpp
 )
 target_link_libraries(tknc-node
 core_interface
 tknc_node
 tknc_ipc
 $<TARGET_NAME_IF_EXISTS:tknc_wallet>
 )
 install_binary_component(tknc-node INTERNAL)
endif()


add_library(tknc_cli STATIC EXCLUDE_FROM_ALL
 compat/stdin.cpp
 rpc/client.cpp
)
target_link_libraries(tknc_cli
 PUBLIC
 core_interface
 univalue
)


# TKNC RPC client
if(BUILD_CLI)
 add_executable(tknc-cli tknc-cli.cpp init/basic.cpp)
 add_windows_resources(tknc-cli tknc-cli-res.rc)
 add_windows_application_manifest(tknc-cli)
 target_link_libraries(tknc-cli
 core_interface
 tknc_cli
 tknc_common
 tknc_ipc
 tknc_util
 tknc_node
 $<IF:$<BOOL:${VCPKG_TARGET_TRIPLET_MATCHES_STATIC}>,,libevent::extra>
 $<IF:$<BOOL:${VCPKG_TARGET_TRIPLET_MATCHES_STATIC}>,,libevent::core>
 $<IF:$<BOOL:${VCPKG_TARGET_TRIPLET_MATCHES_STATIC}>,${VCPKG_STATIC_LIB_DIR}/event.lib;${VCPKG_STATIC_LIB_DIR}/event_core.lib;${VCPKG_STATIC_LIB_DIR}/event_extra.lib,>
 $<IF:$<PLATFORM_ID:Darwin>,,crypto ssl z m>
 )
 if(WIN32)
 target_link_libraries(tknc-cli crypt32 ws2_32 iphlpapi bcrypt)
 endif()
 if(APPLE)
 target_link_libraries(tknc-cli "-framework Security")
 endif()
 install_binary_component(tknc-cli HAS_MANPAGE)
endif()

# TKNC AI Miner (standalone mining + inference executable)
option(BUILD_MINER "Build tknc-miner executable." ON)
if(BUILD_MINER)
 add_library(tknc_miner_lib STATIC EXCLUDE_FROM_ALL
 miner/miner.cpp
 miner/mode_switcher.cpp
 miner/api_server.cpp
 miner/review_chain.cpp
 pow/transformerpow.cpp
 pow/tknchash.cpp
 pow/difficulty.cpp
 pow/real_gpu_miner.cpp
 pow/gpu_transformerpow.cpp
 model/loader.cpp
 model/runtime.cpp
 model/gpu_memory.cpp
 model/llm_inference.cpp
 model/llama_dll.cpp
 net/api_protocol.cpp
 net/dht/dht_core.cpp
 net/dht/routing_table.cpp
 net/nat_traversal.cpp
 net/connection_manager.cpp
 )
 target_include_directories(tknc_miner_lib PUBLIC
 ${CMAKE_CURRENT_SOURCE_DIR}
 ${VCPKG_INSTALLED_DIR}/x64-windows/include
 ${CMAKE_BINARY_DIR}/llama-cpp/include
 ${CMAKE_BINARY_DIR}/llama-cpp/ggml/include
 )
 # from MSVC-compiled llama.dll. MinGW's -fstack-protector inserts canaries that can
 # be invalidated by MSVC SEH-style exceptions in the DLL, causing spurious
 # "stack smashing detected" aborts at function return boundaries in llama_decode.
 if(NOT MSVC)
 target_compile_options(tknc_miner_lib PRIVATE -fno-stack-protector)
 endif()
 target_link_libraries(tknc_miner_lib
 PRIVATE
 core_interface
 tknc_common
 tknc_node
 univalue
 )
 if(WIN32)
 target_link_libraries(tknc_miner_lib
 PRIVATE
 pdh
 d3d11
 d3dcompiler
 $<IF:$<BOOL:${VCPKG_TARGET_TRIPLET_MATCHES_STATIC}>,${VCPKG_STATIC_LIB_DIR}/event_core.lib,event_core>
 $<IF:$<BOOL:${VCPKG_TARGET_TRIPLET_MATCHES_STATIC}>,${VCPKG_STATIC_LIB_DIR}/event_extra.lib,event_extra>
 ws2_32
 )
 endif()

 add_executable(tknc-miner tknc-miner.cpp)
 add_windows_resources(tknc-miner tknc-miner-res.rc)
 add_windows_application_manifest(tknc-miner)

 set(LLAMA_CPP_BUILD_DIR "${CMAKE_BINARY_DIR}/llama-cpp-build")
 set(LLAMA_CPP_SRC_DIR "${CMAKE_BINARY_DIR}/llama-cpp-source")

 target_include_directories(tknc-miner PRIVATE
 ${LLAMA_CPP_SRC_DIR}/include
 ${LLAMA_CPP_SRC_DIR}/ggml/include
 ${CMAKE_CURRENT_BINARY_DIR} # For generated headers (e.g. tokenhash_cl.h)
 )

 target_link_libraries(tknc-miner
 PRIVATE
 tknc_miner_lib
 core_interface
 tknc_common
 tknc_node
 tknc_util
 $<TARGET_NAME_IF_EXISTS:tknc_wallet>
 univalue
 )

if(WIN32)
 if(WIN32)
 target_link_libraries(tknc-miner PRIVATE
 pdh
 d3d11
 d3dcompiler
 dxgi
 )
 endif()
endif()
 install_binary_component(tknc-miner HAS_MANIFEST)
 # Set PE image version (fixes "image version 0.00" which triggers AV heuristics)
 if(WIN32)
 if(MSVC)
 set_target_properties(tknc-miner PROPERTIES LINK_FLAGS "/VERSION:1.0 /STACK:8388608")
 else()
 set_target_properties(tknc-miner PROPERTIES LINK_FLAGS "-Wl,--stack,8388608")
 endif()
 endif()
 # CRITICAL: disable stack-protector on tknc-miner too.
 # core_interface adds -fstack-protector-all via INTERFACE; we need to override
 # it AFTER all inherited flags. Use COMPILE_OPTIONS property with BEFORE/APPEND
 target_compile_options(tknc-miner PRIVATE -fno-stack-protector)
 # Also remove stack-protector from the static lib that contains llama_dll.cpp
 # (the actual code that calls into MSVC-compiled llama.dll).
 if(NOT MSVC)
 target_compile_options(tknc_miner_lib PRIVATE -fno-stack-protector)
 endif()
 # Copy OpenCL kernel file to output directory (A2: exe reads from its own directory)
 add_custom_command(TARGET tknc-miner POST_BUILD
 COMMAND ${CMAKE_COMMAND} -E copy_if_different
 "${CMAKE_CURRENT_SOURCE_DIR}/pow/tokenhash.cl"
 "$<TARGET_FILE_DIR:tknc-miner>/tokenhash.cl"
 COMMENT "Copying tokenhash.cl to build output directory"
 )

 # Embed OpenCL kernel as compile-time string fallback (zero external dependency)
 # Generates pow/tokenhash_cl.h containing: static const char TOKENHASH_CL_KERNEL[] = R"clraw(...)";
 set(EMBEDDED_CL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pow/tokenhash_cl.h")
 add_custom_command(OUTPUT ${EMBEDDED_CL_HEADER}
 COMMAND ${CMAKE_COMMAND}
 "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/pow/tokenhash.cl"
 "-DOUTPUT_FILE=${EMBEDDED_CL_HEADER}"
 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_cl.cmake"
 DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/pow/tokenhash.cl"
 COMMENT "Embedding tokenhash.cl as string constant"
 VERBATIM
 )
 add_custom_target(embed_tokenhash_cl DEPENDS ${EMBEDDED_CL_HEADER})
 add_dependencies(tknc-miner embed_tokenhash_cl)
 add_dependencies(tknc_node embed_tokenhash_cl)
endif()

if(BUILD_TX)
 add_executable(tknc-tx tknc-tx.cpp tknc_chainparams.cpp)
 add_windows_resources(tknc-tx tknc-tx-res.rc)
 add_windows_application_manifest(tknc-tx)
 target_link_libraries(tknc-tx
 core_interface
 tknc_common
 tknc_util
 univalue
 )
 install_binary_component(tknc-tx HAS_MANPAGE)
endif()


if(BUILD_UTIL)
 add_executable(tknc-util tknc-util.cpp tknc_chainparams.cpp)
 add_windows_resources(tknc-util tknc-util-res.rc)
 add_windows_application_manifest(tknc-util)
 target_link_libraries(tknc-util
 core_interface
 tknc_common
 tknc_util
 )
 install_binary_component(tknc-util HAS_MANPAGE)
endif()


if(BUILD_GUI)
 add_subdirectory(qt)
endif()


# Note: tknc-miner is defined above in BUILD_MINER section (line ~390-437)
# The following duplicate definition has been removed to fix CMake error

# Note: tknc-miner-test and diagnostic-test have been moved to temp_backup/
# These were temporary test files and are no longer needed in the build

# P2P Network Basic Test - Kademlia DHT basic function test (Disabled: test file not found)
# add_executable(test-p2p-basic
# ../../test/test_p2p_basic.cpp
# net/dht/routing_table.cpp
# net/dht/dht_core.cpp
# )
# target_include_directories(test-p2p-basic PRIVATE
# ${CMAKE_CURRENT_SOURCE_DIR}/univalue/include
# )
# target_link_libraries(test-p2p-basic
# PRIVATE
# core_interface
# tknc_node
# tknc_common
# tknc_crypto
# univalue
# $<TARGET_NAME_IF_EXISTS:tknc_wallet>
# )
# install_binary_component(test-p2p-basic)

# P2P Network NAT Traversal Test - NAT traversal function test (Disabled: test file not found)
# add_executable(test-nat-traversal
# ../../test/test_nat_traversal.cpp
# net/nat_traversal.cpp
# )
# target_include_directories(test-nat-traversal PRIVATE
# ${CMAKE_CURRENT_SOURCE_DIR}/univalue/include
# )
# target_link_libraries(test-nat-traversal
# PRIVATE
# core_interface
# tknc_node
# tknc_common
# tknc_crypto
# univalue
# $<TARGET_NAME_IF_EXISTS:tknc_wallet>
# )
# install_binary_component(test-nat-traversal)

# P2P Network Connection Manager Test - Connection manager test (Disabled: test file not found)
# add_executable(test-connection-manager
# ../../test/test_connection_manager.cpp
# net/connection_manager.cpp
# )
# target_include_directories(test-connection-manager PRIVATE
# ${CMAKE_CURRENT_SOURCE_DIR}/univalue/include
# )
# target_link_libraries(test-connection-manager
# PRIVATE
# core_interface
# tknc_node
# tknc_common
# tknc_crypto
# univalue
# $<TARGET_NAME_IF_EXISTS:tknc_wallet>
# )
# install_binary_component(test-connection-manager)

if(BUILD_KERNEL_LIB)
 add_subdirectory(kernel)
 if (BUILD_KERNEL_TEST)
 add_subdirectory(test/kernel)
 endif()
 if(BUILD_UTIL_CHAINSTATE)
 add_executable(tknc-chainstate
 tknc-chainstate.cpp
 )
 add_windows_application_manifest(tknc-chainstate)
 target_link_libraries(tknc-chainstate
 PRIVATE
 core_interface
 tknckernel
 )
 install_binary_component(tknc-chainstate INTERNAL)
 endif()
endif()


add_subdirectory(test/util)
if(BUILD_BENCH)
 add_subdirectory(bench)
endif()

if(BUILD_TESTS)
 add_subdirectory(test)
endif()

if(BUILD_FUZZ_BINARY)
 add_subdirectory(test/fuzz)
endif()
