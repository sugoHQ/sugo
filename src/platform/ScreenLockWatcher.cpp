// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScreenLockWatcher.h"

#include "AppSettings.h"
#include "TimerEngine.h"

#ifdef Q_OS_MACOS
#include "MacSessionEvents.h"
#else
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#endif

#include <QLoggingCategory>
#include <QTimer>

namespace
{
Q_LOGGING_CATEGORY(lcScreenLock, "sugo.platform.screenlock")
#ifndef Q_OS_MACOS
const QString kInterface = QStringLiteral("org.freedesktop.ScreenSaver");
const QString kSignal = QStringLiteral("ActiveChanged");
const QString kGnomeInterface = QStringLiteral("org.gnome.ScreenSaver");

const QString kLogind = QStringLiteral("org.freedesktop.login1");
const QString kLogindManagerPath = QStringLiteral("/org/freedesktop/login1");
const QString kLogindManager = QStringLiteral("org.freedesktop.login1.Manager");
const QString kLogindSession = QStringLiteral("org.freedesktop.login1.Session");
const QString kProperties = QStringLiteral("org.freedesktop.DBus.Properties");
const QString kLockedHint = QStringLiteral("LockedHint");
// The source of a lock asked for with logind's Lock signal (no D-Bus name: logind did not lock).
const QString kLockRequest = QStringLiteral("org.freedesktop.login1.Session.Lock");

/// A screen saver name, which is also the interface of its ActiveChanged, and the object paths
/// it is sent on (GetActive is asked on the first).
struct Locker {
    QString name;
    QStringList paths;
};

const QList<Locker> &lockers()
{
    // Plasma emits ActiveChanged on both of its paths; setScreenLocked() drops the repeat. GNOME
    // owns org.freedesktop.ScreenSaver only to forward Inhibit and announces locks on its own name.
    static const QList<Locker> list{
        {kInterface, {QStringLiteral("/org/freedesktop/ScreenSaver"), QStringLiteral("/ScreenSaver")}},
        {kGnomeInterface, {QStringLiteral("/org/gnome/ScreenSaver")}},
        {QStringLiteral("org.xfce.ScreenSaver"), {QStringLiteral("/org/xfce/ScreenSaver")}},
        {QStringLiteral("org.cinnamon.ScreenSaver"), {QStringLiteral("/org/cinnamon/ScreenSaver")}},
        {QStringLiteral("org.mate.ScreenSaver"), {QStringLiteral("/org/mate/ScreenSaver")}},
    };
    return list;
}
#endif
} // namespace

ScreenLockWatcher::ScreenLockWatcher(QObject *parent)
    : ScreenLockWatcher(Source::System, parent)
{
}

#ifdef Q_OS_MACOS
ScreenLockWatcher::ScreenLockWatcher(Source source, QObject *parent)
    : QObject(parent)
{
    if (source == Source::System) {
        // Both callbacks run on the main thread, from the run loop Qt's event loop spins.
        m_macEvents = std::make_unique<MacSessionEvents>([this](bool locked) { onActiveChanged(locked); },
                                                         [this](bool sleeping) { onPrepareForSleep(sleeping); });
    }
}
#else
ScreenLockWatcher::ScreenLockWatcher(Source source, QObject *parent)
    : ScreenLockWatcher(source == Source::System ? QDBusConnection::sessionBus() : QDBusConnection(QStringLiteral("sugo-no-session-bus")),
                        source == Source::System ? QDBusConnection::systemBus() : QDBusConnection(QStringLiteral("sugo-no-system-bus")),
                        parent)
{
}

ScreenLockWatcher::ScreenLockWatcher(const QDBusConnection &bus, QObject *parent)
    : ScreenLockWatcher(bus, QDBusConnection(QStringLiteral("sugo-no-system-bus")), parent)
{
}

