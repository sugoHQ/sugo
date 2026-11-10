// SPDX-License-Identifier: GPL-3.0-or-later
#include <QSignalSpy>
#include <QTest>

#include "FakeClock.h"
#include "KRunnerService.h"
#include "PresetModel.h"
#include "TimerBinding.h"
#include "TimerEngine.h"

class TestKRunnerService : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testActions();
    void testMatchKeyword();
    void testMatchSubCommands();
    void testMatchCustomMinutes();
    void testRunCommands();
    void testRunOpen();
    void testKeywordMustBeAWholeWord();
    void testActionButtonsRunTheirOwnCommand();
    void testMatchesOfferOnlyRelevantActions();
    void testCustomMinutesDoNotChangeThePreset();
    void testPresetByName();
    void testStartingTheCurrentPresetResumesIt();
    void testStartResumesAPausedTimer();
    void testNothingIsOfferedBeforeTheTimerIsAttached();
    void testControlCanBeTurnedOff();
    void testAnotherPresetDoesNotThrowAwayTheActivePhase();
    void testStartingTheCurrentPausedPresetKeepsItsTime();
    void testUnknownPresetIsIgnored();
    void testCustomMinutesOutOfRangeAreIgnored();
    void testRunDoesOnlyOpenWhileControlIsOff();
    void testOtherPresetsAreNotOfferedToStartWhileActive();
    void testCustomMinutesDoNotThrowAwayTheActivePhase();

private:
    /// The engine as main.cpp wires it: choosing another preset resets it (TimerBinding).
    struct Fixture {
        FakeClock clock;
        TimerEngine timer{&clock, nullptr};
        PresetModel presets;
        TimerBinding binding{&timer, &presets};
        KRunnerService service{&timer, &presets};
        QString first;
        QString deep;
        Fixture()
        {
            first = presets.currentUuid();
            deep = presets.create({{QStringLiteral("name"), QStringLiteral("Deep Work")}});
        }
    };
};

void TestKRunnerService::testActions()
{
    TimerEngine timer;
    PresetModel presets;
    KRunnerService service(&timer, &presets);

    const auto actions = service.Actions();
    QCOMPARE(actions.size(), 5);
    QCOMPARE(actions.at(0).id, QStringLiteral("start"));
    QCOMPARE(actions.at(1).id, QStringLiteral("pause"));
    QCOMPARE(actions.at(2).id, QStringLiteral("resume"));
    QCOMPARE(actions.at(3).id, QStringLiteral("skip"));
    QCOMPARE(actions.at(4).id, QStringLiteral("stop"));
}

void TestKRunnerService::testMatchKeyword()
{
    TimerEngine timer;
    PresetModel presets;
    KRunnerService service(&timer, &presets);

    // Unrelated query returns nothing
    QVERIFY(service.Match(QStringLiteral("firefox")).isEmpty());
    QVERIFY(service.Match(QStringLiteral("")).isEmpty());

    // Pomodoro query returns matches
    const auto matches = service.Match(QStringLiteral("pomodoro"));
    QVERIFY(!matches.isEmpty());
    bool hasStart = false;
    bool hasOpen = false;
    for (const auto &m : matches) {
        if (m.id == QStringLiteral("start")) hasStart = true;
        if (m.id == QStringLiteral("open")) hasOpen = true;
    }
    QVERIFY(hasStart);
    QVERIFY(hasOpen);
}

void TestKRunnerService::testMatchSubCommands()
{
    TimerEngine timer;
    PresetModel presets;
    KRunnerService service(&timer, &presets);

    auto matches = service.Match(QStringLiteral("pomodoro start"));
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches.at(0).id, QStringLiteral("start"));
    QCOMPARE(matches.at(0).categoryRelevance, 100); // CategoryRelevance::Highest

    matches = service.Match(QStringLiteral("sugo pause"));
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches.at(0).id, QStringLiteral("pause"));

    matches = service.Match(QStringLiteral("pomodoro stop"));
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches.at(0).id, QStringLiteral("stop"));

    matches = service.Match(QStringLiteral("pomodoro skip"));
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches.at(0).id, QStringLiteral("skip"));
}

