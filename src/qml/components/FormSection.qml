// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import stream.sugo.app

/// A group of settings: a small heading, an optional line under it, and its rows in one card.
ColumnLayout {
    id: section

    property string title
    property string description
    /// Next to the heading (a button, say).
    property alias headerExtra: extraRow.data
    default property alias rows: rowColumn.data

    Layout.fillWidth: true
    Layout.topMargin: section.title.length > 0 ? Theme.spaceLg : 0
    spacing: Theme.spaceSm

    RowLayout {
        Layout.fillWidth: true
        visible: section.title.length > 0
        spacing: Theme.spaceMd
        Text {
            Layout.fillWidth: true
            text: section.title
            color: Theme.muted
            font.pixelSize: Theme.fontCaption
            font.weight: Font.Medium
            elide: Text.ElideRight
        }
        RowLayout {
            id: extraRow
            spacing: Theme.spaceXs
        }
    }
    Text {
        visible: section.description.length > 0
        text: section.description
        color: Theme.muted
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }
    Rectangle {
        Layout.fillWidth: true
        implicitHeight: rowColumn.implicitHeight
        radius: Theme.radiusCard
        color: Theme.card
        border.width: Theme.hairline
        border.color: Theme.border

        ColumnLayout {
            id: rowColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.settingsRowPaddingX
            anchors.rightMargin: Theme.settingsRowPaddingX
            spacing: 0
        }
    }
}
