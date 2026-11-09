// SPDX-License-Identifier: GPL-3.0-or-later
#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QPointer>
#include <QSignalSpy>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>

#include <memory>

#include "AppSettings.h"
#include "Autostart.h"
#include "FakeClock.h"
#include "PresetModel.h"
#include "Settings.h"
#include "TimerEngine.h"
#include "TrayController.h"

namespace
{
class NoAutostart final : public Autostart
{
public:
    bool isSupported() const override { return false; }
    bool isEnabled() const override { return false; }
    bool setEnabled(bool) override { return false; }
    void repair() override {}
};

QAction *actionNamed(const QMenu *menu, const QString &text)
{
    const QList<QAction *> actions = menu->actions();
    for (QAction *action : actions) {
        if (action->text() == text) {
            return action;
        }
    }
    return nullptr;
}

QImage picture(const QSystemTrayIcon *icon)
{
    return icon->icon().pixmap(64, 64).toImage();
}
} // namespace

/// The tray icon: its picture, tooltip and menu, and the window it shows and hides.
class TrayControllerTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir dir;
    FakeClock clock;
    std::unique_ptr<TimerEngine> engine;
    std::unique_ptr<PresetModel> presets;
    std::unique_ptr<AppSettings> settings;
    std::unique_ptr<TrayController> tray;

    QMenu *menu() const { return tray->icon() ? tray->icon()->contextMenu() : nullptr; }
    /// What the menu shows when it opens (its texts are refreshed then).
    void openMenu() { Q_EMIT menu()->aboutToShow(); }

