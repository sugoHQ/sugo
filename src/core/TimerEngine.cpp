// SPDX-License-Identifier: GPL-3.0-or-later
#include "TimerEngine.h"

#include "TimerPreset.h"

#include <algorithm>

namespace
{
constexpr int kTickIntervalMs = 100;
}

TimerEngine::TimerEngine(QObject *parent)
    : TimerEngine(nullptr, parent)
{
}

TimerEngine::TimerEngine(Clock *clock, QObject *parent)
    : QObject(parent)
{
    if (clock) {
        m_clock = clock;
    } else {
        m_ownedClock = std::make_unique<SystemClock>();
        m_clock = m_ownedClock.get();
    }

    m_ticker.setInterval(kTickIntervalMs);
    m_ticker.setTimerType(Qt::PreciseTimer);
    connect(&m_ticker, &QTimer::timeout, this, &TimerEngine::refresh);
}

// --- Queries ---------------------------------------------------------------

bool TimerEngine::isRunning() const
{
    return m_state == State::Working || m_state == State::ShortBreak || m_state == State::LongBreak;
}

qint64 TimerEngine::durationMs(Phase phase) const
{
    switch (phase) {
    case Phase::Work:
        return m_workSeconds * 1000LL;
    case Phase::ShortBreak:
        return m_shortBreakSeconds * 1000LL;
    case Phase::LongBreak:
        return m_longBreakSeconds * 1000LL;
    }
    Q_UNREACHABLE_RETURN(0);
}

qint64 TimerEngine::remainingMs() const
{
    switch (m_state) {
    case State::Idle:
        return durationMs(m_phase);
    case State::Paused:
        return m_remainingAtPauseMs;
    default:
        return std::max<qint64>(0, m_deadlineMs - m_clock->monotonicMs());
    }
}

int TimerEngine::remainingSeconds() const
{
    return static_cast<int>((remainingMs() + 999) / 1000); // round up: 0:00 only when truly done
}

int TimerEngine::totalSeconds() const
{
    const qint64 total = (m_state == State::Idle) ? durationMs(m_phase) : m_plannedMs;
    return static_cast<int>(total / 1000);
}

double TimerEngine::progress() const
{
    if (m_state == State::Idle || m_plannedMs <= 0) {
        return 0.0;
    }
    return std::clamp(1.0 - double(remainingMs()) / double(m_plannedMs), 0.0, 1.0);
}

TimerEngine::State TimerEngine::stateFor(Phase phase)
{
    switch (phase) {
    case Phase::Work:
        return State::Working;
    case Phase::ShortBreak:
        return State::ShortBreak;
    case Phase::LongBreak:
        return State::LongBreak;
    }
    Q_UNREACHABLE_RETURN(State::Idle);
}

// --- Commands --------------------------------------------------------------

void TimerEngine::start()
{
    if (m_state == State::Idle) {
        startPhase(m_phase);
    }
}

bool TimerEngine::finishIfDue()
{
    if (!isRunning()) {
        return false;
    }
    const qint64 now = m_clock->monotonicMs();
    if (now < m_deadlineMs) {
        return false;
    }
    // The time was up before the next tick noticed: the phase was completed, not cut short.
    completePhase();
    return true;
}

void TimerEngine::pause()
{
    if (m_completing) {
        return;
    }
    finishIfDue(); // pausing then applies to the phase that follows, if it started by itself
    if (!isRunning()) {
        return;
    }
    m_remainingAtPauseMs = remainingMs();
    m_ticker.stop();
    setState(State::Paused);
    notifyTime(false);
}

void TimerEngine::resume()
{
    if (m_state != State::Paused) {
        return;
    }
    m_deadlineMs = m_clock->monotonicMs() + m_remainingAtPauseMs;
    setState(stateFor(m_phase));
    m_ticker.start();
    notifyTime(false);
}

void TimerEngine::toggle()
{
    switch (m_state) {
    case State::Idle:
        start();
        break;
    case State::Paused:
        resume();
        break;
    default:
        pause();
        break;
    }
}

void TimerEngine::stop()
{
    if (m_completing) {
        return;
    }
    // A phase that finishIfDue() just auto-started has not run yet: nothing to record for it.
    // One that ended there and left a break waiting is still stopped: back to Work, cycles reset.
    const bool finished = finishIfDue();
    if (m_state == State::Idle && !finished) {
        return;
    }
    if (!finished) {
        emitSessionEnded(false, m_clock->monotonicMs());
    }
    setCycles(0);
    setIdle(Phase::Work);
}

void TimerEngine::reset()
{
    if (m_state != State::Idle) {
        stop();
        return;
    }
    setCycles(0);
    setIdle(Phase::Work);
}

