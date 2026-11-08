// SPDX-License-Identifier: GPL-3.0-or-later
#include <QDir>
#include <QIcon>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

#include "ThemeIcons.h"

/// The icons of the desktop's theme, as QML gets them through image://icon/.
class ThemeIconsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // A one-icon theme: a 16 px PNG, transparent on the left half, green on the right.
        QVERIFY(m_dir.isValid());
        QDir root(m_dir.path());
        QVERIFY(root.mkpath(QStringLiteral("sugotest/16x16/apps")));
        QFile index(root.filePath(QStringLiteral("sugotest/index.theme")));
        QVERIFY(index.open(QIODevice::WriteOnly));
        index.write("[Icon Theme]\nName=sugotest\nDirectories=16x16/apps\n\n[16x16/apps]\nSize=16\nType=Fixed\n");
        index.close();
        QImage half(16, 16, QImage::Format_ARGB32);
        half.fill(Qt::transparent);
        for (int y = 0; y < 16; ++y) {
            for (int x = 8; x < 16; ++x) {
                half.setPixelColor(x, y, Qt::green);
            }
        }
        QVERIFY(half.save(root.filePath(QStringLiteral("sugotest/16x16/apps/sugo-half.png"))));
        m_platformTheme = QIcon::themeName();
        m_platformSearchPaths = QIcon::themeSearchPaths();
        QIcon::setThemeSearchPaths({m_dir.path()});
        QIcon::setThemeName(QStringLiteral("sugotest"));
        QIcon::setFallbackThemeName(QString());
    }

    void exists()
    {
        QVERIFY(ThemeIcons::exists(QStringLiteral("sugo-half")));
        QVERIFY(!ThemeIcons::exists(QStringLiteral("sugo-no-such-icon")));
        QVERIFY(!ThemeIcons::exists(QString()));
    }

    void ownColours()
    {
        ThemeIconProvider provider;
        QSize size;
        const QImage image = provider.requestImage(QStringLiteral("sugo-half"), &size, QSize(32, 32));
        QCOMPARE(size, QSize(32, 32));
        QCOMPARE(image.pixelColor(4, 16).alpha(), 0);
        QCOMPARE(image.pixelColor(24, 16), QColor(Qt::green));
    }

    void tinted()
    {
        // Symbolic use: every visible pixel in the colour asked for, the transparent ones left alone.
        ThemeIconProvider provider;
        QSize size;
        const QImage image = provider.requestImage(QStringLiteral("sugo-half?color=ff0000"), &size, QSize(16, 16));
        QCOMPARE(size, QSize(16, 16));
        QCOMPARE(image.pixelColor(2, 8).alpha(), 0);
        QCOMPARE(image.pixelColor(12, 8), QColor(Qt::red));

        // "aarrggbb", as QML writes a colour that is not opaque.
        const QImage faint = provider.requestImage(QStringLiteral("sugo-half?color=80ffffff"), &size, QSize(16, 16));
        QCOMPARE(faint.pixelColor(12, 8).alpha(), 0x80);
    }

    void defaultSizeAndUnknown()
    {
        ThemeIconProvider provider;
        QSize size;
        QCOMPARE(provider.requestImage(QStringLiteral("sugo-half"), &size, QSize()).size(), QSize(32, 32));
        QVERIFY(provider.requestImage(QStringLiteral("sugo-no-such-icon"), &size, QSize(16, 16)).isNull());
        QVERIFY(size.isEmpty());
    }

    void tileIcons()
    {
        // Sugo's own tiles, as PresetModel::iconSource() hands them over; other names are not tiles.
        QVERIFY(ThemeIcons::isTileIcon(QStringLiteral("qrc:/icons/sugo-focus.svg")));
        QVERIFY(!ThemeIcons::isTileIcon(QStringLiteral("qrc:/icons/sugo-focus.png")));
        QVERIFY(!ThemeIcons::isTileIcon(QStringLiteral("chronometer")));
        QVERIFY(!ThemeIcons::isTileIcon(QString()));

        // Without a colour, or a transparent or invalid one, the tile goes back as it is.
        QCOMPARE(ThemeIcons::tintedTileIcon(QStringLiteral("qrc:/icons/sugo-focus.svg"), QColor()),
                 QStringLiteral("qrc:/icons/sugo-focus.svg"));
        QCOMPARE(ThemeIcons::tintedTileIcon(QStringLiteral("qrc:/icons/sugo-focus.svg"), QColor(0, 0, 0, 0)),
                 QStringLiteral("qrc:/icons/sugo-focus.svg"));
        QCOMPARE(ThemeIcons::tintedTileIcon(QStringLiteral("chronometer"), QColor(Qt::red)),
                 QStringLiteral("chronometer"));
    }

    void bundledFallbackTheme()
    {
        // A theme without the menus' icons, as macOS has none at all: the bundled Breeze subset
        // fills in once it is the fallback theme.
        QVERIFY(!ThemeIcons::exists(QStringLiteral("office-chart-bar")));
        ThemeIcons::installFallbackTheme();
        QCOMPARE(QIcon::fallbackThemeName(), ThemeIcons::fallbackThemeName());
        QVERIFY(QIcon::themeSearchPaths().contains(QStringLiteral(":/icons")));
        // Twice changes nothing.
        ThemeIcons::installFallbackTheme();
        QCOMPARE(QIcon::themeSearchPaths().count(QStringLiteral(":/icons")), 1);

        const QStringList names = {
            QStringLiteral("media-playback-start"), QStringLiteral("media-playback-pause"),
            QStringLiteral("media-playback-stop"), QStringLiteral("media-skip-forward"),
            QStringLiteral("chronometer"), QStringLiteral("office-chart-bar"),
            QStringLiteral("configure"), QStringLiteral("application-exit"),
        };
        for (const QString &name : names) {
            QVERIFY2(ThemeIcons::exists(name), qPrintable(name));
            QVERIFY2(!QIcon::fromTheme(name).pixmap(22, 22).isNull(), qPrintable(name));
        }
        // The theme's own icons still come first.
        QVERIFY(ThemeIcons::exists(QStringLiteral("sugo-half")));
        // Drawn: something visible in the middle of the play triangle.
        ThemeIconProvider provider;
        QSize size;
        const QImage play = provider.requestImage(QStringLiteral("media-playback-start"), &size, QSize(16, 16));
        QVERIFY(play.pixelColor(6, 8).alpha() > 0);
    }

    void bundledFallbackUnderThePlatformTheme()
    {
        // As the program runs: the platform's own theme (none on macOS, where Qt's Apple icons
        // answer first) with the bundled one behind it.
        QIcon::setThemeSearchPaths(m_platformSearchPaths);
        QIcon::setThemeName(m_platformTheme);
        ThemeIcons::installFallbackTheme();
        for (const QString &name : {QStringLiteral("chronometer"), QStringLiteral("office-chart-bar"),
                                    QStringLiteral("configure"), QStringLiteral("media-playback-start")}) {
            QVERIFY2(ThemeIcons::exists(name), qPrintable(name));
            QVERIFY2(!QIcon::fromTheme(name).pixmap(16, 16).isNull(), qPrintable(name));
        }
    }

private:
    QTemporaryDir m_dir;
    QString m_platformTheme;
    QStringList m_platformSearchPaths;
};

QTEST_MAIN(ThemeIconsTest)
#include "tst_themeicons.moc"
