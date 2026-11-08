// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Shapes
import stream.sugo.app

/// A thin line icon of the interface, drawn on a 16 × 16 grid and scaled to `size`.
Item {
    id: icon

    /// One of the names in `paths` below.
    property string name
    property color color: Theme.foreground
    property real size: Theme.icon
    /// Fills the outline instead of stroking it (play, stop).
    readonly property bool filled: name === "play" || name === "stop" || name === "dot"

    implicitWidth: size
    implicitHeight: size

    // artwork-begin
    readonly property var paths: ({
        "timer": "M2.5 9a5.5 5.5 0 1 0 11 0a5.5 5.5 0 1 0 -11 0M8 6v3l2 1.2M6.5 1.5h3",
        "list": "M3 4h10M3 8h10M3 12h6",
        "stats": "M3 13.5V8.5M8 13.5V3M13 13.5V6",
        "settings": "M5.8 8a2.2 2.2 0 1 0 4.4 0a2.2 2.2 0 1 0 -4.4 0M8 1.5v2M8 12.5v2M1.5 8h2M12.5 8h2M3.4 3.4l1.4 1.4M11.2 11.2l1.4 1.4M3.4 12.6l1.4 -1.4M11.2 4.8l1.4 -1.4",
        "info": "M2 8a6 6 0 1 0 12 0a6 6 0 1 0 -12 0M8 7.3v3.7M8 5v0.01",
        "warning": "M8 2.2l6.3 11h-12.6zM8 6.5v3.2M8 11.6v0.01",
        "play": "M5 3.2v9.6l7.6 -4.8z",
        "pause": "M5.5 3.5v9M10.5 3.5v9",
        "stop": "M4 4h8v8h-8z",
        "skip": "M3.5 3.5l6 4.5l-6 4.5zM12.5 3.5v9",
        "chevron-down": "M4.5 6.5l3.5 3.5l3.5 -3.5",
        "chevron-right": "M6.5 4.5l3.5 3.5l-3.5 3.5",
        "chevron-left": "M9.5 4.5l-3.5 3.5l3.5 3.5",
        "menu": "M2.5 4h11M2.5 8h11M2.5 12h11",
        "plus": "M8 3v10M3 8h10",
        "close": "M4 4l8 8M12 4l-8 8",
        "minimize": "M3.5 8.5h9",
        "maximize": "M3.5 3.5h9v9h-9z",
        "restore": "M3.5 5.5h7v7h-7zM5.5 5.5v-2h7v7h-2",
        "check": "M3 8.5l3 3l7 -7",
        "search": "M2.5 7a4.5 4.5 0 1 0 9 0a4.5 4.5 0 1 0 -9 0M10.3 10.3l3.2 3.2",
        "edit": "M10.5 2.5l3 3l-8 8h-3v-3zM9 4l3 3",
        "trash": "M2.5 4.5h11M6 4.5v-2h4v2M4 4.5l0.7 9h6.6l0.7 -9M6.5 7v4M9.5 7v4",
        "download": "M8 2.5v8M4.5 7l3.5 3.5l3.5 -3.5M2.5 13.5h11",
        "upload": "M8 10.5v-8M4.5 6l3.5 -3.5l3.5 3.5M2.5 13.5h11",
        "copy": "M5.5 5.5h8v8h-8zM2.5 10.5v-8h8",
        "refresh": "M13 8a5 5 0 1 1 -1.6 -3.7M13.5 2.5v3h-3",
        "undo": "M3 7h7a3.5 3.5 0 0 1 0 7h-3M5.5 4.5l-2.5 2.5l2.5 2.5",
        "repeat": "M2.5 7.5v-1a2 2 0 0 1 2 -2h8.5M11 2.5l2 2l-2 2M13.5 8.5v1a2 2 0 0 1 -2 2h-8.5M5 13.5l-2 -2l2 -2",
        "bell": "M3.5 11.5h9l-1.3 -1.8v-3.2a3.2 3.2 0 0 0 -6.4 0v3.2zM6.5 13.5h3",
        "bell-off":"M4 11.5h8.5l-1.3 -1.8v-3.2a3.2 3.2 0 0 0 -5.2 -2.5M4.8 6.5v3.2l-1.3 1.8M6.5 13.5h3M2 2l12 12",
        "heart": "M8 13.5s-5.5 -3.2 -5.5 -7a3 3 0 0 1 5.5 -1.7a3 3 0 0 1 5.5 1.7c0 3.8 -5.5 7 -5.5 7z",
        "sparkle": "M8 1.5l1.6 4.9l4.9 1.6l-4.9 1.6l-1.6 4.9l-1.6 -4.9l-4.9 -1.6l4.9 -1.6z",
        "external": "M9 2.5h4.5v4.5M13.5 2.5l-6 6M11.5 9.5v4h-9v-9h4",
        "dot": "M5 8a3 3 0 1 0 6 0a3 3 0 1 0 -6 0",
        "save": "M2.5 2.5h9.5l1.5 1.5v9.5h-11zM5 2.5v4h6v-4zM5 9.5h6v4h-6z",
    })

    Shape {
        width: 16
        height: 16
        scale: icon.size / 16
        transformOrigin: Item.TopLeft
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: icon.filled ? "transparent" : icon.color
            strokeWidth: 1.5
            fillColor: icon.filled ? icon.color : "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg {
                path: icon.paths[icon.name] ?? ""
            }
        }
    }
    // artwork-end
}
