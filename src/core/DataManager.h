// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QObject>
#include <QUrl>

#include <functional>

#include "SessionRepository.h"

class Database;
class PresetModel;
class PresetPersistence;
class PresetRepository;
class SessionRecorder;
class StatsModel;

/// Backup, restore, export and clear the user's recorded timer sessions.
class DataManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString databasePath READ databasePath CONSTANT FINAL)
    Q_PROPERTY(int sessionCount READ sessionCount NOTIFY dataChanged FINAL)
    /// True when running on an in-memory database (the file could not be used): nothing is saved.
    Q_PROPERTY(bool usingTemporaryDatabase READ usingTemporaryDatabase CONSTANT FINAL)
    /// True while finished sessions or timer changes could not be stored (they are retried).
    Q_PROPERTY(bool saveFailing READ saveFailing NOTIFY saveFailingChanged FINAL)
    /// Where a damaged or foreign database found at startup was moved to (a new one was started); empty if none was.
    Q_PROPERTY(QString movedAsideDatabasePath READ movedAsideDatabasePath CONSTANT FINAL)
    /// True if that file was damaged, false if it belonged to another program.
    Q_PROPERTY(bool movedAsideDatabaseWasCorrupt READ movedAsideDatabaseWasCorrupt CONSTANT FINAL)
    /// Where the empty database was kept when the timers and history of Sugo 1.0.0, which an
    /// earlier version had moved aside, took its place at startup; empty if that did not happen.
    Q_PROPERTY(QString sugo100RecoveryReplacedPath READ sugo100RecoveryReplacedPath CONSTANT FINAL)

public:
    DataManager(Database *database, SessionRepository *sessions, PresetRepository *presetsRepository,
                PresetModel *presets, StatsModel *stats, QObject *parent = nullptr);
    QString databasePath() const;
    int sessionCount() const;
    int presetCount() const;
    bool usingTemporaryDatabase() const;
    bool saveFailing() const;
    QString movedAsideDatabasePath() const { return m_movedAsidePath; }
    bool movedAsideDatabaseWasCorrupt() const { return m_movedAsideCorrupt; }
    QString sugo100RecoveryReplacedPath() const { return m_sugo100ReplacedPath; }
    /// Taken from the database at construction; main() sets it when that database was replaced
    /// by a temporary one after the move. Call before the interface reads it.
    void setMovedAsideDatabase(const QString &path, bool corrupt);

    /// Replaces the clock (local time) used for "older than" cut-offs; for tests.
    void setNowProvider(std::function<QDateTime()> provider);
    /// Tells whether a timer is running or paused: a backup is not restored then.
    void setTimerActiveProvider(std::function<bool()> provider);

    /// Tracks newly recorded sessions: keeps sessionCount current and provides lastWorkSessionId().
    void setSessionRecorder(SessionRecorder *recorder);
    /// Reports failed timer saves through saveFailing.
    void setPresetPersistence(PresetPersistence *persistence);
    /// Row id of the most recently ended Work phase, or -1 if it was not recorded or there is no recorder.
    Q_INVOKABLE qint64 lastWorkSessionId() const;

    Q_INVOKABLE bool clearHistory();
    /// Deletes the sessions that started more than `months` months ago. Removing none is a success.
    Q_INVOKABLE bool clearHistoryOlderThan(int months);
    /// Deletes one session (a phase started by mistake, say) and updates the statistics.
    /// `fromCalendar`: its event goes from the calendar too (sessionDeleted()).
    Q_INVOKABLE bool deleteSession(qint64 sessionId, bool fromCalendar = false);
    /// Writes every session to a .csv or .json file (chosen by the file's extension).
    Q_INVOKABLE bool exportHistory(const QUrl &fileUrl);
    /// Opens the folder holding the database in the file manager.
    Q_INVOKABLE void openDatabaseFolder() const;
    Q_INVOKABLE bool backupDatabase(const QUrl &fileUrl);
    Q_INVOKABLE bool restoreDatabase(const QUrl &fileUrl);
    Q_INVOKABLE bool updateSessionNote(qint64 sessionId, const QString &note);
    Q_INVOKABLE QStringList recentTaskNotes(int limit = 5) const;

Q_SIGNALS:
    void operationFailed(const QString &message);
    void operationSucceeded(const QString &message);
    void dataChanged();
    void saveFailingChanged();
    /// A session note was stored (also emitted for an empty note).
    void sessionNoteChanged(qint64 sessionId, const QString &note);
    /// A session was deleted (deleteSession()); `fromCalendar`: the user wants its event gone too.
    void sessionDeleted(const SessionRecord &record, bool fromCalendar);

private:
    bool validateBackup(const QString &path, QString *error) const;
    Database *m_database;
    SessionRepository *m_sessions;
    PresetRepository *m_presetsRepository;
    PresetModel *m_presets;
    StatsModel *m_stats;
    QString m_movedAsidePath;
    bool m_movedAsideCorrupt = false;
    QString m_sugo100ReplacedPath;
    SessionRecorder *m_recorder = nullptr;
    PresetPersistence *m_presetPersistence = nullptr;
    std::function<QDateTime()> m_now;
    std::function<bool()> m_timerActive;
};
