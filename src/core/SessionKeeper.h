// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <functional>

#include "TimerEngine.h"

class PresetModel;
class SessionRecorder;

/**
 * Keeps the session under way when Sugo ends (quit, logout, a crash) and brings it back, paused,
 * at the next start.
 *
 * While a phase is active, what it needs to go on (timer, phase, start, planned and remaining
 * time, cycles) is written to a small file next to the database: whenever the timer changes,
 * every kHeartbeatMs while it is active, and at quit, where keepForQuit() takes the place of stopping
 * the timer. While the timer is idle there is no file. A crash loses at most the last
 * kHeartbeatMs of the countdown; the time Sugo was not running never counts.
 *
 * At the next start, restore() puts the timer back in that phase, paused, with the time it had
 * left, as the same session (same start; the time in between counts like a pause). Too old a
 * session (kMaxAgeMs), or one whose timer is gone, is recorded as interrupted instead, as
 * stopping it would have. A phase taken up again after it ended (TimerEngine::continueFinished)
 * is not kept: quitting ends it, as before.
 *
 * `restored` stays true while the page should say why the timer is paused: until the session is
 * resumed or ended, or the message dismissed. restoreEnded() tells, once, what became of it.
 */
class SessionKeeper : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /// A session of the last run is waiting, paused (for the message on the focus page).
    Q_PROPERTY(bool restored READ isRestored NOTIFY restoredChanged FINAL)

public:
    static constexpr qint64 kMaxAgeMs = 12LL * 3600 * 1000;
    static constexpr int kHeartbeatMs = 30 * 1000;

    explicit SessionKeeper(QObject *parent = nullptr);

    /// Follows `engine` from now on; `path` is the file the session is kept in (defaultPath() in the app).
    void attach(TimerEngine *engine, PresetModel *presets, SessionRecorder *recorder, const QString &path);

    /// Next to the database (Database::defaultPath()).
    static QString defaultPath();

    /// The wall clock (tests).
    void setNow(std::function<QDateTime()> now) { m_now = std::move(now); }

    /// At startup, before the timer is used: brings back the session the last run kept. True if
    /// the timer now holds it, paused. Until it has run, nothing is written.
    bool restore();

    /// At quit: keeps the active session for the next start. True if it was kept, so the timer
    /// must not be stopped (which would record it as interrupted).
    bool keepForQuit();

    bool isRestored() const { return m_notice; }
    /// The timer holds a session of the last run that was neither resumed nor ended yet.
    bool holdsRestoredSession() const { return m_tracking; }

public Q_SLOTS:
    /// Writes the timer's state now (or removes the file while the timer is idle).
    void save();
    /// Hides the message; the session itself is not touched.
    void dismiss();

Q_SIGNALS:
    void restoredChanged();
    /// What became of a session from the last run: "resumed", "stopped" (ended or skipped) or
    /// "expired" (too old, or its timer is gone: recorded as interrupted at startup).
    void restoreEnded(TimerEngine::Phase phase, const QString &result);

private:
    /// Writes the file (or removes it while there is nothing to keep); true if it was written.
    bool write();
    void captureTimer();
    void finishTracking(const QString &result);
    void setNotice(bool shown);
    void removeFile();

    TimerEngine *m_engine = nullptr;
    PresetModel *m_presets = nullptr;
    SessionRecorder *m_recorder = nullptr;
    QString m_path;
    std::function<QDateTime()> m_now;
    QTimer m_heartbeat;
    bool m_quitting = false;
    bool m_restoreDone = false;         ///< restore() has read the last run's file
    bool m_tracking = false;            ///< a restored session not resumed or ended yet
    TimerEngine::Phase m_trackedPhase = TimerEngine::Phase::Work;
    bool m_notice = false;
    QString m_timerUuid;     ///< the timer of the active phase, as it was when the phase began
    QString m_timerName;
    QString m_timerCategory;
};
