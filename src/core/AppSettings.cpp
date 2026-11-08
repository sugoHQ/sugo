// SPDX-License-Identifier: GPL-3.0-or-later
#include "AppSettings.h"

#include "Autostart.h"
#include "FocusSoundController.h"
#include "ShortcutsFocusInhibitor.h"
#include "ShortcutsFocusInstaller.h"
#include "SystemTray.h"

#include <QAudioOutput>
#include <QDebug>
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusServiceWatcher>
#endif
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QMediaPlayer>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTimer>
#include <QUrl>
#include "Localization.h"
#include "Settings.h"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>


namespace
{
const char kShowTrayIcon[] = "ShowTrayIcon";
const char kCloseToTray[] = "CloseToTray";
const char kAllowExternalControl[] = "AllowExternalControl";
const char kStartMinimized[] = "StartMinimized";
const char kShowDockBadge[] = "ShowDockBadge";
const char kHideDockIcon[] = "HideDockIcon";
const char kTrayBadgeStyle[] = "TrayBadgeStyle";
const char kSoundTheme[] = "SoundTheme";
const char kSoundVolume[] = "SoundVolume";
const char kPreAlarmEnabled[] = "PreAlarmEnabled";
const char kPreAlarmSound[] = "PreAlarmSound";
const char kFocusSoundMode[] = "FocusSoundMode";
const char kFocusSoundVolume[] = "FocusSoundVolume";
const char kKeepScreenAwake[] = "KeepScreenAwake";
const char kAutoPauseOnScreenLock[] = "AutoPauseOnScreenLock";
const char kProtectWeekendStreak[] = "ProtectWeekendStreak";
const char kFirstRunCompleted[] = "FirstRunCompleted";
const char kPromptTaskNote[] = "PromptTaskNote";
const char kColorScheme[] = "ColorScheme";
const char kAccentColor[] = "AccentColor";
const char kUpdateChannel[] = "UpdateChannel";
const char kAutoCheckUpdates[] = "AutoCheckUpdates";
const char kShareUsageStats[] = "ShareUsageStats";
const char kVerboseLogging[] = "VerboseLogging";
const char kFocusOnShortcut[] = "FocusOnShortcut";
const char kFocusOffShortcut[] = "FocusOffShortcut";

QString validColorScheme(const QString &scheme)
{
    const QString s = scheme.trimmed().toLower();
    if (s == QLatin1String("light") || s == QLatin1String("dark")) {
        return s;
    }
    // Follow the desktop's light or dark mode unless told otherwise.
    return QStringLiteral("system");
}

QString validAccentColor(const QString &id)
{
    const QString s = id.trimmed().toLower();
    if (AppSettings::accentColors().contains(s)) {
        return s;
    }
    return QStringLiteral("red");
}

/// An integer setting, or `fallback` when it is missing, empty or not a number: a hand-edited or
/// half-written "SoundVolume=" must not read as 0 and mute every alert.
int intSetting(const QSettings &settings, const char *key, int fallback)
{
    bool ok = false;
    const int value = settings.value(QLatin1String(key), fallback).toInt(&ok);
    return ok ? value : fallback;
}

constexpr qint64 kAlertFadeInMs = 300;
constexpr qint64 kAlertFadeOutMs = 600;
/// After the player stops, how long the end of the media or an error may take to be reported.
constexpr int kAlertStoppedGraceMs = 1000;
int alertPlayerLifetimeForTestingMs = 0;

/// Plays a file through Qt Multimedia, fading it in and out. False if Qt Multimedia has no
/// backend; a file it cannot decode falls back to AppSettings::playWithPlayerProgram().
bool playWithFade(const QString &file, int vol, QObject *parent);

#ifndef Q_OS_MACOS
/// A player program that exits with an error sooner than this after starting did not play the
/// sound (pw-play with no PipeWire daemon, a file it cannot decode): the next one plays it instead.
constexpr qint64 kQuickPlayerFailureMs = 2000;

/// Player programs that cannot play here, not tried again in this session: those that failed to
/// start, and those that failed at once on a file the next player then played. One that only
/// failed on a file no player could play (a broken file) keeps its chances.
QStringList &unusableAlertPlayers()
{
    static QStringList players;
    return players;
}

/// Plays a file with the first usable player program from `first` on: pw-play, then paplay.
/// failedHere: the players that failed at once on this file before.
void playWithPlayerFrom(int first, const QString &file, int vol, const QStringList &failedHere = {})
{
    const QString programs[] = {QStringLiteral("pw-play"), QStringLiteral("paplay")};
    for (int i = first; i < int(std::size(programs)); ++i) {
        const QString &program = programs[i];
        if (unusableAlertPlayers().contains(program) || QStandardPaths::findExecutable(program).isEmpty()) {
            continue;
        }
        const QStringList arguments = i == 0
            ? QStringList{QStringLiteral("--volume"), QString::number(vol / 100.0, 'f', 2), file}
            // paplay's volume is linear, 65536 = 100 %.
            : QStringList{QStringLiteral("--volume=%1").arg(vol * 65536 / 100), file};

        // Not detached, to see it fail. No parent: a sound still playing at quit plays to its end.
        auto *process = new QProcess;
        process->setProcessChannelMode(QProcess::ForwardedChannels);
        process->setStandardInputFile(QProcess::nullDevice());
        auto clock = std::make_shared<QElapsedTimer>();
        QObject::connect(process, &QProcess::errorOccurred, process, [process, i, program, file, vol, failedHere](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                process->deleteLater();
                qWarning().noquote() << program << "failed to start - trying the next player";
                unusableAlertPlayers().append(program);
                playWithPlayerFrom(i + 1, file, vol, failedHere);
            }
        });
        QObject::connect(process, &QProcess::finished, process,
                         [process, clock, i, program, file, vol, failedHere](int exitCode, QProcess::ExitStatus status) {
            process->deleteLater();
            if (status == QProcess::NormalExit && exitCode == 0) {
                // It played what those before it could not: they cannot play here.
                unusableAlertPlayers().append(failedHere);
            } else if (status == QProcess::NormalExit && clock->elapsed() < kQuickPlayerFailureMs) {
                // An error right away: it did not play this file. Later, or killed, it was playing.
                qWarning().noquote() << program << "cannot play" << file << QStringLiteral("(exit code %1)").arg(exitCode)
                                     << "- trying the next player";
                playWithPlayerFrom(i + 1, file, vol, failedHere + QStringList{program});
            }
        });
        clock->start();
        process->start(program, arguments);
        return;
    }
    qWarning() << "Cannot play" << file << "- neither pw-play nor paplay could play it";
}
#endif
} // namespace

