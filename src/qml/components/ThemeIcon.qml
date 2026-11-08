// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import stream.sugo.app

/**
 * An icon of the desktop's icon theme (`name`), or an image file when `name` is a URL
 * ("qrc:/…", "file:/…"). With `color` set, a theme icon is painted in that one colour (for
 * symbolic icons), and a Sugo tile icon keeps its dark tile but gets its drawn part in that
 * colour. `valid` is false when there is nothing to show: the theme has no such icon.
 */
Item {
    id: icon

    property string name
    property real size: Theme.icon
    /// Leave it transparent to keep the icon's own colours.
    property color color: "transparent"

    readonly property bool isUrl: name.indexOf(":/") > 0
    readonly property bool isTile: isUrl && ThemeIcons.isTileIcon(name)
    readonly property bool valid: isUrl || ThemeIcons.exists(name)

    implicitWidth: size
    implicitHeight: size

    Image {
        anchors.fill: parent
        sourceSize.width: icon.width
        sourceSize.height: icon.height
        fillMode: Image.PreserveAspectFit
        smooth: true
        source: {
            if (!icon.valid || icon.name.length === 0) {
                return "";
            }
            if (icon.isTile && icon.color.a > 0) {
                return ThemeIcons.tintedTileIcon(icon.name, icon.color);
            }
            if (icon.isUrl) {
                return icon.name;
            }
            // "#rrggbb" or "#aarrggbb" without the "#" (a "#" would start the URL's fragment).
            const tint = icon.color.a > 0 ? "?color=" + String(icon.color).substring(1) : "";
            return "image://icon/" + icon.name + tint;
        }
    }
}