ScreenLockWatcher::ScreenLockWatcher(const QDBusConnection &bus, const QDBusConnection &systemBus, QObject *parent)
    : QObject(parent)
    , m_bus(bus)
    , m_systemBus(systemBus)
{
    if (m_systemBus.isConnected()) {
        m_systemBus.connect(QStringLiteral("org.freedesktop.login1"),
                            QStringLiteral("/org/freedesktop/login1"),
                            QStringLiteral("org.freedesktop.login1.Manager"),
                            QStringLiteral("PrepareForSleep"),
                            this,
                            SLOT(onPrepareForSleep(bool)));
    }

    // Only the owner of a screen saver's name is listened to: another program on the bus cannot
    // fake a lock.
    if (m_bus.isConnected()) {
        for (const Locker &locker : lockers()) {
            for (const QString &path : locker.paths) {
                m_bus.connect(locker.name, path, locker.name, kSignal, this, SLOT(onLockerActiveChanged(bool, QDBusMessage)));
            }
        }

        // A locker that crashes or restarts while the screen is locked never sends its unlock.
        QStringList names;
        for (const Locker &locker : lockers()) {
            names.append(locker.name);
        }
        auto *owners = new QDBusServiceWatcher(this);
        owners->setWatchMode(QDBusServiceWatcher::WatchForOwnerChange);
        owners->setConnection(m_bus);
        owners->setWatchedServices(names);
        connect(owners,
                &QDBusServiceWatcher::serviceOwnerChanged,
                this,
                [this](const QString &name, const QString &oldOwner, const QString &newOwner) {
                    m_lockerOwners.insert(name, newOwner);
                    if (!oldOwner.isEmpty()) {
                        onLockerGone(name);
                    }
                    if (!newOwner.isEmpty()) {
                        askLockerState(name, newOwner);
                    }
                });
    }

    if (m_systemBus.isConnected()) {
        findLogindSession();
    }
}

void ScreenLockWatcher::onLockerActiveChanged(bool active, const QDBusMessage &message)
{
    // The match rule names the interface, which is the screen saver's name.
    setScreenLocked(active, message.interface());
}

void ScreenLockWatcher::findLogindSession()
{
    // The session this process was started in; logind may not know it (a stale or foreign id).
    const QString id = qEnvironmentVariable("XDG_SESSION_ID");
    if (!id.isEmpty()) {
        askLogindSessionPath(id, true);
    } else {
        askLogindAutoSessionId();
    }
}

void ScreenLockWatcher::askLogindSessionPath(const QString &id, bool fromEnvironment)
{
    QDBusMessage call = QDBusMessage::createMethodCall(kLogind, kLogindManagerPath, kLogindManager, QStringLiteral("GetSession"));
    call << id;
    auto *pending = new QDBusPendingCallWatcher(m_systemBus.asyncCall(call), this);
    connect(pending, &QDBusPendingCallWatcher::finished, this, [this, pending, id, fromEnvironment]() {
        pending->deleteLater();
        const QDBusPendingReply<QDBusObjectPath> reply = *pending;
        if (!reply.isError() && !reply.value().path().isEmpty()) {
            followLogindSession(reply.value().path());
        } else if (fromEnvironment) {
            askLogindAutoSessionId();
        } else {
            qCDebug(lcScreenLock) << "logind knows no session" << id << reply.error().message();
        }
    });
}

void ScreenLockWatcher::askLogindAutoSessionId()
{
    // Signals are sent on the session's own path, never on session/auto: its Id leads to that path.
    QDBusMessage call = QDBusMessage::createMethodCall(kLogind, QStringLiteral("/org/freedesktop/login1/session/auto"), kProperties, QStringLiteral("Get"));
    call << kLogindSession << QStringLiteral("Id");
    auto *pending = new QDBusPendingCallWatcher(m_systemBus.asyncCall(call), this);
    connect(pending, &QDBusPendingCallWatcher::finished, this, [this, pending]() {
        pending->deleteLater();
        const QDBusPendingReply<QDBusVariant> reply = *pending;
        const QString id = reply.isError() ? QString() : reply.value().variant().toString();
        if (id.isEmpty()) {
            // No logind (no systemd, or no session at all): the screen saver names still count.
            qCDebug(lcScreenLock) << "No logind session to follow:" << reply.error().message();
            return;
        }
        askLogindSessionPath(id, false);
    });
}

void ScreenLockWatcher::followLogindSession(const QString &path)
{
    // From logind only: the system bus lets no other program own its name.
    m_systemBus.connect(kLogind,
                        path,
                        kProperties,
                        QStringLiteral("PropertiesChanged"),
                        this,
                        SLOT(onLogindPropertiesChanged(QString, QVariantMap, QStringList)));
    m_systemBus.connect(kLogind, path, kLogindSession, QStringLiteral("Lock"), this, SLOT(onLogindLockRequested()));
    m_systemBus.connect(kLogind, path, kLogindSession, QStringLiteral("Unlock"), this, SLOT(onLogindUnlockRequested()));
    m_logindSession = path;
    qCDebug(lcScreenLock) << "Following the lock state of the logind session" << path;
    // The current LockedHint is not read: Sugo starting with the screen locked pauses nothing, and
    // a hint left set by a locker that crashed must not swallow the next real lock.
}

