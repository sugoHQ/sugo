// SPDX-License-Identifier: GPL-3.0-or-later
#include "TrayController.h"

#include "TrayIconStyle.h"

#include "AppSettings.h"
#include "PresetModel.h"
#include "TimerEngine.h"
#include "TimerMenuActions.h"
#include "TrayPresenter.h"

#include "Localization.h"

#include <QAction>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSystemTrayIcon>
#include <QWindow>

#include <cmath>

namespace
{
const QString kIconName = QStringLiteral("stream.sugo.app");

/// The outline of `rect` with rounded corners, starting at 12 o'clock and running clockwise.
QPainterPath roundedFromTop(const QRectF &rect, double radius)
{
    const double d = 2 * radius;
    QPainterPath path(QPointF(rect.center().x(), rect.top()));
    path.arcTo(QRectF(rect.right() - d, rect.top(), d, d), 90, -90);
    path.arcTo(QRectF(rect.right() - d, rect.bottom() - d, d, d), 0, -90);
    path.arcTo(QRectF(rect.left(), rect.bottom() - d, d, d), 270, -90);
    path.arcTo(QRectF(rect.left(), rect.top(), d, d), 180, -90);
    path.closeSubpath();
    return path;
}

/// The spiral of the application icon, from its centre out to 12 o'clock under the leaf.
QPainterPath spiral(double size)
{
    constexpr double sweep = 3.2 * M_PI;
    QPainterPath path;
    for (double t = 0; t <= sweep + 1e-9; t += 0.08) {
        const double r = size * (0.04 + 0.25 * t / sweep);
        const double a = t - sweep - M_PI / 2;
        const QPointF point(size * 0.5 + r * std::cos(a), size * 0.553 + r * std::sin(a));
        t == 0 ? path.moveTo(point) : path.lineTo(point);
    }
    return path;
}

/// Strokes `path` with a faint track in `color` and, over it, its first `progress` (0..1).
void strokeProgress(QPainter &p, const QPainterPath &path, const QColor &color, double width, double progress)
{
    QPen pen(TrayIconColors::track(color), width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setBrush(Qt::NoBrush);
    p.setPen(pen);
    p.drawPath(path);
    if (progress > 0.005) {
        // Dash lengths are in pen widths: one dash as long as the elapsed part, then a gap past the end.
        const double length = path.length() / width;
        pen.setColor(color);
        pen.setDashPattern({length * progress, length + 1});
        p.setPen(pen);
        p.drawPath(path);
    }
}

/// The application icon at several sizes:
/// - When idle (minutes < 0): the application icon.
/// - When active (minutes >= 0): the icon's dark tile in the phase colour `primary` (the accent
///   colour setting for work, the break colour for breaks), with
///   * style 0: a border that fills clockwise as the phase runs and the remaining minutes,
///   * style 1: a fill rising from the bottom and the remaining minutes,
///   * style 2: the icon's spiral filling from its centre, without numbers.
QIcon trayIcon(int minutes, int badgeStyle, double progressRatio, bool isPaused,
               const QColor &primary)
{
    if (minutes < 0) {
#ifdef Q_OS_MACOS
        // The menu bar shows idle items in one colour, which macOS picks to suit the bar
        // (light, dark, tinted): a template image, made from the icon's alpha alone.
        QIcon idle(QStringLiteral(":/icons/sugo-tray-template.svg"));
        if (!idle.isNull()) {
            idle.setIsMask(true);
            return idle;
        }
#endif
        return QGuiApplication::windowIcon();
    }

    QIcon result;
#ifdef Q_OS_MACOS
    // The menu bar is 22 pt high and its icons 18 pt: 18 px, and 36 px on Retina screens.
    constexpr int sizes[] = {18, 36, 22, 32, 48, 64};
#else
    constexpr int sizes[] = {22, 32, 48, 64};
#endif
    for (const int size : sizes) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);

        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);

        if (isPaused) {
            p.setOpacity(0.75);
        }

        // The phase colour comes in as `primary`: the accent for work, the break colour for breaks
        // (both from TrayIconStyle, the same definition the interface's picture reads).
        const double progress = qBound(0.0, progressRatio, 1.0);

        const QRectF tileRect(0.5, 0.5, size - 1.0, size - 1.0);
        const double radius = size * 0.25;
        QPainterPath tile;
        tile.addRoundedRect(tileRect, radius, radius);
        p.setPen(QPen(TrayIconColors::edge(), 1.0));
        p.setBrush(TrayIconColors::dial());
        p.drawPath(tile);

