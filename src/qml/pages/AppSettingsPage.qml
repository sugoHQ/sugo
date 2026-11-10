// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import QtQuick.Dialogs
import stream.sugo.app

/// Settings of the program itself. The options of each timer live on that timer's own page.
QQC2.Page {
    id: page

    title: i18n("Settings")
    background: null

    // The settings are split in categories; only the cards of the chosen one are shown. The
    // window lists them (SettingsNav), or this page in a drop-down list when the window is narrow.
    property string category: "general"
    readonly property var categories: applicationWindow().settingsCategories
    /// The window is too narrow for the list of the categories beside the page.
    readonly property bool narrow: page.Window.width > 0 && page.Window.width < Theme.settingsNavBreakpoint

    /// Index of the category shown in `categories`.
    function categoryIndex(): int {
        return Math.max(0, categories.findIndex(entry => entry.id === category));
    }

    /// Index of the entry of `list` (an array of { value } objects) whose value is `value`;
    /// 0 when there is none. Used by the combo box bindings.
    function valueIndex(list: var, value: var): int {
        for (let i = 0; i < list.length; ++i) {
            if (list[i].value === value) {
                return i;
            }
        }
        return 0;
    }

    // Calendar sync state, guarded because the singleton can be gone at shutdown.
    readonly property bool calOn: CalendarSync ? CalendarSync.enabled : false
    readonly property string calProvider: CalendarSync ? CalendarSync.provider : "nextcloud"
    readonly property bool calCustom: calProvider === "caldav"
    readonly property bool calHasPassword: CalendarSync ? CalendarSync.hasPassword : false
    readonly property bool calConnected: calHasPassword && !calCustom
    readonly property int calStatus: CalendarSync ? CalendarSync.status : 0
    readonly property bool calWaiting: CalendarSync ? calStatus === CalendarSync.WaitingForBrowser : false
    readonly property int calPending: CalendarSync ? CalendarSync.pendingCount : 0
    // Signed in with something to send events to: password and user name are stored.
    readonly property bool calConfigured: calHasPassword && CalendarSync !== null && CalendarSync.username.length > 0
    // While the server is being asked, only the button that started the request is greyed out
    // (the two buttons used to dim together, which looked like both had been pressed).
    property string calAction: ""   // "refresh" or "test"
    readonly property bool calWorking: CalendarSync ? calStatus === CalendarSync.Working : false
    readonly property bool calFailed: CalendarSync ? calStatus === CalendarSync.Failed : false
    readonly property bool calRefreshing: calWorking && calAction === "refresh"
    // A successful connection test is shown on the Test button itself (a green check for a few
    // seconds); failures are explained in a toast.
    property bool calTestOk: false
    onCalFailedChanged: {
        if (calFailed && CalendarSync) {
            applicationWindow().showPassiveNotification(
                CalendarSync.lastError.length > 0 ? CalendarSync.lastError : CalendarSync.statusText);
        }
    }
    onCalWorkingChanged: {
        if (calWorking) {
            calTestOk = false;
        } else {
            if (calAction === "test" && CalendarSync && calStatus === CalendarSync.Ready) {
                calTestOk = true;
                calTestOkTimer.restart();
            }
            calAction = "";
        }
    }
    Timer {
        id: calTestOkTimer
        interval: 3000
        onTriggered: page.calTestOk = false
    }
    // Why the address just typed was refused (the field keeps it to be corrected); cleared by the
    // next edit.
    property string calInputError: ""
    // A refused address leaves its field unbound. Once the stored addresses change, or the service
    // (each keeps its own), both fields show what is stored again and the refusal no longer applies.
    function resetCalFields() {
        page.calInputError = "";
        calServerField.text = Qt.binding(() => CalendarSync ? CalendarSync.serverUrl : "");
        calUrlField.text = Qt.binding(() => CalendarSync ? CalendarSync.calendarUrl : "");
    }
    Connections {
        target: CalendarSync
        function onInputRejected(message) {
            page.calInputError = message;
            applicationWindow().showPassiveNotification(message);
        }
        function onProviderChanged() {
            page.resetCalFields();
        }
        function onServerUrlChanged() {
            page.resetCalFields();
        }
        function onCalendarUrlChanged() {
            page.resetCalFields();
        }
    }
    readonly property string calLastSuccess: {
        const d = CalendarSync ? CalendarSync.lastSuccess : null;
        if (!d) {
            return "";
        }
        const date = new Date(d);
        return isNaN(date.getTime()) ? "" : date.toLocaleString(Qt.locale(), Locale.ShortFormat);
    }
    // Calendars to pick from; the saved one alone when no list has been fetched yet.
    readonly property var calChoices: {
        if (!CalendarSync) {
            return [];
        }
        const list = CalendarSync.calendars;
        if (list && list.length > 0) {
            return list;
        }
        if (CalendarSync.calendarUrl.length > 0) {
            return [{ name: CalendarSync.calendarName.length > 0 ? CalendarSync.calendarName : CalendarSync.calendarUrl,
                      url: CalendarSync.calendarUrl, color: "" }];
        }
        return [];
    }
    function calChoiceIndex(): int {
        const url = CalendarSync ? CalendarSync.calendarUrl : "";
        for (let i = 0; i < calChoices.length; ++i) {
            if (calChoices[i].url === url) {
                return i;
            }
        }
        return -1;
    }
    // The calendar fields store what was typed when they lose the focus (editingFinished). A
    // click on a button does not take it from them (SButton takes the focus by Tab only), so each
    // button that uses what they hold calls this first: the focus moves here, the field being
    // edited stores its text, and the action runs with what is on screen.
    Item {
        id: calFocusSink
    }
    function commitCalendarFields(): void {
        calFocusSink.forceActiveFocus();
    }

    // The timers, for the Timers category: one group per category, in the order of the list
    // (the timers without a category last, under "Other"). Read again whenever a timer changes.
    property int presetsRevision: 0
    Connections {
        target: PresetModel
        function onCountChanged() {
            ++page.presetsRevision;
            page.pruneTimerSelection();
        }
        function onPresetChanged() {
            ++page.presetsRevision;
        }
        function onRowsMoved() {
            ++page.presetsRevision;
        }
        function onModelReset() {
            ++page.presetsRevision;
            page.pruneTimerSelection();
        }
    }
    readonly property var timerGroups: {
        void presetsRevision;
        const groups = [];
        const byName = {};
        const other = { title: i18n("Other"), timers: [] };
        for (let i = 0; i < PresetModel.count; ++i) {
            const uuid = PresetModel.uuidAt(i);
            const preset = PresetModel.get(uuid);
            const category = (preset.category || "").trim();
            let group = other;
            if (category.length > 0) {
                group = byName[category];
                if (!group) {
                    group = { title: category, timers: [] };
                    byName[category] = group;
                    groups.push(group);
                }
            }
            group.timers.push({ uuid: uuid, index: i, name: preset.name || "", summary: timerSummary(preset), icon: preset.iconName || "",
                                 builtin: preset.builtin === true });
        }
        if (other.timers.length > 0) {
            groups.push(other);
        }
        return groups;
    }
    /// What a timer's row says under its name: its durations and when the long break comes.
    function timerSummary(preset: var): string {
        const work = Math.round(preset.workSeconds / 60);
        const shortBreak = Math.round(preset.shortBreakSeconds / 60);
        const longBreak = Math.round(preset.longBreakSeconds / 60);
        const cycles = preset.cyclesBeforeLong;
        if (cycles > 0 && longBreak > 0) {
            return i18nc("a timer: work · short break · long break, in minutes; then when the long break comes",
                         "%1 · %2 · %3 min · %4", work, shortBreak, longBreak,
                         i18np("long break after %1", "long break after %1", cycles));
        }
        return i18nc("a timer: work · short break, in minutes; then that it has no long break",
                     "%1 · %2 min · %3", work, shortBreak, i18n("no long break"));
    }

    // Several timers can be chosen and deleted at once: with the box that takes the place of a
    // timer's icon, Ctrl+click (one more) or Shift+click (all those between). While some are
    // chosen, a click on a row chooses it too, and the header offers Delete instead of New timer.
    property var selectedTimers: []
    /// The timer a Shift+click counts from: the last one chosen or unchosen.
    property string selectionAnchor: ""
    readonly property bool selectingTimers: selectedTimers.length > 0
    /// The Timers category is shown with some timers chosen.
    readonly property bool choosingTimers: category === "timers" && selectingTimers

    function clearTimerSelection(): void {
        selectedTimers = [];
        selectionAnchor = "";
    }
    /// The timers in the order of the page (by category), as their rows show them.
    function shownTimers(): var {
        return timerGroups.reduce((all, group) => all.concat(group.timers), []);
    }
    /// The timer in use while it runs cannot be deleted, nor can the default one.
    function timerLocked(uuid: string): bool {
        return PresetModel.currentUuid === uuid && TimerEngine.active;
    }
    function timerSelectable(timer: var): bool {
        return !timer.builtin && !timerLocked(timer.uuid);
    }
    function timerSelected(uuid: string): bool {
        return selectedTimers.indexOf(uuid) >= 0;
    }
    function setTimerSelected(uuid: string, selected: bool): void {
        const others = selectedTimers.filter(other => other !== uuid);
        selectedTimers = selected ? others.concat([uuid]) : others;
        selectionAnchor = uuid;
    }
    /// Chooses every timer from the anchor to `uuid`, both included.
    function selectTimerRange(uuid: string): void {
        const timers = shownTimers();
        const from = timers.findIndex(timer => timer.uuid === selectionAnchor);
        const to = timers.findIndex(timer => timer.uuid === uuid);
        if (from < 0 || to < 0) {
            return;
        }
        const chosen = selectedTimers.slice();
        for (let i = Math.min(from, to); i <= Math.max(from, to); ++i) {
            if (timerSelectable(timers[i]) && chosen.indexOf(timers[i].uuid) < 0) {
                chosen.push(timers[i].uuid);
            }
        }
        selectedTimers = chosen;
    }
    function selectAllTimers(): void {
        selectedTimers = shownTimers().filter(timer => timerSelectable(timer)).map(timer => timer.uuid);
    }
    /// A click on a timer's row: opens it, or chooses it (with Ctrl or Shift, or while choosing).
    function timerRowClicked(timer: var, modifiers: int): void {
        const shift = (modifiers & Qt.ShiftModifier) !== 0;
        const control = (modifiers & (Qt.ControlModifier | Qt.MetaModifier)) !== 0;
        if (!shift && !control && !selectingTimers) {
            applicationWindow().openTimer(timer.uuid);
            return;
        }
        if (!timerSelectable(timer)) {
            return;
        }
        if (shift && selectionAnchor.length > 0) {
            selectTimerRange(timer.uuid);
        } else {
            setTimerSelected(timer.uuid, !timerSelected(timer.uuid));
        }
    }
    /// A click on a timer's box: chooses or lets go of it, or with Shift all those between.
    function timerBoxClicked(timer: var, modifiers: int): void {
        if (!timerSelectable(timer) && !timerSelected(timer.uuid)) {
            return;
        }
        if ((modifiers & Qt.ShiftModifier) !== 0 && selectionAnchor.length > 0) {
            selectTimerRange(timer.uuid);
        } else {
            setTimerSelected(timer.uuid, !timerSelected(timer.uuid));
        }
    }
    /// Timers deleted meanwhile (here, on another page, by Sugo Sync) leave the selection.
    function pruneTimerSelection(): void {
        const kept = selectedTimers.filter(uuid => PresetModel.indexOf(uuid) >= 0);
        if (kept.length !== selectedTimers.length) {
            selectedTimers = kept;
        }
    }
    function requestDeleteTimers(): void {
        if (selectingTimers) {
            deleteTimersDialog.uuids = selectedTimers.slice();
            deleteTimersDialog.open();
        }
    }
    readonly property bool deleteTimersConfirming: deleteTimersDialog.visible

    ConfirmDialog {
        id: deleteTimersDialog
        property var uuids: []
        readonly property var names: {
            void page.presetsRevision;
            return uuids.map(uuid => PresetModel.get(uuid).name || "");
        }
        title: i18ncp("@title:window the timers chosen on the Timers page", "Delete this timer?", "Delete %1 timers?", uuids.length)
        // At most three names, so a long selection keeps the dialog inside the window.
        readonly property int namesShown: 3
        readonly property string listedNames: names.slice(0, namesShown)
            .map(name => i18nc("a timer's name, in a list", "“%1”", name))
            .join(i18nc("between the names of a list", ", "))
        text: uuids.length === 1
              ? i18n("Delete “%1”? Statistics already recorded with it are kept.", names[0] || "")
              : uuids.length <= namesShown
                ? i18n("Delete %1? Statistics already recorded with them are kept.", listedNames)
                : i18np("Delete %2 and 1 more? Statistics already recorded with them are kept.",
                        "Delete %2 and %1 more? Statistics already recorded with them are kept.",
                        uuids.length - namesShown, listedNames)
        confirmText: i18n("Delete")
        confirmKind: "danger"
        onConfirmed: {
            deleteTimersDialog.close();
            // The timer in use may have been started meanwhile (tray, shortcut, D-Bus).
            const kept = [];
            for (const uuid of uuids) {
                if (page.timerLocked(uuid)) {
                    kept.push(PresetModel.get(uuid).name || "");
                } else {
                    PresetModel.remove(uuid);
                }
            }
            page.clearTimerSelection();
            if (kept.length > 0) {
                applicationWindow().showPassiveNotification(
                    i18n("“%1” was started meanwhile, so it was not deleted. Stop it to delete it.", kept[0]));
            }
        }
    }

    Shortcut {
        sequences: [StandardKey.SelectAll]
        enabled: page.visible && page.category === "timers" && !page.deleteTimersConfirming
        onActivated: page.selectAllTimers()
    }
    Shortcut {
        sequences: [StandardKey.Delete]
        enabled: page.visible && page.selectingTimers && !page.deleteTimersConfirming
        onActivated: page.requestDeleteTimers()
    }
    Shortcut {
        sequences: [StandardKey.Cancel]
        enabled: page.visible && page.selectingTimers && !page.deleteTimersConfirming
        onActivated: page.clearTimerSelection()
    }

    // About: who we are, and the updates.
    readonly property var aboutData: typeof appAboutData !== "undefined" ? appAboutData : ({
        displayName: i18n("Sugo"),
        shortDescription: i18n("A Pomodoro timer with time statistics"),
        version: "",
        copyrightStatement: ""
    })
    // Set when the user closes the update error message; cleared by every new check.
    property bool updateStatusDismissed: false
    // A check that found nothing new is acknowledged on the button itself for a few seconds.
    property bool upToDateShown: false
    Connections {
        target: UpdateChecker
        function onStatusChanged() {
            if (UpdateChecker.checking) {
                page.updateStatusDismissed = false;
                page.upToDateShown = false;
            } else if (UpdateChecker.status === UpdateChecker.UpToDate) {
                page.upToDateShown = true;
                upToDateTimer.restart();
            }
        }
    }
    Timer {
        id: upToDateTimer
        interval: 3000
        onTriggered: page.upToDateShown = false
    }

    // Sugo Sync state, guarded because the singleton can be gone at shutdown.
    readonly property int syncState: SyncController ? SyncController.state : 0
    readonly property bool syncOff: !SyncController || syncState === SyncController.Off
    readonly property bool syncSigningIn: SyncController !== null && syncState === SyncController.SigningIn
    readonly property bool syncWaiting: SyncController !== null && syncState === SyncController.WaitingForApproval
    readonly property bool syncSyncing: SyncController !== null && syncState === SyncController.Syncing
    readonly property bool syncError: SyncController !== null && syncState === SyncController.Error
    /// Linked, with the timers' key: the status, the devices and the recovery code.
    readonly property bool syncLinked: SyncController !== null
                                       && (syncState === SyncController.Idle || syncSyncing || syncError)
    readonly property int syncErrorKind: SyncController ? SyncController.errorKind : 0
    readonly property string syncStatusText: SyncController ? SyncController.statusText : ""
    readonly property bool syncKeyring: SyncController ? SyncController.keyringAvailable : true
    readonly property bool syncSignedOutRemotely: syncOff && SyncController !== null
                                                  && syncErrorKind === SyncController.SignedOutRemotely
    readonly property bool syncLimit: syncSigningIn && SyncController.deviceLimitReached
    readonly property string syncLastSync: {
        const d = SyncController ? SyncController.lastSync : null;
        if (!d) {
            return "";
        }
        const date = new Date(d);
        return isNaN(date.getTime()) ? "" : date.toLocaleString(Qt.locale(), Locale.ShortFormat);
    }
    readonly property var syncDevices: SyncController ? SyncController.devices : []
    readonly property string syncSignInText:
        i18n("Your browser opens sugo.stream, where you sign in and enter a code that Sugo shows here.")
    readonly property string syncSignedOutRemotelyText:
        i18n("This device was disconnected on sugo.stream. Sign in again to keep syncing.")
    readonly property string syncNoKeyringText: DesktopStyle.macos
        ? i18n("Sugo Sync needs the system keyring, to keep its keys safe. Allow Sugo to use the Keychain, then restart Sugo.")
        : i18n("Sugo Sync needs the system keyring, to keep its keys safe. Install or unlock one (for example KWallet or GNOME Keyring), then restart Sugo.")
    readonly property string syncEnterCodeText:
        i18n("Sign in on sugo.stream in your browser and enter the code. Sugo goes on by itself once you have.")
    readonly property string syncLimitText:
        i18n("Your account already links as many devices as the plan allows. Disconnect one on sugo.stream to free a place: Sugo goes on by itself.")
    // The device list is otherwise read only when this computer becomes ready and after an
    // approval: read it again whenever the Sync tab is shown, so a device signed out or
    // disconnected elsewhere since then disappears from it.
    onCategoryChanged: {
        // Another category starts at its top, with no timer chosen.
        flickable.contentY = 0;
        clearTimerSelection();
        // When linked, onSyncDevicesShownChanged does it.
        if (category === "sync" && SyncController && !syncDevicesShown) {
            SyncController.refreshDevices();
        }
    }
    // While the tab is on screen, "Last seen" keeps up: read the list again when the window comes
    // back, every minute (the server moves a device's last_seen at most every 5 minutes), and
    // after each sync of this device, which the server counts as seeing it.
    readonly property bool syncDevicesShown: category === "sync" && page.visible && syncLinked
                                             && !!page.Window.window && page.Window.window.active
    onSyncDevicesShownChanged: {
        if (syncDevicesShown) {
            SyncController.refreshDevices();
        }
    }
    Timer {
        interval: 60 * 1000
        repeat: true
        running: page.syncDevicesShown
        onTriggered: SyncController.refreshDevices()
    }
    onSyncLastSyncChanged: {
        if (syncDevicesShown) {
            SyncController.refreshDevices();
        }
    }
    function syncDate(value: var): string {
        if (!value) {
            return "";
        }
        const date = new Date(value);
        return isNaN(date.getTime()) ? "" : date.toLocaleString(Qt.locale(), Locale.ShortFormat);
    }
    function syncPlatformName(platform: string): string {
        return platform === "macos" ? i18n("macOS") : platform === "linux" ? i18n("Linux") : platform;
    }
    /// What the list of devices says under a device's name.
    function syncDeviceDetails(device: var): string {
        const platform = syncPlatformName(device.platform || "");
        const seen = syncDate(device.lastSeen);
        const state = device.pending ? i18n("Waiting for approval")
                    : seen.length > 0 ? i18n("Last seen %1", seen)
                    : i18n("Not synced yet");
        return platform.length > 0 ? i18nc("platform · last seen", "%1 · %2", platform, state) : state;
    }

    /// Signing out asks first.
    function requestSyncSignOut(): void {
        syncSignOutDialog.open();
    }
    readonly property bool syncSignOutConfirming: syncSignOutDialog.visible

    ConfirmDialog {
        id: syncSignOutDialog
        title: i18n("Sign out of Sugo Sync?")
        text: i18n("This computer stops syncing and leaves your list of devices. Your timers stay on this computer.")
        confirmText: i18n("Sign out")
        confirmKind: "danger"
        onConfirmed: {
            syncSignOutDialog.close();
            if (SyncController) {
                SyncController.signOut();
            }
        }
    }

    ConfirmDialog {
        id: syncDisconnectDialog
        property string deviceId
        property string deviceName
        title: i18n("Disconnect “%1”?", deviceName)
        text: i18n("That device stops syncing and its place on your plan is free again. Its timers stay on it, and it can sign in again later.")
        confirmText: i18n("Disconnect")
        confirmKind: "danger"
        onConfirmed: {
            syncDisconnectDialog.close();
            if (SyncController) {
                SyncController.disconnectDevice(deviceId);
            }
        }
    }

    ConfirmDialog {
        id: syncNewRecoveryDialog
        title: i18n("Create a new recovery code?")
        text: i18n("The recovery code you have now stops working. Sugo shows you the new one, to keep in a safe place.")
        confirmText: i18n("Create new code")
        onConfirmed: {
            syncNewRecoveryDialog.close();
            if (SyncController) {
                SyncController.regenerateRecoveryCode();
            }
        }
    }

    ConfirmDialog {
        id: syncShowRecoveryDialog
        property string code
        title: i18n("Recovery code")
        text: i18n("With this code a new device opens your timers when none of your other devices is at hand. Keep it somewhere safe, away from this computer.")
        confirmText: i18n("Copy")
        cancelText: i18n("Close")
        onConfirmed: {
            syncRecoveryText.selectAll();
            syncRecoveryText.copy();
            syncRecoveryText.deselect();
            syncShowRecoveryDialog.close();
            applicationWindow().showPassiveNotification(i18n("Recovery code copied to the clipboard."));
        }
        onClosed: code = ""

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: syncRecoveryText.implicitHeight + Theme.spaceLg * 2
            radius: Theme.radius
            color: Theme.background
            border.width: Theme.hairline
            border.color: Theme.border
            TextEdit {
                id: syncRecoveryText
                anchors.fill: parent
                anchors.margins: Theme.spaceLg
                verticalAlignment: TextEdit.AlignVCenter
                readOnly: true
                selectByMouse: true
                text: syncShowRecoveryDialog.code
                color: Theme.foreground
                selectionColor: Theme.alpha(Theme.brand, 0.35)
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontTitle + 1
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                Accessible.name: i18n("Recovery code")
            }
        }
    }

    // A copied sign-in code is shown on the Copy button itself (a check for a few seconds).
    property bool syncCodeCopied: false
    Timer {
        id: syncCodeCopiedTimer
        interval: 3000
        onTriggered: page.syncCodeCopied = false
    }

    // The calendar list is not kept between runs, and calendars made on the server since the last
    // visit should show up: reload it when the page opens, if the calendar is on and signed in,
    // unless it was fetched a short while ago (the refresh button always reloads).
    Component.onCompleted: {
        // Opened on one category (the timer page's "Set Up in Settings").
        const window = applicationWindow();
        if (window && window.pendingSettingsCategory && window.pendingSettingsCategory.length > 0) {
            page.category = window.pendingSettingsCategory;
            window.pendingSettingsCategory = "";
        }
        if (DesktopStyle.macos) {
            AppSettings.refreshFocusShortcuts();
        }
        if (CalendarSync && CalendarSync.enabled && CalendarSync.hasPassword && !calWaiting) {
            CalendarSync.refreshCalendarsIfStale();
        }
    }

    // macOS: the user's shortcuts, as last read, for the Focus card.
    property var focusShortcutNames: AppSettings.focusShortcutNames()
    Connections {
        target: AppSettings
        function onFocusShortcutNamesChanged() {
            page.focusShortcutNames = AppSettings.focusShortcutNames();
        }
    }
    readonly property bool focusOnFound: focusShortcutNames.indexOf(AppSettings.focusOnShortcut) >= 0
    readonly property bool focusOffFound: focusShortcutNames.indexOf(AppSettings.focusOffShortcut) >= 0

    /// Disconnect forgets the password: asked first.
    function requestDisconnect(): void {
        disconnectDialog.open();
    }
    readonly property bool disconnectConfirming: disconnectDialog.visible

    ConfirmDialog {
        id: disconnectDialog
        title: i18n("Disconnect the calendar?")
        text: i18n("Sugo forgets the password for this calendar service and stops sending events. You can sign in again at any time.")
        confirmText: i18n("Disconnect")
        confirmKind: "danger"
        onConfirmed: {
            disconnectDialog.close();
            if (CalendarSync) {
                CalendarSync.disconnect();
            }
        }
    }

    // A copy is shown on the Copy button itself (a check for a few seconds), like the calendar test.
    property bool diagnosticsCopied: false
    function showDiagnosticsCopied(): void {
        diagnosticsCopied = true;
        diagnosticsCopiedTimer.restart();
    }
    Timer {
        id: diagnosticsCopiedTimer
        interval: 3000
        onTriggered: page.diagnosticsCopied = false
    }

    Connections {
        target: AppSettings
        function onAutostartFailed(message) {
            applicationWindow().showPassiveNotification(message);
        }
    }

    Connections {
        target: DataManager
        function onOperationFailed(message) {
            applicationWindow().showPassiveNotification(message);
        }
        function onOperationSucceeded(message) {
            applicationWindow().showPassiveNotification(message);
        }
    }

    ConfirmDialog {
        id: clearHistoryDialog
        title: i18n("Clear session history?")
        text: i18n("This permanently removes all recorded sessions and statistics. Presets and settings will remain.")
        confirmText: i18n("Clear history")
        confirmKind: "danger"
        onConfirmed: {
            DataManager.clearHistory();
            clearHistoryDialog.close();
        }
    }

    // Months of the "older than" choice of the session history: 0 stands for all sessions.
    readonly property var historyCleanupChoices: [
        { text: i18n("Older than 3 months"), value: 3 },
        { text: i18n("Older than 6 months"), value: 6 },
        { text: i18n("Older than 1 year"), value: 12 },
        { text: i18n("All sessions"), value: 0 }
    ]

    ConfirmDialog {
        id: clearOldHistoryDialog
        property int months: 3
        title: i18n("Remove old sessions?")
        text: months === 12
            ? i18n("This permanently removes the sessions recorded more than 1 year ago. More recent sessions, presets and settings will remain.")
            : i18n("This permanently removes the sessions recorded more than %1 months ago. More recent sessions, presets and settings will remain.", months)
        confirmText: i18n("Remove sessions")
        confirmKind: "danger"
        onConfirmed: {
            DataManager.clearHistoryOlderThan(months);
            clearOldHistoryDialog.close();
        }
    }

    ConfirmDialog {
        id: restoreConfirmDialog
        title: i18n("Restore this backup?")
        text: i18n("Restoring replaces your current presets and session history with the contents of the selected backup.")
        confirmText: i18n("Restore")
        onConfirmed: {
            DataManager.restoreDatabase(restoreDialog.selectedFile);
            restoreConfirmDialog.close();
        }
    }

    ConfirmDialog {
        id: resetSettingsDialog
        title: i18n("Reset all settings?")
        text: i18n("Every setting on this page goes back to its default. Your timers, session history, calendar account and the choice to start Sugo at login are kept.")
        confirmText: i18n("Reset Settings")
        confirmKind: "danger"
        onConfirmed: {
            resetSettingsDialog.close();
            AppSettings.resetToDefaults();
            applicationWindow().showPassiveNotification(i18n("Settings were reset to their defaults."));
        }
    }
    readonly property bool wide: width >= Theme.contentMaxWidth * 0.62

    // Switches bound to a setting lose their binding when the user toggles them. Re-binding
    // afterwards makes them show the real value again, e.g. when a change was refused.
    component SettingSwitch: SSwitch {
        id: sw
        required property string setting
        // The label is the title of the SettingRow around the switch: screen readers get it here.
        readonly property Item row: {
            let item = sw.parent;
            while (item && (item.first === undefined || item.title === undefined)) {
                item = item.parent;
            }
            return item;
        }
        readonly property string accessibleName: row ? row.title : ""
        Accessible.name: accessibleName
        Accessible.description: row ? row.description : ""
        checked: AppSettings[setting]
        onToggled: {
            AppSettings[setting] = checked;
            checked = Qt.binding(() => AppSettings[sw.setting]);
        }
    }

    // The tray icon in one badge style, resting mid-session. play() runs a work session and a
    // break through it once; while `looping` it runs them over and over.
    component BadgePreview: TrayBadge {
        id: preview
        property bool looping: false
        readonly property real restProgress: 0.6
        implicitWidth: Theme.iconMd
        progress: restProgress

        function play(): void {
            showcase.restart();
        }
        function rest(): void {
            showcase.stop();
            preview.isBreak = false;
            preview.progress = preview.restProgress;
            preview.scale = 1;
        }
        onLoopingChanged: looping ? play() : rest()

        SequentialAnimation {
            id: showcase
            loops: preview.looping ? Animation.Infinite : 1
            PropertyAction { target: preview; property: "isBreak"; value: false }
            ParallelAnimation {
                NumberAnimation { target: preview; property: "progress"; from: 0; to: 1; duration: Theme.durationLong * 5; easing.type: Easing.InOutQuad }
                NumberAnimation { target: preview; property: "scale"; from: 0.6; to: 1; duration: Theme.durationLong; easing.type: Easing.OutBack }
            }
            PauseAnimation { duration: Theme.durationLong }
            PropertyAction { target: preview; property: "isBreak"; value: true }
            NumberAnimation { target: preview; property: "progress"; from: 0; to: 1; duration: Theme.durationLong * 3; easing.type: Easing.InOutQuad }
            PauseAnimation { duration: Theme.durationLong }
            PropertyAction { target: preview; property: "isBreak"; value: false }
            NumberAnimation { target: preview; property: "progress"; from: 0; to: preview.restProgress; duration: Theme.durationLong * 2; easing.type: Easing.OutCubic }
        }
    }

    component SmallText: Text {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.fontCaption
        color: Theme.muted
    }

    /// A code to read out or type elsewhere (Sugo Sync): large, spaced, with a caption above it.
    /// While `code` is empty the place is kept and a busy indicator turns in it.
    component SyncCode: ColumnLayout {
        id: syncCode
        property string caption
        property string code
        Layout.fillWidth: true
        Layout.topMargin: Theme.spaceXl
        Layout.bottomMargin: Theme.spaceXl
        spacing: Theme.spaceSm

        Text {
            Layout.fillWidth: true
            text: syncCode.caption
            color: Theme.muted
            font.pixelSize: Theme.fontCaption
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }
        Item {
            Layout.fillWidth: true
            implicitHeight: codeText.implicitHeight
            Text {
                id: codeText
                anchors.left: parent.left
                anchors.right: parent.right
                // "M" keeps the line's height while there is no code yet.
                text: syncCode.code.length > 0 ? syncCode.code : "M"
                opacity: syncCode.code.length > 0 ? 1 : 0
                color: Theme.foreground
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontHero
                font.weight: Font.DemiBold
                font.letterSpacing: Theme.letterSpacingCaps * 2
                horizontalAlignment: Text.AlignHCenter
                fontSizeMode: Text.HorizontalFit
                minimumPixelSize: Theme.fontTitle
                Accessible.role: Accessible.StaticText
                Accessible.name: syncCode.caption + " " + syncCode.code
            }
            QQC2.BusyIndicator {
                anchors.centerIn: parent
                width: Theme.iconLg
                height: Theme.iconLg
                visible: syncCode.code.length === 0
                running: visible && page.visible
            }
        }
    }

    /// A titled group of setting rows, shown when its category is chosen: a small heading over a
    /// card of rows.
    component SettingsCard: ColumnLayout {
        id: card
        property string title
        property string iconName
        required property string category
        /// False hides the card in every category (a setting this copy of Sugo does not have).
        property bool available: true
        default property alias rows: rowsColumn.data

        visible: card.available && page.category === card.category
        Layout.fillWidth: true
        Layout.topMargin: card.title.length > 0 ? Theme.spaceLg : 0
        spacing: Theme.spaceSm

        Text {
            visible: card.title.length > 0
            text: card.title
            color: Theme.muted
            font.pixelSize: Theme.fontCaption
            font.weight: Font.Medium
            Layout.fillWidth: true
            elide: Text.ElideRight
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: rowsColumn.implicitHeight
            radius: Theme.radiusLg
            color: Theme.card
            border.width: Theme.hairline
            border.color: Theme.border

            ColumnLayout {
                id: rowsColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.leftMargin: Theme.settingsRowPaddingX
                anchors.rightMargin: Theme.settingsRowPaddingX
                spacing: 0
            }
        }
    }

    /// One setting: title and description on the left, controls on the right, or under the text
    /// when they would leave it too little room.
    component SettingRow: Item {
        id: row
        property string title
        property string description
        property bool first: false
        /// The title as a heading (the first row of a card that explains the others).
        property bool emphasized: false
        /// Every text `description` can show, when it changes with a state: the row keeps the
        /// height of the tallest, so nothing moves when the description changes.
        property var descriptionVariants: []
        /// The height of the description shown, and the room the row keeps for its variants
        /// (smoke checks: both follow the text at once, while the layout catches up later).
        readonly property real descriptionHeight: descriptionText.implicitHeight
        readonly property real descriptionRoom: descriptionText.reservedHeight
        /// The controls sit beside the text (else under it).
        readonly property bool sideBySide: controlBox.children.length > 0
            && width - controlBox.implicitWidth - grid.columnSpacing >= Theme.settingsRowTextMinWidth
        default property alias controls: controlBox.data

        Layout.fillWidth: true
        implicitHeight: Math.max(Theme.settingsRowMinHeight, grid.implicitHeight + Theme.settingsRowPaddingY * 2)

        // The line above the row, across the whole card.
        Rectangle {
            visible: !row.first
            x: -Theme.settingsRowPaddingX
            width: row.width + Theme.settingsRowPaddingX * 2
            height: Theme.hairline
            color: Theme.border
        }

        GridLayout {
            id: grid
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            columns: row.sideBySide ? 2 : 1
            columnSpacing: Theme.spaceXl
            rowSpacing: Theme.spaceMd

            ColumnLayout {
                id: textColumn
                Layout.fillWidth: true
                Layout.preferredWidth: 0
                Layout.alignment: Qt.AlignVCenter
                // With variants, as tall as the title and the tallest of them; the text shown sits
                // in the middle of that room, in line with the controls. The two fillers make three
                // gaps, all counted, or the tallest text would push the row taller.
                Layout.minimumHeight: row.descriptionVariants.length > 0
                                      ? titleText.implicitHeight + descriptionText.reservedHeight + spacing * 3 : 0
                spacing: Theme.hairline
                opacity: row.enabled ? 1 : 0.5
                Item { visible: row.descriptionVariants.length > 0; Layout.fillHeight: true }
                Text {
                    id: titleText
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: row.title
                    color: Theme.foreground
                    font.pixelSize: row.emphasized ? Theme.fontTitle : Theme.fontBody
                    font.weight: row.emphasized ? Font.DemiBold : Font.Normal
                    wrapMode: Text.WordWrap
                }
                SmallText {
                    id: descriptionText
                    visible: row.description.length > 0
                    text: row.description
                    readonly property real reservedHeight: {
                        let height = 0;
                        for (let i = 0; i < descriptionMeasures.count; ++i) {
                            const measure = descriptionMeasures.itemAt(i);
                            if (measure) {
                                height = Math.max(height, measure.implicitHeight);
                            }
                        }
                        return height;
                    }

                    // The variants, never shown: their heights at this width.
                    Repeater {
                        id: descriptionMeasures
                        model: row.descriptionVariants
                        delegate: Text {
                            required property string modelData
                            visible: false
                            width: descriptionText.width
                            text: modelData
                            font: descriptionText.font
                            wrapMode: Text.WordWrap
                        }
                    }
                }
                Item { visible: row.descriptionVariants.length > 0; Layout.fillHeight: true }
            }

            RowLayout {
                id: controlBox
                // A row of text only leaves the text the whole width.
                visible: children.length > 0
                Layout.alignment: row.sideBySide ? (Qt.AlignRight | Qt.AlignVCenter) : Qt.AlignLeft
                // Under the text, a field takes the row's width.
                Layout.fillWidth: !row.sideBySide
                spacing: Theme.spaceSm
            }
        }
    }

    /// A timer in the Timers category: its name and durations; a click opens its page.
    component TimerRow: QQC2.AbstractButton {
        id: timerRow
        required property var timer
        property bool first: false

        objectName: "timerRow-" + timer.index
        Layout.fillWidth: true
        implicitHeight: Math.max(Theme.settingsRowMinHeight, timerRowContent.implicitHeight + Theme.settingsRowPaddingY * 2)
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        text: timer.name
        Accessible.name: timer.name
        Accessible.description: timer.summary
        // From the keyboard; the mouse goes through rowMouse, which knows Ctrl and Shift.
        onClicked: page.timerRowClicked(timer, Qt.NoModifier)
        // Ctrl (⌘) or Shift with Space or Enter chooses the timer, as Ctrl/Shift+click does,
        // instead of opening it. Taken here, the press never reaches the button, so no click follows.
        Keys.onPressed: event => {
            const choosing = (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier | Qt.ShiftModifier)) !== 0;
            if (choosing && (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                // A held key chooses once: its repeats would undo and redo the choice.
                if (!event.isAutoRepeat) {
                    page.timerRowClicked(timer, event.modifiers);
                }
                event.accepted = true;
            }
        }

        readonly property bool pointed: rowMouse.containsMouse || boxMouse.containsMouse
        readonly property bool selected: page.timerSelected(timer.uuid)
        // The box shows while timers are chosen, under the pointer, or on the row with keyboard focus.
        readonly property bool showBox: !timer.builtin && (page.selectingTimers || pointed || visualFocus)

        MouseArea {
            id: rowMouse
            // Under the row's content, so the box above it gets its own clicks.
            z: -0.5
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: mouse => page.timerRowClicked(timerRow.timer, mouse.modifiers)
        }

        background: Item {
            // A chosen timer's row, tinted across the whole card.
            Rectangle {
                x: -Theme.settingsRowPaddingX
                width: parent.width + Theme.settingsRowPaddingX * 2
                height: parent.height
                color: Theme.hover
                opacity: timerRow.selected ? 1 : 0
            }
            // The line above the row, across the whole card.
            Rectangle {
                visible: !timerRow.first
                x: -Theme.settingsRowPaddingX
                width: parent.width + Theme.settingsRowPaddingX * 2
                height: Theme.hairline
                color: Theme.border
            }
            Rectangle {
                visible: timerRow.visualFocus
                anchors.fill: parent
                anchors.topMargin: Theme.hairline * 2
                anchors.bottomMargin: Theme.hairline
                radius: Theme.radiusSm
                color: "transparent"
                border.width: Theme.hairline
                border.color: Theme.focusRing
            }
        }
        contentItem: RowLayout {
            id: timerRowContent
            spacing: Theme.spaceLg
            // The timer's own icon, in the accent like the other accents of the interface; the
            // box that chooses the timer takes its place.
            Item {
                implicitWidth: Theme.iconMd
                implicitHeight: Theme.iconMd
                TimerIcon {
                    anchors.centerIn: parent
                    visible: timerRow.timer.icon.length > 0
                    opacity: timerRow.showBox ? 0 : 1
                    iconName: timerRow.timer.icon
                    size: Theme.iconMd
                    color: Theme.accent
                }
                QQC2.AbstractButton {
                    id: selectBox
                    anchors.fill: parent
                    visible: !timerRow.timer.builtin
                    opacity: timerRow.showBox ? 1 : 0
                    // The running timer's box stays enabled, faded, so its tooltip can say why.
                    readonly property bool locked: !timerRow.selected && page.timerLocked(timerRow.timer.uuid)
                    focusPolicy: Qt.NoFocus
                    checkable: true
                    checked: timerRow.selected
                    Accessible.name: i18n("Select “%1”", timerRow.timer.name)
                    QQC2.ToolTip.visible: boxMouse.containsMouse && locked
                    QQC2.ToolTip.text: i18n("This timer is running. Stop it to delete it.")
                    QQC2.ToolTip.delay: Theme.toolTipDelay
                    // From assistive tools; the mouse goes through boxMouse, which knows Shift.
                    onClicked: {
                        checked = Qt.binding(() => timerRow.selected);
                        page.timerBoxClicked(timerRow.timer, Qt.NoModifier);
                    }
                    MouseArea {
                        id: boxMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: mouse => page.timerBoxClicked(timerRow.timer, mouse.modifiers)
                    }
                    contentItem: null
                    background: Rectangle {
                        anchors.centerIn: parent
                        width: Theme.iconSm
                        height: Theme.iconSm
                        radius: Theme.radiusXs
                        color: selectBox.checked ? Theme.accent : "transparent"
                        border.width: selectBox.checked ? 0 : Theme.hairline * 2
                        border.color: boxMouse.containsMouse && !selectBox.locked ? Theme.foreground : Theme.subtle
                        opacity: selectBox.locked ? 0.5 : 1
                        LineIcon {
                            anchors.centerIn: parent
                            visible: selectBox.checked
                            name: "check"
                            size: Theme.iconSm
                            color: Theme.accentText
                        }
                    }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.hairline
                Text {
                    Layout.fillWidth: true
                    text: timerRow.timer.name
                    color: Theme.foreground
                    font.pixelSize: Theme.fontBody
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
                Text {
                    Layout.fillWidth: true
                    text: timerRow.timer.summary
                    color: Theme.muted
                    font.pixelSize: Theme.fontCaption
                    font.features: { "tnum": 1 }
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
            }
            // The timer in use; faded rather than hidden, so the name keeps its room.
            Text {
                text: i18n("Selected")
                opacity: PresetModel.currentUuid === timerRow.timer.uuid ? 1 : 0
                color: Theme.subtle
                font.pixelSize: Theme.fontCaption
            }
            LineIcon {
                name: "chevron-right"
                size: Theme.iconSm
                color: timerRow.pointed ? Theme.foreground : Theme.subtle
            }
        }
    }

    /// A link of About: its name, and the sign of a page that opens in the browser.
    component LinkRow: QQC2.AbstractButton {
        id: linkRow
        property string url
        property bool first: false

        Layout.fillWidth: true
        implicitHeight: Theme.settingsRowMinHeight
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Accessible.role: Accessible.Link
        Accessible.name: text
        onClicked: Qt.openUrlExternally(url)

        background: Item {
            // The line above the row, across the whole card.
            Rectangle {
                visible: !linkRow.first
                x: -Theme.settingsRowPaddingX
                width: parent.width + Theme.settingsRowPaddingX * 2
                height: Theme.hairline
                color: Theme.border
            }
            Rectangle {
                visible: linkRow.visualFocus
                anchors.fill: parent
                anchors.topMargin: Theme.hairline * 2
                anchors.bottomMargin: Theme.hairline
                radius: Theme.radiusSm
                color: "transparent"
                border.width: Theme.hairline
                border.color: Theme.focusRing
            }
        }
        contentItem: RowLayout {
            spacing: Theme.spaceLg
            Text {
                Layout.fillWidth: true
                text: linkRow.text
                color: Theme.foreground
                font.pixelSize: Theme.fontBody
                elide: Text.ElideRight
            }
            LineIcon {
                name: "external"
                size: Theme.iconSm
                color: linkRow.hovered ? Theme.foreground : Theme.subtle
            }
        }
    }

    // The page's padding: less in a narrow window.
    readonly property real paddingX: narrow ? Theme.settingsBodyPaddingCompactX : Theme.settingsBodyPaddingX
    readonly property real paddingY: narrow ? Theme.settingsBodyPaddingCompactY : Theme.settingsBodyPaddingY

    Flickable {
        id: flickable
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.implicitHeight + page.paddingY * 2 + Theme.spaceMd
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        QQC2.ScrollBar.vertical: QQC2.ScrollBar {}

        ColumnLayout {
            id: content
            x: page.paddingX
            y: page.paddingY
            width: Math.max(0, flickable.width - page.paddingX * 2)
            spacing: Theme.spaceSm

            // The category's title, with what it offers (New timer). A narrow window has no room
            // for the list of the categories beside the page: they are chosen here instead.
            RowLayout {
                id: categoryHeader
                visible: page.narrow || page.category !== "about"
                Layout.fillWidth: true
                Layout.bottomMargin: Theme.spaceXs
                spacing: Theme.spaceMd
                SComboBox {
                    id: categoryBox
                    objectName: "settingsCategoryBox"
                    // While timers are chosen, the count takes its place.
                    visible: page.narrow && !page.choosingTimers
                    textRole: "text"
                    valueRole: "id"
                    model: page.categories
                    currentIndex: page.categoryIndex()
                    Accessible.name: i18n("Settings")
                    onActivated: {
                        applicationWindow().openSettings(currentValue);
                        currentIndex = Qt.binding(() => page.categoryIndex());
                    }
                }
                Text {
                    visible: !page.narrow || page.choosingTimers
                    Layout.fillWidth: true
                    text: page.choosingTimers
                          ? i18np("1 timer chosen", "%1 timers chosen", page.selectedTimers.length)
                          : page.categories[page.categoryIndex()].text
                    color: Theme.foreground
                    font.pixelSize: Theme.fontSettingsTitle
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    Accessible.role: Accessible.Heading
                }
                Item {
                    visible: page.narrow && !page.choosingTimers
                    Layout.fillWidth: true
                }
                // The button is taller than the title: it is centred on the row without making it
                // taller, so the Timers page starts at the same height as the others.
                // While timers are chosen, Cancel and Delete take its place.
                Item {
                    visible: page.category === "timers"
                    Layout.fillHeight: true
                    implicitWidth: page.selectingTimers ? selectionButtons.implicitWidth : newTimerButton.implicitWidth
                    SButton {
                        id: newTimerButton
                        objectName: "newTimerButton"
                        visible: !page.selectingTimers
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        kind: "primary"
                        iconName: "plus"
                        text: i18n("New timer")
                        onClicked: applicationWindow().newTimer()
                    }
                    RowLayout {
                        id: selectionButtons
                        visible: page.selectingTimers
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.spaceSm
                        SButton {
                            kind: "ghost"
                            text: i18n("Cancel")
                            onClicked: page.clearTimerSelection()
                        }
                        SButton {
                            objectName: "deleteTimersButton"
                            kind: "danger"
                            iconName: "trash"
                            text: i18n("Delete")
                            onClicked: page.requestDeleteTimers()
                        }
                    }
                }
            }

            // --- Timers -------------------------------------------------------------------
            Repeater {
                model: page.category === "timers" ? page.timerGroups : []
                delegate: SettingsCard {
                    id: timerGroup
                    required property var modelData
                    category: "timers"
                    title: modelData.title
                    Repeater {
                        model: timerGroup.modelData.timers
                        delegate: TimerRow {
                            required property var modelData
                            required property int index
                            timer: modelData
                            first: index === 0
                        }
                    }
                }
            }

            SettingsCard {
                category: "general"
                title: i18n("Appearance")
                iconName: "preferences-desktop-theme"
                SettingRow {
                    first: true
                    title: i18n("Color scheme")
                    SComboBox {
                        id: colorSchemeBox
                        textRole: "text"
                        valueRole: "value"
                        model: [
                            { text: i18n("System default"), value: "system" },
                            { text: i18n("Light"), value: "light" },
                            { text: i18n("Dark"), value: "dark" }
                        ]
                        currentIndex: page.valueIndex(model, AppSettings.colorScheme || "system")
                        Accessible.name: i18n("Color scheme")
                        onActivated: AppSettings.colorScheme = currentValue
                    }
                }
                SettingRow {
                    title: i18n("Accent color")
                    description: i18n("Buttons, switches and the running time. Breaks stay green (blue with a green accent).")
                    AccentButton {}
                }
            }

            SettingsCard {
                category: "general"
                title: i18n("Startup")
                iconName: "system-run"
                SettingRow {
                    first: true
                    title: i18n("Start Sugo automatically when I log in")
                    description: AppSettings.autostartSupported ? ""
                                 : DesktopStyle.macos ? i18n("Starting at login cannot be set up from inside Sugo here. Add Sugo in System Settings → General → Login Items instead.")
                                 : i18n("Starting at login cannot be set up from inside this environment (for example a sandboxed package). Use your desktop's autostart settings instead.")
                    enabled: AppSettings.autostartSupported
                    SettingSwitch { setting: "startAtLogin" }
                }
                SettingRow {
                    title: DesktopStyle.macos ? i18n("Start hidden in the menu bar") : i18n("Start minimized to the system tray")
                    description: DesktopStyle.macos ? i18n("When started at login, Sugo opens in the menu bar without showing its window.")
                                                    : i18n("When started at login, Sugo opens in the tray without showing its window.")
                    enabled: AppSettings.startAtLogin && AppSettings.showTrayIcon && AppSettings.trayAvailable
                    SettingSwitch { setting: "startMinimized" }
                }
            }

            // Only the AppImage and the macOS app (in a folder the user can write to) replace
            // themselves; other packages are updated by whoever installed them.
            SettingsCard {
                category: "general"
                available: UpdateChecker.canInstall
                title: i18n("Updates")
                iconName: "system-software-update"
                SettingRow {
                    first: true
                    title: i18n("Check for updates automatically")
                    description: i18n("Sugo looks for a new version every few hours and tells you; it downloads it only when you click Install. Turned off, it only looks when you ask, in About.")
                    SettingSwitch { setting: "autoCheckUpdates" }
                }
                // Nightly builds are for people who want the newest changes and can live with bugs.
                SettingRow {
                    title: i18n("Update channel")
                    description: AppSettings.updateChannel === "nightly"
                                 ? i18n("Every new build, as soon as it is made. May have bugs.")
                                 : UpdateChecker.currentVersion.indexOf("-nightly") >= 0
                                   ? i18n("Tested releases. This nightly build stays until a newer release comes out.")
                                   : i18n("Tested releases.")
                    descriptionVariants: [i18n("Every new build, as soon as it is made. May have bugs."),
                                          i18n("Tested releases. This nightly build stays until a newer release comes out."),
                                          i18n("Tested releases.")]
                    Segmented {
                        model: [
                            { id: "stable", text: i18n("Stable") },
                            { id: "nightly", text: i18n("Nightly") }
                        ]
                        current: AppSettings.updateChannel
                        Accessible.name: i18n("Update channel")
                        onActivated: id => AppSettings.updateChannel = id
                    }
                }
            }

            SettingsCard {
                category: "general"
                title: DesktopStyle.macos ? i18n("Menu bar") : i18n("System tray")
                iconName: "notifications"
                SettingRow {
                    id: trayRow
                    // The tray can appear or go while the page is open: the explanation is a
                    // variant of this row's description, not a banner that moves the card.
                    readonly property string availableText: DesktopStyle.macos
                        ? i18n("While a timer runs, the menu bar icon shows its progress. Click it to show the window, or to start, pause, stop or quit.")
                        : i18n("While a timer runs, the tray icon shows its progress. Click it to show or hide the window; use its menu to start, pause, stop or quit.")
                    readonly property string unavailableText: DesktopStyle.macos
                        ? i18n("The menu bar icon is not available, so its options are turned off.")
                        : i18n("No system tray was found on this desktop, so the tray options are not available and closing the window quits Sugo.")
                    first: true
                    title: DesktopStyle.macos ? i18n("Show an icon in the menu bar") : i18n("Show an icon in the system tray")
                    description: AppSettings.trayAvailable ? availableText : unavailableText
                    descriptionVariants: [availableText, unavailableText]
                    enabled: AppSettings.trayAvailable
                    SettingSwitch { setting: "showTrayIcon" }
                }
                // On macOS closing the window always hides it (Sugo stays in the Dock and menu bar).
                SettingRow {
                    visible: !DesktopStyle.nativeChrome
                    title: i18n("Keep running in the tray when the window is closed")
                    enabled: AppSettings.showTrayIcon && AppSettings.trayAvailable
                    SettingSwitch { setting: "closeToTray" }
                }
                SettingRow {
                    title: i18n("Badge style")
                    enabled: AppSettings.showTrayIcon && AppSettings.trayAvailable
                    SComboBox {
                        id: trayBadgeBox
                        textRole: "text"
                        valueRole: "value"
                        model: [
                            { text: i18n("Border with minutes"), value: 0 },
                            { text: i18n("Fill with minutes"), value: 1 },
                            { text: i18n("Spiral (no numbers)"), value: 2 }
                        ]
                        currentIndex: AppSettings.trayBadgeStyle
                        Accessible.name: i18n("Badge style")
                        Accessible.description: displayText
                        onActivated: {
                            AppSettings.trayBadgeStyle = currentValue;
                            chosenBadge.play();
                        }

                        // The box shows the badge itself rather than its name, which is in the tooltip
                        // and the list; the list's badges play while the pointer is on them.
                        implicitWidth: leftPadding + Theme.iconMd + rightPadding
                        QQC2.ToolTip.visible: hovered && !popup.visible
                        QQC2.ToolTip.text: displayText
                        QQC2.ToolTip.delay: Theme.toolTipDelay
                        popup.x: width - popup.width
                        popup.width: Theme.space4xl * 5
                        contentItem: Item {
                            BadgePreview {
                                id: chosenBadge
                                anchors.verticalCenter: parent.verticalCenter
                                badgeStyle: trayBadgeBox.currentIndex
                                opacity: trayBadgeBox.enabled ? 1 : 0.5
                            }
                        }
                        delegate: QQC2.ItemDelegate {
                            id: badgeEntry
                            required property var model
                            required property int index
                            readonly property string label: (model.modelData ?? model).text ?? ""
                            width: ListView.view.width
                            implicitHeight: Theme.controlHeightLg
                            rightPadding: Theme.spaceMd * 2 + Theme.iconSm
                            highlighted: trayBadgeBox.highlightedIndex === index
                            hoverEnabled: true
                            Accessible.name: label
                            contentItem: RowLayout {
                                spacing: Theme.spaceMd
                                BadgePreview {
                                    badgeStyle: badgeEntry.index
                                    looping: badgeEntry.highlighted
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: badgeEntry.label
                                    color: Theme.foreground
                                    font: trayBadgeBox.font
                                    elide: Text.ElideRight
                                }
                            }
                            background: Rectangle {
                                radius: Theme.radiusSm
                                color: badgeEntry.highlighted || badgeEntry.hovered ? Theme.hover : "transparent"
                                LineIcon {
                                    visible: trayBadgeBox.currentIndex === badgeEntry.index
                                    anchors.right: parent.right
                                    anchors.rightMargin: Theme.spaceMd
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: "check"
                                    size: Theme.iconSm
                                    color: Theme.foreground
                                }
                            }
                        }
                    }
                }
            }

            // macOS: the Dock icon (see DockController).
            SettingsCard {
                category: "general"
                available: DesktopStyle.macos
                title: i18n("Dock")
                iconName: "preferences-desktop"
                SettingRow {
                    first: true
                    title: i18n("Show the minutes left on the Dock icon")
                    description: i18n("While a timer runs, a badge on the Dock icon shows the minutes left.")
                    SettingSwitch { setting: "showDockBadge" }
                }
                SettingRow {
                    title: i18n("Hide the Dock icon while the window is hidden")
                    description: AppSettings.showTrayIcon && AppSettings.trayAvailable
                                 ? i18n("Sugo stays in the menu bar; the Dock icon comes back when the window is shown.")
                                 : i18n("Needs the menu bar icon, to bring the window back.")
                    enabled: AppSettings.showTrayIcon && AppSettings.trayAvailable
                    SettingSwitch { setting: "hideDockIcon" }
                }
            }

            SettingsCard {
                category: "sounds"
                title: i18n("Sounds")
                iconName: "audio-volume-high"
                SettingRow {
                    first: true
                    title: i18n("Phase sound")
                    SComboBox {
                        id: soundBox
                        textRole: "text"
                        valueRole: "value"
                        model: [
                            { text: i18n("Alarm Clock (Default)"), value: "alarm-clock-elapsed" },
                            { text: i18n("Completion Chime"), value: "complete" },
                            { text: i18n("Instant Message"), value: "message-new-instant" },
                            { text: i18n("Information Sound"), value: "dialog-information" },
                            { text: i18n("Bell"), value: "bell" },
                            { text: i18n("Success"), value: "completion-success" },
                            { text: i18n("Login Chime"), value: "service-login" },
                            { text: i18n("Attention"), value: "window-attention" },
                            { text: i18n("Ringing Phone"), value: "phone-incoming-call" }
                        ]
                        currentIndex: page.valueIndex(model, AppSettings.soundTheme)
                        Accessible.name: i18n("Phase sound")
                        onActivated: AppSettings.soundTheme = currentValue
                    }
                    SButton {
                        kind: "ghost"
                        text: i18n("Test")
                        iconName: "play"
                        onClicked: AppSettings.playSoundPreview(soundBox.currentValue, AppSettings.soundVolume)
                    }
                }
                SettingRow {
                    title: i18n("Alert volume")
                    SSlider {
                        id: soundVolumeSlider
                        objectName: "soundVolumeSlider"
                        Layout.preferredWidth: page.narrow ? Theme.sliderWidthCompact : Theme.sliderWidth
                        from: 0
                        to: 100
                        value: AppSettings.soundVolume
                        Accessible.name: i18n("Alert volume")
                        onMoved: AppSettings.soundVolume = Math.round(value)
                    }
                    Text {
                        Layout.minimumWidth: Theme.space3xl + Theme.spaceXs
                        horizontalAlignment: Text.AlignRight
                        color: Theme.muted
                        font.pixelSize: Theme.fontBody
                        font.features: { "tnum": 1 }
                        text: i18nc("volume percentage", "%1%", Math.round(soundVolumeSlider.value))
                    }
                }
                SettingRow {
                    title: i18n("Play a warning one minute before a phase ends")
                    SettingSwitch { setting: "preAlarmEnabled" }
                }
                SettingRow {
                    title: i18n("Pre-alarm sound")
                    enabled: AppSettings.preAlarmEnabled
                    SComboBox {
                        id: preAlarmBox
                        textRole: "text"
                        valueRole: "value"
                        model: [
                            { text: i18n("Information Sound"), value: "dialog-information" },
                            { text: i18n("Bell"), value: "bell" },
                            { text: i18n("Completion Chime"), value: "complete" },
                            { text: i18n("Alarm Clock"), value: "alarm-clock-elapsed" },
                            { text: i18n("Message"), value: "message" },
                            { text: i18n("Attention"), value: "window-attention" },
                            { text: i18n("Success"), value: "completion-success" }
                        ]
                        currentIndex: page.valueIndex(model, AppSettings.preAlarmSound)
                        Accessible.name: i18n("Pre-alarm sound")
                        onActivated: AppSettings.preAlarmSound = currentValue
                    }
                    SButton {
                        kind: "ghost"
                        text: i18n("Test")
                        enabled: AppSettings.preAlarmEnabled
                        iconName: "play"
                        onClicked: AppSettings.playSoundPreview(preAlarmBox.currentValue, AppSettings.soundVolume)
                    }
                }
                SettingRow {
                    title: i18n("Focus sound")
                    description: i18n("A background sound that plays during work sessions.")
                    SComboBox {
                        id: focusModeBox
                        textRole: "text"
                        valueRole: "value"
                        model: [
                            { text: i18n("Off"), value: 0 },
                            { text: i18n("Clock tick"), value: 1 },
                            { text: i18n("Rain"), value: 2 },
                            { text: i18n("White noise"), value: 3 },
                            { text: i18n("Brown noise"), value: 4 },
                            { text: i18n("Pink noise"), value: 5 },
                            { text: i18n("Waves"), value: 6 }
                        ]
                        currentIndex: AppSettings.focusSoundMode
                        Accessible.name: i18n("Focus sound")
                        onActivated: AppSettings.focusSoundMode = currentValue
                    }
                }
                SettingRow {
                    title: i18n("Focus sound volume")
                    enabled: AppSettings.focusSoundMode > 0
                    SSlider {
                        id: focusVolumeSlider
                        Layout.preferredWidth: page.narrow ? Theme.sliderWidthCompact : Theme.sliderWidth
                        from: 0
                        to: 100
                        value: AppSettings.focusSoundVolume
                        Accessible.name: i18n("Focus sound volume")
                        onMoved: AppSettings.focusSoundVolume = Math.round(value)
                    }
                    Text {
                        Layout.minimumWidth: Theme.space3xl + Theme.spaceXs
                        horizontalAlignment: Text.AlignRight
                        color: Theme.muted
                        font.pixelSize: Theme.fontBody
                        font.features: { "tnum": 1 }
                        text: i18nc("volume percentage", "%1%", Math.round(focusVolumeSlider.value))
                    }
                    SButton {
                        kind: "ghost"
                        text: i18n("Preview")
                        iconName: "play"
                        onClicked: FocusSoundController.preview(focusModeBox.currentValue, Math.round(focusVolumeSlider.value))
                    }
                }
            }

            SettingsCard {
                category: "focus"
                title: i18n("Screen")
                iconName: "brightness-high"
                SettingRow {
                    first: true
                    title: i18n("Keep the screen awake during work sessions")
                    SettingSwitch { setting: "keepScreenAwake" }
                }
                SettingRow {
                    title: i18n("Pause timer when screen is locked")
                    description: i18n("Automatically pauses the active work phase when you lock your screen and prompts to resume when you unlock.")
                    SettingSwitch { setting: "autoPauseOnScreenLock" }
                }
            }

            // macOS: silencing notifications through a Focus, run by two of the user's shortcuts.
            SettingsCard {
                id: focusModeCard
                objectName: "focusModeCard"
                category: "focus"
                available: DesktopStyle.macos
                title: i18n("Focus while working")
                iconName: "notifications-disabled"
                // One row: what the shortcuts are for, where installing them got, and the button.
                // Every state's text is in the variants, so the card keeps its height.
                FocusShortcutsStatus {
                    id: focusInstallStatus
                    found: page.focusOnFound && page.focusOffFound
                }
                SettingRow {
                    first: true
                    objectName: "focusShortcutRow"
                    title: i18n("Silence notifications with a Focus")
                    description: focusInstallStatus.description(AppSettings.focusShortcutsSupported)
                    descriptionVariants: focusInstallStatus.descriptionVariants(AppSettings.focusShortcutsSupported)
                    SButton {
                        objectName: "installFocusShortcutsButton"
                        kind: focusInstallStatus.found ? "secondary" : "primary"
                        text: i18n("Install Shortcuts")
                        enabled: AppSettings.focusShortcutsInstallable && !focusInstallStatus.found && !focusInstallStatus.busy
                        Accessible.description: focusInstallStatus.description(AppSettings.focusShortcutsSupported)
                        onClicked: AppSettings.installFocusShortcuts()
                    }
                }
            }

            SettingsCard {
                category: "focus"
                title: i18n("Streak and notes")
                iconName: "games-highscores"
                SettingRow {
                    first: true
                    title: i18n("Protect daily streak over weekends")
                    description: i18n("When enabled, weekends without recorded sessions do not reset your daily streak on Monday.")
                    SettingSwitch { setting: "protectWeekendStreak" }
                }
                SettingRow {
                    title: i18n("Prompt for a task note when a work session finishes")
                    description: i18n("When a work session ends, Sugo asks what you worked on. Notes are saved to the session history.")
                    SettingSwitch { setting: "promptTaskNote" }
                }
            }

            SettingsCard {
                id: calendarCard
                category: "calendar"
                title: i18n("Calendar")
                iconName: "view-calendar"

                SettingRow {
                    first: true
                    title: i18n("Add an event to your calendar each time you finish a work session")
                    description: i18n("Sugo then sends the name of your timer, its category and the time of each finished work session to the calendar server you choose.")
                    SSwitch {
                        id: calendarSwitch
                        checked: page.calOn
                        Accessible.name: i18n("Add an event to your calendar each time you finish a work session")
                        onToggled: {
                            if (CalendarSync) {
                                CalendarSync.enabled = checked;
                            }
                            checked = Qt.binding(() => page.calOn);
                        }
                    }
                }

                ColumnLayout {
                    id: calendarBody
                    Layout.fillWidth: true
                    spacing: 0
                    enabled: page.calOn
                    opacity: page.calOn ? 1 : 0.6

                    SettingRow {
                        title: i18n("Calendar service")
                        SComboBox {
                            id: calProviderBox
                            textRole: "text"
                            valueRole: "value"
                            model: [
                                { text: i18n("Nextcloud"), value: "nextcloud" },
                                { text: i18n("Custom CalDAV"), value: "caldav" }
                            ]
                            currentIndex: page.valueIndex(model, page.calProvider)
                            Accessible.name: i18n("Calendar service")
                            onActivated: {
                                page.calInputError = "";
                                if (CalendarSync) {
                                    CalendarSync.provider = currentValue;
                                    // Each service keeps its own settings: if the one just chosen is
                                    // already signed in, load its calendars.
                                    if (CalendarSync.hasPassword) {
                                        CalendarSync.refreshCalendars();
                                    }
                                }
                                currentIndex = Qt.binding(() => page.valueIndex(model, page.calProvider));
                            }
                        }
                    }

                    // Nextcloud: server address and browser login, on one row. Who is signed in (or
                    // what the login waits for) is the description, with room kept so nothing moves.
                    SettingRow {
                        visible: !page.calCustom
                        title: i18n("Server address")
                        // Always a line, so the title never moves when the state changes.
                        description: page.calConnected
                            ? i18n("Connected as %1", CalendarSync ? CalendarSync.username : "")
                            : (page.calWaiting && CalendarSync ? CalendarSync.statusText : i18n("Not connected"))
                        descriptionVariants: [
                            i18n("Connected as %1", CalendarSync ? CalendarSync.username : ""),
                            CalendarSync ? CalendarSync.statusText : "",
                            i18n("Not connected")
                        ]
                        STextField {
                            id: calServerField
                            enabled: !page.calConnected && !page.calWaiting
                            Layout.fillWidth: !page.wide
                            // With Log in alone, the field takes the room of the second icon: the row
                            // keeps its width and the field its left edge in every state.
                            Layout.preferredWidth: Theme.space4xl * 5
                                                   + (enabled ? Theme.controlHeight + Theme.spaceSm : 0)
                            placeholderText: "https://cloud.example.org"
                            objectName: "calendarServerField"
                            inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoPredictiveText
                            text: CalendarSync ? CalendarSync.serverUrl : ""
                            Accessible.name: i18n("Server address")
                            onTextEdited: page.calInputError = ""
                            onEditingFinished: {
                                page.calInputError = "";
                                if (CalendarSync) {
                                    CalendarSync.serverUrl = text;
                                }
                                // Refused: what was typed stays to be corrected, the message says why.
                                if (page.calInputError.length === 0) {
                                    text = Qt.binding(() => CalendarSync ? CalendarSync.serverUrl : "");
                                }
                            }
                        }
                        // Icons only: Log in, the wait (spinner and Cancel), or Test and Disconnect.
                        Item {
                            implicitWidth: calLoginButtons.implicitWidth
                            implicitHeight: Theme.controlHeight
                            RowLayout {
                                id: calLoginButtons
                                objectName: "calendarLoginButtons"
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: Theme.spaceSm
                                QQC2.BusyIndicator {
                                    Layout.preferredWidth: Theme.controlHeight
                                    Layout.preferredHeight: Theme.controlHeight
                                    visible: page.calWaiting
                                    running: visible && page.visible
                                }
                                SButton {
                                    kind: "ghost"
                                    iconOnly: true
                                    visible: !page.calConnected && !page.calWaiting
                                    text: i18n("Log in")
                                    iconName: "external"
                                    onClicked: {
                                        page.commitCalendarFields();
                                        CalendarSync.startNextcloudLogin();
                                    }
                                }
                                SButton {
                                    kind: "ghost"
                                    iconOnly: true
                                    visible: page.calWaiting
                                    text: i18n("Cancel")
                                    iconName: "close"
                                    onClicked: CalendarSync.cancelLogin()
                                }
                                SButton {
                                    kind: "ghost"
                                    iconOnly: true
                                    visible: page.calConnected && !page.calWaiting
                                    text: i18n("Test connection")
                                    iconName: page.calTestOk ? "check" : "refresh"
                                    // Not while any request runs: the test would not run, yet look passed.
                                    enabled: !page.calWorking
                                    onClicked: {
                                        page.commitCalendarFields();
                                        page.calAction = "test";
                                        CalendarSync.testConnection();
                                        // Nothing was started (e.g. not signed in yet): a later request's
                                        // result must not be taken for this one's.
                                        if (!page.calWorking) {
                                            page.calAction = "";
                                        }
                                    }
                                }
                                SButton {
                                    kind: "ghost"
                                    iconOnly: true
                                    visible: page.calConnected && !page.calWaiting
                                    text: i18n("Disconnect")
                                    iconName: "close"
                                    onClicked: page.requestDisconnect()
                                }
                            }
                        }
                    }

                    // Custom CalDAV: address, username, password.
                    SettingRow {
                        visible: page.calCustom
                        title: i18n("Calendar address (CalDAV)")
                        description: i18n("Use the CalDAV address of the calendar. A subscription link ending in .ics is read-only and cannot receive events.")
                        STextField {
                            id: calUrlField
                            Layout.fillWidth: !page.wide
                            Layout.preferredWidth: Theme.space4xl * 5
                            inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoPredictiveText
                            text: CalendarSync ? CalendarSync.calendarUrl : ""
                            Accessible.name: i18n("Calendar address (CalDAV)")
                            onTextEdited: page.calInputError = ""
                            onEditingFinished: {
                                page.calInputError = "";
                                if (CalendarSync) {
                                    CalendarSync.calendarUrl = text;
                                }
                                // Refused: what was typed stays to be corrected, the message says why.
                                if (page.calInputError.length === 0) {
                                    text = Qt.binding(() => CalendarSync ? CalendarSync.calendarUrl : "");
                                }
                            }
                        }
                    }
                    SettingRow {
                        visible: page.calCustom
                        title: i18n("Username")
                        STextField {
                            Layout.fillWidth: !page.wide
                            Layout.preferredWidth: Theme.space4xl * 5
                            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                            text: CalendarSync ? CalendarSync.username : ""
                            Accessible.name: i18n("Username")
                            onEditingFinished: {
                                if (CalendarSync) {
                                    CalendarSync.username = text;
                                }
                                text = Qt.binding(() => CalendarSync ? CalendarSync.username : "");
                            }
                        }
                    }
                    SettingRow {
                        visible: page.calCustom
                        title: i18n("Password")
                        STextField {
                            id: calPasswordField
                            property bool dirty: false
                            Layout.fillWidth: !page.wide
                            Layout.preferredWidth: Theme.space4xl * 5
                            echoMode: TextInput.Password
                            // The saved password is never read back; dots only show that one is stored.
                            placeholderText: page.calHasPassword ? "••••••••" : ""
                            Accessible.name: i18n("Password")
                            onTextEdited: dirty = true
                            onEditingFinished: {
                                // Left empty, it keeps the stored password: only Disconnect, which
                                // asks first, forgets it.
                                if (dirty && text.length > 0 && CalendarSync) {
                                    CalendarSync.setPassword(text);
                                }
                                dirty = false;
                                text = "";
                            }
                        }
                    }
                    // Same row as for Nextcloud: test the connection, or forget this service's login.
                    SettingRow {
                        visible: page.calCustom && page.calConfigured
                        title: i18n("Connection")
                        SButton {
                            kind: "ghost"
                            iconOnly: true
                            text: i18n("Test connection")
                            iconName: page.calTestOk ? "check" : "refresh"
                            // Not while any request runs: the test would not run, yet look passed.
                            enabled: !page.calWorking
                            onClicked: {
                                page.commitCalendarFields();
                                page.calAction = "test";
                                CalendarSync.testConnection();
                                // Nothing was started (e.g. not signed in yet): a later request's
                                // result must not be taken for this one's.
                                if (!page.calWorking) {
                                    page.calAction = "";
                                }
                            }
                        }
                        SButton {
                            kind: "ghost"
                            iconOnly: true
                            text: i18n("Disconnect")
                            iconName: "close"
                            onClicked: page.requestDisconnect()
                        }
                    }
                    // Calendar choice, with a refresh button to reload the list from the server.
                    SettingRow {
                        visible: page.calChoices.length > 0 || page.calConfigured
                        title: i18n("Calendar")
                        // The last event sent and those waiting; one line kept for it, so nothing moves.
                        description: {
                            const lastSent = page.calLastSuccess.length > 0 ? i18n("Last event sent: %1", page.calLastSuccess) : "";
                            const waiting = page.calPending > 0
                                ? i18np("%1 event waiting to be sent", "%1 events waiting to be sent", page.calPending) : "";
                            return lastSent.length > 0 && waiting.length > 0
                                ? i18nc("last event sent · events waiting", "%1 · %2", lastSent, waiting)
                                : lastSent + waiting;
                        }
                        descriptionVariants: [i18n("Last event sent: %1", page.calLastSuccess)]
                        SComboBox {
                            id: calChoiceBox
                            Layout.fillWidth: !page.wide
                            Layout.preferredWidth: Theme.space4xl * 5
                            textRole: "name"
                            valueRole: "url"
                            model: page.calChoices
                            currentIndex: page.calChoiceIndex()
                            Accessible.name: i18n("Calendar")
                            onActivated: {
                                const item = page.calChoices[currentIndex];
                                if (CalendarSync && item) {
                                    CalendarSync.selectCalendar(item.url, item.name);
                                }
                                currentIndex = Qt.binding(() => page.calChoiceIndex());
                            }
                            enabled: page.calChoices.length > 0
                            delegate: QQC2.ItemDelegate {
                                id: calEntry
                                required property var modelData
                                required property int index
                                width: ListView.view ? ListView.view.width : calChoiceBox.width
                                implicitHeight: Theme.controlHeight
                                // The style's padding leaves the row too little room: the text would sit low.
                                verticalPadding: 0
                                highlighted: calChoiceBox.highlightedIndex === index
                                hoverEnabled: true
                                background: Rectangle {
                                    radius: Theme.radiusSm
                                    color: calEntry.highlighted || calEntry.hovered ? Theme.hover : "transparent"
                                }
                                contentItem: RowLayout {
                                    spacing: Theme.spaceSm
                                    Rectangle {
                                        visible: !!calEntry.modelData.color
                                        implicitWidth: Theme.spaceMd + Theme.hairline
                                        implicitHeight: implicitWidth
                                        radius: width / 2
                                        color: calEntry.modelData.color || Theme.background
                                        border.width: Theme.hairline
                                        border.color: Theme.border
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: calEntry.modelData.name
                                        elide: Text.ElideRight
                                        color: Theme.foreground
                                        font.pixelSize: Theme.fontBody
                                    }
                                }
                            }
                        }
                        SButton {
                            kind: "ghost"
                            iconOnly: true
                            iconName: "refresh"
                            enabled: !page.calRefreshing
                            text: i18n("Find calendars")
                            QQC2.ToolTip.text: i18n("Find calendars")
                            QQC2.ToolTip.visible: hovered
                            QQC2.ToolTip.delay: Theme.toolTipDelay
                            onClicked: {
                                page.commitCalendarFields();
                                page.calAction = "refresh";
                                CalendarSync.refreshCalendars();
                                // Nothing was started (e.g. not signed in yet): a later request's
                                // result must not be taken for this one's.
                                if (!page.calWorking) {
                                    page.calAction = "";
                                }
                            }
                        }
                    }

                    SettingRow {
                        title: i18n("Include the task note in the event description")
                        description: i18n("Notes are sent to your calendar server.")
                        SSwitch {
                            checked: CalendarSync ? CalendarSync.includeNote : false
                            Accessible.name: i18n("Include the task note in the event description")
                            onToggled: {
                                if (CalendarSync) {
                                    CalendarSync.includeNote = checked;
                                }
                                checked = Qt.binding(() => CalendarSync ? CalendarSync.includeNote : false);
                            }
                        }
                    }
                }
            }

            // Linked: whether the timers are up to date, and Sync now, under the title of the tab.
            RowLayout {
                visible: page.category === "sync" && page.syncLinked
                Layout.fillWidth: true
                spacing: Theme.spaceMd
                StatusLine {
                    objectName: "syncStatusText"
                    Layout.fillWidth: true
                    Layout.maximumWidth: implicitWidth
                    labels: [i18n("Syncing"), i18n("Offline"), i18n("Not synced"), i18n("Up to date")]
                    text: page.syncSyncing ? i18n("Syncing")
                        : page.syncError ? (page.syncErrorKind === SyncController.Network ? i18n("Offline") : i18n("Not synced"))
                        : i18n("Up to date")
                    iconName: page.syncSyncing ? "refresh" : page.syncError ? "warning" : "check"
                    iconColor: page.syncError ? (page.syncErrorKind === SyncController.Network || page.syncErrorKind === SyncController.RateLimited
                                                 ? Theme.warning : Theme.danger)
                             : Theme.muted
                    spinning: page.syncSyncing
                }
                Item {
                    Layout.fillWidth: true
                }
                SButton {
                    objectName: "syncNowButton"
                    labels: [i18n("Sync now"), i18n("Syncing…")]
                    text: page.syncSyncing ? i18n("Syncing…") : i18n("Sync now")
                    iconName: "refresh"
                    enabled: !page.syncSyncing
                    onClicked: SyncController.syncNow()
                }
            }

            // Sugo Sync: signing in, approval by another device, then the status of the timers.
            SettingsCard {
                id: syncCard
                objectName: "syncCard"
                category: "sync"
                title: i18n("Sugo Sync")
                iconName: "refresh"

                // --- Signed out -------------------------------------------------------------
                SettingRow {
                    first: true
                    emphasized: true
                    visible: page.syncOff
                    title: i18n("Your timers on all your computers")
                    description: i18n("Sugo Sync keeps your timers the same on every computer you sign in on. They are encrypted on this computer, so only your devices can read them. The free plan links 2 devices.")
                }
                SettingRow {
                    visible: page.syncOff
                    title: i18n("Device name")
                    description: i18n("How this computer appears in your list of devices.")
                    STextField {
                        id: syncDeviceName
                        Layout.fillWidth: !page.wide
                        Layout.preferredWidth: Theme.space4xl * 5
                        text: SyncController ? SyncController.deviceName : ""
                        maximumLength: 64
                        Accessible.name: i18n("Device name")
                        onAccepted: syncSignInButton.clicked()
                    }
                }
                // What keeps Sugo Sync from starting is said here, in a description that keeps the
                // height of its longest text (the toast tells what happened once, this how things are).
                SettingRow {
                    objectName: "syncSignInRow"
                    visible: page.syncOff
                    title: i18n("Sign in")
                    description: !page.syncKeyring ? page.syncNoKeyringText
                               : page.syncSignedOutRemotely ? page.syncSignedOutRemotelyText
                               : page.syncSignInText
                    descriptionVariants: [page.syncSignInText, page.syncNoKeyringText, page.syncSignedOutRemotelyText]
                    SButton {
                        id: syncSignInButton
                        objectName: "syncSignInButton"
                        kind: "primary"
                        text: i18n("Sign in")
                        enabled: page.syncKeyring && syncDeviceName.text.trim().length > 0
                        onClicked: {
                            if (enabled && SyncController) {
                                SyncController.signIn(syncDeviceName.text.trim());
                            }
                        }
                    }
                }

                // --- Signing in: the code to enter on sugo.stream ---------------------------
                SettingRow {
                    first: true
                    visible: page.syncSigningIn
                    title: i18n("Enter this code on sugo.stream")
                    description: page.syncLimit ? page.syncLimitText : page.syncEnterCodeText
                    descriptionVariants: [page.syncEnterCodeText, page.syncLimitText]
                }
                SyncCode {
                    visible: page.syncSigningIn
                    caption: i18n("Your code")
                    code: SyncController ? SyncController.userCode : ""
                }
                RowLayout {
                    visible: page.syncSigningIn
                    Layout.alignment: Qt.AlignHCenter
                    Layout.bottomMargin: Theme.spaceXl
                    spacing: Theme.spaceSm
                    SButton {
                        kind: "primary"
                        text: i18n("Open in browser")
                        iconName: "external"
                        enabled: SyncController !== null && SyncController.userCode.length > 0
                        onClicked: {
                            if (!SyncController.openVerificationPage()) {
                                applicationWindow().showPassiveNotification(i18n("The browser could not be opened. Go to sugo.stream and enter the code."));
                            }
                        }
                    }
                    SButton {
                        labels: [i18n("Copy"), i18n("Copied")]
                        text: page.syncCodeCopied ? i18n("Copied") : i18n("Copy")
                        iconName: page.syncCodeCopied ? "check" : "copy"
                        enabled: SyncController !== null && SyncController.userCode.length > 0
                        onClicked: {
                            if (SyncController.copyUserCode()) {
                                page.syncCodeCopied = true;
                                syncCodeCopiedTimer.restart();
                            }
                        }
                    }
                    SButton {
                        kind: "ghost"
                        text: i18n("Cancel")
                        onClicked: SyncController.cancelSignIn()
                    }
                }

                // --- Waiting for another device, or for the recovery code -------------------
                SettingRow {
                    first: true
                    visible: page.syncWaiting
                    title: i18n("Approve this computer on another device")
                    description: SyncController && SyncController.verificationCode.length > 0
                        ? i18n("Open Sugo on a device that already syncs and approve this computer there. Check that it shows the same code as here.")
                        : i18n("Open Sugo on a device that already syncs and approve this computer there.")
                    descriptionVariants: [i18n("Open Sugo on a device that already syncs and approve this computer there. Check that it shows the same code as here."),
                                          i18n("Open Sugo on a device that already syncs and approve this computer there.")]
                }
                SyncCode {
                    visible: page.syncWaiting
                    caption: i18n("Check that your other device shows the same code")
                    code: SyncController ? SyncController.verificationCode : ""
                }
                SettingRow {
                    visible: page.syncWaiting
                    title: i18n("Use recovery code")
                    description: i18n("No other device at hand? Enter the recovery code you saved when you first signed in.")
                    STextField {
                        id: syncRecoveryField
                        Layout.fillWidth: !page.wide
                        Layout.preferredWidth: Theme.space4xl * 5
                        font.family: Theme.monoFamily
                        placeholderText: "XXXX-XXXX-…"
                        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | Qt.ImhSensitiveData
                        Accessible.name: i18n("Recovery code")
                        onAccepted: syncRecoveryButton.clicked()
                    }
                    SButton {
                        id: syncRecoveryButton
                        text: i18n("Use")
                        Accessible.name: i18n("Use recovery code")
                        enabled: syncRecoveryField.text.trim().length > 0
                        onClicked: {
                            if (enabled) {
                                SyncController.useRecoveryCode(syncRecoveryField.text.trim());
                            }
                        }
                    }
                    // Kept while the device waits, so a wrong character is corrected rather than
                    // the whole code typed again; cleared once the code opened the timers (or the
                    // device stopped waiting some other way).
                    Connections {
                        target: page
                        function onSyncWaitingChanged() {
                            if (!page.syncWaiting) {
                                syncRecoveryField.text = "";
                            }
                        }
                    }
                }

                // --- Linked: the status of the timers ---------------------------------------
                SettingRow {
                    first: true
                    visible: page.syncLinked
                    objectName: "syncTimersRow"
                    title: i18n("Timers")
                    // What went wrong, else when the timers last synced; as tall as the longest.
                    description: page.syncError && page.syncStatusText.length > 0 ? page.syncStatusText
                               : page.syncLastSync.length > 0 ? i18n("Last synced %1", page.syncLastSync)
                               : i18n("Not synced yet")
                    descriptionVariants: [i18n("Last synced %1", page.syncLastSync), i18n("Not synced yet")]
                                         .concat(SyncController ? SyncController.errorTexts : [])
                }
            }

            // The devices linked to the account: this one, and the others to disconnect.
            SettingsCard {
                objectName: "syncDevicesCard"
                category: "sync"
                available: page.syncLinked && page.syncDevices.length > 0
                title: i18n("Devices")
                iconName: "computer"

                Repeater {
                    model: page.syncDevices
                    delegate: SettingRow {
                        id: deviceRow
                        required property var modelData
                        required property int index
                        first: index === 0
                        title: modelData.name
                        description: modelData.current
                                     ? i18nc("this device · platform · last seen", "%1 · %2", i18n("This device"), page.syncDeviceDetails(modelData))
                                     : page.syncDeviceDetails(modelData)
                        // A device waiting for this one: the approval again, also after "Not Now"
                        // (one that has never shown up yet has no code to compare).
                        SButton {
                            visible: !deviceRow.modelData.current && deviceRow.modelData.pending === true
                                     && page.syncDate(deviceRow.modelData.lastSeen).length > 0
                            kind: "primary"
                            text: i18n("Approve…")
                            Accessible.name: i18n("Approve %1", deviceRow.modelData.name)
                            onClicked: SyncController.reviewDevice(deviceRow.modelData.id)
                        }
                        SButton {
                            kind: "ghost"
                            visible: !deviceRow.modelData.current
                            text: i18n("Disconnect…")
                            destructive: true
                            Accessible.name: i18n("Disconnect %1", deviceRow.modelData.name)
                            onClicked: {
                                syncDisconnectDialog.deviceId = deviceRow.modelData.id;
                                syncDisconnectDialog.deviceName = deviceRow.modelData.name;
                                syncDisconnectDialog.open();
                            }
                        }
                    }
                }
            }

            // The recovery code and signing out.
            SettingsCard {
                objectName: "syncAccountCard"
                category: "sync"
                available: page.syncLinked || page.syncWaiting
                title: i18n("Account")
                iconName: "user-identity"

                SettingRow {
                    first: true
                    visible: page.syncLinked
                    title: i18n("Recovery code")
                    description: i18n("With it, a new device opens your timers when none of your other devices is at hand. Without any device and without the code, the synced timers cannot be opened again.")
                    SButton {
                        kind: "ghost"
                        visible: SyncController !== null && SyncController.recoveryCodeShowable
                        text: i18n("Show")
                        Accessible.name: i18n("Show recovery code")
                        onClicked: {
                            syncShowRecoveryDialog.code = SyncController.recoveryCode();
                            syncShowRecoveryDialog.open();
                        }
                    }
                    SButton {
                        kind: "ghost"
                        text: i18n("Create a new one…")
                        Accessible.name: i18n("Create a new recovery code")
                        // It needs the timers' key: not while the keyring has not given it.
                        enabled: SyncController !== null && SyncController.keysLoaded
                        onClicked: syncNewRecoveryDialog.open()
                    }
                }
                SettingRow {
                    first: !page.syncLinked
                    title: i18n("Sign out")
                    description: i18n("This computer stops syncing. Your timers stay on it.")
                    SButton {
                        kind: "ghost"
                        objectName: "syncSignOutButton"
                        text: i18n("Sign out…")
                        destructive: true
                        onClicked: page.requestSyncSignOut()
                    }
                }
            }

            // What Sugo Sync will carry next: shown, not offered yet.
            SettingsCard {
                objectName: "syncComingSoonCard"
                category: "sync"
                title: i18n("Coming to Sugo Sync")
                iconName: "appointment-soon"

                SettingRow {
                    first: true
                    title: i18n("Session history and statistics")
                    description: i18n("Your finished sessions and their statistics, the same on every device.")
                    Text {
                        text: i18n("Coming soon")
                        color: Theme.subtle
                        font.pixelSize: Theme.fontCaption
                    }
                }
                SettingRow {
                    title: i18n("Task notes")
                    description: i18n("The notes you write after a work session, on every device.")
                    Text {
                        text: i18n("Coming soon")
                        color: Theme.subtle
                        font.pixelSize: Theme.fontCaption
                    }
                }
            }

            SettingsCard {
                category: "advanced"
                title: i18n("Data management")
                iconName: "server-database"
                SettingRow {
                    first: true
                    title: i18n("Backup and restore")
                    description: TimerEngine.active
                        ? i18n("Stop the active timer before restoring a backup.")
                        : i18n("Save your timers and session history to a file, or bring them back from one.")
                    descriptionVariants: [i18n("Stop the active timer before restoring a backup."),
                                          i18n("Save your timers and session history to a file, or bring them back from one.")]
                    SButton {
                        text: i18n("Back up…")
                        iconName: "download"
                        onClicked: backupDialog.open()
                    }
                    SButton {
                        kind: "ghost"
                        text: i18n("Restore…")
                        iconName: "upload"
                        enabled: !TimerEngine.active
                        onClicked: restoreDialog.open()
                    }
                }
                SettingRow {
                    title: i18n("Session history")
                    description: i18n("Remove recorded sessions and their statistics.")
                    SComboBox {
                        id: historyCleanupBox
                        textRole: "text"
                        valueRole: "value"
                        model: page.historyCleanupChoices
                        currentIndex: 0
                        Accessible.name: i18n("Sessions to remove")
                    }
                    SButton {
                        kind: "ghost"
                        id: clearHistoryButton
                        destructive: true
                        text: i18n("Clear history…")
                        iconName: "trash"
                        enabled: DataManager !== null && DataManager.sessionCount > 0
                        onClicked: {
                            if (historyCleanupBox.currentValue === 0) {
                                clearHistoryDialog.open();
                            } else {
                                clearOldHistoryDialog.months = historyCleanupBox.currentValue;
                                clearOldHistoryDialog.open();
                            }
                        }
                    }
                }
            }

            SettingsCard {
                category: "advanced"
                title: i18n("Privacy")
                iconName: "security-high"
                SettingRow {
                    first: true
                    title: i18n("Share anonymous usage statistics")
                    description: i18n("Helps us see which features to improve. Nothing about you is sent.")
                    SettingSwitch { setting: "shareUsageStats" }
                }
            }

            SettingsCard {
                category: "advanced"
                // D-Bus and KRunner on Linux, sugo:// links on macOS.
                available: AppSettings.externalControlSupported
                title: DesktopStyle.macos ? i18n("Shortcuts and Automation") : i18n("Global Shortcuts and Automation")
                iconName: "configure-shortcuts"
                SettingRow {
                    first: true
                    title: i18n("Allow other programs to control the timer")
                    description: DesktopStyle.macos
                        ? i18n("Scripts, the Shortcuts app and other programs can start, pause, skip and stop it by opening sugo:// links. Turn this off if you only use Sugo's own window and menu bar icon.")
                        : i18n("Global shortcuts, scripts (D-Bus) and KRunner can start, pause, skip and stop it. Turn this off if you only use Sugo's own window and tray.")
                    SettingSwitch { setting: "allowExternalControl" }
                }
                // System Settings' shortcut page exists on KDE Plasma only.
                SettingRow {
                    visible: DesktopStyle.style === "kde"
                    title: i18n("Global shortcuts")
                    description: i18n("Assign keys to Sugo's commands in Plasma's System Settings.")
                    SButton {
                        kind: "ghost"
                        text: i18n("Open Shortcut Settings")
                        iconName: "external"
                        onClicked: AppSettings.openShortcutsSettings()
                    }
                }
            }

            SettingsCard {
                category: "advanced"
                title: i18n("Troubleshooting & Diagnostics")
                iconName: "tools-report-bug"
                SettingRow {
                    first: true
                    title: i18n("Diagnostic report")
                    description: i18n("Generate a redacted report with system information and recent logs to share when reporting an issue.")
                    SButton {
                        labels: [i18n("Copied"), i18n("Copy to Clipboard")]
                        text: page.diagnosticsCopied ? i18n("Copied") : i18n("Copy to Clipboard")
                        iconName: page.diagnosticsCopied ? "check" : "copy"
                        onClicked: {
                            Diagnostics.copyToClipboard();
                            page.showDiagnosticsCopied();
                        }
                    }
                }
                SettingRow {
                    title: i18n("Reset settings")
                    description: i18n("Put the settings back to what they were when you first started Sugo. Your timers, session history and calendar account are kept.")
                    SButton {
                        kind: "ghost"
                        destructive: true
                        text: i18n("Reset Settings…")
                        iconName: "undo"
                        onClicked: resetSettingsDialog.open()
                    }
                }
            }

            // --- About --------------------------------------------------------------------
            // Who we are: the logo, the name, the version and what Sugo is.
            RowLayout {
                visible: page.category === "about"
                Layout.fillWidth: true
                Layout.bottomMargin: Theme.spaceSm
                spacing: Theme.spaceLg

                Image {
                    source: "qrc:/icons/stream.sugo.app.svg"
                    sourceSize.width: Theme.aboutLogo * 2
                    sourceSize.height: Theme.aboutLogo * 2
                    Layout.preferredWidth: Theme.aboutLogo
                    Layout.preferredHeight: Theme.aboutLogo
                    smooth: true
                    mipmap: true
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.hairline
                    Text {
                        Layout.fillWidth: true
                        text: page.aboutData.displayName
                        color: Theme.foreground
                        font.pixelSize: Theme.fontSection
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                        Accessible.role: Accessible.Heading
                    }
                    Text {
                        Layout.fillWidth: true
                        text: page.aboutData.version.length > 0
                              ? i18nc("the version · what the application is", "%1 · %2",
                                      i18n("Version %1", page.aboutData.version), page.aboutData.shortDescription)
                              : page.aboutData.shortDescription
                        color: Theme.muted
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.WordWrap
                    }
                }
            }

            SettingsCard {
                category: "about"
                title: i18n("Updates")
                iconName: "system-software-update"

                // One row: what the updates are doing on the left, in a place of two lines whatever
                // it says, and what can be done about it on the right, so the card keeps its height.
                // The furthest step wins: ready, downloading, an error, a newer version found.
                RowLayout {
                    id: updateRow
                    objectName: "updateMessage"
                    readonly property string step: UpdateChecker.status === UpdateChecker.ReadyToRestart ? "ready"
                        : UpdateChecker.status === UpdateChecker.Downloading ? "downloading"
                        : !page.updateStatusDismissed && UpdateChecker.status === UpdateChecker.Error ? "error"
                        : UpdateChecker.hasUpdate ? "available" : ""
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.settingsRowPaddingY
                    Layout.bottomMargin: Theme.settingsRowPaddingY
                    spacing: Theme.spaceSm

                    Text {
                        id: updateText
                        text: {
                            switch (updateRow.step) {
                            case "ready":
                                // On macOS the new Sugo.app waits next to this one and takes its place when Sugo quits.
                                return DesktopStyle.macos ? i18n("Sugo %1 is ready. It replaces this version when Sugo restarts.", UpdateChecker.latestVersion)
                                                          : i18n("Sugo %1 is installed. It starts after a restart.", UpdateChecker.latestVersion);
                            case "downloading":
                                return i18n("Downloading Sugo %1… %2%", UpdateChecker.latestVersion, Math.round(UpdateChecker.progress * 100));
                            case "error":
                                return i18n("Could not update Sugo: %1", UpdateChecker.errorMessage);
                            case "available":
                                return i18n("A newer version (%1) of Sugo is available.", UpdateChecker.latestVersion);
                            }
                            return page.upToDateShown ? i18n("Sugo is up to date.")
                                 : !UpdateChecker.canInstall ? i18n("Look for a newer version on GitHub.")
                                 : AppSettings.updateChannel === "nightly"
                                   ? (AppSettings.autoCheckUpdates ? i18n("Sugo looks for new nightly builds by itself.")
                                                                     : i18n("Updates come from the nightly builds."))
                                   : (AppSettings.autoCheckUpdates ? i18n("Sugo looks for new releases by itself.")
                                                                     : i18n("Updates come from the stable releases."));
                        }
                        color: updateRow.step === "error" ? Theme.danger
                             : updateRow.step === "available" || updateRow.step === "ready" ? Theme.foreground
                             : page.upToDateShown && updateRow.step === "" ? Theme.positive : Theme.muted
                        font.pixelSize: Theme.fontBody
                        wrapMode: Text.WordWrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                        Layout.fillWidth: true
                        Layout.preferredHeight: statusMetrics.lineSpacing * 2
                        Accessible.role: updateRow.step.length > 0 ? Accessible.AlertMessage : Accessible.StaticText
                        Accessible.name: text
                        // A longer error is cut after two lines: all of it on hover.
                        QQC2.ToolTip.text: text
                        QQC2.ToolTip.visible: updateTextHover.hovered && truncated
                        QQC2.ToolTip.delay: Theme.toolTipDelay
                        HoverHandler { id: updateTextHover }
                        FontMetrics {
                            id: statusMetrics
                            font.pixelSize: Theme.fontBody
                        }
                    }

                    SButton {
                        visible: updateRow.step === "" || updateRow.step === "downloading"
                        iconName: page.upToDateShown ? "check" : "download"
                        text: UpdateChecker.checking ? i18n("Checking for updates…") : i18n("Check for Updates")
                        labels: [i18n("Checking for updates…"), i18n("Check for Updates")]
                        enabled: !UpdateChecker.checking && updateRow.step === ""
                        onClicked: {
                            page.updateStatusDismissed = false;
                            UpdateChecker.checkForUpdates();
                        }
                    }
                    SButton {
                        visible: updateRow.step === "available"
                        kind: UpdateChecker.installable ? "ghost" : "primary"
                        iconName: "external"
                        text: i18n("View Release")
                        onClicked: Qt.openUrlExternally(UpdateChecker.releaseUrl)
                    }
                    SButton {
                        visible: updateRow.step === "available" && UpdateChecker.installable
                        kind: "primary"
                        text: i18n("Install")
                        onClicked: UpdateChecker.installUpdate()
                    }
                    SButton {
                        visible: updateRow.step === "ready"
                        kind: "primary"
                        text: i18n("Restart Now")
                        onClicked: applicationWindow().requestRestart()
                    }
                    SButton {
                        visible: updateRow.step === "error"
                        kind: "ghost"
                        text: i18n("Open Releases Page")
                        onClicked: Qt.openUrlExternally("https://github.com/sugoHQ/sugo/releases")
                    }
                    SButton {
                        visible: updateRow.step === "error"
                        kind: "ghost"
                        iconOnly: true
                        iconName: "close"
                        text: i18n("Dismiss")
                        onClicked: page.updateStatusDismissed = true
                    }
                }
            }

            SettingsCard {
                category: "about"
                title: i18n("Links")
                iconName: "external"
                LinkRow {
                    first: true
                    text: i18n("Report a bug")
                    url: "https://github.com/sugoHQ/sugo/issues"
                }
                LinkRow {
                    text: i18n("Project website")
                    url: "https://sugo.stream"
                }
            }

            SettingsCard {
                category: "about"
                title: i18n("License & credits")
                iconName: "license"
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.settingsRowPaddingY
                    Layout.bottomMargin: Theme.settingsRowPaddingY
                    spacing: Theme.hairline
                    Text {
                        Layout.fillWidth: true
                        text: page.aboutData.copyrightStatement
                        visible: text.length > 0
                        color: Theme.foreground
                        font.pixelSize: Theme.fontBody
                        wrapMode: Text.WordWrap
                    }
                    SmallText {
                        text: i18n("Licensed under GNU General Public License, version 3 or later.") + " "
                              + i18n("Sugo is an independent, open source project.")
                    }
                }
            }
        }
    }

        FileDialog {
            id: backupDialog
            title: i18n("Back up Sugo database")
            fileMode: FileDialog.SaveFile
            defaultSuffix: "db"
            nameFilters: [i18n("SQLite database (*.db)")]
            onAccepted: DataManager.backupDatabase(selectedFile)
        }

        FileDialog {
            id: restoreDialog
            title: i18n("Choose a Sugo backup")
            fileMode: FileDialog.OpenFile
            nameFilters: [i18n("SQLite database (*.db)")]
            onAccepted: restoreConfirmDialog.open()
        }

}
