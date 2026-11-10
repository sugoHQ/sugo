// SPDX-License-Identifier: GPL-3.0-or-later
#include "SessionRecorder.h"

#include <QCoreApplication>
#include <QLoggingCategory>

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
    qint64 id = -1;
    if (!m_repository->insert(record, &id)) {
        return false;
    }
    // The phase that ended last, stored only now by a retry: a phase taken up again updates this row.
    if (record.startedAtMs == m_lastEndedStartMs && record.kind == m_lastEndedKind) {
        m_lastSessionId = id;
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
    m_retryTimer.stop();
    if (isSaveFailing()) {
        m_unsaved.clear();
        Q_EMIT saveFailingChanged();
    }
}

void SessionRecorder::onPhaseStarted(TimerEngine::Phase)
{
    m_active = m_presets->currentPreset();
}

void SessionRecorder::onPhaseReopened(TimerEngine::Phase)
{
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
    if (reopenedId <= 0) {
        m_lastEndedStartMs = record.startedAtMs;
        m_lastEndedKind = record.kind;
    }

    if (reopenedId > 0) {
        // The same session, longer: its row gets the new end. If that fails, the row keeps the
        // time it had (a new row would count the session twice).
        record.id = reopenedId;
        if (m_repository->updateEnd(record, &record.note)) {
            m_lastSessionId = reopenedId;
            Q_EMIT sessionRecorded();
            if (isWork && completed) {
                Q_EMIT workSessionRecorded(record);
            }
        }
        return;
    }
    if (reopenedUnsaved && !m_unsaved.isEmpty() && m_unsaved.last().startedAtMs == record.startedAtMs) {
        m_unsaved.last() = record; // not stored yet: it will be, with the new end
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
