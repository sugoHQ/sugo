// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QtQml/qqmlregistration.h>

class NotificationInhibitor;
class PresetModel;
class TimerEngine;

/**
 * Decides when notifications must be silenced and tells the NotificationInhibitor.
 *
 * Rule: notifications are silenced exactly while the engine is *Working* and the current
 * timer has "Silence notifications while working" on. Every other state releases them:
 * Paused, Idle (stopped or a phase just ended), a break. The rule is re-evaluated on any
 * change of engine state, selected timer or timer options, so the durations always follow
 * the timer's own work/break values, and switching timers mid-phase cannot leave
 * notifications off.
 *
 * When an elapsed phase auto-starts Work, the silencing follows one event-loop turn later, so
 * the end-of-phase announcement reaches the desktop before the inhibition does. The silencing of
 * a phase that ends is released as soon as it ends, even when the next phase is Work again (a
 * timer without breaks), where no state change would otherwise re-evaluate the rule.
 */
class SilenceController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /// The desktop can honour silencing (false e.g. on a notification server without Inhibit).
    Q_PROPERTY(bool available READ available NOTIFY availableChanged FINAL)
    /// Notifications are being silenced by us right now.
    Q_PROPERTY(bool active READ active NOTIFY activeChanged FINAL)

public:
    explicit SilenceController(QObject *parent = nullptr);

    /// Wires the controller up. Called once, before the UI loads.
    void attach(TimerEngine *engine, PresetModel *presets, NotificationInhibitor *inhibitor);

    bool available() const;
    bool active() const { return m_active; }

    /// Switches notifications back on and waits briefly for the desktop to confirm.
    /// Called when the application quits.
    void shutdown();

Q_SIGNALS:
    void availableChanged();
    void activeChanged();

private:
    void update();

    TimerEngine *m_engine = nullptr;
    PresetModel *m_presets = nullptr;
    NotificationInhibitor *m_inhibitor = nullptr;
    bool m_active = false;
    bool m_decided = false; ///< update() has told the inhibitor at least once
    bool m_wanted = false;  ///< what the inhibitor was last told
    bool m_phaseEnding = false;     ///< between sessionEnded and phaseFinished of an elapsed phase
    bool m_silenceDeferred = false; ///< silencing put off until that phase end is announced
};
