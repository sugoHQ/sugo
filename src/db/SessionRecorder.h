// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <functional>
#include <QList>
#include <QObject>
#include <QTimer>

#include "TimerEngine.h"
#include "SessionRepository.h"
#include "TimerPreset.h"

class PresetModel;
class QCoreApplication;

/**
 * Stores every finished phase of the TimerEngine.
 *
 * The preset is captured when a phase *starts*, not when it ends, so a session is
 * always attributed to the timer it ran with even if the user switches timers
 * (which stops the running phase) or edits the preset afterwards.
 *
 * Phases cut short by stop/skip are kept only if they lasted at least
 * kMinInterruptedMs, so accidental starts do not clutter the statistics.
 *
 * A session that cannot be stored (database busy, disk full...) is not dropped: it is kept in
 * memory and stored again at the next phase end or retry, and isSaveFailing() says so meanwhile.
 * Quitting tries once more (see connectQuit()).
 *
 * A phase taken up again after it ended (TimerEngine::phaseReopened) is the same session: when it
 * ends, the row stored for it is updated instead of a new one added, and workSessionRecorded()
 * comes again with the same id (the calendar replaces its event).
 */
class SessionRecorder : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 kMinInterruptedMs = TimerEngine::kMinInterruptedMs;

    SessionRecorder(TimerEngine *engine, PresetModel *presets, SessionRepository *repository, QObject *parent = nullptr);

    /// Row id of the most recently ended Work phase, or -1 if that phase was not recorded
    /// (too short, or the insert failed) or no Work phase has ended yet. Breaks leave it as is,
    /// so a note typed during the following break still lands on the right session.
    qint64 lastWorkSessionId() const { return m_lastWorkSessionId; }

    /// Called when the stored history is replaced (cleared or restored). Forgets that id, so a
    /// later note cannot land on an unrelated row that happens to reuse the id, and drops the
    /// sessions still waiting to be stored, so they do not reappear in the replaced history.
    void forgetHistory();

    /// The running phase is one an earlier run began with the timer then named `name` (SessionKeeper):
    /// it is recorded with that name and category, not the timer's current ones.
    void setActiveTimer(const QString &name, const QString &category)
    {
        m_active.name = name;
        m_active.category = category;
    }

    /// Stores a session that ended outside the timer (SessionKeeper: one left by an earlier run),
    /// under the same rules as the timer's: kept if it ran at least kMinInterruptedMs (`activeMs`).
    void storeEnded(const SessionRecord &record, qint64 activeMs);

    /// True while some ended sessions could not be stored yet.
    bool isSaveFailing() const { return !m_unsaved.isEmpty(); }
    void setRetryIntervalForTesting(int ms) { m_retryTimer.setInterval(ms); }

    /// Connects quitting in the order that loses nothing: the engine stops first, so a running
    /// phase is ended as an interrupted session, unless `keep` says it was kept for the next start
    /// (SessionKeeper); then the recorder tries once more to store the sessions still waiting (with
    /// the timer idle, no phase end would retry them) and logs those it still cannot store. Both are
    /// direct connections, called in the order they are made.
    static void connectQuit(QCoreApplication *app, TimerEngine *engine, SessionRecorder *recorder,
                            std::function<bool()> keep = {});

public Q_SLOTS:
    /// The last try at quit; see connectQuit().
    void storeBeforeQuit();

Q_SIGNALS:
    void sessionRecorded();
    /// A Work phase ran to its end and was stored; record.id is its new row id.
    void workSessionRecorded(const SessionRecord &record);
    void saveFailingChanged();

private Q_SLOTS:
    void onPhaseStarted(TimerEngine::Phase phase);
    void onPhaseReopened(TimerEngine::Phase phase);
    void onSessionEnded(TimerEngine::Phase phase,
                        const QDateTime &startedAt,
                        const QDateTime &endedAt,
                        qint64 activeMs,
                        qint64 plannedMs,
                        bool completed);

private:
    bool store(const SessionRecord &record);
    void storeUnsaved();

    PresetModel *m_presets;
    SessionRepository *m_repository;
    TimerPreset m_active; ///< Preset captured at the start of the running phase.
    qint64 m_lastWorkSessionId = -1;
    qint64 m_lastSessionId = -1;     ///< Row id of the phase that ended last, of any kind; -1 if not stored.
    qint64 m_reopenedId = 0;         ///< Row the next ended phase updates (> 0), or 0.
    bool m_reopenedUnsaved = false;  ///< The next ended phase replaces the last unsaved one instead.
    qint64 m_lastEndedStartMs = -1;  ///< Start of the phase that ended last, to know its row once a retry stores it.
    SessionKind m_lastEndedKind = SessionKind::Work;
    QList<SessionRecord> m_unsaved; ///< Ended sessions the database refused, oldest first.
    QTimer m_retryTimer;
};