void AppSettings::playWithPlayerProgram(const QString &file, int vol)
{
#ifdef Q_OS_MACOS
    if (AppSettings::afplayCanPlay(file)
        && QProcess::startDetached(QStringLiteral("/usr/bin/afplay"), AppSettings::afplayArguments(file, vol))) {
        return;
    }
    qWarning() << "Cannot play" << file << "- afplay could not be started or cannot read the format";
#else
    playWithPlayerFrom(0, file, vol);
#endif
}

namespace
{
bool playWithFade(const QString &file, int vol, QObject *parent)
{
    auto *player = new QMediaPlayer(parent);
    if (!player->isAvailable()) {
        delete player;
        return false;
    }
    auto *output = new QAudioOutput(player);
    output->setVolume(0.0f);
    player->setAudioOutput(output);

    auto *fade = new QTimer(player);
    fade->setInterval(20);
    QObject::connect(fade, &QTimer::timeout, player, [player, output, vol]() {
        output->setVolume(float(vol / 100.0 * AppSettings::alertFadeGain(player->position(), player->duration())));
    });
    QObject::connect(player, &QMediaPlayer::mediaStatusChanged, player, [player](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) {
            player->deleteLater();
        }
    });
    QObject::connect(player, &QMediaPlayer::errorOccurred, player, [player, file, vol]() {
        player->disconnect(); // errors can come more than once: play the fallback only once
        player->deleteLater();
        AppSettings::playWithPlayerProgram(file, vol);
    });

    // A backend that reports neither the end nor an error (a source that stalls, an audio device
    // that goes away) would keep the player, and its fade timer, until Sugo quits: one per alert.
    // It is released at the latest a little after the sound should have ended. A stopped player
    // still has a moment for the end of the media or an error (the fallback) to be reported.
    auto *expiry = new QTimer(player);
    expiry->setSingleShot(true);
    const auto lifetime = [](qint64 durationMs) {
        return alertPlayerLifetimeForTestingMs > 0 ? alertPlayerLifetimeForTestingMs : int(AppSettings::alertPlayerLifetimeMs(durationMs));
    };
    QObject::connect(expiry, &QTimer::timeout, player, [player, file]() {
        qWarning() << "The alert sound" << file << "did not end in time; its player is released";
        player->disconnect();
        player->deleteLater();
    });
    QObject::connect(player, &QMediaPlayer::durationChanged, expiry, [expiry, lifetime](qint64 durationMs) {
        expiry->start(lifetime(durationMs));
    });
    QObject::connect(player, &QMediaPlayer::playbackStateChanged, expiry, [expiry](QMediaPlayer::PlaybackState state) {
        if (state == QMediaPlayer::StoppedState && (!expiry->isActive() || expiry->remainingTime() > kAlertStoppedGraceMs)) {
            expiry->start(kAlertStoppedGraceMs);
        }
    });
    expiry->start(lifetime(0));

