// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import stream.sugo.app

/// A group of settings: a heading, an optional line under it, and its rows in one card.
ColumnLayout {
    id: section

    property string title
    property string description
    /// Next to the heading (a button, say).
    property alias headerExtra: extraRow.data
    default property alias rows: rowColumn.data

    Layout.fillWidth: true
    Layout.topMargin: section.title.length > 0 ? Theme.settingsSectionGap : 0
    spacing: Theme.spaceMd

    RowLayout {
        Layout.fillWidth: true
        visible: section.title.length > 0
        spacing: Theme.spaceMd
        // The heading, in line with the text of the rows under it.
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.settingsRowPaddingX
            text: section.title
            color: Theme.foreground
            font.pixelSize: Theme.fontSettingsSection
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            Accessible.role: Accessible.Heading
        }
        RowLayout {
            id: extraRow
            spacing: Theme.spaceXs
        }
    }
    Text {
        visible: section.description.length > 0
        Layout.leftMargin: Theme.settingsRowPaddingX
        text: section.description
        color: Theme.muted
        font.pixelSize: Theme.fontBody
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
