// SPDX-License-Identifier: GPL-3.0-or-later
#include <QTest>
#include <QUrl>

#include "FakeClock.h"
#include "PresetModel.h"
#include "TimerBinding.h"
#include "TimerCommands.h"
#include "TimerEngine.h"

using Action = TimerCommands::UrlCommand::Action;
using Error = TimerCommands::Error;

Q_DECLARE_METATYPE(TimerCommands::UrlCommand::Action)

/// The commands shared by D-Bus and sugo:// links, and the reading of those links.
class TimerCommandsTest : public QObject
{
    Q_OBJECT

private:
    struct Fixture {
        FakeClock clock;
        TimerEngine engine{&clock, nullptr};
        PresetModel presets;
        TimerBinding binding{&engine, &presets};
        TimerCommands commands{&engine, &presets};
        QString deep;
        Fixture() { deep = presets.create({{QStringLiteral("name"), QStringLiteral("Deep Work")}}); }
    };

private Q_SLOTS:
    void parsesLinks_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<Action>("action");
        QTest::addColumn<QString>("argument");

        QTest::newRow("toggle") << "sugo://toggle" << Action::Toggle << "";
        QTest::newRow("start") << "sugo://start" << Action::Start << "";
        QTest::newRow("pause") << "sugo://pause" << Action::Pause << "";
        QTest::newRow("resume") << "sugo://resume" << Action::Resume << "";
        QTest::newRow("stop") << "sugo://stop" << Action::Stop << "";
        QTest::newRow("skip") << "sugo://skip" << Action::Skip << "";
        QTest::newRow("show") << "sugo://show" << Action::Show << "";
        QTest::newRow("any case") << "SUGO://Toggle" << Action::Toggle << "";
        QTest::newRow("trailing slash") << "sugo://pause/" << Action::Pause << "";
        QTest::newRow("no slashes") << "sugo:toggle" << Action::Toggle << "";
        QTest::newRow("empty host") << "sugo:///stop" << Action::Stop << "";
        QTest::newRow("preset by name") << "sugo://preset/Deep%20Work" << Action::SelectPreset << "Deep Work";
        QTest::newRow("preset with a slash") << "sugo://preset/A%2FB" << Action::SelectPreset << "A/B";
        QTest::newRow("preset without slashes") << "sugo:preset/Reading" << Action::SelectPreset << "Reading";
        QTest::newRow("start a preset") << "sugo://start/Deep%20Work" << Action::Start << "Deep Work";
        QTest::newRow("preset without name") << "sugo://preset" << Action::Invalid << "";
        QTest::newRow("argument where none is taken") << "sugo://stop/now" << Action::Invalid << "";
        QTest::newRow("unknown") << "sugo://explode" << Action::Invalid << "";
        QTest::newRow("empty") << "sugo://" << Action::Invalid << "";
        QTest::newRow("other scheme") << "https://toggle" << Action::Invalid << "";
    }

    void parsesLinks()
    {
        QFETCH(QString, url);
        QFETCH(Action, action);
        QFETCH(QString, argument);
        const TimerCommands::UrlCommand command = TimerCommands::parseUrl(QUrl(url));
        QCOMPARE(command.action, action);
        QCOMPARE(command.argument, argument);
    }

    void linksDriveTheTimer()
    {
        Fixture f;
        QVERIFY(f.commands.run(TimerCommands::parseUrl(QUrl(QStringLiteral("sugo://start")))).ok());
        QVERIFY(f.engine.isRunning());
        QVERIFY(f.commands.run(TimerCommands::parseUrl(QUrl(QStringLiteral("sugo://pause")))).ok());
        QVERIFY(f.engine.isPaused());
        QVERIFY(f.commands.run(TimerCommands::parseUrl(QUrl(QStringLiteral("sugo://resume")))).ok());
        QVERIFY(f.engine.isRunning());
        QVERIFY(f.commands.run(TimerCommands::parseUrl(QUrl(QStringLiteral("sugo://toggle")))).ok());
        QVERIFY(f.engine.isPaused());
        QVERIFY(f.commands.run(TimerCommands::parseUrl(QUrl(QStringLiteral("sugo://skip")))).ok());
        QVERIFY(f.commands.run(TimerCommands::parseUrl(QUrl(QStringLiteral("sugo://stop")))).ok());
        QVERIFY(f.engine.isIdle());

        const TimerCommands::Result unknown = f.commands.run(TimerCommands::parseUrl(QUrl(QStringLiteral("sugo://explode"))));
        QCOMPARE(unknown.error, Error::UnknownCommand);
        QVERIFY(!unknown.message.isEmpty());
    }

    void startResumesAPausedTimer()
    {
        Fixture f;

        // From Idle it begins the pending phase.
        QVERIFY(f.commands.start().ok());
        QCOMPARE(f.engine.state(), TimerEngine::State::Working);
        QVERIFY(f.engine.isRunning());

        // Paused: start resumes it, and the countdown goes on from where it stopped.
        f.clock.advanceSeconds(10);
        QVERIFY(f.commands.pause().ok());
        QVERIFY(f.engine.isPaused());
        const int paused = f.engine.remainingSeconds();
        QVERIFY(f.commands.start().ok());
        QCOMPARE(f.engine.state(), TimerEngine::State::Working);
        QVERIFY(f.engine.isRunning());
        QCOMPARE(f.engine.remainingSeconds(), paused);
        f.clock.advanceSeconds(5);
        QCOMPARE(f.engine.remainingSeconds(), paused - 5);

        // Running: start leaves the phase alone (it does not restart it).
        const int running = f.engine.remainingSeconds();
        QVERIFY(f.commands.start().ok());
        QCOMPARE(f.engine.state(), TimerEngine::State::Working);
        QCOMPARE(f.engine.remainingSeconds(), running);
    }

    void linksChooseAndStartTimers()
    {
        Fixture f;
        QVERIFY(f.commands.run(TimerCommands::parseUrl(QUrl(QStringLiteral("sugo://preset/deep%20work")))).ok());
        QCOMPARE(f.presets.currentUuid(), f.deep);

        const TimerCommands::Result missing = f.commands.run(TimerCommands::parseUrl(QUrl(QStringLiteral("sugo://preset/Nope"))));
        QCOMPARE(missing.error, Error::UnknownPreset);
        QCOMPARE(f.presets.currentUuid(), f.deep);

        // Running: another timer would throw the phase away.
        const QString other = f.presets.uuidAt(0);
        QVERIFY(f.commands.run(TimerCommands::parseUrl(QUrl(QStringLiteral("sugo://start/") + f.deep))).ok());
        QVERIFY(f.engine.isRunning());
        const TimerCommands::Result busy = f.commands.run(TimerCommands::parseUrl(QUrl(QStringLiteral("sugo://preset/") + other)));
        QCOMPARE(busy.error, Error::TimerActive);
        QCOMPARE(f.presets.currentUuid(), f.deep);
        QVERIFY(f.engine.isRunning());
    }

    void linksFollowTheSettingButShowAlwaysWorks()
    {
        Fixture f;
        bool allowed = false;
        f.commands.setControlAllowed([&allowed]() { return allowed; });
        for (const char *url : {"sugo://start", "sugo://toggle", "sugo://preset/Deep%20Work", "sugo://start/Deep%20Work"}) {
            const TimerCommands::Result refused = f.commands.run(TimerCommands::parseUrl(QUrl(QString::fromLatin1(url))));
            QCOMPARE(refused.error, Error::ControlDisabled);
            QVERIFY(!refused.message.isEmpty());
        }
        QVERIFY(f.engine.isIdle());
        QVERIFY(f.presets.currentUuid() != f.deep);
        QVERIFY(f.commands.run(TimerCommands::parseUrl(QUrl(QStringLiteral("sugo://show")))).ok());

        allowed = true;
        QVERIFY(f.commands.run(TimerCommands::parseUrl(QUrl(QStringLiteral("sugo://toggle")))).ok());
        QVERIFY(f.engine.isRunning());
    }

    void withoutAnEngineNothingHappens()
    {
        TimerCommands commands(nullptr, nullptr);
        QCOMPARE(commands.toggle().error, Error::Unavailable);
        QCOMPARE(commands.status(), QStringLiteral("unavailable"));
        QVERIFY(commands.presets().isEmpty());
    }

    void anEngineCanBeAttachedLater()
    {
        // The D-Bus objects are set up before the timer exists (main.cpp).
        Fixture f;
        TimerCommands commands(nullptr, nullptr);
        QCOMPARE(commands.startPreset(f.deep).error, Error::Unavailable);
        commands.attach(&f.engine, &f.presets);
        QVERIFY(commands.startPreset(f.deep).ok());
        QCOMPARE(f.presets.currentUuid(), f.deep);
        QVERIFY(f.engine.isRunning());
    }

    void statusAndPresets()
    {
        Fixture f;
        QVERIFY(f.commands.status().startsWith(QStringLiteral("idle:work:")));
        QVERIFY(f.commands.presets().contains(QStringLiteral("Deep Work")));
        QCOMPARE(f.commands.resolvePreset(QStringLiteral("DEEP WORK")), f.deep);
        QCOMPARE(f.commands.resolvePreset(QStringLiteral("  deep work ")), f.deep);
        QVERIFY(f.commands.resolvePreset(QStringLiteral("   ")).isEmpty());
        QCOMPARE(f.commands.resolvePreset(f.deep), f.deep);
        QVERIFY(f.commands.stepPreset(1).ok());
    }
};

QTEST_GUILESS_MAIN(TimerCommandsTest)
#include "tst_timercommands.moc"