void TestKRunnerService::testMatchCustomMinutes()
{
    TimerEngine timer;
    PresetModel presets;
    KRunnerService service(&timer, &presets);

    const auto matches = service.Match(QStringLiteral("pomodoro 25"));
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches.at(0).id, QStringLiteral("custom:25"));
    QCOMPARE(matches.at(0).categoryRelevance, 100);
}

void TestKRunnerService::testRunCommands()
{
    TimerEngine timer;
    timer.setWorkSeconds(1500);
    PresetModel presets;
    KRunnerService service(&timer, &presets);

    QVERIFY(!timer.isRunning());

    // Start
    service.Run(QStringLiteral("start"), QString());
    QVERIFY(timer.isRunning());

    // Pause
    service.Run(QStringLiteral("pause"), QString());
    QVERIFY(timer.isPaused());

    // Resume
    service.Run(QStringLiteral("resume"), QString());
    QVERIFY(timer.isRunning());
    QVERIFY(!timer.isPaused());

    // Stop
    service.Run(QStringLiteral("stop"), QString());
    QVERIFY(!timer.isRunning());
}

void TestKRunnerService::testRunOpen()
{
    TimerEngine timer;
    PresetModel presets;
    KRunnerService service(&timer, &presets);

    QSignalSpy spy(&service, &KRunnerService::openRequested);
    service.Run(QStringLiteral("open"), QString());
    QCOMPARE(spy.count(), 1);
}

void TestKRunnerService::testKeywordMustBeAWholeWord()
{
    TimerEngine timer;
    PresetModel presets;
    KRunnerService service(&timer, &presets);

    QVERIFY(service.Match(QStringLiteral("tomatoes")).isEmpty());
    QVERIFY(service.Match(QStringLiteral("timers")).isEmpty());
    QVERIFY(service.Match(QStringLiteral("pomodorostart")).isEmpty());
    QVERIFY(!service.Match(QStringLiteral("Tomato")).isEmpty());
    QCOMPARE(service.Match(QStringLiteral("timer   stop")).value(0).id, QStringLiteral("stop"));
}

void TestKRunnerService::testActionButtonsRunTheirOwnCommand()
{
    TimerEngine timer;
    timer.setWorkSeconds(1500);
    PresetModel presets;
    KRunnerService service(&timer, &presets);

    service.Run(QStringLiteral("start"), QString());
    QVERIFY(timer.isRunning());

    // "Pause" pressed on the "Stop timer" result pauses; it does not stop.
    service.Run(QStringLiteral("stop"), QStringLiteral("pause"));
    QVERIFY(timer.isPaused());

    service.Run(QStringLiteral("resume"), QStringLiteral("stop"));
    QVERIFY(!timer.isActive());
}

void TestKRunnerService::testMatchesOfferOnlyRelevantActions()
{
    TimerEngine timer;
    timer.setWorkSeconds(1500);
    PresetModel presets;
    KRunnerService service(&timer, &presets);
    const QString actionsKey = QStringLiteral("actions");

    // Command results carry no buttons at all (an explicit empty list).
    auto matches = service.Match(QStringLiteral("pomodoro stop"));
    QVERIFY(matches.at(0).properties.contains(actionsKey));
    QVERIFY(matches.at(0).properties.value(actionsKey).toStringList().isEmpty());

    timer.start();
    matches = service.Match(QStringLiteral("pomodoro"));
    QCOMPARE(matches.at(0).id, QStringLiteral("pause"));
    QCOMPARE(matches.at(0).properties.value(actionsKey).toStringList(),
             (QStringList{QStringLiteral("pause"), QStringLiteral("skip"), QStringLiteral("stop")}));
    for (const auto &m : matches) {
        QVERIFY2(m.properties.contains(actionsKey), qPrintable(m.id));
    }

    timer.pause();
    matches = service.Match(QStringLiteral("pomodoro"));
    QCOMPARE(matches.at(0).id, QStringLiteral("resume"));
    QCOMPARE(matches.at(0).properties.value(actionsKey).toStringList(),
             (QStringList{QStringLiteral("resume"), QStringLiteral("skip"), QStringLiteral("stop")}));
}

