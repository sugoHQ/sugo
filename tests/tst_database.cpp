// SPDX-License-Identifier: GPL-3.0-or-later
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <memory>

#include "Database.h"
#include "PresetRepository.h"
#include "Sugo100Database.h"

namespace
{
QVariant scalar(const QString &connection, const QString &sql)
{
    QSqlQuery query(QSqlDatabase::database(connection, false));
    return query.exec(sql) && query.next() ? query.value(0) : QVariant();
}

QStringList tableNames(const QString &connection)
{
    QSqlQuery query(QSqlDatabase::database(connection, false));
    query.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'"));
    QStringList names;
    while (query.next()) {
        names.append(query.value(0).toString());
    }
    names.sort();
    return names;
}

/// Creates a SQLite file with arbitrary content, as another program would have.
void createForeignDatabase(const QString &path, const QString &schemaSql, int userVersion, int applicationId = 0)
{
    const QString name = QStringLiteral("foreign-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery query(db);
        // The SQLite driver runs one statement per exec().
        for (const QString &statement : schemaSql.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            QVERIFY(query.exec(statement));
        }
        QVERIFY(query.exec(QStringLiteral("INSERT INTO notes (text) VALUES ('precious')")));
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version = %1").arg(userVersion)));
        QVERIFY(query.exec(QStringLiteral("PRAGMA application_id = %1").arg(applicationId)));
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
}

QString foreignNotes(const QString &path)
{
    const QString name = QStringLiteral("check-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString text;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(QStringLiteral("SELECT text FROM notes")) && query.next()) {
                text = query.value(0).toString();
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    return text;
}

QStringList movedAsideFiles(const QDir &dir)
{
    return dir.entryList({QStringLiteral("*.foreign-*")}, QDir::Files);
}

QByteArray fileContents(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/// What Sugo 1.0.1 left: the 1.0.0 database moved aside (with its write-ahead log, if it had one).
QString setUpMovedAsideSugo100(const QDir &dir, const QString &time = QStringLiteral("20261104-232400"),
                               Sugo100Stamp stamp = Sugo100Stamp::InFile)
{
    const QString aside = dir.filePath(QStringLiteral("sugo.db.foreign-") + time);
    createSugo100Database(aside, stamp);
    return aside;
}

/// Checks the timers of the 1.0.0 file as today's PresetRepository reads them.
void verifySugo100Timers(const QString &connection)
{
    bool ok = false;
    const QList<TimerPreset> presets = PresetRepository(connection).loadAll(&ok);
    QVERIFY(ok);
    QCOMPARE(presets.size(), 2);
    QCOMPARE(presets.at(0).uuid, QStringLiteral("default"));
    QVERIFY(presets.at(0).builtin);
    QCOMPARE(presets.at(0).options.value(QStringLiteral("iconName")).toString(), QStringLiteral("sugo-timer"));
    QCOMPARE(presets.at(0).options.value(QStringLiteral("silenceWhileWorking")).toBool(), true);
    const TimerPreset &mine = presets.at(1);
    QCOMPARE(mine.uuid, QStringLiteral("mine"));
    QVERIFY(!mine.builtin);
    QCOMPARE(mine.name, QStringLiteral("Deep work"));
    QCOMPARE(mine.workSeconds, 3000);
    QCOMPARE(mine.cyclesBeforeLong, 3);
    QCOMPARE(mine.createdAt, 1761876000000LL);
    QCOMPARE(mine.options.size(), 4);
    QCOMPARE(mine.options.value(QStringLiteral("iconName")).toString(), QStringLiteral("sugo-focus"));
    QCOMPARE(mine.options.value(QStringLiteral("autoStartMode")).toInt(), 2);
    QCOMPARE(mine.options.value(QStringLiteral("autoStart")).toBool(), true);
    QCOMPARE(mine.options.value(QStringLiteral("soundOnEnd")).toBool(), false);
    QCOMPARE(PresetRepository(connection).currentUuid(), QStringLiteral("mine"));
}

void removeDatabaseFiles(const QString &path)
{
    for (const QString &suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        QFile::remove(path + suffix);
    }
}

/// Clears what the user made, as clearing the history and deleting the timers would.
void clearUserData(const QString &connection)
{
    QSqlQuery query(QSqlDatabase::database(connection, false));
    QVERIFY(query.exec(QStringLiteral("DELETE FROM session")));
    QVERIFY(query.exec(QStringLiteral("DELETE FROM preset WHERE is_builtin = 0")));
    QVERIFY(query.exec(QStringLiteral("DELETE FROM preset_tombstone")));
}

void addBuiltinTimer(const QString &connection)
{
    QSqlQuery query(QSqlDatabase::database(connection, false));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO preset (uuid, name, work_sec, short_break_sec, long_break_sec, is_builtin, created_at) "
        "VALUES ('default', 'Pomodoro', 1500, 300, 900, 1, 0)")));
}
} // namespace

class DatabaseTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void freshDatabaseIsMigratedToLatestSchema()
    {
        Database db(QStringLiteral(":memory:"));
        QVERIFY2(db.open(), qPrintable(db.lastError()));

        QCOMPARE(db.schemaVersion(), Database::latestSchemaVersion());
        QCOMPARE(tableNames(db.connectionName()),
                 (QStringList{QStringLiteral("meta"), QStringLiteral("preset"), QStringLiteral("preset_option"),
                              QStringLiteral("preset_tombstone"), QStringLiteral("session")}));
    }

    void foreignKeysAreEnforced()
    {
        Database db(QStringLiteral(":memory:"));
        QVERIFY(db.open());
        QCOMPARE(scalar(db.connectionName(), QStringLiteral("PRAGMA foreign_keys")).toInt(), 1);

        // An option must belong to an existing preset.
        QSqlQuery query(QSqlDatabase::database(db.connectionName(), false));
        QVERIFY(!query.exec(QStringLiteral("INSERT INTO preset_option (preset_id, key, value) VALUES (999, 'k', 'v')")));
    }

    void checkConstraintsRejectInvalidRows()
    {
        Database db(QStringLiteral(":memory:"));
        QVERIFY(db.open());
        QSqlQuery query(QSqlDatabase::database(db.connectionName(), false));
        QVERIFY(!query.exec(QStringLiteral(
            "INSERT INTO preset (uuid, name, work_sec, short_break_sec, long_break_sec, created_at) "
            "VALUES ('u', 'n', 0, 300, 900, 0)"))); // work_sec must be > 0
        QVERIFY(!query.exec(QStringLiteral(
            "INSERT INTO session (kind, started_at, ended_at, duration_sec, planned_sec, completed, preset_name) "
            "VALUES ('nap', 0, 0, 0, 0, 1, 'x')"))); // unknown kind
    }

    void fileDatabaseUsesWalAndKeepsDataAcrossReopen()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("sub/dir/test.db")); // directory is created

        {
            Database db(path);
            QVERIFY2(db.open(), qPrintable(db.lastError()));
            QCOMPARE(scalar(db.connectionName(), QStringLiteral("PRAGMA journal_mode")).toString().toLower(),
                     QStringLiteral("wal"));
            QSqlQuery query(QSqlDatabase::database(db.connectionName(), false));
            QVERIFY(query.exec(QStringLiteral("INSERT INTO meta (key, value) VALUES ('k', 'v')")));
        }
        {
            Database db(path);
            QVERIFY2(db.open(), qPrintable(db.lastError()));
            QCOMPARE(db.schemaVersion(), Database::latestSchemaVersion());
            QCOMPARE(scalar(db.connectionName(), QStringLiteral("SELECT value FROM meta WHERE key = 'k'")).toString(),
                     QStringLiteral("v")); // migrations did not run twice or wipe data
        }
    }

    void databaseFilesAreForTheOwnerOnly()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("test.db"));
        // Qt reports the owner's bits twice (Owner and User): what matters is that the group and
        // others have nothing and the owner reads and writes.
        const QFile::Permissions others = QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup
            | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
        const auto ownerOnly = [&](const QString &file) {
            const QFile::Permissions p = QFileInfo(file).permissions();
            return (p & others) == QFile::Permissions() && (p & QFile::ReadOwner) && (p & QFile::WriteOwner);
        };

        Database db(path);
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QSqlQuery query(QSqlDatabase::database(db.connectionName(), false));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO meta (key, value) VALUES ('k', 'v')")));
        QVERIFY(ownerOnly(path));
        // The journal files exist while the database is open (WAL); they follow the file.
        for (const QString &suffix : {QStringLiteral("-wal"), QStringLiteral("-shm")}) {
            if (QFile::exists(path + suffix)) {
                QVERIFY2(ownerOnly(path + suffix), qPrintable(suffix));
            }
        }
    }

    void version1TimersAreMigratedForSync()
    {
        // A file as Sugo 1.0 left it: schema 1, created_at in seconds.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("v1.db"));
        const QString name = QStringLiteral("v1-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            raw.setDatabaseName(path);
            QVERIFY(raw.open());
            QSqlQuery query(raw);
            for (const char *statement : {
                     "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)",
                     "CREATE TABLE preset (id INTEGER PRIMARY KEY AUTOINCREMENT, uuid TEXT NOT NULL UNIQUE, "
                     "name TEXT NOT NULL, category TEXT NOT NULL DEFAULT '', work_sec INTEGER NOT NULL CHECK (work_sec > 0), "
                     "short_break_sec INTEGER NOT NULL CHECK (short_break_sec >= 0), "
                     "long_break_sec INTEGER NOT NULL CHECK (long_break_sec >= 0), "
                     "cycles_before_long INTEGER NOT NULL DEFAULT 4 CHECK (cycles_before_long >= 0), "
                     "is_builtin INTEGER NOT NULL DEFAULT 0, sort_order INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL)",
                     "CREATE TABLE preset_option (preset_id INTEGER NOT NULL REFERENCES preset(id) ON DELETE CASCADE, "
                     "key TEXT NOT NULL, value TEXT NOT NULL, PRIMARY KEY (preset_id, key))",
                     "CREATE TABLE session (id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL, started_at INTEGER NOT NULL, "
                     "ended_at INTEGER NOT NULL, duration_sec INTEGER NOT NULL, planned_sec INTEGER NOT NULL, "
                     "completed INTEGER NOT NULL, preset_id INTEGER REFERENCES preset(id) ON DELETE SET NULL, "
                     "preset_name TEXT NOT NULL, category TEXT NOT NULL DEFAULT '', note TEXT NOT NULL DEFAULT '')",
                     "INSERT INTO preset (uuid, name, work_sec, short_break_sec, long_break_sec, created_at) "
                     "VALUES ('old', 'Old timer', 1500, 300, 900, 1700000000)",
                 }) {
                QVERIFY2(query.exec(QString::fromLatin1(statement)), statement);
            }
            QVERIFY(query.exec(QStringLiteral("PRAGMA application_id = %1").arg(Database::kApplicationId)));
            QVERIFY(query.exec(QStringLiteral("PRAGMA user_version = 1")));
            raw.close();
        }
        QSqlDatabase::removeDatabase(name);

        Database db(path);
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QVERIFY(db.movedAsidePath().isEmpty());
        QCOMPARE(db.schemaVersion(), 2);
        QCOMPARE(scalar(db.connectionName(), QStringLiteral("SELECT created_at FROM preset WHERE uuid = 'old'")).toLongLong(),
                 1700000000000LL); // now milliseconds
        QCOMPARE(scalar(db.connectionName(), QStringLiteral("SELECT updated_at FROM preset WHERE uuid = 'old'")).toLongLong(), 0);
        QCOMPARE(scalar(db.connectionName(), QStringLiteral("SELECT updated_by FROM preset WHERE uuid = 'old'")).toString(), QString());
        QVERIFY(tableNames(db.connectionName()).contains(QStringLiteral("preset_tombstone")));

        QSqlQuery query(QSqlDatabase::database(db.connectionName(), false));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO preset_tombstone (uuid, deleted_at) VALUES ('gone', 5)")));
        QCOMPARE(scalar(db.connectionName(), QStringLiteral("SELECT deleted_by FROM preset_tombstone")).toString(), QString());
        QVERIFY(!query.exec(QStringLiteral("INSERT INTO preset_tombstone (uuid, deleted_at) VALUES ('gone', 6)"))); // one per uuid
    }

    void sugo100DatabaseIsKeptAndUpgraded_data()
    {
        QTest::addColumn<bool>("onlyInWal");
        QTest::newRow("folded into the file") << false;
        QTest::newRow("only in the write-ahead log") << true;
    }

    void sugo100DatabaseIsKeptAndUpgraded()
    {
        // Sugo 1.0.0 was released with another application id and its own numbering of
        // user_version: its version 2 (session.note) is today's version 1. Such a file is ours.
        QFETCH(bool, onlyInWal);
        const Sugo100Stamp stamp = onlyInWal ? Sugo100Stamp::OnlyInWal : Sugo100Stamp::InFile;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        createSugo100Database(path, stamp);

        {
            Database db(path);
            QVERIFY2(db.open(), qPrintable(db.lastError()));
            const QString c = db.connectionName();

            QVERIFY2(db.movedAsidePath().isEmpty(), qPrintable(db.movedAsidePath()));
            QVERIFY(movedAsideFiles(QDir(dir.path())).isEmpty());
            QCOMPARE(scalar(c, QStringLiteral("PRAGMA application_id")).toInt(), Database::kApplicationId);
            QCOMPARE(db.schemaVersion(), Database::latestSchemaVersion());

            // The timers, now with what Sugo Sync needs, and their times in milliseconds.
            QCOMPARE(scalar(c, QStringLiteral("SELECT count(*) FROM preset")).toInt(), 2);
            QCOMPARE(scalar(c, QStringLiteral("SELECT name FROM preset WHERE uuid = 'mine'")).toString(),
                     QStringLiteral("Deep work"));
            QCOMPARE(scalar(c, QStringLiteral("SELECT work_sec FROM preset WHERE uuid = 'mine'")).toInt(), 3000);
            QCOMPARE(scalar(c, QStringLiteral("SELECT created_at FROM preset WHERE uuid = 'mine'")).toLongLong(),
                     1761876000000LL);
            QCOMPARE(scalar(c, QStringLiteral("SELECT updated_at FROM preset WHERE uuid = 'mine'")).toLongLong(), 0);
            QCOMPARE(scalar(c, QStringLiteral("SELECT updated_by FROM preset WHERE uuid = 'mine'")).toString(), QString());
            QVERIFY(tableNames(c).contains(QStringLiteral("preset_tombstone")));
            verifySugo100Timers(c);

            // The history, untouched (session times were milliseconds already).
            QCOMPARE(scalar(c, QStringLiteral("SELECT count(*) FROM session")).toInt(), 1);
            QCOMPARE(scalar(c, QStringLiteral("SELECT note FROM session")).toString(), QStringLiteral("Chapter 3"));
            QCOMPARE(scalar(c, QStringLiteral("SELECT started_at FROM session")).toLongLong(), 1761876100000LL);
            QCOMPARE(scalar(c, QStringLiteral("SELECT preset_id FROM session")).toInt(), 2);
            QCOMPARE(scalar(c, QStringLiteral("SELECT value FROM meta WHERE key = 'presets_initialized'")).toString(),
                     QStringLiteral("1"));
            QCOMPARE(scalar(c, QStringLiteral("PRAGMA foreign_key_check")), QVariant());
        }
        {
            // Opening it again changes nothing more (milliseconds are not multiplied twice).
            Database db(path);
            QVERIFY2(db.open(), qPrintable(db.lastError()));
            QVERIFY(db.movedAsidePath().isEmpty());
            QCOMPARE(scalar(db.connectionName(), QStringLiteral("SELECT created_at FROM preset WHERE uuid = 'mine'")).toLongLong(),
                     1761876000000LL);
        }
    }

    void anotherProgramsFileWithThe100IdIsMovedAside()
    {
        // The id alone does not make a file Sugo 1.0.0's: it also needs 1.0.0's tables.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        createForeignDatabase(path, QStringLiteral("CREATE TABLE notes (text TEXT)"), 2, kSugo100ApplicationId);

        Database db(path);
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QVERIFY(!db.movedAsidePath().isEmpty());
        QCOMPARE(db.schemaVersion(), Database::latestSchemaVersion());
        QCOMPARE(foreignNotes(db.movedAsidePath()), QStringLiteral("precious"));
        QCOMPARE(sqliteHeaderApplicationId(db.movedAsidePath()), qint64(kSugo100ApplicationId));
    }

    void sugo100FileThatCannotBeUpgradedKeepsItsStamp()
    {
        // Named like 1.0.0's tables, but not them: migration 2 fails on it. The conversion and the
        // migrations are one transaction, so the file is left as it was, 1.0.0's id and version
        // included, rather than re-stamped as a current file that fails on every start.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        createForeignDatabase(path,
                              QStringLiteral("CREATE TABLE meta (key TEXT); CREATE TABLE preset (id INTEGER); "
                                             "CREATE TABLE preset_option (x); CREATE TABLE session (note TEXT); "
                                             "CREATE TABLE notes (text TEXT)"),
                              2, kSugo100ApplicationId);
        for (int run = 0; run < 2; ++run) {
            Database db(path);
            QVERIFY(!db.open());
            QVERIFY(db.movedAsidePath().isEmpty());
        }
        const QString name = QStringLiteral("check-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            raw.setDatabaseName(path);
            QVERIFY(raw.open());
            QCOMPARE(scalar(name, QStringLiteral("PRAGMA application_id")).toInt(), kSugo100ApplicationId);
            QCOMPARE(scalar(name, QStringLiteral("PRAGMA user_version")).toInt(), 2);
            QCOMPARE(scalar(name, QStringLiteral("SELECT count(*) FROM pragma_table_info('preset') WHERE name = 'updated_at'")).toInt(), 0);
            raw.close();
        }
        QSqlDatabase::removeDatabase(name);
        QCOMPARE(foreignNotes(path), QStringLiteral("precious"));
    }

    void databaseFromNewerVersionIsRefusedAndUntouched()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("future.db"));
        {
            Database db(path);
            QVERIFY(db.open());
            QSqlQuery query(QSqlDatabase::database(db.connectionName(), false));
            QVERIFY(query.exec(QStringLiteral("PRAGMA user_version = 99")));
        }
        {
            Database db(path);
            QVERIFY(!db.open());
            QVERIFY(db.lastError().contains(QStringLiteral("newer")));
        }
    }


    // --- Files that are not ours ---------------------------------------------

    void foreignDatabaseWithSameSchemaVersionIsMovedAsideNotMigrated()
    {
        // The real-world case: another program's file at our path, user_version already 1,
        // different tables. Trusting user_version would skip creating our tables.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        createForeignDatabase(path, QStringLiteral("CREATE TABLE notes (text TEXT)"), 1);

        Database db(path);
        QVERIFY2(db.open(), qPrintable(db.lastError()));

        // The new database is complete and stamped.
        QCOMPARE(db.schemaVersion(), Database::latestSchemaVersion());
        QCOMPARE(tableNames(db.connectionName()),
                 (QStringList{QStringLiteral("meta"), QStringLiteral("preset"), QStringLiteral("preset_option"),
                              QStringLiteral("preset_tombstone"), QStringLiteral("session")}));
        QCOMPARE(scalar(db.connectionName(), QStringLiteral("PRAGMA application_id")).toInt(), Database::kApplicationId);

        // The old file survives untouched under a new name.
        QVERIFY(!db.movedAsidePath().isEmpty());
        QVERIFY(QFile::exists(db.movedAsidePath()));
        QCOMPARE(foreignNotes(db.movedAsidePath()), QStringLiteral("precious"));
        QCOMPARE(movedAsideFiles(QDir(dir.path())).size(), 1);
    }

    void foreignDatabaseWithoutVersionIsAlsoMovedAside()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        createForeignDatabase(path, QStringLiteral("CREATE TABLE notes (text TEXT)"), 0);

        Database db(path);
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QVERIFY(!db.movedAsidePath().isEmpty());
        QCOMPARE(db.schemaVersion(), Database::latestSchemaVersion());
    }

    void foreignDatabaseWithOtherApplicationIdIsMovedAside()
    {
        // Even if it happens to contain tables named like ours.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        createForeignDatabase(path,
                              QStringLiteral("CREATE TABLE meta (key TEXT); CREATE TABLE preset (id INTEGER); "
                                             "CREATE TABLE preset_option (x); CREATE TABLE session (y); "
                                             "CREATE TABLE notes (text TEXT)"),
                              1, 0x12345678);
        Database db(path);
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QVERIFY(!db.movedAsidePath().isEmpty());
    }

    void freshDatabaseIsStampedAndNothingIsMovedAside()
    {
        QTemporaryDir dir;
        Database db(dir.filePath(QStringLiteral("sugo.db")));
        QVERIFY(db.open());
        QCOMPARE(scalar(db.connectionName(), QStringLiteral("PRAGMA application_id")).toInt(), Database::kApplicationId);
        QVERIFY(db.movedAsidePath().isEmpty());
    }

    void emptyExistingFileIsAdoptedNotMovedAside()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly)); // zero-byte file, e.g. created by `touch`
        file.close();

        Database db(path);
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QVERIFY(db.movedAsidePath().isEmpty());
        QCOMPARE(db.schemaVersion(), Database::latestSchemaVersion());
    }

    void unstampedDatabaseWithOurTablesIsMovedAside()
    {
        // Sugo stamps a file before creating any table, so tables without the stamp are not ours.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        {
            Database db(path);
            QVERIFY(db.open());
            QSqlQuery query(QSqlDatabase::database(db.connectionName(), false));
            QVERIFY(query.exec(QStringLiteral("PRAGMA application_id = 0")));
        }
        Database db(path);
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QVERIFY(!db.movedAsidePath().isEmpty());
    }

    void reopeningOwnDatabaseNeverMovesIt()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        for (int i = 0; i < 3; ++i) {
            Database db(path);
            QVERIFY(db.open());
            QVERIFY(db.movedAsidePath().isEmpty());
        }
        QVERIFY(movedAsideFiles(QDir(dir.path())).isEmpty());
    }

    void nonSqliteFileIsMovedAsideAsCorrupt()
    {
        // Must not silently run on :memory: at every launch.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("this is a text file, not a database\n");
        file.close();

        Database db(path);
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QVERIFY(!db.isTemporary());
        QVERIFY(db.movedAsideWasCorrupt());
        QVERIFY(db.movedAsidePath().contains(QStringLiteral(".corrupt-")));
        QCOMPARE(db.schemaVersion(), Database::latestSchemaVersion());

        QFile moved(db.movedAsidePath());
        QVERIFY(moved.open(QIODevice::ReadOnly));
        QCOMPARE(moved.readAll(), QByteArray("this is a text file, not a database\n"));
    }

    void aFailedMoveAsidePutsBackWhatItMoved()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        for (const QString &suffix : {QString(), QStringLiteral("-wal")}) {
            QFile file(path + suffix);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("not a database");
        }
        // The journal cannot follow: its destination is taken (for any second the move may use).
        const QDateTime now = QDateTime::currentDateTime();
        for (int s = 0; s < 5; ++s) {
            QFile taken(path + QStringLiteral(".corrupt-") + now.addSecs(s).toString(QStringLiteral("yyyyMMdd-HHmmss")) + QStringLiteral("-wal"));
            QVERIFY(taken.open(QIODevice::WriteOnly));
        }

        Database db(path);
        QVERIFY(!db.open());
        QVERIFY(db.movedAsidePath().isEmpty());
        QFile main(path);
        QVERIFY(main.open(QIODevice::ReadOnly));
        QCOMPARE(main.readAll(), QByteArray("not a database"));
        QVERIFY(QFile::exists(path + QStringLiteral("-wal")));
    }

    void damagedDatabaseIsMovedAsideAsCorrupt()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        {
            Database db(path);
            QVERIFY(db.open());
        }
        // Keep the 100-byte file header, wreck the schema page behind it.
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.seek(100));
        file.write(QByteArray(1000, '\xAB'));
        file.close();

        Database db(path);
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QVERIFY(db.movedAsideWasCorrupt());
        QVERIFY(db.movedAsidePath().contains(QStringLiteral(".corrupt-")));
        QCOMPARE(db.schemaVersion(), Database::latestSchemaVersion());
        QVERIFY(QFile::exists(db.movedAsidePath()));
    }

    void lockedOwnDatabaseIsNeitherMovedNorReplaced()
    {
        // Another process holding the file (a backup tool, an sqlite3 shell) says nothing about
        // whose file it is: opening must fail and leave the history where it is.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        {
            Database db(path);
            QVERIFY(db.open());
        }

        const QString lockName = QStringLiteral("locker");
        {
            QSqlDatabase locker = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), lockName);
            locker.setDatabaseName(path);
            locker.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=0"));
            QVERIFY(locker.open());
            QSqlQuery query(locker);
            QVERIFY(query.exec(QStringLiteral("PRAGMA locking_mode = EXCLUSIVE")));
            QVERIFY(query.exec(QStringLiteral("BEGIN EXCLUSIVE")));
            QVERIFY(query.exec(QStringLiteral("SELECT count(*) FROM sqlite_master")));
        }

        {
            Database db(path);
            QVERIFY(!db.open());
            QVERIFY(!db.lastError().isEmpty());
            QVERIFY(db.movedAsidePath().isEmpty());
        }
        QVERIFY(movedAsideFiles(QDir(dir.path())).isEmpty());
        QVERIFY(QFile::exists(path));

        {
            QSqlDatabase locker = QSqlDatabase::database(lockName, false);
            QSqlQuery(locker).exec(QStringLiteral("ROLLBACK"));
            locker.close();
        }
        QSqlDatabase::removeDatabase(lockName);

        // Once the lock is gone the same file opens normally.
        Database db(path);
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QVERIFY(db.movedAsidePath().isEmpty());
    }

    void foreignDatabaseIsNotReportedAsCorrupt()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        createForeignDatabase(path, QStringLiteral("CREATE TABLE notes (text TEXT)"), 0, 42);
        Database db(path);
        QVERIFY(db.open());
        QVERIFY(!db.movedAsidePath().isEmpty());
        QVERIFY(!db.movedAsideWasCorrupt());
    }

    // --- Taking back what Sugo 1.0.1 moved aside ------------------------------

    void sugo100DatabaseMovedAsideIsTakenBack_data()
    {
        QTest::addColumn<bool>("onlyInWal");
        QTest::newRow("folded into the file") << false;
        QTest::newRow("only in the write-ahead log") << true;
    }

    void sugo100DatabaseMovedAsideIsTakenBack()
    {
        QFETCH(bool, onlyInWal);
        const Sugo100Stamp stamp = onlyInWal ? Sugo100Stamp::OnlyInWal : Sugo100Stamp::InFile;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        const QString aside = setUpMovedAsideSugo100(QDir(dir.path()), QStringLiteral("20261104-232400"), stamp);
        const QByteArray asideBefore = fileContents(aside);
        const QByteArray asideWalBefore = fileContents(aside + QStringLiteral("-wal"));
        QCOMPARE(asideWalBefore.isEmpty(), stamp == Sugo100Stamp::InFile);
        {
            Database db(path); // what 1.0.1 created
            QVERIFY(db.open());
            addBuiltinTimer(db.connectionName());
        }

        Database db(path);
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QVERIFY2(db.recoverSugo100Database(), qPrintable(db.lastError()));
        QCOMPARE(db.recoveredFromPath(), aside);
        QVERIFY(QFile::exists(db.replacedPath()));
        QVERIFY(db.replacedPath().startsWith(path + QStringLiteral(".replaced-")));

        const QString c = db.connectionName();
        QCOMPARE(scalar(c, QStringLiteral("PRAGMA application_id")).toInt(), Database::kApplicationId);
        QCOMPARE(db.schemaVersion(), Database::latestSchemaVersion());
        QCOMPARE(scalar(c, QStringLiteral("SELECT note FROM session")).toString(), QStringLiteral("Chapter 3"));
        QCOMPARE(scalar(c, QStringLiteral("SELECT name FROM preset WHERE uuid = 'mine'")).toString(), QStringLiteral("Deep work"));
        verifySugo100Timers(c);
        QSqlQuery query(QSqlDatabase::database(c, false));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO meta (key, value) VALUES ('written', 'yes')"))); // usable

        // The file moved aside is left exactly as it was; the replaced one keeps its timer.
        QCOMPARE(fileContents(aside), asideBefore);
        QCOMPARE(fileContents(aside + QStringLiteral("-wal")), asideWalBefore);
        QVERIFY(!QFile::exists(aside + QStringLiteral("-shm")));
        {
            Database replaced(db.replacedPath());
            QVERIFY(replaced.open());
            QCOMPARE(scalar(replaced.connectionName(), QStringLiteral("SELECT uuid FROM preset")).toString(),
                     QStringLiteral("default"));
        }

        // Once is enough: the database now holds the user's data.
        Database again(path);
        QVERIFY(again.open());
        QVERIFY(again.recoverSugo100Database());
        QVERIFY(again.recoveredFromPath().isEmpty());
        QCOMPARE(QDir(dir.path()).entryList({QStringLiteral("*.replaced-*")}, QDir::Files).size(), 1);
    }

    void withoutFilesMovedAsideThereIsNothingToTakeBack()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        Database db(path);
        QVERIFY(db.open());
        QVERIFY(db.recoverSugo100Database());
        QVERIFY(db.recoveredFromPath().isEmpty());
        QCOMPARE(scalar(db.connectionName(), QStringLiteral("SELECT value FROM meta WHERE key = 'sugo100_recovery'")).toString(),
                 QStringLiteral("checked"));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot),
                 (QStringList{QStringLiteral("sugo.db"), QStringLiteral("sugo.db-shm"), QStringLiteral("sugo.db-wal")}));
    }

    void dataIsTakenBackOnlyOnce()
    {
        // A user of 1.0.0's built-in timer only: taken back, then the history cleared. The 1.0.0
        // data must not come back on a later start, nor after the replaced file is deleted.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        const QString aside = setUpMovedAsideSugo100(QDir(dir.path()));
        {
            Database db(path);
            QVERIFY(db.open());
            addBuiltinTimer(db.connectionName());
        }
        {
            Database db(path);
            QVERIFY(db.open());
            QVERIFY(db.recoverSugo100Database());
            QCOMPARE(db.recoveredFromPath(), aside);
            QCOMPARE(scalar(db.connectionName(), QStringLiteral("SELECT value FROM meta WHERE key = 'sugo100_recovery'")).toString(),
                     QFileInfo(aside).fileName());
            clearUserData(db.connectionName());
        }
        const auto replacedFiles = [&dir]() {
            return QDir(dir.path()).entryList({QStringLiteral("sugo.db.replaced-*")}, QDir::Files);
        };
        QCOMPARE(replacedFiles().size(), 1);
        for (int run = 0; run < 2; ++run) {
            Database db(path);
            QVERIFY(db.open());
            QVERIFY(db.recoverSugo100Database());
            QVERIFY(db.recoveredFromPath().isEmpty());
            QCOMPARE(scalar(db.connectionName(), QStringLiteral("SELECT count(*) FROM session")).toInt(), 0);
            QCOMPARE(scalar(db.connectionName(), QStringLiteral("SELECT count(*) FROM preset WHERE uuid = 'mine'")).toInt(), 0);
            if (run == 0) {
                QCOMPARE(replacedFiles().size(), 1);
                // The user tidies up: the key alone still says it was done.
                for (const QString &name : replacedFiles()) {
                    removeDatabaseFiles(dir.filePath(name));
                }
            }
        }
        QVERIFY(replacedFiles().isEmpty());
    }

    void theFirstLookIsTheOnlyOne()
    {
        // The user had recorded sessions under 1.0.1 when this version first looked, then cleared
        // them: the 1.0.0 file is not taken then either.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        setUpMovedAsideSugo100(QDir(dir.path()));
        {
            Database db(path);
            QVERIFY(db.open());
            QSqlQuery query(QSqlDatabase::database(db.connectionName(), false));
            QVERIFY(query.exec(QStringLiteral(
                "INSERT INTO session (kind, started_at, ended_at, duration_sec, planned_sec, completed, preset_name) "
                "VALUES ('work', 1, 2, 1, 1, 1, 'New')")));
            QVERIFY(db.recoverSugo100Database());
            QVERIFY(db.recoveredFromPath().isEmpty());
            QCOMPARE(scalar(db.connectionName(), QStringLiteral("SELECT value FROM meta WHERE key = 'sugo100_recovery'")).toString(),
                     QStringLiteral("checked"));
            clearUserData(db.connectionName());
        }
        Database db(path);
        QVERIFY(db.open());
        QVERIFY(db.recoverSugo100Database());
        QVERIFY(db.recoveredFromPath().isEmpty());
        QCOMPARE(scalar(db.connectionName(), QStringLiteral("SELECT count(*) FROM session")).toInt(), 0);
        QVERIFY(QDir(dir.path()).entryList({QStringLiteral("sugo.db.replaced-*")}, QDir::Files).isEmpty());
    }

    void nothingIsTakenBackOverTheUsersData()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        setUpMovedAsideSugo100(QDir(dir.path()));
        const QStringList rows = {
            QStringLiteral("INSERT INTO session (kind, started_at, ended_at, duration_sec, planned_sec, completed, preset_name) "
                           "VALUES ('work', 1, 2, 1, 1, 1, 'New')"),
            QStringLiteral("INSERT INTO preset (uuid, name, work_sec, short_break_sec, long_break_sec, created_at) "
                           "VALUES ('new', 'New', 1500, 300, 900, 0)"),
            QStringLiteral("INSERT INTO preset_tombstone (uuid, deleted_at) VALUES ('gone', 1)"),
        };
        for (const QString &row : rows) {
            QFile::remove(path);
            QFile::remove(path + QStringLiteral("-wal"));
            QFile::remove(path + QStringLiteral("-shm"));
            Database db(path);
            QVERIFY(db.open());
            QSqlQuery query(QSqlDatabase::database(db.connectionName(), false));
            QVERIFY2(query.exec(row), qPrintable(row));
            QVERIFY(db.recoverSugo100Database());
            QVERIFY(db.recoveredFromPath().isEmpty());
            QCOMPARE(scalar(db.connectionName(), QStringLiteral("SELECT count(*) FROM session WHERE note = 'Chapter 3'")).toInt(), 0);
        }
        QVERIFY(QDir(dir.path()).entryList({QStringLiteral("*.replaced-*")}, QDir::Files).isEmpty());
    }

    void nothingIsTakenBackFromSeveralOrForeignFiles()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        // Another program's file, and a 1.0.0 file with nothing the user made.
        createForeignDatabase(path + QStringLiteral(".foreign-20261101-100000"), QStringLiteral("CREATE TABLE notes (text TEXT)"), 2,
                              kSugo100ApplicationId);
        const QString unused = path + QStringLiteral(".foreign-20261102-100000");
        createSugo100Database(unused);
        {
            const QString name = QStringLiteral("unused-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
                db.setDatabaseName(unused);
                QVERIFY(db.open());
                QSqlQuery query(db);
                QVERIFY(query.exec(QStringLiteral("DELETE FROM session")));
                QVERIFY(query.exec(QStringLiteral("DELETE FROM preset WHERE is_builtin = 0")));
                db.close();
            }
            QSqlDatabase::removeDatabase(name);
        }
        {
            Database db(path);
            QVERIFY(db.open());
            QVERIFY(db.recoverSugo100Database());
            QVERIFY(db.recoveredFromPath().isEmpty());
        }

        // Those two are passed over, the one with data after them is still found.
        {
            QTemporaryDir other;
            const QString otherPath = other.filePath(QStringLiteral("sugo.db"));
            for (const QString &name : QDir(dir.path()).entryList({QStringLiteral("sugo.db.foreign-*")}, QDir::Files)) {
                QVERIFY(QFile::copy(dir.filePath(name), other.filePath(name)));
            }
            const QString wanted = setUpMovedAsideSugo100(QDir(other.path()), QStringLiteral("20261103-100000"));
            Database db(otherPath);
            QVERIFY(db.open());
            QVERIFY(db.recoverSugo100Database());
            QCOMPARE(db.recoveredFromPath(), wanted);
        }

        // Two 1.0.0 files with data: no way to tell which one the user wants. (A new database:
        // the one above has had its look.)
        setUpMovedAsideSugo100(QDir(dir.path()), QStringLiteral("20261103-100000"));
        setUpMovedAsideSugo100(QDir(dir.path()), QStringLiteral("20261104-100000"));
        removeDatabaseFiles(path);
        {
            Database db(path);
            QVERIFY(db.open());
            QVERIFY(db.recoverSugo100Database());
            QVERIFY(db.recoveredFromPath().isEmpty());
            QCOMPARE(scalar(db.connectionName(), QStringLiteral("SELECT count(*) FROM session")).toInt(), 0);
        }
        QVERIFY(QDir(dir.path()).entryList({QStringLiteral("*.replaced-*")}, QDir::Files).isEmpty());
    }

    void inMemoryDatabaseIsTemporary()
    {
        Database memory(QStringLiteral(":memory:"));
        QVERIFY(memory.open());
        QVERIFY(memory.isTemporary());

        QTemporaryDir dir;
        Database file(dir.filePath(QStringLiteral("sugo.db")));
        QVERIFY(file.open());
        QVERIFY(!file.isTemporary());
    }

    void dataDirVariableReplacesDefaultLocation()
    {
        const QByteArray saved = qgetenv("SUGO_DATA_DIR");
        const bool wasSet = qEnvironmentVariableIsSet("SUGO_DATA_DIR");
        QTemporaryDir dir;
        qputenv("SUGO_DATA_DIR", QFile::encodeName(dir.path()));
        QCOMPARE(Database::defaultPath(), dir.filePath(QStringLiteral("sugo.db")));
        qunsetenv("SUGO_DATA_DIR");
        QVERIFY(Database::defaultPath().endsWith(QLatin1String("/sugo/sugo.db")));
        QVERIFY(Database::defaultPath() != dir.filePath(QStringLiteral("sugo.db")));
        if (wasSet) {
            qputenv("SUGO_DATA_DIR", saved);
        }
    }

    void unusablePathReportsAnError()
    {
        QTemporaryDir dir;
        // A regular file where a directory is needed.
        QFile blocker(dir.filePath(QStringLiteral("blocker")));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();

        Database db(dir.filePath(QStringLiteral("blocker/sugo.db")));
        QVERIFY(!db.open());
        QVERIFY(!db.lastError().isEmpty());
    }
};

QTEST_GUILESS_MAIN(DatabaseTest)
#include "tst_database.moc"