void TimerEngine::skip()
{
    if (m_completing) {
        return;
    }
    if (finishIfDue()) {
        // The phase being skipped had just ended: skipping means "go on with the next one now".
        if (m_state == State::Idle) {
            startPhase(m_phase);
        }
        return;
    }
    if (m_state == State::Idle) {
        // Nothing runs, so nothing is recorded: just move on to what would follow the pending phase.
        if (m_phase == Phase::Work) {
            // No work was done, so no cycle is counted (the long break is earned, not skipped to):
            // the ordinary break follows, if there is one.
            setIdle(durationMs(Phase::ShortBreak) > 0 ? Phase::ShortBreak : Phase::Work);
            return;
        }
        setIdle(nextPhaseAfter(m_phase));
        return;
    }
    const bool accidental = m_phase == Phase::Work && m_plannedMs - remainingMs() < kMinInterruptedMs;
    emitSessionEnded(false, m_clock->monotonicMs());
    // Work skipped right after starting was not done: no cycle, the ordinary break follows.
    startPhase(accidental ? Phase::ShortBreak : nextPhaseAfter(m_phase));
}

void TimerEngine::startCustomWork(int seconds)
{
    if (m_completing) {
        return;
    }
    const bool finished = finishIfDue();
    if (m_state != State::Idle && !finished) {
        emitSessionEnded(false, m_clock->monotonicMs());
    }
    if (m_phase == Phase::LongBreak) {
        // The long break it replaces closes the set, as skipping it does: otherwise this work
        // would count as one more cycle and bring another long break.
        setCycles(0);
    }
    startPhase(Phase::Work, std::clamp(seconds, 1, PresetLimits::MaxWorkSeconds) * 1000LL);
}

void TimerEngine::refresh()
{
    if (!isRunning() || m_completing) {
        return;
    }
    const qint64 now = m_clock->monotonicMs();
    if (now >= m_deadlineMs) {
        completePhase();
    } else {
        notifyTime(false);
    }
}

// --- Transitions -----------------------------------------------------------

TimerEngine::Phase TimerEngine::phaseToRun(Phase phase)
{
    if (phase == Phase::LongBreak && durationMs(phase) <= 0) {
        // A pending long break whose duration was set to 0 meanwhile: it still ends the set, and
        // the ordinary break takes its place (normally handled by nextPhaseAfter()).
        setCycles(0);
        phase = Phase::ShortBreak;
    }
    if (phase != Phase::Work && durationMs(phase) <= 0) {
        // A zero-length break means "no break": go straight back to work.
        phase = Phase::Work;
    }
    return phase;
}

void TimerEngine::startPhase(Phase phase, qint64 plannedMs)
{
    phase = phaseToRun(phase);

    // The new timing is in place before phaseChanged/stateChanged go out, so a listener never
    // sees the new phase with the old (passed) deadline.
    m_plannedMs = plannedMs >= 0 ? plannedMs : durationMs(phase);
    m_startWall = m_clock->wallTime();
    m_startMonoMs = m_clock->monotonicMs();
    m_deadlineMs = m_clock->monotonicMs() + m_plannedMs;
    m_warnedOneMinute = false;
    const State previous = m_state;
    m_state = stateFor(phase);
    setPhase(phase);
    if (m_state != previous) {
        Q_EMIT stateChanged();
    }
    m_ticker.start();
    notifyTime(true);
    Q_EMIT phaseStarted(phase);
}

void TimerEngine::setIdle(Phase next)
{
    m_ticker.stop();
    setPhase(next);
    setState(State::Idle);
    m_warnedOneMinute = false;
    notifyTime(true);
}

void TimerEngine::completePhase()
{
    const Phase finished = m_phase;

    m_ticker.stop();
    // The phase is still in its expired running state here: a listener's stop(), pause() or
    // refresh() must not end it a second time, so commands are ignored until it is done.
    m_completing = true;
    // If the tick arrived late, the phase really ended at the deadline.
    emitSessionEnded(true, m_deadlineMs);
    m_completing = false;

    const Phase next = nextPhaseAfter(finished);
    // Start the next phase before announcing the end, so listeners see the real state. Going
    // straight from one running state to the next also avoids a passing Idle state, which would
    // make the power/notification inhibitors and the focus sound let go and grab again.
    if (shouldAutoStart(next)) {
        startPhase(next);
    } else {
        setIdle(next);
    }
    Q_EMIT phaseFinished(finished, next);
}

bool TimerEngine::shouldAutoStart(Phase next) const
{
    switch (m_autoStartMode) {
    case AutoStartMode::All:
        return true;
    case AutoStartMode::BreaksOnly:
        return next != Phase::Work;
    case AutoStartMode::Manual:
        break;
    }
    return false;
}

void TimerEngine::emitSessionEnded(bool completed, qint64 endMonoMs)
{
    const QDateTime wallEnd = m_clock->wallTime().addMSecs(endMonoMs - m_clock->monotonicMs());
    // A wall clock set back meanwhile (NTP, by hand) must not end the session before it started
    // plus the time it took, pauses included; one set forward is believed (it may be a suspend).
    const QDateTime endedAt = std::max(wallEnd, m_startWall.addMSecs(endMonoMs - m_startMonoMs));
    const qint64 activeMs = completed ? m_plannedMs : m_plannedMs - remainingMs();
    Q_EMIT sessionEnded(m_phase, m_startWall, endedAt, activeMs, m_plannedMs, completed);
}

