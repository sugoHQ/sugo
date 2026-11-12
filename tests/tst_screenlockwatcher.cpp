// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtGlobal>
#ifndef Q_OS_MACOS
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusObjectPath>
#endif
#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <utility>

#include "AppSettings.h"
#include "Autostart.h"
#include "FakeClock.h"
#include "PrivateDBus.h"
#include "ScreenLockWatcher.h"
#include "TimerEngine.h"

#ifndef Q_OS_MACOS
namespace
{
// True once `name` has no owner, as `bus` sees it. The bus sends NameOwnerChanged to `bus` before
// the answer that says the name is free, so the watchers on `bus` have been told, and the events
// that QtDBus posted for them are delivered before this returns.
bool nameReleased(const QDBusConnection &bus, const QString &name)
{
    const bool released = QTest::qWaitFor([&] { return !bus.interface()->isServiceRegistered(name).value(); });
    QCoreApplication::processEvents();
    return released;
}

// Returns once `receiver` has handled every signal `sender` sent before: the bus handles each
// connection's messages in order, so it has passed `sender`'s signals on when it answers its
// call, and `receiver` gets them before the answer to its own call. Waiting a while instead
// would prove nothing on a slow machine.
void flush(const QDBusConnection &sender, const QDBusConnection &receiver)
{
    const QString bus = QStringLiteral("org.freedesktop.DBus");
    sender.interface()->isServiceRegistered(bus);
    receiver.interface()->isServiceRegistered(bus);
    QCoreApplication::processEvents();
}

const QString kLogind = QStringLiteral("org.freedesktop.login1");
const QString kSessionInterface = QStringLiteral("org.freedesktop.login1.Session");
const QString kSessionPath = QStringLiteral("/org/freedesktop/login1/session/_37"); // session "7"

// logind's PropertiesChanged for LockedHint on `path`, as kscreenlocker, gnome-shell or
// `loginctl lock-session` with a locker that sets the hint make it send.
QDBusMessage lockedHintChanged(bool locked, const QString &path = kSessionPath)
{
    QDBusMessage signal = QDBusMessage::createSignal(path, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"));
    signal << kSessionInterface << QVariantMap{{QStringLiteral("LockedHint"), locked}} << QStringList();
    return signal;
}

// Disconnects the named connections when a test ends, failed or not: a data row that failed must
// not hand its connections, to a bus already gone, to the next row.
class Connections
{
public:
    explicit Connections(QStringList names)
        : m_names(std::move(names))
    {
    }
    ~Connections()
    {
        for (const QString &name : std::as_const(m_names)) {
            QDBusConnection::disconnectFromBus(name);
        }
    }

private:
    QStringList m_names;
};

// Sets XDG_SESSION_ID for one test (unset if `value` is null) and puts the old value back.
class SessionIdEnv
{
public:
    explicit SessionIdEnv(const QByteArray &value)
        : m_had(qEnvironmentVariableIsSet("XDG_SESSION_ID"))
        , m_saved(qgetenv("XDG_SESSION_ID"))
    {
        if (value.isNull()) {
            qunsetenv("XDG_SESSION_ID");
        } else {
            qputenv("XDG_SESSION_ID", value);
        }
    }
    ~SessionIdEnv()
    {
        if (m_had) {
            qputenv("XDG_SESSION_ID", m_saved);
        } else {
            qunsetenv("XDG_SESSION_ID");
        }
    }

private:
    bool m_had;
    QByteArray m_saved;
};
} // namespace

/// logind's Manager: GetSession knows session "7" only.
class FakeLogindManager : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.login1.Manager")

public Q_SLOTS:
    QDBusObjectPath GetSession(const QString &id)
    {
        if (id != QStringLiteral("7")) {
            sendErrorReply(QStringLiteral("org.freedesktop.login1.NoSuchSession"), QStringLiteral("No session '%1' known").arg(id));
            return {};
        }
        return QDBusObjectPath(kSessionPath);
    }
};

/// A logind session object: its Id and LockedHint, read with Properties.Get.
class FakeLogindSession : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.login1.Session")
    Q_PROPERTY(QString Id READ id)
    Q_PROPERTY(bool LockedHint READ lockedHint)

public:
    QString id() const { return QStringLiteral("7"); }
    bool lockedHint() const { return locked; }
    bool locked = false;
};

/// logind on a private bus that stands in for the system bus: the manager, session "7", and
/// session/auto, which logind resolves to the caller's session.
struct FakeLogind {
    FakeLogindManager manager;
    FakeLogindSession session;
    FakeLogindSession autoSession;

    bool registerOn(QDBusConnection &bus)
    {
        return bus.registerObject(QStringLiteral("/org/freedesktop/login1"), &manager, QDBusConnection::ExportAllSlots)
            && bus.registerObject(kSessionPath, &session, QDBusConnection::ExportAllProperties)
            && bus.registerObject(QStringLiteral("/org/freedesktop/login1/session/auto"), &autoSession, QDBusConnection::ExportAllProperties)
            && bus.registerService(kLogind);
    }
};
#endif

/// A screen locker's GetActive, for a locker that came back without announcing its state.
class FakeLocker : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.ScreenSaver")

public:
    explicit FakeLocker(bool active)
        : m_active(active)
    {
    }
    int calls = 0;

public Q_SLOTS:
    bool GetActive()
    {
        ++calls;
        return m_active;
    }

private:
    bool m_active;
};

class ScreenLockWatcherTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir dataDir;
    std::unique_ptr<FakeClock> clock;
    std::unique_ptr<TimerEngine> engine;
    std::unique_ptr<AppSettings> settings;
    std::unique_ptr<ScreenLockWatcher> watcher;

