// SPDX-License-Identifier: GPL-3.0-or-later
#include "SessionRepository.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>

Q_LOGGING_CATEGORY(lcSessionRepo, "sugo.db.sessions")

namespace
{
// QSqlQuery binds a null QString (e.g. a default-constructed one) as SQL NULL, which the
// NOT NULL text columns reject. Bind empty text instead.
QString text(const QString &value)
{
    return value.isNull() ? QStringLiteral("") : value;
}
} // namespace

SessionRepository::SessionRepository(const QString &connectionName)
    : m_connection(connectionName)
{
}

QString SessionRepository::kindToString(SessionKind kind)
{
    switch (kind) {
    case SessionKind::Work:
        return QStringLiteral("work");
    case SessionKind::ShortBreak:
        return QStringLiteral("short_break");
    case SessionKind::LongBreak:
        return QStringLiteral("long_break");
    }
    Q_UNREACHABLE_RETURN(QString());
}

SessionKind SessionRepository::kindFromString(const QString &text)
{
    if (text == QLatin1String("short_break")) {
        return SessionKind::ShortBreak;
    }
    if (text == QLatin1String("long_break")) {
        return SessionKind::LongBreak;
    }
    return SessionKind::Work;
}

bool SessionRepository::insert(const SessionRecord &r, qint64 *insertedId)
{
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery query(db);
    // The sub-select links the session to its preset; a missing preset yields NULL.
    query.prepare(QStringLiteral(
        "INSERT INTO session (uuid, kind, started_at, ended_at, duration_sec, planned_sec, completed, "
        "preset_id, preset_name, category, note) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, (SELECT id FROM preset WHERE uuid = ?), ?, ?, ?)"));
    query.addBindValue(r.uuid.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : r.uuid);
    query.addBindValue(kindToString(r.kind));
    query.addBindValue(r.startedAtMs);
    query.addBindValue(r.endedAtMs);
    query.addBindValue(r.durationSec);
    query.addBindValue(r.plannedSec);
    query.addBindValue(r.completed ? 1 : 0);
    query.addBindValue(r.presetUuid);
    query.addBindValue(text(r.presetName));
    query.addBindValue(text(r.category));
    query.addBindValue(text(r.note));
    if (!query.exec()) {
        qCWarning(lcSessionRepo) << "Cannot record session:" << query.lastError().text();
        return false;
    }
    if (insertedId) {
        *insertedId = query.lastInsertId().toLongLong();
    }
    return true;
}

bool SessionRepository::updateNote(qint64 sessionId, const QString &note)
{
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE session SET note = ?, updated_at = ?, updated_by = '' WHERE id = ?"));
    query.addBindValue(text(note));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(sessionId);
    if (!query.exec()) {
        qCWarning(lcSessionRepo) << "Cannot update session note:" << query.lastError().text();
        return false;
    }
    // No such session (e.g. the history was cleared since): the note has nowhere to go.
    if (query.numRowsAffected() < 1) {
        qCWarning(lcSessionRepo) << "Cannot update session note: there is no session" << sessionId;
        return false;
    }
    return true;
}

bool SessionRepository::updateEnd(const SessionRecord &r, QString *note)
{
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE session SET ended_at = ?, duration_sec = ?, planned_sec = ?, completed = ? WHERE id = ?"));
    query.addBindValue(r.endedAtMs);
    query.addBindValue(r.durationSec);
    query.addBindValue(r.plannedSec);
    query.addBindValue(r.completed ? 1 : 0);
    query.addBindValue(r.id);
    if (!query.exec()) {
        qCWarning(lcSessionRepo) << "Cannot update session:" << query.lastError().text();
        return false;
    }
    if (query.numRowsAffected() < 1) {
        qCWarning(lcSessionRepo) << "Cannot update session: there is no session" << r.id;
        return false;
    }
    if (note) {
        QSqlQuery read(db);
        read.prepare(QStringLiteral("SELECT note FROM session WHERE id = ?"));
        read.addBindValue(r.id);
        *note = read.exec() && read.next() ? read.value(0).toString() : QString();
    }
    return true;
}

QStringList SessionRepository::recentNotes(int limit) const
{
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery query(db);
    // SQLite's TRIM() only strips spaces, so notes are trimmed and de-duplicated here, the way
    // the statistics group them (QString::trimmed()). Newest first: each note takes the place of
    // its most recent use, and reading stops once `limit` distinct notes are found.
    query.setForwardOnly(true);
    query.prepare(QStringLiteral("SELECT note FROM session WHERE note <> '' ORDER BY id DESC"));
    QStringList result;
    if (limit <= 0) {
        return result;
    }
    if (!query.exec()) {
        qCWarning(lcSessionRepo) << query.lastError().text();
        return result;
    }
    QSet<QString> seen;
    while (result.size() < limit && query.next()) {
        const QString note = query.value(0).toString().trimmed();
        if (!note.isEmpty() && !seen.contains(note)) {
            seen.insert(note);
            result.append(note);
        }
    }
    return result;
}

