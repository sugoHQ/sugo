// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import stream.sugo.app

/**
 * Ctrl+K: type to find a page, a timer or a command, Enter to run it.
 * `commands` is an array of { text, detail, icon, timerIcon, run }: `detail` is a quieter word on
 * the right (a timer's category), found by the search too.
 */
QQC2.Popup {
    id: commandPalette

    property var commands: []
    readonly property var matches: {
        const query = field.text.trim().toLowerCase();
        return commands.filter(c => query.length === 0 || c.text.toLowerCase().includes(query)
                                    || (c.detail || "").toLowerCase().includes(query));
    }

    function runAt(index: int): void {
        const command = matches[index];
        if (command) {
            close();
            command.run();
        }
    }

    parent: QQC2.Overlay.overlay
    x: (parent.width - width) / 2
    y: Theme.paletteTop
    width: Math.min(Theme.paletteWidth, parent.width - Theme.spaceLg * 2)
    padding: Theme.hairline
    modal: true
    focus: true
    onOpened: {
        field.text = "";
        list.currentIndex = 0;
        field.forceActiveFocus();
    }

    QQC2.Overlay.modal: Rectangle {
        color: Theme.scrim
    }
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durationMenu; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: Theme.menuScaleFrom; to: 1; duration: Theme.durationMenu; easing.type: Easing.OutCubic }
    }

    background: Rectangle {
        radius: Theme.radiusXl
        color: Theme.raised
        border.width: Theme.hairline
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: 0

        // The search field, across the top with a line under it.
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.paletteFieldHeight
            Layout.leftMargin: Theme.spaceMd + Theme.spaceXs - Theme.hairline
            Layout.rightMargin: Theme.spaceMd + Theme.spaceXs - Theme.hairline
            spacing: Theme.spaceMd + Theme.hairline * 2
            LineIcon {
                name: "search"
                size: Theme.icon
                color: Theme.muted
            }
            TextInput {
                id: field
                Layout.fillWidth: true
                color: Theme.foreground
                font.pixelSize: Theme.fontTitle
                selectionColor: Theme.alpha(Theme.accent, 0.35)
                clip: true
                Accessible.role: Accessible.EditableText
                Accessible.name: i18n("Search")
                Keys.onDownPressed: list.incrementCurrentIndex()
                Keys.onUpPressed: list.decrementCurrentIndex()
                Keys.onReturnPressed: commandPalette.runAt(list.currentIndex)
                Keys.onEnterPressed: commandPalette.runAt(list.currentIndex)
                onTextChanged: list.currentIndex = 0
                Text {
                    visible: field.text.length === 0
                    text: i18n("Search pages, timers and commands…")
                    color: Theme.subtle
                    font: field.font
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.hairline
            color: Theme.border
        }

        ListView {
            id: list
            Layout.fillWidth: true
            // Every row is as tall: no waiting for the list to measure itself after a search.
            Layout.preferredHeight: Math.min(commandPalette.matches.length, 10) * Theme.menuRowHeight
            Layout.margins: Theme.spaceSm - Theme.hairline
            clip: true
            model: commandPalette.matches
            boundsBehavior: Flickable.StopAtBounds
            highlightMoveDuration: 0

            delegate: SMenuItem {
                id: entry
                required property var modelData
                required property int index
                readonly property string detail: modelData.detail || ""
                width: ListView.view.width
                rightPadding: Theme.spaceMd + (detail.length > 0 ? detailText.implicitWidth + Theme.spaceMd : 0)
                text: modelData.text
                iconName: modelData.timerIcon ? "" : (modelData.icon || "chevron-right")
                leading: modelData.timerIcon ? timerIconComponent : null
                highlighted: ListView.isCurrentItem
                onHoveredChanged: {
                    if (hovered) {
                        list.currentIndex = index;
                    }
                }
                onTriggered: commandPalette.runAt(index)

                Component {
                    id: timerIconComponent
                    TimerIcon {
                        iconName: entry.modelData.timerIcon || ""
                        size: Theme.icon
                        color: Theme.accent
                    }
                }
                Text {
                    id: detailText
                    visible: entry.detail.length > 0
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.spaceMd
                    anchors.verticalCenter: parent.verticalCenter
                    text: entry.detail
                    color: Theme.muted
                    font.pixelSize: Theme.fontCaption
                }
            }
        }
        // Outside the list: a child of the ListView would sit in its content
        // item, which has no height (and is clipped) when nothing matches. Hidden, rather than
        // of no height, when something does: the layout then leaves no room for it.
        Item {
            Layout.fillWidth: true
            implicitHeight: Theme.paletteFieldHeight
            visible: commandPalette.matches.length === 0

            Text {
                objectName: "nothingFound"
                anchors.centerIn: parent
                text: i18n("Nothing found")
                color: Theme.muted
                font.pixelSize: Theme.fontBody
            }
        }
    }
}
