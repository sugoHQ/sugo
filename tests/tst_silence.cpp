// SPDX-License-Identifier: GPL-3.0-or-later
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include "Localization.h"

#include <memory>

#include "FakeClock.h"
#include "FakeInhibitor.h"
#include "PresetModel.h"
#include "SilenceController.h"
#include "TimerBinding.h"
#include "TimerEngine.h"

using State = TimerEngine::State;

class SilenceTest : public QObject
{
    Q_OBJECT

private:
    FakeClock clock;
    std::unique_ptr<TimerEngine> engine;
    std::unique_ptr<PresetModel> presets;
    std::unique_ptr<TimerBinding> binding;
    std::unique_ptr<FakeInhibitor> inhibitor;
    std::unique_ptr<SilenceController> silence;

    void elapse(qint64 seconds)
    {
        clock.advanceSeconds(seconds);
        engine->refresh();
    }

    void enableSilenceOnDefaultTimer()
    {
        QVERIFY(presets->setOption(PresetModel::DefaultUuid, PresetOption::SilenceWhileWorking, true));
    }

private Q_SLOTS:
    void initTestCase()
    {
    }

    void init()
    {
        clock = FakeClock();
        engine = std::make_unique<TimerEngine>(&clock, nullptr);
        presets = std::make_unique<PresetModel>();
        binding = std::make_unique<TimerBinding>(engine.get(), presets.get());
        inhibitor = std::make_unique<FakeInhibitor>();
        silence = std::make_unique<SilenceController>();
        silence->attach(engine.get(), presets.get(), inhibitor.get());
    }

    void cleanup()
    {
        silence.reset();
        inhibitor.reset();
        binding.reset();
        presets.reset();
        engine.reset();
    }

    // --- SilenceController ---------------------------------------------------

    void optionOffNeverSilences()
    {
        // The default timer silences; switched off, nothing is silenced.
        presets->setOption(presets->currentUuid(), PresetOption::SilenceWhileWorking, false);
        engine->start();
        elapse(60);
        QVERIFY(!inhibitor->isInhibited());
        QVERIFY(!inhibitor->calls.contains(QStringLiteral("inhibit")));
        QVERIFY(!silence->active());
    }

    void silencesExactlyWhileWorking()
    {
        enableSilenceOnDefaultTimer();
        QVERIFY(!inhibitor->isInhibited()); // idle

        engine->start();
        QVERIFY(inhibitor->isInhibited());
        QVERIFY(silence->active());
        QVERIFY(!inhibitor->lastReason.isEmpty());

        engine->pause();
        QVERIFY(!inhibitor->isInhibited()); // paused: notifications are back
        QVERIFY(!silence->active());

        engine->resume();
        QVERIFY(inhibitor->isInhibited());

        engine->stop();
        QVERIFY(!inhibitor->isInhibited());
        QVERIFY(!silence->active());
    }

    void workEndReleasesAndTheNextWorkPhaseSilencesAgain()
    {
        enableSilenceOnDefaultTimer();
        QVERIFY(presets->setOption(PresetModel::DefaultUuid, PresetOption::AutoStartMode, 2));

        engine->start();
        QVERIFY(inhibitor->isInhibited());

        elapse(25 * 60); // work ends, the short break auto-starts
        QCOMPARE(engine->state(), State::ShortBreak);
        QVERIFY(!inhibitor->isInhibited());

        elapse(5 * 60); // break ends, next work phase auto-starts
        QCOMPARE(engine->state(), State::Working);
        // Silenced again once the end of the break has been announced: on the next turn.
        QVERIFY(!inhibitor->isInhibited());
        QTRY_VERIFY(inhibitor->isInhibited());
        QVERIFY(silence->active());
    }

    void workEndWithoutAnyBreakReleasesAndSilencesAgain()
    {
        // A timer with no breaks at all: the work phase that ends is followed by another work
        // phase, so the state never leaves Working. The silencing must still be released, or the
        // end-of-phase announcement waits for an inhibition that is never let go.
        enableSilenceOnDefaultTimer();
        QVERIFY(presets->update(PresetModel::DefaultUuid, {{QStringLiteral("shortBreakSeconds"), 0},
                                                          {QStringLiteral("longBreakSeconds"), 0}}));
        QVERIFY(presets->setOption(PresetModel::DefaultUuid, PresetOption::AutoStartMode, 2));

        engine->start();
        QVERIFY(inhibitor->isInhibited());

        elapse(25 * 60); // work ends, the next work phase starts at once
        QCOMPARE(engine->state(), State::Working);
        QVERIFY(!inhibitor->isInhibited()); // released for the announcement
        QTRY_VERIFY(inhibitor->isInhibited()); // and taken again for the new phase
        QVERIFY(silence->active());
    }

