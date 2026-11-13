// SPDX-License-Identifier: GPL-3.0-or-later
#include "DataManager.h"

#include "Database.h"
#include "SessionRecorder.h"
#include "SessionRepository.h"
#include "StatsModel.h"
#include "PresetModel.h"
#include "PresetPersistence.h"
#include "PresetRepository.h"

#include "Localization.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#include <memory>

namespace
{
QString comparablePath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

bool hasSqliteHeader(const QString &path)
{
    QFile file(path);
    static const QByteArray magic("SQLite format 3\0", 16);
    return file.open(QIODevice::ReadOnly) && file.read(magic.size()) == magic;
}

/// Columns copied by a restore, per table. Explicit lists keep the restore correct whatever
/// column order the backup's migrations produced.
struct RestoredTable {
    QString name;
    QString columns;
    /// The rows a restore replaces, if not all: the others belong to this computer's file, not to
    /// the user's data, and are neither deleted here nor copied from the backup.
    QString rows = {};
};

const QList<RestoredTable> &restoredTables()
{
    // Parents first, so foreign keys resolve while inserting.
    static const QList<RestoredTable> tables = {
        {QStringLiteral("preset"),
         QStringLiteral("id, uuid, name, category, work_sec, short_break_sec, long_break_sec, cycles_before_long, "
                        "is_builtin, sort_order, created_at")},
        {QStringLiteral("preset_option"), QStringLiteral("preset_id, key, value")},
        {QStringLiteral("session"),
         QStringLiteral("id, uuid, kind, started_at, ended_at, duration_sec, planned_sec, completed, preset_id, "
                        "preset_name, category, note, updated_at, updated_by")},
        // The one-time look for Sugo 1.0.0's data (Database::recoverSugo100Database()) was made
        // for this file: a backup from before it, or from another computer, must not undo it.
        {QStringLiteral("meta"), QStringLiteral("key, value"), QStringLiteral("key <> 'sugo100_recovery'")},
    };
    return tables;
}
}

Q_LOGGING_CATEGORY(lcDataManager, "sugo.data")

DataManager::DataManager(Database *database, SessionRepository *sessions, PresetRepository *presetsRepository,
                         PresetModel *presets, StatsModel *stats, QObject *parent)
    : QObject(parent), m_database(database), m_sessions(sessions), m_presetsRepository(presetsRepository),
      m_presets(presets), m_stats(stats)
{
    if (m_database) {
        m_movedAsidePath = m_database->movedAsidePath();
        m_movedAsideCorrupt = m_database->movedAsideWasCorrupt();
        m_sugo100ReplacedPath = m_database->replacedPath();
    }
}

void DataManager::setMovedAsideDatabase(const QString &path, bool corrupt)
{
    m_movedAsidePath = path;
    m_movedAsideCorrupt = corrupt;
}

QString DataManager::databasePath() const
{
    return m_database ? m_database->path() : QString();
}

int DataManager::sessionCount() const
{
    return m_sessions ? m_sessions->count() : 0;
}

int DataManager::presetCount() const
{
    return m_presets ? m_presets->rowCount() : 0;
}

bool DataManager::usingTemporaryDatabase() const
{
    return !m_database || m_database->isTemporary();
}

bool DataManager::saveFailing() const
{
    return (m_recorder && m_recorder->isSaveFailing()) || (m_presetPersistence && m_presetPersistence->isSaveFailing());
}

bool DataManager::timersUnreadable() const
{
    return m_presetPersistence && m_presetPersistence->loadFailed();
}

void DataManager::setPresetPersistence(PresetPersistence *persistence)
{
    if (m_presetPersistence) {
        disconnect(m_presetPersistence, nullptr, this, nullptr);
    }
    m_presetPersistence = persistence;
    if (m_presetPersistence) {
        connect(m_presetPersistence, &PresetPersistence::saveFailingChanged, this, &DataManager::saveFailingChanged);
        connect(m_presetPersistence, &QObject::destroyed, this, [this]() {
            m_presetPersistence = nullptr;
        });
    }
    Q_EMIT saveFailingChanged();
}

void DataManager::setSessionRecorder(SessionRecorder *recorder)
{
    if (m_recorder == recorder) {
        return;
    }
    if (m_recorder) {
        disconnect(m_recorder, nullptr, this, nullptr);
    }
    m_recorder = recorder;
    if (m_recorder) {
        // The StatsModel listens to the recorder itself; here only the counters need updating.
        connect(m_recorder, &SessionRecorder::sessionRecorded, this, &DataManager::dataChanged);
        connect(m_recorder, &SessionRecorder::saveFailingChanged, this, &DataManager::saveFailingChanged);
        connect(m_recorder, &QObject::destroyed, this, [this]() {
            m_recorder = nullptr;
        });
    }
}

