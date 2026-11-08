// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringList>

#include <functional>

class PresetModel;
class QUrl;
class TimerEngine;

/**
 * The commands other programs may give the timer, shared by every way in: the D-Bus /Timer
 * object (DBusTimerService, Linux) and sugo:// links (macOS).
 *
 * Every command first asks the "control allowed" check (Settings: "Allow other programs to
 * control the timer") and refuses with ControlDisabled when it says no. Choosing a timer
 * resets the engine (TimerBinding), so while a phase is active choosing another one is refused
 * with TimerActive instead of throwing the running session away; choosing the timer that is
 * already current is always fine.
 */
class TimerCommands
{
public:
    enum class Error {
        None,
        Unavailable,     ///< No engine (nothing to control); the caller is not answered.
        ControlDisabled, ///< Turned off in the settings.
        UnknownPreset,   ///< No timer has that name or uuid.
        TimerActive,     ///< Choosing another timer would discard the running phase.
        UnknownCommand,  ///< A link Sugo does not understand.
    };

    struct Result {
        Error error = Error::None;
        QString message; ///< For the user, when there is an error.
        bool ok() const { return error == Error::None; }
    };

    /// A sugo:// link, understood.
    struct UrlCommand {
        enum class Action { Invalid, Start, Pause, Resume, Toggle, Stop, Skip, Show, SelectPreset };
        Action action = Action::Invalid;
        QString argument; ///< The timer's name or uuid (SelectPreset, or Start with a timer).
    };

    TimerCommands(TimerEngine *engine, PresetModel *presets);

    /// Asked before every command; without one, commands are always allowed.
    void setControlAllowed(std::function<bool()> allowed) { m_allowed = std::move(allowed); }
    /// For commands set up before the timer exists (D-Bus objects registered early): until
    /// then every command is Unavailable.
    void attach(TimerEngine *engine, PresetModel *presets)
    {
        m_engine = engine;
        m_presets = presets;
    }

    Result toggle();
    /// Starts the pending phase, or resumes a paused timer (a running one is left alone).
    Result start();
    Result pause();
    Result resume();
    Result stop();
    Result skip();

    /// Chooses the timer by exact uuid, else by name (case-insensitive, first match).
    Result selectPreset(const QString &nameOrUuid);
    /// Moves the selection by `step` rows, wrapping around.
    Result stepPreset(int step);
    /// selectPreset(), then start (or resume) the timer.
    Result startPreset(const QString &nameOrUuid);

    /// "state:phase:remaining seconds", e.g. "running:work:1200"; "unavailable" without an engine.
    QString status() const;
    /// Names of the timers, in list order.
    QStringList presets() const;

    /// The uuid `nameOrUuid` stands for; empty if there is none.
    QString resolvePreset(const QString &nameOrUuid) const;

    /**
     * Reads a sugo:// link: `sugo://<command>` with command start, pause, resume, toggle, stop,
     * skip or show (any case, a trailing slash allowed, `sugo:toggle` too), `sugo://start/<timer>`
     * and `sugo://preset/<timer>`, with the timer's name (percent-encoded) or uuid.
     */
    static UrlCommand parseUrl(const QUrl &url);
    /// Runs a parsed link. Show only reports success: showing the window is the caller's job,
    /// and it is allowed even when control by other programs is off.
    Result run(const UrlCommand &command);

private:
    bool mayControl(Result &result) const;
    Result switchTo(const QString &uuid);
    template<typename Action>
    Result simple(Action action);

    TimerEngine *m_engine;
    PresetModel *m_presets;
    std::function<bool()> m_allowed;
};