        if (badgeStyle == 2) {
            strokeProgress(p, spiral(size), primary, qMax(1.8, size * 0.09), progress);
            QTransform leaf;
            leaf.translate(size * 0.5, size * 0.263);
            leaf.rotate(20);
            leaf.scale(size * 0.0072, size * 0.0072);
            QPainterPath leafPath;
            leafPath.cubicTo(-10, -6, -10, -18, -2, -24);
            leafPath.cubicTo(10, -18, 10, -6, 0, 0);
            p.setPen(Qt::NoPen);
            p.setBrush(TrayIconColors::leaf());
            p.drawPath(leaf.map(leafPath));
        } else {
            if (badgeStyle == 1) {
                p.save();
                p.setClipPath(tile);
                p.fillRect(QRectF(0, size * (1 - progress), size, size * progress), primary);
                p.restore();
            } else {
                const double penWidth = qMax(2.0, std::round(size * 0.11));
                const double inset = 0.5 + penWidth / 2.0;
                const QRectF borderRect = QRectF(0, 0, size, size).adjusted(inset, inset, -inset, -inset);
                strokeProgress(p, roundedFromTop(borderRect, radius - inset), primary, penWidth, progress);
            }

            const QString text = QString::number(minutes);
            QFont font = p.font();
            font.setBold(true);
            if (size <= 18) {
                font.setPixelSize(text.length() >= 2 ? 9 : 10);
            } else if (size <= 24) {
                font.setPixelSize(text.length() >= 2 ? 10 : 11);
            } else if (size <= 32) {
                font.setPixelSize(text.length() >= 2 ? 13 : 15);
            } else {
                font.setPixelSize(qRound(size * (text.length() >= 2 ? 0.40 : 0.48)));
            }
            p.setFont(font);
            p.setPen(TrayIconColors::digits());
            p.drawText(QRectF(0, size <= 24 ? -0.5 : 0, size, size), Qt::AlignCenter, text);
        }

        p.end();
        result.addPixmap(pixmap);
    }
    return result;
}
} // namespace

TrayController::TrayController(TimerEngine *engine, PresetModel *presets, AppSettings *settings, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_presets(presets)
    , m_settings(settings)
{
    connect(engine, &TimerEngine::stateChanged, this, &TrayController::refresh);
    connect(engine, &TimerEngine::phaseChanged, this, &TrayController::refresh);
    connect(engine, &TimerEngine::remainingChanged, this, &TrayController::refresh);
    connect(presets, &PresetModel::currentChanged, this, &TrayController::refresh);
    connect(presets, &PresetModel::presetChanged, this, &TrayController::refresh);
    connect(settings, &AppSettings::showTrayIconChanged, this, &TrayController::sync);
    connect(settings, &AppSettings::trayAvailableChanged, this, &TrayController::sync);
    connect(settings, &AppSettings::trayBadgeStyleChanged, this, &TrayController::refresh);
    connect(settings, &AppSettings::accentColorChanged, this, &TrayController::refresh);
    sync();
}

TrayController::~TrayController()
{
    // The icon goes before the menu it shows (members would go the other way round).
    destroyItem();
}

void TrayController::setWindow(QWindow *window)
{
    m_window = window;
}

bool TrayController::windowShown() const
{
    return m_window && m_window->isVisible() && !(m_window->windowStates() & Qt::WindowMinimized);
}

void TrayController::toggleWindow()
{
    if (windowShown()) {
        m_window->hide();
    } else {
        Q_EMIT showWindowRequested();
    }
}

void TrayController::sync()
{
    const bool wanted = m_settings->showTrayIcon() && m_settings->trayAvailable();
    if (wanted && !m_item) {
        createItem();
    } else if (!wanted && m_item) {
        destroyItem();
    }
}

void TrayController::createItem()
{
    m_item = std::make_unique<QSystemTrayIcon>(QIcon::fromTheme(kIconName));
    m_menu = std::make_unique<QMenu>();
    QMenu *menu = m_menu.get();
    m_statusAction = menu->addAction(QString());
    m_statusAction->setEnabled(false);
    menu->addSeparator();
    // Start/Pause, Stop, Skip and the timers: the same entries as the macOS Dock menu.
    m_timerActions = std::make_unique<TimerMenuActions>(m_engine, m_presets);
    m_timerActions->addControls(menu);
    connect(m_timerActions.get(), &TimerMenuActions::endPhaseRequested, this, &TrayController::endPhaseRequested);
    menu->addSeparator();
    m_timerActions->addTimerMenu(menu);
    menu->addSeparator();
    m_statsAction = menu->addAction(QIcon::fromTheme(QStringLiteral("office-chart-bar")), QString(), this, [this]() {
        Q_EMIT openPageRequested(QStringLiteral("stats"));
    });
    m_settingsAction = menu->addAction(QIcon::fromTheme(QStringLiteral("configure")), QString(), this, [this]() {
        Q_EMIT openPageRequested(QStringLiteral("settings"));
    });
    menu->addSeparator();
    m_windowAction = menu->addAction(QString(), this, &TrayController::toggleWindow);
    m_quitAction = menu->addAction(QIcon::fromTheme(QStringLiteral("application-exit")), QString(), this,
                                   &TrayController::quitRequested);
    // The clock in the status line is refreshed only when the menu opens, never on a tick.
    connect(menu, &QMenu::aboutToShow, this, [this]() {
        updateStatusLine();
        m_timerActions->rebuildTimerMenu();
        if (m_windowAction) {
            m_windowAction->setText(windowShown() ? i18n("Hide Window") : i18n("Show Window"));
        }
    });
    m_item->setContextMenu(menu);
#ifndef Q_OS_MACOS
    // In the macOS menu bar a click opens the menu and also reports a Trigger: the window is
    // shown and hidden from the menu (and the Dock) there.
    connect(m_item.get(), &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            toggleWindow();
        }
    });
