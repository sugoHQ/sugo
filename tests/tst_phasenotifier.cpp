// SPDX-License-Identifier: GPL-3.0-or-later
#include "Localization.h"

#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "AppSettings.h"
#include "Autostart.h"
#include "FakeClock.h"
#include "FakeInhibitor.h"
#include "FakeNotifier.h"
#include "PhaseNotifier.h"
#include "PresetModel.h"
#include "PresetSettings.h"
#include "Settings.h"
#include "SilenceController.h"
#include "TimerEngine.h"

/// The end-of-phase announcement depends on the desktop handing notifications back
/// (NotificationInhibitor::whenReleased). These tests drive that dependency end to end, then
/// the notification itself: its text, its actions and the task note.
class PhaseNotifierTest : public QObject
{
    Q_OBJECT

private:
    FakeClock clock;
    std::unique_ptr<TimerEngine> engine;
    std::unique_ptr<PresetModel> presets;
    std::unique_ptr<FakeInhibitor> inhibitor;
    std::unique_ptr<FakeNotifier> notifications;
    std::unique_ptr<PhaseNotifier> notifier;
    QTemporaryDir configDir;
    std::unique_ptr<AppSettings> settings;

    /// Ends a work phase while notifications are silenced and their release is in flight.
    void endWorkWhileSilenced()
    {
        engine->start();
        inhibitor->inhibit(QStringLiteral("focus"));
        clock.advanceSeconds(25 * 60);
        // What SilenceController does as the phase ends: ask for the release.
        inhibitor->release();
        engine->refresh();
    }

    void endWork()
    {
        engine->start();
        clock.advanceSeconds(25 * 60);
        engine->refresh();
    }

    /// A timer of our own, so its options can be changed. No sound: tests stay quiet.
    QString useTimer(const QString &name = QStringLiteral("Focus"))
    {
        const QString uuid = presets->create({{QStringLiteral("name"), name}});
        presets->setCurrentUuid(uuid);
        presets->setOption(uuid, PresetOption::SoundOnEnd, false);
        return uuid;
    }