    player->setSource(QUrl::fromLocalFile(file));
    fade->start();
    player->play();
    return true;
}
} // namespace

AppSettings::AppSettings(QObject *parent)
    : AppSettings(Settings::openDefault(), createAutostart(), systemTrayAvailable(), parent)
{
#ifdef Q_OS_LINUX
    watchTray();
#endif
}

AppSettings::AppSettings(Settings::Ptr config, std::unique_ptr<Autostart> autostart, bool trayAvailable, QObject *parent)
    : QObject(parent)
    , m_config(std::move(config))
    , m_autostart(std::move(autostart))
    , m_trayAvailable(trayAvailable)
{
    const QSettings &settings = *m_config;
    m_showTrayIcon = settings.value(QLatin1String(kShowTrayIcon), Defaults::showTrayIcon).toBool();
    m_closeToTray = settings.value(QLatin1String(kCloseToTray), Defaults::closeToTray).toBool();
    m_allowExternalControl = settings.value(QLatin1String(kAllowExternalControl), Defaults::allowExternalControl).toBool();
    m_startMinimized = settings.value(QLatin1String(kStartMinimized), Defaults::startMinimized).toBool();
    m_showDockBadge = settings.value(QLatin1String(kShowDockBadge), Defaults::showDockBadge).toBool();
    m_hideDockIcon = settings.value(QLatin1String(kHideDockIcon), Defaults::hideDockIcon).toBool();
    m_trayBadgeStyle = std::clamp(intSetting(settings, kTrayBadgeStyle, Defaults::trayBadgeStyle), 0, 2);
    m_soundTheme = settings.value(QLatin1String(kSoundTheme), QString(Defaults::soundTheme)).toString();
    m_soundVolume = std::clamp(intSetting(settings, kSoundVolume, Defaults::soundVolume), 0, 100);
    m_preAlarmEnabled = settings.value(QLatin1String(kPreAlarmEnabled), Defaults::preAlarmEnabled).toBool();
    m_preAlarmSound = settings.value(QLatin1String(kPreAlarmSound), QString(Defaults::preAlarmSound)).toString();
    m_focusSoundMode = std::clamp(intSetting(settings, kFocusSoundMode, Defaults::focusSoundMode), 0, FocusSoundController::LastMode);
    m_focusSoundVolume = std::clamp(intSetting(settings, kFocusSoundVolume, Defaults::focusSoundVolume), 0, 100);
    m_keepScreenAwake = settings.value(QLatin1String(kKeepScreenAwake), Defaults::keepScreenAwake).toBool();
    m_autoPauseOnScreenLock = settings.value(QLatin1String(kAutoPauseOnScreenLock), Defaults::autoPauseOnScreenLock).toBool();
    m_protectWeekendStreak = settings.value(QLatin1String(kProtectWeekendStreak), Defaults::protectWeekendStreak).toBool();
    m_firstRunCompleted = settings.value(QLatin1String(kFirstRunCompleted), Defaults::firstRunCompleted).toBool();
    m_promptTaskNote = settings.value(QLatin1String(kPromptTaskNote), Defaults::promptTaskNote).toBool();
    m_colorScheme = validColorScheme(settings.value(QLatin1String(kColorScheme), QString(Defaults::colorScheme)).toString());
    applyColorScheme();
    m_accentColor = validAccentColor(settings.value(QLatin1String(kAccentColor), QString(Defaults::accentColor)).toString());
    m_updateChannel = settings.value(QLatin1String(kUpdateChannel)).toString() == QLatin1String("nightly") ? QStringLiteral("nightly") : QString(Defaults::updateChannel);
    m_autoCheckUpdates = settings.value(QLatin1String(kAutoCheckUpdates), Defaults::autoCheckUpdates).toBool();
    m_shareUsageStats = settings.value(QLatin1String(kShareUsageStats), Defaults::shareUsageStats).toBool();
    m_verboseLogging = settings.value(QLatin1String(kVerboseLogging), Defaults::verboseLogging).toBool();
    m_focusOnShortcut = settings.value(QLatin1String(kFocusOnShortcut), QString(Defaults::focusOnShortcut)).toString().trimmed();
    m_focusOffShortcut = settings.value(QLatin1String(kFocusOffShortcut), QString(Defaults::focusOffShortcut)).toString().trimmed();

    // Left behind by versions that had a daily goal.
    if (m_config->contains(QStringLiteral("DailyGoal"))) {
        m_config->remove(QStringLiteral("DailyGoal"));
        Settings::sync(*m_config);
    }

    // The program may have moved since the entry was written (new install prefix). Not in an
    // isolated run (SUGO_DATA_DIR: tests, a build tried out): the login entry is the user's, and
    // on macOS it would point at the copy being tried.
    if (m_autostart && qEnvironmentVariableIsEmpty("SUGO_DATA_DIR")) {
        m_autostart->repair();
    }
    // The entry can change after setStartAtLogin() reported a failure: the Background portal's
    // dialog answered after Sugo stopped waiting.
    if (m_autostart) {
        m_autostart->setChangedCallback([this]() {
            if (!startAtLogin()) {
                setStartMinimized(false); // as when switched off from here
            }
            Q_EMIT startAtLoginChanged();
        });
    }
}