void ScreenLockWatcher::onLogindPropertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated)
{
    if (interface != kLogindSession) {
        return;
    }
    const auto hint = changed.constFind(kLockedHint);
    if (hint != changed.cend()) {
        setScreenLocked(hint->toBool(), kLogind);
    } else if (invalidated.contains(kLockedHint)) {
        askLockedHint();
    }
}

void ScreenLockWatcher::askLockedHint()
{
    QDBusMessage call = QDBusMessage::createMethodCall(kLogind, m_logindSession, kProperties, QStringLiteral("Get"));
    call << kLogindSession << kLockedHint;
    auto *pending = new QDBusPendingCallWatcher(m_systemBus.asyncCall(call), this);
    connect(pending, &QDBusPendingCallWatcher::finished, this, [this, pending]() {
        pending->deleteLater();
        const QDBusPendingReply<QDBusVariant> reply = *pending;
        if (!reply.isError()) {
            setScreenLocked(reply.value().variant().toBool(), kLogind);
        }
    });
}

void ScreenLockWatcher::onLogindLockRequested()
{
    setScreenLocked(true, kLockRequest);
}

void ScreenLockWatcher::onLogindUnlockRequested()
{
    setScreenLocked(false, kLockRequest);
}

void ScreenLockWatcher::onLockerGone(const QString &name)
{
    // Only the owner that reported the lock: on GNOME, gsd-screensaver-proxy owning
    // org.freedesktop.ScreenSaver can restart without touching gnome-shell's lock.
    if (!m_screenLocked || name != m_lockSource) {
        return;
    }
    // Taken as an unlock: the lock screen is most likely gone with its owner. If the timer was
    // paused by the lock, the user is told as on an unlock and decides whether to resume; nothing
    // is paused here. A locker that comes back still locked sends a new lock, which then counts,
    // and its unlock is reported too: the screen may have been locked again in between.
    qCDebug(lcScreenLock) << "The screen locker left the bus while locked:" << name;
    setScreenLocked(false, QString());
}

void ScreenLockWatcher::askLockerState(const QString &name, const QString &owner)
{
    // A locker that starts again with the screen still locked need not announce it. The call goes
    // to the owner's own connection, so no other program can answer for it.
    QString path;
    for (const Locker &locker : lockers()) {
        if (locker.name == name) {
            path = locker.paths.constFirst();
        }
    }
    const QDBusMessage call = QDBusMessage::createMethodCall(owner, path, name, QStringLiteral("GetActive"));
    auto *pending = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
    connect(pending, &QDBusPendingCallWatcher::finished, this, [this, pending, name, owner]() {
        pending->deleteLater();
        const QDBusPendingReply<bool> reply = *pending;
        // An error (gsd-screensaver-proxy only forwards Inhibit) or "unlocked" changes nothing: a
        // lock reported by the old owner was already dropped when it left. An answer from an owner
        // that has since left is stale.
        if (reply.isError() || !reply.value() || m_lockerOwners.value(name) != owner) {
            return;
        }
        qCDebug(lcScreenLock) << "The screen locker came back with the screen locked:" << name;
        setScreenLocked(true, name);
    });
}
#endif

ScreenLockWatcher::~ScreenLockWatcher() = default;

bool ScreenLockWatcher::available() const
{
#ifdef Q_OS_MACOS
    return m_macEvents != nullptr;
#else
    return m_bus.isConnected();
#endif
}