TimerEngine::Phase TimerEngine::nextPhaseAfter(Phase finished)
{
    switch (finished) {
    case Phase::Work: {
        setCycles(m_completedCycles + 1);
        Phase brk = (m_cyclesBeforeLong > 0 && m_completedCycles >= m_cyclesBeforeLong) ? Phase::LongBreak
                                                                                        : Phase::ShortBreak;
        if (brk == Phase::LongBreak && durationMs(brk) <= 0) {
            // No long break configured: the set still ends here, with the ordinary break.
            setCycles(0);
            brk = Phase::ShortBreak;
        }
        // No break at all: back to work.
        return durationMs(brk) > 0 ? brk : Phase::Work;
    }
    case Phase::ShortBreak:
        return Phase::Work;
    case Phase::LongBreak:
        setCycles(0);
        return Phase::Work;
    }
    Q_UNREACHABLE_RETURN(Phase::Work);
}

// --- Change notification helpers -------------------------------------------

void TimerEngine::setState(State state)
{
    if (m_state != state) {
        m_state = state;
        Q_EMIT stateChanged();
    }
}

void TimerEngine::setPhase(Phase phase)
{
    if (m_phase != phase) {
        m_phase = phase;
        Q_EMIT phaseChanged();
    }
}

void TimerEngine::setCycles(int cycles)
{
    if (m_completedCycles != cycles) {
        m_completedCycles = cycles;
        Q_EMIT cyclesChanged();
    }
}

void TimerEngine::notifyTime(bool force)
{
    const int seconds = remainingSeconds();
    const int previousSeconds = m_lastRemainingSeconds;
    if (force || seconds != m_lastRemainingSeconds) {
        m_lastRemainingSeconds = seconds;
        Q_EMIT remainingChanged();

        // Only phases longer than the warning itself get one; otherwise it would fire at the start.
        if (m_state != State::Idle && m_plannedMs > 60 * 1000 && seconds > 0 && seconds <= 60 && !m_warnedOneMinute) {
            m_warnedOneMinute = true;
            Q_EMIT oneMinuteRemaining(m_phase);
        }
    }
    if (force) {
        Q_EMIT totalChanged();
    }
    // Only when the shown time moves: the ticker runs ten times a second, and every listener
    // (the progress ring, the tray) would otherwise redraw for nothing, even when hidden.
    if (force || seconds != previousSeconds) {
        Q_EMIT progressChanged();
    }
}

void TimerEngine::refreshIdleDisplay()
{
    if (m_state != State::Idle) {
        return;
    }
    // A pending break whose duration was just set to 0 gives way now, as it would on start, so
    // the display, the tray and the notification never offer a 0:00 break that starts Work.
    const Phase pending = phaseToRun(m_phase);
    if (pending != m_phase) {
        setIdle(pending);
        return;
    }
    notifyTime(true);
}

// --- Configuration ---------------------------------------------------------

void TimerEngine::setWorkSeconds(int seconds)
{
    seconds = std::clamp(seconds, 1, PresetLimits::MaxWorkSeconds);
    if (m_workSeconds != seconds) {
        m_workSeconds = seconds;
        Q_EMIT workSecondsChanged();
        refreshIdleDisplay();
    }
}

void TimerEngine::setShortBreakSeconds(int seconds)
{
    seconds = std::clamp(seconds, 0, PresetLimits::MaxBreakSeconds);
    if (m_shortBreakSeconds != seconds) {
        m_shortBreakSeconds = seconds;
        Q_EMIT shortBreakSecondsChanged();
        refreshIdleDisplay();
    }
}

void TimerEngine::setLongBreakSeconds(int seconds)
{
    seconds = std::clamp(seconds, 0, PresetLimits::MaxBreakSeconds);
    if (m_longBreakSeconds != seconds) {
        m_longBreakSeconds = seconds;
        Q_EMIT longBreakSecondsChanged();
        refreshIdleDisplay();
    }
}

void TimerEngine::setCyclesBeforeLong(int cycles)
{
    cycles = std::clamp(cycles, 0, PresetLimits::MaxCycles);
    if (m_cyclesBeforeLong != cycles) {
        m_cyclesBeforeLong = cycles;
        Q_EMIT cyclesBeforeLongChanged();
    }
}

void TimerEngine::setAutoStartMode(AutoStartMode mode)
{
    m_autoStartMode = mode;
}

void TimerEngine::setAutoStartModeInt(int mode)
{
    setAutoStartMode(static_cast<AutoStartMode>(std::clamp(mode, 0, 2)));
}
