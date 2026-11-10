// SPDX-License-Identifier: GPL-3.0-or-later
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <limits>
#include <memory>

#include "Database.h"
#include "FakeClock.h"
#include "PresetModel.h"
#include "PresetPersistence.h"
#include "PresetRepository.h"
#include "SessionKeeper.h"
#include "SessionRecorder.h"
#include "SessionRepository.h"
#include "TimerBinding.h"
#include "TimerEngine.h"

using State = TimerEngine::State;
using Phase = TimerEngine::Phase;

namespace
{
constexpr qint64 kForever = std::numeric_limits<qint64>::max();
}

/// A run of Sugo ends with a session under way; the next run brings it back. Each "run" is a
/// new engine, recorder and keeper over the same database and timers.
class SessionKeeperTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir dir;
    std::unique_ptr<Database> db;
    std::unique_ptr<SessionRepository> sessions;
    std::unique_ptr<PresetRepository> presetRepo;
    std::unique_ptr<PresetModel> presets;
    std::unique_ptr<PresetPersistence> persistence;

    FakeClock clock;
    std::unique_ptr<TimerEngine> engine;
    std::unique_ptr<TimerBinding> binding;
    std::unique_ptr<SessionRecorder> recorder;
    std::unique_ptr<SessionKeeper> keeper;

    QString path() const { return dir.filePath(QStringLiteral("timer-session.json")); }

    void elapse(qint64 seconds)
    {
        clock.advanceSeconds(seconds);
        engine->refresh();
    }

    /// A new run: the timer stack of the last one is gone, the files and the timers stay.
    void startRun()
    {
        keeper.reset();
        recorder.reset();
        binding.reset();
        engine.reset();
        engine = std::make_unique<TimerEngine>(&clock, nullptr);
        binding = std::make_unique<TimerBinding>(engine.get(), presets.get());
        recorder = std::make_unique<SessionRecorder>(engine.get(), presets.get(), sessions.get());
        keeper = std::make_unique<SessionKeeper>();
        keeper->setNow([this]() { return clock.wallTime(); });
        keeper->attach(engine.get(), presets.get(), recorder.get(), path());
    }

    /// A new run that has nothing to bring back.
    void startFreshRun()
    {
        startRun();
        QVERIFY(!keeper->restore());
    }

    QList<SessionRecord> all() const { return sessions->between(0, kForever); }

