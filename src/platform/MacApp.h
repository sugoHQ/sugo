// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtGlobal>

#ifdef Q_OS_MACOS
#include <QString>

/// Brings the application to the front, as a click on its Dock icon does. QWindow::requestActivate()
/// only makes a window key inside the application: when another one is active, the window
/// would stay behind it.
void macActivateApp();

/// Sets the text of the badge on the Dock icon; an empty text removes the badge.
void macSetDockBadge(const QString &label);

/// Hides the Dock icon (an accessory application: no Dock icon, no menu bar of its own) or shows
/// it again (a regular application). Showing it again does not activate the application.
void macSetDockIconHidden(bool hidden);

#include "QuitPolicy.h"

/// Where the request to quit being handled comes from, told by the Apple event Cocoa is handling
/// (call it while applicationShouldTerminate: runs, i.e. from the QEvent::Quit Qt sends then):
/// a `quit` event (the Dock's Quit, Activity Monitor, AppleScript) is External, or SessionEnding
/// when it carries a reason (kAEQuitReason: logging out, restarting, shutting down); no Apple
/// event means Sugo's own QCoreApplication::quit() (Application).
QuitPolicy::Origin macCurrentQuitOrigin();

class QWindow;
/// Fits the system's transparent title bar to Sugo's own: hides the title text it draws (the
/// title bar of the window already shows the name; the title still names the window in the
/// Window menu and Mission Control) and gives the window an empty toolbar in the unified compact
/// style, so the strip of the traffic lights is as tall as a compact toolbar and the lights sit
/// in its middle, which the title bar takes as its height (its safe area). Does nothing without
/// the cocoa platform (offscreen runs).
void macSetUpTitleBar(QWindow *window);

/// Stops Qt from reporting clicks on the menu bar icon (QSystemTrayIcon::activated, which Sugo
/// does not use on macOS). Qt 6.10 reads the click from NSApp.currentEvent when the icon's menu
/// starts tracking; on macOS 27 the menu opens from a deferred step and that event is often not a
/// mouse event, so asking for its click count throws: the menu never opens, and the icon stays
/// highlighted and ignores clicks for a while. Qt fixed it after 6.11.1 (qt_mac_isMouseEvent in
/// QCocoaSystemTrayIcon::emitActivated). Call it after the icon is shown with its menu. Does
/// nothing without the cocoa platform.
void macStopTrayActivationReports();
#endif
