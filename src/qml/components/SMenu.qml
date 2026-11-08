// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import stream.sugo.app

/**
 * A menu that opens from its button: SMenuItem rows, with SMenuHeader captions and
 * SMenuSeparator lines between groups.
 *
 *     SButton { id: button; onClicked: menu.toggle() }
 *     SMenu {
 *         id: menu
 *         anchorItem: button
 *         side: "bottom"      // or "top"; it goes to the other side when there is not room enough
 *         align: "end"        // right edges together ("start": left, "center": centres)
 *         SMenuItem { text: …; checked: …; onTriggered: … }
 *     }
 *
 * It opens Theme.menuOffset away from `anchorItem`, stays Theme.menuEdgeMargin inside the window,
 * and grows out of the button's corner. It is as wide as its widest row, between menuMinWidth
 * and menuMaxWidth (or `fixedWidth`), and scrolls when the window is too short for it. Up/Down
 * move the highlight, Enter chooses, Escape closes; pressing the button again closes it, and so
 * does resizing the window.
 */
QQC2.Popup {
    id: menu

    /// The button the menu opens from. Without one, place the menu with x and y.
    property Item anchorItem
    /// "bottom" (under the button) or "top" (over it).
    property string side: "bottom"
    /// "start" (left edges together), "center" (centres together), or "end" (right edges together).
    property string align: "start"
    /// A width of its own (the whole menu's) instead of the widest row's; still kept inside the window.
    property real fixedWidth: 0
    /// How wide the rows would like to be; a menu with a content of its own may set it.
    property real naturalWidth: {
        let widest = 0;
        const kids = rows.children;
        for (let i = 0; i < kids.length; ++i) {
            if (kids[i].visible) {
                widest = Math.max(widest, kids[i].implicitWidth);
            }
        }
        return widest;
    }
    /// The lit row (mouse or arrow keys); when the menu opens, its checked row.
    property Item highlightedItem: null
    /// Whether one of the rows is checked: a menu of choices.
    readonly property bool hasChecks: {
        const kids = rows.children;
        for (let i = 0; i < kids.length; ++i) {
            if (kids[i].isMenuItem === true && kids[i].checked) {
                return true;
            }
        }
        return false;
    }
    /// The side it opened on, after a flip.
    readonly property string openedSide: placement.side
    default property alias content: rows.data

    /// Opens the menu, or closes it when it is open (for the button's click).
    function toggle(): void {
        if (visible) {
            close();
        } else {
            open();
        }
    }

    // Where the button is and how much room the window has, taken when the menu opens (it closes
    // when the window is resized). The side follows the menu's height: the rows are laid out only
    // once the menu is shown, before it is drawn.
    QtObject {
        id: placement
        property rect anchor: Qt.rect(0, 0, 0, 0)
        property real windowWidth: 0
        property real windowHeight: 0
        readonly property real roomBelow: windowHeight - Theme.menuEdgeMargin - (anchor.y + anchor.height + Theme.menuOffset)
        readonly property real roomAbove: anchor.y - Theme.menuOffset - Theme.menuEdgeMargin
        // The other side when the menu does not fit on its own and has more room there.
        readonly property string side: {
            if (!menu.anchorItem) {
                return menu.side;
            }
            const want = menu.side === "top" ? roomAbove : roomBelow;
            const other = menu.side === "top" ? roomBelow : roomAbove;
            return menu.implicitHeight > want && other > want ? (menu.side === "top" ? "bottom" : "top") : menu.side;
        }
        readonly property real room: side === "top" ? roomAbove : roomBelow
        // In the window's coordinates.
        readonly property real left: {
            let wanted;
            if (menu.align === "end") {
                wanted = anchor.x + anchor.width - menu.width;
            } else if (menu.align === "center") {
                wanted = anchor.x + (anchor.width - menu.width) / 2;
            } else {
                wanted = anchor.x;
            }
            return Math.max(Theme.menuEdgeMargin, Math.min(wanted, windowWidth - Theme.menuEdgeMargin - menu.width));
        }
        readonly property real top: side === "top" ? anchor.y - Theme.menuOffset - menu.height
                                                   : anchor.y + anchor.height + Theme.menuOffset
    }

    function place(): void {
        const overlay = QQC2.Overlay.overlay;
        if (!overlay) {
            return;
        }
        placement.windowWidth = overlay.width;
        placement.windowHeight = overlay.height;
        if (anchorItem) {
            const r = anchorItem.mapToItem(overlay, 0, 0, anchorItem.width, anchorItem.height);
            placement.anchor = Qt.rect(r.x, r.y, r.width, r.height);
        }
    }

    function moveHighlight(step: int): void {
        const kids = rows.children;
        const choices = [];
        for (let i = 0; i < kids.length; ++i) {
            if (kids[i].isMenuItem === true && kids[i].visible && kids[i].enabled) {
                choices.push(kids[i]);
            }
        }
        if (choices.length === 0) {
            return;
        }
        const at = choices.indexOf(highlightedItem);
        const next = at < 0 ? (step > 0 ? 0 : choices.length - 1)
                            : Math.max(0, Math.min(choices.length - 1, at + step));
        highlightedItem = choices[next];
    }

    /// Scrolls a long menu to `item` (a row, or an item inside the rows).
    function showItem(item: Item): void {
        if (!item || !flick || !flick.interactive) {
            return;
        }
        const r = item.mapToItem(rows, 0, 0, item.width, item.height);
        const top = r.y + rows.y - Theme.menuPadding;
        const bottom = r.y + rows.y + r.height + Theme.menuPadding;
        if (top < flick.contentY) {
            flick.contentY = Math.max(0, top);
        } else if (bottom > flick.contentY + flick.height) {
            flick.contentY = Math.min(flick.contentHeight - flick.height, bottom - flick.height);
        }
    }

    function adoptRows(): void {
        const kids = rows.children;
        for (let i = 0; i < kids.length; ++i) {
            if (kids[i].isMenuItem === true) {
                kids[i].menu = menu;
            }
            kids[i].width = Qt.binding(() => rows.width);
        }
    }

    // The rows are made with the menu, not with its content item (made later, on completion), so
    // the bindings above and the `content` alias always find them.
    readonly property Column rowsColumn: Column {
        id: rows
        x: Theme.menuPadding - Theme.hairline
        y: Theme.menuPadding - Theme.hairline
        width: Math.max(0, menu.availableWidth - x * 2)
        onChildrenChanged: menu.adoptRows()
    }

    parent: anchorItem ? anchorItem : QQC2.Overlay.overlay
    // A press on the button itself is left to the button (toggle()), so it does not reopen the menu.
    closePolicy: anchorItem ? QQC2.Popup.CloseOnEscape | QQC2.Popup.CloseOnPressOutsideParent
                            : QQC2.Popup.CloseOnEscape | QQC2.Popup.CloseOnPressOutside
    focus: true
    padding: Theme.hairline
    // In the button's coordinates (it is the parent).
    x: anchorItem ? placement.left - placement.anchor.x : 0
    y: anchorItem ? placement.top - placement.anchor.y : 0
    width: {
        const room = Math.max(0, (anchorItem ? placement.windowWidth : (parent ? parent.width : 0)) - Theme.menuEdgeMargin * 2);
        const wanted = fixedWidth > 0 ? fixedWidth
                                      : Math.max(Theme.menuMinWidth, Math.min(Theme.menuMaxWidth, naturalWidth + Theme.menuPadding * 2));
        return Math.min(wanted, room);
    }
    implicitHeight: rows.implicitHeight + (Theme.menuPadding - Theme.hairline) * 2 + topPadding + bottomPadding
    height: anchorItem ? Math.min(implicitHeight, Math.max(0, placement.room)) : implicitHeight
    transformOrigin: {
        if (align === "center") {
            return placement.side === "top" ? QQC2.Popup.Bottom : QQC2.Popup.Top;
        }
        return placement.side === "top" ? (align === "end" ? QQC2.Popup.BottomRight : QQC2.Popup.BottomLeft)
                                        : (align === "end" ? QQC2.Popup.TopRight : QQC2.Popup.TopLeft);
    }

    onAboutToShow: {
        adoptRows();
        place();
        let checkedRow = null;
        const kids = rows.children;
        for (let i = 0; i < kids.length; ++i) {
            if (kids[i].isMenuItem === true && kids[i].visible && kids[i].checked) {
                checkedRow = kids[i];
                break;
            }
        }
        highlightedItem = checkedRow;
        if (flick) {
            flick.contentY = 0;
        }
        showItem(highlightedItem);
    }
    onHighlightedItemChanged: showItem(highlightedItem)

    Connections {
        target: menu.visible ? QQC2.Overlay.overlay : null
        function onWidthChanged() { menu.close(); }
        function onHeightChanged() { menu.close(); }
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durationMenu; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: Theme.menuScaleFrom; to: 1; duration: Theme.durationMenu; easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; to: 0; duration: Theme.durationMenu; easing.type: Easing.InCubic }
    }

    background: Rectangle {
        radius: Theme.radiusLg
        color: Theme.raised
        border.width: Theme.hairline
        border.color: Theme.border
    }

    contentItem: Flickable {
        id: flick
        focus: true
        clip: interactive
        contentWidth: width
        contentHeight: rows.implicitHeight + rows.y * 2
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        Accessible.role: Accessible.PopupMenu
        QQC2.ScrollIndicator.vertical: QQC2.ScrollIndicator {}

        Keys.onUpPressed: menu.moveHighlight(-1)
        Keys.onDownPressed: menu.moveHighlight(1)
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Home || event.key === Qt.Key_PageUp) {
                menu.highlightedItem = null;
                menu.moveHighlight(1);
                event.accepted = true;
            } else if (event.key === Qt.Key_End || event.key === Qt.Key_PageDown) {
                menu.highlightedItem = null;
                menu.moveHighlight(-1);
                event.accepted = true;
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                if (menu.highlightedItem && menu.highlightedItem.isMenuItem === true) {
                    menu.highlightedItem.activate();
                }
                event.accepted = true;
            }
        }

        Component.onCompleted: rows.parent = flick.contentItem
    }
}
