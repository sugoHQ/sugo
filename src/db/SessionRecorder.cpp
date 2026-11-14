// SPDX-License-Identifier: GPL-3.0-or-later
#include "SessionRecorder.h"

#include <QCoreApplication>
#include <QLoggingCategory>

#include <algorithm>
#include <utility>

#include "PresetModel.h"
#include "SessionRepository.h"

Q_LOGGING_CATEGORY(lcSessionRecorder, "sugo.db.sessions")

namespace
{
SessionKind kindFor(TimerEngine::Phase phase)
{
    switch (phase) {
    case TimerEngine::Phase::Work:
        return SessionKind::Work;
    case TimerEngine::Phase::ShortBreak:
        return SessionKind::ShortBreak;
    case TimerEngine::Phase::LongBreak:
        return SessionKind::LongBreak;
    }
    Q_UNREACHABLE_RETURN(SessionKind::Work);
}
} // namespace

SessionRecorder::SessionRecorder(TimerEngine *engine, PresetModel *presets, SessionRepository *repository, QObject *parent)
    : QObject(parent)
    , m_presets(presets)
    , m_repository(repository)
    , m_active(presets->currentPreset())
    , m_lastEndedTimer(m_active)
{
    connect(engine, &TimerEngine::phaseStarted, this, &SessionRecorder::onPhaseStarted);
    connect(engine, &TimerEngine::sessionEnded, this, &SessionRecorder::onSessionEnded);
    connect(engine, &TimerEngine::phaseReopened, this, &SessionRecorder::onPhaseReopened);
    // A phase of an earlier run, put back: the timer it ran with is the current one again.
    connect(engine, &TimerEngine::phaseRestored, this, &SessionRecorder::onPhaseStarted);
    m_retryTimer.setSingleShot(true);
    m_retryTimer.setInterval(60 * 1000);
    connect(&m_retryTimer, &QTimer::timeout, this, &SessionRecorder::storeUnsaved);
}

bool SessionRecorder::store(const SessionRecord &record)
{
    if (record.id > 0) {
        // A phase taken up again: its row gets the new end.
        SessionRecord updated = record;
        if (!m_repository->updateEnd(updated, &updated.note)) {
            return false;
        }
        Q_EMIT sessionRecorded();
        if (updated.kind == SessionKind::Work && updated.completed) {
            Q_EMIT workSessionRecorded(updated);
        }
        return true;
    }
    qint64 id = -1;
    if (!m_repository->insert(record, &id)) {
        return false;
    }
    if (record.kind == SessionKind::Work && record.startedAtMs > 1) {
        m_storedLate.insert(record.startedAtMs, id);
    }
    // The phase that ended last, stored only now by a retry: a phase taken up again updates this row.
    if (record.startedAtMs == m_lastEndedStartMs && record.kind == m_lastEndedKind) {
        m_lastSessionId = id;
        if (record.kind == SessionKind::Work) {
            m_lastWorkSessionId = id; // its note goes to this row now
        }
        if (m_reopenedUnsaved) {
            m_reopenedUnsaved = false;
            m_reopenedId = id;
        }
    }
    Q_EMIT sessionRecorded();
    if (record.kind == SessionKind::Work && record.completed) {
        SessionRecord stored = record;
        stored.id = id;
        Q_EMIT workSessionRecorded(stored);
    }
    return true;
}

qint64 SessionRecorder::noteTargetId() const
{
    if (m_lastWorkSessionId > 0) {
        return m_lastWorkSessionId;
    }
    if (m_lastWorkStartMs > 1) {
        for (const SessionRecord &r : m_unsaved) {
            if (r.id <= 0 && r.kind == SessionKind::Work && r.startedAtMs == m_lastWorkStartMs) {
                return -m_lastWorkStartMs; // its start stands for it until it has a row
            }
        }
    }
    return -1;
}

qint64 SessionRecorder::rowFor(qint64 target) const
{
    if (!isUnsavedNoteTarget(target)) {
        return target > 0 ? target : -1;
    }
    return m_storedLate.value(-target, -1);
}

