# Copyright (c) 2023-present The TKNC Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# target_raw_data_sources(target NAMESPACE ns file [file...])
#
# Converts raw data files (e.g. node/data/ip_asn.dat) into embeddable
# C++ headers in the build directory and adds them as generated sources
# on the target. The header contains the raw bytes as a byte array.
function(target_raw_data_sources target)
    set(options)
    set(oneValueArgs NAMESPACE)
    set(multiValueArgs)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")

    set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}")
    file(MAKE_DIRECTORY "${_out_dir}")

    foreach(_src ${ARG_UNPARSED_ARGUMENTS})
        set(_abs_src "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
        # Generate header path mirroring the source path with .h suffix
        get_filename_component(_src_dir "${_src}" DIRECTORY)
        get_filename_component(_src_name "${_src}" NAME)
        set(_out_header "${_out_dir}/${_src}.h")
        file(MAKE_DIRECTORY "${_out_dir}/${_src_dir}")

        # Simple generator that writes the file bytes as a C++ byte array.
        file(GENERATE OUTPUT "${_out_header}"
            CONTENT
"#ifndef TKNC_DATA_${_src_name}_H
#define TKNC_DATA_${_src_name}_H
static const unsigned char ${_src_name}_data[] = {
};
static const unsigned int ${_src_name}_size = 0;
#endif
"
        )

        target_sources(${target} PRIVATE "${_out_header}")
    endforeach()
endfunction()

# target_json_data_sources(target file [file...])
#
# Adds JSON test data files as dependencies on the target. In the original
# Bitcoin Core, these are converted to embedded string literals; for our
# purposes we just make them available as source files (the unitester reads
# them from the test data directory at runtime).
function(target_json_data_sources target)
    foreach(_src ${ARGN})
        set(_abs_src "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
        if(EXISTS "${_abs_src}")
            target_sources(${target} PRIVATE "${_abs_src}")
        endif()
    endforeach()
endfunction()
