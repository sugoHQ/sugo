// SPDX-License-Identifier: GPL-3.0-or-later
#include "Database.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QUuid>
#include <QtGlobal>

#include <algorithm>

namespace
{
/// One entry per schema version, in order. Never edit a released entry: append a new one.
/// Sugo 1.0.0 numbered its versions differently; Database::convertSugo100File() renumbers its files.
const QList<QStringList> &migrations()
{
    static const QList<QStringList> list = {
        // Version 1
        {
            QStringLiteral(R"(
CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
))"),
            QStringLiteral(R"(
CREATE TABLE preset (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    uuid               TEXT    NOT NULL UNIQUE,
    name               TEXT    NOT NULL,
    category           TEXT    NOT NULL DEFAULT '',
    work_sec           INTEGER NOT NULL CHECK (work_sec > 0),
    short_break_sec    INTEGER NOT NULL CHECK (short_break_sec >= 0),
    long_break_sec     INTEGER NOT NULL CHECK (long_break_sec >= 0),
    cycles_before_long INTEGER NOT NULL DEFAULT 4 CHECK (cycles_before_long >= 0),
    is_builtin         INTEGER NOT NULL DEFAULT 0,
    sort_order         INTEGER NOT NULL DEFAULT 0,
    created_at         INTEGER NOT NULL
))"),
            QStringLiteral(R"(
CREATE TABLE preset_option (
    preset_id INTEGER NOT NULL REFERENCES preset(id) ON DELETE CASCADE,
    key       TEXT    NOT NULL,
    value     TEXT    NOT NULL,
    PRIMARY KEY (preset_id, key)
))"),
            QStringLiteral(R"(
CREATE TABLE session (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    kind         TEXT    NOT NULL CHECK (kind IN ('work', 'short_break', 'long_break')),
    started_at   INTEGER NOT NULL,
    ended_at     INTEGER NOT NULL,
    duration_sec INTEGER NOT NULL,
    planned_sec  INTEGER NOT NULL,
    completed    INTEGER NOT NULL,
    preset_id    INTEGER REFERENCES preset(id) ON DELETE SET NULL,
    preset_name  TEXT    NOT NULL,
    category     TEXT    NOT NULL DEFAULT '',
    note         TEXT    NOT NULL DEFAULT ''
))"),
            QStringLiteral("CREATE INDEX idx_session_started ON session(started_at)"),
            QStringLiteral("CREATE INDEX idx_session_kind_started ON session(kind, started_at)"),
        },
        // Version 2: what Sugo Sync needs to merge timers (docs/sync-plan.md). Preset times are
        // milliseconds since the epoch (UTC) from here on; version 1 stored created_at in seconds.
        // updated_at 0: not changed since this version. updated_by / deleted_by: the Sugo Sync
        // device that made the change, empty for this computer.
        {
            QStringLiteral("ALTER TABLE preset ADD COLUMN updated_at INTEGER NOT NULL DEFAULT 0"),
            QStringLiteral("ALTER TABLE preset ADD COLUMN updated_by TEXT NOT NULL DEFAULT ''"),
            QStringLiteral("UPDATE preset SET created_at = created_at * 1000"),
            QStringLiteral(R"(
CREATE TABLE preset_tombstone (
    uuid       TEXT    PRIMARY KEY,
    deleted_at INTEGER NOT NULL,
    deleted_by TEXT    NOT NULL DEFAULT ''
))"),
        },
    };
    return list;
}

bool run(QSqlDatabase &db, const QString &sql, QString *error)
{
    QSqlQuery query(db);
    if (!query.exec(sql)) {
        *error = query.lastError().text();
        return false;
    }
    return true;
}

int pragmaInt(QSqlDatabase &db, const QString &name)
{
    QSqlQuery query(db);
    return query.exec(QStringLiteral("PRAGMA ") + name) && query.next() ? query.value(0).toInt() : -1;
}

/// A database file and the journal files that belong to it.
const QStringList &journalSuffixes()
{
    static const QStringList suffixes = {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")};
    return suffixes;
}

/// The history and the notes of sessions are private, like the settings file: the database and
/// its journal files are for the owner only (SQLite gives -wal and -shm the mode of the file).
void restrictToOwner(const QString &path)
{
    for (const QString &suffix : journalSuffixes()) {
        if (QFile::exists(path + suffix)) {
            QFile::setPermissions(path + suffix, QFile::ReadOwner | QFile::WriteOwner);
        }
    }
}

/// What the user made in a database of the current schema: sessions, timers of their own (the
/// built-in one is there from the start) and deleted timers. -1 if it cannot be read.
int userRows(QSqlDatabase &db)
{
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT (SELECT count(*) FROM session) + (SELECT count(*) FROM preset WHERE is_builtin = 0) "
                                   "+ (SELECT count(*) FROM preset_tombstone)"))
        || !query.next()) {
        return -1;
    }
    return query.value(0).toInt();
}

