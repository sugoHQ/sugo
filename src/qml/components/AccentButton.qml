// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import stream.sugo.app

/**
 * The "Accent color" setting: a button like a drop-down list's that shows a dot of the current
 * colour, and its name when it is given the room. It opens a menu under it, right edges together, with every colour of
 * AppSettings.accentColors: its dot, its name and a check on the chosen one.
 */
QQC2.AbstractButton {
    id: button

    /// The name of the accent colour `id`.
    function colourName(id: string): string {
        return AppSettings.accentColorName(id);
    }

    /// The menu of the colours (the smoke checks open it).
    readonly property alias menu: accentMenu

    objectName: "accentColorButton"
    implicitHeight: Theme.controlHeight
    // The dot, the colour's name and the chevron, at least as wide as a drop-down list.
    implicitWidth: Math.max(Theme.selectMinWidth, leftPadding + Theme.swatchSize + Theme.spaceSm + nameMetrics.advanceWidth(colourName(AppSettings.accentColor))
                            + Theme.spaceSm + Theme.iconSm + rightPadding)
    leftPadding: Theme.spaceMd + Theme.hairline * 2
    // The chevron where SComboBox has it.
    rightPadding: Theme.spaceMd
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    text: i18n("Accent color")
    Accessible.role: Accessible.ComboBox
    Accessible.name: text
    Accessible.description: colourName(AppSettings.accentColor)
    QQC2.ToolTip.visible: hovered && !accentMenu.visible
    QQC2.ToolTip.text: colourName(AppSettings.accentColor)
    QQC2.ToolTip.delay: Theme.toolTipDelay

    FontMetrics {
        id: nameMetrics
        font.pixelSize: Theme.fontBody
    }

    onClicked: accentMenu.toggle()
    Keys.onDownPressed: accentMenu.open()

    background: Rectangle {
        radius: Theme.radius
        // hover and pressed are translucent: lay them over the field's colour, not in place of it.
        color: button.pressed ? Qt.tint(Theme.background, Theme.pressed)
             : button.hovered && !accentMenu.visible ? Qt.tint(Theme.background, Theme.hover) : Theme.background
        border.width: Theme.hairline
        border.color: button.visualFocus ? Theme.focusRing : accentMenu.visible ? Theme.borderStrong : Theme.border
    }

    contentItem: Item {
        Rectangle {
            id: dot
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.swatchSize
            height: Theme.swatchSize
            radius: width / 2
            color: Theme.accent
            border.width: Theme.hairline
            border.color: Theme.borderStrong
        }
        // The colour's name, when the button is wide enough for all of it (a settings row's column).
        Text {
            anchors.left: dot.right
            anchors.leftMargin: Theme.spaceSm
            anchors.right: chevron.left
            anchors.rightMargin: Theme.spaceSm
            anchors.verticalCenter: parent.verticalCenter
            text: button.colourName(AppSettings.accentColor)
            visible: width >= implicitWidth
            color: button.enabled ? Theme.foreground : Theme.subtle
            font.pixelSize: Theme.fontBody
        }
        LineIcon {
            id: chevron
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            name: "chevron-down"
            size: Theme.iconSm
            color: Theme.muted
            rotation: accentMenu.visible ? 180 : 0
        }
    }

    SMenu {
        id: accentMenu
        objectName: "accentColorMenu"
        anchorItem: button
        side: "bottom"
        align: "end"

        Repeater {
            model: AppSettings.accentColors
            delegate: SMenuItem {
                required property string modelData
                objectName: "accentEntry-" + modelData
                text: button.colourName(modelData)
                swatch: Theme.accentSwatch(modelData)
                checked: AppSettings.accentColor === modelData
                onTriggered: AppSettings.accentColor = modelData
            }
        }
    }
}
