// SPDX-License-Identifier: GPL-3.0-or-later
#include <QApplication>
#include <QClipboard>
#include <QCommandLineParser>
#include <QDesktopServices>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QFile>
#include <QDir>
#include <QTimer>
#include <QDebug>
#include <QEvent>
#include <QFileOpenEvent>
#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QUrl>
#include <QQmlContext>
#include <QQmlExtensionPlugin>

#include <QLocale>

#include <functional>
#include <memory>

#include "Localization.h"
#include "Notifier.h"

#include "AppSettings.h"
#include "DataManager.h"
#include "Database.h"
#include "InhibitorFactory.h"
#include "LegacyAppId.h"
#include "NotifierFactory.h"
#include "FocusSoundController.h"
#include "PhaseNotifier.h"
#include "PresetModel.h"
#include "PresetPersistence.h"
#include "PresetRepository.h"
#include "SessionKeeper.h"
#include "SessionRecorder.h"
#include "SessionRepository.h"
#include "SilenceController.h"
#ifdef Q_OS_MACOS
#include "DockController.h"
#include "UpdateInstaller.h"
#include "LocalSingleInstance.h"
#include "MacApp.h"
#include "MacKeychainStore.h"
#include "SecretHandoff.h"
#include "QuitPolicy.h"
#include "ShortcutsFocusInhibitor.h"
#include "ShortcutsFocusInstaller.h"
#else
#include "DBusTimerService.h"
#include "KRunnerService.h"
#include "SingleInstance.h"

#include <QDBusConnection>
#endif
#include "StatsModel.h"
#include "TimerBinding.h"
#include "TimerCommands.h"
#include "TimerEngine.h"
#include "TrayController.h"
#include "TrayIconStyle.h"
#include "UnixSignals.h"
#include "PowerInhibitor.h"
#include "ScreenLockWatcher.h"
#include "Diagnostics.h"
#include "SmokeInput.h"
#include "CalendarSync.h"
#include "DesktopStyle.h"
#include "SyncApi.h"
#include "SyncController.h"
#include "Telemetry.h"
#include "TelemetryInfo.h"
#include "ThemeIcons.h"
#include "UpdateChecker.h"

// The QML module is a static library (sugo_core); its plugin registers the types.
Q_IMPORT_QML_PLUGIN(SugoQmlPlugin)

#ifdef Q_OS_MACOS
namespace
{
/**
 * What macOS tells the application as a whole, seen on the application object.
 *
 * - A click on the Dock icon while the window is hidden shows it. Cocoa reports it as
 *   applicationShouldHandleReopen:, which Qt turns into an ApplicationActivate event (forced
 *   even when the application is already active). Activation by other means (Cmd+Tab, a click
 *   on a notification) also shows a hidden window. After a `--background` launch the
 *   activation macOS may give a newly launched application is ignored: the filter is armed a
 *   moment after the event loop starts.
 * - Quitting: every way ends in Cocoa's applicationShouldTerminate:, which Qt turns into
 *   QEvent::Quit to the application; Qt then closes every window and quits only if they all
 *   accept, and an ignored event makes Cocoa cancel the termination (and a logout with it).
 *   Cmd+Q, Sugo › Quit Sugo and the menu bar icon's Quit never come here first: they call the
 *   window's requestQuit(), which asks while a phase runs or is paused and then quits with
 *   QCoreApplication::quit(); on macOS that (like a SIGTERM) takes this same way, as Sugo's own
 *   quit. What else arrives is a `quit` Apple event from another program: Quit in the Dock
 *   menu, Activity Monitor, AppleScript. While a phase is active that one is refused and
 *   turned into the window's requestQuit() (QuitPolicy), so the Dock's Quit asks like Cmd+Q;
 *   the window comes to the front for it. Logging out, restarting or shutting down (a `quit`
 *   event with a reason) go through: refusing them would cancel the logout, and the user chose
 *   to end the session.
 *   A quit that goes through sets the window's `platformQuitting` property before Qt closes it
 *   (the window otherwise refuses closing: it hides, or asks), and the running phase is saved
 *   kept for the next start by SessionKeeper (or, if it cannot be, saved as interrupted by
 *   TimerEngine::stop) at aboutToQuit.
 * - A sugo:// link (`open sugo://toggle`, the Shortcuts app's "Open URLs"): Cocoa's GetURL
 *   event, which Qt delivers as a QFileOpenEvent with a URL, handed to `openUrl`. Opening a link
 *   also activates the application; that must not pop the hidden window up for a command like
 *   toggle, so a hidden window is shown a moment after the activation, unless a link arrives
 *   meanwhile or has just arrived (the order of the two events is not fixed).
 */
class MacApplicationEvents : public QObject
{
public:
    MacApplicationEvents(QQuickWindow *window, TimerEngine *timer, std::function<void()> showWindow,
                         std::function<void(const QUrl &)> openUrl, bool armed, QObject *parent)
        : QObject(parent)
        , m_window(window)
        , m_timer(timer)
        , m_showWindow(std::move(showWindow))
        , m_openUrl(std::move(openUrl))
        , m_armed(armed)
    {
        m_pendingShow.setSingleShot(true);
        m_pendingShow.setInterval(kActivationDelayMs);
        connect(&m_pendingShow, &QTimer::timeout, this, [this]() {
            if (m_window && !m_window->isVisible()) {
                m_showWindow();
            }
        });
    }