/// The meta key that makes recoverSugo100Database() a one-time look: the name of the file whose
/// data was taken back, or "checked" if none was.
const QString kSugo100RecoveryKey = QStringLiteral("sugo100_recovery");

bool hasMeta(QSqlDatabase &db, const QString &key, bool *readable)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT 1 FROM meta WHERE key = ?"));
    query.addBindValue(key);
    *readable = query.exec();
    return *readable && query.next();
}

bool setMeta(QSqlDatabase &db, const QString &key, const QString &value)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("INSERT INTO meta (key, value) VALUES (?, ?) "
                                 "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    query.addBindValue(key);
    query.addBindValue(value);
    return query.exec();
}

/// The application id in a SQLite file's header (big-endian at offset 68), read without SQLite,
/// so that a file is never opened, let alone changed, just to look at it. -1 if unreadable.
qint64 headerApplicationId(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return -1;
    }
    const QByteArray header = file.read(72);
    if (header.size() < 72 || !header.startsWith(QByteArrayLiteral("SQLite format 3"))) {
        return -1;
    }
    qint64 id = 0;
    for (int i = 68; i < 72; ++i) {
        id = (id << 8) | quint8(header.at(i));
    }
    return id;
}
} // namespace

Database::Database(const QString &path)
    : m_path(path)
    , m_connectionName(QStringLiteral("sugo-") + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

Database::~Database()
{
    closeConnection();
}

int Database::latestSchemaVersion()
{
    return int(migrations().size());
}

QString Database::defaultPath()
{
    // Tests and smoke runs (see Settings::defaultPath()).
    const QString dataDir = qEnvironmentVariable("SUGO_DATA_DIR");
    if (!dataDir.isEmpty()) {
        return dataDir + QStringLiteral("/sugo.db");
    }
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/sugo/sugo.db");
}

void Database::closeConnection()
{
    {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
        if (db.isValid() && db.isOpen()) {
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool Database::connect(bool *corrupt)
{
    if (corrupt) {
        *corrupt = false;
    }
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(m_path);
    if (!db.open()) {
        m_error = db.lastError().text();
        if (corrupt) {
            *corrupt = isCorruptionError(db.lastError().nativeErrorCode());
        }
        return false;
    }
    // Foreign keys are off by default in SQLite and must be enabled per connection.
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("PRAGMA foreign_keys = ON"))) {
        m_error = query.lastError().text();
        if (corrupt) {
            *corrupt = isCorruptionError(query.lastError().nativeErrorCode());
        }
        return false;
    }
    // Deleted rows (cleared history, private notes) are overwritten rather than left readable in
    // free pages. Like foreign_keys, a setting of the connection: it writes nothing by itself.
    if (!query.exec(QStringLiteral("PRAGMA secure_delete = ON"))) {
        m_error = query.lastError().text();
        if (corrupt) {
            *corrupt = isCorruptionError(query.lastError().nativeErrorCode());
        }
        return false;
    }
    return true;
}

bool Database::hasSqliteHeader() const
{
    QFile file(m_path);
    if (!file.exists() || file.size() == 0) {
        return true; // nothing there yet: SQLite creates the file
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return true; // unreadable is not the same as damaged; let SQLite report it
    }
    static const QByteArray magic("SQLite format 3\0", 16);
    return file.read(magic.size()) == magic;
}

bool Database::isCorruptionError(const QString &nativeCode)
{
    // SQLITE_CORRUPT (11) and SQLITE_NOTADB (26), possibly as extended codes. Anything else
    // (busy, locked, permissions...) says nothing about the file and must not move it.
    bool ok = false;
    const int code = nativeCode.toInt(&ok) & 0xff;
    return ok && (code == 11 || code == 26);
}

bool Database::open()
{
    const bool inMemory = isTemporary();
    if (!inMemory && !QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        m_error = QStringLiteral("Cannot create the data directory for %1").arg(m_path);
        return false;
    }

    if (!inMemory && !hasSqliteHeader() && !moveFileAside(QStringLiteral("corrupt"))) {
        return false;
    }

    bool unopenable = false;
    if (!connect(&unopenable)) {
        if (inMemory || !unopenable || !moveFileAside(QStringLiteral("corrupt")) || !connect()) {
            return false;
        }
    }

    if (!inMemory) {
        // Decide ownership before any write or pragma that would modify a foreign file.
        // The handle must go out of scope before the connection is removed below.
        Ownership verdict = Ownership::Unknown;
        {
            QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
            verdict = ownership(db);
        }
        switch (verdict) {
        case Ownership::Ours:
            break;
        case Ownership::Corrupt:
            if (!moveFileAside(QStringLiteral("corrupt")) || !connect()) {
                return false;
            }
            break;
        case Ownership::Foreign:
            if (!moveFileAside(QStringLiteral("foreign")) || !connect()) {
                return false;
            }
            break;
        case Ownership::Unknown:
            // m_error says why; the caller falls back to a temporary database for this run.
            closeConnection();
            return false;
        }
    }

    return finishOpen();
}

bool Database::finishOpen()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!isTemporary()) {
        // WAL keeps writes cheap and readers unblocked; a crash cannot corrupt the file.
        if (!run(db, QStringLiteral("PRAGMA journal_mode = WAL"), &m_error)
            || !run(db, QStringLiteral("PRAGMA synchronous = NORMAL"), &m_error)) {
            return false;
        }
        restrictToOwner(m_path);
    }
    return migrate(db);
}