void TestKRunnerService::testCustomMinutesDoNotChangeThePreset()
{
    TimerEngine timer;
    timer.setWorkSeconds(1500);
    PresetModel presets;
    KRunnerService service(&timer, &presets);

    service.Run(QStringLiteral("custom:45"), QString());
    QVERIFY(timer.isRunning());
    QVERIFY(timer.remainingSeconds() > 44 * 60);
    QCOMPARE(timer.workSeconds(), 1500);

    // Out of range, or too long to read at all (toInt() overflows): an explanation, which
    // starts nothing when chosen.
    for (const QString &query : {QStringLiteral("pomodoro 0"), QStringLiteral("pomodoro 999"),
                                 QStringLiteral("pomodoro 99999999999999999999")}) {
        const auto matches = service.Match(query);
        QCOMPARE(matches.size(), 1);
        QCOMPARE(matches.first().id, QStringLiteral("invalid-minutes"));
        QVERIFY(matches.first().text.contains(QStringLiteral("180")));
        timer.stop();
        service.Run(matches.first().id, QString());
        QVERIFY(!timer.isActive());
    }
    // Digits of other scripts are not minutes ("٢٥" is 25 in Arabic-Indic digits).
    const auto arabic = service.Match(QStringLiteral("pomodoro \u0662\u0665"));
    QVERIFY(std::none_of(arabic.cbegin(), arabic.cend(), [](const auto &m) {
        return m.id.startsWith(QStringLiteral("custom:")) || m.id == QStringLiteral("invalid-minutes");
    }));
}

void TestKRunnerService::testPresetByName()
{
    TimerEngine timer;
    PresetModel presets;
    presets.create({{QStringLiteral("name"), QStringLiteral("Deep Work")}});
    presets.create({{QStringLiteral("name"), QStringLiteral("Study")}});
    KRunnerService service(&timer, &presets);
    QVERIFY(presets.count() > 1);

    // Pick a preset other than the current one and search for part of its name.
    const QModelIndex target = presets.index(presets.count() - 1, 0);
    const QString uuid = target.data(PresetModel::UuidRole).toString();
    const QString name = target.data(PresetModel::NameRole).toString();
    QVERIFY(uuid != presets.currentUuid());

    const auto matches = service.Match(QStringLiteral("pomodoro ") + name.left(4).toUpper());
    bool found = false;
    for (const auto &m : matches) {
        QVERIFY(m.id.startsWith(QStringLiteral("preset:")));
        found = found || m.id == QStringLiteral("preset:") + uuid;
    }
    QVERIFY(found);

    QVERIFY(service.Match(QStringLiteral("pomodoro no-such-preset-name")).isEmpty());

    service.Run(QStringLiteral("preset:") + uuid, QString());
    QCOMPARE(presets.currentUuid(), uuid);
    QVERIFY(timer.isRunning());
}

void TestKRunnerService::testStartingTheCurrentPresetResumesIt()
{
    TimerEngine timer;
    timer.setWorkSeconds(1500);
    PresetModel presets;
    KRunnerService service(&timer, &presets);
    const QString uuid = presets.currentUuid();
    QVERIFY(!uuid.isEmpty());

    timer.start();
    timer.pause();
    QVERIFY(timer.isPaused());

    // KRunner offers "Start preset" for the current preset while it is paused too.
    service.Run(QStringLiteral("preset:") + uuid, QString());
    QCOMPARE(presets.currentUuid(), uuid);
    QVERIFY(timer.isRunning());
    QVERIFY(!timer.isPaused());
}

