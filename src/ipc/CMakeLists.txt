# Copyright (c) 2023-present The TKNC Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

add_library(tknc_ipc STATIC EXCLUDE_FROM_ALL
  capnp/mining.cpp
  capnp/protocol.cpp
  interfaces.cpp
  process.cpp
)

target_capnp_sources(tknc_ipc ${CMAKE_CURRENT_SOURCE_DIR}
  capnp/common.capnp
  capnp/echo.capnp
  capnp/init.capnp
  capnp/mining.capnp
  capnp/rpc.capnp
)

target_link_libraries(tknc_ipc
  PRIVATE
    core_interface
    univalue
    Boost::headers
)

if(BUILD_TESTS)
  # tknc_ipc_test library target is defined here in src/ipc/CMakeLists.txt
  # instead of src/ipc/test/CMakeLists.txt so capnp files in src/ipc/test/ are able to
  # reference capnp files in src/ipc/capnp/ by relative path. The Cap'n Proto
  # compiler only allows importing by relative path when the importing and
  # imported files are underneath the same compilation source prefix, so the
  # source prefix must be src/ipc, not src/ipc/test/
  add_library(tknc_ipc_test STATIC EXCLUDE_FROM_ALL
    test/ipc_test.cpp
  )
  target_capnp_sources(tknc_ipc_test ${CMAKE_CURRENT_SOURCE_DIR}
    test/ipc_test.capnp
  )
  add_dependencies(tknc_ipc_test tknc_ipc_headers)

  target_link_libraries(tknc_ipc_test
    PRIVATE
      core_interface
      univalue
      Boost::headers
  )
endif()

configure_file(.clang-tidy.in .clang-tidy USE_SOURCE_PERMISSIONS COPYONLY)
