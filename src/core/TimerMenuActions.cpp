// SPDX-License-Identifier: GPL-3.0-or-later
#include "TimerMenuActions.h"

#include "PresetModel.h"
#include "TimerEngine.h"
#include "TrayPresenter.h"

#include "Localization.h"

#include <QAction>
#include <QActionGroup>
#include <QIcon>
#include <QMenu>

#include <iterator>

namespace
{
/// The choices of Add Time, as in the window's composer.
constexpr int kExtendMinutes[] = {2, 5, 10};
} // namespace

TimerMenuActions::TimerMenuActions(TimerEngine *engine, PresetModel *presets, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_presets(presets)
{
    connect(engine, &TimerEngine::stateChanged, this, &TimerMenuActions::refresh);
    connect(engine, &TimerEngine::phaseChanged, this, &TimerMenuActions::refresh);
    connect(engine, &TimerEngine::extendableChanged, this, &TimerMenuActions::refresh);
    // Anything that can change the list of timers; rebuildTimerMenu() skips unchanged lists.
    for (auto signal : {&PresetModel::modified, &PresetModel::currentChanged, &PresetModel::countChanged}) {
        connect(presets, signal, this, &TimerMenuActions::rebuildTimerMenu);
    }
    connect(presets, &PresetModel::rowsInserted, this, &TimerMenuActions::rebuildTimerMenu);
    connect(presets, &PresetModel::rowsRemoved, this, &TimerMenuActions::rebuildTimerMenu);
    connect(presets, &PresetModel::rowsMoved, this, &TimerMenuActions::rebuildTimerMenu);
    connect(presets, &PresetModel::modelReset, this, &TimerMenuActions::rebuildTimerMenu);
    connect(presets, &PresetModel::dataChanged, this, &TimerMenuActions::rebuildTimerMenu);
}

void TimerMenuActions::addControls(QMenu *menu)
{
    m_toggleAction = menu->addAction(QIcon::fromTheme(QStringLiteral("media-playback-start")), QString(), this, [this]() {
        m_engine->toggle();
    });
    m_stopAction = menu->addAction(QIcon::fromTheme(QStringLiteral("media-playback-stop")), QString(), this, [this]() {
        Q_EMIT endPhaseRequested(false);
    });
    m_skipAction = menu->addAction(QIcon::fromTheme(QStringLiteral("media-skip-forward")), QString(), this, [this]() {
        Q_EMIT endPhaseRequested(true);
    });
    m_extendMenu = menu->addMenu(QIcon::fromTheme(QStringLiteral("list-add")), QString());
    for (const int minutes : kExtendMinutes) {
        m_extendActions.append(m_extendMenu->addAction(QString(), this, [this, minutes]() {
            m_engine->extend(minutes * 60, TimerEngine::ExtendSource::Menu);
        }));
    }
}

void TimerMenuActions::addTimerMenu(QMenu *menu)
{
    m_timerMenu = menu->addMenu(QIcon::fromTheme(QStringLiteral("chronometer")), QString());
    m_timerGroup = new QActionGroup(m_timerMenu);
    m_timerGroup->setExclusive(true);
}

void TimerMenuActions::updateTexts()
{
    if (m_timerMenu) {
        m_timerMenu->setTitle(i18n("Timer"));
    }
    if (m_stopAction) {
        m_stopAction->setText(i18n("Stop"));
    }
    if (m_skipAction) {
        m_skipAction->setText(i18n("Skip"));
    }
    if (m_extendMenu) {
        m_extendMenu->setTitle(i18n("Add Time"));
    }
    for (qsizetype i = 0; i < m_extendActions.size() && i < qsizetype(std::size(kExtendMinutes)); ++i) {
        if (m_extendActions.at(i)) {
            m_extendActions.at(i)->setText(i18np("%1 Minute", "%1 Minutes", kExtendMinutes[i]));
        }
    }
    m_timerMenuKey.clear();
    m_stateKey.clear();
    rebuildTimerMenu();
    refresh();
}

void TimerMenuActions::refresh()
{
    const bool idle = m_engine->state() == TimerEngine::State::Idle;
    const bool paused = m_engine->isPaused();
    if (m_toggleAction) {
        QString text;
        if (paused) {
            text = i18n("Resume");
        } else if (idle) {
            text = m_engine->phase() == TimerEngine::Phase::Work ? i18n("Start") : i18n("Start break");
        } else {
            text = i18n("Pause");
        }
        if (m_toggleAction->text() != text) {
            m_toggleAction->setText(text);
            m_toggleAction->setIcon(QIcon::fromTheme(idle || paused ? QStringLiteral("media-playback-start")
                                                                     : QStringLiteral("media-playback-pause")));
        }
    }

    // Only a change of state touches the entries: menus shown by the desktop are sent again on each change.
    const QString key =
        QStringLiteral("%1/%2/%3").arg(int(m_engine->state())).arg(int(m_engine->phase())).arg(m_engine->isExtendable());
    if (key == m_stateKey) {
        return;
    }
    m_stateKey = key;
    const bool active = !idle;
    if (m_stopAction) {
        m_stopAction->setVisible(active);
    }
    if (m_skipAction) {
        m_skipAction->setVisible(active);
    }
    if (m_extendMenu) {
        m_extendMenu->menuAction()->setVisible(m_engine->isExtendable());
    }
    if (m_timerMenu) {
        m_timerMenu->setEnabled(!active);
    }
}

void TimerMenuActions::rebuildTimerMenu()
{
    if (!m_timerMenu || !m_timerGroup) {
        return;
    }
    const QList<TimerPreset> &list = m_presets->presets();
    // The key covers what the menu entries are made of, NOT which timer is current: choosing another
    // timer must only move the check mark. Rebuilding the entries on every choice gave the desktop a
    // brand-new menu while it still showed the old one, and it ended up with two timers ticked.
    QString key;
    for (int row = 0; row < list.size(); ++row) {
        key += list.at(row).uuid + QLatin1Char('\t') + list.at(row).name + QLatin1Char('\t')
            + m_presets->data(m_presets->index(row), PresetModel::IconNameRole).toString() + QLatin1Char('\n');
    }
    if (key != m_timerMenuKey) {
        m_timerMenuKey = key;
        m_timerMenu->clear(); // deletes the old actions, which leaves the group
        for (int row = 0; row < list.size(); ++row) {
            const TimerPreset &preset = list.at(row);
            const QString iconName = m_presets->data(m_presets->index(row), PresetModel::IconNameRole).toString();
            QAction *action = m_timerMenu->addAction(iconName.isEmpty() ? QIcon() : QIcon::fromTheme(iconName),
                                                     menuText(preset.name));
            action->setCheckable(true);
            action->setData(preset.uuid);
            m_timerGroup->addAction(action);
            const QString uuid = preset.uuid;
            connect(action, &QAction::triggered, this, [this, uuid]() {
                // Locking the submenu does not disable its entries, and a copy of the menu shown before
                // the phase started (Dock menu, desktop tray menu) can still trigger them: switching
                // timers would reset the engine and end the running session.
                if (m_engine->isActive()) {
                    syncTimerChecks();
                    return;
                }
                m_presets->setCurrentUuid(uuid);
                // If the choice was refused the clicked entry must not stay ticked.
                syncTimerChecks();
            });
        }
    }
    syncTimerChecks();
}

void TimerMenuActions::syncTimerChecks()
{
    if (!m_timerGroup) {
        return;
    }
    // The group is exclusive: ticking the current timer unticks every other one, and only the
    // actions whose state really changes are announced to the desktop.
    const QString current = m_presets->currentUuid();
    const QList<QAction *> actions = m_timerGroup->actions();
    for (QAction *action : actions) {
        if (action->data().toString() == current && !action->isChecked()) {
            action->setChecked(true);
            return;
        }
    }
}