    void arm() { m_armed = true; }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == QCoreApplication::instance() && m_window) {
            if (event->type() == QEvent::ApplicationActivate) {
                const bool justOpenedUrl = m_lastUrl.isValid() && m_lastUrl.elapsed() < kUrlActivationMs;
                if (m_armed && !m_window->isVisible() && !justOpenedUrl) {
                    m_pendingShow.start();
                }
            } else if (event->type() == QEvent::Quit) {
                const bool phaseActive = m_timer && m_timer->isActive();
                if (QuitPolicy::needsConfirmation(macCurrentQuitOrigin(), phaseActive)) {
                    // Not inside applicationShouldTerminate:, which waits for the answer.
                    QTimer::singleShot(0, this, [this]() {
                        if (m_window) {
                            m_showWindow();
                            QMetaObject::invokeMethod(m_window, "requestQuit");
                        }
                    });
                    event->ignore();
                    return true;
                }
                m_window->setProperty("platformQuitting", true);
            } else if (event->type() == QEvent::FileOpen) {
                const QUrl url = static_cast<QFileOpenEvent *>(event)->url();
                if (url.scheme().compare(QLatin1String("sugo"), Qt::CaseInsensitive) == 0) {
                    m_pendingShow.stop();
                    m_lastUrl.start();
                    m_openUrl(url);
                    return true;
                }
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    static constexpr int kActivationDelayMs = 250; ///< How long a link may follow its activation.
    static constexpr int kUrlActivationMs = 1000;  ///< How long an activation may follow its link.

    QPointer<QQuickWindow> m_window;
    QPointer<TimerEngine> m_timer;
    std::function<void()> m_showWindow;
    std::function<void(const QUrl &)> m_openUrl;
    bool m_armed;
    QTimer m_pendingShow;
    QElapsedTimer m_lastUrl;
};
} // namespace
#endif

int main(int argc, char *argv[])
{
    Diagnostics::installLogHandler();

    // The same look everywhere: our own components, and the Basic style (coloured from the
    // theme) for the standard controls.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QApplication app(argc, argv);

    // The interface is in English: so are day and month names, in the user's regional order.
    QLocale::setDefault(QLocale(QLocale::English, QLocale::system().territory()));

    QCoreApplication::setOrganizationName(QStringLiteral("sugo"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("sugo.stream"));
    QCoreApplication::setApplicationName(QStringLiteral("sugo"));
    QGuiApplication::setDesktopFileName(QStringLiteral("stream.sugo.app"));
    // Our own file, not the theme's: on Wayland Qt sends a theme icon's name, which the compositor
    // can't resolve when nothing is installed (an AppImage run directly).
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/stream.sugo.app.svg")));

    QGuiApplication::setApplicationDisplayName(i18n("Sugo"));
    QCoreApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION));
    // What the About page shows.
    const QVariantMap about{
        {QStringLiteral("displayName"), i18n("Sugo")},
        {QStringLiteral("version"), QStringLiteral(PROJECT_VERSION)},
        {QStringLiteral("shortDescription"), i18n("A Pomodoro timer with time statistics")},
        {QStringLiteral("copyrightStatement"), i18n("© 2026 Sugo contributors")},
    };

    QCommandLineParser parser;
    parser.setApplicationDescription(about.value(QStringLiteral("shortDescription")).toString());
    parser.addHelpOption();
    parser.addVersionOption();
#ifdef Q_OS_MACOS
    const QCommandLineOption backgroundOption(QStringLiteral("background"),
                                              i18n("Start hidden in the menu bar (used by the login item)"));
#else
    const QCommandLineOption backgroundOption(QStringLiteral("background"),
                                              i18n("Start hidden in the system tray (used by the login entry)"));
#endif
    parser.addOption(backgroundOption);
    parser.process(app);
    const bool background = parser.isSet(backgroundOption);

#ifdef Q_OS_MACOS
    // An update cut off halfway left this copy at "Sugo.app.old": it is put back once Sugo quits.
    MacBundleInstaller::restoreInterruptedUpdate(QCoreApplication::applicationDirPath(), QCoreApplication::applicationPid());
#endif

    // Closing the window must not end the program: the tray (or the close handler in QML) decides.
    app.setQuitOnLastWindowClosed(false);

    // One instance only. A second launch (login entry plus menu, say) shows the first one's
    // window and exits, so two programs never share the database. A second `--background`
    // launch (the login entry while already running) exits without popping the window up.
#ifdef Q_OS_MACOS
    // No session bus here: a lock file and a local socket, private to the user.
    LocalSingleInstance singleInstance(LocalSingleInstance::defaultName(QStringLiteral("sugo")));
    using AppSingleInstance = LocalSingleInstance;
#else
    SingleInstance singleInstance(QDBusConnection::sessionBus(), QStringLiteral("stream.sugo.app"));
    using AppSingleInstance = SingleInstance;

