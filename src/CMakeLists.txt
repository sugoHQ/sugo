# SPDX-License-Identifier: GPL-3.0-or-later

# All the C++ code but main(), compiled once: the program and the tests link it. It is also the QML
# module; its static plugin (sugo_qml) is linked into the program and imported in main.cpp.
qt6_add_library(sugo_core STATIC)

set_source_files_properties(qml/theme/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt6_add_qml_module(sugo_core
    URI "stream.sugo.app"
    VERSION 1.0
    PLUGIN_TARGET sugo_qml
    CLASS_NAME SugoQmlPlugin
    QML_FILES
        qml/Main.qml
        qml/pages/TimerPage.qml
        qml/pages/StatsPage.qml
        qml/pages/PresetEditPage.qml
        qml/pages/AppSettingsPage.qml
        qml/pages/WelcomePage.qml
        qml/components/ProgressRing.qml
        qml/theme/Theme.qml
        qml/components/LineIcon.qml
        qml/components/SButton.qml
        qml/components/Toast.qml
        qml/components/TimerIcon.qml
        qml/components/TrayBadge.qml
        qml/components/TimerMenu.qml
        qml/components/TitleBar.qml
        qml/components/ConfirmDialog.qml
        qml/components/Banner.qml
        qml/components/MessageSlot.qml
        qml/components/FeatureInventory.qml
        qml/components/FocusShortcutsStatus.qml
        qml/components/FormSection.qml
        qml/components/FormRow.qml
        qml/components/SSwitch.qml
        qml/components/STextField.qml
        qml/components/SComboBox.qml
        qml/components/AutoStartMenu.qml
        qml/components/CommandPalette.qml
        qml/components/Composer.qml
        qml/components/ExtendMenu.qml
        qml/components/SSlider.qml
        qml/components/Segmented.qml
        qml/components/NumberField.qml
        qml/components/SMenu.qml
        qml/components/SMenuHeader.qml
        qml/components/SMenuItem.qml
        qml/components/SMenuSeparator.qml
        qml/components/SettingsNav.qml
        qml/components/AccentButton.qml
        qml/components/StatusLine.qml
        qml/components/BrandGlow.qml
        qml/components/LogoIntro.qml
        qml/components/WindowButtons.qml
        qml/components/ThemeIcon.qml
)

# C++ types exposed to QML (QML_ELEMENT / QML_SINGLETON) live in core/.
target_sources(sugo_core PRIVATE
    core/Clock.h
    core/TimerEngine.h
    core/TimerEngine.cpp
    core/TimerPreset.h
    core/PresetSettings.h
    core/PresetSettings.cpp
    core/Localization.h
    core/Localization.cpp
    core/Settings.h
    core/Settings.cpp
    core/PresetModel.h
    core/PresetModel.cpp
    core/TimerBinding.h
    core/TimerBinding.cpp
    core/PhaseNotifier.h
    core/PhaseNotifier.cpp
    core/FocusSoundController.h
    core/FocusSoundController.cpp
    core/DataManager.h
    core/DataManager.cpp
    core/SilenceController.h
    core/SilenceController.cpp
    core/AppSettings.h
    core/AppSettings.cpp
    core/TrayPresenter.h
    core/TrayPresenter.cpp
    core/TrayController.h
    core/TrayController.cpp
    core/TrayIconStyle.h
    core/UpdateChecker.h
    core/UpdateChecker.cpp
    core/UpdateSignature.h
    core/UpdateSignature.cpp
    core/Telemetry.h
    core/Telemetry.cpp
    core/TelemetryInfo.h
    core/TelemetryInfo.cpp
    core/Diagnostics.h
    core/Diagnostics.cpp
    core/SmokeInput.h
    core/SmokeInput.cpp
    core/RetryAfter.h
    core/UrlPolicy.h
    core/QuitPolicy.h
    core/QuitPolicy.cpp
    core/CalendarSync.h
    core/CalendarSync.cpp
    platform/NotificationInhibitor.h
    platform/NotificationInhibitor.cpp
    platform/PowerInhibitor.h
    platform/PowerInhibitor.cpp
    platform/NoopInhibitor.h
    platform/InhibitorFactory.h
    platform/InhibitorFactory.cpp
    platform/Notifier.h
    platform/TrayNotifier.h
    platform/TrayNotifier.cpp
    platform/NotifierFactory.h
    platform/NotifierFactory.cpp
    platform/UnixSignals.h
    platform/UnixSignals.cpp
    platform/Autostart.h
    platform/Autostart.cpp
    platform/LegacyAppId.h
    platform/LegacyAppId.cpp
    platform/SystemTray.h
    platform/SystemTray.cpp
    platform/ScreenLockWatcher.h
    platform/ScreenLockWatcher.cpp
    platform/SecretStore.h
    platform/SecretStore.cpp
    platform/SecretHandoff.h
    platform/SecretHandoff.cpp
    platform/DesktopStyle.h
    platform/DesktopStyle.cpp
    platform/AppImage.h
    platform/AppImage.cpp
    platform/ThemeIcons.h
    platform/ThemeIcons.cpp
    db/Database.h
    db/Database.cpp
    db/PresetRepository.h
    db/PresetRepository.cpp
    db/PresetPersistence.h
    db/PresetPersistence.cpp
    db/SessionRepository.h
    db/SessionRepository.cpp
    db/SessionRecorder.h
    db/SessionRecorder.cpp
    stats/StatsTypes.h
    stats/StatsAggregator.h
    stats/StatsAggregator.cpp
    stats/StatsModel.h
    stats/StatsModel.cpp
    # Sugo Sync: the same code on every system (docs/sync-plan.md).
    sync/PresetSyncFormat.h
    sync/PresetSyncFormat.cpp
    sync/SyncCrypto.h
    sync/SyncCrypto.cpp
    sync/SyncSecrets.h
    sync/SyncSecrets.cpp
    sync/SyncApi.h
    sync/SyncApi.cpp
    sync/SyncAccount.h
    sync/SyncAccount.cpp
    sync/SyncController.h
    sync/SyncController.cpp
)
target_include_directories(sugo_core PUBLIC core db platform stats sync)
# Qt's resource and QML cache objects share these include paths but may compile before moc has
# created them; -Wmissing-include-dirs would warn in a clean parallel build.
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/sugo_core_autogen/include ${CMAKE_CURRENT_BINARY_DIR}/sugo_qml_autogen/include)

