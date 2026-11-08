// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Shapes
import stream.sugo.app

/// A picture of the running-timer tray icon, imitating TrayController's trayIcon(): the dark tile of
/// the application icon in the phase colour, in one of the badge styles of AppSettings.trayBadgeStyle.
/// Its colours come from TrayIconStyle, the same definition the real icon is painted with.
Item {
    id: badge

    /// 0: a border filling clockwise and the minutes, 1: a fill rising from the bottom and the
    /// minutes, 2: the icon's spiral filling from its centre, without numbers.
    property int badgeStyle: 0
    /// How much of the phase has run, 0..1.
    property real progress: 0
    property bool isBreak: false
    readonly property int totalMinutes: isBreak ? 5 : 25
    readonly property int minutes: Math.max(1, Math.ceil(totalMinutes * (1 - progress) - 0.0001))
    /// The badge colours, the same definition the real icon is painted with (TrayIconStyle):
    /// the accent colour for work.
    property color primary: isBreak ? TrayIconStyle.breakColor : TrayIconStyle.workColor
    Behavior on primary { ColorAnimation { duration: Theme.durationLong } }

    readonly property real clampedProgress: Math.min(1, Math.max(0, progress))
    readonly property real tileRadius: width * 0.25

    // artwork-begin
    // The border's centre line, from 12 o'clock clockwise, and its length.
    readonly property real penWidth: Math.max(Theme.hairline * 2, Math.round(width * 0.11))
    readonly property real lo: 0.5 + penWidth / 2
    readonly property real hi: width - lo
    readonly property real r: tileRadius - lo
    readonly property string borderPath: `M ${width / 2} ${lo} L ${hi - r} ${lo} A ${r} ${r} 0 0 1 ${hi} ${lo + r}`
        + ` L ${hi} ${hi - r} A ${r} ${r} 0 0 1 ${hi - r} ${hi} L ${lo + r} ${hi} A ${r} ${r} 0 0 1 ${lo} ${hi - r}`
        + ` L ${lo} ${lo + r} A ${r} ${r} 0 0 1 ${lo + r} ${lo} Z`
    readonly property real borderLength: 4 * (hi - lo - 2 * r) + 2 * Math.PI * r

    // The spiral of the application icon, from its centre out to 12 o'clock under the leaf, and its length.
    readonly property real spiralWidth: Math.max(1.8, width * 0.09)
    readonly property var spiral: {
        const sweep = 3.2 * Math.PI;
        let path = "";
        let length = 0;
        let px = 0;
        let py = 0;
        for (let t = 0; t <= sweep + 1e-9; t += 0.08) {
            const radius = width * (0.04 + 0.25 * t / sweep);
            const a = t - sweep - Math.PI / 2;
            const x = width * 0.5 + radius * Math.cos(a);
            const y = width * 0.553 + radius * Math.sin(a);
            if (t === 0) {
                path = `M ${x} ${y}`;
            } else {
                path += ` L ${x} ${y}`;
                length += Math.hypot(x - px, y - py);
            }
            px = x;
            py = y;
        }
        return { path: path, length: length };
    }

    // The leaf: drawn on its own grid, then scaled, turned by 20 degrees and put at the spiral's end.
    readonly property string leafPath: {
        const scale = width * 0.0072;
        const angle = 20 * Math.PI / 180;
        const point = (x, y) => {
            const sx = x * scale;
            const sy = y * scale;
            return `${width * 0.5 + sx * Math.cos(angle) - sy * Math.sin(angle)} ${width * 0.263 + sx * Math.sin(angle) + sy * Math.cos(angle)}`;
        };
        return `M ${point(0, 0)} C ${point(-10, -6)} ${point(-10, -18)} ${point(-2, -24)}`
            + ` C ${point(10, -18)} ${point(10, -6)} ${point(0, 0)} Z`;
    }
    // artwork-end

    implicitWidth: Theme.iconLg
    implicitHeight: implicitWidth
    height: width

    Rectangle {
        anchors.fill: parent
        anchors.margins: 0.5
        radius: badge.tileRadius
        color: TrayIconStyle.dialColor
        border.width: Theme.hairline
        border.color: TrayIconStyle.edgeColor
    }

    // Style 1: the tile filling from the bottom.
    Item {
        visible: badge.badgeStyle === 1
        anchors.bottom: parent.bottom
        width: parent.width
        height: parent.height * badge.clampedProgress
        clip: true
        Rectangle {
            x: 0.5
            y: parent.height - badge.height + 0.5
            width: badge.width - 1
            height: badge.height - 1
            radius: badge.tileRadius
            color: badge.primary
        }
    }

    // Styles 0 and 2: a track and, over it, its elapsed part. Each is drawn opaque in a layer of its
    // own: a see-through stroke would show darker dots where its segments overlap at the joins.
    Shape {
        id: line
        anchors.fill: parent
        containsMode: Shape.BoundingRectContains
        enabled: false
        visible: badge.badgeStyle !== 1
        opacity: TrayIconStyle.trackOpacity
        layer.enabled: true
        layer.samples: 4
        readonly property string path: badge.badgeStyle === 2 ? badge.spiral.path : badge.borderPath
        readonly property real length: badge.badgeStyle === 2 ? badge.spiral.length : badge.borderLength
        readonly property real stroke: badge.badgeStyle === 2 ? badge.spiralWidth : badge.penWidth

        ShapePath {
            strokeColor: badge.primary
            strokeWidth: line.stroke
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: line.path }
        }
    }
    Shape {
        anchors.fill: parent
        containsMode: Shape.BoundingRectContains
        enabled: false
        layer.enabled: true
        layer.samples: 4

        ShapePath {
            // Fully transparent at 0 so the round cap does not draw a dot.
            strokeColor: badge.badgeStyle !== 1 && badge.clampedProgress > 0.005 ? badge.primary : "transparent"
            strokeWidth: line.stroke
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            // In pen widths: the elapsed part, then a gap past the end, as in the real icon.
            strokeStyle: ShapePath.DashLine
            dashPattern: [line.length / line.stroke * badge.clampedProgress,
                line.length / line.stroke + 1]
            PathSvg { path: line.path }
        }
        // Style 2: the leaf at the end of the spiral.
        ShapePath {
            strokeColor: "transparent"
            fillColor: badge.badgeStyle === 2 ? TrayIconStyle.leafColor : "transparent"
            PathSvg { path: badge.leafPath }
        }
    }

    Text {
        visible: badge.badgeStyle !== 2
        anchors.centerIn: parent
        text: badge.minutes
        color: TrayIconStyle.digitColor
        font.bold: true
        font.pixelSize: Math.round(badge.width * (text.length >= 2 ? 0.40 : 0.48))
    }
}
