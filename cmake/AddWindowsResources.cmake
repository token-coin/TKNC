# Copyright (c) 2023-present The TKNC Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Defines two helper functions used throughout src/CMakeLists.txt:
#   add_windows_resources(target rc_file)
#   add_windows_application_manifest(target)

# Adds the .rc resource file as a source on the target.
# CMake will automatically invoke windres.exe on Windows when the RC
# language is enabled.
function(add_windows_resources target rc_file)
    if(NOT WIN32)
        return()
    endif()
    get_filename_component(_abs_rc "${rc_file}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    target_sources(${target} PRIVATE "${_abs_rc}")
endfunction()

# Generates a Windows application manifest XML and a matching .rc file
# in the build directory, then adds the generated .rc as a source on
# the target so it gets linked into the executable.
function(add_windows_application_manifest target)
    if(NOT WIN32)
        return()
    endif()

    set(_manifest_file "${CMAKE_BINARY_DIR}/src/${target}.manifest")
    set(_manifest_rc   "${CMAKE_BINARY_DIR}/src/${target}-manifest.rc")

    # Manifest XML matching Bitcoin Core pattern, parameterized on target name.
    file(WRITE "${_manifest_file}" "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n")
    file(APPEND "${_manifest_file}" "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\" xmlns:asmv3=\"urn:schemas-microsoft-com:asm.v3\">\n")
    file(APPEND "${_manifest_file}" "  <assemblyIdentity\n")
    file(APPEND "${_manifest_file}" "      type=\"win32\"\n")
    file(APPEND "${_manifest_file}" "      name=\"org.tknccore.${target}\"\n")
    file(APPEND "${_manifest_file}" "      version=\"1.0.0.0\"\n")
    file(APPEND "${_manifest_file}" "  />\n")
    file(APPEND "${_manifest_file}" "  <asmv3:application>\n")
    file(APPEND "${_manifest_file}" "    <asmv3:windowsSettings xmlns=\"http://schemas.microsoft.com/SMI/2019/WindowsSettings\">\n")
    file(APPEND "${_manifest_file}" "      <activeCodePage>UTF-8</activeCodePage>\n")
    file(APPEND "${_manifest_file}" "    </asmv3:windowsSettings>\n")
    file(APPEND "${_manifest_file}" "  </asmv3:application>\n")
    file(APPEND "${_manifest_file}" "  <trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\">\n")
    file(APPEND "${_manifest_file}" "    <security>\n")
    file(APPEND "${_manifest_file}" "      <requestedPrivileges>\n")
    file(APPEND "${_manifest_file}" "        <requestedExecutionLevel level=\"asInvoker\" uiAccess=\"false\"></requestedExecutionLevel>\n")
    file(APPEND "${_manifest_file}" "      </requestedPrivileges>\n")
    file(APPEND "${_manifest_file}" "    </security>\n")
    file(APPEND "${_manifest_file}" "  </trustInfo>\n")
    file(APPEND "${_manifest_file}" "</assembly>\n")

    # RC file that references the manifest.
    file(WRITE "${_manifest_rc}" "1 /* CREATEPROCESS_MANIFEST_RESOURCE_ID */ 24 /* RT_MANIFEST */ \"${target}.manifest\"\n")

    target_sources(${target} PRIVATE "${_manifest_rc}")
endfunction()
