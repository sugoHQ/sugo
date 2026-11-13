// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Qt.labs.platform as Platform
import stream.sugo.app

/**
 * The menu bar of macOS, created only there (Main.qml loads it when DesktopStyle.nativeChrome).
 * Qt.labs.platform rather than Qt Quick Controls' MenuBar: its items take a role, which puts
 * About, Settings… and Quit in the application menu ("Sugo"). Every other item has NoRole, or
 * Qt would move "About" and "Settings" there by their text.
 *
 * Its shortcuts replace the window's Shortcut items, which Main.qml turns off on macOS, so no
 * key is claimed twice.
 */
Item {
    id: menus

    /// The application window (Main.qml): navigation, new timer, quit.
    property var app: null
    readonly property bool navigable: !!app && !app.isWelcomeActive && !app.dialogOpen

    Platform.MenuBar {
        window: menus.app

        Platform.Menu {
            title: i18nc("@title:menu", "Timer")

            // Moved by Qt into the application menu.
            Platform.MenuItem {
                text: i18nc("@action:inmenu", "About Sugo")
                role: Platform.MenuItem.AboutRole
                enabled: menus.navigable
                onTriggered: menus.app.navigate("about")
            }
            Platform.MenuItem {
                text: i18nc("@action:inmenu", "Settings…")
                role: Platform.MenuItem.PreferencesRole
                shortcut: StandardKey.Preferences
                enabled: menus.navigable
                onTriggered: menus.app.navigate("settings")
            }
            Platform.MenuItem {
                text: i18nc("@action:inmenu", "Quit Sugo")
                role: Platform.MenuItem.QuitRole
                shortcut: StandardKey.Quit
                onTriggered: menus.app.requestQuit()
            }

            Platform.MenuItem {
                text: TimerEngine.paused ? i18nc("@action:inmenu", "Resume")
                    : TimerEngine.active ? i18nc("@action:inmenu", "Pause") : i18nc("@action:inmenu", "Start")
                role: Platform.MenuItem.NoRole
                enabled: !!menus.app && !menus.app.dialogOpen
                onTriggered: TimerEngine.toggle()
            }
            Platform.MenuItem {
                text: i18nc("@action:inmenu", "Skip to the Next Phase")
                role: Platform.MenuItem.NoRole
                enabled: TimerEngine.active && !!menus.app && !menus.app.dialogOpen
                onTriggered: menus.app.requestEndPhase(true)
            }
            Platform.MenuItem {
                text: i18nc("@action:inmenu", "Stop")
                role: Platform.MenuItem.NoRole
                enabled: TimerEngine.active && !!menus.app && !menus.app.dialogOpen
                onTriggered: menus.app.requestEndPhase(false)
            }
            Platform.Menu {
                title: i18nc("@title:menu", "Add Time")
                enabled: TimerEngine.extendable && !!menus.app && !menus.app.dialogOpen
                Platform.MenuItem {
                    text: i18ncp("@action:inmenu", "%1 Minute", "%1 Minutes", 2)
                    role: Platform.MenuItem.NoRole
                    onTriggered: TimerEngine.extend(2 * 60, TimerEngine.ExtendSource.Menu)
                }
                Platform.MenuItem {
                    text: i18ncp("@action:inmenu", "%1 Minute", "%1 Minutes", 5)
                    role: Platform.MenuItem.NoRole
                    onTriggered: TimerEngine.extend(5 * 60, TimerEngine.ExtendSource.Menu)
                }
                Platform.MenuItem {
                    text: i18ncp("@action:inmenu", "%1 Minute", "%1 Minutes", 10)
                    role: Platform.MenuItem.NoRole
                    onTriggered: TimerEngine.extend(10 * 60, TimerEngine.ExtendSource.Menu)
                }
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: i18nc("@action:inmenu", "New Timer")
                role: Platform.MenuItem.NoRole
                shortcut: StandardKey.New
                enabled: menus.navigable
                onTriggered: menus.app.newTimer()
            }
        }

        Platform.Menu {
            title: i18nc("@title:menu", "View")

            Platform.MenuItem {
                text: i18nc("@action:inmenu", "Focus")
                role: Platform.MenuItem.NoRole
                shortcut: "Ctrl+1"
                enabled: menus.navigable
                onTriggered: menus.app.navigate("timer")
            }
            Platform.MenuItem {
                text: i18nc("@action:inmenu", "Statistics")
                role: Platform.MenuItem.NoRole
                shortcut: "Ctrl+2"
                enabled: menus.navigable
                onTriggered: menus.app.navigate("stats")
            }
            Platform.MenuItem {
                text: i18nc("@action:inmenu", "Settings")
                role: Platform.MenuItem.NoRole
                shortcut: "Ctrl+3"
                enabled: menus.navigable
                onTriggered: menus.app.navigate("settings")
            }
            Platform.MenuItem {
                text: i18nc("@action:inmenu", "About")
                role: Platform.MenuItem.NoRole
                shortcut: "Ctrl+4"
                enabled: menus.navigable
                onTriggered: menus.app.navigate("about")
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: i18nc("@action:inmenu", "Search…")
                role: Platform.MenuItem.NoRole
                shortcut: "Ctrl+K"
                enabled: menus.navigable
                onTriggered: menus.app.openSearch()
            }
        }

        Platform.Menu {
            title: i18nc("@title:menu", "Window")

            Platform.MenuItem {
                text: i18nc("@action:inmenu", "Minimize")
                role: Platform.MenuItem.NoRole
                shortcut: "Ctrl+M"
                enabled: !!menus.app && menus.app.visible
                onTriggered: menus.app.showMinimized()
            }
            Platform.MenuItem {
                text: i18nc("@action:inmenu", "Zoom")
                role: Platform.MenuItem.NoRole
                enabled: !!menus.app && menus.app.visible
                onTriggered: menus.app.maximized ? menus.app.showNormal() : menus.app.showMaximized()
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                // Hides the window; Sugo keeps running in the menu bar and the Dock.
                text: i18nc("@action:inmenu", "Close Window")
                role: Platform.MenuItem.NoRole
                shortcut: StandardKey.Close
                enabled: !!menus.app && menus.app.visible
                onTriggered: menus.app.close()
            }
            Platform.MenuItem {
                text: i18nc("@action:inmenu", "Sugo")
                role: Platform.MenuItem.NoRole
                // Also out of the Dock: show() and raise() leave a minimised window there.
                onTriggered: menus.app.bringToFront()
            }
        }
    }
}