bool SessionRecorder::setUnsavedNote(qint64 target, const QString &note)
{
    if (!isUnsavedNoteTarget(target)) {
        return false;
    }
    for (SessionRecord &r : m_unsaved) {
        if (r.id <= 0 && r.kind == SessionKind::Work && r.startedAtMs == -target) {
            r.note = note;
            return true;
        }
    }
    return false;
}

void SessionRecorder::storeEnded(const SessionRecord &record, qint64 activeMs)
{
    if (!record.completed && activeMs < kMinInterruptedMs) {
        return;
    }
    storeUnsaved();
    if (isSaveFailing() || !store(record)) {
        const bool wasFailing = isSaveFailing();
        m_unsaved.append(record);
        m_retryTimer.start();
        if (!wasFailing) {
            Q_EMIT saveFailingChanged();
        }
    }
}

void SessionRecorder::storeUnsaved()
{
    const bool wasFailing = isSaveFailing();
    while (!m_unsaved.isEmpty() && store(m_unsaved.first())) {
        m_unsaved.removeFirst();
    }
    if (isSaveFailing()) {
        m_retryTimer.start();
    } else if (wasFailing) {
        Q_EMIT saveFailingChanged();
    }
}

void SessionRecorder::forgetHistory()
{
    m_lastWorkSessionId = -1;
    m_lastSessionId = -1;
    m_reopenedId = 0;
    m_reopenedUnsaved = false;
    m_lastEndedStartMs = -1;
    m_lastWorkStartMs = -1;
    m_storedLate.clear();
    m_retryTimer.stop();
    if (isSaveFailing()) {
        m_unsaved.clear();
        Q_EMIT saveFailingChanged();
    }
}

void SessionRecorder::forgetSession(qint64 sessionId)
{
    if (sessionId <= 0) {
        return;
    }
    if (m_lastWorkSessionId == sessionId) {
        m_lastWorkSessionId = -1;
    }
    if (m_lastSessionId == sessionId) {
        m_lastSessionId = -1;
    }
    if (m_reopenedId == sessionId) {
        m_reopenedId = 0;
    }
    m_storedLate.removeIf([sessionId](const auto &it) { return it.value() == sessionId; });
    // A new end waiting for the deleted row would be refused forever.
    const bool wasFailing = isSaveFailing();
    m_unsaved.removeIf([sessionId](const SessionRecord &r) { return r.id == sessionId; });
    if (wasFailing && !isSaveFailing()) {
        m_retryTimer.stop();
        Q_EMIT saveFailingChanged();
    }
}

void SessionRecorder::onPhaseStarted(TimerEngine::Phase)
{
    m_active = m_presets->currentPreset();
}

void SessionRecorder::onPhaseReopened(TimerEngine::Phase)
{
    // The phase that started by itself meanwhile took the timer as it is now: the session goes on
    // under the name and category it was stored with.
    m_active = m_lastEndedTimer;
    m_reopenedId = m_lastSessionId > 0 ? m_lastSessionId : 0;
    m_reopenedUnsaved = m_reopenedId == 0 && isSaveFailing();
}