qint64 DataManager::lastWorkSessionId() const
{
    return m_recorder ? m_recorder->noteTargetId() : -1;
}

bool DataManager::clearHistory()
{
    if (!m_sessions || !m_sessions->deleteAll()) {
        Q_EMIT operationFailed(i18n("Could not clear session history."));
        return false;
    }
    if (m_recorder) m_recorder->forgetHistory();
    if (m_stats) m_stats->refresh();
    Q_EMIT dataChanged();
    Q_EMIT operationSucceeded(i18n("Session history cleared."));
    return true;
}

bool DataManager::deleteSession(qint64 sessionId, bool fromCalendar)
{
    SessionRecord deleted;
    if (!m_sessions || sessionId <= 0 || !m_sessions->deleteById(sessionId, &deleted)) {
        Q_EMIT operationFailed(i18n("Could not delete the session."));
        return false;
    }
    if (m_recorder) m_recorder->forgetSession(sessionId);
    if (m_stats) m_stats->refresh();
    Q_EMIT dataChanged();
    Q_EMIT sessionDeleted(deleted, fromCalendar);
    Q_EMIT operationSucceeded(i18n("Session deleted."));
    return true;
}

void DataManager::setNowProvider(std::function<QDateTime()> provider)
{
    m_now = std::move(provider);
}

void DataManager::setTimerActiveProvider(std::function<bool()> provider)
{
    m_timerActive = std::move(provider);
}

bool DataManager::clearHistoryOlderThan(int months)
{
    if (!m_sessions || months < 1) {
        Q_EMIT operationFailed(i18n("Could not clear session history."));
        return false;
    }
    const QDateTime now = m_now ? m_now() : QDateTime::currentDateTime();
    const int removed = m_sessions->deleteStartedBefore(now.addMonths(-months).toMSecsSinceEpoch());
    if (removed < 0) {
        Q_EMIT operationFailed(i18n("Could not clear session history."));
        return false;
    }
    if (removed == 0) {
        Q_EMIT operationSucceeded(i18n("There were no sessions to remove."));
        return true;
    }
    // Unlike clearHistory(), the recorder is left alone: sessions still waiting to be stored are
    // newer than the cut-off and must not be dropped.
    if (m_stats) m_stats->refresh();
    Q_EMIT dataChanged();
    Q_EMIT operationSucceeded(i18np("%1 session removed.", "%1 sessions removed.", removed));
    return true;
}

void DataManager::openDatabaseFolder() const
{
    if (usingTemporaryDatabase()) {
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(databasePath()).absolutePath()));
}

bool DataManager::backupDatabase(const QUrl &fileUrl)
{
    const QString path = fileUrl.toLocalFile();
    if (path.isEmpty() || !m_database) {
        Q_EMIT operationFailed(i18n("Choose a local file for the backup."));
        return false;
    }
    if (comparablePath(path) == comparablePath(m_database->path())) {
        Q_EMIT operationFailed(i18n("Choose a backup file other than the active database."));
        return false;
    }
    const QString temporaryPath = path + QStringLiteral(".tmp-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlDatabase db = QSqlDatabase::database(m_database->connectionName(), false);
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("PRAGMA wal_checkpoint(FULL)"))) {
        Q_EMIT operationFailed(i18n("Could not prepare the database for backup: %1", query.lastError().text()));
        return false;
    }
    // The backup holds the whole history: create it private before anything is written into it
    // (VACUUM INTO fills an existing empty file and keeps its permissions).
    {
        QFile file(temporaryPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
            || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
            file.remove();
            Q_EMIT operationFailed(i18n("Could not create the backup file."));
            return false;
        }
    }
    QString escaped = temporaryPath;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    if (!query.exec(QStringLiteral("VACUUM INTO '%1'").arg(escaped))) {
        QFile::remove(temporaryPath);
        Q_EMIT operationFailed(i18n("Could not create the backup: %1", query.lastError().text()));
        return false;
    }
    const QString displacedPath = path + QStringLiteral(".old-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const bool hadOldFile = QFileInfo::exists(path);
    if (hadOldFile && !QFile::rename(path, displacedPath)) {
        QFile::remove(temporaryPath);
        Q_EMIT operationFailed(i18n("Could not replace the selected backup file."));
        return false;
    }
    if (!QFile::rename(temporaryPath, path)) {
        if (hadOldFile) QFile::rename(displacedPath, path);
        QFile::remove(temporaryPath);
        Q_EMIT operationFailed(i18n("Could not finish writing the backup file."));
        return false;
    }
    if (hadOldFile) QFile::remove(displacedPath);
    Q_EMIT operationSucceeded(i18n("Database backup created."));
    return true;
}