private Q_SLOTS:
    void initTestCase()
    {
        // Keep the settings file away from the user's real configuration: SUGO_DATA_DIR, which
        // Settings::defaultPath() reads first, in a directory of its own (test mode alone would
        // leave a file in ~/.qttest, and an exported SUGO_DATA_DIR would be written to).
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(dataDir.isValid());
        qputenv("SUGO_DATA_DIR", QFile::encodeName(dataDir.path()));
    }

    void init()
    {
        clock = std::make_unique<FakeClock>();
        engine = std::make_unique<TimerEngine>(clock.get(), nullptr);
        settings = std::make_unique<AppSettings>(Settings::openDefault(), nullptr, false);
        settings->setAutoPauseOnScreenLock(true);

        // Listening to nothing (on Linux: disconnected buses), so tests don't touch the system's events
        watcher = std::make_unique<ScreenLockWatcher>(ScreenLockWatcher::Source::Nothing);
        watcher->attach(engine.get(), settings.get());
    }

    void lockPausesActiveWorkTimer()
    {
        engine->start();
        QCOMPARE(engine->state(), TimerEngine::State::Working);

        QSignalSpy pauseSpy(watcher.get(), &ScreenLockWatcher::pausedByScreenLockChanged);
        QSignalSpy unlockSpy(watcher.get(), &ScreenLockWatcher::screenUnlockedAfterPause);

        // Screen is locked
        watcher->onActiveChanged(true);

        QCOMPARE(engine->state(), TimerEngine::State::Paused);
        QVERIFY(watcher->isPausedByScreenLock());
        QCOMPARE(pauseSpy.count(), 1);

        // Screen is unlocked
        watcher->onActiveChanged(false);
        QCOMPARE(unlockSpy.count(), 1);
        QVERIFY(watcher->isPausedByScreenLock());

        // Resuming clears the flag
        engine->resume();
        QCOMPARE(engine->state(), TimerEngine::State::Working);
        QVERIFY(!watcher->isPausedByScreenLock());
    }

    void disabledSettingDoesNotPause()
    {
        settings->setAutoPauseOnScreenLock(false);
        engine->start();
        QCOMPARE(engine->state(), TimerEngine::State::Working);

        watcher->onActiveChanged(true);

        QCOMPARE(engine->state(), TimerEngine::State::Working);
        QVERIFY(!watcher->isPausedByScreenLock());
    }

    void lockWhileAlreadyPausedDoesNotMarkScreenLock()
    {
        engine->start();
        engine->pause();
        QCOMPARE(engine->state(), TimerEngine::State::Paused);

        watcher->onActiveChanged(true);
        QVERIFY(!watcher->isPausedByScreenLock());
    }

    void theSameChangeReportedTwiceCountsOnce()
    {
        engine->start();
        QSignalSpy pauseSpy(watcher.get(), &ScreenLockWatcher::pausedByScreenLockChanged);
        QSignalSpy unlockSpy(watcher.get(), &ScreenLockWatcher::screenUnlockedAfterPause);

        // Plasma sends ActiveChanged on /ScreenSaver and /org/freedesktop/ScreenSaver.
        watcher->onActiveChanged(true);
        watcher->onActiveChanged(true);
        QCOMPARE(pauseSpy.count(), 1);

        watcher->onActiveChanged(false);
        watcher->onActiveChanged(false);
        QCOMPARE(unlockSpy.count(), 1);
    }

    void clearNoticeClearsFlag()
    {
        engine->start();
        watcher->onActiveChanged(true);
        QVERIFY(watcher->isPausedByScreenLock());

        watcher->clearLockPauseNotice();
        QVERIFY(!watcher->isPausedByScreenLock());
    }

    void sleepPausesTheRunningPhaseUntilTheUserResumes()
    {
        engine->start();
        QSignalSpy sleepSpy(watcher.get(), &ScreenLockWatcher::pausedBySleepChanged);
        QSignalSpy wokeSpy(watcher.get(), &ScreenLockWatcher::wokeUpAfterPause);

        watcher->onPrepareForSleep(true);
        QCOMPARE(engine->state(), TimerEngine::State::Paused);
        QVERIFY(watcher->isPausedBySleep());
        QCOMPARE(sleepSpy.count(), 1);

        // An hour asleep: on waking up the countdown has not moved on, and stays paused.
        const int remaining = engine->remainingSeconds();
        clock->advanceSeconds(60 * 60);
        engine->refresh();
        watcher->onPrepareForSleep(false);
        QCOMPARE(wokeSpy.count(), 1);
        QCOMPARE(engine->state(), TimerEngine::State::Paused);
        QCOMPARE(engine->remainingSeconds(), remaining);

        engine->resume();
        QVERIFY(!watcher->isPausedBySleep());
    }

    void sleepPausesBreaksTooAndIgnoresIdle()
    {
        watcher->onPrepareForSleep(true);
        QCOMPARE(engine->state(), TimerEngine::State::Idle);
        QVERIFY(!watcher->isPausedBySleep());
        watcher->onPrepareForSleep(false);

        engine->start();
        engine->skip(); // into the short break
        QCOMPARE(engine->state(), TimerEngine::State::ShortBreak);
        watcher->onPrepareForSleep(true);
        QCOMPARE(engine->state(), TimerEngine::State::Paused);
        QVERIFY(watcher->isPausedBySleep());
    }

    void sleepWhenThePhaseIsAlreadyDuePausesTheNextOne()
    {
        // The deadline passed but no tick has noticed yet; the break that follows starts by itself.
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        engine->start();
        clock->advanceSeconds(engine->remainingSeconds() + 1);
        QSignalSpy wokeSpy(watcher.get(), &ScreenLockWatcher::wokeUpAfterPause);

        watcher->onPrepareForSleep(true);
        QCOMPARE(engine->state(), TimerEngine::State::Paused);
        QVERIFY(watcher->isPausedBySleep());
        watcher->onPrepareForSleep(false);
        QCOMPARE(wokeSpy.count(), 1);
    }

    void lockWhenWorkIsAlreadyDueLeavesTheBreakRunning()
    {
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        engine->start();
        clock->advanceSeconds(engine->remainingSeconds() + 1);

        watcher->onActiveChanged(true);
        QCOMPARE(engine->state(), TimerEngine::State::ShortBreak);
        QVERIFY(!watcher->isPausedByScreenLock());
    }

    void aWorkThatStartsByItselfWhileLockedIsPaused()
    {
        // The screen is locked during a break, which is not paused; when it ends, the work that
        // starts by itself must not run on behind the lock screen.
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        engine->start();
        engine->skip();
        QCOMPARE(engine->state(), TimerEngine::State::ShortBreak);
        QSignalSpy pauseSpy(watcher.get(), &ScreenLockWatcher::pausedByScreenLockChanged);
        QSignalSpy unlockSpy(watcher.get(), &ScreenLockWatcher::screenUnlockedAfterPause);
        watcher->onActiveChanged(true);
        QCOMPARE(engine->state(), TimerEngine::State::ShortBreak);
        QVERIFY(!watcher->isPausedByScreenLock());

        clock->advanceSeconds(engine->remainingSeconds() + 1);
        engine->refresh(); // what the next tick does
        QCOMPARE(engine->phase(), TimerEngine::Phase::Work);
        QCOMPARE(engine->state(), TimerEngine::State::Paused);
        QVERIFY(watcher->isPausedByScreenLock());
        QCOMPARE(pauseSpy.count(), 1);
        QCOMPARE(engine->remainingSeconds(), engine->workSeconds()); // none of it ran

        // The unlock asks whether to resume, as after a lock that paused Work itself.
        watcher->onActiveChanged(false);
        QCOMPARE(unlockSpy.count(), 1);
        engine->resume();
        QCOMPARE(engine->state(), TimerEngine::State::Working);
        QVERIFY(!watcher->isPausedByScreenLock());
    }

    void aWorkThatStartsByItselfRunsWhenNotLockedOrNotAsked_data()
    {
        QTest::addColumn<bool>("autoPause");
        QTest::addColumn<bool>("unlockedFirst");
        QTest::newRow("setting off") << false << false;
        QTest::newRow("unlocked during the break") << true << true;
    }

    void aWorkThatStartsByItselfRunsWhenNotLockedOrNotAsked()
    {
        QFETCH(bool, autoPause);
        QFETCH(bool, unlockedFirst);
        settings->setAutoPauseOnScreenLock(autoPause);
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        engine->start();
        engine->skip();
        watcher->onActiveChanged(true);
        if (unlockedFirst) {
            watcher->onActiveChanged(false);
        }
        clock->advanceSeconds(engine->remainingSeconds() + 1);
        engine->refresh();
        QCOMPARE(engine->state(), TimerEngine::State::Working);
        QVERIFY(!watcher->isPausedByScreenLock());
    }

    void wakingUpAfterTheUserResumedSaysNothing()
    {
        engine->start();
        watcher->onPrepareForSleep(true);
        watcher->clearLockPauseNotice();
        QVERIFY(!watcher->isPausedBySleep());
        QSignalSpy wokeSpy(watcher.get(), &ScreenLockWatcher::wokeUpAfterPause);
        watcher->onPrepareForSleep(false);
        QCOMPARE(wokeSpy.count(), 0);
    }

    void onlyTheScreenSaverItselfCanReportALock()
    {
#ifndef Q_OS_LINUX
        QSKIP("The screen saver is watched over D-Bus on Linux only");
#else
        PrivateDBus bus;
        if (!bus.start()) {
            QSKIP("dbus-daemon is not available");
        }
        QDBusConnection session = QDBusConnection::connectToBus(bus.address(), QStringLiteral("watcher-session"));
        QDBusConnection impostor = QDBusConnection::connectToBus(bus.address(), QStringLiteral("impostor"));
        QDBusConnection screenSaver = QDBusConnection::connectToBus(bus.address(), QStringLiteral("screen-saver"));
        QVERIFY(screenSaver.registerService(QStringLiteral("org.freedesktop.ScreenSaver")));

        ScreenLockWatcher lockWatcher(session);
        lockWatcher.attach(engine.get(), settings.get());
        engine->start();
        const auto lockSignal = [] {
            QDBusMessage signal = QDBusMessage::createSignal(QStringLiteral("/org/freedesktop/ScreenSaver"),
                                                             QStringLiteral("org.freedesktop.ScreenSaver"),
                                                             QStringLiteral("ActiveChanged"));
            signal << true;
            return signal;
        };

        QVERIFY(impostor.send(lockSignal()));
        flush(impostor, session);
        QCOMPARE(engine->state(), TimerEngine::State::Working);

        QVERIFY(screenSaver.send(lockSignal()));
        QTRY_COMPARE(engine->state(), TimerEngine::State::Paused);

        for (const auto &name : {QStringLiteral("watcher-session"), QStringLiteral("impostor"), QStringLiteral("screen-saver")}) {
            QDBusConnection::disconnectFromBus(name);
        }
#endif
    }

    void gnomeScreenSaverLockPausesTheTimer()
    {
#ifndef Q_OS_LINUX
        QSKIP("The screen saver is watched over D-Bus on Linux only");
#else
        PrivateDBus bus;
        if (!bus.start()) {
            QSKIP("dbus-daemon is not available");
        }
        QDBusConnection session = QDBusConnection::connectToBus(bus.address(), QStringLiteral("gnome-session"));
        QDBusConnection gnomeShell = QDBusConnection::connectToBus(bus.address(), QStringLiteral("gnome-shell"));
        // GNOME announces locks on its own name only, never on org.freedesktop.ScreenSaver.
        QVERIFY(gnomeShell.registerService(QStringLiteral("org.gnome.ScreenSaver")));

        ScreenLockWatcher lockWatcher(session);
        lockWatcher.attach(engine.get(), settings.get());
        QSignalSpy unlockSpy(&lockWatcher, &ScreenLockWatcher::screenUnlockedAfterPause);
        engine->start();
        const auto activeChanged = [](bool active) {
            QDBusMessage signal = QDBusMessage::createSignal(QStringLiteral("/org/gnome/ScreenSaver"),
                                                             QStringLiteral("org.gnome.ScreenSaver"),
                                                             QStringLiteral("ActiveChanged"));
            signal << active;
            return signal;
        };

        QVERIFY(gnomeShell.send(activeChanged(true)));
        QTRY_COMPARE(engine->state(), TimerEngine::State::Paused);
        QVERIFY(lockWatcher.isPausedByScreenLock());

        QVERIFY(gnomeShell.send(activeChanged(false)));
        QTRY_COMPARE(unlockSpy.count(), 1);

        QDBusConnection::disconnectFromBus(QStringLiteral("gnome-shell"));
        QDBusConnection::disconnectFromBus(QStringLiteral("gnome-session"));
#endif
    }

    void aLockerThatRestartedWhileLockedCanLockAgain()
    {
#ifndef Q_OS_LINUX
        QSKIP("The screen saver is watched over D-Bus on Linux only");
#else
        PrivateDBus bus;
        if (!bus.start()) {
            QSKIP("dbus-daemon is not available");
        }
        QDBusConnection session = QDBusConnection::connectToBus(bus.address(), QStringLiteral("restart-session"));
        const auto locked = [] {
            QDBusMessage signal = QDBusMessage::createSignal(QStringLiteral("/org/freedesktop/ScreenSaver"),
                                                             QStringLiteral("org.freedesktop.ScreenSaver"),
                                                             QStringLiteral("ActiveChanged"));
            signal << true;
            return signal;
        };

        ScreenLockWatcher lockWatcher(session);
        lockWatcher.attach(engine.get(), settings.get());
        engine->start();
        {
            QDBusConnection locker = QDBusConnection::connectToBus(bus.address(), QStringLiteral("locker"));
            QVERIFY(locker.registerService(QStringLiteral("org.freedesktop.ScreenSaver")));
            QVERIFY(locker.send(locked()));
            QTRY_COMPARE(engine->state(), TimerEngine::State::Paused);
        }
        engine->resume();
        QCOMPARE(engine->state(), TimerEngine::State::Working);

        // The locker crashes while the screen is locked: its connection is gone (no copy of it
        // is left here) and ActiveChanged(false) never comes. It starts again, and the next lock
        // must pause the work phase as the first one did.
        QDBusConnection::disconnectFromBus(QStringLiteral("locker"));
        // The bus may handle the new owner's request before the old connection's end.
        QVERIFY(nameReleased(session, QStringLiteral("org.freedesktop.ScreenSaver")));
        QDBusConnection restarted = QDBusConnection::connectToBus(bus.address(), QStringLiteral("locker-restarted"));
        QVERIFY(restarted.registerService(QStringLiteral("org.freedesktop.ScreenSaver")));
        QVERIFY(restarted.send(locked()));
        QTRY_COMPARE(engine->state(), TimerEngine::State::Paused);
        QVERIFY(lockWatcher.isPausedByScreenLock());

        QDBusConnection::disconnectFromBus(QStringLiteral("locker-restarted"));
        QDBusConnection::disconnectFromBus(QStringLiteral("restart-session"));
#endif
    }

    void aLockerGoneWhileLockedCountsAsAnUnlock()
    {
#ifndef Q_OS_LINUX
        QSKIP("The screen saver is watched over D-Bus on Linux only");
#else
        PrivateDBus bus;
        if (!bus.start()) {
            QSKIP("dbus-daemon is not available");
        }
        QDBusConnection session = QDBusConnection::connectToBus(bus.address(), QStringLiteral("gone-session"));
        const auto activeChanged = [](bool active) {
            QDBusMessage signal = QDBusMessage::createSignal(QStringLiteral("/org/freedesktop/ScreenSaver"),
                                                             QStringLiteral("org.freedesktop.ScreenSaver"),
                                                             QStringLiteral("ActiveChanged"));
            signal << active;
            return signal;
        };

        ScreenLockWatcher lockWatcher(session);
        lockWatcher.attach(engine.get(), settings.get());
        QSignalSpy pauseSpy(&lockWatcher, &ScreenLockWatcher::pausedByScreenLockChanged);
        QSignalSpy unlockSpy(&lockWatcher, &ScreenLockWatcher::screenUnlockedAfterPause);
        engine->start();
        {
            QDBusConnection locker = QDBusConnection::connectToBus(bus.address(), QStringLiteral("gone-locker"));
            QVERIFY(locker.registerService(QStringLiteral("org.freedesktop.ScreenSaver")));
            QVERIFY(locker.send(activeChanged(true)));
            QTRY_COMPARE(engine->state(), TimerEngine::State::Paused);
        }
        QCOMPARE(pauseSpy.count(), 1);

        // The locker crashes: the user is asked whether to resume, as after an unlock; the timer
        // stays paused until they answer.
        QDBusConnection::disconnectFromBus(QStringLiteral("gone-locker"));
        QTRY_COMPARE(unlockSpy.count(), 1);
        QCOMPARE(engine->state(), TimerEngine::State::Paused);
        QVERIFY(lockWatcher.isPausedByScreenLock());
        QCOMPARE(pauseSpy.count(), 1);

        // The locker that replaces it reports an unlock of its own: already told.
        QDBusConnection restarted = QDBusConnection::connectToBus(bus.address(), QStringLiteral("gone-restarted"));
        QVERIFY(restarted.registerService(QStringLiteral("org.freedesktop.ScreenSaver")));
        QVERIFY(restarted.send(activeChanged(false)));
        flush(restarted, session);
        QCOMPARE(unlockSpy.count(), 1);

        // It locks and unlocks before the user answered: the lock pauses nothing more, and the
        // unlock, the user's real return, is reported again (Main.qml shows one prompt for both).
        QVERIFY(restarted.send(activeChanged(true)));
        flush(restarted, session);
        QCOMPARE(pauseSpy.count(), 1);
        QCOMPARE(engine->state(), TimerEngine::State::Paused);
        QVERIFY(restarted.send(activeChanged(false)));
        QTRY_COMPARE(unlockSpy.count(), 2);
        QCOMPARE(engine->state(), TimerEngine::State::Paused);
        QCOMPARE(pauseSpy.count(), 1);

        QDBusConnection::disconnectFromBus(QStringLiteral("gone-restarted"));
        QDBusConnection::disconnectFromBus(QStringLiteral("gone-session"));
#endif
    }

    void aLockerThatCameBackLockedIsAsked()
    {
#ifndef Q_OS_LINUX
        QSKIP("The screen saver is watched over D-Bus on Linux only");
#else
        PrivateDBus bus;
        if (!bus.start()) {
            QSKIP("dbus-daemon is not available");
        }
        const QString name = QStringLiteral("org.freedesktop.ScreenSaver");
        const QString path = QStringLiteral("/org/freedesktop/ScreenSaver");
        QDBusConnection session = QDBusConnection::connectToBus(bus.address(), QStringLiteral("asked-session"));
        const auto activeChanged = [&](bool active) {
            QDBusMessage signal = QDBusMessage::createSignal(path, name, QStringLiteral("ActiveChanged"));
            signal << active;
            return signal;
        };

        ScreenLockWatcher lockWatcher(session);
        lockWatcher.attach(engine.get(), settings.get());
        QSignalSpy unlockSpy(&lockWatcher, &ScreenLockWatcher::screenUnlockedAfterPause);
        engine->start();
        {
            QDBusConnection locker = QDBusConnection::connectToBus(bus.address(), QStringLiteral("asked-locker"));
            QVERIFY(locker.registerService(name));
            QVERIFY(locker.send(activeChanged(true)));
            QTRY_COMPARE(engine->state(), TimerEngine::State::Paused);
        }
        engine->resume();
        QDBusConnection::disconnectFromBus(QStringLiteral("asked-locker"));
        QVERIFY(nameReleased(session, name));
        QCOMPARE(engine->state(), TimerEngine::State::Working);

        // It comes back with the screen locked and says nothing: asked, it answers it is locked.
        FakeLocker stillLocked(true);
        QDBusConnection restarted = QDBusConnection::connectToBus(bus.address(), QStringLiteral("asked-restarted"));
        QVERIFY(restarted.registerObject(path, &stillLocked, QDBusConnection::ExportAllSlots));
        QVERIFY(restarted.registerService(name));
        QTRY_COMPARE(engine->state(), TimerEngine::State::Paused);
        QVERIFY(lockWatcher.isPausedByScreenLock());
        QCOMPARE(stillLocked.calls, 1);

        QVERIFY(restarted.send(activeChanged(false)));
        QTRY_COMPARE(unlockSpy.count(), 1);

        QDBusConnection::disconnectFromBus(QStringLiteral("asked-restarted"));
        QDBusConnection::disconnectFromBus(QStringLiteral("asked-session"));
#endif
    }

    void aLockerAnsweringUnlockedChangesNothing()
    {
#ifndef Q_OS_LINUX
        QSKIP("The screen saver is watched over D-Bus on Linux only");
#else
        PrivateDBus bus;
        if (!bus.start()) {
            QSKIP("dbus-daemon is not available");
        }
        QDBusConnection session = QDBusConnection::connectToBus(bus.address(), QStringLiteral("unlocked-session"));
        ScreenLockWatcher lockWatcher(session);
        lockWatcher.attach(engine.get(), settings.get());
        engine->start();

        FakeLocker unlocked(false);
        QDBusConnection locker = QDBusConnection::connectToBus(bus.address(), QStringLiteral("unlocked-locker"));
        QVERIFY(locker.registerObject(QStringLiteral("/org/freedesktop/ScreenSaver"), &unlocked, QDBusConnection::ExportAllSlots));
        QVERIFY(locker.registerService(QStringLiteral("org.freedesktop.ScreenSaver")));
        QTRY_COMPARE(unlocked.calls, 1);
        // The answer left before the locker's call to the bus: the watcher has it after flush().
        flush(locker, session);
        QCOMPARE(engine->state(), TimerEngine::State::Working);
        QVERIFY(!lockWatcher.isPausedByScreenLock());

        QDBusConnection::disconnectFromBus(QStringLiteral("unlocked-locker"));
        QDBusConnection::disconnectFromBus(QStringLiteral("unlocked-session"));
#endif
    }

    void anotherScreenSaverNameLeavingKeepsTheLock()
    {
#ifndef Q_OS_LINUX
        QSKIP("The screen saver is watched over D-Bus on Linux only");
#else
        PrivateDBus bus;
        if (!bus.start()) {
            QSKIP("dbus-daemon is not available");
        }
        QDBusConnection session = QDBusConnection::connectToBus(bus.address(), QStringLiteral("proxy-session"));
        QDBusConnection gnomeShell = QDBusConnection::connectToBus(bus.address(), QStringLiteral("proxy-gnome-shell"));
        QVERIFY(gnomeShell.registerService(QStringLiteral("org.gnome.ScreenSaver")));
        const auto activeChanged = [](bool active) {
            QDBusMessage signal = QDBusMessage::createSignal(QStringLiteral("/org/gnome/ScreenSaver"),
                                                             QStringLiteral("org.gnome.ScreenSaver"),
                                                             QStringLiteral("ActiveChanged"));
            signal << active;
            return signal;
        };

        ScreenLockWatcher lockWatcher(session);
        lockWatcher.attach(engine.get(), settings.get());
        QSignalSpy unlockSpy(&lockWatcher, &ScreenLockWatcher::screenUnlockedAfterPause);
        engine->start();
        {
            // On GNOME, gsd-screensaver-proxy owns org.freedesktop.ScreenSaver for Inhibit only.
            QDBusConnection proxy = QDBusConnection::connectToBus(bus.address(), QStringLiteral("proxy"));
            QVERIFY(proxy.registerService(QStringLiteral("org.freedesktop.ScreenSaver")));
            QVERIFY(gnomeShell.send(activeChanged(true)));
            QTRY_COMPARE(engine->state(), TimerEngine::State::Paused);
        }

        // The proxy restarting says nothing about gnome-shell's lock.
        QDBusConnection::disconnectFromBus(QStringLiteral("proxy"));
        QVERIFY(nameReleased(session, QStringLiteral("org.freedesktop.ScreenSaver")));
        QCOMPARE(unlockSpy.count(), 0);

        QVERIFY(gnomeShell.send(activeChanged(false)));
        QTRY_COMPARE(unlockSpy.count(), 1);

        QDBusConnection::disconnectFromBus(QStringLiteral("proxy-gnome-shell"));
        QDBusConnection::disconnectFromBus(QStringLiteral("proxy-session"));
#endif
    }

    void availableFollowsTheBackend()
    {
        const ScreenLockWatcher offline(ScreenLockWatcher::Source::Nothing);
        QVERIFY(!offline.available());

#ifdef Q_OS_MACOS
        // The default constructor listens through MacSessionEvents, with or without a bus.
        const ScreenLockWatcher mac;
        QVERIFY(mac.available());
#else
        PrivateDBus bus;
        if (!bus.start()) {
            QSKIP("dbus-daemon is not available");
        }
        QDBusConnection session = QDBusConnection::connectToBus(bus.address(), QStringLiteral("available-session"));
        {
            const ScreenLockWatcher online(session);
            QVERIFY(online.available());
        }
        QDBusConnection::disconnectFromBus(QStringLiteral("available-session"));
#endif
    }

    void logindSignalOnTheSystemBusPausesTheTimer()
    {
#ifndef Q_OS_LINUX
        QSKIP("logind exists on Linux only");
#else
        PrivateDBus bus;
        if (!bus.start()) {
            QSKIP("dbus-daemon is not available");
        }
        QDBusConnection systemBus = QDBusConnection::connectToBus(bus.address(), QStringLiteral("fake-system"));
        QDBusConnection logind = QDBusConnection::connectToBus(bus.address(), QStringLiteral("fake-logind"));
        QVERIFY(logind.registerService(QStringLiteral("org.freedesktop.login1")));

        ScreenLockWatcher sleepWatcher(QDBusConnection(QStringLiteral("tst_disconnected_bus")), systemBus);
        sleepWatcher.attach(engine.get(), settings.get());
        engine->start();

        QDBusMessage signal = QDBusMessage::createSignal(QStringLiteral("/org/freedesktop/login1"),
                                                         QStringLiteral("org.freedesktop.login1.Manager"),
                                                         QStringLiteral("PrepareForSleep"));
        signal << true;
        QVERIFY(logind.send(signal));
        QTRY_COMPARE(engine->state(), TimerEngine::State::Paused);
        QVERIFY(sleepWatcher.isPausedBySleep());

        QDBusConnection::disconnectFromBus(QStringLiteral("fake-logind"));
        QDBusConnection::disconnectFromBus(QStringLiteral("fake-system"));
#endif
    }

    void logindLockedHintPausesTheTimer()
    {
#ifndef Q_OS_LINUX
        QSKIP("logind exists on Linux only");
#else
        // Xfce, MATE, or sway with a locker that sets the hint: no screen saver name says anything.
        PrivateDBus bus;
        if (!bus.start()) {
            QSKIP("dbus-daemon is not available");
        }
        const SessionIdEnv sessionId("7");
        const Connections connections({QStringLiteral("hint-system"), QStringLiteral("hint-logind"), QStringLiteral("hint-impostor")});
        QDBusConnection systemBus = QDBusConnection::connectToBus(bus.address(), QStringLiteral("hint-system"));
        QDBusConnection logind = QDBusConnection::connectToBus(bus.address(), QStringLiteral("hint-logind"));
        QDBusConnection impostor = QDBusConnection::connectToBus(bus.address(), QStringLiteral("hint-impostor"));
        FakeLogind fake;
        QVERIFY(fake.registerOn(logind));

        ScreenLockWatcher lockWatcher(QDBusConnection(QStringLiteral("tst_disconnected_bus")), systemBus);
        lockWatcher.attach(engine.get(), settings.get());
        QSignalSpy pauseSpy(&lockWatcher, &ScreenLockWatcher::pausedByScreenLockChanged);
        QSignalSpy unlockSpy(&lockWatcher, &ScreenLockWatcher::screenUnlockedAfterPause);
        engine->start();
        QTRY_COMPARE(lockWatcher.logindSession(), kSessionPath);
        flush(logind, systemBus);

        // Only logind itself can say the session is locked.
        QVERIFY(impostor.send(lockedHintChanged(true)));
        flush(impostor, systemBus);
        QCOMPARE(engine->state(), TimerEngine::State::Working);

        QVERIFY(logind.send(lockedHintChanged(true)));
        QTRY_COMPARE(engine->state(), TimerEngine::State::Paused);
        QVERIFY(lockWatcher.isPausedByScreenLock());
        QCOMPARE(pauseSpy.count(), 1);

        QVERIFY(logind.send(lockedHintChanged(false)));
        QTRY_COMPARE(unlockSpy.count(), 1);
        QCOMPARE(engine->state(), TimerEngine::State::Paused);

#endif
    }

    void logindSessionIsFoundWithoutXdgSessionId_data()
    {
        QTest::addColumn<QByteArray>("sessionId");
        QTest::newRow("unset") << QByteArray();
        QTest::newRow("unknown to logind") << QByteArray("c42");
    }

    void logindSessionIsFoundWithoutXdgSessionId()
    {
#ifndef Q_OS_LINUX
        QSKIP("logind exists on Linux only");
#else
        QFETCH(QByteArray, sessionId);
        PrivateDBus bus;
        if (!bus.start()) {
            QSKIP("dbus-daemon is not available");
        }
        const SessionIdEnv env(sessionId);
        const Connections connections({QStringLiteral("auto-system"), QStringLiteral("auto-logind")});
        QDBusConnection systemBus = QDBusConnection::connectToBus(bus.address(), QStringLiteral("auto-system"));
        QDBusConnection logind = QDBusConnection::connectToBus(bus.address(), QStringLiteral("auto-logind"));
        FakeLogind fake;
        QVERIFY(fake.registerOn(logind));

        // session/auto: the caller's session, or the user's graphical one (a Flatpak app's scope
        // belongs to no session).
        ScreenLockWatcher lockWatcher(QDBusConnection(QStringLiteral("tst_disconnected_bus")), systemBus);
        lockWatcher.attach(engine.get(), settings.get());
        engine->start();
        QTRY_COMPARE(lockWatcher.logindSession(), kSessionPath);
        flush(logind, systemBus);

        QVERIFY(logind.send(lockedHintChanged(true)));
        QTRY_COMPARE(engine->state(), TimerEngine::State::Paused);

#endif
    }

    void aLockSeenByTheLockerAndLogindCountsOnce()
    {
#ifndef Q_OS_LINUX
        QSKIP("logind exists on Linux only");
#else
        // Plasma and GNOME report a lock on their screen saver name and in LockedHint.
        PrivateDBus bus;
        PrivateDBus system;
        if (!bus.start() || !system.start()) {
            QSKIP("dbus-daemon is not available");
        }
        const SessionIdEnv sessionId("7");
        const Connections connections({QStringLiteral("both-session"), QStringLiteral("both-locker"), QStringLiteral("both-system"), QStringLiteral("both-logind")});
        QDBusConnection session = QDBusConnection::connectToBus(bus.address(), QStringLiteral("both-session"));
        QDBusConnection locker = QDBusConnection::connectToBus(bus.address(), QStringLiteral("both-locker"));
        QVERIFY(locker.registerService(QStringLiteral("org.freedesktop.ScreenSaver")));
        QDBusConnection systemBus = QDBusConnection::connectToBus(system.address(), QStringLiteral("both-system"));
        QDBusConnection logind = QDBusConnection::connectToBus(system.address(), QStringLiteral("both-logind"));
        FakeLogind fake;
        QVERIFY(fake.registerOn(logind));
        const auto activeChanged = [](bool active) {
            QDBusMessage signal = QDBusMessage::createSignal(QStringLiteral("/ScreenSaver"),
                                                             QStringLiteral("org.freedesktop.ScreenSaver"),
                                                             QStringLiteral("ActiveChanged"));
            signal << active;
            return signal;
        };

        ScreenLockWatcher lockWatcher(session, systemBus);
        lockWatcher.attach(engine.get(), settings.get());
        QSignalSpy pauseSpy(&lockWatcher, &ScreenLockWatcher::pausedByScreenLockChanged);
        QSignalSpy unlockSpy(&lockWatcher, &ScreenLockWatcher::screenUnlockedAfterPause);
        engine->start();
        QTRY_COMPARE(lockWatcher.logindSession(), kSessionPath);
        flush(logind, systemBus);

        QVERIFY(locker.send(activeChanged(true)));
        QTRY_COMPARE(engine->state(), TimerEngine::State::Paused);
        QVERIFY(logind.send(lockedHintChanged(true)));
        flush(logind, systemBus);
        QCOMPARE(pauseSpy.count(), 1);

        // Unlocked: logind first this time, then the locker.
        QVERIFY(logind.send(lockedHintChanged(false)));
        QTRY_COMPARE(unlockSpy.count(), 1);
        QVERIFY(locker.send(activeChanged(false)));
        flush(locker, session);
        QCOMPARE(unlockSpy.count(), 1);
        QCOMPARE(pauseSpy.count(), 1);

#endif
    }

    void logindLockRequestPausesTheTimer()
    {
#ifndef Q_OS_LINUX
        QSKIP("logind exists on Linux only");
#else
        // `loginctl lock-session` (or logind's IdleAction=lock) asks the locker to lock: xss-lock and
        // swayidle start i3lock or swaylock, which set no LockedHint and announce nothing.
        PrivateDBus bus;
        if (!bus.start()) {
            QSKIP("dbus-daemon is not available");
        }
        const SessionIdEnv sessionId("7");
        const Connections connections({QStringLiteral("request-system"), QStringLiteral("request-logind")});
        QDBusConnection systemBus = QDBusConnection::connectToBus(bus.address(), QStringLiteral("request-system"));
        QDBusConnection logind = QDBusConnection::connectToBus(bus.address(), QStringLiteral("request-logind"));
        FakeLogind fake;
        QVERIFY(fake.registerOn(logind));
        const auto request = [](const QString &member) {
            return QDBusMessage::createSignal(kSessionPath, kSessionInterface, member);
        };

        ScreenLockWatcher lockWatcher(QDBusConnection(QStringLiteral("tst_disconnected_bus")), systemBus);
        lockWatcher.attach(engine.get(), settings.get());
        QSignalSpy unlockSpy(&lockWatcher, &ScreenLockWatcher::screenUnlockedAfterPause);
        engine->start();
        QTRY_COMPARE(lockWatcher.logindSession(), kSessionPath);
        flush(logind, systemBus);

        QVERIFY(logind.send(request(QStringLiteral("Lock"))));
        QTRY_COMPARE(engine->state(), TimerEngine::State::Paused);
        QVERIFY(lockWatcher.isPausedByScreenLock());

        // swaylock unlocks without a word: the user resuming the timer is the proof they are back,
        // and the next lock request pauses again.
        engine->resume();
        QCOMPARE(engine->state(), TimerEngine::State::Working);
        QVERIFY(logind.send(request(QStringLiteral("Lock"))));
        QTRY_COMPARE(engine->state(), TimerEngine::State::Paused);

        // `loginctl unlock-session` is an unlock as any other.
        QVERIFY(logind.send(request(QStringLiteral("Unlock"))));
        QTRY_COMPARE(unlockSpy.count(), 1);

#endif
    }

    void aLockThatEndsADueBreakStillReportsTheUnlock()
    {
#ifndef Q_OS_LINUX
        QSKIP("logind exists on Linux only");
#else
        // Back from idle or sleep, the break was due but no tick has ended it yet when logind says
        // the session is locked: the break ends, the work that starts by itself is paused, and the
        // unlock asks whether to resume. The phase change is no sign that the user is back.
        // logind's slot is called directly: no turn of the event loop between the deadline and the
        // lock, so the engine's own tick cannot end the break first.
        const auto logindHint = [this](bool locked) {
            return QMetaObject::invokeMethod(watcher.get(),
                                             "onLogindPropertiesChanged",
                                             Qt::DirectConnection,
                                             Q_ARG(QString, kSessionInterface),
                                             Q_ARG(QVariantMap, QVariantMap({{QStringLiteral("LockedHint"), locked}})),
                                             Q_ARG(QStringList, QStringList()));
        };
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        engine->start();
        engine->skip();
        QCOMPARE(engine->state(), TimerEngine::State::ShortBreak);
        clock->advanceSeconds(engine->remainingSeconds() + 1);
        QSignalSpy unlockSpy(watcher.get(), &ScreenLockWatcher::screenUnlockedAfterPause);

        QVERIFY(logindHint(true));
        QCOMPARE(engine->state(), TimerEngine::State::Paused);
        QCOMPARE(engine->phase(), TimerEngine::Phase::Work);
        QVERIFY(watcher->isPausedByScreenLock());

        QVERIFY(logindHint(false));
        QCOMPARE(unlockSpy.count(), 1);
        QCOMPARE(engine->state(), TimerEngine::State::Paused);
#endif
    }

    void aLogindLockKeptThroughABreakPausesTheWorkThatFollows()
    {
#ifndef Q_OS_LINUX
        QSKIP("logind exists on Linux only");
#else
        // Locked during the break through logind's hint: the work that starts by itself when the
        // break ends is no sign of the user, so the lock holds and that work is paused.
        const auto logindHint = [this](bool locked) {
            return QMetaObject::invokeMethod(watcher.get(),
                                             "onLogindPropertiesChanged",
                                             Qt::DirectConnection,
                                             Q_ARG(QString, kSessionInterface),
                                             Q_ARG(QVariantMap, QVariantMap({{QStringLiteral("LockedHint"), locked}})),
                                             Q_ARG(QStringList, QStringList()));
        };
        engine->setAutoStartMode(TimerEngine::AutoStartMode::All);
        engine->start();
        engine->skip();
        QSignalSpy unlockSpy(watcher.get(), &ScreenLockWatcher::screenUnlockedAfterPause);
        QVERIFY(logindHint(true));
        QCOMPARE(engine->state(), TimerEngine::State::ShortBreak);

        clock->advanceSeconds(engine->remainingSeconds() + 1);
        engine->refresh();
        QCOMPARE(engine->phase(), TimerEngine::Phase::Work);
        QCOMPARE(engine->state(), TimerEngine::State::Paused);
        QVERIFY(watcher->isPausedByScreenLock());

        QVERIFY(logindHint(false));
        QCOMPARE(unlockSpy.count(), 1);
#endif
    }

    void aLockRequestNeverUnlockedEndsWhenTheUserStartsOrSkips()
    {
#ifndef Q_OS_LINUX
        QSKIP("logind exists on Linux only");
#else
        // swaylock unlocks without a word. A lock asked while nothing ran to pause is over when the
        // user starts the timer, or skips the break it came during: the next request pauses Work.
        const auto lockRequest = [this]() {
            return QMetaObject::invokeMethod(watcher.get(), "onLogindLockRequested", Qt::DirectConnection);
        };
        QVERIFY(lockRequest());
        QCOMPARE(engine->state(), TimerEngine::State::Idle);
        engine->start();
        QVERIFY(lockRequest());
        QCOMPARE(engine->state(), TimerEngine::State::Paused);
        QVERIFY(watcher->isPausedByScreenLock());

        engine->skip(); // the user is back and moves on to the break
        QCOMPARE(engine->state(), TimerEngine::State::ShortBreak);
        QVERIFY(lockRequest());
        QCOMPARE(engine->state(), TimerEngine::State::ShortBreak);
        engine->skip();
        QCOMPARE(engine->state(), TimerEngine::State::Working);
        QVERIFY(lockRequest());
        QCOMPARE(engine->state(), TimerEngine::State::Paused);
#endif
    }

    void aReopenedPhaseThatIsStoppedLetsTheUserEndALockRequest()
    {
#ifndef Q_OS_LINUX
        QSKIP("logind exists on Linux only");
#else
        // "+5 min" on a work phase that has just ended, then Stop: the engine reports a completed
        // session but announces nothing (no phaseFinished). Starting the timer later is still the
        // user's doing: it ends a lock request never unlocked, and the next request pauses Work.
        const auto lockRequest = [this]() {
            return QMetaObject::invokeMethod(watcher.get(), "onLogindLockRequested", Qt::DirectConnection);
        };
        engine->start();
        clock->advanceSeconds(engine->remainingSeconds() + 1);
        engine->refresh();
        QCOMPARE(engine->state(), TimerEngine::State::Idle); // nothing starts by itself

        QVERIFY(engine->extend()); // the work phase that just ended goes on
        QCOMPARE(engine->state(), TimerEngine::State::Working);
        clock->advanceSeconds(60);
        engine->stop();
        QCOMPARE(engine->state(), TimerEngine::State::Idle);
        QTest::qWait(20); // the user comes back later, not in the same event-loop turn

        QVERIFY(lockRequest()); // nothing runs to pause; swaylock will never report the unlock
        QCOMPARE(engine->state(), TimerEngine::State::Idle);
        engine->start();
        QCOMPARE(engine->state(), TimerEngine::State::Working);
        QVERIFY(lockRequest());
        QCOMPARE(engine->state(), TimerEngine::State::Paused);
        QVERIFY(watcher->isPausedByScreenLock());
#endif
    }

    void otherDesktopScreenSaversPauseTheTimer_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("path");
        QTest::newRow("Xfce") << QStringLiteral("org.xfce.ScreenSaver") << QStringLiteral("/org/xfce/ScreenSaver");
        QTest::newRow("Cinnamon") << QStringLiteral("org.cinnamon.ScreenSaver") << QStringLiteral("/org/cinnamon/ScreenSaver");
        QTest::newRow("MATE") << QStringLiteral("org.mate.ScreenSaver") << QStringLiteral("/org/mate/ScreenSaver");
    }

    void otherDesktopScreenSaversPauseTheTimer()
    {
#ifndef Q_OS_LINUX
        QSKIP("The screen saver is watched over D-Bus on Linux only");
#else
        QFETCH(QString, name);
        QFETCH(QString, path);
        PrivateDBus bus;
        if (!bus.start()) {
            QSKIP("dbus-daemon is not available");
        }
        const Connections connections({QStringLiteral("desktop-session"), QStringLiteral("desktop-impostor"), QStringLiteral("desktop-screen-saver")});
        QDBusConnection session = QDBusConnection::connectToBus(bus.address(), QStringLiteral("desktop-session"));
        QDBusConnection impostor = QDBusConnection::connectToBus(bus.address(), QStringLiteral("desktop-impostor"));
        QDBusConnection screenSaver = QDBusConnection::connectToBus(bus.address(), QStringLiteral("desktop-screen-saver"));
        QVERIFY(screenSaver.registerService(name));
        const auto activeChanged = [&](bool active) {
            QDBusMessage signal = QDBusMessage::createSignal(path, name, QStringLiteral("ActiveChanged"));
            signal << active;
            return signal;
        };

        ScreenLockWatcher lockWatcher(session);
        lockWatcher.attach(engine.get(), settings.get());
        QSignalSpy unlockSpy(&lockWatcher, &ScreenLockWatcher::screenUnlockedAfterPause);
        engine->start();

        QVERIFY(impostor.send(activeChanged(true)));
        flush(impostor, session);
        QCOMPARE(engine->state(), TimerEngine::State::Working);

        QVERIFY(screenSaver.send(activeChanged(true)));
        QTRY_COMPARE(engine->state(), TimerEngine::State::Paused);
        QVERIFY(lockWatcher.isPausedByScreenLock());

        QVERIFY(screenSaver.send(activeChanged(false)));
        QTRY_COMPARE(unlockSpy.count(), 1);

#endif
    }
};

QTEST_GUILESS_MAIN(ScreenLockWatcherTest)
#include "tst_screenlockwatcher.moc"