QList<SessionRecord> SessionRepository::between(qint64 fromMs, qint64 toMs, bool *ok) const
{
    if (ok) {
        *ok = false;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT s.id, s.uuid, s.kind, s.started_at, s.ended_at, s.duration_sec, s.planned_sec, s.completed, "
        "COALESCE(p.uuid, ''), s.preset_name, s.category, s.note "
        "FROM session s LEFT JOIN preset p ON p.id = s.preset_id "
        "WHERE s.started_at >= ? AND s.started_at < ? ORDER BY s.started_at, s.id"));
    query.addBindValue(fromMs);
    query.addBindValue(toMs);

    QList<SessionRecord> result;
    if (!query.exec()) {
        qCWarning(lcSessionRepo) << query.lastError().text();
        return result;
    }
    while (query.next()) {
        SessionRecord r;
        r.id = query.value(0).toLongLong();
        r.uuid = query.value(1).toString();
        r.kind = kindFromString(query.value(2).toString());
        r.startedAtMs = query.value(3).toLongLong();
        r.endedAtMs = query.value(4).toLongLong();
        r.durationSec = query.value(5).toInt();
        r.plannedSec = query.value(6).toInt();
        r.completed = query.value(7).toBool();
        r.presetUuid = query.value(8).toString();
        r.presetName = query.value(9).toString();
        r.category = query.value(10).toString();
        r.note = query.value(11).toString();
        result.append(r);
    }
    // next() also ends the loop on an error (an I/O error or a damaged page met halfway).
    if (query.lastError().isValid()) {
        qCWarning(lcSessionRepo) << query.lastError().text();
        return result;
    }
    if (ok) {
        *ok = true;
    }
    return result;
}

int SessionRepository::count() const
{
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery query(db);
    if (query.exec(QStringLiteral("SELECT COUNT(*) FROM session")) && query.next()) {
        return query.value(0).toInt();
    }
    return -1;
}

bool SessionRepository::deleteAll()
{
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("DELETE FROM session"))) {
        qCWarning(lcSessionRepo) << "Cannot clear session history:" << query.lastError().text();
        return false;
    }
    return true;
}

bool SessionRepository::deleteById(qint64 sessionId, SessionRecord *deleted)
{
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery read(db);
    read.prepare(QStringLiteral("SELECT uuid, kind, started_at, ended_at, completed FROM session WHERE id = ?"));
    read.addBindValue(sessionId);
    if (!read.exec()) {
        qCWarning(lcSessionRepo) << "Cannot delete session:" << read.lastError().text();
        return false;
    }
    if (!read.next()) {
        qCWarning(lcSessionRepo) << "Cannot delete session: there is no session" << sessionId;
        return false;
    }
    SessionRecord record;
    record.id = sessionId;
    record.uuid = read.value(0).toString();
    record.kind = kindFromString(read.value(1).toString());
    record.startedAtMs = read.value(2).toLongLong();
    record.endedAtMs = read.value(3).toLongLong();
    record.completed = read.value(4).toBool();
    read.finish();

    // The row and its tombstone go together, or neither.
    if (!db.transaction()) {
        qCWarning(lcSessionRepo) << "Cannot delete session:" << db.lastError().text();
        return false;
    }
    QSqlQuery query(db);
    bool ok = true;
    if (!record.uuid.isEmpty()) {
        query.prepare(QStringLiteral("INSERT OR REPLACE INTO session_tombstone (uuid, deleted_at, deleted_by) VALUES (?, ?, '')"));
        query.addBindValue(record.uuid);
        query.addBindValue(QDateTime::currentMSecsSinceEpoch());
        ok = query.exec();
    }
    if (ok) {
        query.prepare(QStringLiteral("DELETE FROM session WHERE id = ?"));
        query.addBindValue(sessionId);
        ok = query.exec() && query.numRowsAffected() == 1;
    }
    if (!ok || !db.commit()) {
        qCWarning(lcSessionRepo) << "Cannot delete session:" << query.lastError().text() << db.lastError().text();
        db.rollback();
        return false;
    }
    if (deleted) {
        *deleted = record;
    }
    return true;
}

int SessionRepository::deleteStartedBefore(qint64 ms)
{
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM session WHERE started_at < ?"));
    query.addBindValue(ms);
    if (!query.exec()) {
        qCWarning(lcSessionRepo) << "Cannot delete old sessions:" << query.lastError().text();
        return -1;
    }
    return query.numRowsAffected();
}

QList<qint64> SessionRepository::completedWorkStartsAfter(qint64 afterId, qint64 *maxId) const
{
    if (maxId) {
        *maxId = afterId;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT id, started_at FROM session WHERE kind = 'work' AND completed = 1 AND id > ? "
                                 "ORDER BY started_at, id"));
    query.addBindValue(afterId);
    QList<qint64> result;
    if (!query.exec()) {
        qCWarning(lcSessionRepo) << query.lastError().text();
        return result;
    }
    qint64 highest = afterId;
    while (query.next()) {
        highest = std::max(highest, query.value(0).toLongLong());
        result.append(query.value(1).toLongLong());
    }
    // A row that cannot be read (a damaged page, an I/O error) ends the loop early. The rows are
    // in start order, not id order, so `highest` may already be past rows never read: keep the
    // cursor where it was and return nothing, so the next call reads them again.
    if (query.lastError().isValid()) {
        qCWarning(lcSessionRepo) << "Cannot read the completed work sessions:" << query.lastError().text();
        return {};
    }
    if (maxId) {
        *maxId = highest;
    }
    return result;
}