AppSettings::~AppSettings() = default;

void AppSettings::store(const char *key, bool value)
{
    m_config->setValue(QLatin1String(key), value);
    Settings::sync(*m_config);
}

void AppSettings::storeInt(const char *key, int value)
{
    m_config->setValue(QLatin1String(key), value);
    Settings::sync(*m_config);
}

void AppSettings::storeString(const char *key, const QString &value)
{
    m_config->setValue(QLatin1String(key), value);
    Settings::sync(*m_config);
}

#ifdef Q_OS_LINUX
void AppSettings::watchTray()
{
    // The tray host can appear after us (login) or restart (plasmashell crash).
    auto *watcher = new QDBusServiceWatcher(QStringLiteral("org.kde.StatusNotifierWatcher"),
                                            QDBusConnection::sessionBus(),
                                            QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration,
                                            this);
    connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, [this]() {
        setTrayAvailable(true);
    });
    connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this]() {
        setTrayAvailable(false);
    });
}
#endif

void AppSettings::setTrayAvailable(bool available)
{
    if (m_trayAvailable == available) {
        return;
    }
    m_trayAvailable = available;
    Q_EMIT trayAvailableChanged();
    Q_EMIT keepRunningInTrayChanged();
    Q_EMIT startHiddenChanged();
}

bool AppSettings::autostartSupported() const
{
    return m_autostart && m_autostart->isSupported();
}

void AppSettings::setShowTrayIcon(bool show)
{
    if (m_showTrayIcon == show) {
        return;
    }
    m_showTrayIcon = show;
    store(kShowTrayIcon, show);
    Q_EMIT showTrayIconChanged();
    Q_EMIT keepRunningInTrayChanged();
    Q_EMIT startHiddenChanged();
}

void AppSettings::setShowDockBadge(bool show)
{
    if (m_showDockBadge == show) {
        return;
    }
    m_showDockBadge = show;
    store(kShowDockBadge, show);
    Q_EMIT showDockBadgeChanged();
}

void AppSettings::setHideDockIcon(bool hide)
{
    if (m_hideDockIcon == hide) {
        return;
    }
    m_hideDockIcon = hide;
    store(kHideDockIcon, hide);
    Q_EMIT hideDockIconChanged();
}

void AppSettings::setCloseToTray(bool close)
{
    if (m_closeToTray == close) {
        return;
    }
    m_closeToTray = close;
    store(kCloseToTray, close);
    Q_EMIT closeToTrayChanged();
    Q_EMIT keepRunningInTrayChanged();
}

void AppSettings::setAllowExternalControl(bool allow)
{
    if (m_allowExternalControl == allow) {
        return;
    }
    m_allowExternalControl = allow;
    store(kAllowExternalControl, allow);
    Q_EMIT allowExternalControlChanged();
}

bool AppSettings::startAtLogin() const
{
    return m_autostart && m_autostart->isEnabled();
}

void AppSettings::setStartAtLogin(bool enabled)
{
    if (!m_autostart) {
        return;
    }
    QString failure;
    if (enabled != startAtLogin()) {
        if (enabled && m_autostart->runsFromTemporaryLocation()) {
            // Opened from the disk image (or a copy macOS moved aside): it is gone at the next login.
            failure = i18n("To start Sugo at login, move it to the Applications folder and open it from there.");
        } else if (!m_autostart->setEnabled(enabled)) {
            failure = enabled ? i18n("Could not set up starting Sugo at login.")
                              : i18n("Could not remove the entry that starts Sugo at login.");
        }
    }
    if (failure.isEmpty() && !enabled) {
        // "Start minimized" only means something for the start at login: switched off with it,
        // it never stays on unseen behind the disabled row.
        setStartMinimized(false);
    }
    // Always announced: after a failure this makes views show the real state again. Before the
    // failure, so a view that clears its message on a change (the welcome tour) keeps this one.
    Q_EMIT startAtLoginChanged();
    if (!failure.isEmpty()) {
        Q_EMIT autostartFailed(failure);
    }
}