    void anAutoStartedWorkPhaseStoppedAtOnceIsNeverSilenced()
    {
        enableSilenceOnDefaultTimer();
        QVERIFY(presets->setOption(PresetModel::DefaultUuid, PresetOption::AutoStartMode, 2));
        engine->start();
        elapse(25 * 60);
        elapse(5 * 60); // the break ends, work auto-starts
        engine->stop(); // before the put-off silencing comes
        QTest::qWait(20);
        QVERIFY(!inhibitor->isInhibited());
        QCOMPARE(inhibitor->calls.last(), QStringLiteral("release"));
    }

    void workEndWithoutAutoStartReleases()
    {
        enableSilenceOnDefaultTimer();
        engine->start();
        elapse(25 * 60);
        QCOMPARE(engine->state(), State::Idle);
        QVERIFY(!inhibitor->isInhibited());
    }

    void skippingMovesBetweenSilencedAndReleased()
    {
        enableSilenceOnDefaultTimer();
        engine->start();
        QVERIFY(inhibitor->isInhibited());
        engine->skip(); // work -> short break
        QVERIFY(!inhibitor->isInhibited());
        engine->skip(); // short break -> work
        QVERIFY(inhibitor->isInhibited());
    }

    void durationsFollowTheTimersOwnValues()
    {
        const QString study = presets->create({{QStringLiteral("name"), QStringLiteral("Study")},
                                               {QStringLiteral("workSeconds"), 50 * 60},
                                               {QStringLiteral("shortBreakSeconds"), 10 * 60}});
        presets->setOption(study, PresetOption::SilenceWhileWorking, true);
        presets->setOption(study, PresetOption::AutoStartMode, 2);
        presets->setCurrentUuid(study);

        engine->start();
        QVERIFY(inhibitor->isInhibited());
        elapse(50 * 60 - 1); // one second before this timer's work ends
        QVERIFY(inhibitor->isInhibited());
        elapse(1);           // work done: silencing ends exactly now
        QCOMPARE(engine->state(), State::ShortBreak);
        QVERIFY(!inhibitor->isInhibited());
        elapse(10 * 60 - 1);
        QVERIFY(!inhibitor->isInhibited());
        elapse(1);           // this timer's 10 minute break is over
        QCOMPARE(engine->state(), State::Working);
        QTRY_VERIFY(inhibitor->isInhibited());
    }

    void turningTheOptionOffDuringWorkReleasesAtOnce()
    {
        enableSilenceOnDefaultTimer();
        engine->start();
        QVERIFY(inhibitor->isInhibited());

        presets->setOption(PresetModel::DefaultUuid, PresetOption::SilenceWhileWorking, false);
        QVERIFY(!inhibitor->isInhibited());
        QVERIFY(!silence->active());

        presets->setOption(PresetModel::DefaultUuid, PresetOption::SilenceWhileWorking, true);
        QVERIFY(inhibitor->isInhibited()); // and back on while the phase still runs
    }

    void editingAnotherTimerDoesNotTouchTheInhibitor()
    {
        enableSilenceOnDefaultTimer();
        const QString other = presets->create({});
        engine->start();
        QVERIFY(inhibitor->isInhibited());
        inhibitor->calls.clear();

        presets->setOption(other, PresetOption::SilenceWhileWorking, true);
        presets->update(other, {{QStringLiteral("workSeconds"), 10 * 60}});
        presets->update(PresetModel::DefaultUuid, {{QStringLiteral("name"), QStringLiteral("Renamed")}});
        QVERIFY(inhibitor->calls.isEmpty());
        QVERIFY(inhibitor->isInhibited());
    }

    void switchingTimerDuringWorkNeverLeavesNotificationsOff()
    {
        enableSilenceOnDefaultTimer();
        const QString other = presets->create({});
        engine->start();
        QVERIFY(inhibitor->isInhibited());

        presets->setCurrentUuid(other); // the binding resets the engine, ending the phase
        QCOMPARE(engine->state(), State::Idle);
        QVERIFY(!inhibitor->isInhibited());

        engine->start(); // the other timer has the option off
        QVERIFY(!inhibitor->isInhibited());
    }

    void deletingTheCurrentTimerDuringWorkReleases()
    {
        const QString temp = presets->create({});
        presets->setOption(temp, PresetOption::SilenceWhileWorking, true);
        presets->setCurrentUuid(temp);
        engine->start();
        QVERIFY(inhibitor->isInhibited());

        presets->remove(temp); // falls back to the default timer, which does not silence
        QVERIFY(!inhibitor->isInhibited());
    }

    void unavailableDesktopIsReportedAndNeverClaimsToSilence()
    {
        inhibitor->available = false;
        enableSilenceOnDefaultTimer();
        QVERIFY(!silence->available());

        engine->start();
        QVERIFY(!silence->active()); // the UI must not claim notifications are silenced
    }

    void aRefusedInhibitionIsNotShownAsActive()
    {
        enableSilenceOnDefaultTimer();
        inhibitor->refuse = true;
        engine->start();
        QVERIFY(inhibitor->calls.contains(QStringLiteral("inhibit")));
        QVERIFY(!silence->active());

        // The desktop grants it later (e.g. its notification service came back).
        QSignalSpy activeChanged(silence.get(), &SilenceController::activeChanged);
        inhibitor->refuse = false;
        inhibitor->inhibit(QStringLiteral("x"));
        QVERIFY(silence->active());
        QCOMPARE(activeChanged.count(), 1);
    }