# The public key that checks the signature of updates (core/UpdateSignature.h): base64 in one file.
# Empty (as long as no signing key exists) means that nothing is checked.
set(SUGO_UPDATE_KEY_FILE "${PROJECT_SOURCE_DIR}/packaging/update-public-key.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SUGO_UPDATE_KEY_FILE}")
set(SUGO_UPDATE_PUBLIC_KEY "")
if(EXISTS "${SUGO_UPDATE_KEY_FILE}")
    file(READ "${SUGO_UPDATE_KEY_FILE}" SUGO_UPDATE_PUBLIC_KEY)
    string(STRIP "${SUGO_UPDATE_PUBLIC_KEY}" SUGO_UPDATE_PUBLIC_KEY)
endif()
if(SUGO_UPDATE_PUBLIC_KEY AND NOT SUGO_UPDATE_PUBLIC_KEY MATCHES "^[A-Za-z0-9+/]+=*$")
    message(FATAL_ERROR "packaging/update-public-key.txt must hold only the base64 of the public key")
endif()
if(SUGO_UPDATE_PUBLIC_KEY)
    target_compile_definitions(sugo_core PRIVATE SUGO_UPDATE_PUBLIC_KEY="${SUGO_UPDATE_PUBLIC_KEY}")
endif()

# Private: what a test may use of Qt is up to the test (see tests/CMakeLists.txt).
target_link_libraries(sugo_core PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Sql
    ${SUGO_DBUS}
    Qt6::Network
    Qt6::Multimedia
    PkgConfig::sodium
)