void AppSettings::setStartMinimized(bool minimized)
{
    if (m_startMinimized == minimized) {
        return;
    }
    m_startMinimized = minimized;
    store(kStartMinimized, minimized);
    Q_EMIT startMinimizedChanged();
    Q_EMIT startHiddenChanged();
}

void AppSettings::setTrayBadgeStyle(int style)
{
    style = std::clamp(style, 0, 2);
    if (m_trayBadgeStyle == style) {
        return;
    }
    m_trayBadgeStyle = style;
    storeInt(kTrayBadgeStyle, style);
    Q_EMIT trayBadgeStyleChanged();
}

void AppSettings::setSoundTheme(const QString &theme)
{
    if (m_soundTheme == theme) {
        return;
    }
    m_soundTheme = theme;
    storeString(kSoundTheme, theme);
    Q_EMIT soundThemeChanged();
}

void AppSettings::setSoundVolume(int volume)
{
    volume = std::clamp(volume, 0, 100);
    if (m_soundVolume == volume) {
        return;
    }
    m_soundVolume = volume;
    storeInt(kSoundVolume, volume);
    Q_EMIT soundVolumeChanged();
}

void AppSettings::setPreAlarmEnabled(bool enabled)
{
    if (m_preAlarmEnabled == enabled) {
        return;
    }
    m_preAlarmEnabled = enabled;
    store(kPreAlarmEnabled, enabled);
    Q_EMIT preAlarmEnabledChanged();
}

void AppSettings::setPreAlarmSound(const QString &sound)
{
    if (m_preAlarmSound == sound) {
        return;
    }
    m_preAlarmSound = sound;
    storeString(kPreAlarmSound, sound);
    Q_EMIT preAlarmSoundChanged();
}

void AppSettings::setFocusSoundMode(int mode)
{
    mode = std::clamp(mode, 0, FocusSoundController::LastMode);
    if (m_focusSoundMode == mode) {
        return;
    }
    m_focusSoundMode = mode;
    storeInt(kFocusSoundMode, mode);
    Q_EMIT focusSoundModeChanged();
}

void AppSettings::setFocusSoundVolume(int volume)
{
    volume = std::clamp(volume, 0, 100);
    if (m_focusSoundVolume == volume) {
        return;
    }
    m_focusSoundVolume = volume;
    storeInt(kFocusSoundVolume, volume);
    Q_EMIT focusSoundVolumeChanged();
}

void AppSettings::setKeepScreenAwake(bool awake)
{
    if (m_keepScreenAwake == awake) {
        return;
    }
    m_keepScreenAwake = awake;
    store(kKeepScreenAwake, awake);
    Q_EMIT keepScreenAwakeChanged();
}

void AppSettings::setAutoPauseOnScreenLock(bool pause)
{
    if (m_autoPauseOnScreenLock == pause) {
        return;
    }
    m_autoPauseOnScreenLock = pause;
    store(kAutoPauseOnScreenLock, pause);
    Q_EMIT autoPauseOnScreenLockChanged();
}

void AppSettings::setProtectWeekendStreak(bool protect)
{
    if (m_protectWeekendStreak == protect) {
        return;
    }
    m_protectWeekendStreak = protect;
    store(kProtectWeekendStreak, protect);
    Q_EMIT protectWeekendStreakChanged();
}

void AppSettings::setFirstRunCompleted(bool completed)
{
    if (m_firstRunCompleted == completed) {
        return;
    }
    m_firstRunCompleted = completed;
    store(kFirstRunCompleted, completed);
    Q_EMIT firstRunCompletedChanged();
}

void AppSettings::setPromptTaskNote(bool prompt)
{
    if (m_promptTaskNote == prompt) {
        return;
    }
    m_promptTaskNote = prompt;
    store(kPromptTaskNote, prompt);
    Q_EMIT promptTaskNoteChanged();
}

void AppSettings::setColorScheme(const QString &scheme)
{
    const QString valid = validColorScheme(scheme);
    if (m_colorScheme == valid) {
        return;
    }
    m_colorScheme = valid;
    storeString(kColorScheme, valid);
    applyColorScheme();
    Q_EMIT colorSchemeChanged();
}

void AppSettings::setAccentColor(const QString &id)
{
    const QString valid = validAccentColor(id);
    if (m_accentColor == valid) {
        return;
    }
    m_accentColor = valid;
    storeString(kAccentColor, valid);
    Q_EMIT accentColorChanged();
}

QStringList AppSettings::accentColors()
{
    // Around the colour wheel, so the menu reads as a spectrum.
    static const QStringList ids{
        QStringLiteral("red"), QStringLiteral("orange"), QStringLiteral("yellow"), QStringLiteral("lime"), QStringLiteral("green"),
        QStringLiteral("teal"), QStringLiteral("cyan"), QStringLiteral("blue"), QStringLiteral("violet"), QStringLiteral("pink"),
    };
    return ids;
}