private Q_SLOTS:
    void init()
    {
        clock = FakeClock();
        engine = std::make_unique<TimerEngine>(&clock, nullptr);
        presets = std::make_unique<PresetModel>();
        settings = std::make_unique<AppSettings>(Settings::open(dir.filePath(QStringLiteral("settings.conf"))),
                                                 std::make_unique<NoAutostart>(), true);
        settings->setShowTrayIcon(true);
        tray = std::make_unique<TrayController>(engine.get(), presets.get(), settings.get());
    }

    void cleanup()
    {
        tray.reset();
        settings.reset();
        presets.reset();
        engine.reset();
        QFile::remove(dir.filePath(QStringLiteral("settings.conf")));
    }

    void followsTheShowTrayIconSetting()
    {
        QVERIFY(tray->isVisible());
        QVERIFY(tray->icon()->isVisible());
        settings->setShowTrayIcon(false);
        QVERIFY(!tray->isVisible());
        QCOMPARE(tray->icon(), nullptr);
        settings->setShowTrayIcon(true);
        QVERIFY(tray->isVisible());
        QVERIFY(menu());
    }

    void noTrayNoIcon()
    {
        tray.reset();
        settings = std::make_unique<AppSettings>(Settings::open(dir.filePath(QStringLiteral("other.conf"))),
                                                 std::make_unique<NoAutostart>(), false);
        tray = std::make_unique<TrayController>(engine.get(), presets.get(), settings.get());
        QVERIFY(!tray->isVisible());
    }

    void tooltipAndPictureFollowTheTimer()
    {
        QVERIFY(tray->icon()->toolTip().startsWith(QStringLiteral("Sugo: ready")));
        QVERIFY(tray->icon()->toolTip().contains(QStringLiteral("Pomodoro")));
        // Idle, the icon is the application's own from the icon theme (none offscreen).
        const QImage idle = picture(tray->icon());

        engine->start();
        clock.advanceSeconds(60);
        engine->refresh();
        QVERIFY(tray->icon()->toolTip().contains(QStringLiteral("left")));
        const QImage running = picture(tray->icon());
        QVERIFY(!running.isNull()); // the badge and the progress are drawn
        QVERIFY(running != idle);

        // Every badge style draws something different.
        settings->setTrayBadgeStyle(1);
        const QImage filled = picture(tray->icon());
        settings->setTrayBadgeStyle(2);
        const QImage spiral = picture(tray->icon());
        QVERIFY(filled != running);
        QVERIFY(spiral != filled);

        engine->pause();
        QVERIFY(tray->icon()->toolTip().contains(QStringLiteral("paused")));
        QVERIFY(picture(tray->icon()) != spiral);
    }

    void menuControlsTheTimer()
    {
        openMenu();
        QAction *start = actionNamed(menu(), QStringLiteral("Start"));
        QAction *stop = actionNamed(menu(), QStringLiteral("Stop"));
        QAction *skip = actionNamed(menu(), QStringLiteral("Skip"));
        QAction *extend = actionNamed(menu(), QStringLiteral("Add 5 Minutes"));
        QVERIFY(start && stop && skip && extend);
        QVERIFY(!stop->isVisible()); // nothing to stop while idle
        QVERIFY(!skip->isVisible());
        QVERIFY(!extend->isVisible());
        // The first line says what is coming.
        QVERIFY(menu()->actions().first()->text().contains(QStringLiteral("25:00")));
        QVERIFY(!menu()->actions().first()->isEnabled());

        start->trigger();
        QVERIFY(engine->isActive());
        QCOMPARE(start->text(), QStringLiteral("Pause"));
        QVERIFY(stop->isVisible());
        QVERIFY(skip->isVisible());
        QVERIFY(extend->isVisible());
        const int total = engine->totalSeconds();
        extend->trigger();
        QCOMPARE(engine->totalSeconds(), total + TimerEngine::kExtendSeconds);

        start->trigger();
        QVERIFY(engine->isPaused());
        QCOMPARE(start->text(), QStringLiteral("Resume"));

        // Stop and Skip ask the window, which asks first when work would be cut short: the
        // menu never ends the phase by itself.
        QSignalSpy endPhase(tray.get(), SIGNAL(endPhaseRequested(bool)));
        QVERIFY(endPhase.isValid());
        skip->trigger();
        stop->trigger();
        QCOMPARE(engine->phase(), TimerEngine::Phase::Work);
        QVERIFY(engine->isActive());
        QCOMPARE(endPhase.count(), 2);
        QCOMPARE(endPhase.at(0).at(0).toBool(), true);
        QCOMPARE(endPhase.at(1).at(0).toBool(), false);

        engine->skip();
        QCOMPARE(engine->phase(), TimerEngine::Phase::ShortBreak);
        engine->stop();
        QVERIFY(!engine->isActive());
        QCOMPARE(start->text(), engine->phase() == TimerEngine::Phase::Work ? QStringLiteral("Start") : QStringLiteral("Start break"));
        QVERIFY(!stop->isVisible());
    }

    void menuPicksTheTimer()
    {
        const QString other = presets->create({{QStringLiteral("name"), QStringLiteral("Reading")}});
        openMenu();
        QAction *timers = actionNamed(menu(), QStringLiteral("Timer"));
        QVERIFY(timers && timers->menu());
        QAction *reading = actionNamed(timers->menu(), QStringLiteral("Reading"));
        QAction *pomodoro = actionNamed(timers->menu(), QStringLiteral("Pomodoro"));
        QVERIFY(reading && pomodoro);
        QVERIFY(pomodoro->isChecked());

        reading->trigger();
        QCOMPARE(presets->currentUuid(), other);
        QVERIFY(reading->isChecked());
        QVERIFY(!pomodoro->isChecked());

        // While a phase runs the timer cannot be changed from here.
        engine->start();
        QVERIFY(!timers->isEnabled());
        engine->stop();
        QVERIFY(timers->isEnabled());

        // Renaming a timer renames its entry.
        presets->update(other, {{QStringLiteral("name"), QStringLiteral("Deep reading")}});
        openMenu();
        QVERIFY(actionNamed(timers->menu(), QStringLiteral("Deep reading")));
    }

    void namesWithAnAmpersandKeepIt()
    {
        // "&" marks a mnemonic in menu texts: a timer named "R&D" must not show as "RD" or with an
        // underlined D, nor "Tom & Jerry" lose its "&".
        const QString rnd = presets->create({{QStringLiteral("name"), QStringLiteral("R&D")}});
        presets->create({{QStringLiteral("name"), QStringLiteral("Tom & Jerry")}});
        presets->setCurrentUuid(rnd);
        openMenu();
        QAction *timers = actionNamed(menu(), QStringLiteral("Timer"));
        QVERIFY(timers && timers->menu());
        QAction *entry = actionNamed(timers->menu(), QStringLiteral("R&&D"));
        QVERIFY(entry);
        QCOMPARE(entry->iconText(), QStringLiteral("R&D")); // what the menu shows
        QVERIFY(entry->isChecked());
        QAction *tom = actionNamed(timers->menu(), QStringLiteral("Tom && Jerry"));
        QVERIFY(tom);
        QCOMPARE(tom->iconText(), QStringLiteral("Tom & Jerry"));

        QAction *status = menu()->actions().first();
        QVERIFY2(status->text().endsWith(QStringLiteral(" R&&D")), qPrintable(status->text()));
        QVERIFY2(status->iconText().endsWith(QStringLiteral(" R&D")), qPrintable(status->iconText()));
    }

    void menuOpensPagesAndTheWindow()
    {
        QSignalSpy pages(tray.get(), &TrayController::openPageRequested);
        QSignalSpy show(tray.get(), &TrayController::showWindowRequested);
        openMenu();
        actionNamed(menu(), QStringLiteral("Statistics"))->trigger();
        actionNamed(menu(), QStringLiteral("Settings"))->trigger();
        QCOMPARE(pages.count(), 2);
        QCOMPARE(pages.at(0).at(0).toString(), QStringLiteral("stats"));
        QCOMPARE(pages.at(1).at(0).toString(), QStringLiteral("settings"));

        // No window yet: "Show Window", and clicking the icon asks for it.
        QAction *window = actionNamed(menu(), QStringLiteral("Show Window"));
        QVERIFY(window);
#ifdef Q_OS_MACOS
        // The macOS menu bar opens the menu on a click: the click itself leaves the window alone.
        Q_EMIT tray->icon()->activated(QSystemTrayIcon::Trigger);
        QCOMPARE(show.count(), 0);
        window->trigger();
#else
        Q_EMIT tray->icon()->activated(QSystemTrayIcon::Trigger);
#endif
        QCOMPARE(show.count(), 1);

        // A window on screen: the click hides it, and the menu offers to hide it.
        QWindow shown;
        shown.resize(100, 100);
        shown.show();
        QVERIFY(QTest::qWaitForWindowExposed(&shown));
        tray->setWindow(&shown);
        openMenu();
        QCOMPARE(window->text(), QStringLiteral("Hide Window"));
#ifdef Q_OS_MACOS
        window->trigger();
#else
        Q_EMIT tray->icon()->activated(QSystemTrayIcon::Trigger);
#endif
        QVERIFY(!shown.isVisible());
        QCOMPARE(show.count(), 1);
#ifndef Q_OS_MACOS
        // Hidden: the click asks for it again; other clicks do nothing.
        Q_EMIT tray->icon()->activated(QSystemTrayIcon::Context);
        QCOMPARE(show.count(), 1);
        Q_EMIT tray->icon()->activated(QSystemTrayIcon::Trigger);
        QCOMPARE(show.count(), 2);
#endif
        tray->setWindow(nullptr);
    }

    void quitAsksTheWindow()
    {
        // Quit leaves the decision to the window (main.cpp), which asks first while a phase runs:
        // the tray itself neither quits nor ends the phase.
        QSignalSpy quit(tray.get(), &TrayController::quitRequested);
        engine->start();
        openMenu();
        QAction *action = actionNamed(menu(), QStringLiteral("Quit"));
        QVERIFY(action);
        action->trigger();
        QCOMPARE(quit.count(), 1);
        QVERIFY(engine->isActive());
        QVERIFY(tray->isVisible());
    }

    void iconGoesBeforeItsMenu()
    {
        // Removed by the setting, and with the controller: the icon never outlives its menu.
        for (const bool byController : {false, true}) {
            QPointer<QMenu> owned = menu();
            QVERIFY(owned);
            bool menuThereWhenIconWent = false;
            connect(tray->icon(), &QObject::destroyed, this, [&menuThereWhenIconWent, &owned]() {
                menuThereWhenIconWent = !owned.isNull();
            });
            if (byController) {
                tray.reset();
            } else {
                settings->setShowTrayIcon(false);
                settings->setShowTrayIcon(true);
            }
            QVERIFY(menuThereWhenIconWent);
            QVERIFY(owned.isNull());
        }
    }
};

QTEST_MAIN(TrayControllerTest)
#include "tst_traycontroller.moc"