set(sugo_app_sources main.cpp)
if(APPLE)
    # Sugo.app: the icon (Contents/Resources/Sugo.icns, from packaging/macos/render-icons.sh's PNGs)
    # and an Info.plist whose version is only X.Y.Z (nightly suffixes are not valid there).
    include(ECMAddAppIcon)
    file(GLOB sugo_macos_icons "${PROJECT_SOURCE_DIR}/data/icons/macos/*-apps-stream.sugo.app.png")
    if(sugo_macos_icons)
        ecm_add_app_icon(sugo_app_sources ICONS ${sugo_macos_icons} OUTFILE_BASENAME Sugo)
    endif()
    string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" SUGO_BUNDLE_VERSION "${sugo_version}")
    if(NOT SUGO_BUNDLE_VERSION)
        set(SUGO_BUNDLE_VERSION "${PROJECT_VERSION}")
    endif()
    set(SUGO_BUNDLE_EXECUTABLE Sugo)
    # <true/> or <false/> for the key SugoInstallsUpdates (MacBundleInstaller::installsUpdates()).
    if(SUGO_INSTALLS_UPDATES)
        set(SUGO_BUNDLE_INSTALLS_UPDATES true)
    else()
        set(SUGO_BUNDLE_INSTALLS_UPDATES false)
    endif()
    configure_file("${PROJECT_SOURCE_DIR}/packaging/macos/Info.plist.in" "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
endif()

# A plain program on Linux; Sugo.app on macOS (MACOSX_BUNDLE is ignored elsewhere).
add_executable(sugo MACOSX_BUNDLE ${sugo_app_sources})
if(APPLE)
    set_target_properties(sugo PROPERTIES
        OUTPUT_NAME Sugo
        MACOSX_BUNDLE_GUI_IDENTIFIER stream.sugo.app
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    )
endif()

# The timer icons, shown through PresetModel::iconSource() as qrc:/icons/<name>.svg.
qt6_add_resources(sugo "preseticons"
    PREFIX "/icons"
    BASE "${PROJECT_SOURCE_DIR}/data/icons/presets"
    FILES
        ${PROJECT_SOURCE_DIR}/data/icons/presets/sugo-break.svg
        ${PROJECT_SOURCE_DIR}/data/icons/presets/sugo-coding.svg
        ${PROJECT_SOURCE_DIR}/data/icons/presets/sugo-favorite.svg
        ${PROJECT_SOURCE_DIR}/data/icons/presets/sugo-focus.svg
        ${PROJECT_SOURCE_DIR}/data/icons/presets/sugo-goal.svg
        ${PROJECT_SOURCE_DIR}/data/icons/presets/sugo-research.svg
        ${PROJECT_SOURCE_DIR}/data/icons/presets/sugo-sprint.svg
        ${PROJECT_SOURCE_DIR}/data/icons/presets/sugo-study.svg
        ${PROJECT_SOURCE_DIR}/data/icons/presets/sugo-tasks.svg
        ${PROJECT_SOURCE_DIR}/data/icons/presets/sugo-timer.svg
        ${PROJECT_SOURCE_DIR}/data/icons/presets/sugo-tomato.svg
        ${PROJECT_SOURCE_DIR}/data/icons/presets/sugo-writing.svg
)

# Embedded fallback so the icon also shows when running from the build tree.
qt6_add_resources(sugo "appicon"
    PREFIX "/icons"
    BASE "${CMAKE_SOURCE_DIR}/data/icons/hicolor/scalable/apps"
    FILES "${CMAKE_SOURCE_DIR}/data/icons/hicolor/scalable/apps/stream.sugo.app.svg"
)

# The QML plugin first, then the library it links privately: listed the other way round, CMake repeats
# libsugo_core.a after the plugin to keep the static link order (Apple's ld warns about duplicates).
target_link_libraries(sugo PRIVATE
    sugo_qml
    sugo_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Sql
    ${SUGO_DBUS}
    Qt6::Network
)

if(APPLE)
    # KDE_INSTALL_TARGETS_DEFAULT_ARGS would put the bundle in /Applications/KDE (absolute).
    install(TARGETS sugo BUNDLE DESTINATION . RUNTIME DESTINATION ${KDE_INSTALL_BINDIR})
else()
    install(TARGETS sugo ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
endif()

# --- macOS port: B1 menu bar icon, notifications, single instance, Dock ---
target_sources(sugo_core PRIVATE
    platform/MacNotificationMapping.h
    platform/MacNotificationMapping.cpp
    platform/LocalSingleInstance.h
    platform/LocalSingleInstance.cpp
)
# The menu bar icon while idle, a template image for macOS (see TrayController, which the tests
# use too: so in the library, not the program).
qt6_add_resources(sugo_core "trayicon"
    PREFIX "/icons"
    BASE "${PROJECT_SOURCE_DIR}/data/icons/tray"
    FILES "${PROJECT_SOURCE_DIR}/data/icons/tray/sugo-tray-template.svg"
)
if(APPLE)
    target_sources(sugo_core PRIVATE
        platform/MacNotifier.h
        platform/MacNotifier.mm
        platform/MacApp.h
        platform/MacApp.mm
    )
    set_source_files_properties(platform/MacNotifier.mm platform/MacApp.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(sugo_core PRIVATE "-framework Foundation" "-framework AppKit" "-framework UserNotifications")
endif()

# --- macOS port: B2 (keep-awake, screen lock and sleep, Keychain, login item) ---
target_sources(sugo_core PRIVATE
    platform/LaunchAgentAutostart.cpp platform/LaunchAgentAutostart.h
)
if(APPLE)
    target_sources(sugo_core PRIVATE
        platform/MacPowerAssertion.cpp platform/MacPowerAssertion.h
        platform/MacSessionEvents.cpp platform/MacSessionEvents.h
        platform/MacKeychainStore.cpp platform/MacKeychainStore.h
    )
    target_link_libraries(sugo_core PRIVATE
        "-framework CoreFoundation"
        "-framework IOKit"
        "-framework Security"
    )
endif()

# --- macOS port: B3 sounds ---
# The alert sounds the settings page offers, for macOS, which has no freedesktop sound themes:
# copied into Sugo.app/Contents/Resources/sounds/<theme>/stereo/ (see AppSettings::findSoundFile).
if(APPLE)
    set(SUGO_BUNDLED_SOUNDS
        ocean/stereo/alarm-clock-elapsed.oga
        ocean/stereo/bell.oga
        ocean/stereo/completion-success.oga
        ocean/stereo/dialog-information.oga
        ocean/stereo/message-new-instant.oga
        ocean/stereo/phone-incoming-call.oga
        ocean/stereo/service-login.oga
        freedesktop/stereo/complete.oga
        freedesktop/stereo/message.oga
        freedesktop/stereo/window-attention.oga
    )
    foreach(sound IN LISTS SUGO_BUNDLED_SOUNDS)
        set(sound_file "${PROJECT_SOURCE_DIR}/data/sounds/${sound}")
        get_filename_component(sound_dir "${sound}" DIRECTORY)
        set_source_files_properties("${sound_file}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/sounds/${sound_dir}")
        target_sources(sugo PRIVATE "${sound_file}")
    endforeach()
endif()

# --- macOS port: C (window chrome, menus, shortcuts, texts) ---
# The macOS menu bar (Qt.labs.platform). A plain resource next to the module's QML, not one of its
# QML_FILES: Main.qml loads it by URL only on macOS, so Linux neither compiles nor imports it.
qt_add_resources(sugo_core "sugo_macos_menu"
    PREFIX "/qt/qml/stream/sugo/app"
    FILES qml/components/MacMenuBar.qml
)

# --- macOS port: W2-B Dock integration and fallback icons ---
target_sources(sugo_core PRIVATE
    core/TimerMenuActions.h core/TimerMenuActions.cpp
    core/DockController.h core/DockController.cpp
)
# The theme icons the menus use, from Breeze, for platforms without an icon theme (see
# data/icons/sugo-fallback/README.md and ThemeIcons::installFallbackTheme()).
set(sugo_fallback_icon_names
    application-exit chronometer configure media-playback-pause media-playback-start
    media-playback-stop media-skip-forward office-chart-bar
)
set(sugo_fallback_icons "${PROJECT_SOURCE_DIR}/data/icons/sugo-fallback/index.theme")
foreach(size 16 22)
    foreach(name IN LISTS sugo_fallback_icon_names)
        list(APPEND sugo_fallback_icons "${PROJECT_SOURCE_DIR}/data/icons/sugo-fallback/actions/${size}/${name}.svg")
    endforeach()
endforeach()
qt6_add_resources(sugo_core "fallbackicons"
    PREFIX "/icons/sugo-fallback"
    BASE "${PROJECT_SOURCE_DIR}/data/icons/sugo-fallback"
    FILES ${sugo_fallback_icons}
)

# --- macOS port: W2-A (Focus through Shortcuts, sugo:// links) ---
# Plain Qt, compiled everywhere: the Linux tests drive them with a fake `shortcuts` program.
target_sources(sugo_core PRIVATE
    core/TimerCommands.cpp core/TimerCommands.h
    platform/ShortcutsFocusInhibitor.cpp platform/ShortcutsFocusInhibitor.h
)

# --- macOS port: W4 install the Focus shortcuts ---
target_sources(sugo_core PRIVATE
    platform/ShortcutsFocusInstaller.cpp platform/ShortcutsFocusInstaller.h
)

# --- macOS port: W2-C self-update from the disk image ---
# Built everywhere: the macOS install steps are QProcess calls, tested on Linux with stand-ins.
target_sources(sugo_core PRIVATE
    core/UpdateInstaller.h
    core/UpdateInstaller.cpp
    platform/MacBundle.h
    platform/MacBundle.cpp
)

# --- macOS port: W3 QtDBus only on Linux ---
# The freedesktop services over D-Bus: notifications, notification and screen-sleep inhibition,
# the single-instance bus name, /Timer, KRunner, the Background portal and the Secret Service.
# PowerInhibitor, ScreenLockWatcher, AppSettings and the factories keep their D-Bus code under
# #ifndef Q_OS_MACOS / #ifdef Q_OS_LINUX, built everywhere.
if(NOT APPLE)
    target_sources(sugo_core PRIVATE
        platform/FreedesktopInhibitor.h
        platform/FreedesktopInhibitor.cpp
        platform/FreedesktopNotifier.h
        platform/FreedesktopNotifier.cpp
        platform/SingleInstance.h
        platform/SingleInstance.cpp
        platform/DBusTimerService.h
        platform/DBusTimerService.cpp
        platform/KRunnerService.h
        platform/KRunnerService.cpp
        platform/PortalAutostart.h
        platform/PortalAutostart.cpp
        platform/SecretServiceStore.h
        platform/SecretServiceStore.cpp
    )
endif()

# --- macOS port: W5 shipped Focus shortcuts ---
# The two Apple-signed shortcuts ShortcutsFocusInstaller opens, in Sugo.app/Contents/Resources/shortcuts/.
if(APPLE)
    foreach(shortcut IN ITEMS "Sugo Focus On.shortcut" "Sugo Focus Off.shortcut")
        set(shortcut_file "${PROJECT_SOURCE_DIR}/data/shortcuts/${shortcut}")
        set_source_files_properties("${shortcut_file}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/shortcuts")
        target_sources(sugo PRIVATE "${shortcut_file}")
    endforeach()
endif()

# --- macOS: sign the bundle built here ---
# The linker signs only the binary, ad hoc and under the name "Sugo"; macOS then refuses the
# notification permission (it goes by the bundle's identifier). Signing the whole bundle ad hoc,
# as packaging/macos/build.sh does for the disk image, makes a build tried from here behave like
# the installed app.
if(APPLE)
    add_custom_command(TARGET sugo POST_BUILD
        COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:sugo>"
        COMMENT "Signing Sugo.app ad hoc"
        VERBATIM
    )
endif()
