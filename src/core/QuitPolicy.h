// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * Whether a request to quit the whole application may go through at once or must first ask
 * "Quit Sugo?", as the window does for its own Quit.
 *
 * On macOS every way of quitting ends in Cocoa's applicationShouldTerminate:, which Qt turns
 * into QEvent::Quit (main.cpp, MacApplicationEvents): Sugo's own QCoreApplication::quit() too.
 * What the request came from is told apart by the Apple event being handled (macCurrentQuitOrigin()
 * in MacApp.h). Plain C++, so that the decision is tested on every system.
 */
namespace QuitPolicy
{
enum class Origin {
    /// Sugo's own quit: QCoreApplication::quit() after the window asked (or had nothing to ask),
    /// a signal (`kill`), a menu item. Nothing is left to ask.
    Application,
    /// Another program asked Sugo to quit: Quit in the Dock menu, Activity Monitor's Quit, an
    /// AppleScript `quit`. The user may not know a phase is running.
    External,
    /// Logging out, restarting or shutting down. Refusing would cancel the logout, so it is let
    /// through and the running phase is saved as interrupted, as on Linux.
    SessionEnding,
};

/// True when the request must be refused and turned into the window's question: an external
/// request while a phase runs or is paused, or while sessions or timer changes could not be
/// saved yet (DataManager::saveFailing()), which quitting would lose.
bool needsConfirmation(Origin origin, bool phaseActive, bool unsavedData);
} // namespace QuitPolicy