void TestKRunnerService::testStartResumesAPausedTimer()
{
    FakeClock clock;
    TimerEngine timer(&clock, nullptr);
    timer.setWorkSeconds(1500);
    PresetModel presets;
    KRunnerService service(&timer, &presets);

    timer.start();
    clock.advanceSeconds(60);
    timer.pause();
    QVERIFY(timer.isPaused());

    // "pomodoro start" is offered while the timer is paused too, and means what start means
    // over D-Bus and in a sugo:// link (TimerCommands::start()): start, or resume.
    const auto matches = service.Match(QStringLiteral("pomodoro start"));
    QCOMPARE(matches.size(), 1);
    service.Run(matches.first().id, QString());
    QVERIFY(timer.isRunning());
    QVERIFY(!timer.isPaused());
    QCOMPARE(timer.phase(), TimerEngine::Phase::Work);
    QCOMPARE(timer.remainingSeconds(), 1440); // resumed, not started again

    // The same for the "Start" button on another result.
    timer.pause();
    QVERIFY(timer.isPaused());
    service.Run(QStringLiteral("stop"), QStringLiteral("start"));
    QVERIFY(timer.isRunning());
    QVERIFY(!timer.isPaused());
    QCOMPARE(timer.remainingSeconds(), 1440);

    // A running timer is left alone.
    service.Run(QStringLiteral("start"), QString());
    QVERIFY(timer.isRunning());
    QVERIFY(!timer.isPaused());
    QCOMPARE(timer.remainingSeconds(), 1440);
}

void TestKRunnerService::testNothingIsOfferedBeforeTheTimerIsAttached()
{
    // main.cpp registers the runner before the timer exists.
    KRunnerService service(nullptr, nullptr);
    QVERIFY(service.Match(QStringLiteral("pomodoro")).isEmpty());
    QVERIFY(service.Match(QStringLiteral("pomodoro 25")).isEmpty());
    service.Run(QStringLiteral("start"), QString()); // nothing to run, and no crash
    service.Run(QStringLiteral("custom:25"), QString());
    service.Run(QStringLiteral("x"), QStringLiteral("start"));

    TimerEngine timer;
    PresetModel presets;
    service.attach(&timer, &presets);
    QVERIFY(!service.Match(QStringLiteral("pomodoro")).isEmpty());
    service.Run(QStringLiteral("start"), QString());
    QVERIFY(timer.isRunning());
}

void TestKRunnerService::testControlCanBeTurnedOff()
{
    TimerEngine timer;
    PresetModel presets;
    KRunnerService service(&timer, &presets);
    bool allowed = false;
    service.setControlAllowed([&allowed]() { return allowed; });
    QSignalSpy open(&service, &KRunnerService::openRequested);

    // Only an explanation is offered, and choosing it opens Sugo (where the setting is).
    const auto matches = service.Match(QStringLiteral("pomodoro 25"));
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches.first().id, QStringLiteral("control-disabled"));
    service.Run(matches.first().id, QString());
    QCOMPARE(open.count(), 1);

    // Commands (also a match left over from before the change) do nothing.
    for (const QString &id : {QStringLiteral("start"), QStringLiteral("custom:25"), QStringLiteral("toggle")}) {
        service.Run(id, QString());
        QVERIFY(!timer.isActive());
    }
    service.Run(QStringLiteral("x"), QStringLiteral("start"));
    QVERIFY(!timer.isActive());

    allowed = true;
    service.Run(QStringLiteral("start"), QString());
    QVERIFY(timer.isActive());
}

