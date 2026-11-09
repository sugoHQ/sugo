// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

class PresetModel;
class QAction;
class QActionGroup;
class QMenu;
class TimerEngine;

/**
 * The entries that drive the timer from a menu: Start/Pause/Resume, Stop, Skip and Add 5 Minutes, and a Timer
 * submenu that picks the timer in use. Shared by the tray (menu bar) icon's menu and the macOS
 * Dock menu, so both say and do the same.
 *
 * The entries are added to a menu the caller owns (which owns the actions too) and follow the
 * timer by themselves: Stop and Skip show only while a phase runs, Add 5 Minutes while there is a
 * phase to extend (TimerEngine::extendable), the timer list is locked
 * then, and the first entry's text and icon follow the state.
 *
 * Stop and Skip do not end the phase themselves: they emit endPhaseRequested(), which main.cpp
 * hands to the window, so they ask first when work under way would be cut short, as everywhere else.
 */
class TimerMenuActions : public QObject
{
    Q_OBJECT

public:
    TimerMenuActions(TimerEngine *engine, PresetModel *presets, QObject *parent = nullptr);

    /// Adds Start/Pause/Resume, Stop, Skip and Add 5 Minutes to `menu`, in that order.
    void addControls(QMenu *menu);
    /// Adds the Timer submenu to `menu`.
    void addTimerMenu(QMenu *menu);

    /// Sets every text and brings the entries up to date: call once the entries are added.
    void updateTexts();
    /// Brings the entries in line with the timer's state, phase and timer.
    void refresh();
    /// Lists the timers again if they changed, and ticks the one in use.
    void rebuildTimerMenu();

Q_SIGNALS:
    /// Stop (`skipping` false) or Skip was chosen.
    void endPhaseRequested(bool skipping);

private:
    void syncTimerChecks();

    TimerEngine *m_engine;
    PresetModel *m_presets;
    QPointer<QAction> m_toggleAction;
    QPointer<QAction> m_stopAction;
    QPointer<QAction> m_skipAction;
    QPointer<QAction> m_extendAction;
    QPointer<QMenu> m_timerMenu;
    QPointer<QActionGroup> m_timerGroup;
    QString m_timerMenuKey; ///< What the Timer submenu currently lists, to skip no-op rebuilds.
    QString m_stateKey;     ///< State and phase the entries were last set for.
};