    // The D-Bus objects of global shortcuts, scripts and KRunner are registered before the name is
    // taken: a call that starts Sugo by D-Bus activation is delivered as soon as the name is ours,
    // and must find its object (else: UnknownObject). It waits in the event queue until
    // app.exec(); by then the objects are attached to the timer (below). Declared here, they
    // outlive the engine: they are unregistered and detached right after app.exec(). (A second
    // launch registers them too, on its own connection, which nobody calls, then exits.)
    TimerDBusObject timerDBusObject;
    auto *dbusTimer = new DBusTimerService(nullptr, nullptr, &timerDBusObject);
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/Timer"), &timerDBusObject, QDBusConnection::ExportAdaptors);
    KRunnerService krunnerService(nullptr, nullptr);
    auto *krunner = &krunnerService;
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/krunner"), krunner, QDBusConnection::ExportAllSlots);
#endif
    if (!singleInstance.acquire(!background)) {
        return EXIT_SUCCESS;
    }
#ifdef Q_OS_MACOS
    // The Keychain secrets the previous version left when it installed this one (it quit just
    // before): this version's items from now on, before anything reads them.
    {
        MacKeychainStore keychain(secretStoreService());
        SecretHandoff::takeBack(keychain, SecretHandoff::defaultPath());
    }
#endif

#ifdef Q_OS_LINUX
    // Inside the Flatpak: the data of Sugo 1.0.2 and older, whose Flatpak had another app ID
    // (io.github.mineraleyt.sugo), is copied over the first time, before anything reads ours.
    if (QFile::exists(QStringLiteral("/.flatpak-info")) && qEnvironmentVariableIsEmpty("SUGO_DATA_DIR")) {
        LegacyAppId::importFlatpakData(QDir::homePath() + QStringLiteral("/.var/app/") + LegacyAppId::id(),
                                       QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation),
                                       QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation));
    }
#endif

    // Lifetimes: objects are destroyed in reverse order of declaration. The database, the
    // repositories and the notification inhibitor are declared before the QML engine (whose
    // singletons, e.g. SilenceController and StatsModel, use them), and the helpers parented to
    // `services` after it, so each object goes away before what it points to.

    // Storage. If the file cannot be used, run with a throw-away in-memory database so the
    // timer still works; nothing is saved in that case (the UI shows a warning).
    auto database = std::make_unique<Database>(Database::defaultPath());
    // An empty database next to the one Sugo 1.0.1 to 1.0.x moved aside: take that one back.
    const bool databaseOpened = database->open() && database->recoverSugo100Database();
    if (!database->recoveredFromPath().isEmpty()) {
        qInfo() << "Took back the timers and history of Sugo 1.0.0 from" << database->recoveredFromPath()
                << "; the empty database that replaced it is kept as" << database->replacedPath();
    }
    // Kept apart: a file can be moved aside and the new one still fail to open.
    const QString movedAsidePath = database->movedAsidePath();
    const bool movedAsideWasCorrupt = database->movedAsideWasCorrupt();
    if (!databaseOpened) {
        qWarning() << "Cannot open the database" << Database::defaultPath() << ":" << database->lastError();
        qWarning() << "Using a temporary in-memory database: timers and statistics will NOT be saved";
        database = std::make_unique<Database>(QStringLiteral(":memory:"));
        if (!database->open()) {
            qCritical() << "Cannot open even an in-memory database:" << database->lastError();
            return EXIT_FAILURE;
        }
    }
    if (!movedAsidePath.isEmpty()) {
        if (movedAsideWasCorrupt) {
            qWarning() << "The existing database was damaged; it was kept as" << movedAsidePath
                       << "and a new one was started";
        } else {
            qWarning() << "The existing database was not created by Sugo; it was kept as"
                       << movedAsidePath << "and a new one was started";
        }
    }
    PresetRepository presetRepository(database->connectionName());
    SessionRepository sessionRepository(database->connectionName());
    const auto inhibitor = createNotificationInhibitor();

    // QML warnings of a smoke run, counted by a handler of the engine: declared before it, so
    // the handler's reference stays valid while the engine and its window are destroyed.
    int smokeWarnings = 0;
    QQmlApplicationEngine engine;
    QObject services; // parent of the C++ helpers below; destroyed before the engine
    // i18n() and friends for the QML files.
    engine.rootContext()->setContextObject(new LocalizedContext(&engine));
#ifdef Q_OS_MACOS
    // No freedesktop icon theme here: the names Qt's own Apple icons do not cover come from
    // the Breeze subset bundled in the program.
    ThemeIcons::installFallbackTheme();