void TestKRunnerService::testAnotherPresetDoesNotThrowAwayTheActivePhase()
{
    Fixture f;
    QVERIFY(!f.first.isEmpty());
    QVERIFY(f.deep != f.first);
    QCOMPARE(f.presets.currentUuid(), f.first);
    QSignalSpy ended(&f.timer, &TimerEngine::sessionEnded);

    f.timer.start();
    f.clock.advanceSeconds(60);
    const int remaining = f.timer.remainingSeconds();
    QVERIFY(remaining > 0);

    // A match from "pomodoro deep" (offered, or left over from before the start): the running
    // work phase of the other timer stays, as with D-Bus selectPreset/startPreset.
    f.service.Run(QStringLiteral("preset:") + f.deep, QString());
    QCOMPARE(f.presets.currentUuid(), f.first);
    QVERIFY(f.timer.isRunning());
    QCOMPARE(f.timer.phase(), TimerEngine::Phase::Work);
    QCOMPARE(f.timer.remainingSeconds(), remaining);
    QCOMPARE(ended.count(), 0);

    // Paused is active too: it stays paused, with its time.
    f.timer.pause();
    f.service.Run(QStringLiteral("preset:") + f.deep, QString());
    QCOMPARE(f.presets.currentUuid(), f.first);
    QVERIFY(f.timer.isPaused());
    QCOMPARE(f.timer.remainingSeconds(), remaining);
    QCOMPARE(ended.count(), 0);

    // Once stopped, the other preset is chosen and started.
    f.timer.stop();
    f.service.Run(QStringLiteral("preset:") + f.deep, QString());
    QCOMPARE(f.presets.currentUuid(), f.deep);
    QVERIFY(f.timer.isRunning());
}

void TestKRunnerService::testStartingTheCurrentPausedPresetKeepsItsTime()
{
    Fixture f;
    QSignalSpy ended(&f.timer, &TimerEngine::sessionEnded);
    f.timer.start();
    f.clock.advanceSeconds(90);
    f.timer.pause();
    const int remaining = f.timer.remainingSeconds();

    f.service.Run(QStringLiteral("preset:") + f.first, QString());
    QCOMPARE(f.presets.currentUuid(), f.first);
    QVERIFY(f.timer.isRunning());
    QCOMPARE(f.timer.remainingSeconds(), remaining);
    QCOMPARE(ended.count(), 0);

    // Running already: choosing it again changes nothing.
    f.clock.advanceSeconds(10);
    f.service.Run(QStringLiteral("preset:") + f.first, QString());
    QVERIFY(f.timer.isRunning());
    QCOMPARE(f.timer.remainingSeconds(), remaining - 10);
    QCOMPARE(ended.count(), 0);
}

void TestKRunnerService::testUnknownPresetIsIgnored()
{
    Fixture f;
    f.service.Run(QStringLiteral("preset:no-such-uuid"), QString());
    f.service.Run(QStringLiteral("preset:"), QString());
    QCOMPARE(f.presets.currentUuid(), f.first);
    QVERIFY(f.timer.isIdle());
}

void TestKRunnerService::testCustomMinutesOutOfRangeAreIgnored()
{
    Fixture f;
    for (const QString &id : {QStringLiteral("custom:0"), QStringLiteral("custom:-5"), QStringLiteral("custom:181"),
                              QStringLiteral("custom:abc"), QStringLiteral("custom:")}) {
        f.service.Run(id, QString());
        QVERIFY2(f.timer.isIdle(), qPrintable(id));
    }
    f.service.Run(QStringLiteral("custom:180"), QString());
    QVERIFY(f.timer.isRunning());
    QCOMPARE(f.timer.remainingSeconds(), 180 * 60);
}

void TestKRunnerService::testRunDoesOnlyOpenWhileControlIsOff()
{
    Fixture f;
    f.service.setControlAllowed([]() { return false; });
    QSignalSpy open(&f.service, &KRunnerService::openRequested);

    const QStringList ids{QStringLiteral("preset:") + f.deep, QStringLiteral("preset:") + f.first,
                          QStringLiteral("custom:25"), QStringLiteral("skip"), QStringLiteral("resume")};
    for (const QString &id : ids) {
        f.service.Run(id, QString());
        QVERIFY2(f.timer.isIdle(), qPrintable(id));
        QCOMPARE(f.presets.currentUuid(), f.first);
    }
    QCOMPARE(open.count(), 0);

    f.service.Run(QStringLiteral("open"), QString());
    QCOMPARE(open.count(), 1);
    QVERIFY(f.timer.isIdle());
}