Database::Ownership Database::ownership(QSqlDatabase &db)
{
    // Unreadable schema or check: damaged or not a database. Other errors (busy, locked...) say
    // nothing about whose file it is.
    const auto failed = [this](const QSqlError &error) {
        m_error = error.text();
        return isCorruptionError(error.nativeErrorCode()) ? Ownership::Corrupt : Ownership::Unknown;
    };

    bool hasTables = false;
    {
        QSqlQuery query(db);
        if (!query.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%' LIMIT 1"))) {
            return failed(query.lastError());
        }
        hasTables = query.next();
    }

    if (!hasTables) {
        return Ownership::Ours; // new or empty file: ours to initialise
    }

    // A readable schema can still sit on top of damaged pages. The files are small, so the
    // quick check costs a few milliseconds.
    QSqlQuery check(db);
    if (!check.exec(QStringLiteral("PRAGMA quick_check(1)"))) {
        return failed(check.lastError());
    }
    if (!check.next()) {
        m_error = QStringLiteral("The integrity check returned no result");
        return Ownership::Corrupt;
    }
    const QString integrity = check.value(0).toString();
    if (integrity != QLatin1String("ok")) {
        m_error = QStringLiteral("The integrity check failed: %1").arg(integrity);
        return Ownership::Corrupt;
    }

    QSqlQuery idQuery(db);
    if (!idQuery.exec(QStringLiteral("PRAGMA application_id")) || !idQuery.next()) {
        return failed(idQuery.lastError());
    }
    const int applicationId = idQuery.value(0).toInt();
    if (applicationId == kApplicationId) {
        return Ownership::Ours;
    }
    if (applicationId == kSugo100ApplicationId) {
        // Written by Sugo 1.0.0, which stamped its files after creating the tables, so its files
        // have version 1 or 2. Any other version under that id was never written by a Sugo.
        QSqlQuery versionQuery(db);
        if (!versionQuery.exec(QStringLiteral("PRAGMA user_version")) || !versionQuery.next()) {
            return failed(versionQuery.lastError());
        }
        const int version = versionQuery.value(0).toInt();
        if (version < 1 || version > kSugo100SchemaVersion) {
            return Ownership::Foreign;
        }
        // And it has 1.0.0's tables: another program's file that happens to carry the id is not
        // converted (and then left failing on every start), it is moved aside.
        QSqlQuery tables(db);
        if (!tables.exec(QStringLiteral("SELECT count(*) FROM sqlite_master WHERE type = 'table' "
                                        "AND name IN ('meta', 'preset', 'preset_option', 'session')"))
            || !tables.next()) {
            return failed(tables.lastError());
        }
        return tables.value(0).toInt() == 4 ? Ownership::Ours : Ownership::Foreign;
    }
    return Ownership::Foreign;
}

bool Database::moveFileAside(const QString &reason)
{
    closeConnection();

    const QString target = m_path + QLatin1Char('.') + reason + QLatin1Char('-')
        + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    if (!moveFiles(m_path, target)) {
        m_error = QStringLiteral("%1 is not a usable Sugo database and cannot be moved aside").arg(m_path);
        return false;
    }
    m_movedAsidePath = target;
    m_movedAsideCorrupt = reason == QLatin1String("corrupt");
    return true;
}

