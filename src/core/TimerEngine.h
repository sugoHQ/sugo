// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QObject>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <memory>

#include "Clock.h"

/**
 * Pomodoro state machine.
 *
 * Accuracy: the remaining time is always `deadline - clock.now()`. The internal
 * QTimer only drives UI refreshes and phase-end detection, so a delayed or
 * skipped tick (minimized window, system load) never makes the countdown drift.
 *
 * States and transitions
 * ----------------------
 *   Idle        --start-->            Working | ShortBreak | LongBreak   (the pending phase())
 *   running     --pause-->            Paused          (remembers its phase and remaining time)
 *   Paused      --resume-->           the phase it was paused from
 *   any active  --stop-->             Idle, phase = Work, cycles reset   (session saved as interrupted)
 *   any active  --skip-->             next phase, started immediately    (session saved as interrupted;
 *                                     a skipped Work phase still counts as a cycle)
 *   Idle        --skip-->             Idle, pending phase advanced       (nothing recorded; a pending break
 *                                     becomes a pending Work phase, a pending Work phase becomes a
 *                                     short break without counting a cycle: no work was done)
 *   Working     --time elapsed-->     ShortBreak, or LongBreak once `cyclesBeforeLong` work phases completed
 *   ShortBreak  --time elapsed-->     Working
 *   LongBreak   --time elapsed-->     Working, cycles reset
 *
 * When a phase elapses, the next phase starts by itself if the auto-start mode allows it;
 * otherwise the engine waits in Idle with phase() set to the upcoming phase. An auto-started
 * transition goes straight from one running state to the next (no Idle in between), and
 * phaseFinished() is emitted after the transition, so listeners see the real new state.
 * A long break with a zero duration still ends the set, with a short break in its place. A
 * break with a zero duration is skipped: the upcoming phase is Work right away. Skipping a
 * work phase counts as a cycle just like finishing it, so skipping through a set reaches the
 * long break.
 *
 * More time
 * ---------
 *   any active  --extend-->           the same phase, `seconds` longer (also while paused)
 *   a phase that has just elapsed  --continueFinished-->  that phase again, for `seconds` more
 * A phase that elapsed can be taken up again for kReopenWindowMs, until any other command: the
 * phase auto-started after it, if any, gives way without being recorded, the cycle count goes back
 * to what it was, and the phase goes on as the same session (same start, the time in between
 * counted like a pause). phaseReopened() tells SessionRecorder to update the session it stored
 * instead of adding another. A reopened phase cut short by stop/skip still counts as completed.
 */
class TimerEngine : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    // State and Phase share enumerator names: expose them only as TimerEngine.State.X / .Phase.X.
    Q_CLASSINFO("RegisterEnumClassesUnscoped", "false")

