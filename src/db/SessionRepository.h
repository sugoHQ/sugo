// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QMetaType>
#include <QString>

enum class SessionKind {
    Work,
    ShortBreak,
    LongBreak,
};

/// One finished (or interrupted) phase, as stored. Times are Unix milliseconds, UTC.
struct SessionRecord {
    qint64 id = 0;
    QString uuid;        ///< The same on every device. insert() makes one when it is empty.
    SessionKind kind = SessionKind::Work;
    qint64 startedAtMs = 0;
    qint64 endedAtMs = 0;
    int durationSec = 0; ///< Active time, excluding pauses.
    int plannedSec = 0;
    bool completed = false;
    QString presetUuid;  ///< Used to link to the preset; empty or unknown links to nothing.
    QString presetName;  ///< Snapshot, so statistics survive renames and deletion.
    QString category;    ///< Snapshot.
    QString note;        ///< User task note or activity summary.
};
Q_DECLARE_METATYPE(SessionRecord)

class SessionRepository
{
public:
    explicit SessionRepository(const QString &connectionName);

    /// Returns false (and logs) on failure, also when a session with the record's uuid exists.
    /// On success `insertedId` (if given) receives the row id.
    bool insert(const SessionRecord &record, qint64 *insertedId = nullptr);

    /// Updates the task note for an existing session and marks it changed now (updated_at).
    /// Returns false (and logs) on failure or when there is no session with that id.
    bool updateNote(qint64 sessionId, const QString &note);

    /// Gives the session `record.id` the end, active and planned time and completion of `record`
    /// (a phase taken up again after it ended); its start, timer and note stay. On success `note`
    /// (if given) receives the session's note. Returns false (and logs) on failure or when there
    /// is no session with that id.
    bool updateEnd(const SessionRecord &record, QString *note = nullptr);

    /// Recent unique non-empty task notes (trimmed), most recently used first.
    QStringList recentNotes(int limit = 5) const;

    /// Sessions that started in [fromMs, toMs), oldest first. On a read error the list is empty
    /// (or cut short) and `ok` (if given) is false.
    QList<SessionRecord> between(qint64 fromMs, qint64 toMs, bool *ok = nullptr) const;

    /// Started timestamps (Unix ms) of completed work sessions with an id greater than `afterId`,
    /// oldest first, so callers can keep a cache up to date. `maxId` (if given) receives the highest id seen, or
    /// `afterId` when there are no new rows, or when they cannot be read (then the list is empty).
    QList<qint64> completedWorkStartsAfter(qint64 afterId, qint64 *maxId = nullptr) const;

    /// The number of sessions, or -1 if it cannot be read.
    int count() const;
    bool deleteAll();
    /// Deletes the session `sessionId` and leaves a tombstone of its uuid, so Sugo Sync can tell
    /// the other devices. On success `deleted` (if given) receives the session as it was (its id,
    /// uuid, kind, start and end, completion). Returns false (and logs) on failure or when there
    /// is no session with that id; then nothing changes.
    bool deleteById(qint64 sessionId, SessionRecord *deleted = nullptr);
    /// Deletes the sessions that started before `ms` (Unix ms). Returns how many rows were
    /// deleted, or -1 on error.
    int deleteStartedBefore(qint64 ms);

    static QString kindToString(SessionKind kind);
    static SessionKind kindFromString(const QString &text);

private:
    QString m_connection;
};
