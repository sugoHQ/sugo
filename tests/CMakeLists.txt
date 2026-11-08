# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Test)
include(ECMAddTests)

# Each test links the program's code (sugo_core, see src/CMakeLists.txt), Qt Test and the Qt modules
# it uses itself; a test built on every system names Qt6::DBus as ${SUGO_DBUS}, empty on macOS.
# The modules decide what QTEST_MAIN creates: a QApplication with Qt6::Widgets, a
# QGuiApplication with Qt6::Gui (both need QT_QPA_PLATFORM=offscreen on CI), else a QCoreApplication.
function(sugo_add_test name)
    ecm_add_test(${name}.cpp TEST_NAME ${name} LINK_LIBRARIES Qt6::Test sugo_core ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

sugo_add_test(tst_timerengine Qt6::Core Qt6::Qml)
sugo_add_test(tst_focussoundcontroller Qt6::Core Qt6::Gui Qt6::Qml ${SUGO_DBUS} Qt6::Multimedia)
sugo_add_test(tst_datamanager Qt6::Core Qt6::Qml Qt6::Sql)
sugo_add_test(tst_presetmodel Qt6::Core Qt6::Qml)
sugo_add_test(tst_timerbinding Qt6::Core Qt6::Qml)
sugo_add_test(tst_database Qt6::Core Qt6::Qml Qt6::Sql)
sugo_add_test(tst_presetrepository Qt6::Core Qt6::Qml Qt6::Sql)
sugo_add_test(tst_sessions Qt6::Core Qt6::Qml Qt6::Sql)
sugo_add_test(tst_silence Qt6::Core Qt6::Qml)
sugo_add_test(tst_autostart Qt6::Core Qt6::Gui ${SUGO_DBUS})
sugo_add_test(tst_legacyappid Qt6::Core)
sugo_add_test(tst_appsettings Qt6::Core Qt6::Gui Qt6::Qml ${SUGO_DBUS})
sugo_add_test(tst_alertsound Qt6::Core Qt6::Multimedia)
sugo_add_test(tst_traypresenter Qt6::Core Qt6::Qml)
sugo_add_test(tst_statsaggregator Qt6::Core Qt6::Sql)
sugo_add_test(tst_statsmodel Qt6::Core Qt6::Qml Qt6::Sql)
sugo_add_test(tst_updatechecker Qt6::Core Qt6::Qml Qt6::Network)
sugo_add_test(tst_telemetry Qt6::Core Qt6::Qml Qt6::Network)
sugo_add_test(tst_screenlockwatcher Qt6::Core Qt6::Gui Qt6::Qml ${SUGO_DBUS})
sugo_add_test(tst_diagnostics Qt6::Core Qt6::Gui Qt6::Qml Qt6::Network Qt6::Sql ${SUGO_DBUS})
# The test builds a QGuiApplication (clipboard access): run it without a display, e.g. on CI.
set_tests_properties(tst_diagnostics PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

sugo_add_test(tst_calendarsync Qt6::Core Qt6::Gui Qt6::Qml Qt6::Network ${SUGO_DBUS})
set_tests_properties(tst_calendarsync PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
sugo_add_test(tst_askoncesecretstore Qt6::Core ${SUGO_DBUS})
sugo_add_test(tst_secrethandoff Qt6::Core)

sugo_add_test(tst_phasenotifier Qt6::Core Qt6::Gui Qt6::Qml ${SUGO_DBUS})
# A GUI application (AppSettings); no display on CI.
set_tests_properties(tst_phasenotifier PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

sugo_add_test(tst_traynotifier Qt6::Core Qt6::Gui Qt6::Widgets)
set_tests_properties(tst_traynotifier PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

sugo_add_test(tst_traycontroller Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml ${SUGO_DBUS})
set_tests_properties(tst_traycontroller PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

sugo_add_test(tst_desktopstyle Qt6::Core Qt6::Gui Qt6::Qml)
# A QGuiApplication (the system's fixed-width font); no display on CI.
set_tests_properties(tst_desktopstyle PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
sugo_add_test(tst_themeicons Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)
set_tests_properties(tst_themeicons PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

sugo_add_test(tst_localization Qt6::Core Qt6::Qml)
sugo_add_test(tst_unixsignals Qt6::Core)
sugo_add_test(tst_quitpolicy Qt6::Core)

# Sugo Sync: no D-Bus, the same tests on Linux and macOS.
sugo_add_test(tst_synccrypto Qt6::Core)
sugo_add_test(tst_presetsyncformat Qt6::Core Qt6::Qml)
sugo_add_test(tst_syncsecrets Qt6::Core)
sugo_add_test(tst_syncapi Qt6::Core Qt6::Network)
sugo_add_test(tst_syncaccount Qt6::Core Qt6::Network)
sugo_add_test(tst_synccontroller Qt6::Core Qt6::Qml Qt6::Network Qt6::Sql)

# The installed runner definition carries the project version (see the top-level CMakeLists.txt).
add_test(NAME tst_runner_version
    COMMAND grep -qx "X-KDE-PluginInfo-Version=${PROJECT_VERSION}" ${CMAKE_BINARY_DIR}/stream.sugo.app.runner.desktop
)
# KRunner skips a query shorter than the runner's minimum: it must let the bare keyword "sugo" through.
add_test(NAME tst_runner_min_letters
    COMMAND grep -qx "X-Plasma-Runner-Min-Letter-Count=[1-4]" ${CMAKE_BINARY_DIR}/stream.sugo.app.runner.desktop
)

# Linux only: the freedesktop services over D-Bus (notifications, inhibition, KRunner, Secret Service,
# the single-instance bus name and the /Timer service), and the shutdown test (D-Bus, GNU tools).
if(NOT APPLE)
    # macOS keeps the screen awake with IOKit power assertions (tst_macpowerassertion).
    sugo_add_test(tst_powerinhibitor Qt6::Core Qt6::Gui Qt6::Qml Qt6::DBus)
    # Gui: the factory it also checks names the inhibitor after the application's desktop file.
    sugo_add_test(tst_freedesktopinhibitor Qt6::Core Qt6::Gui Qt6::DBus)
    sugo_add_test(tst_singleinstance Qt6::Core Qt6::DBus)
    sugo_add_test(tst_krunnerservice Qt6::Core Qt6::Qml Qt6::DBus)
    sugo_add_test(tst_freedesktopnotifier Qt6::Core Qt6::DBus)
    sugo_add_test(tst_dbustimerservice Qt6::Core Qt6::Qml Qt6::DBus)
    sugo_add_test(tst_secretstore Qt6::Core Qt6::DBus)

    # Starts the program on a private bus, uses /Timer, quits with SIGTERM: the teardown must be clean.
    add_test(NAME tst_app_shutdown
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/shutdown_app.sh $<TARGET_FILE:sugo>
    )
    set_tests_properties(tst_app_shutdown PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)

    # They check the sources, the same on every system: Linux runs them.
    # Colours in the interface come from the theme (or TrayIconStyle), never from literals.
    add_test(NAME tst_qml_colours
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/check_qml_colours.sh ${CMAKE_SOURCE_DIR}/src/qml
    )

    add_test(NAME tst_qml_sizes
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/check_qml_sizes.sh ${CMAKE_SOURCE_DIR}/src/qml
    )
endif()

add_test(NAME tst_app_startup
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke_app.sh $<TARGET_FILE:sugo>
)
# smoke_app.sh stops the app itself after 8 s (and kills it 2 s later); the rest is the first start
# of a freshly built binary, which a macOS runner checks for several seconds (13.7 s in all once).
set_tests_properties(tst_app_startup PROPERTIES TIMEOUT 30)

add_test(NAME tst_stats_page_ui
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke_app.sh $<TARGET_FILE:sugo> stats
)
set_tests_properties(tst_stats_page_ui PROPERTIES TIMEOUT 15)

add_test(NAME tst_about_page_ui
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke_app.sh $<TARGET_FILE:sugo> about
)
set_tests_properties(tst_about_page_ui PROPERTIES TIMEOUT 15)

add_test(NAME tst_settings_page_ui
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke_app.sh $<TARGET_FILE:sugo> settings
)
set_tests_properties(tst_settings_page_ui PROPERTIES TIMEOUT 15)

add_test(NAME tst_presets_page_ui
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke_app.sh $<TARGET_FILE:sugo> presets
)
set_tests_properties(tst_presets_page_ui PROPERTIES TIMEOUT 15)

add_test(NAME tst_timer_page_ui
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke_app.sh $<TARGET_FILE:sugo> timer
)
set_tests_properties(tst_timer_page_ui PROPERTIES TIMEOUT 15)

# Started on a damaged sugo.db: it is moved aside, and the window says so.
add_test(NAME tst_damaged_database_ui
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke_app.sh $<TARGET_FILE:sugo> damaged-db
)
set_tests_properties(tst_damaged_database_ui PROPERTIES TIMEOUT 10)

# Started next to a database of Sugo 1.0.0 that 1.0.1 moved aside: its data is taken back, and the
# window says so. The script writes that database with Python's sqlite3.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME tst_sugo100_database_ui
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke_app.sh $<TARGET_FILE:sugo> sugo100-db ${Python3_EXECUTABLE}
    )
    set_tests_properties(tst_sugo100_database_ui PROPERTIES TIMEOUT 15)
endif()

add_test(NAME tst_welcome_page_ui
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke_app.sh $<TARGET_FILE:sugo> welcome
)
set_tests_properties(tst_welcome_page_ui PROPERTIES TIMEOUT 15)

# Ended with SIGTERM, Sugo quits cleanly by itself (on every system; without D-Bus).
add_test(NAME tst_app_quit
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke_app.sh $<TARGET_FILE:sugo> quit
)
set_tests_properties(tst_app_quit PROPERTIES TIMEOUT 25)

# --- macOS port: B1 menu bar icon, notifications, single instance, Dock ---
sugo_add_test(tst_localsingleinstance Qt6::Core Qt6::Network)
sugo_add_test(tst_macnotificationmapping Qt6::Core)

# --- macOS port: B2 (keep-awake, screen lock and sleep, Keychain, login item) ---
sugo_add_test(tst_launchagentautostart Qt6::Core ${SUGO_DBUS})
if(APPLE)
    sugo_add_test(tst_macpowerassertion Qt6::Core "-framework CoreFoundation" "-framework IOKit")
    sugo_add_test(tst_mackeychainstore Qt6::Core "-framework Security")
endif()

# --- macOS port: B3 sounds ---
# The sounds bundled for macOS (data/sounds), checked against the names the settings page offers.
target_compile_definitions(tst_appsettings PRIVATE SUGO_BUNDLED_SOUNDS_ROOT="${PROJECT_SOURCE_DIR}/data")

# --- macOS port: W2-B Dock integration and fallback icons ---
sugo_add_test(tst_dockcontroller Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml ${SUGO_DBUS})
set_tests_properties(tst_dockcontroller PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# --- macOS port: W2-A (Focus through Shortcuts, sugo:// links) ---
# Both run on Linux too: the Focus with a fake `shortcuts` script (tests/FakeShortcuts.h).
sugo_add_test(tst_shortcutsfocusinhibitor Qt6::Core)
sugo_add_test(tst_timercommands Qt6::Core Qt6::Qml)

# --- macOS port: W4 install the Focus shortcuts ---
# Runs on Linux too, with fake shortcuts/open scripts (tests/FakeShortcutTools.h); also checks
# the shipped files in data/shortcuts.
sugo_add_test(tst_shortcutsfocusinstaller Qt6::Core)
target_compile_definitions(tst_shortcutsfocusinstaller PRIVATE SUGO_SHORTCUTS_SOURCE_DIR="${PROJECT_SOURCE_DIR}/data/shortcuts")

# These use QStandardPaths' test mode, whose folders (~/.qttest) every test process shares, and
# write the default settings file there: `ctest --parallel` runs them one at a time.
set_tests_properties(tst_alertsound tst_appsettings tst_calendarsync tst_desktopstyle tst_phasenotifier tst_screenlockwatcher
    PROPERTIES RESOURCE_LOCK qt_test_mode_dirs)

# --- Platform parity ---
# Every screen offers the same on Linux and macOS, but for the differences listed with their
# reasons in platform-parity.json; each platform feature has its tests here. Runs the program in
# the smoke mode "inventory". Registered last: it checks the tests above are all there.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    get_property(_sugo_registered_tests DIRECTORY PROPERTY TESTS)
    list(APPEND _sugo_registered_tests tst_platform_parity)
    list(JOIN _sugo_registered_tests "\n" _sugo_registered_tests_text)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/registered-tests.txt "${_sugo_registered_tests_text}\n")
    add_test(NAME tst_platform_parity
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/check_platform_parity.py
                $<TARGET_FILE:sugo> ${CMAKE_CURRENT_SOURCE_DIR}/platform-parity.json
                ${CMAKE_CURRENT_BINARY_DIR}/registered-tests.txt
    )
    set_tests_properties(tst_platform_parity PROPERTIES TIMEOUT 90)
endif()