QString AppSettings::accentColorName(const QString &id)
{
    // In the order of accentColors().
    const QStringList names{
        i18nc("accent colour", "Red"),  i18nc("accent colour", "Orange"), i18nc("accent colour", "Yellow"),
        i18nc("accent colour", "Lime"), i18nc("accent colour", "Green"),  i18nc("accent colour", "Teal"),
        i18nc("accent colour", "Cyan"), i18nc("accent colour", "Blue"),   i18nc("accent colour", "Violet"),
        i18nc("accent colour", "Pink"),
    };
    const qsizetype at = accentColors().indexOf(id);
    return at >= 0 && at < names.size() ? names.at(at) : id;
}

void AppSettings::setUpdateChannel(const QString &channel)
{
    const QString valid = channel == QLatin1String("nightly") ? channel : QStringLiteral("stable");
    if (m_updateChannel == valid) {
        return;
    }
    m_updateChannel = valid;
    storeString(kUpdateChannel, valid);
    Q_EMIT updateChannelChanged();
}

void AppSettings::setAutoCheckUpdates(bool enabled)
{
    if (m_autoCheckUpdates == enabled) {
        return;
    }
    m_autoCheckUpdates = enabled;
    store(kAutoCheckUpdates, enabled);
    Q_EMIT autoCheckUpdatesChanged();
}

void AppSettings::setShareUsageStats(bool enabled)
{
    if (m_shareUsageStats == enabled) {
        return;
    }
    m_shareUsageStats = enabled;
    store(kShareUsageStats, enabled);
    Q_EMIT shareUsageStatsChanged();
}

void AppSettings::setVerboseLogging(bool verbose)
{
    if (m_verboseLogging == verbose) {
        return;
    }
    m_verboseLogging = verbose;
    store(kVerboseLogging, verbose);
    Q_EMIT verboseLoggingChanged();
}

void AppSettings::setFocusOnShortcut(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (m_focusOnShortcut == trimmed) {
        return;
    }
    m_focusOnShortcut = trimmed;
    storeString(kFocusOnShortcut, trimmed);
    Q_EMIT focusOnShortcutChanged();
}

void AppSettings::setFocusOffShortcut(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (m_focusOffShortcut == trimmed) {
        return;
    }
    m_focusOffShortcut = trimmed;
    storeString(kFocusOffShortcut, trimmed);
    Q_EMIT focusOffShortcutChanged();
}

bool AppSettings::focusShortcutsSupported() const
{
    return m_focusInhibitor && m_focusInhibitor->toolFound();
}

QStringList AppSettings::focusShortcutNames() const
{
    return m_focusInhibitor ? m_focusInhibitor->shortcutList() : QStringList();
}

void AppSettings::refreshFocusShortcuts()
{
    if (m_focusInhibitor) {
        m_focusInhibitor->refreshShortcutList();
    }
}

void AppSettings::attachFocusShortcuts(ShortcutsFocusInhibitor *inhibitor)
{
    Q_ASSERT_X(!m_focusInhibitor, "AppSettings::attachFocusShortcuts", "attach once");
    m_focusInhibitor = inhibitor;
    const auto pushNames = [this]() {
        if (m_focusInhibitor) {
            m_focusInhibitor->setShortcutNames(m_focusOnShortcut, m_focusOffShortcut);
        }
    };
    connect(this, &AppSettings::focusOnShortcutChanged, inhibitor, pushNames);
    connect(this, &AppSettings::focusOffShortcutChanged, inhibitor, pushNames);
    connect(inhibitor, &ShortcutsFocusInhibitor::shortcutListChanged, this, &AppSettings::focusShortcutNamesChanged);
    pushNames(); // also reads the list of shortcuts
    Q_EMIT focusShortcutNamesChanged();
}

void AppSettings::attachFocusShortcutsInstaller(ShortcutsFocusInstaller *installer)
{
    Q_ASSERT_X(!m_focusInstaller, "AppSettings::attachFocusShortcutsInstaller", "attach once");
    m_focusInstaller = installer;
    connect(installer, &ShortcutsFocusInstaller::stateChanged, this, [this](ShortcutsFocusInstaller::State state) {
        switch (state) {
        case ShortcutsFocusInstaller::State::Idle:
            m_focusInstallStatus.clear();
            break;
        case ShortcutsFocusInstaller::State::Installing:
            m_focusInstallStatus = QStringLiteral("installing");
            break;
        case ShortcutsFocusInstaller::State::WaitingForOn:
            m_focusInstallStatus = QStringLiteral("waitingOn");
            break;
        case ShortcutsFocusInstaller::State::WaitingForOff:
            m_focusInstallStatus = QStringLiteral("waitingOff");
            break;
        case ShortcutsFocusInstaller::State::Done:
            m_focusInstallStatus = QStringLiteral("done");
            // The inhibitor (and the pages) learn the shortcuts are there.
            refreshFocusShortcuts();
            break;
        case ShortcutsFocusInstaller::State::Failed:
            m_focusInstallStatus = QStringLiteral("failed");
            break;
        }
        m_focusInstallDetail = state == ShortcutsFocusInstaller::State::Failed ? m_focusInstaller->message() : QString();
        Q_EMIT focusShortcutsInstallChanged();
    });
    Q_EMIT focusShortcutsInstallChanged();
}