#endif
    // Icons of the desktop's theme, as image://icon/<name> (the engine owns the provider).
    engine.addImageProvider(QStringLiteral("icon"), new ThemeIconProvider);
    engine.rootContext()->setContextProperty(QStringLiteral("appAboutData"), about);
    const QString smokePage = qEnvironmentVariable("SUGO_SMOKE_PAGE");
    const bool statsSmokeTest = qEnvironmentVariable("SUGO_STATS_SMOKE_TEST") == QStringLiteral("1");
    engine.rootContext()->setContextProperty(QStringLiteral("appStatsSmokeTest"), statsSmokeTest);
    engine.rootContext()->setContextProperty(QStringLiteral("appSmokePage"), smokePage);
    // Input QML cannot make itself (a wheel event), for the smoke checks only.
    SmokeInput smokeInput;
    engine.rootContext()->setContextProperty(QStringLiteral("appSmokeInput"),
                                             statsSmokeTest || !smokePage.isEmpty() ? &smokeInput : nullptr);
    // For development and screenshots: open on another page ("presets" for the timer in use, "stats",
    // "settings", "about" (the settings' About), "welcome"), or on a part of one ("settings/focus",
    // "welcome/2").
    engine.rootContext()->setContextProperty(QStringLiteral("appStartPage"), qEnvironmentVariable("SUGO_START_PAGE"));
    const bool smokeTest = statsSmokeTest || !smokePage.isEmpty();
    if (smokeTest) {
        // Any QML warning (a failed binding, an undefined property...) fails the smoke test.
        QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app, [&smokeWarnings](const QList<QQmlError> &warnings) {
            for (const QQmlError &warning : warnings) {
                qWarning().noquote() << warning.toString();
            }
            smokeWarnings += int(warnings.size());
        });
    }

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);

    // Services that must outlive any page. They are wired before the UI loads so the
    // first frame already shows the stored timers. Order matters: load presets first,
    // then configure the engine from them, then start recording.
    auto *timer = engine.singletonInstance<TimerEngine *>("stream.sugo.app", "TimerEngine");
    auto *presets = engine.singletonInstance<PresetModel *>("stream.sugo.app", "PresetModel");
    auto *settings = engine.singletonInstance<AppSettings *>("stream.sugo.app", "AppSettings");
    if (!timer || !presets || !settings) {
        qCritical() << "Cannot create the QML singletons";
        return EXIT_FAILURE;
    }
    settings->setLaunchedInBackground(background);
    settings->applyColorScheme();
    auto *presetPersistence = new PresetPersistence(presets, &presetRepository, &services);
    new TimerBinding(timer, presets, &services);
    auto *silence = engine.singletonInstance<SilenceController *>("stream.sugo.app", "SilenceController");
    if (!silence) {
        qCritical() << "Cannot create the QML singletons";
        return EXIT_FAILURE;
    }
#ifdef Q_OS_MACOS
    // The Focus shortcuts the user chose in the settings; their list is read again whenever Sugo
    // comes to the front (the user may just have created them in the Shortcuts app). Never in a
    // smoke run: it starts the timer, and must not run the user's real shortcuts.
    auto *focusShortcuts = qobject_cast<ShortcutsFocusInhibitor *>(inhibitor.get());
    if (focusShortcuts && !smokeTest) {
        settings->attachFocusShortcuts(focusShortcuts);
        // "Install Shortcuts" in the settings and the welcome tour (never in a smoke run either).
        settings->attachFocusShortcutsInstaller(new ShortcutsFocusInstaller(&services));
        QObject::connect(&app, &QGuiApplication::applicationStateChanged, focusShortcuts, [focusShortcuts](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive) {
                focusShortcuts->refreshShortcutList();
            }
        });
    }
#endif
    silence->attach(timer, presets, inhibitor.get());
    // Global shortcuts, scripts and KRunner: only while the settings allow other programs to control the timer.
    const auto externalControlAllowed = [settings]() { return settings->allowExternalControl(); };
#ifndef Q_OS_MACOS
    dbusTimer->setControlAllowed(externalControlAllowed);
    dbusTimer->attach(timer, presets);
    krunner->setControlAllowed(externalControlAllowed);
    krunner->attach(timer, presets);
#endif
    auto *recorder = new SessionRecorder(timer, presets, &sessionRepository, &services);

    auto *stats = engine.singletonInstance<StatsModel *>("stream.sugo.app", "StatsModel");
    if (!stats) {
        qCritical() << "Cannot create the StatsModel singleton";
        return EXIT_FAILURE;
    }
    stats->attach(&sessionRepository, recorder);
    auto *dataManager = new DataManager(database.get(), &sessionRepository, &presetRepository, presets, stats, &services);
    dataManager->setMovedAsideDatabase(movedAsidePath, movedAsideWasCorrupt);
    dataManager->setSessionRecorder(recorder);
    dataManager->setPresetPersistence(presetPersistence);
    dataManager->setTimerActiveProvider([timer]() { return timer->isActive(); });
    engine.rootContext()->setContextProperty(QStringLiteral("DataManager"), dataManager);

    // The AppImage and the macOS app look for updates by themselves (when the user lets them) and install them.
    auto *updates = engine.singletonInstance<UpdateChecker *>("stream.sugo.app", "UpdateChecker");
    if (updates && !smokeTest) {
        updates->attach(settings);
    }

#ifdef Q_OS_MACOS
    auto *powerInhibitor = new PowerInhibitor(&services);
#else
    auto *powerInhibitor = new PowerInhibitor(QDBusConnection::sessionBus(), &services);