public:
    enum class State {
        Idle,
        Working,
        ShortBreak,
        LongBreak,
        Paused,
    };
    Q_ENUM(State)

    /// Kind of phase. While Idle it is the phase that start() will begin;
    /// while Paused it is the phase that was paused.
    enum class Phase {
        Work,
        ShortBreak,
        LongBreak,
    };
    Q_ENUM(Phase)

    Q_PROPERTY(State state READ state NOTIFY stateChanged FINAL)
    Q_PROPERTY(Phase phase READ phase NOTIFY phaseChanged FINAL)
    /// True while a phase is running or paused (i.e. not Idle).
    Q_PROPERTY(bool active READ isActive NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool paused READ isPaused NOTIFY stateChanged FINAL)

    Q_PROPERTY(int remainingSeconds READ remainingSeconds NOTIFY remainingChanged FINAL)
    Q_PROPERTY(int totalSeconds READ totalSeconds NOTIFY totalChanged FINAL)
    /// Elapsed fraction of the current phase, 0..1.
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged FINAL)
    /// Work phases completed in the current set (towards the long break).
    Q_PROPERTY(int completedCycles READ completedCycles NOTIFY cyclesChanged FINAL)

    Q_PROPERTY(int workSeconds READ workSeconds WRITE setWorkSeconds NOTIFY workSecondsChanged FINAL)
    Q_PROPERTY(int shortBreakSeconds READ shortBreakSeconds WRITE setShortBreakSeconds NOTIFY shortBreakSecondsChanged FINAL)
    Q_PROPERTY(int longBreakSeconds READ longBreakSeconds WRITE setLongBreakSeconds NOTIFY longBreakSecondsChanged FINAL)
    enum class AutoStartMode {
        Manual = 0,
        BreaksOnly = 1,
        All = 2
    };
    Q_ENUM(AutoStartMode)

    /// Who asked for more time (for the usage statistics).
    enum class ExtendSource {
        App = 0,
        Notification = 1,
        Menu = 2,
        Command = 3
    };
    Q_ENUM(ExtendSource)

    /// True while extend() can add time: a phase is active, or one has just elapsed (see continueFinished()).
    Q_PROPERTY(bool extendable READ isExtendable NOTIFY extendableChanged FINAL)
    /// True while the phase that has just elapsed can be taken up again (continueFinished()).
    Q_PROPERTY(bool continuable READ isContinuable NOTIFY extendableChanged FINAL)
    /// The phase continueFinished() would take up again (valid while `continuable`).
    Q_PROPERTY(Phase finishedPhase READ finishedPhase NOTIFY extendableChanged FINAL)

    Q_PROPERTY(int cyclesBeforeLong READ cyclesBeforeLong WRITE setCyclesBeforeLong NOTIFY cyclesBeforeLongChanged FINAL)

    /// A phase cut short before this much time ran is an accidental start: it is not recorded
    /// (SessionRecorder) and a Work phase skipped that early does not count a cycle.
    static constexpr qint64 kMinInterruptedMs = 5000;
    /// What "+5 min" adds.
    static constexpr int kExtendSeconds = 5 * 60;
    /// How long after its end a phase can still be taken up again.
    static constexpr qint64 kReopenWindowMs = 10 * 60 * 1000;

    /// Uses the real monotonic clock. This is the constructor QML uses.
    explicit TimerEngine(QObject *parent = nullptr);
    /// Uses an externally owned clock (tests).
    TimerEngine(Clock *clock, QObject *parent);

    State state() const { return m_state; }
    Phase phase() const { return m_phase; }
    bool isActive() const { return m_state != State::Idle; }
    bool isRunning() const;
    bool isPaused() const { return m_state == State::Paused; }
    bool isIdle() const { return m_state == State::Idle; }

    int remainingSeconds() const;
    int totalSeconds() const;
    double progress() const;
    int completedCycles() const { return m_completedCycles; }
    bool isExtendable() const { return isActive() || isContinuable(); }
    bool isContinuable() const;
    Phase finishedPhase() const { return m_reopen.phase; }

    int workSeconds() const { return m_workSeconds; }
    void setWorkSeconds(int seconds);
    int shortBreakSeconds() const { return m_shortBreakSeconds; }
    void setShortBreakSeconds(int seconds);
    int longBreakSeconds() const { return m_longBreakSeconds; }
    void setLongBreakSeconds(int seconds);
    int cyclesBeforeLong() const { return m_cyclesBeforeLong; }
    void setCyclesBeforeLong(int cycles);
    void setAutoStartMode(AutoStartMode mode);
    void setAutoStartModeInt(int mode);

public Q_SLOTS:
    void start();
    void pause();
    void resume();
    /// Idle -> start, running -> pause, Paused -> resume.
    void toggle();
    void stop();
    void skip();
    /// Runs one Work phase of `seconds` (at least 1) instead of the preset's work length.
    /// An active phase is ended first (saved as interrupted). Later phases use the normal durations.
    void startCustomWork(int seconds);
    /// Back to a fresh Work phase with the cycle count cleared (e.g. after switching preset).
    /// An active phase is stopped first (saved as interrupted).
    void reset();

    /// Adds `seconds` to the active phase; while Idle right after a phase elapsed, takes that one
    /// up again (continueFinished()). False if there is nothing to extend or the phase is at its
    /// longest (PresetLimits).
    bool extend(int seconds = kExtendSeconds, TimerEngine::ExtendSource source = ExtendSource::App);
    /// Takes the phase that has just elapsed up again for `seconds` more (see "More time" above),
    /// also when the next phase has started by itself meanwhile. False when it can no longer be.
    bool continueFinished(int seconds = kExtendSeconds, TimerEngine::ExtendSource source = ExtendSource::App);

    /// Recomputes everything from the clock and ends the phase if due.
    /// Called by the internal ticker; also callable directly (tests).
    void refresh();