void SessionRecorder::onSessionEnded(TimerEngine::Phase phase,
                                     const QDateTime &startedAt,
                                     const QDateTime &endedAt,
                                     qint64 activeMs,
                                     qint64 plannedMs,
                                     bool completed)
{
    const bool isWork = phase == TimerEngine::Phase::Work;
    const qint64 reopenedId = std::exchange(m_reopenedId, 0);
    const bool reopenedUnsaved = std::exchange(m_reopenedUnsaved, false);
    if (!completed && activeMs < kMinInterruptedMs) {
        if (isWork) {
            m_lastWorkSessionId = -1;
            m_lastWorkStartMs = -1;
        }
        m_lastSessionId = -1;
        return;
    }

    SessionRecord record;
    record.kind = kindFor(phase);
    record.startedAtMs = startedAt.toMSecsSinceEpoch();
    record.endedAtMs = endedAt.toMSecsSinceEpoch();
    record.durationSec = int((activeMs + 500) / 1000);
    record.plannedSec = int((plannedMs + 500) / 1000);
    record.completed = completed;
    record.presetUuid = m_active.uuid;
    record.presetName = m_active.name;
    record.category = m_active.category;
    if (isWork) {
        m_lastWorkStartMs = record.startedAtMs;
    }
    if (reopenedId <= 0) {
        m_lastEndedStartMs = record.startedAtMs;
        m_lastEndedKind = record.kind;
        m_lastEndedTimer = m_active;
    }

    if (reopenedId > 0) {
        // The same session, longer: its row gets the new end, never a new row (that would count
        // the session twice). If the database refuses it, the new end waits with the unsaved
        // sessions and is retried, after them, so no extra time is lost.
        record.id = reopenedId;
        m_lastSessionId = reopenedId;
        if (isWork) {
            m_lastWorkSessionId = reopenedId;
        }
        storeUnsaved();
        if (isSaveFailing() || !store(record)) {
            const bool wasFailing = isSaveFailing();
            const auto same = std::find_if(m_unsaved.begin(), m_unsaved.end(), [reopenedId](const SessionRecord &r) { return r.id == reopenedId; });
            if (same != m_unsaved.end()) {
                *same = record; // the latest end of that row is the one to store
            } else {
                m_unsaved.append(record);
            }
            m_retryTimer.start();
            if (!wasFailing) {
                Q_EMIT saveFailingChanged();
            }
        }
        return;
    }
    if (reopenedUnsaved && !m_unsaved.isEmpty() && m_unsaved.last().startedAtMs == record.startedAtMs) {
        record.note = m_unsaved.last().note; // a note already written for it stays
        m_unsaved.last() = record;           // not stored yet: it will be, with the new end
        return;
    }

    // Older sessions that could not be stored go first, so the history stays in order.
    storeUnsaved();
    qint64 id = -1;
    const bool inserted = !isSaveFailing() && m_repository->insert(record, &id);
    if (isWork) {
        m_lastWorkSessionId = inserted ? id : -1;
    }
    m_lastSessionId = inserted ? id : -1;
    if (inserted) {
        Q_EMIT sessionRecorded();
        if (isWork && completed) {
            record.id = id;
            Q_EMIT workSessionRecorded(record);
        }
        return;
    }
    const bool wasFailing = isSaveFailing();
    m_unsaved.append(record);
    m_retryTimer.start();
    if (!wasFailing) {
        Q_EMIT saveFailingChanged();
    }
}

void SessionRecorder::connectQuit(QCoreApplication *app, TimerEngine *engine, SessionRecorder *recorder,
                                  std::function<bool()> keep)
{
    QObject::connect(app, &QCoreApplication::aboutToQuit, engine, [engine, keep = std::move(keep)]() {
        if (!keep || !keep()) {
            engine->stop();
        }
    });
    QObject::connect(app, &QCoreApplication::aboutToQuit, recorder, &SessionRecorder::storeBeforeQuit);
}

void SessionRecorder::storeBeforeQuit()
{
    m_retryTimer.stop();
    if (!isSaveFailing()) {
        return;
    }
    storeUnsaved();
    m_retryTimer.stop(); // storeUnsaved() starts it again on a failure; there is no later now
    if (!isSaveFailing()) {
        return;
    }
    qCWarning(lcSessionRecorder).nospace() << "Quitting with " << m_unsaved.size()
                                           << " sessions that could not be stored; they are lost:";
    for (const SessionRecord &record : std::as_const(m_unsaved)) {
        qCWarning(lcSessionRecorder).nospace().noquote()
            << "  " << SessionRepository::kindToString(record.kind) << " from "
            << QDateTime::fromMSecsSinceEpoch(record.startedAtMs).toString(Qt::ISODate) << ", " << record.durationSec << " s"
            << (record.completed ? "" : ", interrupted");
    }
}
