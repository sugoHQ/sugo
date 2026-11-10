// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusContext>
#include <QObject>

#include <functional>

#include "TimerCommands.h"
#include "TimerEngine.h"

class PresetModel;

/**
 * Exposes TimerEngine controls over D-Bus at object path /Timer
 * with interface stream.sugo.app.Timer.
 *
 * This allows KDE Plasma Global Shortcuts, command line calls (qdbus),
 * scripts, and hardware buttons to start, pause, skip, or stop the timer.
 *
 * The commands can be switched off (Settings: "Allow other programs to control the timer");
 * they then answer with the D-Bus error ControlDisabled and change nothing. status() and
 * presets() stay available: they only read.
 *
 * Choosing a timer (selectPreset, nextPreset, previousPreset, startPreset) resets the engine
 * (TimerBinding), so while a phase is active these refuse with TimerActive instead of throwing
 * the running session away; choosing the timer that is already current is always fine.
 *
 * The commands themselves are TimerCommands, shared with the sugo:// links of macOS; this class
 * only turns their errors into D-Bus errors.
 */
class DBusTimerService;

/// The object registered at /Timer: it carries the adaptor and the D-Bus call context (for an
/// adaptor's slot, QtDBus gives the context to the adaptor's parent, so it must live here).
class TimerDBusObject : public QObject, public QDBusContext
{
    Q_OBJECT

public:
    using QObject::QObject;
};

class DBusTimerService : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "stream.sugo.app.Timer")

public:
    /// Register `host` (not the engine) at /Timer with QDBusConnection::ExportAdaptors.
    DBusTimerService(TimerEngine *engine, PresetModel *presets, TimerDBusObject *host);

    static inline const QString ControlDisabledError = QStringLiteral("stream.sugo.app.Error.ControlDisabled");
    static inline const QString UnknownPresetError = QStringLiteral("stream.sugo.app.Error.UnknownPreset");
    static inline const QString TimerActiveError = QStringLiteral("stream.sugo.app.Error.TimerActive");

    /// Asked before every command; without one, commands are always allowed.
    void setControlAllowed(std::function<bool()> allowed) { m_commands.setControlAllowed(std::move(allowed)); }
    /// For a service created (and registered) before the timer exists: until then its commands
    /// do nothing.
    void attach(TimerEngine *engine, PresetModel *presets) { m_commands.attach(engine, presets); }

public Q_SLOTS:
    void toggle();
    void start();
    void pause();
    void stop();
    void skip();
    /// Five more minutes for the active phase, or for the one that has just ended.
    void extend();
    /// `minutes` more (1 to 60), the same way.
    void extendBy(int minutes);
    QString status() const;

    /// Names of the timers, in list order.
    QStringList presets() const;
    /// Chooses the timer by exact uuid, else by name (case-insensitive, first match).
    /// Errors: UnknownPreset, TimerActive.
    bool selectPreset(const QString &nameOrUuid);
    /// The next / previous timer in the list, wrapping around. Error: TimerActive.
    void nextPreset();
    void previousPreset();
    /// selectPreset(), then start (or resume) the timer.
    bool startPreset(const QString &nameOrUuid);

private:
    /// True on success; otherwise answers the caller (when it is a D-Bus call) with the error.
    bool answer(const TimerCommands::Result &result);

    TimerCommands m_commands;
};