bool DataManager::validateBackup(const QString &path, QString *error) const
{
    if (!QFileInfo(path).isFile() || !hasSqliteHeader(path)) {
        *error = i18n("This file is not a Sugo database.");
        return false;
    }
    const QString connection = QStringLiteral("sugo-restore-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool valid = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        // `path` is the private copy: opening it read-write lets SQLite read its write-ahead log
        // (and create the -shm file it needs for that), wherever the user's backup lies.
        db.setDatabaseName(path);
        if (!db.open()) {
            *error = db.lastError().text();
        } else {
            QSqlQuery query(db);
            int appId = -1;
            int version = -1;
            if (query.exec(QStringLiteral("PRAGMA application_id")) && query.next()) appId = query.value(0).toInt();
            if (query.exec(QStringLiteral("PRAGMA user_version")) && query.next()) version = query.value(0).toInt();
            // A backup of Sugo 1.0.0 carries its id and its own numbering; Database converts it below.
            const bool sugo100 = appId == Database::kSugo100ApplicationId;
            const bool ours = sugo100 ? version <= Database::kSugo100SchemaVersion : appId == Database::kApplicationId;
            if (!ours || version < 1) {
                *error = i18n("This file is not a Sugo database.");
            } else if (version > Database::latestSchemaVersion()) {
                *error = i18n("This backup was created by a newer version of Sugo.");
            } else {
                valid = true;
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return valid;
}

bool DataManager::restoreDatabase(const QUrl &fileUrl)
{
    // The settings page asks for this too, but a timer can start (tray, D-Bus, shortcut) while
    // its file and confirm dialogs are open.
    if (m_timerActive && m_timerActive()) {
        Q_EMIT operationFailed(i18n("Stop the active timer before restoring a backup."));
        return false;
    }
    const QString path = fileUrl.toLocalFile();
    if (path.isEmpty() || !m_database) {
        Q_EMIT operationFailed(i18n("Choose a local backup file."));
        return false;
    }
    if (usingTemporaryDatabase()) {
        // The data file could not be opened: what is restored would be gone when Sugo quits.
        Q_EMIT operationFailed(i18n("The data file could not be opened, so a backup cannot be restored now."));
        return false;
    }
    if (comparablePath(path) == comparablePath(m_database->path())) {
        Q_EMIT operationFailed(i18n("Choose a backup file other than the active database."));
        return false;
    }
    QString error;
    if (!QFileInfo(path).isFile()) {
        Q_EMIT operationFailed(i18n("Could not restore the backup: %1", i18n("This file is not a Sugo database.")));
        return false;
    }

    // Work on a private copy: it is checked there, the Database class brings an older schema up
    // to date there, and the user's files are never written to. The write-ahead log goes with
    // the file: a database in WAL mode (as Sugo's is) keeps its latest changes there until a checkpoint.
    QTemporaryDir workDir;
    const QString copyPath = workDir.filePath(QStringLiteral("restore.db"));
    const QString walSuffix = QStringLiteral("-wal");
    const bool hasWal = QFileInfo(path + walSuffix).isFile();
    if (!workDir.isValid() || !QFile::copy(path, copyPath)
        || (hasWal && !QFile::copy(path + walSuffix, copyPath + walSuffix))) {
        Q_EMIT operationFailed(i18n("Could not read the backup file."));
        return false;
    }
    QFile::setPermissions(copyPath, QFile::ReadOwner | QFile::WriteOwner);
    if (hasWal) {
        QFile::setPermissions(copyPath + walSuffix, QFile::ReadOwner | QFile::WriteOwner);
    }
    if (!validateBackup(copyPath, &error)) {
        Q_EMIT operationFailed(i18n("Could not restore the backup: %1", error));
        return false;
    }
    {
        Database migrated(copyPath);
        if (!migrated.open() || !migrated.movedAsidePath().isEmpty()) {
            // A copy that had to be moved aside was damaged (or, past the checks above, not ours):
            // say so rather than repeat the low-level reason, if any.
            QString reason = migrated.lastError();
            if (!migrated.movedAsidePath().isEmpty()) {
                reason = migrated.movedAsideWasCorrupt() ? i18n("The backup is damaged.")
                                                         : i18n("This file is not a Sugo database.");
            }
            Q_EMIT operationFailed(i18n("Could not restore the backup: %1", reason));
            return false;
        }
    } // closing the connection folds the WAL back into the file before it is attached

    QSqlDatabase db = QSqlDatabase::database(m_database->connectionName(), false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("ATTACH DATABASE ? AS restore_db"));
    query.addBindValue(copyPath);
    if (!query.exec()) {
        Q_EMIT operationFailed(i18n("Could not open the backup: %1", query.lastError().text()));
        return false;
    }
    auto detach = [&db]() {
        QSqlQuery detachQuery(db);
        if (!detachQuery.exec(QStringLiteral("DETACH DATABASE restore_db"))) {
            qCWarning(lcDataManager) << "Cannot detach the restored backup:" << detachQuery.lastError().text();
        }
    };
    if (!db.transaction()) {
        const QString reason = db.lastError().text();
        detach();
        Q_EMIT operationFailed(i18n("Could not open the backup: %1", reason));
        return false;
    }

    // The timers the backup does not have are deleted by the restore, as by the user: Sugo Sync
    // must hear of it, or it brings them back from the other computers. A timer the backup brings
    // back is no longer deleted.
    QStringList statements = {
        QStringLiteral("INSERT INTO main.preset_tombstone (uuid, deleted_at, deleted_by) "
                       "SELECT uuid, %1, '' FROM main.preset WHERE is_builtin = 0 "
                       "AND uuid NOT IN (SELECT uuid FROM restore_db.preset) "
                       "ON CONFLICT(uuid) DO UPDATE SET deleted_at = excluded.deleted_at, deleted_by = excluded.deleted_by")
            .arg(QDateTime::currentMSecsSinceEpoch()),
        QStringLiteral("DELETE FROM main.preset_tombstone WHERE uuid IN (SELECT uuid FROM restore_db.preset)"),
        // A session deleted here and brought back by the backup is no longer deleted: Sugo Sync
        // must not delete it again.
        QStringLiteral("DELETE FROM main.session_tombstone WHERE uuid IN (SELECT uuid FROM restore_db.session)"),
    };
    for (auto it = restoredTables().crbegin(); it != restoredTables().crend(); ++it) {
        const QString where = it->rows.isEmpty() ? QString() : QStringLiteral(" WHERE ") + it->rows;
        statements.append(QStringLiteral("DELETE FROM main.%1%2").arg(it->name, where));
    }
    for (const RestoredTable &table : restoredTables()) {
        const QString where = table.rows.isEmpty() ? QString() : QStringLiteral(" WHERE ") + table.rows;
        statements.append(QStringLiteral("INSERT INTO main.%1 (%2) SELECT %2 FROM restore_db.%1%3")
                              .arg(table.name, table.columns, where));
    }
    bool ok = true;
    for (const QString &statement : std::as_const(statements)) {
        if (!query.exec(statement)) {
            error = query.lastError().text();
            ok = false;
            break;
        }
    }
    // Read the restored timers before committing: if they cannot be read, the model would keep
    // the old ones, and its next save would delete the restored ones.
    QList<TimerPreset> presets;
    QString currentUuid;
    if (ok && m_presets && m_presetsRepository) {
        presets = m_presetsRepository->loadAll(&ok);
        currentUuid = m_presetsRepository->currentUuid();
        if (!ok) {
            error = i18n("The restored timers could not be read.");
        }
    }
    if (ok) {
        ok = db.commit();
        if (!ok) {
            error = db.lastError().text();
            db.rollback();
        }
    } else {
        db.rollback();
    }
    query.finish();
    detach();
    if (!ok) {
        Q_EMIT operationFailed(i18n("Could not restore the backup: %1", error));
        return false;
    }
    if (m_presets && m_presetsRepository) {
        m_presets->setPresets(presets, currentUuid);
        // The model holds the stored timers again: if they could not be read at startup, the
        // changes from now on (the next line's first) are saved.
        if (m_presetPersistence) m_presetPersistence->adoptStored();
        // Restoring is the user's choice: Sugo Sync publishes these timers over newer ones elsewhere.
        m_presets->markAllModified();
    }
    if (m_recorder) m_recorder->forgetHistory();
    if (m_stats) m_stats->refresh();
    Q_EMIT dataChanged();
    Q_EMIT operationSucceeded(i18n("Backup restored."));
    return true;
}

bool DataManager::updateSessionNote(qint64 sessionId, const QString &note)
{
    if (!m_sessions) {
        return false;
    }
    if (SessionRecorder::isUnsavedNoteTarget(sessionId)) {
        // A session that waited to be stored: its row if a retry stored it meanwhile, else the
        // note waits with it and is stored together.
        const qint64 row = m_recorder ? m_recorder->rowFor(sessionId) : -1;
        if (row <= 0) {
            return m_recorder && m_recorder->setUnsavedNote(sessionId, note);
        }
        sessionId = row;
    }
    const bool ok = m_sessions->updateNote(sessionId, note);
    if (ok) {
        if (m_stats) {
            m_stats->refresh();
        }
        Q_EMIT dataChanged();
        Q_EMIT sessionNoteChanged(sessionId, note);
    }
    return ok;
}

QStringList DataManager::recentTaskNotes(int limit) const
{
    if (!m_sessions) {
        return {};
    }
    return m_sessions->recentNotes(limit);
}
