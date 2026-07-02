# Copyright (c) 2023-present The TKNC Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# install_binary_component(target [HAS_MANPAGE] [HAS_MANIFEST] [INTERNAL])
#
# Installs the executable target to ${CMAKE_INSTALL_BINDIR}.
# Optionally installs a manpage if HAS_MANPAGE is given.
function(install_binary_component target)
    set(options HAS_MANPAGE HAS_MANIFEST INTERNAL)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "${options}" "" "")

    if(ARG_INTERNAL)
        set(_component Internal)
    else()
        set(_component Bin)
    endif()

    install(TARGETS ${target}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT ${_component}
    )

    if(ARG_HAS_MANPAGE)
        # Look for a manpage doc/man/<target>.1
        set(_manpage "${CMAKE_SOURCE_DIR}/doc/man/${target}.1")
        if(EXISTS "${_manpage}")
            install(FILES "${_manpage}"
                DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
                COMPONENT ${_component}
            )
        endif()
    endif()
endfunction()
