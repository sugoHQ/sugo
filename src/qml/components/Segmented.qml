// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import stream.sugo.app

/// A row of choices where one is on: `model` is an array of { id, text }.
Rectangle {
    id: segmented

    property var model: []
    property string current
    signal activated(string id)

    // A 1 px frame, 2 px around the choices and between them.
    readonly property real inset: Theme.hairline * 3

    implicitWidth: row.implicitWidth + inset * 2
    implicitHeight: Theme.controlHeightSm
    radius: Theme.radius
    color: "transparent"
    border.width: Theme.hairline
    border.color: Theme.border
    Accessible.role: Accessible.PageTabList

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: Theme.hairline * 2

        Repeater {
            id: choices
            model: segmented.model
            delegate: QQC2.AbstractButton {
                id: choice
                required property var modelData
                readonly property bool on: segmented.current === modelData.id
                implicitHeight: segmented.implicitHeight - segmented.inset * 2
                leftPadding: Theme.spaceMd + Theme.hairline * 2
                rightPadding: leftPadding
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                text: modelData.text
                Accessible.role: Accessible.PageTab
                Accessible.checked: on
                onClicked: segmented.activated(modelData.id)
                background: Rectangle {
                    radius: Theme.radiusSm
                    color: choice.on ? Theme.selected : (choice.hovered ? Theme.hover : "transparent")
                    border.width: choice.visualFocus ? Theme.hairline : 0
                    border.color: Theme.focusRing
                }
                contentItem: Text {
                    text: choice.text
                    color: choice.on || choice.hovered ? Theme.foreground : Theme.muted
                    font.pixelSize: Theme.fontCaption
                    font.weight: Font.Medium
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }
}
