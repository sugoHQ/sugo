// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import stream.sugo.app

/**
 * The bar at the top of the window: the logo and the name at the left, the three tabs (Focus,
 * Statistics, Settings) in the middle of the window, search and the desktop's window buttons at
 * the right. The window has no system title bar on Linux: this bar moves it (drag) and maximises
 * it (double click). On macOS the system's transparent title bar lies over it, as tall as it,
 * with the traffic lights at its left.
 */
Rectangle {
    id: bar

    /// The destination shown: "timer", "stats", "settings", "preset", "about" or "welcome".
    property string current
    /// Only the logo, the name and the window buttons (the welcome tour).
    property bool bare: false
    property bool maximized: false
    /// A narrow window: the name is hidden, the tabs are tighter.
    readonly property bool compact: width < Theme.compactBreakpoint

    signal tabRequested(string name)
    signal searchRequested()
    // The window has no system title bar: this bar moves it and carries its buttons.
    signal moveRequested()
    signal toggleMaximizeRequested()
    signal minimizeRequested()
    signal closeRequested()

    /// The tab of a destination: a timer's page and About belong to Settings.
    readonly property string currentTab: current === "timer" ? "timer"
                                        : current === "stats" ? "stats"
                                        : current === "settings" || current === "preset" || current === "about" ? "settings" : ""

    /// Full screen: macOS hides the traffic lights, so the bar leaves no room for them.
    property bool fullScreen: false

    // On macOS the bar is as tall as the system's title bar (a compact toolbar's, MacApp.mm), so
    // its middle is that of the traffic lights; in full screen the system has none there.
    implicitHeight: DesktopStyle.nativeChrome && !bar.fullScreen && bar.SafeArea.margins.top > 0
                    ? bar.SafeArea.margins.top : Theme.titleBarHeight
    color: Theme.background

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: Theme.hairline
        color: Theme.border
    }

    DragHandler {
        target: null
        onActiveChanged: if (active) bar.moveRequested()
    }
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onDoubleTapped: bar.toggleMaximizeRequested()
    }

    // --- The time left, for the Focus tab while a timer runs ---------------------------------

    function clockText(total: int): string {
        const h = Math.floor(total / 3600);
        const m = Math.floor((total % 3600) / 60);
        const s = total % 60;
        const pad = n => String(n).padStart(2, "0");
        return h > 0 ? h + ":" + pad(m) + ":" + pad(s) : pad(m) + ":" + pad(s);
    }
    readonly property string clock: TimerEngine.active ? clockText(TimerEngine.remainingSeconds) : ""
    /// "Work · 12:04 left", for the tooltip and screen readers.
    readonly property string statusText: {
        if (!TimerEngine.active) {
            return "";
        }
        const phase = TimerEngine.phase === TimerEngine.Phase.Work ? i18n("Work")
                    : TimerEngine.phase === TimerEngine.Phase.ShortBreak ? i18n("Short break") : i18n("Long break");
        return TimerEngine.paused ? i18nc("phase · time", "%1 · paused at %2", phase, bar.clock)
                                  : i18nc("phase · time", "%1 · %2 left", phase, bar.clock);
    }
    /// The widest the time gets with this timer: an hour or more takes "0:00:00".
    readonly property string widestClock: Math.max(TimerEngine.workSeconds, TimerEngine.shortBreakSeconds,
                                                   TimerEngine.longBreakSeconds) >= 3600 ? "0:00:00" : "00:00"

    // --- Left: the window buttons the desktop puts there, the logo and the name --------------

    RowLayout {
        id: leftGroup
        objectName: "titleBarBrand"
        anchors.left: parent.left
        anchors.leftMargin: DesktopStyle.nativeChrome && !bar.fullScreen ? Theme.titleBarMacInset : Theme.titleBarPaddingLeft
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spaceMd

        WindowButtons {
            objectName: "titleBarLeftButtons"
            visible: DesktopStyle.leftButtons.length > 0
            Layout.rightMargin: Theme.spaceXs
            buttons: DesktopStyle.leftButtons
            maximized: bar.maximized
            barHeight: bar.height - Theme.hairline
            onMinimizeRequested: bar.minimizeRequested()
            onMaximizeRequested: bar.toggleMaximizeRequested()
            onCloseRequested: bar.closeRequested()
        }
        Image {
            source: "qrc:/icons/stream.sugo.app.svg"
            sourceSize.width: Theme.logoSmall * 2
            sourceSize.height: Theme.logoSmall * 2
            Layout.preferredWidth: Theme.logoSmall
            Layout.preferredHeight: Theme.logoSmall
            smooth: true
            mipmap: true
        }
        Text {
            visible: !bar.compact || bar.bare
            text: i18n("Sugo")
            color: Theme.foreground
            font.pixelSize: Theme.fontTitle
            font.weight: Font.DemiBold
        }
    }

    // --- Middle: the tabs, centred on the window, pushed aside rather than over the sides ----

    Row {
        id: tabs
        objectName: "titleBarTabs"
        visible: !bar.bare
        spacing: Theme.hairline * 2
        anchors.verticalCenter: parent.verticalCenter
        x: Math.max(leftGroup.x + leftGroup.width + Theme.spaceMd,
                    Math.min((bar.width - width) / 2, rightGroup.x - Theme.spaceMd - width))

        Tab {
            name: "timer"
            text: i18n("Focus")
            showsTime: true
        }
        Tab {
            name: "stats"
            text: i18n("Statistics")
        }
        Tab {
            name: "settings"
            text: i18n("Settings")
        }
    }

    // --- Right: the update, search, the window buttons ---------------------------------------

    RowLayout {
        id: rightGroup
        anchors.right: parent.right
        anchors.rightMargin: DesktopStyle.style === "windows" && DesktopStyle.rightButtons.length > 0 ? 0 : Theme.titleBarPaddingRight
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.hairline
        spacing: Theme.spaceXs

        // The update of the AppImage or the macOS app: a small icon beside search, with a dot in
        // the accent, while one is found, downloading or waiting for the restart. It opens
        // Settings › About, where the update is installed and Sugo restarted; its tooltip says
        // what is going on.
        SButton {
            id: updateButton
            objectName: "titleBarUpdate"
            readonly property bool found: UpdateChecker.installable
            readonly property bool downloading: UpdateChecker.status === UpdateChecker.Downloading
            readonly property bool installed: UpdateChecker.status === UpdateChecker.ReadyToRestart
            visible: !bar.bare && (found || downloading || installed)
            kind: "ghost"
            iconOnly: true
            implicitHeight: Theme.iconButton
            Layout.alignment: Qt.AlignVCenter
            iconName: installed ? "refresh" : "download"
            text: installed ? (DesktopStyle.macos ? i18n("Sugo %1 is ready. It replaces this version when Sugo restarts.", UpdateChecker.latestVersion)
                                                  : i18n("Sugo %1 is installed. It starts after a restart.", UpdateChecker.latestVersion))
                  : downloading ? i18n("Downloading Sugo %1… %2%", UpdateChecker.latestVersion, Math.round(UpdateChecker.progress * 100))
                  : i18n("Sugo %1 is available.", UpdateChecker.latestVersion)
            onClicked: bar.tabRequested("about")

            Rectangle {
                width: Theme.badgeDot
                height: Theme.badgeDot
                radius: width / 2
                color: Theme.accent
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: (parent.height - Theme.iconSm) / 2 - Theme.hairline
                anchors.rightMargin: (parent.width - Theme.iconSm) / 2 - Theme.hairline
            }
        }

        // Pages, timers and commands; the one shortcut the interface shows is in its tooltip.
        SButton {
            objectName: "titleBarSearch"
            visible: !bar.bare
            kind: "ghost"
            iconOnly: true
            iconName: "search"
            implicitHeight: Theme.iconButton
            Layout.alignment: Qt.AlignVCenter
            text: i18n("Search")
            toolTipText: i18nc("tooltip: an action, then its keyboard shortcut", "%1 (%2)", i18n("Search"),
                               DesktopStyle.shortcutText("Ctrl+K"))
            onClicked: bar.searchRequested()
        }

        // The window's own buttons on this side, as the desktop draws them.
        WindowButtons {
            objectName: "topBarWindowButtons"
            visible: DesktopStyle.rightButtons.length > 0
            Layout.leftMargin: DesktopStyle.style === "windows" ? 0 : Theme.spaceXs
            Layout.alignment: Qt.AlignVCenter
            Layout.fillHeight: DesktopStyle.style === "windows"
            buttons: DesktopStyle.rightButtons
            maximized: bar.maximized
            barHeight: bar.height - Theme.hairline
            onMinimizeRequested: bar.minimizeRequested()
            onMaximizeRequested: bar.toggleMaximizeRequested()
            onCloseRequested: bar.closeRequested()
        }
    }

    /// One tab. The Focus tab also shows the time left while a timer runs, in the colour of the
    /// phase; it keeps the same width whether the time is there or not, so no tab moves.
    component Tab: QQC2.AbstractButton {
        id: tab

        required property string name
        property bool showsTime: false
        readonly property bool isCurrent: bar.currentTab === name
        readonly property bool timeShown: showsTime && TimerEngine.active
        // A narrow window has room for the time or the name: the time while it runs.
        readonly property bool timeOnly: bar.compact && timeShown

        objectName: "titleBarTab-" + name
        implicitHeight: Theme.tabHeight
        implicitWidth: leftPadding + rightPadding + content.implicitWidth
        leftPadding: bar.compact ? Theme.tabPaddingXCompact : Theme.tabPaddingX
        rightPadding: leftPadding
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        font.pixelSize: Theme.fontBody
        font.weight: Font.Medium
        Accessible.role: Accessible.PageTab
        Accessible.name: text
        Accessible.selected: isCurrent
        Accessible.description: timeShown ? bar.statusText : ""
        onClicked: bar.tabRequested(name)

        QQC2.ToolTip.visible: timeShown && hovered
        QQC2.ToolTip.text: bar.statusText
        QQC2.ToolTip.delay: Theme.toolTipDelay

        FontMetrics {
            id: metrics
            font: label.font
        }
        FontMetrics {
            id: timeMetrics
            font: time.font
        }

        background: Rectangle {
            radius: Theme.radiusSm
            color: tab.isCurrent ? Theme.selected : tab.down ? Theme.pressed : tab.hovered ? Theme.hover : "transparent"
            Behavior on color { ColorAnimation { duration: Theme.durationShort } }

            Rectangle {
                anchors.fill: parent
                anchors.margins: -Theme.hairline * 2
                radius: Theme.radiusSm + Theme.hairline * 2
                color: "transparent"
                border.width: Theme.hairline
                border.color: Theme.focusRing
                visible: tab.visualFocus
            }
        }

        contentItem: Item {
            id: content
            readonly property real labelWidth: Math.ceil(metrics.advanceWidth(tab.text))
            readonly property real timeWidth: tab.showsTime ? Math.ceil(timeMetrics.advanceWidth(bar.widestClock)) : 0
            // Wide: the name, then room for the time. Narrow: the wider of the two.
            implicitWidth: !tab.showsTime ? labelWidth
                         : bar.compact ? Math.max(labelWidth, timeWidth)
                         : labelWidth + Theme.spaceSm + timeWidth
            implicitHeight: label.implicitHeight

            Row {
                anchors.centerIn: parent
                spacing: Theme.spaceSm

                Text {
                    id: label
                    visible: !tab.timeOnly
                    text: tab.text
                    font: tab.font
                    color: tab.isCurrent || tab.hovered ? Theme.foreground : Theme.muted
                }
                Text {
                    id: time
                    visible: tab.timeShown
                    width: content.timeWidth
                    horizontalAlignment: Text.AlignLeft
                    text: bar.clock
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.Medium
                    font.features: { "tnum": 1 }
                    color: TimerEngine.phase === TimerEngine.Phase.Work ? Theme.accent : Theme.breakAccent
                    opacity: TimerEngine.paused ? 0.6 : 1
                }
            }
        }
    }
}
