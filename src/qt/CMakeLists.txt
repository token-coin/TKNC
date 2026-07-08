﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿# Copyright (c) 2023-present The TKNC Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
  set(CMAKE_OBJCXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
  set(CMAKE_OBJCXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  set(CMAKE_OBJCXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL}")
  string(APPEND CMAKE_OBJCXX_COMPILE_OBJECT " ${APPEND_CPPFLAGS} ${APPEND_CXXFLAGS}")
endif()

if(MSVC)
  add_compile_options(/permissive- /Zc:twoPhase- /Zc:__cplusplus)
endif()

get_target_property(qt_lib_type Qt6::Core TYPE)

function(import_plugins target)
  if(qt_lib_type STREQUAL "STATIC_LIBRARY")
    set(plugins Qt6::QMinimalIntegrationPlugin)
    if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|FreeBSD)$")
      list(APPEND plugins Qt6::QXcbIntegrationPlugin)
    elseif(WIN32)
      list(APPEND plugins Qt6::QWindowsIntegrationPlugin Qt6::QModernWindowsStylePlugin)
    elseif(APPLE)
      list(APPEND plugins Qt6::QCocoaIntegrationPlugin Qt6::QMacStylePlugin)
    endif()
    qt6_import_plugins(${target}
      INCLUDE ${plugins}
      EXCLUDE_BY_TYPE
        accessiblebridge
        platforms
        platforms_darwin
        xcbglintegrations
        platformthemes
        platforminputcontexts
        generic
        iconengines
        imageformats
        egldeviceintegrations
        styles
        networkaccess
        networkinformation
        tls
    )
  endif()
endfunction()

# For Qt-specific commands and variables, please consult:
#  - https://cmake.org/cmake/help/latest/manual/cmake-qt.7.html
#  - https://doc.qt.io/qt-5/cmake-manual.html

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOMOC_MOC_OPTIONS "-p${CMAKE_CURRENT_SOURCE_DIR}")
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS forms)

set(CMAKE_AUTORCC OFF)
configure_file(tknc_locale.qrc tknc_locale.qrc USE_SOURCE_PERMISSIONS COPYONLY)
qt6_add_resources(TKN_QRC tknc.qrc)
qt6_add_resources(TKN_LOCALE_QRC ${CMAKE_CURRENT_BINARY_DIR}/tknc_locale.qrc)
# See: https://bugreports.qt.io/browse/QTBUG-141858.
get_target_property(warn_flags warn_interface INTERFACE_COMPILE_OPTIONS)
if("-Wtrailing-whitespace=any" IN_LIST warn_flags)
  set_source_files_properties(${TKN_QRC} ${TKN_LOCALE_QRC} PROPERTIES COMPILE_OPTIONS -Wno-trailing-whitespace)
endif()
unset(warn_flags)