#endif

    updateTexts();
    m_item->show();
}

void TrayController::destroyItem()
{
    // The icon first: it still refers to its menu.
    m_item.reset();
    m_timerActions.reset();
    m_menu.reset(); // also deletes the actions
}

void TrayController::updateTexts()
{
    if (!m_item) {
        return;
    }
    if (m_statsAction) {
        m_statsAction->setText(i18n("Statistics"));
    }
    if (m_settingsAction) {
        m_settingsAction->setText(i18n("Settings"));
    }
    if (m_quitAction) {
        m_quitAction->setText(i18n("Quit"));
    }
    if (m_windowAction) {
        m_windowAction->setText(windowShown() ? i18n("Hide Window") : i18n("Show Window"));
    }
    m_iconKey.clear();
    m_toolTip.clear();
    m_menuKey.clear();
    m_timerActions->updateTexts();
    refresh();
}

void TrayController::updateStatusLine()
{
    if (m_statusAction) {
        const QString text = menuText(menuStatusLine(m_engine->state(), m_engine->phase(), m_engine->remainingSeconds(),
                                                     m_presets->currentPreset().name));
        if (m_statusAction->text() != text) {
            m_statusAction->setText(text);
        }
    }
}

void TrayController::refresh()
{
    if (!m_item) {
        return;
    }

    const TrayStatus status = describeTray(m_engine->state(), m_engine->phase(), m_engine->remainingSeconds(),
                                           m_presets->currentPreset().name);
    const int badgeStyle = m_settings ? m_settings->trayBadgeStyle() : 0;
    const bool isWork = m_engine->phase() == TimerEngine::Phase::Work;
    const bool isPaused = m_engine->isPaused();
    // The ring is drawn in 1/16 degree steps; 1/200 of a turn is finer than any tray size shows.
    const int progressStep = status.badgeMinutes >= 0 ? qRound(m_engine->progress() * 200.0) : 0;

    // Repainting and sending pixmaps over D-Bus is costly: do it only when the picture changes.
    const QString accent = m_settings ? m_settings->accentColor() : QString();
    const QString iconKey = QStringLiteral("%1/%2/%3/%4/%5/%6")
                                .arg(status.badgeMinutes)
                                .arg(badgeStyle)
                                .arg(progressStep)
                                .arg(isWork ? 1 : 0)
                                .arg(isPaused ? 1 : 0)
                                .arg(accent);
    if (iconKey != m_iconKey) {
        m_iconKey = iconKey;
        m_badgeMinutes = status.badgeMinutes;
        // The phase colour, from the same shades the interface (TrayBadge.qml) reads.
        const QColor primary = isWork ? TrayIconStyle::workBadgeColor(accent)
                                      : TrayIconStyle::breakBadgeColor(accent);
        const QIcon icon = trayIcon(m_badgeMinutes, badgeStyle, progressStep / 200.0, isPaused,
                                    primary);
        m_item->setIcon(icon);
    }
    const QString toolTip = status.toolTipText.isEmpty() ? status.toolTipTitle : status.toolTipTitle + QLatin1Char('\n') + status.toolTipText;
    if (toolTip != m_toolTip) {
        m_toolTip = toolTip;
        m_item->setToolTip(toolTip);
    }

    // The status line changes only with the state, the phase or the timer, not with each tick
    // (the entries that drive the timer follow it by themselves, see TimerMenuActions).
    const bool active = m_engine->isActive();
    // While idle the clock is the full phase length and can change with the timer; while a phase
    // runs it ticks, so it stays out of the key.
    const QString stableKey = QStringLiteral("%1/%2/%3/%4/%5")
                                  .arg(int(m_engine->state()))
                                  .arg(int(m_engine->phase()))
                                  .arg(m_presets->currentPreset().name, m_presets->currentUuid())
                                  .arg(active ? -1 : m_engine->remainingSeconds());
    if (stableKey != m_menuKey) {
        m_menuKey = stableKey;
        updateStatusLine();
    }
}
