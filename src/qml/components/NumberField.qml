// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import stream.sugo.app

/**
 * A small number field with its unit ("25 min"): type the number, or use the arrow keys or the
 * mouse wheel. A number being typed counts as soon as it is in range; `commit()` takes whatever is
 * typed (brought into range) and shows it back.
 */
QQC2.Control {
    id: field

    property int value: 0
    property int from: 0
    property int to: 99
    /// After the number, e.g. "min".
    property string unit
    /// Shown instead of the number for `from` (e.g. "Never" for 0), if set.
    property string fromText

    function setValue(v: int): void {
        const clamped = Math.max(from, Math.min(to, v));
        if (clamped !== value) {
            value = clamped;
        }
        input.text = Qt.binding(() => field.displayText);
    }

    /// Takes what is typed, as leaving the field does: before acting on `value` (e.g. Save), since
    /// clicking a button does not take the focus from the field.
    /// The whole text must be a whole number, sign included ("-5" is below `from`, not 5); any
    /// other text leaves the value as it was.
    function commit(): void {
        const match = input.text.match(/^\s*([+-]?\d+)\s*$/);
        // Brought into range here already: a number too long for an int must not wrap.
        setValue(match ? Math.max(from, Math.min(to, parseInt(match[1], 10))) : value);
    }

    /// One step up (1) or down (-1), as the arrow keys and the mouse wheel do: from what is typed
    /// (brought into range), not from the value it had.
    function step(delta: int): void {
        commit();
        setValue(value + delta);
    }

    readonly property string displayText: fromText.length > 0 && value === from ? fromText : String(value)

    implicitWidth: Math.max(Theme.space4xl * 1.7, row.implicitWidth + leftPadding + rightPadding)
    implicitHeight: Theme.controlHeightSm
    leftPadding: Theme.spaceMd + Theme.hairline * 2
    rightPadding: Theme.spaceMd + Theme.hairline * 2
    hoverEnabled: true
    activeFocusOnTab: true
    onActiveFocusChanged: if (activeFocus) input.forceActiveFocus()

    background: Rectangle {
        radius: Theme.radius
        color: Theme.background
        border.width: Theme.hairline
        border.color: input.activeFocus ? Theme.focusRing : (field.hovered ? Theme.borderStrong : Theme.border)
        Behavior on border.color { ColorAnimation { duration: Theme.durationShort } }
    }

    contentItem: RowLayout {
        id: row
        spacing: Theme.spaceXs
        TextInput {
            id: input
            Layout.fillWidth: true
            text: field.displayText
            color: field.enabled ? Theme.foreground : Theme.subtle
            selectionColor: Theme.alpha(Theme.accent, 0.35)
            selectedTextColor: Theme.foreground
            font.pixelSize: Theme.fontBody
            font.weight: Font.Medium
            font.features: { "tnum": 1 }
            horizontalAlignment: Text.AlignRight
            selectByMouse: true
            inputMethodHints: Qt.ImhDigitsOnly
            Accessible.name: field.Accessible.name
            Keys.onUpPressed: field.step(1)
            Keys.onDownPressed: field.step(-1)
            onEditingFinished: field.commit()
            // A number in range counts while it is typed (unsaved changes, Save), out of range only
            // once committed. The text stays as typed until then: no rewriting under the caret
            // ("0" into "Never", "05" into "5"), so the assignment below unbinds it first.
            onTextEdited: {
                text = text;
                const match = text.match(/^\s*(\d+)\s*$/);
                const typed = match ? parseInt(match[1]) : NaN;
                if (typed >= field.from && typed <= field.to && typed !== field.value) {
                    field.value = typed;
                }
            }
            onActiveFocusChanged: if (activeFocus) selectAll()
        }
        Text {
            visible: field.unit.length > 0 && !(field.fromText.length > 0 && field.value === field.from)
            text: field.unit
            color: Theme.muted
            font.pixelSize: Theme.fontBody
        }
    }

    // Only for the field being edited: scrolling the page over a field must not change it.
    WheelHandler {
        enabled: input.activeFocus
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        // Only a vertical turn steps: a horizontal scroll (touchpad, Shift+wheel) or an event
        // without one (the end of a touchpad gesture) is no step down.
        onWheel: event => {
            if (event.angleDelta.y !== 0)
                field.step(event.angleDelta.y > 0 ? 1 : -1);
        }
    }
}
