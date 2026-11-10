// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Settings.h"
#include "core/Telemetry.h"
#include "core/TelemetryInfo.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <QUuid>

namespace
{
QSet<QString> keysOf(const QJsonObject &object)
{
    const QStringList keys = object.keys();
    return QSet<QString>(keys.cbegin(), keys.cend());
}

/// Keeps what Telemetry sends; the test answers each batch by hand.
class FakeTelemetryTransport : public TelemetryTransport
{
public:
    using TelemetryTransport::TelemetryTransport;
    void send(const QByteArray &json, std::function<void(int)> done) override
    {
        sent.append(QJsonDocument::fromJson(json).object());
        pending = std::move(done);
    }
    void abort() override
    {
        ++aborts;
        pending = nullptr;
    }
    void answer(int status)
    {
        QVERIFY(pending);
        auto done = std::move(pending);
        pending = nullptr;
        done(status);
    }

    QList<QJsonObject> sent;
    std::function<void(int)> pending;
    int aborts = 0;
};

const QDateTime kNow(QDate(2026, 11, 8), QTime(14, 37, 12), QTimeZone::UTC);
} // namespace

class TestTelemetry : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_path;
    Settings::Ptr m_settings;
    FakeTelemetryTransport *m_transport = nullptr;
    std::unique_ptr<Telemetry> m_telemetry;

    Telemetry *make()
    {
        m_telemetry.reset();
        delete m_transport;
        m_transport = new FakeTelemetryTransport;
        m_telemetry = std::make_unique<Telemetry>(m_settings, m_transport);
        m_telemetry->setNow([]() { return kNow; });
        // Long timers: the test calls flush() itself.
        m_telemetry->setTimingForTesting(3600 * 1000, 3600 * 1000, {3600 * 1000});
        m_telemetry->setStartProperties([]() { return QJsonObject{{QStringLiteral("os"), QStringLiteral("linux")}}; });
        return m_telemetry.get();
    }