#endif
    powerInhibitor->attach(timer, settings);
    auto *screenLockWatcher = engine.singletonInstance<ScreenLockWatcher *>("stream.sugo.app", "ScreenLockWatcher");
    if (screenLockWatcher) {
        screenLockWatcher->attach(timer, settings);
    }
    auto *diagnostics = engine.singletonInstance<Diagnostics *>("stream.sugo.app", "Diagnostics");
    if (diagnostics) {
        diagnostics->attach(settings, dataManager);
    }
    auto *calendarSync = engine.singletonInstance<CalendarSync *>("stream.sugo.app", "CalendarSync");
    if (calendarSync) {
        QObject::connect(recorder, &SessionRecorder::workSessionRecorded, calendarSync, &CalendarSync::enqueueWorkSession);
        QObject::connect(dataManager, &DataManager::sessionNoteChanged, calendarSync, &CalendarSync::onNoteChanged);
        // The calendar login and server never appear in a diagnostics report.
        const auto updateRedactions = [calendarSync]() {
            Diagnostics::setAccountRedactions({calendarSync->username(), QUrl(calendarSync->serverUrl()).host(),
                                               QUrl(calendarSync->calendarUrl()).host()});
        };
        updateRedactions();
        for (const auto signal : {&CalendarSync::usernameChanged, &CalendarSync::serverUrlChanged, &CalendarSync::calendarUrlChanged}) {
            QObject::connect(calendarSync, signal, calendarSync, updateRedactions);
        }
    }
    // Sugo Sync of the timers. Not in a smoke run: there it stays Off and never reads the keyring.
    auto *presetSync = engine.singletonInstance<SyncController *>("stream.sugo.app", "SyncController");
    if (presetSync && !smokeTest) {
        presetSync->setDesktopActions([](const QUrl &url) { return QDesktopServices::openUrl(url); },
                                      [](const QString &text) { QGuiApplication::clipboard()->setText(text); });
        presetSync->attach(presets, presetPersistence, &presetRepository);
        // A timer deleted elsewhere is not taken away from a running phase.
        presetSync->setTimerBusy([timer]() { return !timer->isIdle(); });
        QObject::connect(timer, &TimerEngine::stateChanged, presetSync, [timer, presetSync]() {
            if (timer->isIdle()) {
                presetSync->timerIdle();
            }
        });
        QObject::connect(&app, &QGuiApplication::applicationStateChanged, presetSync, [presetSync](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive) {
                presetSync->windowActivated();
            } else {
                presetSync->windowDeactivated();
            }
        });
    }
    // The diagnostics report says how Sugo Sync is doing (never who, or with what keys).
    if (diagnostics && presetSync) {
        diagnostics->attachSync(presetSync);
    }
    auto *focusSound = new FocusSoundController(&services);
    focusSound->attach(timer, settings);
    // Anonymous usage statistics, only while the user lets them (off by default); never in a smoke
    // run (there it exists for the pages, never turned on). They go to Sugo's server (the one of
    // Sugo Sync), which forwards them to PostHog.
    auto *telemetryTransport =
        new HttpTelemetryTransport(SyncApi::defaultBaseUrl().resolved(QUrl(QStringLiteral("/v1/telemetry"))), &services);
    auto *telemetry = new Telemetry(Settings::openDefault(), telemetryTransport, &services);
    engine.rootContext()->setContextProperty(QStringLiteral("Telemetry"), telemetry);
    if (!smokeTest) {
        telemetry->setStartProperties([settings, presets, calendarSync, presetSync, databaseOpened]() {
            const QString version = TelemetryInfo::version();
            QJsonObject properties{
                {QStringLiteral("version"), version},
                {QStringLiteral("channel"), TelemetryInfo::channel(version)},
                {QStringLiteral("os"), TelemetryInfo::os()},
                {QStringLiteral("packaging"), TelemetryInfo::packaging()},
                {QStringLiteral("desktop"), TelemetryInfo::desktop()},
                {QStringLiteral("distro"), TelemetryInfo::distro()},
                {QStringLiteral("presets"), TelemetryInfo::presetsBand(presets->count())},
                {QStringLiteral("calendar_sync"), calendarSync && calendarSync->enabled()},
                {QStringLiteral("focus_sound"), settings->focusSoundMode() != 0},
                {QStringLiteral("tray_icon"), settings->trayAvailable() && settings->showTrayIcon()},
                {QStringLiteral("start_at_login"), settings->startAtLogin()},
                {QStringLiteral("sugo_sync"), presetSync && presetSync->state() != SyncController::Off},
                {QStringLiteral("keep_screen_awake"), settings->keepScreenAwake()},
                {QStringLiteral("color_scheme"), settings->colorScheme()},
                {QStringLiteral("auto_pause_on_lock"), settings->autoPauseOnScreenLock()},
                {QStringLiteral("pre_alarm"), settings->preAlarmEnabled()},
                {QStringLiteral("task_notes"), settings->promptTaskNote()},
                {QStringLiteral("external_control"), settings->externalControlSupported() && settings->allowExternalControl()},
                {QStringLiteral("storage"), databaseOpened ? QStringLiteral("file") : QStringLiteral("memory")},
            };
            const QString qt = TelemetryInfo::qtVersion();
            if (!qt.isEmpty()) {
                properties.insert(QStringLiteral("qt"), qt);
            }
            return properties;
        });
        telemetry->setEnabled(settings->shareUsageStats());
        // Turned on before the welcome tour was finished: from the tour.
        QObject::connect(settings, &AppSettings::shareUsageStatsChanged, telemetry, [settings, telemetry]() {
            telemetry->setEnabled(settings->shareUsageStats(),
                                  settings->firstRunCompleted() ? QStringLiteral("settings") : QStringLiteral("welcome"));
        });
        QObject::connect(timer, &TimerEngine::sessionEnded, telemetry,
                         [telemetry](TimerEngine::Phase phase, const QDateTime &, const QDateTime &, qint64, qint64 plannedMs, bool completed) {
                             telemetry->recordSession(phase, plannedMs, completed);
                         });
        QObject::connect(timer, &TimerEngine::phaseExtended, telemetry, &Telemetry::recordExtend);
        QObject::connect(timer, &TimerEngine::phaseReopened, telemetry, &Telemetry::skipNextSession);
        if (updates) {
            QObject::connect(updates, &UpdateChecker::installFinished, telemetry, &Telemetry::recordUpdateInstall);
        }
        if (calendarSync) {
            QObject::connect(calendarSync, &CalendarSync::sendFailed, telemetry, [telemetry](const QString &kind) {
                telemetry->recordSyncError(QStringLiteral("calendar"), kind);
            });
        }
        if (presetSync) {
            QObject::connect(presetSync, &SyncController::stateChanged, telemetry, [presetSync, telemetry]() {
                const bool failed = presetSync->state() == SyncController::Error
                    || (presetSync->state() == SyncController::Off && presetSync->errorKind() == SyncController::SignedOutRemotely);
                if (!failed) {
                    return;
                }
                static const QHash<int, QString> kinds = {
                    {SyncController::Network, QStringLiteral("network")},
                    {SyncController::SignedOutRemotely, QStringLiteral("signed_out")},
                    {SyncController::FormatTooNew, QStringLiteral("format_too_new")},
                    {SyncController::UnreadableData, QStringLiteral("unreadable_data")},
                    {SyncController::TooLarge, QStringLiteral("too_large")},
                    {SyncController::KeyringUnavailable, QStringLiteral("keyring_unavailable")},
                    {SyncController::KeyringWriteFailed, QStringLiteral("keyring_write_failed")},
                    {SyncController::RateLimited, QStringLiteral("rate_limited")},
                    {SyncController::Server, QStringLiteral("server")},
                    {SyncController::LocalSaveFailed, QStringLiteral("local_save_failed")},
                };
                telemetry->recordSyncError(QStringLiteral("sugo_sync"), kinds.value(presetSync->errorKind()));
            });
        }
    }
    engine.rootContext()->setContextProperty(QStringLiteral("FocusSoundController"), focusSound);
    stats->setProtectWeekendStreak(settings->protectWeekendStreak());
    QObject::connect(settings, &AppSettings::protectWeekendStreakChanged, stats, [settings, stats]() {
        stats->setProtectWeekendStreak(settings->protectWeekendStreak());
    });
    // The running-timer badge (menu bar icon and its pictures in the interface) is painted in
    // the accent colour, of which TrayIconStyle holds the shades.
    auto *trayStyle = engine.singletonInstance<TrayIconStyle *>(QStringLiteral("stream.sugo.app"), QStringLiteral("TrayIconStyle"));
    trayStyle->setAccentColor(settings->accentColor());
    QObject::connect(settings, &AppSettings::accentColorChanged, trayStyle, [settings, trayStyle]() {
        trayStyle->setAccentColor(settings->accentColor());
    });
    auto *tray = new TrayController(timer, presets, settings, &services);
    // The inventory (platform parity test) lists the tray's settings too, which an offscreen run
    // would hide: no tray there.
    if (smokePage == QLatin1String("inventory")) {
        settings->setTrayAvailable(true);
    }