# The tkncqt sources have to include headers in
# order to parse them to collect translatable strings.
add_library(tkncqt STATIC EXCLUDE_FROM_ALL
  bantablemodel.cpp
  bantablemodel.h
  tknc.cpp
  tknc.h
  tkncaddressvalidator.cpp
  tkncaddressvalidator.h
  tkncamountfield.cpp
  tkncamountfield.h
  tkncoingui.cpp
  tkncoingui.h
  tkncunits.cpp
  tkncunits.h
  clientmodel.cpp
  clientmodel.h
  csvmodelwriter.cpp
  csvmodelwriter.h
  freespacechecker.cpp
  freespacechecker.h
  guiutil.cpp
  guiutil.h
  initexecutor.cpp
  initexecutor.h
  intro.cpp
  intro.h
  $<$<PLATFORM_ID:Darwin>:macdockiconhandler.h>
  $<$<PLATFORM_ID:Darwin>:macdockiconhandler.mm>
  $<$<PLATFORM_ID:Darwin>:macnotificationhandler.h>
  $<$<PLATFORM_ID:Darwin>:macnotificationhandler.mm>
  $<$<PLATFORM_ID:Darwin>:macos_appnap.h>
  $<$<PLATFORM_ID:Darwin>:macos_appnap.mm>
  modaloverlay.cpp
  modaloverlay.h
  # TokenCoin custom pages
  tkncmainwindow.cpp
  tkncmainwindow.h
  walletpage.cpp
  walletpage.h
  miningpage.cpp
  miningpage.h
  modelpage.cpp
  modelpage.h
  reviewpage.cpp
  reviewpage.h
  resourcepage.cpp
  resourcepage.h
  apikeypage.cpp    # TKNC API Key management page
  apikeypage.h      # TKNC API Key management page
  networkstyle.cpp
  networkstyle.h
  notificator.cpp
  notificator.h
  optionsdialog.cpp
  optionsdialog.h
  optionsmodel.cpp
  optionsmodel.h
  peertablemodel.cpp
  peertablemodel.h
  peertablesortproxy.cpp
  peertablesortproxy.h
  platformstyle.cpp
  platformstyle.h
  qvalidatedlineedit.cpp
  qvalidatedlineedit.h
  qvaluecombobox.cpp
  qvaluecombobox.h
  rpcconsole.cpp
  rpcconsole.h
  splashscreen.cpp
  splashscreen.h
  trafficgraphwidget.cpp
  trafficgraphwidget.h
  utilitydialog.cpp
  utilitydialog.h
  $<$<PLATFORM_ID:Windows>:winshutdownmonitor.cpp>
  $<$<PLATFORM_ID:Windows>:winshutdownmonitor.h>
  ${TKN_QRC}
  ${TKN_LOCALE_QRC}
)
target_compile_definitions(tkncqt
  PUBLIC
    QT_NO_KEYWORDS
    QT_USE_QSTRINGBUILDER
)
target_include_directories(tkncqt
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
set_property(SOURCE macnotificationhandler.mm
  # Ignore warnings "'NSUserNotificationCenter' is deprecated: first deprecated in macOS 11.0".
  APPEND PROPERTY COMPILE_OPTIONS -Wno-deprecated-declarations
)
target_link_libraries(tkncqt
  PUBLIC
    Qt6::Widgets
  PRIVATE
    core_interface
    tknc_cli
    leveldb
    Boost::headers
    $<TARGET_NAME_IF_EXISTS:QRencode::QRencode>
    $<$<PLATFORM_ID:Darwin>:-framework\ AppKit>
    $<$<PLATFORM_ID:Windows>:shlwapi;ole32;uuid;ws2_32;iphlpapi>
)

if(ENABLE_WALLET)
  target_sources(tkncqt
    PRIVATE
      addressbookpage.cpp
      addressbookpage.h
      addresstablemodel.cpp
      addresstablemodel.h
      askpassphrasedialog.cpp
      askpassphrasedialog.h
      coincontroldialog.cpp
      coincontroldialog.h
      coincontroltreewidget.cpp
      coincontroltreewidget.h
      createwalletdialog.cpp
      createwalletdialog.h
      editaddressdialog.cpp
      editaddressdialog.h
      openuridialog.cpp
      openuridialog.h
      overviewpage.cpp
      overviewpage.h
      paymentserver.cpp
      paymentserver.h
      psbtoperationsdialog.cpp
      psbtoperationsdialog.h
      qrimagewidget.cpp
      qrimagewidget.h
      receivecoinsdialog.cpp
      receivecoinsdialog.h
      receiverequestdialog.cpp
      receiverequestdialog.h
      recentrequeststablemodel.cpp
      recentrequeststablemodel.h
      sendcoinsdialog.cpp
      sendcoinsdialog.h
      sendcoinsentry.cpp
      sendcoinsentry.h
      signverifymessagedialog.cpp
      signverifymessagedialog.h
      transactiondesc.cpp
      transactiondesc.h
      transactiondescdialog.cpp
      transactiondescdialog.h
      transactionfilterproxy.cpp
      transactionfilterproxy.h
      transactionoverviewwidget.cpp
      transactionoverviewwidget.h
      transactionrecord.cpp
      transactionrecord.h
      transactiontablemodel.cpp
      transactiontablemodel.h
      transactionview.cpp
      transactionview.h
      walletcontroller.cpp
      walletcontroller.h
      walletframe.cpp
      walletframe.h
      walletmodel.cpp
      walletmodel.h
      walletmodeltransaction.cpp
      walletmodeltransaction.h
      walletview.cpp
      walletview.h
  )
  target_link_libraries(tkncqt
    PRIVATE
      tknc_wallet
      Qt6::Network
  )
endif()

if(WITH_DBUS)
  target_link_libraries(tkncqt PRIVATE Qt6::DBus)
endif()

if(qt_lib_type STREQUAL "STATIC_LIBRARY")
  # We want to define static plugins to link ourselves, thus preventing
  # automatic linking against a "sane" set of default static plugins.
  qt6_import_plugins(tkncqt
    EXCLUDE_BY_TYPE
      accessiblebridge
      platforms
      platforms_darwin
      xcbglintegrations
      platformthemes
      platforminputcontexts
      generic
      iconengines
      imageformats
      egldeviceintegrations
      styles
      networkaccess
      networkinformation
      tls
  )
endif()

add_subdirectory(locale)

add_executable(tknc-qt
  main.cpp
  ../init/tknc-qt.cpp
)

add_windows_resources(tknc-qt res/tknc-qt-res.rc)
add_windows_application_manifest(tknc-qt)

target_link_libraries(tknc-qt
  core_interface
  tkncqt
  tknc_node
)

import_plugins(tknc-qt)
install_binary_component(tknc-qt HAS_MANPAGE)
if(WIN32)
  set_target_properties(tknc-qt PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(ENABLE_IPC)
  add_executable(tknc-gui
    main.cpp
    ../init/tknc-gui.cpp
  )
  target_link_libraries(tknc-gui
    core_interface
    tkncqt
    tknc_node
    tknc_ipc
  )
  import_plugins(tknc-gui)
  install_binary_component(tknc-gui INTERNAL)
  if(WIN32)
    set_target_properties(tknc-gui PROPERTIES WIN32_EXECUTABLE TRUE)
  endif()
endif()

if(BUILD_GUI_TESTS)
  add_subdirectory(test)
endif()

find_program(XGETTEXT_EXECUTABLE xgettext)
if(NOT XGETTEXT_EXECUTABLE)
  add_custom_target(translate
    COMMAND ${CMAKE_COMMAND} -E echo "Error: GNU gettext-tools not found"
  )
else()
  add_custom_target(translate COMMAND ${CMAKE_COMMAND}
    -D "PROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    -D "COPYRIGHT_HOLDERS=${COPYRIGHT_HOLDERS}"
    -D "LCONVERT_EXECUTABLE=$<TARGET_FILE:Qt6::lconvert>"
    -D "LUPDATE_EXECUTABLE=$<TARGET_FILE:Qt6::lupdate>"
    -D "XGETTEXT_EXECUTABLE=${XGETTEXT_EXECUTABLE}"
    -P ${PROJECT_SOURCE_DIR}/share/qt/translate.cmake
  )
endif()
