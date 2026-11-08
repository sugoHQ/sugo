// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

/**
 * The fixed colours of the running-timer tray icon: the application icon's dark tile, whose
 * border fills in the badge's phase colours around white minutes, readable on any panel
 * whatever the colour scheme.
 *
 * The one place the tile's fixed colours are defined: TrayController paints the real icon with
 * them and its pictures in the interface (TrayBadge.qml) read them from QML (TrayIconStyle), so
 * the two cannot drift apart. The phase colours are not fixed: they come from the accent colour
 * setting, whose shades are defined here too (the one place Theme.qml reads as well).
 */
namespace TrayIconColors
{
inline QColor rest()
{
    return QColor(0x22, 0xc5, 0x5e);
}
inline QColor dial()
{
    return QColor(20, 23, 28, 240);
}
inline QColor digits()
{
    return QColor(Qt::white);
}
/// The tile's faint outline, so it stands out on a dark panel too.
inline QColor edge()
{
    return QColor(255, 255, 255, 30);
}
/// The leaf at the end of the spiral, as in the application icon.
inline QColor leaf()
{
    return QColor(0x3d, 0xaa, 0x4a);
}
/// Opacity (0..255) of the ring's track, in the phase colour.
constexpr int TrackAlpha = 55;

inline QColor track(const QColor &phase)
{
    QColor c = phase;
    c.setAlpha(TrackAlpha);
    return c;
}
} // namespace TrayIconColors

class TrayIconStyle : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /// The badge's colour of work: the chosen accent's shade for the dark tile, so the menu bar
    /// follows the accent colour setting (kept in step by main.cpp).
    Q_PROPERTY(QColor workColor READ workColor NOTIFY accentColorChanged FINAL)
    /// The badge's colour of breaks: the break green, or blue when the accent is itself a green
    /// (as Theme.breakAccent in the interface), so work and break never look alike.
    Q_PROPERTY(QColor breakColor READ breakColor NOTIFY accentColorChanged FINAL)
    Q_PROPERTY(QColor dialColor READ dialColor CONSTANT FINAL)
    Q_PROPERTY(QColor digitColor READ digitColor CONSTANT FINAL)
    Q_PROPERTY(QColor edgeColor READ edgeColor CONSTANT FINAL)
    Q_PROPERTY(QColor leafColor READ leafColor CONSTANT FINAL)
    /// The track's opacity, 0..1.
    Q_PROPERTY(qreal trackOpacity READ trackOpacity CONSTANT FINAL)

public:
    using QObject::QObject;

    /// The shades of an accent colour: for the dark theme, for the light theme, and the text and
    /// icons drawn on each. One place, read by Theme.qml (the interface) and by the badge's
    /// colours, so they cannot drift apart. An unknown id falls back to red.
    struct AccentShade
    {
        QColor dark, light, textDark, textLight;
    };
    static AccentShade accentShades(const QString &id)
    {
        static const QHash<QString, AccentShade> shades{
            {QStringLiteral("red"), {QColor(0xff, 0x4d, 0x4d), QColor(0xe5, 0x38, 0x3b), QColor(0xff, 0xff, 0xff), QColor(0xff, 0xff, 0xff)}},
            {QStringLiteral("orange"), {QColor(0xfb, 0x92, 0x3c), QColor(0xea, 0x58, 0x0c), QColor(0x1a, 0x0d, 0x04), QColor(0xff, 0xff, 0xff)}},
            {QStringLiteral("yellow"), {QColor(0xfa, 0xcc, 0x15), QColor(0xca, 0x8a, 0x04), QColor(0x1a, 0x14, 0x06), QColor(0x1a, 0x14, 0x06)}},
            {QStringLiteral("lime"), {QColor(0xa3, 0xe6, 0x35), QColor(0x65, 0xa3, 0x0d), QColor(0x0f, 0x1a, 0x04), QColor(0xff, 0xff, 0xff)}},
            {QStringLiteral("green"), {QColor(0x4a, 0xde, 0x80), QColor(0x16, 0xa3, 0x4a), QColor(0x04, 0x19, 0x0c), QColor(0xff, 0xff, 0xff)}},
            {QStringLiteral("teal"), {QColor(0x2d, 0xd4, 0xbf), QColor(0x0d, 0x94, 0x88), QColor(0x04, 0x1a, 0x17), QColor(0xff, 0xff, 0xff)}},
            {QStringLiteral("cyan"), {QColor(0x22, 0xd3, 0xee), QColor(0x08, 0x91, 0xb2), QColor(0x04, 0x17, 0x1a), QColor(0xff, 0xff, 0xff)}},
            {QStringLiteral("blue"), {QColor(0x60, 0xa5, 0xfa), QColor(0x25, 0x63, 0xeb), QColor(0x04, 0x10, 0x1f), QColor(0xff, 0xff, 0xff)}},
            {QStringLiteral("violet"), {QColor(0xa7, 0x8b, 0xfa), QColor(0x7c, 0x3a, 0xed), QColor(0x14, 0x07, 0x2e), QColor(0xff, 0xff, 0xff)}},
            {QStringLiteral("pink"), {QColor(0xf4, 0x72, 0xb6), QColor(0xdb, 0x27, 0x77), QColor(0x1f, 0x05, 0x12), QColor(0xff, 0xff, 0xff)}},
        };
        return shades.value(id, shades.value(QStringLiteral("red")));
    }
    /// A shade of an accent colour for QML: the id, and whether the theme is dark.
    Q_INVOKABLE static QColor accentShadeColor(const QString &id, bool darkTheme)
    {
        const AccentShade shades = accentShades(id);
        return darkTheme ? shades.dark : shades.light;
    }
    /// The colour of text and icons drawn on an accent colour, for QML.
    Q_INVOKABLE static QColor accentTextOnColor(const QString &id, bool darkTheme)
    {
        const AccentShade shades = accentShades(id);
        return darkTheme ? shades.textDark : shades.textLight;
    }

    /// Work's badge colour for the accent id (the tile is dark on any panel, so the dark shade).
    static QColor workBadgeColor(const QString &accentId)
    {
        return accentShades(accentId).dark;
    }
    /// Breaks' badge colour for the accent id: the break green, or blue on a green accent.
    static QColor breakBadgeColor(const QString &accentId)
    {
        static const QStringList greens{QStringLiteral("lime"), QStringLiteral("green"), QStringLiteral("teal")};
        return greens.contains(accentId) ? QColor(0x60, 0xa5, 0xfa) : TrayIconColors::rest();
    }

    /// The accent the badge is painted with, kept in step with the setting (main.cpp).
    void setAccentColor(const QString &id)
    {
        if (m_accentColor == id) {
            return;
        }
        m_accentColor = id;
        Q_EMIT accentColorChanged();
    }

    QColor workColor() const { return workBadgeColor(m_accentColor); }
    QColor breakColor() const { return breakBadgeColor(m_accentColor); }
    QColor dialColor() const { return TrayIconColors::dial(); }
    QColor digitColor() const { return TrayIconColors::digits(); }
    QColor edgeColor() const { return TrayIconColors::edge(); }
    QColor leafColor() const { return TrayIconColors::leaf(); }
    qreal trackOpacity() const { return TrayIconColors::TrackAlpha / 255.0; }

Q_SIGNALS:
    /// The accent colour setting changed (see setAccentColor).
    void accentColorChanged();

private:
    QString m_accentColor = QStringLiteral("red");
};