    void nothingSilencesAgainWhileShuttingDown()
    {
        enableSilenceOnDefaultTimer();
        engine->start();
        inhibitor->deferReleases = true;
        // Something changes the current timer while shutdown() waits for the desktop.
        QTimer::singleShot(20, presets.get(), [this]() {
            presets->update(PresetModel::DefaultUuid, {{QStringLiteral("name"), QStringLiteral("Renamed")}});
        });
        QTimer::singleShot(80, inhibitor.get(), [this]() { inhibitor->completeRelease(); });
        silence->shutdown();
        QVERIFY(!inhibitor->isInhibited());
        QCOMPARE(inhibitor->calls.last(), QStringLiteral("release"));
    }

    void shutdownReleasesAndWaitsForConfirmation()
    {
        enableSilenceOnDefaultTimer();
        engine->start();
        QVERIFY(inhibitor->isInhibited());

        inhibitor->deferReleases = true; // the desktop is slow to confirm
        QTimer::singleShot(80, inhibitor.get(), [this]() { inhibitor->completeRelease(); });
        silence->shutdown();
        QVERIFY(!inhibitor->isInhibited());
    }

    void shutdownGivesUpOnAnUnresponsiveDesktop()
    {
        enableSilenceOnDefaultTimer();
        engine->start();
        inhibitor->deferReleases = true; // never confirms
        QElapsedTimer timer;
        timer.start();
        silence->shutdown();
        QVERIFY(timer.elapsed() < 3000); // bounded: quitting is never held hostage
    }

    // --- NotificationInhibitor helpers ---------------------------------------

    void whenReleasedRunsAtOnceWhenNothingIsHeld()
    {
        bool ran = false;
        inhibitor->whenReleased(this, [&]() { ran = true; });
        QVERIFY(ran);
    }

    void whenReleasedWaitsForTheReleaseAndRunsOnce()
    {
        inhibitor->inhibit(QStringLiteral("x"));
        inhibitor->deferReleases = true;
        inhibitor->release();
        QVERIFY(inhibitor->isInhibited()); // release not confirmed yet

        int runs = 0;
        inhibitor->whenReleased(this, [&]() { ++runs; });
        QCOMPARE(runs, 0);

        inhibitor->completeRelease();
        QCOMPARE(runs, 1);

        // A later inhibit/release cycle must not run it again.
        inhibitor->inhibit(QStringLiteral("y"));
        inhibitor->completeRelease();
        inhibitor->release();
        inhibitor->completeRelease();
        QCOMPARE(runs, 1);
    }

    void whenReleasedFallsBackToTheTimeout()
    {
        inhibitor->inhibit(QStringLiteral("x"));
        inhibitor->deferReleases = true;
        inhibitor->release(); // never completes

        int runs = 0;
        inhibitor->whenReleased(this, [&]() { ++runs; }, 80);
        QCOMPARE(runs, 0);
        QTRY_COMPARE_WITH_TIMEOUT(runs, 1, 2000);
        inhibitor->completeRelease(); // late confirmation: still only once
        QCOMPARE(runs, 1);
    }

    void whenReleasedIsDroppedIfTheContextIsGone()
    {
        inhibitor->inhibit(QStringLiteral("x"));
        inhibitor->deferReleases = true;
        inhibitor->release();

        bool ran = false;
        auto *context = new QObject;
        inhibitor->whenReleased(context, [&]() { ran = true; });
        delete context;

        inhibitor->completeRelease();
        QTest::qWait(150);
        QVERIFY(!ran);
    }

    void followsTheInhibitorBecomingAvailable()
    {
        // A macOS Focus: the shortcuts are created while Sugo runs.
        inhibitor->available = false;
        enableSilenceOnDefaultTimer();
        QSignalSpy availableChanged(silence.get(), &SilenceController::availableChanged);
        engine->start();
        QVERIFY(!silence->available());
        QVERIFY(!silence->active());

        inhibitor->available = true;
        Q_EMIT inhibitor->availableChanged();
        QCOMPARE(availableChanged.count(), 1);
        QVERIFY(silence->available());
        QVERIFY(silence->active());
    }

    void waitUntilReleasedReportsTimeoutAndSuccess()
    {
        inhibitor->inhibit(QStringLiteral("x"));
        inhibitor->deferReleases = true;
        inhibitor->release();
        QVERIFY(!inhibitor->waitUntilReleased(80));

        QTimer::singleShot(40, inhibitor.get(), [this]() { inhibitor->completeRelease(); });
        QVERIFY(inhibitor->waitUntilReleased(2000));
    }
};

QTEST_GUILESS_MAIN(SilenceTest)
#include "tst_silence.moc"
