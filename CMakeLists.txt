# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)

# macOS: the oldest system the bundle runs on (the Qt it is built with supports macOS 13 and later).
# Only a default: it must be known before project(), and the packaging scripts pass it anyway.
if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
endif()

project(Sugo VERSION 1.0.4LANGUAGES CXX)

if(APPLE)
    # The few macOS-only sources are Objective-C++ (.mm, compiled with -fobjc-arc).
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(QT_MIN_VERSION 6.9.0)  # Qt::ExpandedClientAreaHint for the native title bar
else()
    set(QT_MIN_VERSION 6.8.0)  # QStyleHints::setColorScheme
endif()
set(ECM_MIN_VERSION 6.8.0)

find_package(ECM ${ECM_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(FeatureSummary)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
    # KDECompilerSettings sets the warnings for C++ only: the same ones for Objective-C++.
    add_compile_options("$<$<COMPILE_LANGUAGE:OBJCXX>:-Wall;-Wextra>")
endif()

# macOS: whether Sugo.app replaces itself with a new release (MacBundleInstaller). On only in the
# disk image's build (packaging/macos/build.sh); a build from source only reports new versions.
option(SUGO_INSTALLS_UPDATES "macOS: the app bundle installs updates itself (the disk image's build)" OFF)

# The version the program shows and compares when it looks for updates. Nightly builds set it to
# a pre-release of the coming version, "1.0.1-nightly.20261031.42": newer than 1.0.0, older than
# 1.0.1 (see .github/workflows/nightly.yml).
# Empty: the project version (kept out of the cache, so a version bump reaches existing builds).
set(SUGO_VERSION "" CACHE STRING "Version shown by the program and compared by the update check (empty: the project version)")
set(sugo_version "${SUGO_VERSION}")
if(sugo_version STREQUAL "")
    set(sugo_version "${PROJECT_VERSION}")
endif()

add_compile_definitions(
    PROJECT_VERSION="${sugo_version}"
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
    QT_NO_URL_CAST_FROM_STRING
    QT_USE_QSTRINGBUILDER
    QT_DISABLE_DEPRECATED_BEFORE=0x060800
)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Gui Widgets Qml Quick QuickControls2 Sql Network Multimedia)
# D-Bus (notifications, inhibition, KRunner, Secret Service, the single-instance name...) is the
# freedesktop desktop's: macOS uses its own services and does not ship QtDBus.
if(NOT APPLE)
    find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS DBus)
    set(SUGO_DBUS Qt6::DBus)
else()
    set(SUGO_DBUS "")
endif()

# Sugo Sync's encryption, the same library on every system (docs/sync-plan.md). 1.0.18 is the
# oldest the supported distributions ship.
find_package(PkgConfig REQUIRED)
pkg_check_modules(sodium REQUIRED IMPORTED_TARGET libsodium>=1.0.18)

# QML modules live under qrc:/qt/qml/ (QTP0001). Qt >= 6.8: one qmldir per QML subdirectory (QTP0004).
qt6_policy(SET QTP0001 NEW)
if(QT_KNOWN_POLICY_QTP0004)
    qt6_policy(SET QTP0004 NEW)
endif()
# The QML module's build output goes under its URI path, where Qt's tools (qmllint, qmlls) look for it.
set(QT_QML_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml)

add_subdirectory(src)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Linux desktop integration. The configured files stay on every system (tst_runner_version reads one).
# KRunner only looks for D-Bus runners in share/krunner/dbusplugins. The plugin version
# comes from project() so it can never fall behind a release.
configure_file(data/stream.sugo.app.runner.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/stream.sugo.app.runner.desktop @ONLY)
# D-Bus activation: KRunner or a `qdbus` call can start Sugo (hidden in the tray) on demand.
configure_file(data/stream.sugo.app.service.in ${CMAKE_CURRENT_BINARY_DIR}/stream.sugo.app.service @ONLY)
if(NOT APPLE)
    install(FILES data/stream.sugo.app.desktop DESTINATION ${KDE_INSTALL_APPDIR})
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/stream.sugo.app.runner.desktop DESTINATION ${KDE_INSTALL_DATADIR}/krunner/dbusplugins)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/stream.sugo.app.service DESTINATION ${KDE_INSTALL_DBUSSERVICEDIR})
    install(FILES data/stream.sugo.app.metainfo.xml DESTINATION ${KDE_INSTALL_METAINFODIR})
    install(FILES data/icons/hicolor/scalable/apps/stream.sugo.app.svg
            DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/scalable/apps)
endif()

feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)
