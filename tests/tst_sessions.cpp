// SPDX-License-Identifier: GPL-3.0-or-later
#include <QCoreApplication>
#include <QDateTime>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>

#include "Localization.h"

#include <limits>
#include <memory>

#include "DamagedDatabase.h"
#include "Database.h"
#include "FakeClock.h"
#include "PresetModel.h"
#include "PresetPersistence.h"
#include "PresetRepository.h"
#include "SessionRecorder.h"
#include "SessionRepository.h"
#include "TimerBinding.h"
#include "TimerEngine.h"

namespace
{
constexpr qint64 kForever = std::numeric_limits<qint64>::max();

SessionRecord makeRecord(qint64 startedAtMs, SessionKind kind = SessionKind::Work)
{
    SessionRecord r;
    r.kind = kind;
    r.startedAtMs = startedAtMs;
    r.endedAtMs = startedAtMs + 1'500'000;
    r.durationSec = 1500;
    r.plannedSec = 1500;
    r.completed = true;
    r.presetName = QStringLiteral("Default");
    return r;
}
} // namespace

class SessionsTest : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<Database> db;
    std::unique_ptr<SessionRepository> sessions;

    // Engine stack used by the recorder tests.
    FakeClock clock;
    std::unique_ptr<TimerEngine> engine;
    std::unique_ptr<PresetRepository> presetRepo;
    std::unique_ptr<PresetModel> presets;
    std::unique_ptr<PresetPersistence> persistence; // as in the app: presets exist in the database
    std::unique_ptr<TimerBinding> binding;
    std::unique_ptr<SessionRecorder> recorder;

    void elapse(qint64 seconds)
    {
        clock.advanceSeconds(seconds);
        engine->refresh();
    }

    QList<SessionRecord> all() const
    {
        return sessions->between(0, kForever);
    }

    /// Runs the application's event loop and quits it, so aboutToQuit is emitted as when the program quits.
    static void quitApplication()
    {
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        QCOMPARE(QCoreApplication::exec(), 0);
    }

