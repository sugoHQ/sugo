// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import stream.sugo.app

/**
 * macOS: what the Focus row of the settings and of the welcome tour says. It covers whether the
 * two Focus shortcuts are there and where "Install shortcuts" (AppSettings.installFocusShortcuts())
 * got. Every text it can show is in descriptionVariants(), so the row can keep the height of the
 * tallest one.
 */
QtObject {
    id: status

    /// Both Focus shortcuts are among the user's shortcuts.
    property bool found: false
    readonly property string installStatus: AppSettings.focusShortcutsInstallStatus
    /// Installing, and waiting for the user in the Shortcuts app.
    readonly property bool busy: ["installing", "waitingOn", "waitingOff"].indexOf(installStatus) >= 0

    readonly property string unsupportedText: i18n("The Shortcuts app is not available: silencing notifications needs macOS 12 or later.")
    readonly property string foundText: i18n("Installed. A timer with \"Silence notifications while working\" turns on Do Not Disturb while you work.")
    readonly property string missingText: i18n("Sugo turns Do Not Disturb on and off with two shortcuts. Install them, and click Add Shortcut for each one.")
    readonly property string installingText: i18n("Preparing the shortcuts…")
    readonly property string waitingOnText: i18n("Shortcuts asks you to add \"%1\": click Add Shortcut.", AppSettings.focusOnShortcut)
    readonly property string waitingOffText: i18n("Now add \"%1\" too: click Add Shortcut.", AppSettings.focusOffShortcut)
    readonly property string failedText: i18n("The shortcuts could not be installed: %1", AppSettings.focusShortcutsInstallDetail)

    function description(supported: bool): string {
        if (!supported) {
            return unsupportedText;
        }
        switch (installStatus) {
        case "installing":
            return installingText;
        case "waitingOn":
            return waitingOnText;
        case "waitingOff":
            return waitingOffText;
        case "failed":
            return failedText;
        }
        return found ? foundText : missingText;
    }

    function descriptionVariants(supported: bool): var {
        return supported ? [foundText, missingText, installingText, waitingOnText, waitingOffText, failedText]
                         : [unsupportedText];
    }
}
