// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import stream.sugo.app

/// A drop-down list in the style of the other controls: its list is an SMenu centred under the
/// field, with a check on the current entry. As wide as its longest entry, so it does not move
/// when the choice changes.
QQC2.ComboBox {
    id: combo

    /// The width of the longest entry's text. (ComboBox.WidestText measures the entries only for a
    /// content item that takes typing, which this one does not.)
    readonly property real widestTextWidth: {
        void model;
        let widest = textMetrics.advanceWidth(displayText);
        for (let i = 0; i < count; ++i) {
            widest = Math.max(widest, textMetrics.advanceWidth(textAt(i)));
        }
        return Math.ceil(widest);
    }

    implicitWidth: Math.max(Theme.selectMinWidth, widestTextWidth + leftPadding + rightPadding)
    implicitHeight: Theme.controlHeightSm
    leftPadding: Theme.spaceMd + Theme.hairline * 2
    rightPadding: Theme.spaceMd * 2 + Theme.iconSm
    font.pixelSize: Theme.fontBody
    hoverEnabled: true

    FontMetrics {
        id: textMetrics
        font: combo.font
    }

    background: Rectangle {
        radius: Theme.radius
        // hover and pressed are translucent: lay them over the field's colour, not in place of it.
        color: combo.pressed ? Qt.tint(Theme.background, Theme.pressed)
                          : (combo.hovered && !combo.popup.visible ? Qt.tint(Theme.background, Theme.hover) : Theme.background)
        border.width: Theme.hairline
        border.color: combo.visualFocus ? Theme.focusRing : combo.popup.visible ? Theme.borderStrong : Theme.border
    }

    contentItem: Text {
        text: combo.displayText
        font: combo.font
        color: combo.enabled ? Theme.foreground : Theme.subtle
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: LineIcon {
        x: combo.width - width - Theme.spaceMd
        y: (combo.height - height) / 2
        name: "chevron-down"
        size: Theme.iconSm
        color: Theme.muted
        rotation: combo.popup.visible ? 180 : 0
    }

    delegate: SMenuItem {
        required property var model
        required property int index
        width: ListView.view ? ListView.view.width : implicitWidth
        text: model[combo.textRole] ?? model.modelData ?? ""
        font: combo.font
        checked: combo.currentIndex === index
        highlighted: combo.highlightedIndex === index
    }

    popup: SMenu {
        anchorItem: combo
        side: "bottom"
        align: "center"
        // The combo box keeps the keyboard (Up, Down, Enter, Escape) while its list is open.
        focus: false
        naturalWidth: {
            let widest = combo.width - Theme.menuPadding * 2;
            const entries = list.contentItem.children;
            for (let i = 0; i < entries.length; ++i) {
                widest = Math.max(widest, entries[i].implicitWidth);
            }
            return widest;
        }
        onOpened: showItem(list.currentItem)

        // The menu scrolls, not the list: it is as tall as its entries.
        ListView {
            id: list
            implicitHeight: contentHeight
            interactive: false
            model: combo.delegateModel
            currentIndex: combo.highlightedIndex
            onCurrentItemChanged: if (combo.popup.visible) combo.popup.showItem(currentItem)
        }
    }
}