void AppSettings::installFocusShortcuts()
{
    if (!m_focusInstaller) {
        m_focusInstallStatus = QStringLiteral("failed");
        m_focusInstallDetail = i18nc("why the Focus shortcuts could not be installed", "not supported on this system");
        Q_EMIT focusShortcutsInstallChanged();
        return;
    }
    if (m_focusInstaller->isRunning()) {
        return;
    }
    // The shipped shortcuts have fixed names: the inhibitor must run those.
    setFocusOnShortcut(ShortcutsFocusInstaller::onName());
    setFocusOffShortcut(ShortcutsFocusInstaller::offName());
    m_focusInstaller->install();
}

void AppSettings::resetToDefaults()
{
    // Through the setters: they skip what is already right, store the rest and tell the views.
    setShowTrayIcon(Defaults::showTrayIcon);
    setCloseToTray(Defaults::closeToTray);
    setAllowExternalControl(Defaults::allowExternalControl);
    setStartMinimized(Defaults::startMinimized);
    setTrayBadgeStyle(Defaults::trayBadgeStyle);
    setShowDockBadge(Defaults::showDockBadge);
    setHideDockIcon(Defaults::hideDockIcon);
    setSoundTheme(QString(Defaults::soundTheme));
    setSoundVolume(Defaults::soundVolume);
    setPreAlarmEnabled(Defaults::preAlarmEnabled);
    setPreAlarmSound(QString(Defaults::preAlarmSound));
    setFocusSoundMode(Defaults::focusSoundMode);
    setFocusSoundVolume(Defaults::focusSoundVolume);
    setKeepScreenAwake(Defaults::keepScreenAwake);
    setAutoPauseOnScreenLock(Defaults::autoPauseOnScreenLock);
    setProtectWeekendStreak(Defaults::protectWeekendStreak);
    setPromptTaskNote(Defaults::promptTaskNote);
    setColorScheme(QString(Defaults::colorScheme));
    setAccentColor(QString(Defaults::accentColor));
    setUpdateChannel(QString(Defaults::updateChannel));
    setAutoCheckUpdates(Defaults::autoCheckUpdates);
    setShareUsageStats(Defaults::shareUsageStats);
    setVerboseLogging(Defaults::verboseLogging);
    setFocusOnShortcut(QString(Defaults::focusOnShortcut));
    setFocusOffShortcut(QString(Defaults::focusOffShortcut));
}

void AppSettings::applyColorScheme()
{
    // The interface (Theme.qml) follows the style hints; "system" lets the platform decide.
    if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        return;
    }
    Qt::ColorScheme scheme = Qt::ColorScheme::Unknown;
    if (m_colorScheme == QLatin1String("dark")) {
        scheme = Qt::ColorScheme::Dark;
    } else if (m_colorScheme == QLatin1String("light")) {
        scheme = Qt::ColorScheme::Light;
    }
    QGuiApplication::styleHints()->setColorScheme(scheme);
}

void AppSettings::playSoundPreview(const QString &soundName, int volume)
{
    const int vol = (volume >= 0) ? std::clamp(volume, 0, 100) : m_soundVolume;
    if (!isUsableSoundName(soundName)) {
        qWarning() << "Ignoring the sound" << soundName << ": not a sound name or an absolute path";
        return;
    }
    if (m_soundPlayer) {
        m_soundPlayer(soundName, vol);
        return;
    }
    const QString file = findSoundFile(soundName, soundSearchRoots());
    if (file.isEmpty()) {
        // Not a file we can find: let libcanberra resolve the event sound name. Its volume is in
        // decibels, and none is low enough for silence: a muted alert is not played at all.
        if (vol == 0) {
            return;
        }
        if (QStandardPaths::findExecutable(QStringLiteral("canberra-gtk-play")).isEmpty()
            || !QProcess::startDetached(QStringLiteral("canberra-gtk-play"), canberraArguments(soundName, vol))) {
            qWarning() << "Sound" << soundName << "not found and canberra-gtk-play is unavailable";
        }
        return;
    }

    if (!playWithFade(file, vol, this)) {
        playWithPlayerProgram(file, vol);
    }
}

