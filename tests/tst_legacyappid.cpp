// SPDX-License-Identifier: GPL-3.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include "LegacyAppId.h"

namespace
{
const QString kNewId = QStringLiteral("stream.sugo.app");

void writeFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
} // namespace

class TestLegacyAppId : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void oldLoginEntryIsRenamed()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("io.github.mineraleyt.sugo.desktop")), "old");
        QVERIFY(LegacyAppId::moveLoginEntry(dir.path(), QStringLiteral(".desktop"), kNewId));
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("io.github.mineraleyt.sugo.desktop"))));
        QCOMPARE(readFile(dir.filePath(QStringLiteral("stream.sugo.app.desktop"))), QByteArray("old"));
    }

    void newLoginEntryWinsOverTheOldOne()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("io.github.mineraleyt.sugo.plist")), "old");
        writeFile(dir.filePath(QStringLiteral("stream.sugo.app.plist")), "new");
        QVERIFY(LegacyAppId::moveLoginEntry(dir.path(), QStringLiteral(".plist"), kNewId));
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("io.github.mineraleyt.sugo.plist"))));
        QCOMPARE(readFile(dir.filePath(QStringLiteral("stream.sugo.app.plist"))), QByteArray("new"));
    }

    void noOldLoginEntryChangesNothing()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("stream.sugo.app.desktop")), "new");
        QVERIFY(!LegacyAppId::moveLoginEntry(dir.path(), QStringLiteral(".desktop"), kNewId));
        QVERIFY(!LegacyAppId::moveLoginEntry(dir.filePath(QStringLiteral("missing")), QStringLiteral(".desktop"), kNewId));
        QCOMPARE(readFile(dir.filePath(QStringLiteral("stream.sugo.app.desktop"))), QByteArray("new"));
    }

    void flatpakDataIsCopiedOnce()
    {
        QTemporaryDir home;
        const QString old = home.filePath(QStringLiteral("old"));
        const QString data = home.filePath(QStringLiteral("new/data"));
        const QString config = home.filePath(QStringLiteral("new/config"));
        writeFile(old + QStringLiteral("/data/sugo/sugo.db"), "db");
        writeFile(old + QStringLiteral("/data/sugo/sugo.db-wal"), "wal");
        writeFile(old + QStringLiteral("/data/sugo/sounds/bell.ogg"), "bell");
        writeFile(old + QStringLiteral("/config/sugo/sugo.conf"), "conf");
        QVERIFY(QFile::setPermissions(old + QStringLiteral("/config/sugo/sugo.conf"), QFile::ReadOwner | QFile::WriteOwner));

        QVERIFY(LegacyAppId::importFlatpakData(old, data, config));
        QCOMPARE(readFile(data + QStringLiteral("/sugo/sugo.db")), QByteArray("db"));
        QCOMPARE(readFile(data + QStringLiteral("/sugo/sugo.db-wal")), QByteArray("wal"));
        QCOMPARE(readFile(data + QStringLiteral("/sugo/sounds/bell.ogg")), QByteArray("bell"));
        QCOMPARE(readFile(config + QStringLiteral("/sugo/sugo.conf")), QByteArray("conf"));
        QCOMPARE(QFile::permissions(config + QStringLiteral("/sugo/sugo.conf")) & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions());
        QVERIFY(QFileInfo::exists(old + QStringLiteral("/data/sugo/sugo.db"))); // left as it was

        // Ours exist now: a later start copies nothing, and never overwrites.
        writeFile(old + QStringLiteral("/data/sugo/sugo.db"), "newer old db");
        QVERIFY(!LegacyAppId::importFlatpakData(old, data, config));
        QCOMPARE(readFile(data + QStringLiteral("/sugo/sugo.db")), QByteArray("db"));
    }

    void flatpakDataOfOursIsKept()
    {
        QTemporaryDir home;
        const QString old = home.filePath(QStringLiteral("old"));
        const QString data = home.filePath(QStringLiteral("new/data"));
        const QString config = home.filePath(QStringLiteral("new/config"));
        writeFile(old + QStringLiteral("/data/sugo/sugo.db"), "old db");
        writeFile(old + QStringLiteral("/config/sugo/sugo.conf"), "old conf");
        writeFile(data + QStringLiteral("/sugo/sugo.db"), "our db");

        // Our database stays; the settings, which we lack, come over.
        QVERIFY(LegacyAppId::importFlatpakData(old, data, config));
        QCOMPARE(readFile(data + QStringLiteral("/sugo/sugo.db")), QByteArray("our db"));
        QCOMPARE(readFile(config + QStringLiteral("/sugo/sugo.conf")), QByteArray("old conf"));
    }

    void flatpakLoginStateIsNotCopied()
    {
        QTemporaryDir home;
        const QString old = home.filePath(QStringLiteral("old"));
        const QString data = home.filePath(QStringLiteral("new/data"));
        const QString config = home.filePath(QStringLiteral("new/config"));
        writeFile(old + QStringLiteral("/config/sugo/sugo.conf"), "conf");
        writeFile(old + QStringLiteral("/config/sugo/portal-autostart"), "autostart=true\n");

        // The Background portal was asked for the old app ID, never for ours: start at login is
        // off until the user turns it on again, which asks the portal for this app.
        QVERIFY(LegacyAppId::importFlatpakData(old, data, config));
        QCOMPARE(readFile(config + QStringLiteral("/sugo/sugo.conf")), QByteArray("conf"));
        QVERIFY(!QFileInfo::exists(config + QStringLiteral("/sugo/portal-autostart")));
        QVERIFY(QFileInfo::exists(old + QStringLiteral("/config/sugo/portal-autostart"))); // left as it was
    }

    void noOldFlatpakCopiesNothing()
    {
        QTemporaryDir home;
        QVERIFY(!LegacyAppId::importFlatpakData(home.filePath(QStringLiteral("missing")), home.filePath(QStringLiteral("data")),
                                                home.filePath(QStringLiteral("config"))));
        QVERIFY(!QFileInfo::exists(home.filePath(QStringLiteral("data"))));
    }
};

QTEST_GUILESS_MAIN(TestLegacyAppId)
#include "tst_legacyappid.moc"