private Q_SLOTS:
    void init()
    {
        m_path = m_dir.filePath(QStringLiteral("sugo-%1.conf").arg(QTest::currentTestFunction()));
        QFile::remove(m_path);
        m_settings = Settings::open(m_path);
    }

    void cleanup()
    {
        m_telemetry.reset();
        delete m_transport;
        m_transport = nullptr;
        m_settings.reset();
    }

    void offRecordsAndSendsNothing()
    {
        Telemetry *t = make();
        t->setEnabled(false);
        t->recordSession(TimerEngine::Phase::Work, 25 * 60 * 1000, true);
        t->flush();
        QVERIFY(t->installId().isEmpty());
        QVERIFY(t->queue().isEmpty());
        QVERIFY(m_transport->sent.isEmpty());
        QVERIFY(m_settings->allKeys().isEmpty());
    }

    void turningOnMakesAnIdAndRecordsTheStart()
    {
        Telemetry *t = make();
        t->setEnabled(true);
        const QString id = t->installId();
        const QUuid uuid(id);
        QVERIFY(!uuid.isNull());
        QCOMPARE(uuid.version(), QUuid::Random);
        QCOMPARE(id, id.toLower());
        QCOMPARE(m_settings->value(QStringLiteral("Telemetry/Id")).toString(), id);

        // The start is recorded at the first send.
        QVERIFY(t->queue().isEmpty());
        t->flush();
        QCOMPARE(m_transport->sent.size(), 1);
        QCOMPARE(t->queue().size(), 1);
        const QJsonObject started = t->queue().at(0).toObject();
        QCOMPARE(started.value(QStringLiteral("event")).toString(), QStringLiteral("app_started"));
        QCOMPARE(started.value(QStringLiteral("ts")).toString(), QStringLiteral("2026-11-08T14:00:00Z"));
        QCOMPARE(started.value(QStringLiteral("props")).toObject().value(QStringLiteral("os")).toString(), QStringLiteral("linux"));

        // Once per run: turning it on again does not repeat it, the id stays.
        m_transport->answer(202);
        t->setEnabled(true);
        t->recordSession(TimerEngine::Phase::Work, 25 * 60 * 1000, true);
        t->flush();
        QCOMPARE(m_transport->sent.size(), 2);
        QCOMPARE(t->queue().size(), 1);
        QCOMPARE(t->queue().at(0).toObject().value(QStringLiteral("event")).toString(), QStringLiteral("session_ended"));
        QCOMPARE(t->installId(), id);
    }

    void theIdSurvivesARestart()
    {
        make()->setEnabled(true);
        m_telemetry->flush(); // records the start, not answered
        const QString id = m_telemetry->installId();
        Telemetry *again = make();
        again->setEnabled(true);
        QCOMPARE(again->installId(), id);
        // Each run records its own start; the unanswered one is still queued.
        again->flush();
        QCOMPARE(again->queue().size(), 2);
    }

    void turningOffForgetsEverything()
    {
        Telemetry *t = make();
        t->setEnabled(true);
        const QString first = t->installId();
        t->recordSession(TimerEngine::Phase::Work, 25 * 60 * 1000, true);
        t->flush();
        QVERIFY(t->isSending());

        t->setEnabled(false);
        QCOMPARE(m_transport->aborts, 1);
        QVERIFY(!t->isSending());
        QVERIFY(t->installId().isEmpty());
        QVERIFY(t->queue().isEmpty());
        QVERIFY(!m_settings->contains(QStringLiteral("Telemetry/Id")));
        QVERIFY(!m_settings->contains(QStringLiteral("Telemetry/Queue")));

        // Back on: a new id, and the start again.
        t->setEnabled(true);
        QVERIFY(!t->installId().isEmpty());
        QVERIFY(t->installId() != first);
        t->flush();
        QCOMPARE(t->queue().size(), 1);
        QCOMPARE(t->queue().at(0).toObject().value(QStringLiteral("event")).toString(), QStringLiteral("app_started"));
    }

    void startingOffDropsWhatAnEarlierRunLeft()
    {
        m_settings->setValue(QStringLiteral("Telemetry/Id"), QStringLiteral("6f1c2a3b-4d5e-4f60-8a7b-9c0d1e2f3a4b"));
        m_settings->setValue(QStringLiteral("Telemetry/Queue"), QStringLiteral("[{\"event\":\"session_ended\"}]"));
        Telemetry *t = make();
        t->setEnabled(false);
        QVERIFY(t->installId().isEmpty());
        QVERIFY(t->queue().isEmpty());
        QVERIFY(m_settings->allKeys().isEmpty());
    }

    void sessionsBecomeEvents_data()
    {
        QTest::addColumn<int>("phase");
        QTest::addColumn<qint64>("plannedMs");
        QTest::addColumn<bool>("completed");
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("minutes");
        QTest::newRow("work") << int(TimerEngine::Phase::Work) << qint64(25 * 60 * 1000) << true << QStringLiteral("work") << 25;
        QTest::newRow("short") << int(TimerEngine::Phase::ShortBreak) << qint64(5 * 60 * 1000 + 20 * 1000) << false << QStringLiteral("short_break") << 5;
        QTest::newRow("long") << int(TimerEngine::Phase::LongBreak) << qint64(14 * 60 * 1000 + 40 * 1000) << true << QStringLiteral("long_break") << 15;
        QTest::newRow("tiny") << int(TimerEngine::Phase::Work) << qint64(10 * 1000) << false << QStringLiteral("work") << 1;
        QTest::newRow("huge") << int(TimerEngine::Phase::Work) << qint64(10 * 3600 * 1000) << true << QStringLiteral("work") << 240;
    }

    void sessionsBecomeEvents()
    {
        QFETCH(int, phase);
        QFETCH(qint64, plannedMs);
        QFETCH(bool, completed);
        QFETCH(QString, name);
        QFETCH(int, minutes);
        Telemetry *t = make();
        t->setEnabled(true);
        t->recordSession(TimerEngine::Phase(phase), plannedMs, completed);
        const QJsonObject event = t->queue().last().toObject();
        QCOMPARE(event.value(QStringLiteral("event")).toString(), QStringLiteral("session_ended"));
        const QJsonObject props = event.value(QStringLiteral("props")).toObject();
        QCOMPARE(keysOf(props),
                 QSet<QString>({QStringLiteral("phase"), QStringLiteral("planned_min"), QStringLiteral("completed")}));
        QCOMPARE(props.value(QStringLiteral("phase")).toString(), name);
        QCOMPARE(props.value(QStringLiteral("planned_min")).toInt(), minutes);
        QCOMPARE(props.value(QStringLiteral("completed")).toBool(), completed);
    }

    void sendsBatchesAndDropsWhatWasDelivered()
    {
        Telemetry *t = make();
        t->setEnabled(true);
        for (int i = 0; i < 59; ++i) {
            t->recordSession(TimerEngine::Phase::Work, 25 * 60 * 1000, true);
        }
        QCOMPARE(t->queue().size(), 59);
        t->flush(); // and the start: 60

        QCOMPARE(m_transport->sent.size(), 1);
        const QJsonObject body = m_transport->sent.at(0);
        QCOMPARE(keysOf(body), QSet<QString>({QStringLiteral("id"), QStringLiteral("events")}));
        QCOMPARE(body.value(QStringLiteral("id")).toString(), t->installId());
        QCOMPARE(body.value(QStringLiteral("events")).toArray().size(), Telemetry::maxBatch);

        t->flush(); // one at a time
        QCOMPARE(m_transport->sent.size(), 1);

        m_transport->answer(202);
        QCOMPARE(t->queue().size(), 10);
        // The rest goes right after.
        QTRY_COMPARE(m_transport->sent.size(), 2);
        QCOMPARE(m_transport->sent.at(1).value(QStringLiteral("events")).toArray().size(), 10);
        m_transport->answer(202);
        QVERIFY(t->queue().isEmpty());
        QVERIFY(!m_settings->contains(QStringLiteral("Telemetry/Queue")));
    }

    void keepsTheBatchWhenTheServerCannotBeReached()
    {
        Telemetry *t = make();
        t->setTimingForTesting(3600 * 1000, 3600 * 1000, {10, 20});
        t->setEnabled(true);
        t->flush();
        m_transport->answer(0);
        QCOMPARE(t->queue().size(), 1);
        QTRY_COMPARE(m_transport->sent.size(), 2); // retried
        m_transport->answer(503);
        QTRY_COMPARE(m_transport->sent.size(), 3);
        m_transport->answer(429);
        QCOMPARE(t->queue().size(), 1);
        QTRY_COMPARE(m_transport->sent.size(), 4);
        m_transport->answer(202);
        QVERIFY(t->queue().isEmpty());
    }

    void dropsABatchTheServerRefuses()
    {
        Telemetry *t = make();
        t->setEnabled(true);
        t->flush();
        m_transport->answer(400);
        QVERIFY(t->queue().isEmpty());
    }

    void keepsTheBatchOnARedirect()
    {
        Telemetry *t = make();
        t->setEnabled(true);
        t->flush();
        m_transport->answer(308); // moved elsewhere: not followed, not lost
        QCOMPARE(t->queue().size(), 1);
    }

    void theQueueHasACapAndSurvivesARestart()
    {
        Telemetry *t = make();
        t->setEnabled(true);
        for (int i = 0; i < Telemetry::maxQueued + 20; ++i) {
            t->recordSession(TimerEngine::Phase::Work, qint64(1 + i % 200) * 60 * 1000, true);
        }
        QCOMPARE(t->queue().size(), Telemetry::maxQueued);
        // The oldest went first: the start is gone.
        QCOMPARE(t->queue().at(0).toObject().value(QStringLiteral("event")).toString(), QStringLiteral("session_ended"));
        const QJsonArray before = t->queue();

        Telemetry *again = make();
        QCOMPARE(again->queue(), before);
    }

    void eventsPushedOutWhileSendingAreNotCountedTwice()
    {
        Telemetry *t = make();
        t->setEnabled(true);
        for (int i = 1; i < Telemetry::maxQueued; ++i) {
            t->recordSession(TimerEngine::Phase::Work, 60 * 1000, true);
        }
        t->flush(); // the first 50
        for (int i = 0; i < 5; ++i) {
            t->recordSession(TimerEngine::Phase::ShortBreak, 5 * 60 * 1000, true); // push 5 of them out
        }
        m_transport->answer(202);
        // 5 of the 50 sent were already gone: only the other 45 are taken out.
        QCOMPARE(t->queue().size(), Telemetry::maxQueued - (Telemetry::maxBatch - 5));
        QCOMPARE(t->queue().at(0).toObject().value(QStringLiteral("props")).toObject().value(QStringLiteral("phase")).toString(),
                 QStringLiteral("work"));
        QCOMPARE(t->queue().last().toObject().value(QStringLiteral("props")).toObject().value(QStringLiteral("phase")).toString(),
                 QStringLiteral("short_break"));
    }

    void theAnswerToABatchSentBeforeTurningOffIsIgnored()
    {
        Telemetry *t = make();
        t->setEnabled(true);
        t->flush();
        auto late = m_transport->pending; // the answer arrives although abort() was asked
        t->setEnabled(false);
        t->setEnabled(true);
        t->recordSession(TimerEngine::Phase::Work, 25 * 60 * 1000, true);
        QCOMPARE(t->queue().size(), 1);
        late(202);
        QCOMPARE(t->queue().size(), 1);
    }

    void optInAndTourEvents()
    {
        Telemetry *t = make();
        t->tourEnded(2, true); // off: nothing
        t->setEnabled(true, QStringLiteral("welcome"));
        t->tourEnded(3, false);
        t->tourEnded(42, true);
        QCOMPARE(t->queue().size(), 3);
        const auto at = [t](int i) { return t->queue().at(i).toObject(); };
        QCOMPARE(at(0).value(QStringLiteral("event")).toString(), QStringLiteral("stats_enabled"));
        QCOMPARE(at(0).value(QStringLiteral("props")).toObject(), QJsonObject({{QStringLiteral("source"), QStringLiteral("welcome")}}));
        QCOMPARE(at(1).value(QStringLiteral("props")).toObject(),
                 QJsonObject({{QStringLiteral("result"), QStringLiteral("done")}, {QStringLiteral("step"), 3}}));
        QCOMPARE(at(2).value(QStringLiteral("props")).toObject().value(QStringLiteral("step")).toInt(), 9); // clamped

        // On at startup (no source): no stats_enabled. An unknown source is ignored too.
        Telemetry *again = make();
        again->setEnabled(true);
        QCOMPARE(again->queue().size(), 3);
        again->setEnabled(false);
        again->setEnabled(true, QStringLiteral("somewhere"));
        QVERIFY(again->queue().isEmpty());
    }

    void extendEvents()
    {
        Telemetry *t = make();
        t->recordExtend(TimerEngine::Phase::Work, false, TimerEngine::ExtendSource::App, 300); // off
        t->setEnabled(true);
        t->recordExtend(TimerEngine::Phase::Work, true, TimerEngine::ExtendSource::Notification, 300);
        t->recordExtend(TimerEngine::Phase::ShortBreak, false, TimerEngine::ExtendSource::Menu, 600);
        QCOMPARE(t->queue().size(), 2);
        const auto at = [t](int i) { return t->queue().at(i).toObject(); };
        QCOMPARE(at(0).value(QStringLiteral("event")).toString(), QStringLiteral("timer_extended"));
        QCOMPARE(at(0).value(QStringLiteral("props")).toObject(),
                 QJsonObject({{QStringLiteral("phase"), QStringLiteral("work")},
                              {QStringLiteral("moment"), QStringLiteral("ended")},
                              {QStringLiteral("source"), QStringLiteral("notification")},
                              {QStringLiteral("minutes"), 5}}));
        QCOMPARE(at(1).value(QStringLiteral("props")).toObject().value(QStringLiteral("source")).toString(), QStringLiteral("menu"));
        QCOMPARE(at(1).value(QStringLiteral("props")).toObject().value(QStringLiteral("minutes")).toInt(), 10);

        // The session taken up again was counted when it first ended.
        t->recordSession(TimerEngine::Phase::Work, 25 * 60 * 1000, true);
        QCOMPARE(t->queue().size(), 3);
        t->skipNextSession();
        t->recordSession(TimerEngine::Phase::Work, 30 * 60 * 1000, true);
        QCOMPARE(t->queue().size(), 3);
        t->skipNextSession(); // and taken up once more: still the session counted at first
        t->recordSession(TimerEngine::Phase::Work, 35 * 60 * 1000, true);
        QCOMPARE(t->queue().size(), 3);
        t->recordSession(TimerEngine::Phase::ShortBreak, 5 * 60 * 1000, true);
        QCOMPARE(t->queue().size(), 4);

        // Ended while the statistics were off, taken up again, turned on before its new end:
        // that end is the only one sent.
        Telemetry *off = make();
        off->recordSession(TimerEngine::Phase::Work, 25 * 60 * 1000, true);
        off->skipNextSession();
        off->setEnabled(true);
        const qsizetype before = off->queue().size();
        off->recordSession(TimerEngine::Phase::Work, 30 * 60 * 1000, true);
        QCOMPARE(off->queue().size(), before + 1);
    }

    void restoreEvents()
    {
        Telemetry *t = make();
        t->recordRestore(TimerEngine::Phase::Work, QStringLiteral("resumed")); // off
        t->setEnabled(true);
        t->recordRestore(TimerEngine::Phase::Work, QStringLiteral("resumed"));
        t->recordRestore(TimerEngine::Phase::LongBreak, QStringLiteral("expired"));
        t->recordRestore(TimerEngine::Phase::Work, QStringLiteral("eaten")); // not a result
        QCOMPARE(t->queue().size(), 2);
        QCOMPARE(t->queue().at(0).toObject().value(QStringLiteral("event")).toString(), QStringLiteral("session_restored"));
        QCOMPARE(t->queue().at(1).toObject().value(QStringLiteral("props")).toObject(),
                 QJsonObject({{QStringLiteral("phase"), QStringLiteral("long_break")}, {QStringLiteral("result"), QStringLiteral("expired")}}));
    }

    void updateInstallEvents()
    {
        Telemetry *t = make();
        t->recordUpdateInstall(false, QStringLiteral("download")); // off
        t->setEnabled(true);
        t->recordUpdateInstall(false, QStringLiteral("verify"));
        t->recordUpdateInstall(true, QStringLiteral("install"));
        t->recordUpdateInstall(false, QStringLiteral("Could not download the update: host not found")); // not a stage
        QCOMPARE(t->queue().size(), 2);
        QCOMPARE(t->queue().at(0).toObject().value(QStringLiteral("props")).toObject(),
                 QJsonObject({{QStringLiteral("ok"), false}, {QStringLiteral("stage"), QStringLiteral("verify")}}));
    }

    void syncErrorsAtMostOnceADayEach()
    {
        QDateTime now = kNow;
        Telemetry *t = make();
        t->setNow([&now]() { return now; });
        t->recordSyncError(QStringLiteral("calendar"), QStringLiteral("network")); // off
        t->setEnabled(true);
        for (int i = 0; i < 5; ++i) {
            t->recordSyncError(QStringLiteral("calendar"), QStringLiteral("network"));
        }
        t->recordSyncError(QStringLiteral("calendar"), QStringLiteral("auth"));
        t->recordSyncError(QStringLiteral("sugo_sync"), QStringLiteral("network"));
        t->recordSyncError(QStringLiteral("sugo_sync"), QString()); // no kind
        QCOMPARE(t->queue().size(), 3);
        now = now.addDays(1);
        t->recordSyncError(QStringLiteral("calendar"), QStringLiteral("network"));
        QCOMPARE(t->queue().size(), 4);
        QCOMPARE(t->queue().at(3).toObject().value(QStringLiteral("props")).toObject(),
                 QJsonObject({{QStringLiteral("area"), QStringLiteral("calendar")}, {QStringLiteral("kind"), QStringLiteral("network")}}));

        // Also after a restart the same day.
        Telemetry *restarted = make();
        restarted->setNow([&now]() { return now; });
        restarted->setEnabled(true);
        const qsizetype queued = restarted->queue().size();
        restarted->recordSyncError(QStringLiteral("calendar"), QStringLiteral("network"));
        QCOMPARE(restarted->queue().size(), queued);
        t = restarted;

        // Turned off and on: counted afresh.
        t->setEnabled(false);
        t->setEnabled(true);
        t->recordSyncError(QStringLiteral("calendar"), QStringLiteral("network"));
        QCOMPARE(t->queue().size(), 1);
    }

    void infoValues()
    {
        QCOMPARE(TelemetryInfo::channel(QStringLiteral("1.2.0")), QStringLiteral("stable"));
        QCOMPARE(TelemetryInfo::channel(QStringLiteral("1.2.1-nightly.20261108.1")), QStringLiteral("nightly"));
        QCOMPARE(TelemetryInfo::desktopFrom(QStringLiteral("KDE")), QStringLiteral("kde"));
        QCOMPARE(TelemetryInfo::desktopFrom(QStringLiteral("ubuntu:GNOME")), QStringLiteral("gnome"));
        QCOMPARE(TelemetryInfo::desktopFrom(QStringLiteral("X-Cinnamon")), QStringLiteral("other"));
        QCOMPARE(TelemetryInfo::desktopFrom(QString()), QStringLiteral("other"));
        QCOMPARE(TelemetryInfo::distroFrom("NAME=\"Fedora Linux\"\nID=fedora\nVERSION_ID=41\n"), QStringLiteral("fedora"));
        QCOMPARE(TelemetryInfo::distroFrom("ID=\"opensuse-tumbleweed\"\nID_LIKE=\"opensuse suse\"\n"), QStringLiteral("opensuse-tumbleweed"));
        QCOMPARE(TelemetryInfo::distroFrom("ID_LIKE=arch\nID=cachyos\n"), QStringLiteral("cachyos"));
        QCOMPARE(TelemetryInfo::distroFrom("ID=myhomebrew-alice\n"), QStringLiteral("other"));
        QCOMPARE(TelemetryInfo::distroFrom("ID=fedora\nVARIANT_ID=kinoite\n"), QStringLiteral("kinoite"));
        QCOMPARE(TelemetryInfo::distroFrom("ID=fedora\nVARIANT_ID=workstation\n"), QStringLiteral("fedora"));
        QCOMPARE(TelemetryInfo::distroFrom("NAME=\"Ubuntu\"\nID=ubuntu\nID_LIKE=debian\n"), QStringLiteral("ubuntu"));
        QCOMPARE(TelemetryInfo::distroFrom(""), QStringLiteral("other"));
        QCOMPARE(TelemetryInfo::presetsBand(0), QStringLiteral("1"));
        QCOMPARE(TelemetryInfo::presetsBand(1), QStringLiteral("1"));
        QCOMPARE(TelemetryInfo::presetsBand(3), QStringLiteral("2-3"));
        QCOMPARE(TelemetryInfo::presetsBand(9), QStringLiteral("4+"));
        QVERIFY(QRegularExpression(QStringLiteral("^\\d+\\.\\d+\\.\\d+$")).match(TelemetryInfo::qtVersion()).hasMatch());
        const QStringList packaging = {QStringLiteral("appimage"), QStringLiteral("flatpak"), QStringLiteral("source"), QStringLiteral("macos")};
        QVERIFY(packaging.contains(TelemetryInfo::packaging()));
    }
};

QTEST_GUILESS_MAIN(TestTelemetry)
#include "tst_telemetry.moc"
