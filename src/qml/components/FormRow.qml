// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import stream.sugo.app

/**
 * A row of a FormSection: the title and a description on the left, the control on the right, or
 * under the text when it would leave the text too little room. With `stacked` the control always
 * takes the full width under the text (an icon grid, a long field). Controls that fill their width
 * (Layout.fillWidth) share a column of Theme.settingsControlWidth, as in Settings; a switch keeps
 * its own width, at the column's right edge.
 */
Item {
    id: row

    property string title
    property string description
    property bool stacked: false
    /// No line above it (the first row of a section).
    property bool first: false
    /// Every text `description` can show, when it changes with a state: the row keeps the
    /// height of the tallest, so nothing moves when the description changes.
    property var descriptionVariants: []
    /// How far the line above the row reaches past its sides: to the edges of the card, whose
    /// rows are this far from them.
    property real separatorOverhang: Theme.settingsRowPaddingX
    /// A control fills the column (else the controls keep their own width).
    readonly property bool fillsColumn: {
        for (const child of controlHolder.children) {
            if (child.visible && child.Layout.fillWidth) {
                return true;
            }
        }
        return false;
    }
    /// The width of the controls beside the text.
    readonly property real controlWidth: fillsColumn ? Theme.settingsControlWidth : controlHolder.implicitWidth
    /// The control sits beside the text (else under it).
    readonly property bool sideBySide: !stacked && controlHolder.children.length > 0
        && width - controlWidth - layout.columnSpacing >= Theme.settingsRowTextMinWidth
    /// The controls (smoke checks: the column's edges).
    readonly property alias controlColumn: controlHolder
    default property alias control: controlHolder.data

    Layout.fillWidth: true
    implicitHeight: Math.max(Theme.settingsRowMinHeight, layout.implicitHeight + Theme.settingsRowPaddingY * 2)
    Accessible.role: Accessible.Grouping
    Accessible.name: title

    Rectangle {
        visible: !row.first
        x: -row.separatorOverhang
        width: parent.width + row.separatorOverhang * 2
        height: Theme.hairline
        color: Theme.border
    }

    GridLayout {
        id: layout
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        columns: row.sideBySide ? 2 : 1
        columnSpacing: Theme.spaceXl
        rowSpacing: Theme.spaceMd

        ColumnLayout {
            Layout.fillWidth: true
            Layout.preferredWidth: 0
            // With variants, as tall as the title and the tallest of them; the text shown sits in
            // the middle of that room, in line with the control.
            Layout.minimumHeight: row.descriptionVariants.length > 0
                                  ? titleText.implicitHeight + spacing + descriptionText.reservedHeight : 0
            spacing: Theme.hairline
            Item { visible: row.descriptionVariants.length > 0; Layout.fillHeight: true }
            Text {
                id: titleText
                text: row.title
                color: row.enabled ? Theme.foreground : Theme.subtle
                font.pixelSize: Theme.fontBody
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Text {
                id: descriptionText
                visible: row.description.length > 0
                text: row.description
                color: Theme.muted
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
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
            id: controlHolder
            // A row of text only leaves the text the whole width.
            visible: children.length > 0
            Layout.alignment: row.sideBySide ? (Qt.AlignRight | Qt.AlignVCenter) : Qt.AlignLeft
            // Beside the text, the column; under it, the row's width.
            Layout.preferredWidth: row.sideBySide ? row.controlWidth : -1
            Layout.fillWidth: !row.sideBySide
            spacing: Theme.spaceSm
        }
    }
}