Q_SIGNALS:
    void stateChanged();
    void phaseChanged();
    void remainingChanged();
    void totalChanged();
    void progressChanged();
    void cyclesChanged();
    void workSecondsChanged();
    void shortBreakSecondsChanged();
    void longBreakSecondsChanged();
    void cyclesBeforeLongChanged();
    void extendableChanged();
    void oneMinuteRemaining(TimerEngine::Phase phase);

    /// A phase ended, either elapsed (completed) or cut short by stop/skip.
    /// `activeMs` excludes time spent paused; `endedAt - startedAt` does not.
    void sessionEnded(TimerEngine::Phase phase,
                      const QDateTime &startedAt,
                      const QDateTime &endedAt,
                      qint64 activeMs,
                      qint64 plannedMs,
                      bool completed);

    /// A phase began (start, auto-start or skip). Not emitted on resume.
    void phaseStarted(TimerEngine::Phase phase);

    /// A phase elapsed naturally. `next` is the upcoming phase. Use for notifications.
    /// Emitted after the transition: if `next` was auto-started, isRunning() is already true.
    void phaseFinished(TimerEngine::Phase finished, TimerEngine::Phase next);

    /// `seconds` were added: to the running phase (`afterEnd` false) or to the one that had just ended.
    void phaseExtended(TimerEngine::Phase phase, bool afterEnd, TimerEngine::ExtendSource source, int seconds);
    /// The phase that had just ended, already reported by sessionEnded(), goes on: its next
    /// sessionEnded() is the same session, longer. Emitted before the state changes.
    void phaseReopened(TimerEngine::Phase phase);

private:
    qint64 durationMs(Phase phase) const;
    qint64 remainingMs() const;
    static State stateFor(Phase phase);

    /// The phase that really runs in place of `phase`: a zero long break becomes a short break
    /// (ending the set, so the cycles reset), a zero break becomes Work.
    Phase phaseToRun(Phase phase);
    /// `plannedMs` < 0 uses the configured duration of the phase.
    void startPhase(Phase phase, qint64 plannedMs = -1);
    bool shouldAutoStart(Phase next) const;
    void setIdle(Phase next);
    void completePhase();
    /// `endMonoMs`: when the phase ended, on the monotonic clock.
    void emitSessionEnded(bool completed, qint64 endMonoMs);
    /// Advances the cycle count when a Work phase ends (finished or skipped) and returns what follows.
    /// A zero long break becomes a short break (still ending the set); a zero break is passed over.
    Phase nextPhaseAfter(Phase finished);

    void setState(State state);
    /// Completes a running phase whose time is already up (before the ticker noticed); true if it did.
    bool finishIfDue();
    void setPhase(Phase phase);
    void setCycles(int cycles);
    void notifyTime(bool force);
    /// Refreshes the shown time while Idle, replacing a pending break that now lasts 0.
    void refreshIdleDisplay();
    qint64 maxDurationMs(Phase phase) const;
    /// Forgets the phase that could be taken up again.
    void dropReopen();
    void updateExtendable();

    std::unique_ptr<Clock> m_ownedClock;
    Clock *m_clock = nullptr;
    QTimer m_ticker;

    State m_state = State::Idle;
    Phase m_phase = Phase::Work;
    int m_completedCycles = 0;

    // Active phase bookkeeping.
    qint64 m_plannedMs = 0;
    qint64 m_deadlineMs = 0;          // monotonic; valid while running
    qint64 m_remainingAtPauseMs = 0;  // valid while paused
    QDateTime m_startWall;
    qint64 m_startMonoMs = 0;
    int m_lastRemainingSeconds = -1;

    // Configuration.
    int m_workSeconds = 25 * 60;
    int m_shortBreakSeconds = 5 * 60;
    int m_longBreakSeconds = 15 * 60;
    int m_cyclesBeforeLong = 4;
    AutoStartMode m_autoStartMode = AutoStartMode::Manual;
    bool m_warnedOneMinute = false;
    bool m_completing = false; ///< sessionEnded of a completed phase is being emitted

    /// The phase that elapsed last, while it can be taken up again.
    struct Reopen {
        bool valid = false;
        Phase phase = Phase::Work;
        qint64 plannedMs = 0;
        QDateTime startWall;
        qint64 startMonoMs = 0;
        qint64 endMonoMs = 0;
        int cycles = 0;
    };
    Reopen m_reopen;
    QTimer m_reopenExpiry;
    bool m_reopened = false;   ///< the running phase continues a session already reported
    int m_extendableKey = 0; ///< what extendableChanged() last told
    bool m_holdExtendable = false;
};