private Q_SLOTS:
    void init()
    {
        QVERIFY(dir.isValid());
        QFile::remove(path());
        db = std::make_unique<Database>(QStringLiteral(":memory:"));
        QVERIFY(db->open());
        sessions = std::make_unique<SessionRepository>(db->connectionName());
        presetRepo = std::make_unique<PresetRepository>(db->connectionName());
        presets = std::make_unique<PresetModel>();
        persistence = std::make_unique<PresetPersistence>(presets.get(), presetRepo.get());
        clock = FakeClock();
        startFreshRun();
    }

    void cleanup()
    {
        keeper.reset();
        recorder.reset();
        binding.reset();
        engine.reset();
        persistence.reset();
        presets.reset();
        presetRepo.reset();
        sessions.reset();
        db.reset();
    }

    void nothingIsKeptWhileIdle()
    {
        keeper->save();
        QVERIFY(!QFile::exists(path()));
        engine->start();
        QVERIFY(QFile::exists(path()));
        engine->stop();
        QVERIFY(!QFile::exists(path()));
        QVERIFY(!keeper->keepForQuit());
    }

    void theSessionComesBackPaused()
    {
        engine->start();
        elapse(10 * 60);
        QVERIFY(keeper->keepForQuit());
        QVERIFY(all().isEmpty()); // not recorded as interrupted

        clock.advanceSeconds(3600); // Sugo was not running for an hour
        startRun();
        QSignalSpy ended(keeper.get(), &SessionKeeper::restoreEnded);
        QVERIFY(keeper->restore());
        QVERIFY(QFile::exists(path())); // kept again, for the run that follows
        QCOMPARE(engine->state(), State::Paused);
        QCOMPARE(engine->phase(), Phase::Work);
        QCOMPARE(engine->remainingSeconds(), 15 * 60);
        QCOMPARE(engine->totalSeconds(), 25 * 60);
        QVERIFY(keeper->isRestored());

        engine->resume();
        QVERIFY(!keeper->isRestored());
        QCOMPARE(ended.size(), 1);
        QCOMPARE(ended.at(0).at(1).toString(), QStringLiteral("resumed"));

        elapse(15 * 60);
        const QList<SessionRecord> r = all();
        QCOMPARE(r.size(), 1);
        QVERIFY(r.at(0).completed);
        QCOMPARE(r.at(0).startedAtMs, clock.base().toMSecsSinceEpoch());
        QCOMPARE(r.at(0).durationSec, 25 * 60); // the hour away is a pause
        QCOMPARE(r.at(0).endedAtMs, clock.base().addSecs(25 * 60 + 3600).toMSecsSinceEpoch());
    }

    void aCrashLosesOnlyTheTimeSinceTheLastSave()
    {
        engine->setCyclesBeforeLong(4);
        engine->start();
        elapse(25 * 60);
        engine->start(); // the short break
        elapse(2 * 60);
        keeper->save(); // the heartbeat
        elapse(20);     // then the crash: no keepForQuit()

        startRun();
        QVERIFY(keeper->restore());
        QCOMPARE(engine->phase(), Phase::ShortBreak);
        QCOMPARE(engine->remainingSeconds(), 3 * 60);
        QCOMPARE(engine->completedCycles(), 1);
    }

    void theTimerItRanWithIsChosenAgain()
    {
        const QString deep = presets->create({{QStringLiteral("name"), QStringLiteral("Deep work")},
                                              {QStringLiteral("workSeconds"), 50 * 60}});
        presets->setCurrentUuid(deep);
        engine->start();
        elapse(60);
        QVERIFY(keeper->keepForQuit());

        startRun();
        presets->setCurrentUuid(PresetModel::DefaultUuid);
        QVERIFY(keeper->restore());
        QCOMPARE(presets->currentUuid(), deep);
        QCOMPARE(engine->totalSeconds(), 50 * 60);
        engine->stop();
        QCOMPARE(all().at(0).presetName, QStringLiteral("Deep work"));
    }

    void aCrashRightAfterSwitchingTimersBringsBackTheNewOne()
    {
        const QString deep = presets->create({{QStringLiteral("name"), QStringLiteral("Deep work")},
                                              {QStringLiteral("workSeconds"), 50 * 60}});
        presets->setCurrentUuid(deep);
        engine->start();
        elapse(20); // then the crash, before any heartbeat: no keepForQuit()

        startRun();
        presets->setCurrentUuid(PresetModel::DefaultUuid);
        QVERIFY(keeper->restore());
        QCOMPARE(presets->currentUuid(), deep);
        QCOMPARE(engine->totalSeconds(), 50 * 60);
    }

    void stoppingItIsReported()
    {
        engine->start();
        elapse(60);
        QVERIFY(keeper->keepForQuit());
        startRun();
        QSignalSpy ended(keeper.get(), &SessionKeeper::restoreEnded);
        QVERIFY(keeper->restore());
        engine->stop();
        QCOMPARE(ended.size(), 1);
        QCOMPARE(ended.at(0).at(1).toString(), QStringLiteral("stopped"));
        QVERIFY(!all().at(0).completed);
        QCOMPARE(all().at(0).durationSec, 60);
        QVERIFY(!QFile::exists(path()));
    }

    void dismissingHidesOnlyTheMessage()
    {
        engine->start();
        elapse(60);
        QVERIFY(keeper->keepForQuit());
        startRun();
        QVERIFY(keeper->restore());
        keeper->dismiss();
        QVERIFY(!keeper->isRestored());
        QVERIFY(engine->isPaused());
    }

    void tooOldASessionIsRecordedAsInterrupted()
    {
        engine->start();
        elapse(10 * 60);
        QVERIFY(keeper->keepForQuit());
        clock.advanceMs(SessionKeeper::kMaxAgeMs + 1000);
        startRun();
        QSignalSpy ended(keeper.get(), &SessionKeeper::restoreEnded);
        QVERIFY(!keeper->restore());
        QVERIFY(engine->isIdle());
        QVERIFY(!keeper->isRestored());
        QCOMPARE(ended.size(), 1);
        QCOMPARE(ended.at(0).at(1).toString(), QStringLiteral("expired"));
        const QList<SessionRecord> r = all();
        QCOMPARE(r.size(), 1);
        QVERIFY(!r.at(0).completed);
        QCOMPARE(r.at(0).durationSec, 10 * 60);
        QCOMPARE(r.at(0).endedAtMs, clock.base().addSecs(10 * 60).toMSecsSinceEpoch());
        QVERIFY(!QFile::exists(path()));
    }

    void aSessionWhoseTimerIsGoneIsRecordedAsInterrupted()
    {
        const QString uuid = presets->create({{QStringLiteral("name"), QStringLiteral("Gone")}});
        presets->setCurrentUuid(uuid);
        engine->start();
        elapse(60);
        QVERIFY(keeper->keepForQuit());
        startRun();
        presets->setCurrentUuid(PresetModel::DefaultUuid);
        QVERIFY(presets->remove(uuid));
        QVERIFY(!keeper->restore());
        QCOMPARE(all().size(), 1);
        QCOMPARE(all().at(0).presetName, QStringLiteral("Gone"));
    }

    void aWorkTakenUpAgainIsNotKept()
    {
        engine->start();
        elapse(25 * 60);
        QVERIFY(engine->continueFinished());
        QVERIFY(!QFile::exists(path()));
        QVERIFY(!keeper->keepForQuit());
    }

    void theSessionKeepsTheTimerNameItBeganWith()
    {
        const QString uuid = presets->create({{QStringLiteral("name"), QStringLiteral("Focus")},
                                              {QStringLiteral("category"), QStringLiteral("Writing")}});
        presets->setCurrentUuid(uuid);
        engine->start();
        elapse(60);
        QVERIFY(presets->update(uuid, {{QStringLiteral("name"), QStringLiteral("Deep focus")},
                                       {QStringLiteral("category"), QStringLiteral("Reading")}}));
        QVERIFY(keeper->keepForQuit());

        startRun();
        QVERIFY(keeper->restore());
        QVERIFY(keeper->holdsRestoredSession());
        engine->stop();
        QVERIFY(!keeper->holdsRestoredSession());
        QCOMPARE(all().size(), 1);
        QCOMPARE(all().at(0).presetName, QStringLiteral("Focus"));
        QCOMPARE(all().at(0).category, QStringLiteral("Writing"));
    }

    void aSessionThatCannotBeWrittenIsNotKept()
    {
        QFile blocker(dir.filePath(QStringLiteral("blocker")));
        QVERIFY(blocker.open(QIODevice::WriteOnly)); // a file where the folder should be
        blocker.close();
        keeper.reset();
        keeper = std::make_unique<SessionKeeper>();
        keeper->attach(engine.get(), presets.get(), recorder.get(), dir.filePath(QStringLiteral("blocker/timer-session.json")));
        QVERIFY(!keeper->restore());
        engine->start();
        elapse(60);
        QVERIFY(!keeper->keepForQuit()); // so the quit stops the timer and records it
        QFile::remove(blocker.fileName());
    }

    void anUnreadableFileIsDropped()
    {
        QFile file(path());
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"format\":1,\"phase\":\"nap\"}");
        file.close();
        startRun();
        QVERIFY(!keeper->restore());
        QVERIFY(engine->isIdle());
        QVERIFY(!QFile::exists(path()));
        QVERIFY(all().isEmpty());
    }
};

QTEST_GUILESS_MAIN(SessionKeeperTest)
#include "tst_sessionkeeper.moc"