bool Database::moveFiles(const QString &from, const QString &to)
{
    // The journal files belong to the same database and must travel with it.
    QStringList moved;
    for (const QString &suffix : journalSuffixes()) {
        if (!QFile::exists(from + suffix)) {
            continue;
        }
        if (QFile::exists(to + suffix) || !QFile::rename(from + suffix, to + suffix)) {
            // Put back what was moved already: a journal must never be left with another file.
            for (const QString &done : std::as_const(moved)) {
                QFile::rename(to + done, from + done);
            }
            return false;
        }
        moved.append(suffix);
    }
    restrictToOwner(to);
    return true;
}

int Database::schemaVersion() const
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery query(db);
    if (query.exec(QStringLiteral("PRAGMA user_version")) && query.next()) {
        return query.value(0).toInt();
    }
    return -1;
}

bool Database::convertSugo100File(QSqlDatabase &db)
{
    // Runs in migrate()'s transaction, with the migrations after it: the new id never sits on
    // top of 1.0.0's numbering (where version 2 means "session.note added", not "ready for Sugo
    // Sync"), and a file the migrations cannot bring up to date keeps 1.0.0's id and version.
    const int version = pragmaInt(db, QStringLiteral("user_version"));
    bool ok = version >= 1 && version <= kSugo100SchemaVersion;
    if (!ok) {
        m_error = QStringLiteral("Unexpected schema version %1 in a Sugo 1.0.0 database").arg(version);
    }
    // 1.0.0's version 2 added session.note, which today's version 1 creates with the table.
    if (ok && version < 2) {
        ok = run(db, QStringLiteral("ALTER TABLE session ADD COLUMN note TEXT NOT NULL DEFAULT ''"), &m_error);
    }
    return ok && run(db, QStringLiteral("PRAGMA user_version = 1"), &m_error)
        && run(db, QStringLiteral("PRAGMA application_id = %1").arg(kApplicationId), &m_error);
}

bool Database::migrate(QSqlDatabase &db)
{
    // A file of Sugo 1.0.0 (ownership() accepted it): renumbered first, and brought up to date in
    // the same transaction, all or nothing. One without tables or version is simply stamped
    // below, like any new file.
    const bool sugo100 = pragmaInt(db, QStringLiteral("application_id")) == kSugo100ApplicationId && schemaVersion() > 0;
    if (sugo100) {
        if (!db.transaction()) {
            m_error = db.lastError().text();
            return false;
        }
        if (!convertSugo100File(db)) {
            db.rollback();
            return false;
        }
    }
    const auto fail = [&db, sugo100]() {
        if (sugo100) {
            db.rollback();
        }
        return false;
    };

    const int current = schemaVersion();
    if (current < 0) {
        m_error = QStringLiteral("Cannot read the schema version");
        return fail();
    }
    if (current > latestSchemaVersion()) {
        m_error = QStringLiteral("The database was created by a newer version of Sugo (schema %1)").arg(current);
        return fail();
    }

    // Stamp before the first table exists, so a file Sugo began to fill always reads as ours.
    if (pragmaInt(db, QStringLiteral("application_id")) != kApplicationId
        && !run(db, QStringLiteral("PRAGMA application_id = %1").arg(kApplicationId), &m_error)) {
        return fail();
    }

    for (int version = current; version < latestSchemaVersion(); ++version) {
        if (!sugo100 && !db.transaction()) {
            m_error = db.lastError().text();
            return false;
        }
        bool ok = true;
        for (const QString &statement : migrations().at(version)) {
            if (!run(db, statement, &m_error)) {
                ok = false;
                break;
            }
        }
        // PRAGMA takes no bound parameters; the value is an int we control.
        ok = ok && run(db, QStringLiteral("PRAGMA user_version = %1").arg(version + 1), &m_error);

        if (!ok) {
            db.rollback();
            return false;
        }
        if (!sugo100 && !db.commit()) {
            m_error = db.lastError().text();
            return false;
        }
    }
    if (sugo100 && !db.commit()) {
        m_error = db.lastError().text();
        db.rollback();
        return false;
    }
    return true;
}