    /// A notifier with settings (the task note needs them) and a recorded work session 42.
    void withSettings()
    {
        const QString path = configDir.filePath(QStringLiteral("sugo.conf"));
        QFile::remove(path); // the defaults, whatever an earlier test changed
        settings = std::make_unique<AppSettings>(Settings::open(path),
                                                 std::make_unique<UnsupportedAutostart>(), false);
        notifier = std::make_unique<PhaseNotifier>(engine.get(), presets.get(), inhibitor.get(), notifications.get(), settings.get());
        notifier->setReleaseTimeoutMs(300);
        notifier->setLastWorkSessionIdProvider([]() { return qint64(42); });
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void init()
    {
        clock = FakeClock();
        engine = std::make_unique<TimerEngine>(&clock, nullptr);
        presets = std::make_unique<PresetModel>();
        inhibitor = std::make_unique<FakeInhibitor>();
        notifications = std::make_unique<FakeNotifier>();
        notifier = std::make_unique<PhaseNotifier>(engine.get(), presets.get(), inhibitor.get(), notifications.get());
        notifier->setReleaseTimeoutMs(300);
    }

    void cleanup()
    {
        notifier.reset();
        settings.reset();
        notifications.reset();
        inhibitor.reset();
        presets.reset();
        engine.reset();
    }

    void preAlarmPlaysOneMinuteBeforeTheEnd()
    {
        withSettings();
        QStringList played;
        settings->setSoundPlayerForTesting([&played](const QString &sound, int volume) {
            played.append(sound + QLatin1Char('@') + QString::number(volume));
        });
        settings->setPreAlarmSound(QStringLiteral("bell"));
        settings->setSoundVolume(40);
        useTimer();

        engine->start();
        clock.advanceSeconds(23 * 60);
        engine->refresh();
        QVERIFY(played.isEmpty()); // two minutes left
        clock.advanceSeconds(61);
        engine->refresh();
        QCOMPARE(played, QStringList{QStringLiteral("bell@40")});

        // Switched off: no warning.
        engine->stop();
        played.clear();
        settings->setPreAlarmEnabled(false);
        engine->start();
        clock.advanceSeconds(24 * 60 + 1);
        engine->refresh();
        QVERIFY(played.isEmpty());
    }

    void theEndOfAPhasePlaysTheChosenSound()
    {
        withSettings();
        QStringList played;
        settings->setSoundPlayerForTesting([&played](const QString &sound, int) { played.append(sound); });
        settings->setPreAlarmEnabled(false);
        settings->setSoundTheme(QStringLiteral("complete"));
        const QString uuid = useTimer();
        presets->setOption(uuid, PresetOption::SoundOnEnd, true);

        engine->start();
        clock.advanceSeconds(25 * 60);
        engine->refresh();
        QCOMPARE(played, QStringList{QStringLiteral("complete")});

        // The timer's option off: silence.
        presets->setOption(uuid, PresetOption::SoundOnEnd, false);
        played.clear();
        engine->start();
        clock.advanceSeconds(5 * 60);
        engine->refresh();
        QVERIFY(played.isEmpty());
    }

    void aReleaseThatNeverCompletesDoesNotSwallowTheAnnouncement()
    {
        inhibitor->deferReleases = true; // the desktop never confirms
        QElapsedTimer waited;
        waited.start();
        endWorkWhileSilenced();
        QVERIFY(inhibitor->isInhibited());

        // Held back while the release may still come...
        QTest::qWait(100);
        QCOMPARE(notifications->shown.size(), 0);
        // ...and sent anyway once the timeout is over.
        QTRY_COMPARE_WITH_TIMEOUT(notifications->shown.size(), 1, 2000);
        // The timeout is 300 ms, but a coarse timer (QTimer's default) may fire up to 5% early.
        QVERIFY(waited.elapsed() >= 250);
        QCOMPARE(notifications->last().title, QStringLiteral("Work session finished"));

        // A release that finally arrives later does not announce it a second time.
        inhibitor->completeRelease();
        QTest::qWait(50);
        QCOMPARE(notifications->shown.size(), 1);
    }

    void aConfirmedReleaseAnnouncesAtOnce()
    {
        inhibitor->deferReleases = true;
        endWorkWhileSilenced();
        QCOMPARE(notifications->shown.size(), 0);
        inhibitor->completeRelease();
        QCOMPARE(notifications->shown.size(), 1); // no need to wait for the timeout
    }

    void withNothingSilencedItIsAnnouncedStraightAway()
    {
        engine->start();
        clock.advanceSeconds(25 * 60);
        engine->refresh();
        QCOMPARE(notifications->shown.size(), 1);
    }

    void withoutAnyInhibitorSupportItIsAnnouncedStraightAway()
    {
        inhibitor->available = false; // like the no-op fallback: nothing is ever held
        engine->start();
        clock.advanceSeconds(25 * 60);
        engine->refresh();
        QCOMPARE(notifications->shown.size(), 1);
    }

    void theEndOfWorkOffersToStartOrSkipTheBreak()
    {
        endWork();
        QCOMPARE(notifications->shown.size(), 1);
        QCOMPARE(notifications->lastContext.data(), notifier.get());
        const auto &shown = notifications->last();
        QCOMPARE(shown.title, QStringLiteral("Work session finished"));
        QCOMPARE(shown.text, QStringLiteral("Time for a short break: 5 minutes."));
        QCOMPARE(shown.iconName, QStringLiteral("stream.sugo.app"));
        QCOMPARE(notifications->actionLabels(), (QStringList{QStringLiteral("Start Short Break"), QStringLiteral("Skip Break"),
                                                              QStringLiteral("5 More Minutes")}));
        QVERIFY(!shown.reply); // no settings, no task note

        QVERIFY(engine->isIdle());
        QVERIFY(notifications->invoke(QStringLiteral("Start Short Break")));
        QCOMPARE(engine->phase(), TimerEngine::Phase::ShortBreak);
        QVERIFY(engine->isRunning());
    }

    void skipActsOnThePendingBreak()
    {
        endWork();
        QVERIFY(notifications->invoke(QStringLiteral("Skip Break")));
        QCOMPARE(engine->phase(), TimerEngine::Phase::Work);
    }

    void theEndOfABreakOffersToStartWork()
    {
        endWork();
        engine->start();
        clock.advanceSeconds(5 * 60);
        engine->refresh();
        QCOMPARE(notifications->shown.size(), 2);
        QCOMPARE(notifications->last().title, QStringLiteral("Break is over"));
        QCOMPARE(notifications->last().text, QStringLiteral("Time to get back to work."));
        QCOMPARE(notifications->actionLabels(), (QStringList{QStringLiteral("Start Work"), QStringLiteral("Skip"), QStringLiteral("5 More Minutes")}));
    }

    void anAutoStartedBreakCanBePaused()
    {
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        endWork();
        QVERIFY(engine->isRunning());
        QCOMPARE(notifications->last().text, QStringLiteral("Short break started: 5 minutes."));
        QCOMPARE(notifications->actionLabels(), (QStringList{QStringLiteral("Pause Break"), QStringLiteral("Skip Break"),
                                                              QStringLiteral("5 More Minutes")}));
        QVERIFY(notifications->invoke(QStringLiteral("Pause Break")));
        QVERIFY(engine->isPaused());
        QCOMPARE(engine->phase(), TimerEngine::Phase::ShortBreak);
    }

    void anAutoStartedWorkPhaseIsAnnouncedBeforeItSilences()
    {
        // The real order: SilenceController is wired before the notifier (src/main.cpp).
        notifier.reset();
        SilenceController silence;
        silence.attach(engine.get(), presets.get(), inhibitor.get());
        notifier = std::make_unique<PhaseNotifier>(engine.get(), presets.get(), inhibitor.get(), notifications.get());
        notifier->setReleaseTimeoutMs(300);

        const QString uuid = useTimer();
        QVERIFY(presets->setOption(uuid, PresetOption::SilenceWhileWorking, true));
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        endWork(); // the break auto-starts: released, then announced
        QVERIFY(!inhibitor->isInhibited());
        QCOMPARE(notifications->shown.size(), 1);

        inhibitor->calls.clear();
        clock.advanceSeconds(5 * 60);
        engine->refresh(); // the break ends, Work auto-starts
        QCOMPARE(engine->state(), TimerEngine::State::Working);
        // Announced at once, while nothing is silenced yet: the desktop shows it.
        QCOMPARE(notifications->shown.size(), 2);
        QCOMPARE(notifications->last().title, QStringLiteral("Break is over"));
        QCOMPARE(notifications->last().text, QStringLiteral("Work session started."));
        QVERIFY(inhibitor->calls.isEmpty());
        // Then the work phase is silenced as usual.
        QTRY_VERIFY(inhibitor->isInhibited());
        QCOMPARE(inhibitor->calls, QStringList{QStringLiteral("inhibit")});
        QVERIFY(silence.active());
        notifier.reset(); // before the local controller goes
    }

    void actionsOfAnOutdatedNotificationDoNothing()
    {
        endWork();
        engine->start(); // the break was started from the window instead
        QVERIFY(notifications->invoke(QStringLiteral("Skip Break")));
        QCOMPARE(engine->phase(), TimerEngine::Phase::ShortBreak);
        QVERIFY(engine->isRunning());
        QVERIFY(notifications->invoke(QStringLiteral("Start Short Break")));
        QVERIFY(engine->isRunning());
    }

    void fiveMoreMinutesTakesTheWorkUpAgain()
    {
        QSignalSpy extended(engine.get(), &TimerEngine::phaseExtended);
        endWork();
        QVERIFY(notifications->invoke(QStringLiteral("5 More Minutes")));
        QCOMPARE(engine->state(), TimerEngine::State::Working);
        QCOMPARE(engine->remainingSeconds(), TimerEngine::kExtendSeconds);
        QCOMPARE(extended.size(), 1);
        QCOMPARE(extended.at(0).at(2).value<TimerEngine::ExtendSource>(), TimerEngine::ExtendSource::Notification);
        // Pressed again (a second click, an old copy of the notification): nothing more.
        QVERIFY(notifications->invoke(QStringLiteral("5 More Minutes")));
        QCOMPARE(engine->remainingSeconds(), TimerEngine::kExtendSeconds);
    }

    void fiveMoreMinutesIsLateOnceTheBreakStarted()
    {
        endWork();
        engine->start(); // the break, from the window
        QVERIFY(notifications->invoke(QStringLiteral("5 More Minutes")));
        QCOMPARE(engine->state(), TimerEngine::State::ShortBreak);
    }

    void aClickOnTheNotificationShowsTheWindow()
    {
        QSignalSpy show(notifier.get(), &PhaseNotifier::showWindowRequested);
        endWork();
        QCOMPARE(notifications->last().defaultActionLabel, QStringLiteral("View"));
        notifications->last().defaultAction(QStringLiteral("token-1"));
        QCOMPARE(show.count(), 1);
        QCOMPARE(show.first().first().toString(), QStringLiteral("token-1")); // for the focus on Wayland
    }

    void withNotificationsOffOnlyTheSoundIsLeft()
    {
        withSettings();
        QStringList played;
        settings->setSoundPlayerForTesting([&played](const QString &sound, int) { played.append(sound); });
        settings->setPreAlarmEnabled(false);
        const QString uuid = useTimer();
        presets->setOption(uuid, PresetOption::NotifyOnEnd, false);
        presets->setOption(uuid, PresetOption::SoundOnEnd, true);
        endWork();
        QCOMPARE(played, QStringList{settings->soundTheme()});
        QVERIFY(notifications->shown.isEmpty());
    }

    void withNeitherNotificationNorSoundNothingHappens()
    {
        withSettings();
        QStringList played;
        settings->setSoundPlayerForTesting([&played](const QString &sound, int) { played.append(sound); });
        settings->setPreAlarmEnabled(false);
        const QString uuid = useTimer();
        presets->setOption(uuid, PresetOption::NotifyOnEnd, false);
        endWork();
        QVERIFY(played.isEmpty());
        QVERIFY(notifications->shown.isEmpty());
    }

    void theTaskNoteCanBeWrittenInTheNotification()
    {
        useTimer(QStringLiteral("Writing"));
        withSettings();
        QSignalSpy submitted(notifier.get(), &PhaseNotifier::taskNoteSubmitted);
        QSignalSpy prompted(notifier.get(), &PhaseNotifier::taskNotePromptRequested);
        endWork();

        const auto &shown = notifications->last();
        QVERIFY(shown.reply);
        QCOMPARE(shown.reply->label, QStringLiteral("Log Task"));
        QCOMPARE(shown.reply->placeholder, QStringLiteral("What did you accomplish?"));
        QCOMPARE(shown.reply->submitLabel, QStringLiteral("Save"));

        shown.reply->replied(QStringLiteral("  Chapter two  "));
        QCOMPARE(submitted.count(), 1);
        QCOMPARE(submitted.first().at(0).toLongLong(), 42);
        QCOMPARE(submitted.first().at(1).toString(), QStringLiteral("Chapter two"));
        shown.reply->replied(QStringLiteral("   ")); // nothing written: nothing saved
        QCOMPARE(submitted.count(), 1);

        // A desktop without a reply field: an action opens the prompt in the window.
        shown.reply->fallback(QStringLiteral("token-2"));
        QCOMPARE(prompted.count(), 1);
        QCOMPARE(prompted.first().at(0).toString(), QStringLiteral("Writing"));
        QCOMPARE(prompted.first().at(1).toLongLong(), 42);
        QCOMPARE(prompted.first().at(2).toString(), QStringLiteral("token-2"));

        // So does a click on the notification, while the option is on.
        shown.defaultAction(QString());
        QCOMPARE(prompted.count(), 2);
        settings->setPromptTaskNote(false);
        QSignalSpy show(notifier.get(), &PhaseNotifier::showWindowRequested);
        shown.defaultAction(QString());
        QCOMPARE(prompted.count(), 2);
        QCOMPARE(show.count(), 1);
    }

    void noTaskNoteWhenTheOptionIsOff()
    {
        useTimer();
        withSettings();
        settings->setPromptTaskNote(false);
        endWork();
        QVERIFY(!notifications->last().reply);
    }

    void noTaskNoteAfterABreak()
    {
        useTimer();
        withSettings();
        endWork();
        engine->start();
        clock.advanceSeconds(5 * 60);
        engine->refresh();
        QCOMPARE(notifications->shown.size(), 2);
        QVERIFY(!notifications->last().reply);
    }

    void withoutANotifierTheEndIsStillAnnounced()
    {
        withSettings();
        QStringList played;
        settings->setSoundPlayerForTesting([&played](const QString &sound, int) { played.append(sound); });
        settings->setPreAlarmEnabled(false);
        notifier = std::make_unique<PhaseNotifier>(engine.get(), presets.get(), inhibitor.get(), nullptr, settings.get());
        const QString uuid = useTimer();
        presets->setOption(uuid, PresetOption::SoundOnEnd, true);
        endWork();
        QCOMPARE(played, QStringList{settings->soundTheme()});
    }
};

QTEST_MAIN(PhaseNotifierTest)
#include "tst_phasenotifier.moc"
