// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import stream.sugo.app

/// A one-line text input with a 1 px border.
QQC2.TextField {
    id: field

    implicitWidth: Theme.space4xl * 4
    implicitHeight: Theme.controlHeight
    leftPadding: Theme.spaceMd + Theme.hairline * 2
    rightPadding: leftPadding
    color: Theme.foreground
    placeholderTextColor: Theme.subtle
    selectionColor: Theme.alpha(Theme.accent, 0.35)
    selectedTextColor: Theme.foreground
    font.pixelSize: Theme.fontBody
    hoverEnabled: true

    // Without focus, show the start of a text too long to fit, not its end.
    function showStart(): void {
        if (!activeFocus) {
            cursorPosition = 0;
        }
    }
    onTextChanged: showStart()
    onActiveFocusChanged: showStart()
    Component.onCompleted: showStart()

    background: Rectangle {
        radius: Theme.radius
        color: Theme.background
        border.width: Theme.hairline
        border.color: field.activeFocus ? Theme.focusRing : (field.hovered ? Theme.borderStrong : Theme.border)
        Behavior on border.color { ColorAnimation { duration: Theme.durationShort } }
    }
}
