// SPDX-License-Identifier: GPL-3.0-or-later
#include "ThemeIcons.h"

#include <QColor>
#include <QFile>
#include <QIcon>
#include <QPainter>
#include <QRegularExpression>
#include <QStringList>
#include <QUrlQuery>

ThemeIcons::ThemeIcons(QObject *parent)
    : QObject(parent)
{
}

bool ThemeIcons::exists(const QString &name)
{
    return !name.isEmpty() && QIcon::hasThemeIcon(name);
}

QString ThemeIcons::fallbackThemeName()
{
    return QStringLiteral("sugo-fallback");
}

bool ThemeIcons::isTileIcon(const QString &name)
{
    return name.startsWith(QStringLiteral("qrc:/icons/sugo-")) && name.endsWith(QStringLiteral(".svg"));
}

namespace {

// `color` as SVG stop-color values spell it: "#rrggbb".
QString hexColor(const QColor &color)
{
    return color.name(QColor::HexRgb);
}

// The tile SVG keeps its dark tile (the gradient called "tile"); every other gradient of the
// file is repainted in shades of `color`, spread from lighter to darker along its stops, so
// the drawn part keeps some depth instead of turning into one flat colour.
QString repaintGlyphGradients(const QString &svg, const QColor &color)
{
    static const QRegularExpression gradient(
        QStringLiteral("<linearGradient\\b[^>]*\\bid=\"(?!tile\")([^\"]*)\"[^>]*>(.*?)</linearGradient>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression stop(
        QStringLiteral("(stop-color\\s*=\\s*\")#[0-9a-fA-F]{3,8}(\")"));

    QString result;
    qsizetype cursor = 0;
    const auto append = [&](qsizetype from, qsizetype to) {
        result += QStringView(svg).sliced(from, to - from);
    };

    QRegularExpressionMatchIterator gradients = gradient.globalMatch(svg);
    while (gradients.hasNext()) {
        const QRegularExpressionMatch match = gradients.next();
        const QString stops = match.captured(2);
        // The stops are counted first, so the shades spread over all of them.
        int stopCount = 0;
        for (const QRegularExpressionMatch &one : stop.globalMatch(stops)) {
            Q_UNUSED(one);
            ++stopCount;
        }

        QString repainted;
        qsizetype stopCursor = 0;
        int index = 0;
        for (const QRegularExpressionMatch &one : stop.globalMatch(stops)) {
            const qreal part = stopCount > 1 ? qreal(index) / qreal(stopCount - 1) : 0.0;
            repainted += QStringView(stops).sliced(stopCursor, one.capturedStart() - stopCursor);
            repainted += one.captured(1);
            repainted += hexColor(color.lighter(int(140 - 55 * part)));
            repainted += one.captured(2);
            stopCursor = one.capturedEnd();
            ++index;
        }
        repainted += QStringView(stops).sliced(stopCursor);

        append(cursor, match.capturedStart());
        append(match.capturedStart(), match.capturedStart(2));
        result += repainted;
        append(match.capturedEnd(2), match.capturedEnd());
        cursor = match.capturedEnd();
    }
    append(cursor, svg.size());
    return result;
}

} // namespace

QString ThemeIcons::tintedTileIcon(const QString &name, const QColor &color)
{
    if (!isTileIcon(name) || !color.isValid() || color.alpha() == 0) {
        return name;
    }
    // "qrc:/…" is a QUrl spelling; QFile knows the same resource as ":/…".
    QFile file(QStringLiteral(":") + name.mid(4));
    if (!file.open(QIODevice::ReadOnly)) {
        return name;
    }
    const QString rewritten = repaintGlyphGradients(QString::fromUtf8(file.readAll()), color);
    return QStringLiteral("data:image/svg+xml;base64,")
           + QString::fromLatin1(rewritten.toUtf8().toBase64());
}

void ThemeIcons::installFallbackTheme()
{
    // The theme is looked up in the theme search paths; ":/icons" is among Qt's defaults, but
    // the platform or a test may have replaced them.
    const QString resources = QStringLiteral(":/icons");
    QStringList themePaths = QIcon::themeSearchPaths();
    if (!themePaths.contains(resources)) {
        themePaths.append(resources);
        QIcon::setThemeSearchPaths(themePaths);
    }
    QIcon::setFallbackThemeName(fallbackThemeName());
    QStringList plainPaths = QIcon::fallbackSearchPaths();
    if (!plainPaths.contains(resources)) {
        plainPaths.append(resources);
        QIcon::setFallbackSearchPaths(plainPaths);
    }
}

ThemeIconProvider::ThemeIconProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage ThemeIconProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const qsizetype queryAt = id.indexOf(QLatin1Char('?'));
    const QString name = id.left(queryAt);
    QColor color;
    if (queryAt >= 0) {
        const QString hex = QUrlQuery(id.mid(queryAt + 1)).queryItemValue(QStringLiteral("color"));
        if (!hex.isEmpty()) {
            color = QColor::fromString(QLatin1Char('#') + hex);
        }
    }

    const QSize wanted(requestedSize.width() > 0 ? requestedSize.width() : (requestedSize.height() > 0 ? requestedSize.height() : 32),
                       requestedSize.height() > 0 ? requestedSize.height() : (requestedSize.width() > 0 ? requestedSize.width() : 32));
    QImage image;
    if (ThemeIcons::exists(name)) {
        // The pixmap may come out smaller than asked (fixed-size icons): centre it.
        const QPixmap pixmap = QIcon::fromTheme(name).pixmap(wanted, 1.0);
        image = QImage(wanted, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const QSize drawn = pixmap.size().scaled(wanted, Qt::KeepAspectRatio);
        painter.drawPixmap(QRect(QPoint((wanted.width() - drawn.width()) / 2, (wanted.height() - drawn.height()) / 2), drawn), pixmap);
        if (color.isValid()) {
            // Every visible pixel in `color`, keeping the icon's own transparency.
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(image.rect(), color);
        }
    }
    if (size) {
        *size = image.size();
    }
    return image;
}
