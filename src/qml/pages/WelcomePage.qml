// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import stream.sugo.app

/**
 * The tour of the first start, in four steps: the logo comes in and says what Sugo is; how a
 * timer works; a few first choices (weekends, tray, login; on macOS the Focus shortcuts); ready. Finishing or skipping it marks
 * the first run as done and opens the focus page. The arrow keys move between the steps.
 */
QQC2.Page {
    id: page

    title: i18n("Welcome")
    background: null
    focus: true

    readonly property int count: 4
    property int current: 0
    /// 1 when moving forwards, -1 backwards: where the next step comes in from.
    property int direction: 0
    /// The logo's entrance runs once, the first time the tour opens.
    property bool introPlayed: false
    property string autostartError: ""
    /// What the start-at-login switch last asked for (undefined before any attempt).
    property var autostartWanted: undefined
    /// The tour starts once the window is on screen and the start-up work is over, so that the
    /// logo's entrance is not spent before anyone can see it.
    property bool started: false
    /// macOS: the user's shortcuts, as last read, and whether both Focus shortcuts are there.
    property var focusShortcutNames: AppSettings.focusShortcutNames()
    readonly property bool focusShortcutsFound: focusShortcutNames.indexOf(AppSettings.focusOnShortcut) >= 0
                                                && focusShortcutNames.indexOf(AppSettings.focusOffShortcut) >= 0

    Timer {
        interval: Theme.durationShort
        running: !page.started && page.visible && !!page.Window.window && page.Window.window.visible
        onTriggered: {
            page.started = true;
            // For the arrow keys.
            page.forceActiveFocus();
        }
    }

    function go(index: int): void {
        if (index < 0 || index >= count || index === current) {
            return;
        }
        direction = index > current ? 1 : -1;
        current = index;
    }

    /// The primary button: the next step, or out of the tour from the last one.
    function advance(): void {
        if (current === count - 1) {
            finish(false);
        } else {
            go(current + 1);
        }
    }

    function finish(skipped: bool): void {
        end(skipped);
        applicationWindow().navigate("timer");
    }

    // The tour is over (the usage statistics, if they are on, note where and how).
    function end(skipped: bool): void {
        if (!AppSettings.firstRunCompleted) {
            Telemetry.tourEnded(page.current, skipped);
        }
        AppSettings.firstRunCompleted = true;
    }

    Keys.onRightPressed: go(current + 1)
    Keys.onLeftPressed: go(current - 1)
    Keys.onReturnPressed: advance()
    Keys.onEnterPressed: advance()

    Connections {
        target: AppSettings
        // Not cleared on every startAtLoginChanged: AppSettings announces that after a failure too,
        // right before this message. A new attempt clears it (the switch's `applying`), and so does
        // the entry reaching what was asked for after all (the Background portal answering late).
        function onAutostartFailed(message) {
            page.autostartError = message;
        }
        function onStartAtLoginChanged() {
            if (AppSettings.startAtLogin === page.autostartWanted) {
                page.autostartError = "";
            }
        }
        function onFocusShortcutNamesChanged() {
            page.focusShortcutNames = AppSettings.focusShortcutNames();
        }
    }

    // --- Pieces --------------------------------------------------------------------------------

    /**
     * One step of the tour: centred when it fits, scrollable when the window is too short. It
     * slides in from the side the tour moves to, and its elements come in one after the other:
     * each binds its opacity to `reveal(order)` and its offset to `rise(order)`.
     */
    component Step: Flickable {
        id: step
        required property int index
        default property alias content: column.data
        readonly property bool active: page.started && page.current === index
        /// Milliseconds since the step came in: the clock of the staggered entrance.
        property real clock: 0
        /// Before the first element comes in (the logo's entrance, on the first step).
        property int delay: 0
        /// How wide the step's column may grow.
        property int groupWidth: Theme.welcomeGroupWidth
        property real shift: 0
        signal entered()

        /// How far the element `order` (0, 1, 2…) has come in, 0..1, eased.
        function reveal(order: real): real {
            return revealAt(order * Theme.durationStagger, Theme.durationLong);
        }
        /// The same for a part starting `start` ms after the delay and lasting `length` ms.
        function revealAt(start: real, length: real): real {
            const t = Math.max(0, Math.min(1, (clock - delay - start) / length));
            return 1 - Math.pow(1 - t, 3);
        }
        /// The vertical offset that goes with `reveal(order)`.
        function rise(order: real): real {
            return (1 - reveal(order)) * Theme.spaceMd;
        }

        visible: opacity > 0
        opacity: 0
        clip: true
        contentWidth: width
        contentHeight: Math.max(height, column.implicitHeight + Theme.space3xl * 2)
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height
        QQC2.ScrollBar.vertical: QQC2.ScrollBar {}
        transform: Translate { x: step.shift }

        onActiveChanged: {
            if (active) {
                leave.stop();
                enter.restart();
            } else {
                enter.stop();
                leave.restart();
            }
        }

        SequentialAnimation {
            id: enter
            ScriptAction {
                script: {
                    step.contentY = 0;
                    step.clock = 0;
                    step.shift = Theme.space2xl * page.direction;
                    step.entered();
                }
            }
            // Leaves room for the step going out.
            PauseAnimation { duration: page.direction !== 0 ? Theme.durationShort : 0 }
            ParallelAnimation {
                NumberAnimation { target: step; property: "opacity"; to: 1; duration: Theme.durationLong; easing.type: Easing.OutCubic }
                NumberAnimation { target: step; property: "shift"; to: 0; duration: Theme.durationLong; easing.type: Easing.OutCubic }
                NumberAnimation { target: step; property: "clock"; from: 0; to: 4000; duration: 4000 }
            }
        }
        ParallelAnimation {
            id: leave
            NumberAnimation { target: step; property: "opacity"; to: 0; duration: Theme.durationShort; easing.type: Easing.InCubic }
            NumberAnimation { target: step; property: "shift"; to: -Theme.space2xl * page.direction; duration: Theme.durationShort; easing.type: Easing.InCubic }
        }

        ColumnLayout {
            id: column
            width: Math.min(step.width - Theme.space2xl * 2, step.groupWidth)
            x: (step.width - width) / 2
            y: Math.max(Theme.space3xl, (step.height - implicitHeight) / 2)
            spacing: 0
        }
    }

    /// The title of a step.
    component StepTitle: Text {
        Layout.fillWidth: true
        color: Theme.foreground
        font.pixelSize: Theme.fontWelcomeTitle
        font.weight: Font.DemiBold
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
    }

    /// The text under a step's title: muted, in a narrow column.
    component StepText: Text {
        Layout.fillWidth: true
        Layout.maximumWidth: Theme.welcomeTextWidth
        Layout.alignment: Qt.AlignHCenter
        color: Theme.muted
        font.pixelSize: Theme.fontBody
        lineHeight: 1.15
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
    }

    /// The title and the text at the top of a step.
    component StepHeader: ColumnLayout {
        id: header
        required property Step step
        property string title
        property string text
        Layout.fillWidth: true
        spacing: Theme.spaceLg

        StepTitle {
            text: header.title
            opacity: header.step.reveal(1)
            transform: Translate { y: header.step.rise(1) }
        }
        StepText {
            text: header.text
            opacity: header.step.reveal(2)
            transform: Translate { y: header.step.rise(2) }
        }
    }

    /// A card of the interface: neutral, 1 px border.
    component Panel: Rectangle {
        Layout.fillWidth: true
        radius: Theme.radiusLg
        color: Theme.card
        border.width: Theme.hairline
        border.color: Theme.border
    }

    /// A small card: a picture on the left, a title and a line of text.
    component Feature: Panel {
        id: feature
        property string title
        property string text
        default property alias picture: pictureHolder.data
        implicitHeight: featureRow.implicitHeight + Theme.settingsRowPaddingY * 2
        RowLayout {
            id: featureRow
            anchors.fill: parent
            anchors.leftMargin: Theme.settingsRowPaddingX
            anchors.rightMargin: Theme.settingsRowPaddingX
            anchors.topMargin: Theme.settingsRowPaddingY
            anchors.bottomMargin: Theme.settingsRowPaddingY
            spacing: Theme.spaceLg
            Item {
                id: pictureHolder
                implicitWidth: Theme.iconLg
                implicitHeight: Theme.iconLg
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.hairline * 2
                Text {
                    Layout.fillWidth: true
                    text: feature.title
                    color: Theme.foreground
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.Medium
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    text: feature.text
                    color: Theme.muted
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.WordWrap
                }
            }
        }
    }

    // A switch bound to a setting, named after its row for screen readers. Clicking it drops the
    // binding; binding again afterwards shows the real value, e.g. when the change was refused.
    component SettingSwitch: SSwitch {
        id: sw
        required property string setting
        required property FormRow row
        readonly property string accessibleName: row.title
        Accessible.name: accessibleName
        Accessible.description: row.description
        checked: AppSettings[setting]
        // Just before the new value goes to AppSettings: the page clears an earlier attempt's message.
        signal applying()
        onToggled: {
            sw.applying();
            AppSettings[setting] = checked;
            checked = Qt.binding(() => AppSettings[sw.setting]);
        }
    }

    // --- The steps -----------------------------------------------------------------------------

    Item {
        id: stage
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: bottomBar.top

        // 1 · Welcome: the logo comes in, then the name and what Sugo does.
        Step {
            id: welcomeStep
            index: 0
            anchors.fill: parent
            onEntered: {
                if (page.introPlayed) {
                    delay = 0;
                    logo.finish();
                } else {
                    page.introPlayed = true;
                    delay = logo.textDelay;
                    logo.play();
                }
            }

            LogoIntro {
                id: logo
                objectName: "welcomeLogo"
                size: Theme.logoWelcome
                Layout.alignment: Qt.AlignHCenter
                Layout.bottomMargin: Theme.space2xl
            }
            StepTitle {
                text: i18n("Sugo")
                opacity: welcomeStep.reveal(0)
                transform: Translate { y: welcomeStep.rise(0) }
            }
            StepText {
                Layout.topMargin: Theme.spaceLg
                text: i18n("A calm Pomodoro timer. Focus in rounds, rest in between, and see where your time goes.")
                opacity: welcomeStep.reveal(1)
                transform: Translate { y: welcomeStep.rise(1) }
            }
            StepText {
                Layout.topMargin: Theme.spaceLg
                text: i18n("No account, no tracking: everything stays on this computer.")
                color: Theme.subtle
                font.pixelSize: Theme.fontCaption
                opacity: welcomeStep.reveal(2)
                transform: Translate { y: welcomeStep.rise(2) }
            }
        }

        // 2 · Timers: the cycle of the timer in use, the Pomodoro to start from, tray and silence.
        Step {
            id: timersStep
            index: 1
            anchors.fill: parent

            // The cycle of the timer in use: work, break, work… and the long break at the end.
            readonly property var segments: {
                const cycles = TimerEngine.cyclesBeforeLong;
                const work = Math.max(1, TimerEngine.workSeconds);
                const list = [];
                for (let i = 0; i < Math.max(1, cycles); ++i) {
                    list.push({ kind: "work", seconds: work });
                    if (i === Math.max(1, cycles) - 1 && cycles > 0 && TimerEngine.longBreakSeconds > 0) {
                        list.push({ kind: "long", seconds: TimerEngine.longBreakSeconds });
                    } else if (TimerEngine.shortBreakSeconds > 0) {
                        list.push({ kind: "break", seconds: TimerEngine.shortBreakSeconds });
                    }
                }
                let total = 0;
                for (const s of list) {
                    total += s.seconds;
                }
                let start = 0;
                for (const s of list) {
                    s.start = start / total;
                    s.share = s.seconds / total;
                    start += s.seconds;
                }
                return list;
            }
            function minutes(seconds: int): string {
                return i18nc("minutes", "%1 min", Math.max(1, Math.round(seconds / 60)));
            }

            StepHeader {
                step: timersStep
                title: i18n("Work in rounds, rest in between")
                // macOS does not let applications silence notifications (Focus modes do that).
                text: DesktopStyle.macos
                      ? i18n("A timer alternates focus and short breaks, with a longer break every few rounds. You start with a classic Pomodoro; add your own from the timer menu on the Focus page.")
                      : i18n("A timer alternates focus and short breaks, with a longer break every few rounds. You start with a classic Pomodoro that keeps notifications quiet while you work; add your own from the timer menu on the Focus page.")
            }

            Panel {
                Layout.topMargin: Theme.space2xl
                implicitHeight: cycleColumn.implicitHeight + Theme.spaceLg * 2
                opacity: timersStep.reveal(3)
                transform: Translate { y: timersStep.rise(3) }

                ColumnLayout {
                    id: cycleColumn
                    anchors.fill: parent
                    anchors.margins: Theme.spaceLg
                    anchors.leftMargin: Theme.settingsRowPaddingX
                    anchors.rightMargin: Theme.settingsRowPaddingX
                    spacing: Theme.spaceMd + Theme.spaceXs

                    Item {
                        id: cycleBar
                        readonly property real gap: Theme.hairline * 3
                        readonly property real usable: width - gap * (timersStep.segments.length - 1)
                        Layout.fillWidth: true
                        implicitHeight: Theme.spaceMd
                        Accessible.role: Accessible.Graphic
                        Accessible.name: i18n("The cycle of the timer in use")

                        Repeater {
                            model: timersStep.segments
                            delegate: Rectangle {
                                id: segment
                                required property var modelData
                                required property int index
                                x: modelData.start * cycleBar.usable + index * cycleBar.gap
                                width: Math.max(Theme.hairline, modelData.share * cycleBar.usable)
                                height: cycleBar.height
                                radius: height / 2
                                color: modelData.kind === "long" ? Theme.breakDeep : Theme.breakColor
                                gradient: modelData.kind === "work" ? workGradient : null
                                // The segments grow from the left one after the other.
                                transform: Scale {
                                    xScale: timersStep.revealAt(Theme.durationLong + segment.index * Theme.durationStagger, Theme.durationLong)
                                }
                                Gradient {
                                    id: workGradient
                                    orientation: Gradient.Horizontal
                                    GradientStop { position: 0; color: Theme.brandPeach }
                                    GradientStop { position: 0.6; color: Theme.brand }
                                    GradientStop { position: 1; color: Theme.brandMagenta }
                                }
                            }
                        }
                    }

                    Flow {
                        Layout.fillWidth: true
                        spacing: Theme.spaceXl
                        Repeater {
                            model: [
                                { color: Theme.brand, value: timersStep.minutes(TimerEngine.workSeconds), label: i18n("focus"), shown: true },
                                { color: Theme.breakColor, value: timersStep.minutes(TimerEngine.shortBreakSeconds), label: i18n("break"),
                                  shown: TimerEngine.shortBreakSeconds > 0 },
                                { color: Theme.breakDeep, value: timersStep.minutes(TimerEngine.longBreakSeconds), label: i18n("long break"),
                                  shown: TimerEngine.cyclesBeforeLong > 0 && TimerEngine.longBreakSeconds > 0 }
                            ]
                            delegate: RowLayout {
                                required property var modelData
                                visible: modelData.shown
                                spacing: Theme.spaceSm
                                Rectangle {
                                    implicitWidth: Theme.spaceSm
                                    implicitHeight: Theme.spaceSm
                                    radius: width / 2
                                    color: modelData.color
                                }
                                Text {
                                    text: modelData.value
                                    color: Theme.foreground
                                    font.pixelSize: Theme.fontCaption
                                    font.weight: Font.Medium
                                    font.features: { "tnum": 1 }
                                }
                                Text {
                                    text: modelData.label
                                    color: Theme.muted
                                    font.pixelSize: Theme.fontCaption
                                }
                            }
                        }
                    }
                }
            }

            Panel {
                Layout.topMargin: Theme.spaceLg
                implicitHeight: presetColumn.implicitHeight

                ColumnLayout {
                    id: presetColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    spacing: 0

                    Repeater {
                        model: [
                            { name: i18n("Pomodoro"), category: i18n("Work"), icon: "sugo-timer", work: 25, rest: 5 }
                        ]
                        delegate: Item {
                            id: presetRow
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            implicitHeight: Theme.controlHeightLg + Theme.spaceXs
                            opacity: timersStep.reveal(4 + index)
                            transform: Translate { y: timersStep.rise(4 + presetRow.index) }

                            Rectangle {
                                visible: presetRow.index > 0
                                width: parent.width
                                height: Theme.hairline
                                color: Theme.border
                            }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.settingsRowPaddingX
                                anchors.rightMargin: Theme.settingsRowPaddingX
                                spacing: Theme.spaceLg
                                TimerIcon {
                                    iconName: presetRow.modelData.icon
                                    size: Theme.iconMd + Theme.spaceXs
                                    color: Theme.accent
                                }
                                Text {
                                    text: presetRow.modelData.name
                                    color: Theme.foreground
                                    font.pixelSize: Theme.fontBody
                                    font.weight: Font.Medium
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: presetRow.modelData.category
                                    color: Theme.muted
                                    font.pixelSize: Theme.fontCaption
                                    elide: Text.ElideRight
                                }
                                Text {
                                    text: i18nc("work and break minutes of a timer", "%1 / %2 min", presetRow.modelData.work, presetRow.modelData.rest)
                                    color: Theme.muted
                                    font.pixelSize: Theme.fontCaption
                                    font.features: { "tnum": 1 }
                                }
                            }
                        }
                    }
                }
            }

            GridLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceLg
                columns: width >= Theme.welcomeGroupWidth ? 2 : 1
                columnSpacing: Theme.spaceLg
                rowSpacing: Theme.spaceLg

                Feature {
                    title: DesktopStyle.macos ? i18n("Live menu bar icon") : i18n("Live tray icon")
                    text: DesktopStyle.macos ? i18n("The minutes left, right in the menu bar.") : i18n("The minutes left, right in the panel.")
                    opacity: timersStep.reveal(7)
                    transform: Translate { y: timersStep.rise(7) }

                    TrayBadge {
                        id: trayDial
                        objectName: "trayDialPreview"
                        width: Theme.iconLg
                        // A time-lapse of a work session and a break, while the step is on screen.
                        SequentialAnimation {
                            loops: Animation.Infinite
                            running: timersStep.active && page.visible
                            PropertyAction { target: trayDial; property: "isBreak"; value: false }
                            NumberAnimation { target: trayDial; property: "progress"; from: 0; to: 1; duration: Theme.durationLong * 14 }
                            PauseAnimation { duration: Theme.durationLong * 2 }
                            PropertyAction { target: trayDial; property: "isBreak"; value: true }
                            NumberAnimation { target: trayDial; property: "progress"; from: 0; to: 1; duration: Theme.durationLong * 7 }
                            PauseAnimation { duration: Theme.durationLong * 2 }
                        }
                    }
                }
                Feature {
                    title: i18n("Quiet while you work")
                    text: i18n("Notifications can wait for the break.")
                    opacity: timersStep.reveal(8)
                    transform: Translate { y: timersStep.rise(8) }

                    Rectangle {
                        anchors.fill: parent
                        radius: Theme.radius
                        color: "transparent"
                        border.width: Theme.hairline
                        border.color: Theme.border
                        LineIcon {
                            anchors.centerIn: parent
                            name: "bell-off"
                        }
                    }
                }
            }
        }

        // 3 · Set up: the first choices, all of them also in Settings.
        Step {
            id: setupStep
            index: 2
            anchors.fill: parent

            StepHeader {
                step: setupStep
                title: i18n("Make it yours")
                text: i18n("A few choices to start with. You can change all of them later in Settings.")
            }

            // One group of rows, as in Settings.
            Panel {
                objectName: "welcomeSetupPanel"
                Layout.topMargin: Theme.space2xl
                implicitHeight: settingsColumn.implicitHeight

                ColumnLayout {
                    id: settingsColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: Theme.settingsRowPaddingX
                    anchors.rightMargin: Theme.settingsRowPaddingX
                    spacing: 0

                    FormRow {
                        id: weekendRow
                        first: true
                        title: i18n("Protect weekend streaks")
                        description: i18n("A weekend without sessions does not end your streak.")
                        opacity: setupStep.reveal(3)
                        transform: Translate { y: setupStep.rise(3) }
                        SettingSwitch { setting: "protectWeekendStreak"; row: weekendRow }
                    }
                    FormRow {
                        id: trayRow
                        visible: AppSettings.trayAvailable
                        title: DesktopStyle.macos ? i18n("Show an icon in the menu bar") : i18n("Show an icon in the tray")
                        description: i18n("While a timer runs, it shows the phase and the minutes left.")
                        opacity: setupStep.reveal(4)
                        transform: Translate { y: setupStep.rise(4) }
                        SettingSwitch { setting: "showTrayIcon"; row: trayRow }
                    }
                    FormRow {
                        id: closeRow
                        // On macOS closing the window always hides it.
                        visible: AppSettings.trayAvailable && !DesktopStyle.nativeChrome
                        enabled: AppSettings.showTrayIcon
                        title: i18n("Keep running in the tray")
                        description: i18n("Closing the window leaves the timer running.")
                        opacity: setupStep.reveal(5)
                        transform: Translate { y: setupStep.rise(5) }
                        SettingSwitch { setting: "closeToTray"; row: closeRow }
                    }
                    FormRow {
                        id: loginRow
                        visible: AppSettings.autostartSupported
                        title: i18n("Start at login")
                        description: i18n("Sugo is ready as soon as you log in.")
                        opacity: setupStep.reveal(6)
                        transform: Translate { y: setupStep.rise(6) }
                        SettingSwitch {
                            setting: "startAtLogin"
                            row: loginRow
                            onApplying: {
                                page.autostartError = "";
                                page.autostartWanted = checked;
                            }
                        }
                    }
                    // macOS: the two shortcuts that turn Do Not Disturb on and off while working.
                    // Optional: the tour goes on without them.
                    FormRow {
                        id: focusRow
                        visible: DesktopStyle.macos
                        title: i18n("Silence notifications with a Focus")
                        description: focusStatus.description(AppSettings.focusShortcutsSupported)
                        // Every state's text: the row keeps the height of the tallest.
                        descriptionVariants: focusStatus.descriptionVariants(AppSettings.focusShortcutsSupported)
                        opacity: setupStep.reveal(7)
                        transform: Translate { y: setupStep.rise(7) }
                        SButton {
                            objectName: "welcomeInstallFocusShortcuts"
                            text: i18n("Install Focus Shortcuts")
                            enabled: AppSettings.focusShortcutsInstallable && !page.focusShortcutsFound && !focusStatus.busy
                            Accessible.description: focusRow.description
                            onClicked: AppSettings.installFocusShortcuts()
                        }
                    }
                    // Opt-in: off until the user turns it on (docs/USAGE.md lists what is sent).
                    FormRow {
                        id: usageStatsRow
                        objectName: "welcomeUsageStatsRow"
                        title: i18n("Share anonymous usage statistics")
                        description: i18n("Helps us see which features to improve. Nothing about you is sent.")
                        opacity: setupStep.reveal(8)
                        transform: Translate { y: setupStep.rise(8) }
                        SButton {
                            objectName: "welcomeUsageStatsDetails"
                            kind: "ghost"
                            text: i18n("What is sent")
                            iconName: "external"
                            Accessible.description: i18n("Opens the list of what the usage statistics send, in your browser.")
                            onClicked: Qt.openUrlExternally("https://github.com/sugoHQ/sugo/blob/main/docs/USAGE.md#anonymous-usage-statistics")
                        }
                        SettingSwitch { setting: "shareUsageStats"; row: usageStatsRow }
                    }
                }
            }

            FocusShortcutsStatus {
                id: focusStatus
                found: page.focusShortcutsFound
            }

            // The login entry's error: in a place of its own below the card, so the step does not
            // move when it comes or goes.
            MessageSlot {
                objectName: "welcomeSetupMessage"
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceMd
                shown: page.autostartError.length > 0
                tone: "danger"
                text: page.autostartError
            }
        }

        // 4 · Ready: the ring closes, a check, and the focus page's composer to pick a timer and start.
        Step {
            id: readyStep
            index: 3
            anchors.fill: parent
            groupWidth: Theme.composerWidth

            Item {
                Layout.alignment: Qt.AlignHCenter
                Layout.bottomMargin: Theme.space2xl
                implicitWidth: Theme.logoWelcome
                implicitHeight: implicitWidth

                BrandGlow {
                    anchors.centerIn: parent
                    width: parent.width * 2
                    height: width
                    opacity: 0.6 * readyStep.revealAt(0, Theme.durationLong * 3)
                    scale: 0.7 + 0.3 * readyStep.revealAt(0, Theme.durationLong * 3)
                }
                ProgressRing {
                    id: ring
                    anchors.fill: parent
                    lineWidth: Theme.spaceSm
                    animated: false
                    progress: {
                        // Ease in and out over three long durations.
                        const t = Math.max(0, Math.min(1, (readyStep.clock - Theme.durationStagger) / (Theme.durationLong * 3)));
                        return t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;
                    }
                    // The whole ring gives a small beat as the check appears.
                    scale: 1 + 0.06 * Math.sin(Math.PI * readyStep.revealAt(Theme.durationLong * 3, Theme.durationLong * 1.5))

                    LineIcon {
                        anchors.centerIn: parent
                        name: "check"
                        size: Theme.iconXl
                        color: Theme.foreground
                        readonly property real pop: readyStep.revealAt(Theme.durationLong * 3 - Theme.durationStagger, Theme.durationLong)
                        opacity: pop
                        scale: 0.5 + 0.5 * pop + 0.15 * Math.sin(Math.PI * pop)
                    }
                }
            }

            StepTitle {
                text: i18n("You're all set")
                opacity: readyStep.reveal(8)
                transform: Translate { y: readyStep.rise(8) }
            }
            StepText {
                Layout.topMargin: Theme.spaceLg
                text: i18n("Your timer is ready. Sessions and streaks will show up in Statistics.")
                opacity: readyStep.reveal(9)
                transform: Translate { y: readyStep.rise(9) }
            }
            // Starting the timer here ends the tour, on the focus page.
            Composer {
                id: readyComposer
                Layout.fillWidth: true
                Layout.topMargin: Theme.space2xl
                app: composerApp
                compact: page.width < Theme.compactBreakpoint
                opacity: readyStep.reveal(10)
                transform: Translate { y: readyStep.rise(10) }
                onStartPressed: {
                    if (TimerEngine.active && !TimerEngine.paused) {
                        page.finish(false);
                    }
                }
            }
        }
    }

    // The window, as the composer of the last step sees it: what leads out of the tour from its
    // menus (a new timer, the settings) ends the tour first, as Skip Tour does.
    QtObject {
        id: composerApp
        function chooseTimer(uuid: string): void {
            applicationWindow().chooseTimer(uuid);
        }
        function newTimer(): void {
            page.end(false);
            applicationWindow().newTimer();
        }
        function openSettings(category: string): void {
            page.end(false);
            applicationWindow().openSettings(category);
        }
        function requestEndPhase(skipping: bool): bool {
            return applicationWindow().requestEndPhase(skipping);
        }
    }

    // --- Bottom bar: skip at the left; the step dots, centred; back and next at the right ------

    // Opaque, so that a scrolled step never shows through it (the software renderer does not
    // clip shapes).
    Rectangle {
        id: bottomBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        implicitHeight: Theme.welcomeFooterHeight
        height: implicitHeight
        color: Theme.background

        Rectangle {
            width: parent.width
            height: Theme.hairline
            color: Theme.border
        }

        SButton {
            id: skipButton
            anchors.left: parent.left
            anchors.leftMargin: Theme.welcomeFooterPaddingX
            anchors.verticalCenter: parent.verticalCenter
            kind: "ghost"
            text: i18n("Skip Tour")
            visible: page.current < page.count - 1
            onClicked: page.finish(true)
        }

        Row {
            id: indicator
            objectName: "welcomeIndicator"
            readonly property int count: page.count
            // Only ever follows the steps; the dots move the steps themselves.
            readonly property int currentIndex: page.current
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spaceSm
            // Hidden when the window is too narrow to fit the dots between the buttons.
            visible: bottomBar.width - 2 * Theme.welcomeFooterPaddingX - skipButton.implicitWidth
                     - navButtons.implicitWidth - navButtons.spacing > implicitWidth

            Repeater {
                model: indicator.count
                delegate: Item {
                    id: dot
                    required property int index
                    objectName: "welcomeStepDot-" + index
                    readonly property bool active: index === indicator.currentIndex
                    readonly property string accessibleName: i18nc("@action:button welcome tour", "Step %1 of %2", index + 1, indicator.count)
                    function activate(): void {
                        page.go(index);
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: accessibleName
                    Accessible.onPressAction: activate()
                    implicitWidth: Theme.phaseDot
                    implicitHeight: Theme.spaceXl
                    TapHandler {
                        // A dot is small: the tap counts a little around it too.
                        margin: Theme.spaceXs
                        onTapped: dot.activate()
                    }
                    HoverHandler {
                        margin: Theme.spaceXs
                        cursorShape: Qt.PointingHandCursor
                    }
                    Rectangle {
                        anchors.centerIn: parent
                        width: Theme.phaseDot
                        height: Theme.phaseDot
                        radius: height / 2
                        color: dot.active ? Theme.accent : Theme.borderStrong
                        Behavior on color { ColorAnimation { duration: Theme.durationLong } }
                    }
                }
            }
        }
        RowLayout {
            id: navButtons
            anchors.right: parent.right
            anchors.rightMargin: Theme.welcomeFooterPaddingX
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spaceMd

            SButton {
                id: backButton
                kind: "ghost"
                iconName: "chevron-left"
                text: i18n("Back")
                visible: page.current > 0
                onClicked: page.go(page.current - 1)
            }
            // Next, and Done on the last step (whose composer starts the timer): always in the same
            // place and of the same width, so that neither it nor Back moves from one step to the next.
            SButton {
                id: primaryButton
                objectName: "welcomePrimaryButton"
                readonly property bool last: page.current === page.count - 1
                Layout.preferredWidth: Math.max(nextSize.implicitWidth, doneSize.implicitWidth)
                kind: "primary"
                text: last ? doneSize.text : nextSize.text
                onClicked: page.advance()
            }
            // Not shown (layouts skip hidden items): they only measure the two labels.
            SButton {
                id: nextSize
                visible: false
                kind: "primary"
                text: i18n("Next")
            }
            SButton {
                id: doneSize
                visible: false
                kind: "primary"
                text: i18n("Done")
            }
        }
    }
}