double AppSettings::alertFadeGain(qint64 positionMs, qint64 durationMs)
{
    // A short sound gets shorter fades, so that it still reaches full volume.
    const qint64 fadeIn = durationMs > 0 ? std::min(kAlertFadeInMs, durationMs / 3) : kAlertFadeInMs;
    double gain = fadeIn > 0 ? std::clamp(double(positionMs) / fadeIn, 0.0, 1.0) : 1.0;
    if (durationMs > 0) {
        const qint64 fadeOut = std::min(kAlertFadeOutMs, durationMs / 3);
        if (fadeOut > 0) {
            gain = std::min(gain, std::clamp(double(durationMs - positionMs) / fadeOut, 0.0, 1.0));
        }
    }
    return gain;
}

qint64 AppSettings::alertPlayerLifetimeMs(qint64 durationMs)
{
    return durationMs > 0 ? durationMs + kAlertPlayerGraceMs : kAlertPlayerUnknownDurationMs;
}

void AppSettings::setAlertPlayerLifetimeForTesting(int ms)
{
    alertPlayerLifetimeForTestingMs = ms;
}

QStringList AppSettings::soundSearchRoots()
{
    QStringList roots = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    roots << QStringLiteral("/usr/share") << QStringLiteral("/run/host/usr/share");
#ifdef Q_OS_MACOS
    // The sounds bundled in Sugo.app/Contents/Resources/sounds/ (macOS has no sound themes).
    roots << QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../Resources"));
#endif
    return roots;
}

QString AppSettings::findSoundFile(const QString &soundName, const QStringList &roots)
{
    if (soundName.startsWith(QLatin1Char('/'))) {
        return QFileInfo(soundName).isFile() ? soundName : QString();
    }
    for (const char *theme : {"ocean", "freedesktop"}) {
        for (const char *ext : {"oga", "ogg", "wav"}) {
            const QString relative = QStringLiteral("/sounds/%1/stereo/%2.%3").arg(QLatin1String(theme), soundName, QLatin1String(ext));
            for (const QString &root : roots) {
                const QString file = root + relative;
                if (!root.isEmpty() && QFileInfo(file).isFile()) {
                    return file;
                }
            }
        }
    }
    return {};
}

bool AppSettings::afplayCanPlay(const QString &file)
{
    static const QStringList formats = {
        QStringLiteral("wav"), QStringLiteral("aif"), QStringLiteral("aiff"), QStringLiteral("aifc"),
        QStringLiteral("caf"), QStringLiteral("mp3"), QStringLiteral("m4a"), QStringLiteral("aac"),
    };
    return formats.contains(QFileInfo(file).suffix().toLower());
}

QStringList AppSettings::afplayArguments(const QString &file, int volume)
{
    const double factor = std::clamp(volume, 0, 100) / 100.0;
    // The file is an absolute path (see findSoundFile()), so it is never read as an option.
    return {QStringLiteral("-v"), QString::number(factor, 'f', 2), file};
}

bool AppSettings::isUsableSoundName(const QString &soundName)
{
    if (soundName.startsWith(QLatin1Char('/'))) {
        return !soundName.contains(QStringLiteral("/../")) && !soundName.endsWith(QStringLiteral("/.."));
    }
    static const QRegularExpression name(QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9._-]*$"));
    return name.match(soundName).hasMatch() && !soundName.contains(QStringLiteral(".."));
}

QStringList AppSettings::canberraArguments(const QString &soundName, int volume)
{
    // canberra-gtk-play takes the volume as a gain in dB: 100 % is 0 dB, 50 % about -6 dB. It sets
    // the user's locale and libcanberra reads the value with strtod, which in a locale with a
    // decimal comma rejects "-6.0" and plays nothing: whole decibels read the same everywhere.
    const double gainDb = 20.0 * std::log10(std::clamp(volume, 1, 100) / 100.0);
    return {QStringLiteral("--id=") + soundName, QStringLiteral("--volume=") + QString::number(qRound(gainDb))};
}

void AppSettings::openShortcutsSettings()
{
    if (!QProcess::startDetached(QStringLiteral("systemsettings"), {QStringLiteral("kcm_keys")})) {
        qWarning() << "Failed to launch systemsettings kcm_keys";
    }
}

bool AppSettings::keepRunningInTray() const
{
    return m_showTrayIcon && m_closeToTray && m_trayAvailable;
}

bool AppSettings::startHidden() const
{
    return m_launchedInBackground && m_startMinimized && m_showTrayIcon && m_trayAvailable;
}

void AppSettings::setLaunchedInBackground(bool background)
{
    m_launchedInBackground = background;
    Q_EMIT startHiddenChanged();
}
