// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QQuickImageProvider>
#include <QtQml/qqmlregistration.h>

/**
 * Icons of the desktop's icon theme, for QML. ThemeIconProvider serves them as
 * `image://icon/<name>`, or `image://icon/<name>?color=<hex>` to paint the icon in one colour
 * (symbolic icons, following the interface's text colour); `<hex>` is "rrggbb" or "aarrggbb",
 * without the "#". ThemeIcons tells QML whether the theme has an icon, so it can fall back to
 * its own drawing instead of asking for one that is not there.
 *
 * src/qml/components/ThemeIcon.qml puts the two together.
 */
class ThemeIcons : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit ThemeIcons(QObject *parent = nullptr);

    /// Whether the icon theme (or its fallbacks) has an icon called `name`.
    Q_INVOKABLE static bool exists(const QString &name);

    /// Whether `name` is one of Sugo's own tile icons (`qrc:/icons/…svg`), whose drawn part
    /// can be repainted in the accent colour while the tile keeps its own look.
    Q_INVOKABLE static bool isTileIcon(const QString &name);

    /// The tile icon `name` (`qrc:/icons/…svg`) with every gradient of its drawn part
    /// repainted in shades of `color`, as a `data:` URL for QML's Image. The dark tile
    /// gradient keeps its own colours; `name` without a usable file comes back unchanged.
    Q_INVOKABLE static QString tintedTileIcon(const QString &name, const QColor &color);

    /// The name of the icon theme bundled in the program (`:/icons/sugo-fallback`): the Breeze
    /// icons Sugo's menus use, for desktops without a freedesktop icon theme (macOS).
    static QString fallbackThemeName();
    /// Makes the bundled theme the fallback of the icon theme in use, and the bundled icons
    /// (`:/icons`) the place for names no theme has. main.cpp calls it on macOS before the
    /// first icon is looked up.
    static void installFallbackTheme();
};

class ThemeIconProvider : public QQuickImageProvider
{
public:
    ThemeIconProvider();

    /// The id is "<name>" or "<name>?color=<hex>". The icon is drawn at `requestedSize`
    /// (32 × 32 when none is given); an unknown name gives a null image.
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
