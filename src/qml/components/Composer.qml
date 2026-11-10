// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import stream.sugo.app

/**
 * The controls of the focus page in one bar: on the left the timer in use, how its phases start
 * and whether notifications are silenced while working; on the right Stop and Skip (there only
 * while a phase runs, without moving anything when they come) and Start/Pause.
 *
 * `app` is the window (Main.qml): the controls ask it to stop or skip (which may ask first) and
 * to choose a timer.
 */
Rectangle {
    id: composer
    objectName: "composer"

    /// The window, for requestEndPhase(), openSettings().
    property var app: null
    /// A narrow window: the labels of the start mode and of Quiet are left out.
    property bool compact: false
    /// The timer button and its menu.
    readonly property alias timerButton: timerButton
    readonly property alias timerMenu: timerMenu
    /// The start mode button and its menu.
    readonly property alias autoStartButton: autoStartButton
    readonly property alias autoStartMenu: autoStartMenu

    readonly property bool isIdle: TimerEngine.state === TimerEngine.State.Idle
    readonly property bool isWorkPhase: TimerEngine.phase === TimerEngine.Phase.Work

    // The options of the timer in use (setOption() on it signals currentChanged).
    readonly property int autoStartMode: {
        const value = PresetModel.option(PresetModel.currentUuid, "autoStartMode");
        return value === undefined ? 0 : Math.max(0, Math.min(2, Number(value)));
    }
    readonly property bool silenceWhileWorking: PresetModel.option(PresetModel.currentUuid, "silenceWhileWorking") === true
    readonly property var autoStartLabels: [i18n("Manual"), i18n("Breaks only"), i18n("All phases")]

    /// Start, Pause or Resume was pressed, after the timer was told (the welcome tour ends there).
    signal startPressed()

    implicitWidth: Theme.composerWidth
    implicitHeight: controls.implicitHeight + Theme.composerPadding * 2
    radius: Theme.radiusXl
    color: Theme.card
    border.width: Theme.hairline
    border.color: Theme.border

    // A disabled button gets no hover of its own: the bar tells it when the pointer is over it,
    // for its tooltip.
    HoverHandler {
        id: hover
    }
    function pointerOver(item: Item): bool {
        if (!hover.hovered || !item.visible) {
            return false;
        }
        const p = composer.mapToItem(item, hover.point.position.x, hover.point.position.y);
        return p.x >= 0 && p.y >= 0 && p.x < item.width && p.y < item.height;
    }

    TimerMenu {
        id: timerMenu
        anchorItem: timerButton
        app: composer.app
        // A timer cannot be switched while it runs (Space, the tray).
        Connections {
            target: TimerEngine
            function onStateChanged() {
                if (TimerEngine.active) {
                    timerMenu.close();
                }
            }
        }
    }

    AutoStartMenu {
        id: autoStartMenu
        anchorItem: autoStartButton
        mode: composer.autoStartMode
    }

    RowLayout {
        id: controls
        anchors.fill: parent
        anchors.margins: Theme.composerPadding
        anchors.leftMargin: Theme.spaceMd
        spacing: Theme.hairline * 2

        // The timer in use; its menu lists the timers.
        ComposerButton {
            id: timerButton
            objectName: "composerTimerButton"
            Layout.fillWidth: true
            Layout.maximumWidth: implicitWidth
            Layout.minimumWidth: Theme.iconButton * 2
            iconName: "timer"
            text: PresetModel.currentName
            strong: true
            trailingIcon: "chevron-down"
            enabled: !TimerEngine.active
            toolTipText: TimerEngine.active ? i18n("Stop the timer to switch to a different timer.") : ""
            Accessible.description: toolTipText
            open: timerMenu.visible
            onClicked: timerMenu.toggle()
        }

        // How the phases start: by hand, the breaks by themselves, or all of them.
        ComposerButton {
            id: autoStartButton
            objectName: "composerAutoStartButton"
            iconName: "repeat"
            text: composer.autoStartLabels[composer.autoStartMode]
            labels: composer.autoStartLabels
            iconOnly: composer.compact
            toolTipText: iconOnly ? i18n("Start automatically") : ""
            Accessible.name: i18n("Start automatically")
            Accessible.description: text
            open: autoStartMenu.visible
            onClicked: autoStartMenu.toggle()
        }

        // Quiet: notifications silenced while working (the timer's option). Where the desktop
        // cannot silence them it says why; on macOS it leads to the Focus shortcuts to install.
        ComposerButton {
            id: quietButton
            objectName: "composerQuietButton"
            readonly property bool setUp: SilenceController.available
            readonly property bool on: setUp && composer.silenceWhileWorking
            iconName: on ? "bell-off" : "bell"
            text: i18n("Quiet")
            iconOnly: composer.compact
            off: !on
            // A switch for screen readers; not checkable for the button itself, which would
            // replace the binding of `checked` with its own toggling.
            Accessible.checkable: setUp
            Accessible.checked: on
            enabled: setUp || DesktopStyle.macos
            toolTipText: !setUp ? (DesktopStyle.macos ? i18n("Needs the two Focus shortcuts: install them in Settings → Focus.")
                                                      : i18n("Not available: this desktop's notification service cannot be silenced by applications."))
                       : SilenceController.active ? i18n("Notifications silenced until the break")
                       : iconOnly ? i18n("Silence notifications while working") : ""
            Accessible.name: i18n("Silence notifications while working")
            Accessible.description: SilenceController.active ? i18n("Notifications silenced until the break")
                                                              : !setUp ? toolTipText : ""
            onClicked: {
                if (!setUp) {
                    if (composer.app) {
                        composer.app.openSettings("focus");
                    }
                    return;
                }
                PresetModel.setOption(PresetModel.currentUuid, "silenceWhileWorking", !composer.silenceWhileWorking);
            }
        }

        Item {
            Layout.fillWidth: true
        }

        // Stop and Skip, only while a phase runs; their place is kept, so nothing moves.
        SButton {
            objectName: "composerStop"
            kind: "ghost"
            iconOnly: true
            iconName: "stop"
            text: i18n("Stop")
            enabled: TimerEngine.active
            opacity: TimerEngine.active ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.durationShort } }
            onClicked: composer.app.requestEndPhase(false)
        }
        SButton {
            objectName: "composerSkip"
            kind: "ghost"
            iconOnly: true
            iconName: "skip"
            text: i18n("Skip")
            enabled: TimerEngine.active
            opacity: TimerEngine.active ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.durationShort } }
            onClicked: composer.app.requestEndPhase(true)
        }

        // Start, Pause, Resume: the round button in the accent colour.
        QQC2.AbstractButton {
            id: startButton
            objectName: "composerStart"
            Layout.leftMargin: Theme.spaceXs
            implicitWidth: Theme.roundButton
            implicitHeight: Theme.roundButton
            hoverEnabled: true
            focusPolicy: Qt.TabFocus
            text: TimerEngine.paused ? i18n("Resume")
                                     : composer.isIdle ? (composer.isWorkPhase ? i18n("Start") : i18n("Start break")) : i18n("Pause")
            Accessible.name: text
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.text: text
            QQC2.ToolTip.delay: Theme.toolTipDelay
            onClicked: {
                TimerEngine.toggle();
                composer.startPressed();
            }

            background: Rectangle {
                radius: width / 2
                color: Theme.alpha(Theme.accent, startButton.down ? 0.8 : (startButton.hovered ? 0.9 : 1))
                scale: startButton.down ? 0.96 : 1
                Behavior on scale { NumberAnimation { duration: Theme.durationShort } }
                Behavior on color { ColorAnimation { duration: Theme.durationShort } }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -Theme.hairline * 3
                    radius: width / 2
                    color: "transparent"
                    border.width: Theme.hairline
                    border.color: Theme.focusRing
                    visible: startButton.visualFocus
                }
            }
            contentItem: Item {
                LineIcon {
                    anchors.centerIn: parent
                    name: composer.isIdle || TimerEngine.paused ? "play" : "pause"
                    size: Theme.iconSm
                    color: Theme.accentText
                }
            }
        }
    }

    /// A quiet button of the bar: an icon, a label (left out with `iconOnly`) and maybe a chevron.
    component ComposerButton: QQC2.AbstractButton {
        id: button
        property string iconName
        property string trailingIcon
        /// The timer's name: in the text colour, not the quieter one of the others.
        property bool strong: false
        /// Switched off (Quiet): in the faint colour.
        property bool off: false
        property bool iconOnly: false
        /// Its menu is open: lit as when pressed.
        property bool open: false
        property string toolTipText
        /// Every label it takes in turn: it is as wide as with the widest, so the bar does not move.
        property var labels: []
        readonly property real labelWidth: {
            let widest = metrics.advanceWidth(text);
            for (const label of labels) {
                widest = Math.max(widest, metrics.advanceWidth(label));
            }
            return Math.ceil(widest);
        }
        // Off: faint; disabled (the timer while it runs): muted and, as a whole, half transparent.
        readonly property color labelColor: off ? Theme.subtle : !enabled ? Theme.muted
                                           : strong || open || hovered ? Theme.foreground : Theme.dim

        implicitHeight: Theme.controlHeight
        implicitWidth: contentRow.implicitWidth + leftPadding + rightPadding
        leftPadding: iconOnly ? Theme.spaceSm + Theme.hairline : Theme.spaceMd + Theme.hairline
        rightPadding: leftPadding
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        font.pixelSize: Theme.fontBody
        font.weight: Font.Medium
        opacity: enabled || off ? 1 : 0.6
        Accessible.name: text
        QQC2.ToolTip.visible: toolTipText.length > 0 && (enabled ? hovered : composer.pointerOver(button))
        QQC2.ToolTip.text: toolTipText
        QQC2.ToolTip.delay: Theme.toolTipDelay

        FontMetrics {
            id: metrics
            font: button.font
        }

        background: Rectangle {
            radius: Theme.radius
            color: button.open || button.down ? Theme.selected : button.hovered && button.enabled ? Theme.hover : "transparent"
            Behavior on color { ColorAnimation { duration: Theme.durationShort } }
            Rectangle {
                anchors.fill: parent
                anchors.margins: -Theme.hairline * 2
                radius: Theme.radius + Theme.hairline * 2
                color: "transparent"
                border.width: Theme.hairline
                border.color: Theme.focusRing
                visible: button.visualFocus
            }
        }
        contentItem: RowLayout {
            id: contentRow
            spacing: Theme.spaceSm + Theme.hairline
            LineIcon {
                name: button.iconName
                size: Theme.iconSm
                color: button.off || !button.enabled ? Theme.subtle : Theme.muted
            }
            Text {
                visible: !button.iconOnly
                Layout.fillWidth: true
                Layout.preferredWidth: button.labelWidth
                Layout.minimumWidth: 0
                text: button.text
                font: button.font
                color: button.labelColor
                elide: Text.ElideRight
                maximumLineCount: 1
            }
            LineIcon {
                visible: button.trailingIcon.length > 0
                name: button.trailingIcon
                size: Theme.iconSm
                color: Theme.muted
                rotation: button.open ? 180 : 0
                Behavior on rotation { NumberAnimation { duration: Theme.durationShort } }
            }
        }
    }
}
