// SPDX-License-Identifier: GPL-3.0-or-later
#include <QSignalSpy>
#include <QTest>

#include <limits>
#include <memory>

#include "FakeClock.h"
#include "TimerEngine.h"
#include "TimerPreset.h"


using State = TimerEngine::State;
using Phase = TimerEngine::Phase;

class TimerEngineTest : public QObject
{
    Q_OBJECT

private:
    FakeClock clock;
    std::unique_ptr<TimerEngine> engine;

    // Advances the fake clock and lets the engine react, like one ticker tick.
    void elapse(qint64 seconds)
    {
        clock.advanceSeconds(seconds);
        engine->refresh();
    }

private Q_SLOTS:
    void init()
    {
        clock = FakeClock();
        engine = std::make_unique<TimerEngine>(&clock, nullptr);
    }

    void initialState()
    {
        QCOMPARE(engine->state(), State::Idle);
        QCOMPARE(engine->phase(), Phase::Work);
        QCOMPARE(engine->remainingSeconds(), 25 * 60);
        QCOMPARE(engine->totalSeconds(), 25 * 60);
        QCOMPARE(engine->progress(), 0.0);
        QCOMPARE(engine->completedCycles(), 0);
    }

    void countsDownFromTimestamps()
    {
        engine->start();
        QCOMPARE(engine->state(), State::Working);
        elapse(10);
        QCOMPARE(engine->remainingSeconds(), 25 * 60 - 10);
        QVERIFY(engine->progress() > 0.0);
    }

    void remainingDoesNotDependOnTicks()
    {
        engine->start();
        // A frozen event loop: 10 minutes pass with no refresh() call at all.
        clock.advanceSeconds(600);
        QCOMPARE(engine->remainingSeconds(), 15 * 60);
        QCOMPARE(engine->state(), State::Working);
    }

    void pauseFreezesAndResumeContinues()
    {
        engine->start();
        elapse(60);
        engine->pause();
        QCOMPARE(engine->state(), State::Paused);
        QVERIFY(engine->isPaused());
        elapse(600);
        QCOMPARE(engine->remainingSeconds(), 24 * 60);
        engine->resume();
        QCOMPARE(engine->state(), State::Working);
        elapse(30);
        QCOMPARE(engine->remainingSeconds(), 24 * 60 - 30);
    }

    void toggleCyclesThroughStates()
    {
        engine->toggle();
        QCOMPARE(engine->state(), State::Working);
        engine->toggle();
        QCOMPARE(engine->state(), State::Paused);
        engine->toggle();
        QCOMPARE(engine->state(), State::Working);
    }

    void workEndWaitsWhenAutoStartOff()
    {
        QSignalSpy finished(engine.get(), &TimerEngine::phaseFinished);
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);

        engine->start();
        elapse(25 * 60);