bool Database::recoverSugo100Database()
{
    if (isTemporary() || !m_movedAsidePath.isEmpty() || !m_recoveredFromPath.isEmpty()) {
        return true;
    }
    const QFileInfo info(m_path);
    const QDir dir = info.absoluteDir();
    {
        // Once per database: data taken back must not come back again after the user cleared it.
        // A database replaced earlier (sugo.db.replaced-*) also says it was done, should a
        // restored backup have taken the key away.
        QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
        bool readable = false;
        if (!db.isOpen() || hasMeta(db, kSugo100RecoveryKey, &readable) || !readable) {
            return true;
        }
        if (!dir.entryList({info.fileName() + QStringLiteral(".replaced-*")}, QDir::Files).isEmpty()
            || userRows(db) != 0) {
            // The user has data here (or it cannot be read): never replace it, nor look again.
            setMeta(db, kSugo100RecoveryKey, QStringLiteral("checked"));
            return true;
        }
    }
    const auto checked = [this]() {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
        setMeta(db, kSugo100RecoveryKey, QStringLiteral("checked"));
        return true;
    };

    // Candidates: the files Sugo moved aside as foreign, whose header carries 1.0.0's id. Each is
    // copied (with its write-ahead log) into a private directory and converted there.
    QStringList names = dir.entryList({info.fileName() + QStringLiteral(".foreign-*")}, QDir::Files, QDir::Name);
    names.removeIf([](const QString &name) {
        return name.endsWith(QLatin1String("-wal")) || name.endsWith(QLatin1String("-shm"));
    });
    if (names.isEmpty()) {
        return checked(); // the usual case: nothing was ever moved aside
    }
    QTemporaryDir work;
    if (!work.isValid()) {
        return true; // try again next time
    }
    QString found;
    QString foundCopy;
    int candidates = 0;
    int copies = 0;
    for (const QString &name : std::as_const(names)) {
        const QString original = dir.filePath(name);
        // A file whose header still reads 0 may have its stamp in the write-ahead log.
        const qint64 headerId = headerApplicationId(original);
        if (headerId != kSugo100ApplicationId && !(headerId == 0 && QFile::exists(original + QStringLiteral("-wal")))) {
            continue;
        }
        const QString copy = work.filePath(QString::number(++copies) + QStringLiteral(".db"));
        bool copied = QFile::copy(original, copy);
        if (copied && QFile::exists(original + QStringLiteral("-wal"))) {
            copied = QFile::copy(original + QStringLiteral("-wal"), copy + QStringLiteral("-wal"));
        }
        if (!copied) {
            continue;
        }
        QFile::setPermissions(copy, QFile::ReadOwner | QFile::WriteOwner);
        if (QFile::exists(copy + QStringLiteral("-wal"))) {
            QFile::setPermissions(copy + QStringLiteral("-wal"), QFile::ReadOwner | QFile::WriteOwner);
        }
        // Read the id through SQLite now (it sees the log), before Database would stamp the copy.
        bool isSugo100 = false;
        {
            const QString probe = QStringLiteral("sugo-probe-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), probe);
                db.setDatabaseName(copy);
                if (db.open()) {
                    const int version = pragmaInt(db, QStringLiteral("user_version"));
                    isSugo100 = pragmaInt(db, QStringLiteral("application_id")) == kSugo100ApplicationId
                        && version >= 1 && version <= kSugo100SchemaVersion;
                }
                db.close();
            }
            QSqlDatabase::removeDatabase(probe);
        }
        if (!isSugo100) {
            continue;
        }
        // Closing the converted copy folds its write-ahead log back into the file.
        Database converted(copy);
        if (!converted.open() || !converted.movedAsidePath().isEmpty()) {
            continue; // damaged: leave it to the user
        }
        QSqlDatabase db = QSqlDatabase::database(converted.connectionName(), false);
        if (userRows(db) <= 0) {
            continue; // nothing worth taking back
        }
        // Marked in the copy itself, so the database that takes this one's place already says so.
        if (!setMeta(db, kSugo100RecoveryKey, name)) {
            continue;
        }
        ++candidates;
        found = original;
        foundCopy = copy;
    }
    if (candidates != 1) {
        return checked(); // none, or several and no way to tell which one the user wants
    }

    // Swap: this (empty) database is kept aside, the converted copy takes its place.
    closeConnection();
    const QString replaced = m_path + QStringLiteral(".replaced-")
        + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    bool swapped = moveFiles(m_path, replaced);
    if (swapped) {
        swapped = QFile::copy(foundCopy, m_path) && connect() && finishOpen();
        if (!swapped) {
            closeConnection();
            for (const QString &suffix : journalSuffixes()) {
                QFile::remove(m_path + suffix);
            }
            if (!moveFiles(replaced, m_path)) {
                m_error = QStringLiteral("Cannot put back %1 (kept as %2)").arg(m_path, replaced);
                return false;
            }
        }
    }
    if (!swapped) {
        // Back to the database open() had opened.
        const QString why = m_error;
        if (!connect() || !finishOpen()) {
            return false;
        }
        m_error = why;
        return true;
    }
    m_recoveredFromPath = found;
    m_replacedPath = replaced;
    return true;
}
