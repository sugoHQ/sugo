// SPDX-License-Identifier: GPL-3.0-or-later
#include "SilenceController.h"

#include "NotificationInhibitor.h"
#include "PresetModel.h"
#include "TimerEngine.h"

#include "Localization.h"

#include <QTimer>

SilenceController::SilenceController(QObject *parent)
    : QObject(parent)
{
}

void SilenceController::attach(TimerEngine *engine, PresetModel *presets, NotificationInhibitor *inhibitor)
{
    Q_ASSERT_X(!m_engine, "SilenceController::attach", "attach() must be called once");
    m_engine = engine;
    m_presets = presets;
    m_inhibitor = inhibitor;

    connect(engine, &TimerEngine::stateChanged, this, &SilenceController::update);
    // A phase that elapses emits sessionEnded(completed) first, then starts what follows (an
    // auto-started Work phase changes the state), and only then phaseFinished, which announces
    // the end. Silencing at the state change would send Inhibit before that announcement, and
    // the desktop would swallow it: the silencing waits for the next event-loop turn instead.
    connect(engine, &TimerEngine::sessionEnded, this,
            [this](TimerEngine::Phase, const QDateTime &, const QDateTime &, qint64, qint64, bool completed) {
                m_phaseEnding = completed;
                if (completed && m_decided && m_wanted) {
                    // The phase that just ended was silenced: let go now, before the announcement.
                    // A timer without breaks goes straight back to Work, so no state change would
                    // ever call update() and the inhibition would outlive the phase it belongs to.
                    // phaseFinished() silences again on the next event-loop turn.
                    m_wanted = false;
                    m_silenceDeferred = true;
                    m_inhibitor->release();
                }
            });
    connect(engine, &TimerEngine::phaseFinished, this, [this]() {
        m_phaseEnding = false;
        if (m_silenceDeferred) {
            m_silenceDeferred = false;
            // Queued, not called: the announcement's own phaseFinished slot may still be to come.
            QTimer::singleShot(0, this, &SilenceController::update);
        }
    });
    // `active` follows what the desktop actually granted (or lost).
    connect(inhibitor, &NotificationInhibitor::inhibitedChanged, this, &SilenceController::update);
    // The shortcuts of a macOS Focus can be created, renamed or chosen at any time.
    connect(inhibitor, &NotificationInhibitor::availableChanged, this, [this]() {
        Q_EMIT availableChanged();
        update();
    });
    connect(presets, &PresetModel::currentChanged, this, &SilenceController::update);
    // Only the current timer's options matter; editing another timer changes nothing here.
    connect(presets, &PresetModel::presetChanged, this, [this](const QString &uuid) {
        if (uuid == m_presets->currentUuid()) {
            update();
        }
    });

    Q_EMIT availableChanged();
    update();
}

bool SilenceController::available() const
{
    return m_inhibitor && m_inhibitor->isAvailable();
}

void SilenceController::update()
{
    if (!m_engine) {
        return;
    }

    const bool wanted = m_engine->state() == TimerEngine::State::Working
        && m_presets->currentPreset().boolOption(PresetOption::SilenceWhileWorking);

    if (wanted && m_phaseEnding && !(m_decided && m_wanted)) {
        // Announce first, silence after (see attach()). Releasing is never put off.
        m_silenceDeferred = true;
        return;
    }

    // Ask the desktop only when the decision changes (e.g. not on every edit of the timer).
    if (!m_decided || wanted != m_wanted) {
        m_decided = true;
        m_wanted = wanted;
        if (wanted) {
            m_inhibitor->inhibit(i18n("A focus session is in progress"));
        } else {
            m_inhibitor->release();
        }
    }

    const bool nowActive = wanted && available() && m_inhibitor->isInhibited();
    if (nowActive != m_active) {
        m_active = nowActive;
        Q_EMIT activeChanged();
    }
}

void SilenceController::shutdown()
{
    if (!m_inhibitor) {
        return;
    }
    m_engine = nullptr; // turns update() off: nothing may silence again while quitting
    m_wanted = false;
    m_inhibitor->release();
    // Running a shortcut (macOS) takes longer than a D-Bus call: each inhibitor says how long.
    m_inhibitor->waitUntilReleased(m_inhibitor->releaseTimeoutMs());
}