private Q_SLOTS:
    void initTestCase()
    {
    }

    void init()
    {
        db = std::make_unique<Database>(QStringLiteral(":memory:"));
        QVERIFY(db->open());
        sessions = std::make_unique<SessionRepository>(db->connectionName());

        clock = FakeClock();
        engine = std::make_unique<TimerEngine>(&clock, nullptr);
        presetRepo = std::make_unique<PresetRepository>(db->connectionName());
        presets = std::make_unique<PresetModel>();
        persistence = std::make_unique<PresetPersistence>(presets.get(), presetRepo.get());
        binding = std::make_unique<TimerBinding>(engine.get(), presets.get());
        recorder = std::make_unique<SessionRecorder>(engine.get(), presets.get(), sessions.get());
    }

    void cleanup()
    {
        recorder.reset();
        binding.reset();
        persistence.reset();
        presets.reset();
        presetRepo.reset();
        engine.reset();
        sessions.reset();
        db.reset();
    }

    // --- SessionRepository ---------------------------------------------------

    void insertAndReadBackEveryField()
    {
        SessionRecord r = makeRecord(1'000'000, SessionKind::LongBreak);
        r.durationSec = 800;
        r.plannedSec = 900;
        r.completed = false;
        r.presetName = QStringLiteral("Deep work");
        r.category = QStringLiteral("Work");

        QVERIFY(sessions->insert(r));
        QCOMPARE(sessions->count(), 1);

        const QList<SessionRecord> loaded = all();
        QCOMPARE(loaded.size(), 1);
        const SessionRecord &g = loaded.at(0);
        QVERIFY(g.id > 0);
        QCOMPARE(g.kind, SessionKind::LongBreak);
        QCOMPARE(g.startedAtMs, r.startedAtMs);
        QCOMPARE(g.endedAtMs, r.endedAtMs);
        QCOMPARE(g.durationSec, 800);
        QCOMPARE(g.plannedSec, 900);
        QVERIFY(!g.completed);
        QCOMPARE(g.presetName, QStringLiteral("Deep work"));
        QCOMPARE(g.category, QStringLiteral("Work"));
    }

    void betweenIsHalfOpenAndOrdered()
    {
        QVERIFY(sessions->insert(makeRecord(3000)));
        QVERIFY(sessions->insert(makeRecord(1000)));
        QVERIFY(sessions->insert(makeRecord(2000)));

        QList<SessionRecord> r = sessions->between(1000, 3000);
        QCOMPARE(r.size(), 2);
        QCOMPARE(r.at(0).startedAtMs, 1000); // from is inclusive, results ordered
        QCOMPARE(r.at(1).startedAtMs, 2000); // to is exclusive

        QVERIFY(sessions->between(5000, 6000).isEmpty());
    }

    void deleteStartedBeforeRemovesOnlyOlderSessions()
    {
        QVERIFY(sessions->insert(makeRecord(1'000)));
        QVERIFY(sessions->insert(makeRecord(2'000)));
        QVERIFY(sessions->insert(makeRecord(3'000)));

        QCOMPARE(sessions->deleteStartedBefore(2'000), 1); // strictly before the cut-off
        const auto left = sessions->between(0, kForever);
        QCOMPARE(left.size(), 2);
        QCOMPARE(left.first().startedAtMs, qint64(2'000));

        QCOMPARE(sessions->deleteStartedBefore(500), 0);
        QCOMPARE(sessions->count(), 2);
        QCOMPARE(sessions->deleteStartedBefore(kForever), 2);
        QCOMPARE(sessions->count(), 0);
    }

    void allThreeKindsRoundTrip()
    {
        QVERIFY(sessions->insert(makeRecord(1, SessionKind::Work)));
        QVERIFY(sessions->insert(makeRecord(2, SessionKind::ShortBreak)));
        QVERIFY(sessions->insert(makeRecord(3, SessionKind::LongBreak)));
        const QList<SessionRecord> r = all();
        QCOMPARE(r.at(0).kind, SessionKind::Work);
        QCOMPARE(r.at(1).kind, SessionKind::ShortBreak);
        QCOMPARE(r.at(2).kind, SessionKind::LongBreak);
    }

    void sessionsOutliveTheirPresetAndKeepTheSnapshot()
    {
        PresetRepository presetRepo(db->connectionName());
        TimerPreset custom;
        custom.uuid = QStringLiteral("custom");
        custom.name = QStringLiteral("Temp");
        TimerPreset builtin;
        builtin.uuid = PresetModel::DefaultUuid;
        builtin.name = QStringLiteral("Default");
        builtin.builtin = true;
        QVERIFY(presetRepo.saveAll({builtin, custom}));

        SessionRecord r = makeRecord(1000);
        r.presetUuid = QStringLiteral("custom");
        r.presetName = QStringLiteral("Temp");
        r.category = QStringLiteral("Study");
        QVERIFY(sessions->insert(r));
        QCOMPARE(all().at(0).presetUuid, QStringLiteral("custom")); // linked

        QVERIFY(presetRepo.saveAll({builtin})); // deletes the preset
        const QList<SessionRecord> after = all();
        QCOMPARE(after.size(), 1);                            // session kept
        QVERIFY(after.at(0).presetUuid.isEmpty());            // link cleared (ON DELETE SET NULL)
        QCOMPARE(after.at(0).presetName, QStringLiteral("Temp"));
        QCOMPARE(after.at(0).category, QStringLiteral("Study"));
    }

    void sessionWithUnknownPresetIsStoredUnlinked()
    {
        SessionRecord r = makeRecord(1000);
        r.presetUuid = QStringLiteral("never-saved");
        QVERIFY(sessions->insert(r));
        QVERIFY(all().at(0).presetUuid.isEmpty());
    }

    void eachSessionGetsItsOwnUuid()
    {
        QVERIFY(sessions->insert(makeRecord(1000)));
        QVERIFY(sessions->insert(makeRecord(2000)));
        const QList<SessionRecord> rows = all();
        QCOMPARE(rows.size(), 2);
        for (const SessionRecord &r : rows) {
            QVERIFY2(!QUuid::fromString(r.uuid).isNull(), qPrintable(r.uuid));
        }
        QVERIFY(rows.at(0).uuid != rows.at(1).uuid);
    }

    void aSessionKeepsTheUuidItCameWith()
    {
        SessionRecord r = makeRecord(1000);
        r.uuid = QStringLiteral("0b6f2c4e-8a51-4d7e-9c3a-5f1e2d7b9a60");
        QVERIFY(sessions->insert(r));
        QCOMPARE(all().at(0).uuid, r.uuid);
        QVERIFY(!sessions->insert(r)); // the same session twice: one row
        QCOMPARE(sessions->count(), 1);
    }

    void aChangedNoteMarksTheSessionChanged()
    {
        qint64 id = 0;
        QVERIFY(sessions->insert(makeRecord(1000), &id));
        const auto updatedAt = [this, id]() {
            QSqlQuery query(QSqlDatabase::database(db->connectionName()));
            query.prepare(QStringLiteral("SELECT updated_at FROM session WHERE id = ?"));
            query.addBindValue(id);
            return query.exec() && query.next() ? query.value(0).toLongLong() : -1;
        };
        QCOMPARE(updatedAt(), 0); // as recorded
        const qint64 before = QDateTime::currentMSecsSinceEpoch();
        QVERIFY(sessions->updateNote(id, QStringLiteral("Chapter 4")));
        QVERIFY(updatedAt() >= before);
    }

    void aDeletedSessionLeavesATombstone()
    {
        qint64 kept = 0;
        qint64 gone = 0;
        QVERIFY(sessions->insert(makeRecord(1000), &kept));
        QVERIFY(sessions->insert(makeRecord(2000), &gone));
        const QString uuid = all().at(1).uuid;
        const qint64 before = QDateTime::currentMSecsSinceEpoch();
        SessionRecord deleted;
        QVERIFY(sessions->deleteById(gone, &deleted));
        QCOMPARE(deleted.id, gone);
        QCOMPARE(deleted.uuid, uuid);
        QCOMPARE(deleted.startedAtMs, 2000);
        QCOMPARE(all().size(), 1);
        QCOMPARE(all().at(0).id, kept);

        QSqlQuery query(QSqlDatabase::database(db->connectionName()));
        QVERIFY(query.exec(QStringLiteral("SELECT uuid, deleted_at, deleted_by FROM session_tombstone")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), uuid);
        QVERIFY(query.value(1).toLongLong() >= before);
        QCOMPARE(query.value(2).toString(), QString()); // this device
        QVERIFY(!query.next());

        // No such session: nothing changes, no tombstone.
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("there is no session")));
        QVERIFY(!sessions->deleteById(gone));
        QVERIFY(query.exec(QStringLiteral("SELECT count(*) FROM session_tombstone")) && query.next());
        QCOMPARE(query.value(0).toInt(), 1);
        QCOMPARE(sessions->count(), 1);
    }

    // --- SessionRecorder -----------------------------------------------------

    /// Makes the database refuse new sessions, as a full disk or a lock would.
    void failSessionInserts(bool fail)
    {
        QSqlQuery query(QSqlDatabase::database(db->connectionName()));
        if (fail) {
            QVERIFY(query.exec(QStringLiteral("CREATE TEMP TRIGGER refuse BEFORE INSERT ON session "
                                              "BEGIN SELECT RAISE(ABORT, 'disk full'); END")));
        } else {
            QVERIFY(query.exec(QStringLiteral("DROP TRIGGER refuse")));
        }
    }

    /// Makes the database refuse changes to stored sessions.
    void failSessionUpdates(bool fail)
    {
        QSqlQuery query(QSqlDatabase::database(db->connectionName()));
        if (fail) {
            QVERIFY(query.exec(QStringLiteral("CREATE TEMP TRIGGER refuseUpdate BEFORE UPDATE ON session "
                                              "BEGIN SELECT RAISE(ABORT, 'disk full'); END")));
        } else {
            QVERIFY(query.exec(QStringLiteral("DROP TRIGGER refuseUpdate")));
        }
    }

    void extraTimeTheDatabaseRefusesIsStoredLater()
    {
        QSignalSpy recorded(recorder.get(), &SessionRecorder::workSessionRecorded);
        recorder->setRetryIntervalForTesting(30);
        engine->start();
        elapse(25 * 60); // Manual: the short break waits
        const qint64 id = all().at(0).id;
        QCOMPARE(recorded.size(), 1);

        failSessionUpdates(true);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Cannot update session")));
        QVERIFY(engine->continueFinished());
        elapse(5 * 60);
        QVERIFY(recorder->isSaveFailing()); // said, not lost
        QCOMPARE(all().at(0).durationSec, 25 * 60);
        QCOMPARE(recorder->lastWorkSessionId(), id);

        failSessionUpdates(false);
        QTRY_VERIFY(!recorder->isSaveFailing());
        const QList<SessionRecord> r = all();
        QCOMPARE(r.size(), 1);
        QCOMPARE(r.at(0).id, id);
        QCOMPARE(r.at(0).durationSec, 30 * 60);
        QCOMPARE(recorded.size(), 2); // the calendar replaces its event
        QCOMPARE(recorded.at(1).at(0).value<SessionRecord>().id, id);
    }

    void extraTimeWaitingForADeletedSessionIsDropped()
    {
        recorder->setRetryIntervalForTesting(30);
        engine->start();
        elapse(25 * 60);
        const qint64 id = all().at(0).id;
        failSessionUpdates(true);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Cannot update session")));
        QVERIFY(engine->continueFinished());
        elapse(5 * 60);
        QVERIFY(recorder->isSaveFailing());
        failSessionUpdates(false);
        QVERIFY(sessions->deleteById(id));
        recorder->forgetSession(id);
        QVERIFY(!recorder->isSaveFailing());
        QTest::qWait(60);
        QVERIFY(all().isEmpty());
    }

    void sessionsTheDatabaseRefusesAreKeptAndStoredLater()
    {
        QSignalSpy failing(recorder.get(), &SessionRecorder::saveFailingChanged);
        QSignalSpy workRecorded(recorder.get(), &SessionRecorder::workSessionRecorded);
        recorder->setRetryIntervalForTesting(30);
        failSessionInserts(true);
        engine->start();
        elapse(25 * 60);
        QVERIFY(all().isEmpty());
        QVERIFY(recorder->isSaveFailing());
        QCOMPARE(failing.count(), 1);
        QCOMPARE(recorder->lastWorkSessionId(), -1);
        QCOMPARE(workRecorded.count(), 0);

        // A second phase ends while the database still refuses: both wait, in order.
        engine->start();
        elapse(5 * 60);
        QVERIFY(all().isEmpty());
        QCOMPARE(failing.count(), 1);

        failSessionInserts(false);
        QTRY_VERIFY(!recorder->isSaveFailing());
        QCOMPARE(failing.count(), 2);
        const QList<SessionRecord> r = all();
        QCOMPARE(r.size(), 2);
        QCOMPARE(r.at(0).kind, SessionKind::Work);
        QCOMPARE(r.at(1).kind, SessionKind::ShortBreak);
        QCOMPARE(workRecorded.count(), 1); // the calendar still hears about it
    }

    void refusedSessionsGoInBeforeTheNextOne()
    {
        recorder->setRetryIntervalForTesting(60 * 60 * 1000); // only the next phase end retries
        failSessionInserts(true);
        engine->start();
        elapse(25 * 60);
        QVERIFY(recorder->isSaveFailing());
        failSessionInserts(false);
        engine->start();
        elapse(5 * 60);
        QVERIFY(!recorder->isSaveFailing());
        const QList<SessionRecord> r = all();
        QCOMPARE(r.size(), 2);
        QVERIFY(r.at(0).id < r.at(1).id);
        QCOMPARE(r.at(0).kind, SessionKind::Work);
    }

    void quittingWhileIdleStoresTheSessionsStillWaiting()
    {
        SessionRecorder::connectQuit(qApp, engine.get(), recorder.get()); // as main() does
        QSignalSpy workRecorded(recorder.get(), &SessionRecorder::workSessionRecorded);
        recorder->setRetryIntervalForTesting(60 * 60 * 1000); // no retry before the quit
        failSessionInserts(true);
        engine->start();
        elapse(25 * 60);
        QVERIFY(engine->isIdle()); // the break is not started: quitting ends no phase
        QVERIFY(recorder->isSaveFailing());

        failSessionInserts(false); // the database works again, but no phase will end before the quit
        quitApplication();
        QVERIFY(!recorder->isSaveFailing());
        const QList<SessionRecord> r = all();
        QCOMPARE(r.size(), 1);
        QCOMPARE(r.at(0).kind, SessionKind::Work);
        QVERIFY(r.at(0).completed);
        QCOMPARE(workRecorded.count(), 1);
    }

    void quittingLogsTheSessionsThatAreStillRefused()
    {
        SessionRecorder::connectQuit(qApp, engine.get(), recorder.get());
        recorder->setRetryIntervalForTesting(60 * 60 * 1000);
        failSessionInserts(true);
        engine->start();
        elapse(25 * 60); // a Work phase the database refuses
        engine->start();
        elapse(60); // a break still running at the quit

        // Both are counted: the running break was ended (and refused) before the last try.
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("^Quitting with 2 sessions that could not be stored")));
        quitApplication();
        QVERIFY(engine->isIdle());
        QVERIFY(all().isEmpty());
    }

    void completedWorkPhaseIsRecorded()
    {
        engine->start();
        elapse(25 * 60);

        const QList<SessionRecord> r = all();
        QCOMPARE(r.size(), 1);
        QCOMPARE(r.at(0).kind, SessionKind::Work);
        QVERIFY(r.at(0).completed);
        QCOMPARE(r.at(0).durationSec, 25 * 60);
        QCOMPARE(r.at(0).plannedSec, 25 * 60);
        QCOMPARE(r.at(0).startedAtMs, clock.base().toMSecsSinceEpoch());
        QCOMPARE(r.at(0).endedAtMs, clock.base().addSecs(25 * 60).toMSecsSinceEpoch());
        QCOMPARE(r.at(0).presetUuid, PresetModel::DefaultUuid);
        QCOMPARE(r.at(0).presetName, presets->get(PresetModel::DefaultUuid).value(QStringLiteral("name")).toString());
    }

    void breaksAreRecordedToo()
    {
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        engine->start();
        elapse(25 * 60);
        elapse(5 * 60);

        const QList<SessionRecord> r = all();
        QCOMPARE(r.size(), 2);
        QCOMPARE(r.at(0).kind, SessionKind::Work);
        QCOMPARE(r.at(1).kind, SessionKind::ShortBreak);
        QCOMPARE(r.at(1).durationSec, 5 * 60);
    }

    void accidentalStartsAreNotRecorded()
    {
        engine->start();
        elapse(4); // under the 5 s threshold
        engine->stop();
        QCOMPARE(sessions->count(), 0);
    }

    void interruptedSessionIsRecordedFromFiveSeconds()
    {
        engine->start();
        elapse(5);
        engine->stop();

        const QList<SessionRecord> r = all();
        QCOMPARE(r.size(), 1);
        QVERIFY(!r.at(0).completed);
        QCOMPARE(r.at(0).durationSec, 5);
        QCOMPARE(r.at(0).plannedSec, 25 * 60);
    }

    void skippedPhaseIsRecordedAsInterrupted()
    {
        engine->start();
        elapse(120);
        engine->skip();

        const QList<SessionRecord> r = all();
        QCOMPARE(r.size(), 1);
        QVERIFY(!r.at(0).completed);
        QCOMPARE(r.at(0).durationSec, 120);
    }

    void pausedTimeIsNotCounted()
    {
        engine->start();
        elapse(60);
        engine->pause();
        elapse(300);
        engine->resume();
        elapse(40);
        engine->stop();

        const SessionRecord r = all().at(0);
        QCOMPARE(r.durationSec, 100);
        QCOMPARE((r.endedAtMs - r.startedAtMs) / 1000, 400); // wall time includes the pause
    }

    void quitWhileRunningRecordsTheSession()
    {
        engine->start();
        elapse(90);
        engine->stop(); // what main() does on aboutToQuit
        QCOMPARE(all().size(), 1);
        QCOMPARE(all().at(0).durationSec, 90);
    }

    void switchingTimerAttributesTheRunningSessionToTheOldOne()
    {
        const QString study = presets->create({{QStringLiteral("name"), QStringLiteral("Study")},
                                               {QStringLiteral("category"), QStringLiteral("Uni")}});
        const QString defaultName = presets->get(PresetModel::DefaultUuid).value(QStringLiteral("name")).toString();

        engine->start(); // runs with the default timer
        elapse(60);
        presets->setCurrentUuid(study); // binding resets the engine, which ends the session

        QCOMPARE(all().size(), 1);
        QCOMPARE(all().at(0).presetName, defaultName);
        QCOMPARE(all().at(0).presetUuid, PresetModel::DefaultUuid);

        engine->start(); // now with "Study"
        elapse(30);
        engine->stop();
        QCOMPARE(all().size(), 2);
        QCOMPARE(all().at(1).presetName, QStringLiteral("Study"));
        QCOMPARE(all().at(1).category, QStringLiteral("Uni"));
    }

    void renamingAfterTheStartDoesNotChangeTheSnapshot()
    {
        const QString custom = presets->create({{QStringLiteral("name"), QStringLiteral("Before")},
                                                {QStringLiteral("category"), QStringLiteral("A")}});
        presets->setCurrentUuid(custom);
        engine->start();
        elapse(60);

        presets->update(custom, {{QStringLiteral("name"), QStringLiteral("After")},
                                 {QStringLiteral("category"), QStringLiteral("B")}});
        engine->stop();

        QCOMPARE(all().at(0).presetName, QStringLiteral("Before"));
        QCOMPARE(all().at(0).category, QStringLiteral("A"));
    }

    void notesCanBeSavedAndQueried()
    {
        SessionRecord r1 = makeRecord(1000);
        r1.note = QStringLiteral("Task 1");
        sessions->insert(r1);

        SessionRecord r2 = makeRecord(2000);
        qint64 lastId = 0;
        QVERIFY(sessions->insert(r2, &lastId));
        QVERIFY(lastId > 0);
        QVERIFY(sessions->updateNote(lastId, QStringLiteral("Task 2 updated")));

        const auto records = sessions->between(0, 10000);
        QCOMPARE(records.size(), 2);
        QCOMPARE(records.at(0).note, QStringLiteral("Task 1"));
        QCOMPARE(records.at(1).note, QStringLiteral("Task 2 updated"));

        const QStringList recent = sessions->recentNotes(5);
        QCOMPARE(recent.size(), 2);
        QCOMPARE(recent.at(0), QStringLiteral("Task 2 updated"));
        QCOMPARE(recent.at(1), QStringLiteral("Task 1"));
    }

    void noteForAMissingSessionIsRefused()
    {
        qint64 id = 0;
        QVERIFY(sessions->insert(makeRecord(1000), &id));
        QVERIFY(sessions->deleteAll());

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("there is no session")));
        QVERIFY(!sessions->updateNote(id, QStringLiteral("Lost")));
        QVERIFY(sessions->recentNotes(5).isEmpty());

        // Saving the note a session already has still counts as stored.
        QVERIFY(sessions->insert(makeRecord(2000), &id));
        QVERIFY(sessions->updateNote(id, QStringLiteral("Same")));
        QVERIFY(sessions->updateNote(id, QStringLiteral("Same")));
    }

    void recentNotesAreTrimmedDedupedAndOrderedByLastUse()
    {
        const QStringList notes = {QStringLiteral("Alpha"), QStringLiteral("Beta"), QStringLiteral("  Alpha "),
                                   QStringLiteral(""), QStringLiteral("   "), QStringLiteral("Gamma"),
                                   QStringLiteral("Beta")};
        qint64 start = 1000;
        for (const QString &note : notes) {
            SessionRecord r = makeRecord(start += 1000);
            r.note = note;
            QVERIFY(sessions->insert(r));
        }
        // Beta used last, then Gamma, then Alpha (its padded variant counts as the same note).
        QCOMPARE(sessions->recentNotes(5), (QStringList{QStringLiteral("Beta"), QStringLiteral("Gamma"), QStringLiteral("Alpha")}));
        // The limit applies after de-duplication.
        QCOMPARE(sessions->recentNotes(2), (QStringList{QStringLiteral("Beta"), QStringLiteral("Gamma")}));
    }

    void recentNotesTreatAllSurroundingWhitespaceAlike()
    {
        // A note typed with a trailing newline or a tab is the same note (as in the statistics).
        const QStringList notes = {QStringLiteral("Write report"), QStringLiteral("Write report\n"),
                                   QStringLiteral("\tWrite report "), QStringLiteral("\r\n"), QStringLiteral("Read\u00a0")};
        qint64 start = 1000;
        for (const QString &note : notes) {
            SessionRecord r = makeRecord(start += 1000);
            r.note = note;
            QVERIFY(sessions->insert(r));
        }
        QCOMPARE(sessions->recentNotes(5), (QStringList{QStringLiteral("Read"), QStringLiteral("Write report")}));
    }

        void insertReportsTheNewRowId()
    {
        qint64 first = 0;
        qint64 second = 0;
        QVERIFY(sessions->insert(makeRecord(1000), &first));
        QVERIFY(sessions->insert(makeRecord(2000), &second));
        QVERIFY(first > 0);
        QCOMPARE(second, first + 1);
    }

    void completedWorkStartsAfterReturnsOnlyNewRows()
    {
        QVERIFY(sessions->insert(makeRecord(1000)));
        QVERIFY(sessions->insert(makeRecord(2000, SessionKind::ShortBreak)));
        qint64 maxId = 0;
        QCOMPARE(sessions->completedWorkStartsAfter(0, &maxId), (QList<qint64>{1000}));
        QVERIFY(maxId > 0);

        SessionRecord interrupted = makeRecord(3000);
        interrupted.completed = false;
        QVERIFY(sessions->insert(interrupted));
        QVERIFY(sessions->insert(makeRecord(4000)));
        qint64 next = 0;
        QCOMPARE(sessions->completedWorkStartsAfter(maxId, &next), (QList<qint64>{4000}));
        QVERIFY(next > maxId);
        QVERIFY(sessions->completedWorkStartsAfter(next).isEmpty());
        QCOMPARE(sessions->completedWorkStartsAfter(0), (QList<qint64>{1000, 4000}));
    }

    void lastWorkSessionIdFollowsTheEndedWorkPhase()
    {
        QCOMPARE(recorder->lastWorkSessionId(), -1);

        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        engine->start();
        elapse(25 * 60); // work completes, short break starts
        const QList<SessionRecord> afterWork = all();
        QCOMPARE(afterWork.size(), 1);
        QCOMPARE(recorder->lastWorkSessionId(), afterWork.at(0).id);

        // A note typed during the break must still go to the work session, even once the
        // break itself is recorded (it is a newer row).
        elapse(5 * 60); // break completes, next work starts
        QCOMPARE(all().size(), 2);
        QCOMPARE(recorder->lastWorkSessionId(), afterWork.at(0).id);

        // A work phase too short to be recorded resets it.
        elapse(2);
        engine->stop();
        QCOMPARE(all().size(), 2);
        QCOMPARE(recorder->lastWorkSessionId(), -1);
    }
    void workTakenUpAgainUpdatesItsSession()
    {
        QSignalSpy recorded(recorder.get(), &SessionRecorder::workSessionRecorded);
        engine->start();
        elapse(25 * 60); // Manual: the short break waits
        QCOMPARE(all().size(), 1);
        const qint64 id = all().at(0).id;
        QVERIFY(sessions->updateNote(id, QStringLiteral("Report")));

        elapse(60); // a minute later: five more minutes
        QVERIFY(engine->continueFinished());
        elapse(5 * 60);

        const QList<SessionRecord> r = all();
        QCOMPARE(r.size(), 1);
        QCOMPARE(r.at(0).id, id);
        QVERIFY(r.at(0).completed);
        QCOMPARE(r.at(0).durationSec, 30 * 60);
        QCOMPARE(r.at(0).plannedSec, 30 * 60);
        QCOMPARE(r.at(0).startedAtMs, clock.base().toMSecsSinceEpoch());
        QCOMPARE(r.at(0).endedAtMs, clock.base().addSecs(31 * 60).toMSecsSinceEpoch());
        QCOMPARE(r.at(0).note, QStringLiteral("Report"));
        QCOMPARE(recorder->lastWorkSessionId(), id);
        // Sent again with the same id, so the calendar replaces its event, note included.
        QCOMPARE(recorded.size(), 2);
        const auto again = recorded.at(1).at(0).value<SessionRecord>();
        QCOMPARE(again.id, id);
        QCOMPARE(again.durationSec, 30 * 60);
        QCOMPARE(again.note, QStringLiteral("Report"));
    }

    void workTakenUpAgainAfterItsSessionWasDeletedIsStoredAnew()
    {
        engine->start();
        elapse(25 * 60); // Manual: the short break waits
        const qint64 id = all().at(0).id;
        QVERIFY(sessions->deleteById(id));
        recorder->forgetSession(id);
        QCOMPARE(recorder->lastWorkSessionId(), -1);

        elapse(60);
        QVERIFY(engine->continueFinished());
        elapse(5 * 60);

        const QList<SessionRecord> r = all();
        QCOMPARE(r.size(), 1);
        QVERIFY(r.at(0).id != id);
        QCOMPARE(r.at(0).durationSec, 30 * 60);
        QCOMPARE(recorder->lastWorkSessionId(), r.at(0).id);
    }

    void workTakenUpAgainKeepsTheTimerNameItBeganWith()
    {
        QSignalSpy recorded(recorder.get(), &SessionRecorder::workSessionRecorded);
        const QString custom = presets->create({{QStringLiteral("name"), QStringLiteral("Before")},
                                                {QStringLiteral("category"), QStringLiteral("A")}});
        presets->setCurrentUuid(custom);
        engine->start();
        elapse(60);
        presets->update(custom, {{QStringLiteral("name"), QStringLiteral("After")},
                                 {QStringLiteral("category"), QStringLiteral("B")}});
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All); // after the timer's own setting
        elapse(24 * 60); // the break starts by itself, with the timer as it is now
        QCOMPARE(engine->state(), TimerEngine::State::ShortBreak);
        QVERIFY(engine->continueFinished());
        elapse(5 * 60);

        QCOMPARE(all().size(), 1);
        QCOMPARE(all().at(0).presetName, QStringLiteral("Before"));
        QCOMPARE(recorded.size(), 2);
        const auto again = recorded.at(1).at(0).value<SessionRecord>();
        QCOMPARE(again.presetName, QStringLiteral("Before")); // what the calendar is sent
        QCOMPARE(again.category, QStringLiteral("A"));
    }

    void workStoredByARetryIsUpdatedWhenTakenUpAgain_data()
    {
        QTest::addColumn<bool>("retryBeforeReopening");
        QTest::newRow("stored before it is taken up again") << true;
        QTest::newRow("stored while it goes on") << false;
    }

    void workStoredByARetryIsUpdatedWhenTakenUpAgain()
    {
        QFETCH(bool, retryBeforeReopening);
        recorder->setRetryIntervalForTesting(30);
        failSessionInserts(true);
        engine->start();
        elapse(25 * 60);
        QVERIFY(recorder->isSaveFailing());
        if (retryBeforeReopening) {
            failSessionInserts(false);
            QTRY_VERIFY(!recorder->isSaveFailing());
            QVERIFY(engine->continueFinished());
        } else {
            QVERIFY(engine->continueFinished());
            failSessionInserts(false);
            QTRY_VERIFY(!recorder->isSaveFailing());
        }
        elapse(5 * 60);

        const QList<SessionRecord> r = all();
        QCOMPARE(r.size(), 1); // one session, not the first end and the longer one
        QCOMPARE(r.at(0).durationSec, 30 * 60);
    }

    void breakThatStartedByItselfGivesWayUnrecorded()
    {
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        engine->start();
        elapse(25 * 60);
        elapse(90); // the short break has run a while
        QVERIFY(engine->continueFinished());
        engine->stop(); // cut short at once: still the completed session, as long as it ran

        const QList<SessionRecord> r = all();
        QCOMPARE(r.size(), 1);
        QCOMPARE(r.at(0).kind, SessionKind::Work);
        QVERIFY(r.at(0).completed);
        QCOMPARE(r.at(0).durationSec, 25 * 60);
    }

    void extendingTheRunningPhaseRecordsOneLongerSession()
    {
        engine->start();
        elapse(20 * 60);
        QVERIFY(engine->extend());
        elapse(10 * 60);
        const QList<SessionRecord> r = all();
        QCOMPARE(r.size(), 1);
        QVERIFY(r.at(0).completed);
        QCOMPARE(r.at(0).plannedSec, 30 * 60);
        QCOMPARE(r.at(0).durationSec, 30 * 60);
    }

    // A page that cannot be read ends the query partway: the cursor must not move past the rows
    // that were never read, or the statistics would never count them.
    void completedWorkStartsAfterKeepsItsPlaceWhenAPageCannotBeRead()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("sugo.db"));
        {
            Database file(path);
            QVERIFY(file.open());
            SessionRepository writer(file.connectionName());
            for (int i = 0; i < 300; ++i) {
                SessionRecord r = makeRecord(1000 * (i + 1));
                r.note = QStringLiteral("session-note-%1").arg(i, 4, 10, QLatin1Char('0'));
                QVERIFY(writer.insert(r));
            }
            QCOMPARE(writer.completedWorkStartsAfter(0).size(), 300);
        }
        QVERIFY(DamagedDatabase::damagePagesHolding(path, QByteArray("session-note-0280")) > 0);

        DamagedDatabase::Connection damaged(path, QStringLiteral("damaged"));
        QVERIFY(damaged.isOpen());
        SessionRepository reader(damaged.name());
        qint64 maxId = -1;
        QVERIFY(reader.completedWorkStartsAfter(0, &maxId).isEmpty());
        QCOMPARE(maxId, 0);
    }
};

QTEST_GUILESS_MAIN(SessionsTest)
#include "tst_sessions.moc"