#ifdef Q_OS_MACOS
    // The Dock icon: minutes left on its badge, the timer in its menu, hidden on request.
    auto *dock = new DockController(timer, presets, settings, DockController::platformNative(), &services);
#endif
    // Desktop notifications; where there is no notification server, messages from the tray icon.
    auto *notifications = createNotifier([tray]() { return tray->icon(); }, &services).release();
    auto *notifier = new PhaseNotifier(timer, presets, inhibitor.get(), notifications, settings, &services);
    notifier->setLastWorkSessionIdProvider([recorder]() { return recorder->lastWorkSessionId(); });
    // Direct inline reply from the notification.
    QObject::connect(notifier, &PhaseNotifier::taskNoteSubmitted, dataManager, [dataManager](qint64 sessionId, const QString &note) {
        if (sessionId > 0) {
            dataManager->updateSessionNote(sessionId, note);
        }
    });

    // The session under way is kept when Sugo ends, and comes back paused at the next start.
    auto *keeper = engine.singletonInstance<SessionKeeper *>("stream.sugo.app", "SessionKeeper");
    if (keeper) {
        keeper->attach(timer, presets, recorder, SessionKeeper::defaultPath());
        QObject::connect(keeper, &SessionKeeper::restoreEnded, telemetry, &Telemetry::recordRestore);
        keeper->restore();
    }

    // Quitting mid-phase keeps the session for the next start (or, when it cannot be kept, ends
    // it as an interrupted session instead of losing it). Then the sessions the database refused
    // get one last try (with the timer idle, nothing else would retry them).
    SessionRecorder::connectQuit(&app, timer, recorder, [keeper]() { return keeper && keeper->keepForQuit(); });
    // ... and always hands the notifications back before the process ends.
    QObject::connect(&app, &QCoreApplication::aboutToQuit, silence, &SilenceController::shutdown);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, powerInhibitor, &PowerInhibitor::shutdown);
    installQuitSignalHandlers(&app);

    engine.loadFromModule("stream.sugo.app", "Main");

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
    // For development and for the screenshots of the store pages: save a picture of the window
    // (SUGO_SCREENSHOT=file.png, after SUGO_SCREENSHOT_DELAY ms, 1500 by default) and quit.
    if (window && qEnvironmentVariableIsSet("SUGO_SCREENSHOT")) {
        const int delay = qEnvironmentVariableIsSet("SUGO_SCREENSHOT_DELAY") ? qEnvironmentVariableIntValue("SUGO_SCREENSHOT_DELAY") : 1500;
        QTimer::singleShot(delay, window, [window]() {
            const QString file = qEnvironmentVariable("SUGO_SCREENSHOT");
            if (!window->grabWindow().save(file)) {
                qWarning() << "Cannot save the screenshot to" << file;
            }
            QCoreApplication::quit();
        });
    }
    if (window) {
        tray->setWindow(window);
#ifdef Q_OS_MACOS
        macSetUpTitleBar(window);
        dock->setWindow(window);
        auto activateWindow = [window, dock](const QString &activationToken = QString()) {
#else
        auto activateWindow = [window](const QString &activationToken = QString()) {
#endif
#ifdef Q_OS_LINUX
            // On Wayland the compositor only gives focus with the token of the launch that asked
            // for it; Qt's Wayland plugin takes it from this variable on requestActivate().
            if (!activationToken.isEmpty()) {
                qputenv("XDG_ACTIVATION_TOKEN", activationToken.toUtf8());
            }
#else
            Q_UNUSED(activationToken)
#endif
#ifdef Q_OS_MACOS
            // A hidden Dock icon comes back first: an accessory application gets no focus or menu bar.
            dock->prepareToShowWindow();
            // requestActivate() alone would leave the window behind the active application.
            macActivateApp();
#endif
            // Back from the taskbar or the Dock as it was: a maximised window stays maximised.
            DesktopStyle::showRestored(window);
            window->raise();
            window->requestActivate();
        };

        // A click on the tray icon while the window is hidden.
        QObject::connect(tray, &TrayController::showWindowRequested, window, [activateWindow]() { activateWindow(); });

        // A second launch asks this instance to come to the front.
        QObject::connect(&singleInstance, &AppSingleInstance::activateRequested, window, activateWindow);

#ifdef Q_OS_MACOS
        // sugo:// links: the same commands as D-Bus on Linux, under the same setting. A refused
        // or failed command shows the window with the reason, so it does not fail silently.
        auto urlCommands = std::make_shared<TimerCommands>(timer, presets);
        urlCommands->setControlAllowed(externalControlAllowed);
        const auto openUrl = [urlCommands, window, activateWindow](const QUrl &url) {
            const TimerCommands::UrlCommand command = TimerCommands::parseUrl(url);
            const TimerCommands::Result result = urlCommands->run(command);
            if (result.ok() && command.action != TimerCommands::UrlCommand::Action::Show) {
                return;
            }
            if (!result.ok()) {
                qWarning().noquote() << "Cannot follow" << url.toDisplayString() << ":" << result.message;
            }
            activateWindow();
            if (!result.ok()) {
                QMetaObject::invokeMethod(window, "showPassiveNotification", Q_ARG(QString, result.message), Q_ARG(QVariant, QVariant()));
            }
        };
        // The Dock icon, links, and quitting from the system (see MacApplicationEvents).
        auto *macEvents = new MacApplicationEvents(window, timer, [activateWindow]() { activateWindow(); }, openUrl, !background,
                                                   &services);
        app.installEventFilter(macEvents);
        if (background) {
            QTimer::singleShot(1000, macEvents, [macEvents]() { macEvents->arm(); });
        }
#endif

#ifndef Q_OS_MACOS
        // KRunner runner action to open window
        QObject::connect(krunner, &KRunnerService::openRequested, window, [activateWindow]() { activateWindow(); });
#endif

        // The tray menu's Statistics / Settings entries: show the window on that page.
        QObject::connect(tray, &TrayController::openPageRequested, window, [window, activateWindow](const QString &page) {
            activateWindow();
            QMetaObject::invokeMethod(window, "openPage", Q_ARG(QString, page));
        });

        // The tray menu's Quit: the window's own, which asks first while a phase runs; the window
        // comes to the front then, so the question is seen.
        QObject::connect(tray, &TrayController::quitRequested, window, [window, timer, activateWindow]() {
            if (timer->isActive()) {
                activateWindow();
            }
            QMetaObject::invokeMethod(window, "requestQuit");
        });

        // Stop and Skip in the tray and Dock menus: the window's own, which ends the phase at once
        // unless work under way would be cut short; then it asks, and comes to the front (also
        // from the tray, the window closed) so the question is seen.
        const auto requestEndPhase = [window, activateWindow](bool skipping) {
            bool asking = false;
            if (!QMetaObject::invokeMethod(window, "requestEndPhase", Q_RETURN_ARG(bool, asking), Q_ARG(bool, skipping))) {
                qWarning() << "Cannot ask the window to end the phase";
                return;
            }
            if (asking) {
                activateWindow();
            }
        };
        QObject::connect(tray, &TrayController::endPhaseRequested, window, requestEndPhase);
#ifdef Q_OS_MACOS
        QObject::connect(dock, &DockController::endPhaseRequested, window, requestEndPhase);
#endif

        // A click on a notification with nothing more specific to do.
        QObject::connect(notifier, &PhaseNotifier::showWindowRequested, window, activateWindow);

        // A session brought back while Sugo starts hidden (at login): the focus page that says so
        // is not on screen, so a notification does.
        if (keeper && keeper->isRestored() && settings->startHidden()) {
            const int minutes = (timer->remainingSeconds() + 59) / 60;
            Notifier::Notification restored;
            restored.title = timer->phase() == TimerEngine::Phase::Work ? i18n("Work session paused") : i18n("Break paused");
            restored.text = i18np("Sugo was closed during this session. %1 minute is left: resume it when you are ready.",
                                  "Sugo was closed during this session. %1 minutes are left: resume it when you are ready.",
                                  minutes);
            restored.iconName = QStringLiteral("stream.sugo.app");
            restored.actions.append({i18n("Resume"), [timer](const QString &) {
                                         if (timer->isPaused()) {
                                             timer->resume();
                                         }
                                     }});
            restored.defaultActionLabel = i18n("View");
            restored.defaultAction = activateWindow;
            notifications->notify(keeper, restored);
        }

        // Sugo Sync: a device asking for approval while the window is hidden is notified; a click
        // shows the window on the Sync settings, where the approval dialog waits.
        if (presetSync && !smokeTest) {
            const QPointer<QQuickWindow> shown(window);
            presetSync->attachNotifier(notifications, [shown]() {
                return shown && shown->isVisible() && !(shown->windowStates() & Qt::WindowMinimized);
            });
            QObject::connect(presetSync, &SyncController::showSyncPageRequested, window,
                             [window, activateWindow](const QString &activationToken) {
                                 activateWindow(activationToken);
                                 QMetaObject::invokeMethod(window, "openSettings", Q_ARG(QString, QStringLiteral("sync")));
                             });
        }

        // When a task note prompt is requested (e.g. from end-of-work notification action)
        QObject::connect(notifier, &PhaseNotifier::taskNotePromptRequested, window,
                         [window, activateWindow](const QString &presetName, qint64 sessionId, const QString &activationToken) {
                             activateWindow(activationToken);
                             QMetaObject::invokeMethod(window, "openTaskNotePrompt", Q_ARG(QVariant, presetName),
                                                       Q_ARG(QVariant, sessionId));
                         });
    } else {
        // No window to ask from.
        QObject::connect(tray, &TrayController::quitRequested, &app, &QCoreApplication::quit);
        QObject::connect(tray, &TrayController::endPhaseRequested, timer, [timer](bool skipping) {
            skipping ? timer->skip() : timer->stop();
        });
#ifdef Q_OS_MACOS
        QObject::connect(dock, &DockController::endPhaseRequested, timer, [timer](bool skipping) {
            skipping ? timer->skip() : timer->stop();
        });
#endif
    }

    const int status = app.exec();

    // Teardown. Nothing outside may reach what is about to go: first the D-Bus objects, which
    // outlive the timer (an engine singleton) and so let go of it too, in case a call already
    // queued for them is still delivered; then the context properties
    // that point into `services`, which is destroyed before the engine and its window.
#ifndef Q_OS_MACOS
    QDBusConnection::sessionBus().unregisterObject(QStringLiteral("/krunner"));
    QDBusConnection::sessionBus().unregisterObject(QStringLiteral("/Timer"));
    krunner->attach(nullptr, nullptr);
    dbusTimer->attach(nullptr, nullptr);
#endif
    engine.rootContext()->setContextProperty(QStringLiteral("DataManager"), QVariant::fromValue<QObject *>(nullptr));
    if (presetSync) {
        presetSync->attach(nullptr, nullptr, nullptr); // its persistence goes with `services`
        presetSync->attachNotifier(nullptr, {});        // and so do the notifications
    }
    engine.rootContext()->setContextProperty(QStringLiteral("FocusSoundController"), QVariant::fromValue<QObject *>(nullptr));
    if (smokeTest && status == EXIT_SUCCESS && smokeWarnings > 0) {
        qCritical() << "The smoke test produced" << smokeWarnings << "QML warning(s)";
        return 30;
    }
#ifdef Q_OS_MACOS
    // The next start is another build, which could not read this one's Keychain items without
    // macOS asking the user: it takes the secrets back from a file (SecretHandoff). If the new
    // Sugo.app does not get in place after all, this build takes them back itself.
    if (updates && updates->status() == UpdateChecker::Status::ReadyToRestart) {
        MacKeychainStore keychain(secretStoreService());
        SecretHandoff::handOver(keychain, SecretHandoff::defaultPath());
    }
#endif
    // "Restart to update": the updated AppImage, or the new Sugo.app put in place now, starts once
    // this process has ended (on macOS a plain quit also puts it in place).
    if (updates) {
        updates->launchUpdated();
    }
    return status;
}
