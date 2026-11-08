// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import stream.sugo.app

/// One timer: everything about it on one page, changed in place and kept with Save.
QQC2.Page {
    id: page

    /// Empty means "create a new timer".
    property string uuid: ""

    readonly property bool isNew: uuid === ""
    readonly property var preset: isNew
        ? ({ name: "", category: "", iconName: "sugo-timer", workSeconds: 25 * 60, shortBreakSeconds: 5 * 60,
             longBreakSeconds: 15 * 60, cyclesBeforeLong: 4, builtin: false,
             options: ({ autoStartMode: 0, silenceWhileWorking: false, soundOnEnd: true, notifyOnEnd: true }) })
        : PresetModel.get(uuid)
    /// The controls' starting values: what they show for `preset`.
    readonly property var shown: shownFields(preset)
    property string selectedIcon: shown.iconName
    readonly property var iconChoices: PresetModel.availableIcons()
    readonly property bool valid: nameField.text.trim().length > 0
    readonly property bool isCurrent: !isNew && PresetModel.currentUuid === uuid
    /// Deleting the running timer would switch timers, which stops the session.
    readonly property bool lockedByTimer: isCurrent && TimerEngine.active

    // `preset` is read once, for the controls' starting values; what shows the stored name follows
    // every change of the timer (a save, a rename elsewhere) through this count, and so do the
    // controls the user has not changed (followStored()).
    property int storedRevision: 0
    Connections {
        target: PresetModel
        function onPresetChanged(changedUuid) {
            if (changedUuid === page.uuid) {
                ++page.storedRevision;
                page.followStored();
            }
        }
    }
    /// The name as stored, not as being typed.
    readonly property string storedName: {
        void storedRevision;
        return isNew ? "" : (PresetModel.get(uuid).name || "");
    }

    /// The header shows "Timers / <name>".
    title: isNew ? i18n("New Timer") : storedName
    background: null

    /// What Save would store, as the controls show it now.
    function currentFields(): var {
        return {
            name: nameField.text,
            category: categoryField.text,
            iconName: page.selectedIcon,
            workSeconds: workBox.value * 60,
            shortBreakSeconds: shortBox.value * 60,
            longBreakSeconds: longBox.value * 60,
            cyclesBeforeLong: cyclesBox.value,
            autoStartMode: autoStartBox.currentIndex,
            silenceWhileWorking: silenceBox.checked,
            soundOnEnd: soundBox.checked,
            notifyOnEnd: notifyBox.checked
        };
    }

    /// What the controls show for the stored timer `p` (a PresetModel.get() map), as currentFields() reads them.
    function shownFields(p: var): var {
        const o = p.options || {};
        return {
            name: p.name || "",
            category: p.category || "",
            iconName: p.iconName || "sugo-timer",
            workSeconds: Math.max(1, Math.round(p.workSeconds / 60)) * 60,
            shortBreakSeconds: Math.round(p.shortBreakSeconds / 60) * 60,
            longBreakSeconds: Math.round(p.longBreakSeconds / 60) * 60,
            cyclesBeforeLong: p.cyclesBeforeLong,
            autoStartMode: o.autoStartMode !== undefined ? Math.max(0, Math.min(2, Number(o.autoStartMode))) : 0,
            silenceWhileWorking: o.silenceWhileWorking !== undefined ? Boolean(o.silenceWhileWorking) : false,
            soundOnEnd: o.soundOnEnd !== undefined ? Boolean(o.soundOnEnd) : true,
            notifyOnEnd: o.notifyOnEnd !== undefined ? Boolean(o.notifyOnEnd) : true
        };
    }

    /// The timer changed while the page is open (Sugo Sync brought another computer's version, or a
    /// save): the controls the user has not changed show the stored values, the others keep what
    /// the user made of them.
    function followStored(): void {
        if (isNew || initialFields.length === 0) {
            return;
        }
        const stored = PresetModel.get(uuid);
        if (stored.uuid === undefined) {
            return;
        }
        const was = JSON.parse(initialFields);
        const now = currentFields();
        const next = shownFields(stored);
        const show = {
            name: v => nameField.text = v,
            category: v => categoryField.text = v,
            iconName: v => page.selectedIcon = v,
            workSeconds: v => workBox.value = v / 60,
            shortBreakSeconds: v => shortBox.value = v / 60,
            longBreakSeconds: v => longBox.value = v / 60,
            cyclesBeforeLong: v => cyclesBox.value = v,
            autoStartMode: v => autoStartBox.currentIndex = v,
            silenceWhileWorking: v => silenceBox.checked = v,
            soundOnEnd: v => soundBox.checked = v,
            notifyOnEnd: v => notifyBox.checked = v
        };
        for (const key in show) {
            if (now[key] === was[key] && next[key] !== was[key]) {
                show[key](next[key]);
            }
        }
        initialFields = JSON.stringify(next);
    }

    // The fields as stored when the page opened or last followed the timer, to tell what was changed since.
    property string initialFields: ""
    // Taken once every control has its starting value.
    Component.onCompleted: Qt.callLater(() => initialFields = JSON.stringify(currentFields()))
    readonly property bool modified: initialFields.length > 0 && JSON.stringify(currentFields()) !== initialFields
    /// A confirmation is showing (also read by the smoke test).
    readonly property bool confirming: discardDialog.visible || resetDialog.visible || deleteDialog.visible

    /// Save does something: a new timer, or changes to store.
    readonly property bool canSave: valid && (isNew || modified)

    /// Takes the numbers still being typed: clicking a button or a shortcut leaves the focus in the field.
    function commitFields(): void {
        workBox.commit();
        shortBox.commit();
        longBox.commit();
        cyclesBox.commit();
    }

    function save(): void {
        commitFields();
        if (!canSave) {
            return;
        }
        const fields = currentFields();
        if (isNew) {
            const created = PresetModel.create(fields);
            leaving = true;
            applicationWindow().openTimer(created);
        } else {
            // Only what the user changed: the rest keeps what is stored, which may have come from
            // another computer since the page opened.
            const was = JSON.parse(initialFields);
            const changed = {};
            for (const key in fields) {
                if (fields[key] !== was[key]) {
                    changed[key] = fields[key];
                }
            }
            if (!PresetModel.update(uuid, changed)) {
                // Deleted meanwhile (on another computer, through Sugo Sync).
                applicationWindow().showPassiveNotification(
                    i18n("This timer no longer exists, so your changes were not saved."));
                return;
            }
            initialFields = JSON.stringify(currentFields());
        }
    }

    /// Back to the list of the timers (a new timer that is not created after all).
    function close(): void {
        applicationWindow().openSettings("timers");
    }

    // Set once the page is being left for good, so leaving does not ask again.
    property bool leaving: false

    /// Leaves the page for `then` (e.g. going to another page); without it, drops the changes.
    function leave(then: var): void {
        leaving = true;
        if (then) {
            then();
        } else if (isNew) {
            close();
        } else {
            applicationWindow().openTimer(uuid, true);
        }
    }

    /// Escape, the back link and the tabs: unsaved changes are only thrown away once confirmed.
    function requestClose(then: var): void {
        commitFields();
        discardDialog.then = then ?? null;
        if (modified) {
            discardDialog.open();
        } else {
            leave(discardDialog.then);
        }
    }

    /// Opens a copy of the stored timer, once unsaved changes are confirmed thrown away: the
    /// copy is not made while the question is open, nor when the answer is Keep Editing.
    function duplicate(): void {
        requestClose(() => {
            const copy = PresetModel.duplicate(page.uuid);
            applicationWindow().openTimer(copy.length > 0 ? copy : page.uuid, true);
        });
    }

    function requestReset(): void {
        resetDialog.open();
    }

    ConfirmDialog {
        id: discardDialog
        property var then: null
        title: i18n("Discard Changes?")
        text: i18n("The changes you made to this timer have not been saved.")
        confirmText: i18n("Discard")
        confirmKind: "danger"
        cancelText: i18n("Keep Editing")
        onConfirmed: {
            discardDialog.close();
            page.leave(discardDialog.then);
        }
    }

    ConfirmDialog {
        id: resetDialog
        title: i18n("Reset to Defaults?")
        text: i18n("“%1” goes back to its original durations and options. Your changes to it are lost.", page.storedName)
        confirmText: i18n("Reset")
        onConfirmed: {
            resetDialog.close();
            PresetModel.reset(page.uuid);
            page.leaving = true;
            applicationWindow().openTimer(page.uuid, true);
        }
    }

    ConfirmDialog {
        id: deleteDialog
        title: i18n("Delete Timer")
        text: i18n("Delete “%1”? Statistics already recorded with it are kept.", page.storedName)
        confirmText: i18n("Delete")
        confirmKind: "danger"
        onConfirmed: {
            deleteDialog.close();
            // The timer may have been started meanwhile (tray, shortcut, D-Bus).
            if (page.lockedByTimer) {
                applicationWindow().showPassiveNotification(
                    i18n("“%1” was started meanwhile, so it was not deleted. Stop it to delete it.", page.storedName));
                return;
            }
            page.leaving = true;
            PresetModel.remove(page.uuid);
            applicationWindow().openSettings("timers");
        }
    }

    Shortcut {
        sequences: [StandardKey.Save]
        enabled: page.visible && !page.confirming
        onActivated: page.save()
    }

    Shortcut {
        sequences: [StandardKey.Cancel]
        enabled: page.visible && !autoStartBox.popup.visible && !iconPopup.visible && !page.confirming && (page.modified || page.isNew)
        onActivated: page.requestClose()
    }

    /// The window is too narrow for the list of the settings' categories beside the page.
    readonly property bool narrow: page.Window.width > 0 && page.Window.width < Theme.settingsNavBreakpoint
    /// Too narrow for the timer's name in the header.
    readonly property bool compact: width < Theme.compactBreakpoint
    // The page's padding: less in a narrow window.
    readonly property real paddingX: narrow ? Theme.settingsBodyPaddingCompactX : Theme.settingsBodyPaddingX
    readonly property real paddingY: narrow ? Theme.settingsBodyPaddingCompactY : Theme.settingsBodyPaddingY

    // Back to the list of the timers, where the timer is, and what can be done with it. Above the
    // page, so Save stays at hand however far the page is scrolled.
    // Laid out as the settings' categories: the title where theirs is and the page below it at
    // the same height, the buttons (taller than the title) centred on it without making the row
    // taller, so opening a timer from the Timers list moves nothing.
    header: Item {
        implicitHeight: page.paddingY + titleText.implicitHeight + Theme.spaceXs + Theme.spaceSm

        RowLayout {
            id: headerRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: page.paddingX
            anchors.rightMargin: page.paddingX
            anchors.topMargin: page.paddingY - (implicitHeight - titleText.implicitHeight) / 2
            spacing: Theme.spaceSm

            // "Timers", back to the list: unsaved changes are only thrown away once confirmed.
            QQC2.AbstractButton {
                id: backLink
                objectName: "timersBackLink"
                implicitHeight: Theme.controlHeight
                leftPadding: Theme.spaceXs
                rightPadding: Theme.spaceXs
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                text: i18n("Timers")
                Accessible.name: text
                Layout.leftMargin: -Theme.spaceXs
                onClicked: page.requestClose(() => applicationWindow().openSettings("timers"))
                background: Rectangle {
                    radius: Theme.radiusSm
                    color: "transparent"
                    border.width: backLink.visualFocus ? Theme.hairline : 0
                    border.color: Theme.focusRing
                }
                // Just the word, drawn as the Timers list draws its title and where it does, so
                // opening a timer only adds "/ <name>" after it.
                contentItem: Text {
                    text: backLink.text
                    verticalAlignment: Text.AlignVCenter
                    color: backLink.hovered ? Theme.foreground : Theme.muted
                    font.pixelSize: Theme.fontSettingsTitle
                    font.weight: Font.DemiBold
                }
            }
            Text {
                visible: !page.compact
                text: "/"
                color: Theme.subtle
                font.pixelSize: Theme.fontSettingsTitle
            }
            Text {
                id: titleText
                visible: !page.compact
                Layout.fillWidth: true
                text: page.title
                color: Theme.foreground
                font.pixelSize: Theme.fontSettingsTitle
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                Accessible.role: Accessible.Heading
            }
            Item {
                visible: page.compact
                Layout.fillWidth: true
            }

            SButton {
                visible: !page.isNew
                kind: "ghost"
                iconOnly: true
                iconName: "copy"
                text: i18n("Duplicate")
                onClicked: page.duplicate()
            }
            // A disabled button gets no hover events, so the wrapper says why it cannot be deleted.
            Item {
                visible: !page.isNew
                implicitWidth: deleteButton.implicitWidth
                implicitHeight: deleteButton.implicitHeight
                HoverHandler {
                    id: deleteHover
                }
                QQC2.ToolTip.visible: !deleteButton.enabled && deleteHover.hovered
                QQC2.ToolTip.text: deleteButton.text
                QQC2.ToolTip.delay: Theme.toolTipDelay
                SButton {
                    id: deleteButton
                    objectName: "deleteButton-" + page.uuid
                    anchors.fill: parent
                    kind: "ghost"
                    iconOnly: true
                    destructive: true
                    iconName: "trash"
                    text: page.preset.builtin ? i18n("The default timer cannot be deleted")
                        : page.lockedByTimer ? i18n("This timer is running. Stop it to delete it.")
                        : i18n("Delete")
                    enabled: !page.preset.builtin && !page.lockedByTimer
                    onClicked: deleteDialog.open()
                }
            }
            // The timer's own actions stand apart from what is done with its changes.
            Rectangle {
                visible: !page.isNew
                Layout.preferredWidth: Theme.hairline
                Layout.preferredHeight: Theme.controlHeight - Theme.spaceMd * 2
                Layout.alignment: Qt.AlignVCenter
                color: Theme.border
            }
            // The changes, together: back to the default values (a built-in timer only), back to
            // what is stored, and Save.
            SButton {
                objectName: "resetButton"
                visible: page.preset.builtin === true
                kind: "ghost"
                iconOnly: true
                iconName: "refresh"
                text: i18n("Reset to Defaults")
                onClicked: page.requestReset()
            }
            // Always there, lit only when there is something to take back, so nothing moves.
            SButton {
                objectName: "revertButton"
                readonly property bool active: page.isNew || page.modified
                kind: "ghost"
                iconOnly: true
                iconName: "undo"
                lit: active
                text: page.isNew ? i18n("Cancel") : i18n("Revert")
                enabled: active
                onClicked: page.requestClose()
            }
            SButton {
                kind: "primary"
                iconOnly: true
                iconName: "save"
                text: page.isNew ? i18n("Create Timer") : i18n("Save")
                enabled: page.canSave
                onClicked: page.save()
            }
        }
    }

    // The icons to choose from, under the icon's button, right edges together.
    QQC2.Popup {
        id: iconPopup
        parent: iconTile
        x: iconTile.width - width
        y: iconTile.height + Theme.menuOffset
        width: Theme.space4xl * 6.5
        margins: Theme.menuEdgeMargin
        padding: Theme.spaceMd
        focus: true
        // A press on the button is left to the button, which toggles the popup: closing on the
        // press would have the click open it again.
        closePolicy: QQC2.Popup.CloseOnEscape | QQC2.Popup.CloseOnPressOutsideParent
        background: Rectangle {
            radius: Theme.radiusLg
            color: Theme.raised
            border.width: Theme.hairline
            border.color: Theme.border
        }
        contentItem: Flow {
            spacing: Theme.spaceXs
            Repeater {
                model: page.iconChoices
                delegate: QQC2.AbstractButton {
                    id: iconBtn
                    required property var modelData
                    readonly property bool isSelected: page.selectedIcon === modelData.iconName
                    hoverEnabled: true
                    text: modelData.label
                    Accessible.role: Accessible.RadioButton
                    Accessible.checked: isSelected
                    implicitWidth: Theme.iconXl + Theme.spaceSm
                    implicitHeight: implicitWidth
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.text: modelData.label
                    QQC2.ToolTip.delay: Theme.toolTipDelay
                    background: Rectangle {
                        radius: Theme.radius
                        color: iconBtn.isSelected ? Theme.selected : (iconBtn.hovered ? Theme.hover : "transparent")
                        border.width: iconBtn.isSelected || iconBtn.visualFocus ? Theme.hairline : 0
                        border.color: iconBtn.visualFocus ? Theme.focusRing : Theme.borderStrong
                    }
                    contentItem: Item {
                        TimerIcon {
                            anchors.centerIn: parent
                            iconName: iconBtn.modelData.iconName
                            size: Theme.iconLg - Theme.spaceXs
                            color: Theme.accent
                        }
                    }
                    onClicked: {
                        page.selectedIcon = modelData.iconName;
                        iconPopup.close();
                    }
                }
            }
        }
    }

    Flickable {
        id: flickable
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight + page.paddingY + Theme.spaceMd
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        QQC2.ScrollBar.vertical: QQC2.ScrollBar {}

        ColumnLayout {
            id: column
            x: page.paddingX
            y: 0
            width: Math.max(0, flickable.width - page.paddingX * 2)
            spacing: Theme.spaceSm

            // One cycle as it will run: every phase as long as it lasts, work in the accent colour.
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: cycleColumn.implicitHeight + Theme.spaceLg * 2
                radius: Theme.radiusLg
                color: Theme.card
                border.width: Theme.hairline
                border.color: Theme.border

                readonly property var phases: {
                    const list = [];
                    const sessions = Math.max(1, cyclesBox.value);
                    for (let i = 0; i < sessions; ++i) {
                        list.push({ work: true, minutes: workBox.value });
                        if (i < sessions - 1 || cyclesBox.value === 0) {
                            if (shortBox.value > 0) {
                                list.push({ work: false, long: false, minutes: shortBox.value });
                            }
                        } else if (longBox.value > 0) {
                            list.push({ work: false, long: true, minutes: longBox.value });
                        }
                    }
                    return list;
                }
                readonly property int totalMinutes: phases.reduce((sum, p) => sum + p.minutes, 0)

                ColumnLayout {
                    id: cycleColumn
                    anchors.fill: parent
                    anchors.topMargin: Theme.spaceLg
                    anchors.bottomMargin: Theme.spaceLg
                    anchors.leftMargin: Theme.settingsRowPaddingX
                    anchors.rightMargin: Theme.settingsRowPaddingX
                    spacing: Theme.spaceSm

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: cyclesBox.value > 0 ? i18n("One cycle") : i18n("Work and break")
                            color: Theme.muted
                            font.pixelSize: Theme.fontCaption
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                        Text {
                            readonly property int total: cycleColumn.parent.totalMinutes
                            text: total >= 60 ? i18nc("hours and minutes", "%1h %2m", Math.floor(total / 60), total % 60)
                                              : i18nc("minutes", "%1 min", total)
                            color: Theme.muted
                            font.pixelSize: Theme.fontCaption
                            font.features: { "tnum": 1 }
                        }
                    }

                    Item {
                        id: cycleRow
                        objectName: "cyclePreview"
                        Layout.fillWidth: true
                        // Taller than the track (a small space above and below), to grab it.
                        implicitHeight: Theme.cyclePreviewHeight + Theme.spaceLg
                        // A long set (up to 99 work phases) still fits: the gaps take at most half the
                        // row, the blocks' minimum width shrinks to their share of the rest, and the
                        // row clips what rounding leaves over.
                        clip: true
                        readonly property var phases: cycleColumn.parent.phases
                        readonly property int count: phases.length
                        readonly property real total: Math.max(1, cycleColumn.parent.totalMinutes)
                        readonly property real gap: count > 1 ? Math.min(Theme.cycleTrackGap, width / 2 / (count - 1)) : 0
                        readonly property real available: Math.max(0, width - gap * (count - 1))
                        readonly property real minBlock: Math.min(Theme.spaceXs, available / Math.max(1, count))
                        // Each block as long as its phase, at least minBlock; when those minimums would
                        // push the row past its box, each gets the minimum plus its share of what is left
                        // (the hairline only absorbs floating-point error in a sum that fits).
                        readonly property bool crowded: phases.reduce(
                            (sum, p) => sum + Math.max(minBlock, available * p.minutes / total), 0) > available + Theme.hairline
                        /// Pixels per minute of the blocks' variable part (what a drag moves).
                        readonly property real pixelsPerMinute: (crowded ? available - minBlock * count : available) / total
                        Row {
                            id: cycleBlocks
                            objectName: "cycleBlocks"
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            height: Theme.cyclePreviewHeight
                            spacing: cycleRow.gap
                            Repeater {
                                model: cycleRow.phases
                                Rectangle {
                                    id: block
                                    required property var modelData
                                    width: cycleRow.crowded ? cycleRow.minBlock + cycleRow.pixelsPerMinute * modelData.minutes
                                                            : Math.max(cycleRow.minBlock, cycleRow.pixelsPerMinute * modelData.minutes)
                                    height: cycleBlocks.height
                                    radius: height / 2
                                    color: modelData.work ? Theme.accent : Theme.alpha(Theme.breakAccent, modelData.long ? 1 : 0.55)
                                }
                            }
                        }
                        // Drag a phase by one of its edges, as when resizing a partition: the half you grab is the
                        // edge that moves, so dragging outwards lengthens it (every phase of its kind follows).
                        // Lives on the row, not the blocks: each change rebuilds the blocks and would drop the grab.
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.SizeHorCursor
                            preventStealing: true
                            property var box: null
                            property int startValue: 0
                            property real startX: 0
                            // +1 when the right edge was grabbed, -1 for the left one.
                            property int direction: 1
                            // Frozen at press, so the scale does not shift under the pointer while dragging.
                            property real pixelsPerMinute: 1
                            onPressed: mouse => {
                                const block = cycleBlocks.childAt(mouse.x, cycleBlocks.height / 2);
                                box = block && block.modelData ? (block.modelData.work ? workBox : block.modelData.long ? longBox : shortBox) : null;
                                if (!box) {
                                    mouse.accepted = false;
                                    return;
                                }
                                startValue = box.value;
                                startX = mouse.x;
                                direction = mouse.x < block.x + block.width / 2 ? -1 : 1;
                                pixelsPerMinute = Math.max(1, cycleRow.pixelsPerMinute);
                            }
                            onPositionChanged: mouse => {
                                if (box) {
                                    // At least a minute: a phase dragged to nothing would leave no block to drag back.
                                    box.setValue(Math.max(1, startValue + direction * Math.round((mouse.x - startX) / pixelsPerMinute)));
                                }
                            }
                            onReleased: box = null
                            onCanceled: box = null
                        }
                    }
                }
            }

            FormSection {
                title: i18n("Timer")

                FormRow {
                    id: nameRow
                    first: true
                    title: i18n("Name")
                    STextField {
                        id: nameField
                        Layout.preferredWidth: Theme.fieldWidthWide
                        Layout.fillWidth: !nameRow.sideBySide
                        text: page.shown.name
                        maximumLength: PresetModel.maxNameLength
                        placeholderText: i18n("Name this timer")
                        Accessible.name: i18n("Name")
                        onAccepted: page.save()
                        Component.onCompleted: if (page.isNew) forceActiveFocus()
                    }
                }
                FormRow {
                    id: categoryRow
                    title: i18n("Category")
                    STextField {
                        id: categoryField
                        Layout.preferredWidth: Theme.fieldWidthWide
                        Layout.fillWidth: !categoryRow.sideBySide
                        text: page.shown.category
                        maximumLength: PresetModel.maxNameLength
                        placeholderText: i18n("Add a category, e.g. Work or Study")
                        Accessible.name: i18n("Category")
                        onAccepted: page.save()
                    }
                }
                FormRow {
                    title: i18n("Icon")
                    // The icon in a button like a drop-down list's: a click shows them all.
                    QQC2.AbstractButton {
                        id: iconTile
                        implicitHeight: Theme.controlHeightSm
                        implicitWidth: iconTileRow.implicitWidth + leftPadding + rightPadding
                        leftPadding: Theme.spaceSm
                        rightPadding: Theme.spaceMd
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        text: i18n("Icon")
                        Accessible.description: i18n("Choose the icon of this timer")
                        QQC2.ToolTip.visible: hovered && !iconPopup.visible
                        QQC2.ToolTip.text: i18n("Change the icon")
                        QQC2.ToolTip.delay: Theme.toolTipDelay
                        onClicked: iconPopup.opened ? iconPopup.close() : iconPopup.open()
                        background: Rectangle {
                            radius: Theme.radius
                            color: iconTile.pressed ? Qt.tint(Theme.background, Theme.pressed)
                                 : iconTile.hovered && !iconPopup.visible ? Qt.tint(Theme.background, Theme.hover) : Theme.background
                            border.width: Theme.hairline
                            border.color: iconTile.visualFocus ? Theme.focusRing : iconPopup.visible ? Theme.borderStrong : Theme.border
                        }
                        contentItem: Item {
                            implicitWidth: iconTileRow.implicitWidth
                            implicitHeight: iconTileRow.implicitHeight
                            Row {
                                id: iconTileRow
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: Theme.spaceSm
                                TimerIcon {
                                    anchors.verticalCenter: parent.verticalCenter
                                    iconName: page.selectedIcon
                                    size: Theme.iconMd
                                    color: Theme.accent
                                }
                                LineIcon {
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: "chevron-down"
                                    size: Theme.iconSm
                                    color: Theme.muted
                                    rotation: iconPopup.visible ? 180 : 0
                                }
                            }
                        }
                    }
                }
            }

            FormSection {
                title: i18n("Durations")

                FormRow {
                    first: true
                    title: i18n("Work")
                    // A few common lengths, one click away.
                    Repeater {
                        model: [15, 25, 45, 50]
                        delegate: QQC2.AbstractButton {
                            id: chip
                            required property int modelData
                            readonly property bool on: workBox.value === modelData
                            implicitHeight: Theme.controlHeightSm
                            leftPadding: Theme.spaceMd
                            rightPadding: Theme.spaceMd
                            hoverEnabled: true
                            focusPolicy: Qt.TabFocus
                            text: modelData
                            Accessible.name: i18nc("@action set the work duration", "%1 minutes", modelData)
                            onClicked: workBox.setValue(modelData)
                            background: Rectangle {
                                radius: Theme.radius
                                color: chip.on ? Theme.selected : (chip.hovered ? Theme.hover : "transparent")
                                border.width: Theme.hairline
                                border.color: chip.on ? Theme.borderStrong : Theme.border
                            }
                            contentItem: Text {
                                text: chip.text
                                color: chip.on ? Theme.foreground : Theme.muted
                                font.pixelSize: Theme.fontCaption
                                font.features: { "tnum": 1 }
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                    NumberField {
                        id: workBox
                        objectName: "workField"
                        Layout.leftMargin: Theme.spaceSm
                        from: 1
                        to: PresetModel.maxWorkMinutes
                        value: page.shown.workSeconds / 60
                        unit: i18nc("minutes, short", "min")
                        Accessible.name: i18n("Work")
                    }
                }
                FormRow {
                    title: i18n("Short break")
                    NumberField {
                        id: shortBox
                        objectName: "shortBreakField"
                        // 0 is no break (the engine goes straight back to work): typed, not dragged.
                        from: 0
                        to: PresetModel.maxBreakMinutes
                        value: page.shown.shortBreakSeconds / 60
                        unit: i18nc("minutes, short", "min")
                        fromText: i18nc("no break", "None")
                        Accessible.name: i18n("Short break")
                    }
                }
                FormRow {
                    title: i18n("Long break")
                    NumberField {
                        id: longBox
                        objectName: "longBreakField"
                        // 0 is no break (the engine goes straight back to work): typed, not dragged.
                        from: 0
                        to: PresetModel.maxBreakMinutes
                        value: page.shown.longBreakSeconds / 60
                        unit: i18nc("minutes, short", "min")
                        fromText: i18nc("no break", "None")
                        Accessible.name: i18n("Long break")
                    }
                }
                FormRow {
                    title: i18n("Long break after")
                    description: i18n("How many work sessions come before the long break.")
                    NumberField {
                        id: cyclesBox
                        objectName: "cyclesField"
                        from: 0
                        to: PresetModel.maxCycles
                        value: page.shown.cyclesBeforeLong
                        unit: i18ncp("work sessions, short", "session", "sessions", cyclesBox.value)
                        fromText: i18n("Never")
                        Accessible.name: i18n("Long break after")
                    }
                }
            }

            FormSection {
                title: i18n("Behavior")

                FormRow {
                    first: true
                    title: i18n("Start automatically")
                    description: i18n("Whether breaks, work phases, or neither start by themselves.")
                    SComboBox {
                        id: autoStartBox
                        Accessible.name: i18n("Start automatically")
                        model: [i18n("Manual"), i18n("Breaks only"), i18n("All phases")]
                        currentIndex: page.shown.autoStartMode
                    }
                }
                FormRow {
                    title: i18n("Silence notifications while working")
                    // Availability can change while the page is open (the shortcuts installed, the
                    // notification service appearing): the row keeps the height of the taller text.
                    readonly property var silenceTexts: DesktopStyle.macos
                        ? [i18n("Turns Do Not Disturb on for the work phase and off for the break, through the two Focus shortcuts."),
                           i18n("Needs the two Focus shortcuts: install them in Settings → Focus.")]
                        : [i18n("Turns off all system notifications for the work phase and back on for the break."),
                           i18n("Not available: this desktop's notification service cannot be silenced by applications.")]
                    description: silenceTexts[SilenceController.available ? 0 : 1]
                    descriptionVariants: silenceTexts
                    enabled: SilenceController.available || DesktopStyle.macos
                    // macOS: until the shortcuts exist, a way to set them up instead of the switch.
                    SButton {
                        objectName: "silenceSetupButton"
                        visible: DesktopStyle.macos && !SilenceController.available
                        text: i18n("Set Up…")
                        iconName: "settings"
                        onClicked: applicationWindow().openSettings("focus")
                    }
                    SSwitch {
                        id: silenceBox
                        visible: SilenceController.available || !DesktopStyle.macos
                        enabled: SilenceController.available
                        Accessible.name: i18n("Silence notifications while working")
                        checked: page.shown.silenceWhileWorking
                    }
                }
                FormRow {
                    title: i18n("Play a sound when a phase ends")
                    SSwitch {
                        id: soundBox
                        Accessible.name: i18n("Play a sound when a phase ends")
                        checked: page.shown.soundOnEnd
                    }
                }
                FormRow {
                    title: i18n("Show a notification when a phase ends")
                    SSwitch {
                        id: notifyBox
                        Accessible.name: i18n("Show a notification when a phase ends")
                        checked: page.shown.notifyOnEnd
                    }
                }
            }
        }
    }
}
