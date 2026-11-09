// SPDX-License-Identifier: GPL-3.0-or-later
#include <QFile>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cstring>
#include <memory>

#include "FakeClock.h"
#include "FocusSoundController.h"
#include "TimerEngine.h"

class FocusSoundControllerTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir dir;
    QByteArray oldPath;
    QByteArray oldCapture;
    FakeClock clock;
    std::unique_ptr<TimerEngine> engine;
    std::unique_ptr<FocusSoundController> sounds;

    QString capture() const { return dir.filePath(QStringLiteral("audio.raw")); }

    /// The process ids of the players started so far, oldest first.
    QList<qint64> startedPlayers() const
    {
        QFile file(dir.filePath(QStringLiteral("audio.raw.pids")));
        QList<qint64> pids;
        if (file.open(QIODevice::ReadOnly)) {
            for (const QByteArray &line : file.readAll().split('\n')) {
                if (!line.isEmpty()) pids.append(line.toLongLong());
            }
        }
        return pids;
    }

    /// The names of the players started so far (those that failed too), oldest first.
    QStringList startedPrograms() const
    {
        QFile file(dir.filePath(QStringLiteral("audio.raw.programs")));
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts) : QStringList();
    }

    /// The fake player of that name exits with an error at once from now on.
    bool makeFail(const char *program)
    {
        QFile file(dir.filePath(QStringLiteral("audio.raw.fail-") + QLatin1String(program)));
        return file.open(QIODevice::WriteOnly);
    }

    /// The process exists and has not ended (a zombie not yet reaped counts as ended).
    static bool isRunning(qint64 pid)
    {
#ifdef Q_OS_LINUX
        QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
        if (!stat.open(QIODevice::ReadOnly)) return false;
        const QByteArray line = stat.readAll();
        const qsizetype end = line.lastIndexOf(')');
        return end >= 0 && line.size() > end + 2 && line.at(end + 2) != 'Z' && line.at(end + 2) != 'X';
#else
        // No /proc (macOS): ask ps for the process state; nothing printed if it is gone.
        QProcess ps;
        ps.start(QStringLiteral("ps"), {QStringLiteral("-o"), QStringLiteral("stat="), QStringLiteral("-p"), QString::number(pid)});
        if (!ps.waitForFinished(3000)) return false;
        const QByteArray state = ps.readAllStandardOutput().trimmed();
        return !state.isEmpty() && !state.startsWith('Z');
#endif
    }

    bool playerRunning() const
    {
        const QList<qint64> pids = startedPlayers();
        return !pids.isEmpty() && isRunning(pids.last());
    }

    /// A player is running and has been fed audio.
    bool soundPlaying() const { return playerRunning() && QFileInfo(capture()).size() > 0; }

    QByteArray capturedAudio() const
    {
        QFile file(capture());
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    /// The loudest of the samples [from, to) of mono s16 audio.
    static int loudest(const QByteArray &raw, qsizetype from, qsizetype to)
    {
        const auto *samples = reinterpret_cast<const qint16 *>(raw.constData());
        int peak = 0;
        for (qsizetype i = from; i < to; ++i) peak = std::max(peak, std::abs(int(samples[i])));
        return peak;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        const QString bin = dir.filePath(QStringLiteral("bin"));
        QVERIFY(QDir().mkpath(bin));
        // The same fake player under both names. Each start is recorded in .programs; a file
        // .fail-<name> makes that player exit with an error at once, as pw-play does where no
        // PipeWire daemon runs.
        for (const char *name : {"pw-play", "aplay"}) {
            QFile player(bin + QLatin1Char('/') + QLatin1String(name));
            QVERIFY(player.open(QIODevice::WriteOnly));
            player.write("#!/bin/sh\n"
                         "name=$(basename \"$0\")\n"
                         "echo \"$name\" >> \"$FOCUS_SOUND_CAPTURE.programs\"\n"
                         "[ -f \"$FOCUS_SOUND_CAPTURE.fail-$name\" ] && { echo \"$name: cannot connect\" >&2; exit 1; }\n"
                         "echo $$ >> \"$FOCUS_SOUND_CAPTURE.pids\"\n"
                         "exec cat > \"$FOCUS_SOUND_CAPTURE\"\n");
            player.close();
            QVERIFY(player.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        }
        oldPath = qgetenv("PATH");
        oldCapture = qgetenv("FOCUS_SOUND_CAPTURE");
        QByteArray path = bin.toLocal8Bit();
        path.append(':');
        path.append(oldPath);
        qputenv("PATH", path);
        qputenv("FOCUS_SOUND_CAPTURE", dir.filePath(QStringLiteral("audio.raw")).toLocal8Bit());
    }

    void cleanupTestCase()
    {
        qputenv("PATH", oldPath);
        if (oldCapture.isNull()) qunsetenv("FOCUS_SOUND_CAPTURE");
        else qputenv("FOCUS_SOUND_CAPTURE", oldCapture);
    }

    void init()
    {
        QFile::remove(dir.filePath(QStringLiteral("audio.raw")));
        QFile::remove(dir.filePath(QStringLiteral("audio.raw.pids")));
        QFile::remove(dir.filePath(QStringLiteral("audio.raw.programs")));
        QFile::remove(dir.filePath(QStringLiteral("audio.raw.fail-pw-play")));
        QFile::remove(dir.filePath(QStringLiteral("audio.raw.fail-aplay")));
        clock = FakeClock();
        engine = std::make_unique<TimerEngine>(&clock, nullptr);
        sounds = std::make_unique<FocusSoundController>();
#ifndef Q_OS_LINUX
        // Elsewhere (macOS) the default is QAudioSink; these tests watch the fake pw-play.
        sounds->setBackendForTesting(FocusSoundController::Backend::PwPlay);
#endif
        sounds->attach(engine.get(), nullptr);
        sounds->setMode(static_cast<int>(FocusSoundController::Mode::WhiteNoise));
        sounds->setVolume(45);
    }

    void cleanup()
    {
        sounds.reset();
        engine.reset();
    }

    void audioStartsForWorkAndStopsWhenPaused()
    {
        engine->start();
        QTRY_VERIFY_WITH_TIMEOUT(soundPlaying(), 3000);

        engine->pause();
        QTRY_VERIFY_WITH_TIMEOUT(!playerRunning(), 5000); // faded out, then ended
        engine->resume();
        QTRY_VERIFY_WITH_TIMEOUT(startedPlayers().size() == 2 && soundPlaying(), 3000);
        engine->stop();
        QTRY_VERIFY_WITH_TIMEOUT(!playerRunning(), 5000);
    }

    void quittingWhilePlayingEndsThePlayerFirst()
    {
        engine->start();
        QTRY_VERIFY_WITH_TIMEOUT(soundPlaying(), 3000);
        // The destructor runs after the event loop: nothing would delete the killed player later.
        QTest::failOnWarning(QRegularExpression(QStringLiteral("Destroyed while process")));
        sounds.reset();
    }

    void changingModeOffStopsPlayback()
    {
        engine->start();
        QTRY_VERIFY_WITH_TIMEOUT(soundPlaying(), 3000);
        sounds->setMode(static_cast<int>(FocusSoundController::Mode::Off));
        QTRY_VERIFY_WITH_TIMEOUT(!playerRunning(), 5000);
    }

    void changingModeWhilePlayingKeepsThePlayer()
    {
        engine->start();
        QTRY_VERIFY_WITH_TIMEOUT(soundPlaying(), 3000);
        sounds->setMode(static_cast<int>(FocusSoundController::Mode::Rain));
        engine->pause();
        engine->resume();
        sounds->setMode(static_cast<int>(FocusSoundController::Mode::ClockTick));
        QTest::qWait(2500); // longer than a whole fade-out: a player being stopped would be gone
        QVERIFY(playerRunning());
        QCOMPARE(startedPlayers().size(), 1); // the mode changes did not restart it
    }

    void aPlayerThatDiesWhileWorkingIsRestarted()
    {
        engine->start();
        QTRY_VERIFY_WITH_TIMEOUT(soundPlaying(), 3000);
        QCOMPARE(startedPlayers().size(), 1);
        const qint64 first = startedPlayers().first();
        QCOMPARE(QProcess::execute(QStringLiteral("kill"), {QStringLiteral("-KILL"), QString::number(first)}), 0);

        QTRY_VERIFY(!isRunning(first));
        // The restart waits kRestartDelayMs (1 s) on purpose, then starts a new player script; on
        // macOS every isRunning() check also runs ps and blocks. A loaded CI runner needs more than
        // 5 s for all of it, and a longer timeout costs nothing when the player comes back sooner.
        QTRY_VERIFY_WITH_TIMEOUT(startedPlayers().size() == 2 && soundPlaying(), 15000);
    }

    void aPlayerThatCannotPlayHereGivesWayToTheNext()
    {
#ifndef Q_OS_LINUX
        QSKIP("pw-play and aplay are the Linux players; macOS plays through Qt Multimedia only");
#endif
        // pw-play is installed but exits at once (no PipeWire daemon): aplay takes over.
        QVERIFY(makeFail("pw-play"));
        engine->start();
        QTRY_VERIFY_WITH_TIMEOUT(soundPlaying(), 3000);
        QCOMPARE(startedPrograms(), (QStringList{QStringLiteral("pw-play"), QStringLiteral("aplay")}));

        // Remembered: the next work phase goes straight to aplay.
        engine->pause();
        QTRY_VERIFY_WITH_TIMEOUT(!playerRunning(), 5000);
        engine->resume();
        QTRY_VERIFY_WITH_TIMEOUT(startedPlayers().size() == 2 && soundPlaying(), 3000);
        QCOMPARE(startedPrograms(), (QStringList{QStringLiteral("pw-play"), QStringLiteral("aplay"), QStringLiteral("aplay")}));
    }

    void whenNoPlayerProgramCanPlayQtMultimediaTakesOver()
    {
#ifndef Q_OS_LINUX
        QSKIP("pw-play and aplay are the Linux players; macOS plays through Qt Multimedia only");
#endif
        QVERIFY(makeFail("pw-play"));
        QVERIFY(makeFail("aplay"));
        sounds->setVolume(0);
        engine->start();
        // Each tried once, then never again: Qt Multimedia plays (or, without a device, retries).
        QTest::qWait(2500);
        QCOMPARE(startedPrograms(), (QStringList{QStringLiteral("pw-play"), QStringLiteral("aplay")}));
        QVERIFY(startedPlayers().isEmpty());
        if (!QMediaDevices::defaultAudioOutput().isNull()) {
            QVERIFY(sounds->isPlaying());
        }
        sounds.reset();
    }

    void keepsHalfASecondBufferedAhead()
    {
        engine->start();
        const QString capture = dir.filePath(QStringLiteral("audio.raw"));
        // 500 ms of mono s16 at 44.1 kHz is 44100 bytes, written right at the start.
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(capture).size() >= 44100, 3000);
    }

    void previewNeverChangesTheWorkPhaseVolume()
    {
        engine->start(); // playing at the work volume (45)
        QTRY_VERIFY_WITH_TIMEOUT(soundPlaying(), 3000);
        sounds->preview(static_cast<int>(FocusSoundController::Mode::Rain), 90);
        QCOMPARE(sounds->playbackVolume(), 90);
        QCOMPARE(sounds->volume(), 45);

        // A change of the setting during the preview is kept, not replaced by the preview's.
        sounds->setVolume(60);
        QCOMPARE(sounds->playbackVolume(), 90);
        sounds->stopPreview();
        QCOMPARE(sounds->volume(), 60);
        QCOMPARE(sounds->playbackVolume(), 60);
        // The work phase goes on with its sound: the player is not stopped.
        QTest::qWait(2500); // longer than a whole fade-out
        QVERIFY(playerRunning());
        QCOMPARE(startedPlayers().size(), 1);
    }

    void theEndOfAPreviewFadesOutThePreviewSound()
    {
        // Not working: the end of the preview stops the player, after a fade-out.
        sounds->preview(static_cast<int>(FocusSoundController::Mode::ClockTick), 50);
        QTRY_VERIFY_WITH_TIMEOUT(soundPlaying(), 3000);
        QTest::qWait(300); // let it fade in a little, so there is something to fade out
        sounds->stopPreview(); // the configured sound (white noise) does not take over
        QTRY_VERIFY_WITH_TIMEOUT(!playerRunning(), 5000);

        // A clock tick is silent but for 20 ms a second; white noise almost never is.
        const QByteArray raw = capturedAudio();
        const qsizetype n = raw.size() / 2;
        QVERIFY(n > 22050);
        const auto *samples = reinterpret_cast<const qint16 *>(raw.constData());
        const qsizetype sounding = std::count_if(samples, samples + n, [](qint16 s) { return s != 0; });
        QVERIFY2(sounding < n / 10, qPrintable(QStringLiteral("%1 of %2 samples not silent").arg(qlonglong(sounding)).arg(qlonglong(n))));
    }

    void theEndOfAPreviewKeepsItsVolume()
    {
        sounds->preview(static_cast<int>(FocusSoundController::Mode::WhiteNoise), 10);
        QTRY_VERIFY_WITH_TIMEOUT(soundPlaying(), 3000);
        QTest::qWait(1000); // well into the fade-in
        sounds->setVolume(100); // the setting changes during the preview
        sounds->stopPreview();
        QTRY_VERIFY_WITH_TIMEOUT(!playerRunning(), 5000);

        // White noise at 10 % never goes past half of a tenth of full scale.
        const QByteArray raw = capturedAudio();
        QVERIFY(raw.size() > 0);
        QVERIFY(loudest(raw, 0, raw.size() / 2) <= 1700);
    }

    void soundFadesInFromSilence()
    {
        engine->start();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(capture()).size() >= 44100, 3000);
        const QByteArray raw = capturedAudio().left(44100);
        QVERIFY(loudest(raw, 0, 100) < 50);         // starts nearly silent
        QVERIFY(loudest(raw, 20000, 22050) > 1000); // and grows (white noise at 45 %, a quarter in)
    }

    void pausingFadesOutBeforeTheEnd()
    {
        engine->start();
        QTRY_VERIFY_WITH_TIMEOUT(soundPlaying(), 3000);
        QTest::qWait(2500); // fully faded in
        engine->pause();
        QTRY_VERIFY_WITH_TIMEOUT(!playerRunning(), 5000);

        // 1.5 s of fade-out (66150 samples) at the end of what the player got.
        const QByteArray raw = capturedAudio();
        const qsizetype n = raw.size() / 2;
        QVERIFY(n > 90000);
        QVERIFY(loudest(raw, n - 90000, n - 80000) > 3000); // full volume before the pause
        const int fading = loudest(raw, n - 12000, n - 10000);
        QVERIFY(fading > 50 && fading < 3000);              // getting quieter
        QVERIFY(loudest(raw, n - 100, n) < 50);             // silent at the end
    }

    void resumingDuringTheFadeOutKeepsThePlayer()
    {
        engine->start();
        QTRY_VERIFY_WITH_TIMEOUT(soundPlaying(), 3000);
        QTest::qWait(300); // let it fade in a little, so there is something to fade out
        engine->pause();
        engine->resume();
        QTest::qWait(2500); // longer than a whole fade-out
        QVERIFY(playerRunning());
        QCOMPARE(startedPlayers().size(), 1); // still the first player
        const QByteArray raw = capturedAudio();
        QVERIFY(loudest(raw, raw.size() / 2 - 2205, raw.size() / 2) > 1000); // faded back in, not out
    }

    void invalidModesAreClamped()
    {
        sounds->setMode(9);
        QCOMPARE(sounds->mode(), FocusSoundController::LastMode);
        sounds->setMode(-2);
        QCOMPARE(sounds->mode(), 0);
    }
    void theBackendFollowsThePlayersThereAre()
    {
        using Backend = FocusSoundController::Backend;
        const auto with = [](QStringList programs) {
            return FocusSoundController::chooseBackend([programs](const QString &p) { return programs.contains(p); });
        };
#ifdef Q_OS_MACOS
        QCOMPARE(with({QStringLiteral("pw-play"), QStringLiteral("aplay")}), Backend::AudioSink);
#else
        QCOMPARE(with({QStringLiteral("pw-play"), QStringLiteral("aplay")}), Backend::PwPlay);
        QCOMPARE(with({QStringLiteral("aplay")}), Backend::Aplay);
        QCOMPARE(with({}), Backend::AudioSink);
#endif
    }

    void monoAudioIsConvertedForTheDevice()
    {
        QByteArray mono(4, Qt::Uninitialized);
        const qint16 in[2] = {16384, -32768};
        std::memcpy(mono.data(), in, sizeof(in));

        QCOMPARE(FocusSoundController::convertMonoS16(mono, 1, false), mono);
        const QByteArray stereo = FocusSoundController::convertMonoS16(mono, 2, false);
        QCOMPARE(stereo.size(), 8);
        const auto *s16 = reinterpret_cast<const qint16 *>(stereo.constData());
        QCOMPARE(s16[0], qint16(16384));
        QCOMPARE(s16[1], qint16(16384));
        QCOMPARE(s16[2], qint16(-32768));
        QCOMPARE(s16[3], qint16(-32768));

        const QByteArray floats = FocusSoundController::convertMonoS16(mono, 2, true);
        QCOMPARE(floats.size(), 16);
        float f[4];
        std::memcpy(f, floats.constData(), sizeof(f));
        QCOMPARE(f[0], 0.5f);
        QCOMPARE(f[1], 0.5f);
        QCOMPARE(f[2], -1.0f);
        QCOMPARE(f[3], -1.0f);
    }

    void theAudioSinkBackendCopesWithoutADevice()
    {
        // With an audio device it plays (silently here) and stops; without one (CI) it only logs.
        const bool haveDevice = !QMediaDevices::defaultAudioOutput().isNull();
        sounds->setBackendForTesting(FocusSoundController::Backend::AudioSink);
        sounds->setVolume(0);
        engine->start();
        if (haveDevice) {
            QTRY_VERIFY_WITH_TIMEOUT(sounds->isPlaying(), 3000);
        } else {
            QTest::qWait(1500); // failed, retried
            QVERIFY(!sounds->isPlaying());
        }
        engine->pause();
        QTRY_VERIFY_WITH_TIMEOUT(!sounds->isPlaying(), 5000); // nothing audible to fade: stopped
        engine->resume();
        if (haveDevice) {
            QTRY_VERIFY_WITH_TIMEOUT(sounds->isPlaying(), 3000);
        }
        QVERIFY(startedPlayers().isEmpty()); // no player process
        sounds.reset();
    }
};

QTEST_GUILESS_MAIN(FocusSoundControllerTest)
#include "tst_focussoundcontroller.moc"