        QCOMPARE(engine->state(), State::Idle);
        QCOMPARE(engine->phase(), Phase::ShortBreak);
        QCOMPARE(engine->remainingSeconds(), 5 * 60);
        QCOMPARE(engine->completedCycles(), 1);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(0).value<Phase>(), Phase::Work);
        QCOMPARE(finished.at(0).at(1).value<Phase>(), Phase::ShortBreak);
        QCOMPARE(ended.count(), 1);
        QVERIFY(ended.at(0).at(5).toBool()); // completed

        engine->start();
        QCOMPARE(engine->state(), State::ShortBreak);
    }

    void workEndAutoStartsNextPhase()
    {
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        engine->start();
        elapse(25 * 60);
        QCOMPARE(engine->state(), State::ShortBreak);
        elapse(5 * 60);
        QCOMPARE(engine->state(), State::Working);
    }

    void lateTickRecordsEndAtDeadline()
    {
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        engine->start();
        elapse(25 * 60 + 7); // tick arrives 7 s late
        QCOMPARE(ended.count(), 1);
        const QDateTime end = ended.at(0).at(2).toDateTime();
        QCOMPARE(end, clock.base().addSecs(25 * 60));
    }

    void longBreakAfterConfiguredCycles()
    {
        engine->setCyclesBeforeLong(2);
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);

        engine->start();
        elapse(25 * 60); // work 1 -> short break
        QCOMPARE(engine->state(), State::ShortBreak);
        elapse(5 * 60);  // -> work 2
        QCOMPARE(engine->state(), State::Working);
        elapse(25 * 60); // work 2 -> long break
        QCOMPARE(engine->state(), State::LongBreak);
        QCOMPARE(engine->remainingSeconds(), 15 * 60);
        QCOMPARE(engine->completedCycles(), 2);

        elapse(15 * 60); // long break -> work, set resets
        QCOMPARE(engine->state(), State::Working);
        QCOMPARE(engine->completedCycles(), 0);
    }

    void longBreakDisabledWithZeroCycles()
    {
        engine->setCyclesBeforeLong(0);
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        engine->start();
        for (int i = 0; i < 6; ++i) {
            elapse(25 * 60);
            QCOMPARE(engine->state(), State::ShortBreak); // never a long break
            elapse(5 * 60);
            QCOMPARE(engine->state(), State::Working);
        }
    }

    void stopRecordsInterruptedSessionAndResets()
    {
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        engine->start();
        elapse(100);
        engine->stop();

        QCOMPARE(engine->state(), State::Idle);
        QCOMPARE(engine->phase(), Phase::Work);
        QCOMPARE(engine->completedCycles(), 0);
        QCOMPARE(ended.count(), 1);
        QCOMPARE(ended.at(0).at(0).value<Phase>(), Phase::Work);
        QCOMPARE(ended.at(0).at(3).toLongLong(), 100'000);
        QCOMPARE(ended.at(0).at(4).toLongLong(), 25 * 60 * 1000);
        QVERIFY(!ended.at(0).at(5).toBool());
    }

    void activeTimeExcludesPause()
    {
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        engine->start();
        elapse(60);
        engine->pause();
        elapse(300);
        engine->resume();
        elapse(40);
        engine->stop();

        QCOMPARE(ended.count(), 1);
        QCOMPARE(ended.at(0).at(3).toLongLong(), 100'000); // 60 + 40
        const QDateTime start = ended.at(0).at(1).toDateTime();
        const QDateTime end = ended.at(0).at(2).toDateTime();
        QCOMPARE(start.secsTo(end), 400); // wall time includes the pause
    }

    void stopWhilePausedRecordsElapsedTime()
    {
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        engine->start();
        elapse(120);
        engine->pause();
        elapse(30);
        engine->stop();
        QCOMPARE(ended.at(0).at(3).toLongLong(), 120'000);
    }

    void skipAdvancesAndStartsNextPhase()
    {
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        QSignalSpy finished(engine.get(), &TimerEngine::phaseFinished);

        engine->start();
        elapse(30);
        engine->skip();
        QCOMPARE(engine->state(), State::ShortBreak);
        QCOMPARE(engine->completedCycles(), 1); // a skipped work phase still counts as a cycle
        QCOMPARE(ended.count(), 1);
        QVERIFY(!ended.at(0).at(5).toBool());
        QCOMPARE(finished.count(), 0);          // skip is not a natural end

        engine->skip();
        QCOMPARE(engine->state(), State::Working);
    }

    void skipWhilePausedStartsNextPhaseAndCountsTheCycle()
    {
        engine->start();
        elapse(10);
        engine->pause();
        engine->skip();
        QCOMPARE(engine->state(), State::ShortBreak);
        QCOMPARE(engine->completedCycles(), 1);
    }

    void skippingBreaksDoesNotAddCycles()
    {
        engine->start();
        elapse(10);
        engine->skip(); // work -> short break, counts
        QCOMPARE(engine->completedCycles(), 1);
        engine->skip(); // short break -> work, does not count
        QCOMPARE(engine->state(), State::Working);
        QCOMPARE(engine->completedCycles(), 1);
    }

    void skippingThroughASetReachesTheLongBreak()
    {
        engine->setCyclesBeforeLong(2);
        engine->start();

        elapse(10);
        engine->skip(); // work 1 -> short break
        QCOMPARE(engine->state(), State::ShortBreak);
        engine->skip(); // -> work 2
        QCOMPARE(engine->state(), State::Working);
        elapse(10);
        engine->skip(); // work 2 -> long break
        QCOMPARE(engine->state(), State::LongBreak);
        QCOMPARE(engine->completedCycles(), 2);

        engine->skip(); // long break -> work; the set starts over
        QCOMPARE(engine->state(), State::Working);
        QCOMPARE(engine->completedCycles(), 0);
    }

    void skippedAndFinishedWorkPhasesCountTogether()
    {
        engine->setCyclesBeforeLong(2);
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        engine->start();
        elapse(25 * 60); // finished -> short break (cycle 1)
        elapse(5 * 60);  // -> work
        elapse(10);
        engine->skip();  // skipped -> long break (cycle 2)
        QCOMPARE(engine->state(), State::LongBreak);
    }

    void workSkippedRightAfterStartingCountsNoCycle()
    {
        engine->setCyclesBeforeLong(1);
        engine->start();
        clock.advanceMs(TimerEngine::kMinInterruptedMs - 1);
        engine->skip(); // an accidental start, not recorded: no cycle, so no long break either
        QCOMPARE(engine->state(), State::ShortBreak);
        QCOMPARE(engine->completedCycles(), 0);

        engine->skip();
        clock.advanceMs(TimerEngine::kMinInterruptedMs);
        engine->skip(); // long enough to be recorded: it counts
        QCOMPARE(engine->state(), State::LongBreak);
        QCOMPARE(engine->completedCycles(), 1);
    }

    void wallClockSetBackNeverEndsASessionBeforeItsStart()
    {
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        engine->start();
        elapse(60);
        engine->pause();
        clock.advanceSeconds(30);
        clock.setWallBy(-3600 * 1000); // NTP or the user sets the clock back an hour
        engine->resume();
        elapse(60);
        engine->stop();
        QCOMPARE(ended.count(), 1);
        const QDateTime start = ended.at(0).at(1).toDateTime();
        QCOMPARE(start, clock.base().addSecs(3600)); // read before the jump
        QCOMPARE(ended.at(0).at(2).toDateTime(), start.addSecs(60 + 30 + 60));
        QCOMPARE(ended.at(0).at(3).toLongLong(), 120 * 1000);

        // Completed phases too, and a clock set forward is believed.
        engine->start();
        clock.setWallBy(-3600 * 1000);
        elapse(25 * 60);
        QCOMPARE(ended.at(1).at(2).toDateTime(), ended.at(1).at(1).toDateTime().addSecs(25 * 60));
        engine->start();
        clock.setWallBy(600 * 1000);
        elapse(5 * 60);
        QCOMPARE(ended.at(2).at(2).toDateTime(), ended.at(2).at(1).toDateTime().addSecs(15 * 60));
    }

    void zeroLengthBreakIsSkipped()
    {
        engine->setShortBreakSeconds(0);
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        engine->start();
        elapse(25 * 60);
        QCOMPARE(engine->state(), State::Working);
        QCOMPARE(engine->completedCycles(), 1);
    }

    void configChangeUpdatesIdleDisplay()
    {
        QSignalSpy remaining(engine.get(), &TimerEngine::remainingChanged);
        engine->setWorkSeconds(50 * 60);
        QCOMPARE(engine->remainingSeconds(), 50 * 60);
        QVERIFY(remaining.count() >= 1);
    }

    void configChangeDoesNotAffectRunningPhase()
    {
        engine->start();
        engine->setWorkSeconds(10 * 60);
        QCOMPARE(engine->remainingSeconds(), 25 * 60);
        QCOMPARE(engine->totalSeconds(), 25 * 60);
    }

    void invalidConfigIsClamped()
    {
        engine->setWorkSeconds(0);
        QCOMPARE(engine->workSeconds(), 1);
        engine->setShortBreakSeconds(-5);
        QCOMPARE(engine->shortBreakSeconds(), 0);
        engine->setCyclesBeforeLong(-1);
        QCOMPARE(engine->cyclesBeforeLong(), 0);
    }

    void resetClearsPendingBreakAndCycles()
    {
        engine->start();
        elapse(25 * 60); // work done, Idle with a pending short break
        QCOMPARE(engine->phase(), Phase::ShortBreak);
        QCOMPARE(engine->completedCycles(), 1);

        engine->reset();
        QCOMPARE(engine->state(), State::Idle);
        QCOMPARE(engine->phase(), Phase::Work);
        QCOMPARE(engine->completedCycles(), 0);
    }

    void resetStopsActivePhase()
    {
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        engine->start();
        elapse(20);
        engine->reset();
        QCOMPARE(engine->state(), State::Idle);
        QCOMPARE(ended.count(), 1);
        QVERIFY(!ended.at(0).at(5).toBool());
    }

    void phaseStartedIsEmittedOnStartAutoStartAndSkipButNotResume()
    {
        QSignalSpy started(engine.get(), &TimerEngine::phaseStarted);
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);

        engine->start();
        QCOMPARE(started.count(), 1);
        QCOMPARE(started.at(0).at(0).value<Phase>(), Phase::Work);

        engine->pause();
        engine->resume();
        QCOMPARE(started.count(), 1); // resume is not a new phase

        elapse(25 * 60); // auto-start of the short break
        QCOMPARE(started.count(), 2);
        QCOMPARE(started.at(1).at(0).value<Phase>(), Phase::ShortBreak);

        engine->skip(); // back to work
        QCOMPARE(started.count(), 3);
        QCOMPARE(started.at(2).at(0).value<Phase>(), Phase::Work);
    }

    void oneMinuteWarningIsEmittedOncePerPhase()
    {
        engine->setWorkSeconds(90);
        QSignalSpy warning(engine.get(), &TimerEngine::oneMinuteRemaining);
        engine->start();
        elapse(29);
        QCOMPARE(warning.count(), 0);
        elapse(1);
        QCOMPARE(warning.count(), 1);
        QCOMPARE(warning.first().first().value<Phase>(), Phase::Work);
        elapse(20);
        QCOMPARE(warning.count(), 1);
        engine->reset();
        engine->start();
        elapse(30);
        QCOMPARE(warning.count(), 2);
    }

    void autoStartModeControlsWorkAndBreakTransitions()
    {
        engine->setWorkSeconds(2);
        engine->setShortBreakSeconds(2);
        engine->setAutoStartMode(TimerEngine::AutoStartMode::BreaksOnly);
        engine->start();
        elapse(2);
        QCOMPARE(engine->state(), State::ShortBreak); // work completion starts a break
        elapse(2);
        QCOMPARE(engine->state(), State::Idle); // break completion waits before work
        QCOMPARE(engine->phase(), Phase::Work);

        engine->setAutoStartModeInt(2);
        engine->start();
        elapse(2);
        QCOMPARE(engine->state(), State::ShortBreak);
        elapse(2);
        QCOMPARE(engine->state(), State::Working);
    }

    void phaseFinishedSeesTheAutoStartedPhase()
    {
        engine->setAutoStartMode(TimerEngine::AutoStartMode::BreaksOnly);
        bool runningAtFinish = false;
        State stateAtFinish = State::Idle;
        connect(engine.get(), &TimerEngine::phaseFinished, this, [&]() {
            runningAtFinish = engine->isRunning();
            stateAtFinish = engine->state();
        });
        engine->start();
        elapse(25 * 60);
        QVERIFY(runningAtFinish);
        QCOMPARE(stateAtFinish, State::ShortBreak);

        elapse(5 * 60); // BreaksOnly: work waits
        QVERIFY(!runningAtFinish);
        QCOMPARE(stateAtFinish, State::Idle);
    }

    void autoStartedTransitionDoesNotPassThroughIdle()
    {
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        engine->start();
        QList<State> states;
        connect(engine.get(), &TimerEngine::stateChanged, this, [&]() {
            states.append(engine->state());
        });
        elapse(25 * 60);
        QCOMPARE(states, QList<State>{State::ShortBreak});
    }

    void sessionEndedComesBeforeTheNextPhaseStarted()
    {
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        QStringList order;
        connect(engine.get(), &TimerEngine::sessionEnded, this, [&]() { order << QStringLiteral("ended"); });
        connect(engine.get(), &TimerEngine::phaseStarted, this, [&]() { order << QStringLiteral("started"); });
        connect(engine.get(), &TimerEngine::phaseFinished, this, [&]() { order << QStringLiteral("finished"); });
        engine->start();
        order.clear();
        elapse(25 * 60);
        QCOMPARE(order, (QStringList{QStringLiteral("ended"), QStringLiteral("started"), QStringLiteral("finished")}));
    }

    void skipWhileIdleAdvancesThePendingPhase()
    {
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        engine->start();
        elapse(25 * 60); // Idle, pending short break
        QCOMPARE(engine->phase(), Phase::ShortBreak);
        ended.clear();

        engine->skip(); // skip the break
        QCOMPARE(engine->state(), State::Idle);
        QCOMPARE(engine->phase(), Phase::Work);
        QCOMPARE(engine->completedCycles(), 1);

        engine->skip(); // skip the pending work phase: no work was done, so no cycle
        QCOMPARE(engine->state(), State::Idle);
        QCOMPARE(engine->phase(), Phase::ShortBreak);
        QCOMPARE(engine->completedCycles(), 1);
        QCOMPARE(ended.count(), 0); // nothing ran, nothing recorded
    }

    void skippingWaitingWorkNeverReachesTheLongBreak()
    {
        // Pressing Skip on every "time to work" prompt must not earn a long break.
        engine->setCyclesBeforeLong(2);
        for (int i = 0; i < 6; ++i) {
            QCOMPARE(engine->phase(), Phase::Work);
            engine->skip(); // waiting work -> short break
            QCOMPARE(engine->phase(), Phase::ShortBreak);
            engine->skip(); // waiting break -> work
        }
        QCOMPARE(engine->completedCycles(), 0);

        // With no short break there is nothing to skip to: work stays pending.
        engine->setShortBreakSeconds(0);
        engine->skip();
        QCOMPARE(engine->phase(), Phase::Work);
        QCOMPARE(engine->completedCycles(), 0);
    }

    void skipWhileIdleOverAPendingLongBreakEndsTheSet()
    {
        engine->setCyclesBeforeLong(1);
        engine->start();
        elapse(25 * 60);
        QCOMPARE(engine->phase(), Phase::LongBreak);
        engine->skip();
        QCOMPARE(engine->phase(), Phase::Work);
        QCOMPARE(engine->completedCycles(), 0);
    }

    void customWorkInPlaceOfAPendingLongBreakEndsTheSet()
    {
        engine->setCyclesBeforeLong(2);
        engine->start();
        elapse(25 * 60);
        QCOMPARE(engine->phase(), Phase::ShortBreak);
        engine->skip(); // waiting short break -> work
        engine->start();
        elapse(25 * 60);
        QCOMPARE(engine->phase(), Phase::LongBreak);
        QCOMPARE(engine->completedCycles(), 2);

        engine->startCustomWork(10 * 60);
        QCOMPARE(engine->completedCycles(), 0);
        elapse(10 * 60);
        // The set ended with the long break it replaced: this work is the first cycle of the
        // next one, so an ordinary break follows (not a second long break).
        QCOMPARE(engine->phase(), Phase::ShortBreak);
        QCOMPARE(engine->completedCycles(), 1);
    }

    void zeroLengthBreakWaitsForWorkInBreaksOnlyMode()
    {
        QSignalSpy finished(engine.get(), &TimerEngine::phaseFinished);
        engine->setShortBreakSeconds(0);
        engine->setAutoStartMode(TimerEngine::AutoStartMode::BreaksOnly);
        engine->start();
        elapse(25 * 60);
        QCOMPARE(engine->state(), State::Idle); // work never auto-starts in BreaksOnly
        QCOMPARE(engine->phase(), Phase::Work);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(1).value<Phase>(), Phase::Work);
    }

    void zeroLengthLongBreakGivesAShortBreakAndEndsTheSet()
    {
        // "No long break" must not take the ordinary break away as well.
        engine->setCyclesBeforeLong(2);
        engine->setLongBreakSeconds(0);
        engine->setShortBreakSeconds(5 * 60);
        engine->start();
        engine->skip();  // work 1 -> short break
        engine->skip();  // -> work 2
        engine->skip();  // work 2 -> (no long break) -> short break instead
        QCOMPARE(engine->state(), State::ShortBreak);
        QCOMPARE(engine->totalSeconds(), 5 * 60);
        QCOMPARE(engine->completedCycles(), 0);

        // The same when it elapses rather than being skipped.
        engine->stop();
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        engine->start();
        elapse(25 * 60);  // work 1 -> short break
        elapse(5 * 60);   // -> work 2
        elapse(25 * 60);  // -> short break instead of the long one
        QCOMPARE(engine->state(), State::ShortBreak);
        QCOMPARE(engine->completedCycles(), 0);
    }

    void bothBreaksZeroGoesStraightBackToWork()
    {
        engine->setCyclesBeforeLong(2);
        engine->setLongBreakSeconds(0);
        engine->setShortBreakSeconds(0);
        engine->start();
        engine->skip();  // work 1 -> work 2
        engine->skip();  // work 2 -> work, set ended
        QCOMPARE(engine->state(), State::Working);
        QCOMPARE(engine->completedCycles(), 0);
    }

    void pendingLongBreakSetToZeroBecomesAShortBreak()
    {
        engine->setCyclesBeforeLong(1);
        engine->setAutoStartMode(TimerEngine::AutoStartMode::Manual);
        engine->start();
        elapse(25 * 60);
        QCOMPARE(engine->phase(), Phase::LongBreak);
        engine->setLongBreakSeconds(0);
        engine->start();
        QCOMPARE(engine->state(), State::ShortBreak);
        QCOMPARE(engine->completedCycles(), 0);
    }

    void pendingShortBreakSetToZeroGivesWayToWork()
    {
        engine->start();
        elapse(25 * 60);
        QCOMPARE(engine->state(), State::Idle);
        QCOMPARE(engine->phase(), Phase::ShortBreak);

        QSignalSpy phase(engine.get(), &TimerEngine::phaseChanged);
        engine->setShortBreakSeconds(0);
        // Not a 0:00 break that would start Work: Work is what waits now.
        QCOMPARE(phase.count(), 1);
        QCOMPARE(engine->state(), State::Idle);
        QCOMPARE(engine->phase(), Phase::Work);
        QCOMPARE(engine->remainingSeconds(), 25 * 60);
        QCOMPARE(engine->totalSeconds(), 25 * 60);
        QCOMPARE(engine->completedCycles(), 1); // the work done still counts
        engine->start();
        QCOMPARE(engine->state(), State::Working);
    }

    void pendingLongBreakSetToZeroIsReplacedAtOnce()
    {
        engine->setCyclesBeforeLong(1);
        engine->start();
        elapse(25 * 60);
        QCOMPARE(engine->phase(), Phase::LongBreak);
        QCOMPARE(engine->completedCycles(), 1);

        QSignalSpy phase(engine.get(), &TimerEngine::phaseChanged);
        engine->setLongBreakSeconds(0);
        // The set still ends here, with the ordinary break.
        QCOMPARE(phase.count(), 1);
        QCOMPARE(engine->state(), State::Idle);
        QCOMPARE(engine->phase(), Phase::ShortBreak);
        QCOMPARE(engine->remainingSeconds(), 5 * 60);
        QCOMPARE(engine->completedCycles(), 0);

        // With no ordinary break either, Work waits.
        engine->setShortBreakSeconds(0);
        QCOMPARE(phase.count(), 2);
        QCOMPARE(engine->phase(), Phase::Work);
        QCOMPARE(engine->remainingSeconds(), 25 * 60);
    }

    void pendingLongBreakWithBothBreaksSetToZeroWaitsForWork()
    {
        engine->setCyclesBeforeLong(1);
        engine->start();
        elapse(25 * 60);
        QCOMPARE(engine->phase(), Phase::LongBreak);
        // The order the binding pushes them in: the short break first.
        engine->setShortBreakSeconds(0);
        QCOMPARE(engine->phase(), Phase::LongBreak);
        engine->setLongBreakSeconds(0);
        QCOMPARE(engine->phase(), Phase::Work);
        QCOMPARE(engine->completedCycles(), 0);
    }

    void breakThatIsNotPendingSetToZeroChangesNothing()
    {
        QSignalSpy phase(engine.get(), &TimerEngine::phaseChanged);
        QSignalSpy cycles(engine.get(), &TimerEngine::cyclesChanged);

        // Work waits: neither break is pending.
        engine->setShortBreakSeconds(0);
        engine->setLongBreakSeconds(0);
        QCOMPARE(engine->phase(), Phase::Work);
        QCOMPARE(phase.count(), 0);

        // A short break waits: the long break is not the one pending.
        engine->setShortBreakSeconds(5 * 60);
        engine->setLongBreakSeconds(15 * 60);
        engine->start();
        elapse(25 * 60);
        QCOMPARE(engine->phase(), Phase::ShortBreak);
        phase.clear();
        cycles.clear();
        engine->setLongBreakSeconds(0);
        QCOMPARE(engine->phase(), Phase::ShortBreak);
        QCOMPARE(engine->completedCycles(), 1);
        QCOMPARE(phase.count(), 0);
        QCOMPARE(cycles.count(), 0);

        // A running break is left alone: the change applies to the next one.
        engine->start();
        engine->setShortBreakSeconds(0);
        QCOMPARE(engine->state(), State::ShortBreak);
        QCOMPARE(engine->remainingSeconds(), 5 * 60);
    }

    void timeSignalsFollowTheDisplayedSecondNotTheTicker()
    {
        // The ticker runs ten times a second; listeners (the progress ring repaints, the tray)
        // only need to hear about it when the shown time moves.
        engine->start();
        QSignalSpy progress(engine.get(), &TimerEngine::progressChanged);
        QSignalSpy remaining(engine.get(), &TimerEngine::remainingChanged);
        for (int i = 0; i < 30; ++i) {
            clock.advanceMs(100);
            engine->refresh();
        }
        QCOMPARE(remaining.count(), 3);
        QCOMPARE(progress.count(), 3);
        QVERIFY(engine->progress() > 0.0);
    }

        void durationsHaveTheSameUpperBoundsAsTimers()
    {
        engine->setWorkSeconds(std::numeric_limits<int>::max());
        QCOMPARE(engine->workSeconds(), PresetLimits::MaxWorkSeconds);
        engine->setShortBreakSeconds(std::numeric_limits<int>::max());
        QCOMPARE(engine->shortBreakSeconds(), PresetLimits::MaxBreakSeconds);
        engine->setLongBreakSeconds(std::numeric_limits<int>::max());
        QCOMPARE(engine->longBreakSeconds(), PresetLimits::MaxBreakSeconds);
        engine->setCyclesBeforeLong(100000);
        QCOMPARE(engine->cyclesBeforeLong(), PresetLimits::MaxCycles);

        // A custom phase from any caller (QML, KRunner) cannot run for days, nor overflow.
        engine->startCustomWork(std::numeric_limits<int>::max());
        QCOMPARE(engine->totalSeconds(), PresetLimits::MaxWorkSeconds);
        engine->stop();
        engine->startCustomWork(-5);
        QCOMPARE(engine->totalSeconds(), 1);
    }

    void phaseChangedAlreadySeesTheNewPhasesTiming()
    {
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        engine->start();
        int remainingSeen = -1;
        State stateSeen = State::Idle;
        connect(engine.get(), &TimerEngine::phaseChanged, this, [&] {
            remainingSeen = engine->remainingSeconds();
            stateSeen = engine->state();
        });
        elapse(25 * 60); // work -> short break, auto-started
        QCOMPARE(engine->phase(), Phase::ShortBreak);
        QCOMPARE(remainingSeen, 5 * 60);
        QCOMPARE(stateSeen, State::ShortBreak);
    }

    void startingAPhaseInTheSameStateSendsNoStateChange()
    {
        engine->setShortBreakSeconds(0);
        engine->setCyclesBeforeLong(0);
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        engine->start();
        QSignalSpy state(engine.get(), &TimerEngine::stateChanged);
        elapse(25 * 60); // work -> (no break) -> work
        QCOMPARE(engine->state(), State::Working);
        QCOMPARE(state.count(), 0);
    }

    void stopAfterTheDeadlineButBeforeTheTickCountsAsCompleted()
    {
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        engine->start();
        clock.advanceSeconds(25 * 60 + 1); // the time is up; the ticker has not run yet
        engine->stop();
        QCOMPARE(ended.count(), 1);
        QCOMPARE(ended.first().at(5).toBool(), true); // completed
        QCOMPARE(engine->state(), State::Idle);
        // Still a stop: the break the finished phase left waiting is dropped, the set starts over.
        QCOMPARE(engine->phase(), Phase::Work);
        QCOMPARE(engine->completedCycles(), 0);
    }

    void resetAfterTheDeadlineButBeforeTheTickStartsAFreshSet()
    {
        engine->setCyclesBeforeLong(1); // the finished work phase would leave a long break waiting
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        engine->start();
        clock.advanceSeconds(25 * 60 + 1);
        engine->reset();
        QCOMPARE(ended.count(), 1);
        QCOMPARE(ended.first().at(5).toBool(), true);
        QCOMPARE(engine->state(), State::Idle);
        QCOMPARE(engine->phase(), Phase::Work);
        QCOMPARE(engine->completedCycles(), 0);
    }

    void skipAfterTheDeadlineCompletesAndStartsTheNextPhase()
    {
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        QSignalSpy finished(engine.get(), &TimerEngine::phaseFinished);
        engine->start();
        clock.advanceSeconds(25 * 60 + 1);
        engine->skip();
        QCOMPARE(ended.count(), 1);
        QCOMPARE(ended.first().at(5).toBool(), true);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(engine->state(), State::ShortBreak); // the break, not the work phase after it
        QCOMPARE(engine->completedCycles(), 1);
    }

    void pauseAfterTheDeadlineCompletesFirst()
    {
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        engine->start();
        clock.advanceSeconds(25 * 60 + 1);
        engine->pause(); // manual mode: the phase ends, nothing is left running to pause
        QCOMPARE(ended.count(), 1);
        QCOMPARE(ended.first().at(5).toBool(), true);
        QCOMPARE(engine->state(), State::Idle);
        QCOMPARE(engine->phase(), Phase::ShortBreak);
    }

        void oneMinuteWarningIsNotEmittedForShortPhases()
    {
        engine->setWorkSeconds(60);
        QSignalSpy warning(engine.get(), &TimerEngine::oneMinuteRemaining);
        engine->start();
        elapse(30);
        QCOMPARE(warning.count(), 0);

        engine->stop();
        engine->setWorkSeconds(45);
        engine->start();
        elapse(10);
        QCOMPARE(warning.count(), 0);
    }

    void stopAfterTheDeadlineWithAutoStartRecordsOnlyTheFinishedPhase()
    {
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        engine->start();
        clock.advanceSeconds(25 * 60 + 1);
        engine->stop(); // the break has auto-started but not run: it is not recorded
        QCOMPARE(ended.count(), 1);
        QCOMPARE(ended.first().at(5).toBool(), true);
        QCOMPARE(engine->state(), State::Idle);

        engine->start();
        clock.advanceSeconds(25 * 60 + 1);
        engine->startCustomWork(60);
        QCOMPARE(ended.count(), 2);
        QCOMPARE(ended.last().at(5).toBool(), true);
        QCOMPARE(engine->state(), State::Working);
    }

    void commandsFromASessionEndedListenerDoNotEndThePhaseTwice()
    {
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        connect(engine.get(), &TimerEngine::sessionEnded, this, [this]() {
            engine->refresh();
            engine->pause();
            engine->skip();
            engine->stop();
        });
        engine->start();
        elapse(25 * 60);
        QCOMPARE(ended.count(), 1);
        QCOMPARE(engine->completedCycles(), 1);
        QCOMPARE(engine->state(), State::ShortBreak);
        disconnect(engine.get(), &TimerEngine::sessionEnded, this, nullptr);
    }

    void startCustomWorkRunsOneWorkPhase()
    {
        engine->startCustomWork(120);
        QCOMPARE(engine->state(), State::Working);
        QCOMPARE(engine->totalSeconds(), 120);
        QCOMPARE(engine->workSeconds(), 25 * 60); // the preset is untouched
        elapse(120);
        QCOMPARE(engine->state(), State::Idle);
        QCOMPARE(engine->phase(), Phase::ShortBreak);
        QCOMPARE(engine->completedCycles(), 1);

        engine->skip(); // -> pending work, back to the normal length
        QCOMPARE(engine->totalSeconds(), 25 * 60);
        engine->start();
        QCOMPARE(engine->totalSeconds(), 25 * 60);
    }

    void startCustomWorkEndsTheActivePhaseAsInterrupted()
    {
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        engine->start();
        elapse(100);
        engine->startCustomWork(0); // clamped to 1 s
        QCOMPARE(ended.count(), 1);
        QVERIFY(!ended.at(0).at(5).toBool());
        QCOMPARE(engine->state(), State::Working);
        QCOMPARE(engine->totalSeconds(), 1);
    }

    void commandsAreNoOpsInWrongState()
    {
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        engine->pause();
        engine->resume();
        engine->stop();
        engine->skip();
        QCOMPARE(engine->state(), State::Idle);
        QCOMPARE(ended.count(), 0);
    }

    // --- More time ------------------------------------------------------------

    void extendAddsToTheRunningPhase()
    {
        QSignalSpy extended(engine.get(), &TimerEngine::phaseExtended);
        QVERIFY(!engine->isExtendable());
        QVERIFY(!engine->extend());
        engine->start();
        QVERIFY(engine->isExtendable());
        QVERIFY(!engine->isContinuable());
        elapse(60);
        QVERIFY(engine->extend());
        QCOMPARE(engine->remainingSeconds(), 29 * 60);
        QCOMPARE(engine->totalSeconds(), 30 * 60);
        QCOMPARE(engine->state(), State::Working);
        QCOMPARE(extended.size(), 1);
        QCOMPARE(extended.at(0).at(0).value<Phase>(), Phase::Work);
        QCOMPARE(extended.at(0).at(1).toBool(), false);
        QCOMPARE(extended.at(0).at(2).value<TimerEngine::ExtendSource>(), TimerEngine::ExtendSource::App);
        QCOMPARE(extended.at(0).at(3).toInt(), 5 * 60);
    }

    void extendWorksWhilePaused()
    {
        engine->start();
        elapse(60);
        engine->pause();
        QVERIFY(engine->extend(120));
        QCOMPARE(engine->state(), State::Paused);
        QCOMPARE(engine->remainingSeconds(), 26 * 60);
        engine->resume();
        elapse(26 * 60);
        QCOMPARE(engine->state(), State::Idle);
        QCOMPARE(engine->phase(), Phase::ShortBreak);
    }

    void extendStopsAtTheLongestPhase()
    {
        engine->setWorkSeconds(PresetLimits::MaxWorkSeconds - 60);
        engine->start();
        QVERIFY(engine->extend());
        QCOMPARE(engine->totalSeconds(), PresetLimits::MaxWorkSeconds);
        QVERIFY(!engine->extend());
    }

    void extendBringsTheOneMinuteWarningBack()
    {
        QSignalSpy warned(engine.get(), &TimerEngine::oneMinuteRemaining);
        engine->start();
        elapse(24 * 60 + 30);
        QCOMPARE(warned.size(), 1);
        QVERIFY(engine->extend());
        elapse(5 * 60);
        QCOMPARE(warned.size(), 2);
    }

    void workThatEndedCanGoOn()
    {
        QSignalSpy reopened(engine.get(), &TimerEngine::phaseReopened);
        QSignalSpy extended(engine.get(), &TimerEngine::phaseExtended);
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        engine->start();
        elapse(25 * 60);
        QCOMPARE(engine->state(), State::Idle);
        QCOMPARE(engine->phase(), Phase::ShortBreak);
        QCOMPARE(engine->completedCycles(), 1);
        QVERIFY(engine->isExtendable());
        QVERIFY(engine->isContinuable());
        QCOMPARE(engine->finishedPhase(), Phase::Work);

        elapse(30);
        QVERIFY(engine->extend()); // while Idle, extend() takes the ended phase up again
        QCOMPARE(reopened.size(), 1);
        QCOMPARE(extended.at(0).at(1).toBool(), true);
        QCOMPARE(engine->state(), State::Working);
        QCOMPARE(engine->completedCycles(), 0);
        QCOMPARE(engine->remainingSeconds(), 5 * 60);
        QCOMPARE(engine->totalSeconds(), 30 * 60);
        QVERIFY(!engine->isContinuable());

        elapse(5 * 60);
        QCOMPARE(ended.size(), 2);
        const QList<QVariant> last = ended.at(1);
        QCOMPARE(last.at(1).toDateTime(), clock.base());
        QCOMPARE(last.at(3).toLongLong(), 30 * 60 * 1000LL);
        QCOMPARE(last.at(4).toLongLong(), 30 * 60 * 1000LL);
        QVERIFY(last.at(5).toBool());
        QCOMPARE(engine->phase(), Phase::ShortBreak);
        QCOMPARE(engine->completedCycles(), 1);
        QVERIFY(engine->isContinuable()); // and once more
    }

    void phaseThatStartedByItselfGivesWay()
    {
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        engine->start();
        elapse(25 * 60);
        QCOMPARE(engine->state(), State::ShortBreak);
        QVERIFY(engine->isContinuable());
        elapse(60);
        // extend() lengthens the break that runs; continueFinished() takes the work up again.
        QVERIFY(engine->continueFinished());
        QCOMPARE(engine->state(), State::Working);
        QCOMPARE(ended.size(), 1); // the break is not recorded
        elapse(5 * 60);
        QCOMPARE(ended.size(), 2);
        QCOMPARE(engine->state(), State::ShortBreak);
    }

    void breakThatEndedCanGoOn()
    {
        engine->start();
        elapse(25 * 60);
        engine->start(); // the short break
        elapse(5 * 60);
        QCOMPARE(engine->phase(), Phase::Work);
        QCOMPARE(engine->finishedPhase(), Phase::ShortBreak);
        QVERIFY(engine->extend());
        QCOMPARE(engine->state(), State::ShortBreak);
        QCOMPARE(engine->completedCycles(), 1);
        elapse(5 * 60);
        QCOMPARE(engine->state(), State::Idle);
        QCOMPARE(engine->phase(), Phase::Work);
    }

    void longBreakThatEndedKeepsTheSetEnded()
    {
        engine->setCyclesBeforeLong(1);
        engine->start();
        elapse(25 * 60);
        QCOMPARE(engine->phase(), Phase::LongBreak);
        engine->start();
        elapse(15 * 60);
        QCOMPARE(engine->completedCycles(), 0);
        QVERIFY(engine->continueFinished());
        QCOMPARE(engine->state(), State::LongBreak);
        QCOMPARE(engine->completedCycles(), 1);
        elapse(5 * 60);
        QCOMPARE(engine->completedCycles(), 0);
        QCOMPARE(engine->phase(), Phase::Work);
    }

    void onlyRightAfterTheEnd()
    {
        engine->start();
        elapse(25 * 60);
        clock.advanceMs(TimerEngine::kReopenWindowMs);
        QVERIFY(!engine->isContinuable());
        QVERIFY(!engine->isExtendable());
        QVERIFY(!engine->extend());
        QCOMPARE(engine->state(), State::Idle);
    }

    void anyOtherCommandEndsTheChance()
    {
        engine->start();
        elapse(25 * 60);
        engine->skip(); // the pending break is passed over
        QVERIFY(!engine->isContinuable());

        engine->stop();
        engine->setAutoStartMode(TimerEngine::AutoStartMode::BreaksOnly);
        engine->start();
        elapse(25 * 60);
        engine->pause(); // the break that started by itself is the one wanted
        QVERIFY(!engine->isContinuable());
        QVERIFY(!engine->continueFinished());
    }

    void stoppedEarlyAfterGoingOnIsStillCompleted()
    {
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        engine->start();
        elapse(25 * 60);
        QVERIFY(engine->continueFinished());
        elapse(60);
        engine->stop();
        QCOMPARE(ended.size(), 2);
        QCOMPARE(ended.at(1).at(3).toLongLong(), 26 * 60 * 1000LL);
        QCOMPARE(ended.at(1).at(4).toLongLong(), 26 * 60 * 1000LL);
        QVERIFY(ended.at(1).at(5).toBool());
        QCOMPARE(engine->completedCycles(), 0);
    }

    void skippingAfterGoingOnCountsTheCycleOnce()
    {
        engine->start();
        elapse(25 * 60);
        QVERIFY(engine->continueFinished());
        elapse(60);
        engine->skip();
        QCOMPARE(engine->state(), State::ShortBreak);
        QCOMPARE(engine->completedCycles(), 1);
    }

    void extendableIsAnnounced()
    {
        QSignalSpy changed(engine.get(), &TimerEngine::extendableChanged);
        engine->start();
        QCOMPARE(changed.size(), 1);
        elapse(25 * 60); // still extendable, but now by taking the work up again
        QCOMPARE(changed.size(), 2);
        engine->reset();
        QCOMPARE(changed.size(), 3);
        QVERIFY(!engine->isExtendable());
    }

    void aPhaseOfAnEarlierRunComesBackPaused()
    {
        QSignalSpy restored(engine.get(), &TimerEngine::phaseRestored);
        const QDateTime started = clock.wallTime().addSecs(-3600);
        QVERIFY(!engine->restorePaused(Phase::Work, 0, 0, started, 0));
        QVERIFY(engine->restorePaused(Phase::LongBreak, 15 * 60 * 1000, 6 * 60 * 1000, started, 4));
        QCOMPARE(engine->state(), State::Paused);
        QCOMPARE(engine->phase(), Phase::LongBreak);
        QCOMPARE(engine->remainingSeconds(), 6 * 60);
        QCOMPARE(engine->completedCycles(), 4);
        QCOMPARE(engine->phaseStartedAt(), started);
        QCOMPARE(restored.size(), 1);
        // Only while idle.
        QVERIFY(!engine->restorePaused(Phase::Work, 60 * 1000, 30 * 1000, started, 0));
        QSignalSpy ended(engine.get(), &TimerEngine::sessionEnded);
        engine->resume();
        elapse(6 * 60);
        QCOMPARE(ended.size(), 1);
        QCOMPARE(ended.at(0).at(1).toDateTime(), started);
        QCOMPARE(engine->completedCycles(), 0);
    }
};

QTEST_GUILESS_MAIN(TimerEngineTest)
#include "tst_timerengine.moc"
