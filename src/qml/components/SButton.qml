// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import stream.sugo.app

/**
 * The button of the interface. `kind`: "primary" (filled with the accent colour, one per view),
 * "secondary" (outlined card), "ghost" (no frame until hovered) or "danger".
 */
QQC2.AbstractButton {
    id: button

    property string kind: "secondary"
    /// A LineIcon name.
    property string iconName
    property bool large: false
    property bool iconOnly: false
    property string toolTipText: iconOnly ? text : ""
    /// Turns red when hovered (delete).
    property bool destructive: false
    /// When true, the label is drawn in the accent colour (a lit, prominent action).
    property bool lit: false
    /// Joined to the drop-down list on its left (an SComboBox with `joinedRight`), as one control:
    /// its height and colour, no corners between them and one line where they meet. In a row
    /// spaced by Theme.spaceSm.
    property bool joinedLeft: false
    /// Every label the button takes in turn ("Copy", "Copied"): it is as wide as with the widest,
    /// so nothing beside it moves when the label changes.
    property var labels: []
    readonly property real labelsExtraWidth: {
        let widest = 0;
        for (const label of labels) {
            widest = Math.max(widest, labelMetrics.advanceWidth(label));
        }
        return Math.max(0, Math.ceil(widest - labelMetrics.advanceWidth(text)));
    }

    readonly property bool isPrimary: kind === "primary"
    readonly property color contentColor: !enabled ? Theme.subtle
                                         : isPrimary ? Theme.accentText
                                         : lit ? Theme.accent
                                         : kind === "danger" || (destructive && hovered) ? Theme.danger
                                         : kind === "ghost" && iconOnly && !hovered && !visualFocus ? Theme.muted
                                         : Theme.foreground

    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    implicitHeight: joinedLeft ? Theme.controlHeightSm : large ? Theme.controlHeightLg : Theme.controlHeight
    Layout.leftMargin: joinedLeft ? -(Theme.spaceSm + Theme.hairline) : 0
    implicitWidth: Math.max(implicitHeight, row.implicitWidth + leftPadding + rightPadding + (iconOnly ? 0 : labelsExtraWidth))
    leftPadding: iconOnly ? 0 : (large ? Theme.spaceXl : Theme.spaceLg)
    rightPadding: leftPadding
    font.pixelSize: large ? Theme.fontTitle : Theme.fontBody
    font.weight: isPrimary ? Font.DemiBold : Font.Medium
    Accessible.name: text

    FontMetrics {
        id: labelMetrics
        font: button.font
    }

    QQC2.ToolTip.visible: toolTipText.length > 0 && hovered
    QQC2.ToolTip.text: toolTipText
    QQC2.ToolTip.delay: Theme.toolTipDelay

    background: Rectangle {
        radius: Theme.radius
        topLeftRadius: button.joinedLeft ? 0 : radius
        bottomLeftRadius: button.joinedLeft ? 0 : radius
        color: {
            if (button.isPrimary) {
                return button.enabled ? Theme.alpha(Theme.accent, button.down ? 0.8 : (button.hovered ? 0.9 : 1)) : Theme.selected;
            }
            // A secondary button's hover is laid over its own fill: animating from the opaque fill to
            // the translucent overlay would pass through a half-opaque grey (a light flash).
            const secondary = button.kind === "secondary";
            const fill = button.joinedLeft ? Theme.background : Theme.card;
            const over = c => secondary ? Qt.tint(fill, c) : c;
            if (button.down) {
                return over(Theme.pressed);
            }
            if (button.hovered && button.enabled) {
                return over(button.kind === "danger" || button.destructive ? Theme.dangerFill : Theme.hover);
            }
            return secondary ? fill : "transparent";
        }
        border.width: button.kind === "secondary" || button.kind === "danger" ? Theme.hairline : 0
        border.color: button.kind === "danger" ? Theme.alpha(Theme.danger, 0.4) : Theme.border
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

    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight
        RowLayout {
            id: row
            anchors.centerIn: parent
            spacing: Theme.spaceSm
            LineIcon {
                visible: button.iconName.length > 0
                name: button.iconName
                color: button.contentColor
                size: button.large ? Theme.icon : Theme.iconSm
            }
            Text {
                visible: !button.iconOnly && button.text.length > 0
                text: button.text
                color: button.contentColor
                font: button.font
            }
        }
    }
}