void ScreenLockWatcher::attach(TimerEngine *engine, AppSettings *settings)
{
    m_engine = engine;
    m_settings = settings;

    if (m_engine) {
#ifndef Q_OS_MACOS
        // From a completed session to phaseFinished, the engine moves on by itself (auto-start):
        // no state change in between is the user's doing.
        connect(m_engine, &TimerEngine::sessionEnded, this, [this](TimerEngine::Phase, const QDateTime &, const QDateTime &, qint64, qint64, bool completed) {
            if (completed) {
                m_phaseEnding = true;
                // A phase taken up again ("+5 min") and then stopped or skipped is a completed
                // session too, but no phaseFinished follows. That signal comes within the call
                // that ended the session or not at all, so on the next event-loop turn the wait
                // is over, or the user's next start would not end a lock logind told of.
                QTimer::singleShot(0, this, [this]() {
                    m_phaseEnding = false;
                });
            }
        });
        connect(m_engine, &TimerEngine::phaseFinished, this, [this]() {
            m_phaseEnding = false;
        });
#endif
        // A break that ends while the screen is locked (it was not paused) can start Work by
        // itself: that Work is paused as if the lock came now, and the unlock asks as usual.
        // The engine has already started it when phaseFinished goes out.
        connect(m_engine, &TimerEngine::phaseFinished, this, [this]() {
            if (m_screenLocked && m_settings && m_settings->autoPauseOnScreenLock()
                && m_engine->state() == TimerEngine::State::Working) {
                qCDebug(lcScreenLock) << "Pausing the work that started by itself: the screen is locked";
                pauseForScreenLock();
            }
        });
        connect(m_engine, &TimerEngine::stateChanged, this, [this]() {
            if (m_engine && m_engine->state() != TimerEngine::State::Paused) {
                if (m_pausedByScreenLock) {
                    m_pausedByScreenLock = false;
                    Q_EMIT pausedByScreenLockChanged();
                }
                setPausedBySleep(false);
#ifndef Q_OS_MACOS
                // The user set the timer running again, so they are back: a lock only logind
                // told of (a locker that may never report its unlock, or a hint left set) is over.
                // A phase that ended and started the next one by itself says nothing of the
                // user: not while the screen is locked, nor when the refresh() of a lock being
                // recorded ends a phase that was due (its unlock would never be reported).
                if (!m_phaseEnding && m_screenLocked && (m_lockSource == kLogind || m_lockSource == kLockRequest)) {
                    qCDebug(lcScreenLock) << "The timer runs again: the lock reported by logind is over";
                    m_screenLocked = false;
                    m_lockSource.clear();
                }
#endif
            }
        });
    }
}

void ScreenLockWatcher::clearLockPauseNotice()
{
    if (m_pausedByScreenLock) {
        m_pausedByScreenLock = false;
        Q_EMIT pausedByScreenLockChanged();
    }
    setPausedBySleep(false);
}

void ScreenLockWatcher::setPausedBySleep(bool paused)
{
    if (m_pausedBySleep != paused) {
        m_pausedBySleep = paused;
        Q_EMIT pausedBySleepChanged();
    }
}

void ScreenLockWatcher::onPrepareForSleep(bool start)
{
    if (!m_engine) {
        return;
    }
    if (start) {
        // A phase already due ends first: the pause, and its notice, apply to the one that follows.
        m_engine->refresh();
        // Any running phase: a break slept through is not a break taken either.
        if (m_engine->isRunning()) {
            qCDebug(lcScreenLock) << "Pausing the timer: the system is going to sleep";
            setPausedBySleep(true);
            m_engine->pause();
        }
    } else if (m_pausedBySleep && m_engine->isPaused()) {
        qCDebug(lcScreenLock) << "Woke up with the timer paused by the sleep";
        Q_EMIT wokeUpAfterPause();
    }
}

void ScreenLockWatcher::pauseForScreenLock()
{
    m_pausedByScreenLock = true;
    Q_EMIT pausedByScreenLockChanged();
    m_engine->pause();
}

void ScreenLockWatcher::onActiveChanged(bool active)
{
    setScreenLocked(active, QString());
}

void ScreenLockWatcher::setScreenLocked(bool active, const QString &source)
{
    if (active == m_screenLocked) {
        return; // the same change, reported again on another object path or name
    }
    m_screenLocked = active;
#ifndef Q_OS_MACOS
    m_lockSource = active ? source : QString();
#else
    Q_UNUSED(source)
#endif
    qCDebug(lcScreenLock) << "Screen lock state changed:" << active;

    if (active) {
        if (m_settings && m_settings->autoPauseOnScreenLock() && m_engine) {
            // A work phase already due ends first; only a Work still running is paused. A due
            // break that starts Work by itself has its Work paused already (phaseFinished).
            m_engine->refresh();
            if (m_engine->state() == TimerEngine::State::Working) {
                qCDebug(lcScreenLock) << "Auto-pausing active timer due to screen lock";
                pauseForScreenLock();
            }
        }
    } else {
        if (m_pausedByScreenLock) {
            qCDebug(lcScreenLock) << "Screen unlocked after auto-pause";
            Q_EMIT screenUnlockedAfterPause();
        }
    }
}