void TestKRunnerService::testOtherPresetsAreNotOfferedToStartWhileActive()
{
    Fixture f;
    const auto matchFor = [&f](const QString &uuid) {
        const auto matches = f.service.Match(QStringLiteral("pomodoro deep"));
        const auto it = std::find_if(matches.cbegin(), matches.cend(),
                                     [&uuid](const RemoteMatch &m) { return m.id == QStringLiteral("preset:") + uuid; });
        return it == matches.cend() ? RemoteMatch() : *it;
    };
    const QString subtextKey = QStringLiteral("subtext");

    // Idle: another preset is offered to start.
    RemoteMatch idle = matchFor(f.deep);
    QVERIFY(idle.text.startsWith(QStringLiteral("Start preset")));
    QVERIFY(idle.properties.value(subtextKey).toString().contains(QStringLiteral("minute")));

    // Running or paused: still listed, but saying the timer must be stopped first.
    f.timer.start();
    for (int i = 0; i < 2; ++i) {
        RemoteMatch busy = matchFor(f.deep);
        QCOMPARE(busy.id, QStringLiteral("preset:") + f.deep);
        QVERIFY2(!busy.text.startsWith(QStringLiteral("Start")), qPrintable(busy.text));
        QVERIFY(busy.properties.value(subtextKey).toString().contains(QStringLiteral("Stop it")));
        f.timer.pause();
    }

    // The current preset is still offered: choosing it resumes.
    f.presets.update(f.first, {{QStringLiteral("name"), QStringLiteral("Deep focus")}});
    QVERIFY(matchFor(f.first).text.startsWith(QStringLiteral("Start preset")));
}

void TestKRunnerService::testCustomMinutesDoNotThrowAwayTheActivePhase()
{
    Fixture f;
    QSignalSpy ended(&f.timer, &TimerEngine::sessionEnded);
    const QString subtextKey = QStringLiteral("subtext");

    // Idle: "pomodoro 15" is offered to start.
    auto matches = f.service.Match(QStringLiteral("pomodoro 15"));
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches.first().id, QStringLiteral("custom:15"));
    QVERIFY(matches.first().text.startsWith(QStringLiteral("Start")));

    f.timer.start();
    f.clock.advanceSeconds(60);
    const int remaining = f.timer.remainingSeconds();
    QVERIFY(remaining > 15 * 60);

    // Running or paused: still listed, but saying the timer must be stopped first, and
    // choosing it (offered, or left over from before the start) keeps the phase.
    for (int i = 0; i < 2; ++i) {
        matches = f.service.Match(QStringLiteral("pomodoro 15"));
        QCOMPARE(matches.size(), 1);
        QCOMPARE(matches.first().id, QStringLiteral("custom:15"));
        QVERIFY2(!matches.first().text.startsWith(QStringLiteral("Start")), qPrintable(matches.first().text));
        QVERIFY(matches.first().properties.value(subtextKey).toString().contains(QStringLiteral("Stop it")));

        const bool paused = f.timer.isPaused();
        f.service.Run(QStringLiteral("custom:15"), QString());
        QCOMPARE(ended.count(), 0);
        QCOMPARE(f.timer.phase(), TimerEngine::Phase::Work);
        QCOMPARE(f.timer.isPaused(), paused);
        QCOMPARE(f.timer.remainingSeconds(), remaining);
        f.timer.pause();
    }

    // Once stopped, it starts.
    f.timer.stop();
    ended.clear();
    f.service.Run(QStringLiteral("custom:15"), QString());
    QVERIFY(f.timer.isRunning());
    QCOMPARE(f.timer.remainingSeconds(), 15 * 60);
    QCOMPARE(ended.count(), 0);
}

QTEST_MAIN(TestKRunnerService)
#include "tst_krunnerservice.moc"
