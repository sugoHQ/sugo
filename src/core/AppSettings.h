// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Settings.h"

#include <QLatin1String>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>

class Autostart;
class ShortcutsFocusInhibitor;
class ShortcutsFocusInstaller;

/**
 * Settings of the program itself (as opposed to the per-timer options in PresetSettings):
 * how it lives in the desktop session.
 *
 * Stored in the settings file (see Settings). "Start at login" is not stored there: its source of truth
 * is the autostart entry on disk, so it stays right when another tool (System Settings)
 * enables or disables the entry.
 */
class AppSettings : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /// A system tray exists on this desktop (the settings below that need one are disabled otherwise).
    /// Follows the tray host coming and going (e.g. plasmashell restarting).
    Q_PROPERTY(bool trayAvailable READ trayAvailable NOTIFY trayAvailableChanged FINAL)
    /// This platform can manage a login entry from inside the application.
    Q_PROPERTY(bool autostartSupported READ autostartSupported CONSTANT FINAL)
    /// Other programs can drive the timer: D-Bus, KRunner and global shortcuts on Linux, sugo://
    /// links (scripts, the Shortcuts app) on macOS.
    Q_PROPERTY(bool externalControlSupported READ externalControlSupported CONSTANT FINAL)

    Q_PROPERTY(bool showTrayIcon READ showTrayIcon WRITE setShowTrayIcon NOTIFY showTrayIconChanged FINAL)
    /// Closing the window hides it instead of quitting (needs the tray icon).
    Q_PROPERTY(bool closeToTray READ closeToTray WRITE setCloseToTray NOTIFY closeToTrayChanged FINAL)
    /// Other programs (global shortcuts, scripts over D-Bus, KRunner; sugo:// links on macOS) may
    /// start, pause, skip and stop the timer.
    Q_PROPERTY(bool allowExternalControl READ allowExternalControl WRITE setAllowExternalControl NOTIFY allowExternalControlChanged FINAL)
    Q_PROPERTY(bool startAtLogin READ startAtLogin WRITE setStartAtLogin NOTIFY startAtLoginChanged FINAL)
    /// When started at login, start without showing the window.
    Q_PROPERTY(bool startMinimized READ startMinimized WRITE setStartMinimized NOTIFY startMinimizedChanged FINAL)

    /// macOS: the Dock icon shows the minutes left while a phase runs.
    Q_PROPERTY(bool showDockBadge READ showDockBadge WRITE setShowDockBadge NOTIFY showDockBadgeChanged FINAL)
    /// macOS: no Dock icon while the window is hidden (needs the menu bar icon; see DockController).
    Q_PROPERTY(bool hideDockIcon READ hideDockIcon WRITE setHideDockIcon NOTIFY hideDockIconChanged FINAL)

    /// Tray badge style: 0 = Minutes, 1 = Pie, 2 = Plain.
    Q_PROPERTY(int trayBadgeStyle READ trayBadgeStyle WRITE setTrayBadgeStyle NOTIFY trayBadgeStyleChanged FINAL)

    /// Notification sound theme / event sound name.
    Q_PROPERTY(QString soundTheme READ soundTheme WRITE setSoundTheme NOTIFY soundThemeChanged FINAL)
    /// Sound volume percentage (0..100).
    Q_PROPERTY(int soundVolume READ soundVolume WRITE setSoundVolume NOTIFY soundVolumeChanged FINAL)

    /// Pre-alarm at 1 minute before phase ends.
    Q_PROPERTY(bool preAlarmEnabled READ preAlarmEnabled WRITE setPreAlarmEnabled NOTIFY preAlarmEnabledChanged FINAL)
    Q_PROPERTY(QString preAlarmSound READ preAlarmSound WRITE setPreAlarmSound NOTIFY preAlarmSoundChanged FINAL)

    /// Ambient focus background sound mode (0=Off, 1=Clock, 2=Rain, 3=WhiteNoise).
    Q_PROPERTY(int focusSoundMode READ focusSoundMode WRITE setFocusSoundMode NOTIFY focusSoundModeChanged FINAL)
    Q_PROPERTY(int focusSoundVolume READ focusSoundVolume WRITE setFocusSoundVolume NOTIFY focusSoundVolumeChanged FINAL)

    /// Keep screen awake / prevent system idle sleep during work sessions.
    Q_PROPERTY(bool keepScreenAwake READ keepScreenAwake WRITE setKeepScreenAwake NOTIFY keepScreenAwakeChanged FINAL)

    /// Automatically pause active timer when screen is locked.
    Q_PROPERTY(bool autoPauseOnScreenLock READ autoPauseOnScreenLock WRITE setAutoPauseOnScreenLock NOTIFY autoPauseOnScreenLockChanged FINAL)


    /// Whether weekends without sessions do not break the streak.
    Q_PROPERTY(bool protectWeekendStreak READ protectWeekendStreak WRITE setProtectWeekendStreak NOTIFY protectWeekendStreakChanged FINAL)

    /// Whether the first-launch onboarding walkthrough has been completed.
    Q_PROPERTY(bool firstRunCompleted READ firstRunCompleted WRITE setFirstRunCompleted NOTIFY firstRunCompletedChanged FINAL)

    /// Whether to prompt the user to log a task note when a work session finishes.
    Q_PROPERTY(bool promptTaskNote READ promptTaskNote WRITE setPromptTaskNote NOTIFY promptTaskNoteChanged FINAL)

    /// Window colours: "dark", "light" or "system" (the default: the desktop's light or dark mode).
    Q_PROPERTY(QString colorScheme READ colorScheme WRITE setColorScheme NOTIFY colorSchemeChanged FINAL)
    /// The colour of work and of the main controls: one of accentColors (the default: "red").
    /// Theme.qml has its shades for the dark and the light theme.
    Q_PROPERTY(QString accentColor READ accentColor WRITE setAccentColor NOTIFY accentColorChanged FINAL)
    /// Every id accentColor takes, in the order the colour menu shows them.
    Q_PROPERTY(QStringList accentColors READ accentColors CONSTANT FINAL)
    /// "stable" (releases) or "nightly" (a build of every change), for the updates of the AppImage and the macOS app.
    Q_PROPERTY(QString updateChannel READ updateChannel WRITE setUpdateChannel NOTIFY updateChannelChanged FINAL)
    /// The AppImage looks for updates by itself (see UpdateChecker); it installs them when asked.
    Q_PROPERTY(bool autoCheckUpdates READ autoCheckUpdates WRITE setAutoCheckUpdates NOTIFY autoCheckUpdatesChanged FINAL)
    /// Anonymous usage statistics are sent (opt-in, off by default). Applied by Telemetry.
    Q_PROPERTY(bool shareUsageStats READ shareUsageStats WRITE setShareUsageStats NOTIFY shareUsageStatsChanged FINAL)
    /// Sugo's debug messages are recorded (in the diagnostic report). Applied by Diagnostics.
    Q_PROPERTY(bool verboseLogging READ verboseLogging WRITE setVerboseLogging NOTIFY verboseLoggingChanged FINAL)

    /// macOS: the shortcuts (Shortcuts app) that turn the Focus on and off while working,
    /// run by ShortcutsFocusInhibitor.
    Q_PROPERTY(QString focusOnShortcut READ focusOnShortcut WRITE setFocusOnShortcut NOTIFY focusOnShortcutChanged FINAL)
    Q_PROPERTY(QString focusOffShortcut READ focusOffShortcut WRITE setFocusOffShortcut NOTIFY focusOffShortcutChanged FINAL)
    /// macOS: the `shortcuts` program is there (macOS 12 or later).
    Q_PROPERTY(bool focusShortcutsSupported READ focusShortcutsSupported NOTIFY focusShortcutNamesChanged FINAL)
    /// macOS: Sugo can install the two shortcuts itself (installFocusShortcuts()).
    Q_PROPERTY(bool focusShortcutsInstallable READ focusShortcutsInstallable NOTIFY focusShortcutsInstallChanged FINAL)
    /// Where the last installFocusShortcuts() got: "" (never asked), "installing", "waitingOn" /
    /// "waitingOff" (the Shortcuts app asks the user to add that shortcut), "done", "failed".
    Q_PROPERTY(QString focusShortcutsInstallStatus READ focusShortcutsInstallStatus NOTIFY focusShortcutsInstallChanged FINAL)
    /// A technical detail of a "failed" installation (the tool's error).
    Q_PROPERTY(QString focusShortcutsInstallDetail READ focusShortcutsInstallDetail NOTIFY focusShortcutsInstallChanged FINAL)


    /// The close button really keeps the program running: tray icon on, tray present, option set.
    Q_PROPERTY(bool keepRunningInTray READ keepRunningInTray NOTIFY keepRunningInTrayChanged FINAL)
    /// Start with the window hidden: launched with --background and everything for it is in place.
    Q_PROPERTY(bool startHidden READ startHidden NOTIFY startHiddenChanged FINAL)

public:
    /// Real configuration file, real autostart entry, real tray detection. Used by QML.
    explicit AppSettings(QObject *parent = nullptr);
    AppSettings(Settings::Ptr config, std::unique_ptr<Autostart> autostart, bool trayAvailable, QObject *parent = nullptr);
    ~AppSettings() override;


    bool trayAvailable() const { return m_trayAvailable; }
    /// Normally driven by the D-Bus watcher of the tray host; public for tests.
    void setTrayAvailable(bool available);
    bool autostartSupported() const;
    static constexpr bool externalControlSupported() { return true; }

    bool showTrayIcon() const { return m_showTrayIcon; }
    void setShowTrayIcon(bool show);
    bool closeToTray() const { return m_closeToTray; }
    void setCloseToTray(bool close);
    bool allowExternalControl() const { return m_allowExternalControl; }
    void setAllowExternalControl(bool allow);
    bool startAtLogin() const;
    void setStartAtLogin(bool enabled);
    bool startMinimized() const { return m_startMinimized; }
    void setStartMinimized(bool minimized);

    bool showDockBadge() const { return m_showDockBadge; }
    void setShowDockBadge(bool show);
    bool hideDockIcon() const { return m_hideDockIcon; }
    void setHideDockIcon(bool hide);

    int trayBadgeStyle() const { return m_trayBadgeStyle; }
    void setTrayBadgeStyle(int style);

    QString soundTheme() const { return m_soundTheme; }
    void setSoundTheme(const QString &theme);

    int soundVolume() const { return m_soundVolume; }
    void setSoundVolume(int volume);

    bool preAlarmEnabled() const { return m_preAlarmEnabled; }
    void setPreAlarmEnabled(bool enabled);

    QString preAlarmSound() const { return m_preAlarmSound; }
    void setPreAlarmSound(const QString &sound);

    int focusSoundMode() const { return m_focusSoundMode; }
    void setFocusSoundMode(int mode);

    int focusSoundVolume() const { return m_focusSoundVolume; }
    void setFocusSoundVolume(int volume);

    bool keepScreenAwake() const { return m_keepScreenAwake; }
    void setKeepScreenAwake(bool awake);

    bool autoPauseOnScreenLock() const { return m_autoPauseOnScreenLock; }
    void setAutoPauseOnScreenLock(bool pause);


    bool protectWeekendStreak() const { return m_protectWeekendStreak; }
    void setProtectWeekendStreak(bool protect);

    bool firstRunCompleted() const { return m_firstRunCompleted; }
    void setFirstRunCompleted(bool completed);

    bool promptTaskNote() const { return m_promptTaskNote; }
    void setPromptTaskNote(bool prompt);

    QString colorScheme() const { return m_colorScheme; }
    void setColorScheme(const QString &scheme);
    QString accentColor() const { return m_accentColor; }
    /// An id that is not one of accentColors() sets the default.
    void setAccentColor(const QString &id);
    static QStringList accentColors();
    /// The name of an accent colour for the interface ("Red"), or the id itself if it is not one.
    Q_INVOKABLE static QString accentColorName(const QString &id);

    QString updateChannel() const { return m_updateChannel; }
    void setUpdateChannel(const QString &channel);
    bool autoCheckUpdates() const { return m_autoCheckUpdates; }
    void setAutoCheckUpdates(bool enabled);
    bool shareUsageStats() const { return m_shareUsageStats; }
    void setShareUsageStats(bool enabled);
    bool verboseLogging() const { return m_verboseLogging; }
    void setVerboseLogging(bool verbose);

    QString focusOnShortcut() const { return m_focusOnShortcut; }
    void setFocusOnShortcut(const QString &name);
    QString focusOffShortcut() const { return m_focusOffShortcut; }
    void setFocusOffShortcut(const QString &name);
    bool focusShortcutsSupported() const;
    /// The names of the user's shortcuts, as last read (empty without the Focus inhibitor).
    Q_INVOKABLE QStringList focusShortcutNames() const;
    /// Reads the user's shortcuts again; focusShortcutNamesChanged() when done.
    Q_INVOKABLE void refreshFocusShortcuts();
    /// Gives the Focus inhibitor its shortcut names (now and on every change) and reads its list
    /// of shortcuts. Called once at startup on macOS; tests call it on Linux.
    void attachFocusShortcuts(ShortcutsFocusInhibitor *inhibitor);
    bool focusShortcutsInstallable() const { return !m_focusInstaller.isNull(); }
    QString focusShortcutsInstallStatus() const { return m_focusInstallStatus; }
    QString focusShortcutsInstallDetail() const { return m_focusInstallDetail; }
    /// Opens the two Focus shortcuts shipped in the app, one after the other (see
    /// ShortcutsFocusInstaller); the Shortcuts app asks the user to add each. Sets the shortcut
    /// names back to those of the shipped shortcuts. Without an installer (Linux, smoke runs)
    /// reports "failed".
    Q_INVOKABLE void installFocusShortcuts();
    /// The installer installFocusShortcuts() uses. Called once at startup on macOS; tests call
    /// it on Linux with fake programs.
    void attachFocusShortcutsInstaller(ShortcutsFocusInstaller *installer);
    /// Puts every preference back to its default, through the setters (so views are told and the
    /// file is updated). Kept: whether the welcome tour was seen, the login entry (it lives on
    /// disk, outside the settings file) and everything that is not a preference of this class
    /// (timers, history, the calendar account).
    Q_INVOKABLE void resetToDefaults();
    /// Installs the chosen colour scheme as the application palette (needs a GUI application).
    void applyColorScheme();


    /// Plays an event sound or a file at the given volume, fading it in and out where Qt
    /// Multimedia can decode it.
    Q_INVOKABLE void playSoundPreview(const QString &soundName, int volume = -1);
    /// The fade level (0..1) of an alert sound at a position; the duration is 0 while unknown.
    static double alertFadeGain(qint64 positionMs, qint64 durationMs);
    /// How long an alert's Qt Multimedia player may live: a little more than the sound
    /// (kAlertPlayerGraceMs), or kAlertPlayerUnknownDurationMs while the duration is unknown (0).
    /// A backend that never reports the end nor an error then does not keep it until quit.
    static qint64 alertPlayerLifetimeMs(qint64 durationMs);
    static constexpr qint64 kAlertPlayerGraceMs = 2000;
    static constexpr qint64 kAlertPlayerUnknownDurationMs = 60 * 1000;
    /// Tests: every alert player lives at most `ms` (0: alertPlayerLifetimeMs() again).
    static void setAlertPlayerLifetimeForTesting(int ms);
    /// Plays a file with pw-play or paplay (afplay on macOS), at a fixed volume: where Qt
    /// Multimedia cannot. A player that fails to start or exits with an error right away gives way
    /// to the next for this sound; it is not tried again in this session if it failed to start, or
    /// if the next one played the file it could not (pw-play with no PipeWire daemon).
    /// Public for tests.
    static void playWithPlayerProgram(const QString &file, int volume);
    /// Tests: called with the sound name and volume instead of starting a player.
    void setSoundPlayerForTesting(std::function<void(const QString &, int)> player) { m_soundPlayer = std::move(player); }
    /// canberra-gtk-play arguments for an event sound name at a volume of 1..100 (in whole dB for
    /// canberra). The name is glued to its option, so one that starts with '-' can never be read
    /// as another option.
    static QStringList canberraArguments(const QString &soundName, int volume);
    /// An event sound name ("complete", "alarm-clock-elapsed") or an absolute path to a file.
    /// Anything else from the configuration ("../x", "-x", "a/b") is not looked up or played.
    static bool isUsableSoundName(const QString &soundName);
    /// The directories that may hold sounds/<theme>/stereo/<name>.<ext>, searched in order: the
    /// XDG data dirs (so it also works inside Flatpak), the host's /usr/share and, on macOS, the
    /// sounds bundled in the app's Resources.
    static QStringList soundSearchRoots();
    /// The file for an event sound name under one of the roots ("ocean" before "freedesktop",
    /// .oga before .ogg before .wav), or empty. An absolute path is used as is if it is a file.
    static QString findSoundFile(const QString &soundName, const QStringList &roots);
    /// afplay (macOS) can play the file: it has a format Core Audio decodes (not Ogg).
    static bool afplayCanPlay(const QString &file);
    /// afplay arguments for a file at a volume percentage (afplay's volume is linear, 1 = 100 %).
    static QStringList afplayArguments(const QString &file, int volume);
    Q_INVOKABLE void openShortcutsSettings();

    bool keepRunningInTray() const;
    bool startHidden() const;

    /// Set once at startup from the --background command line option.
    void setLaunchedInBackground(bool background);

Q_SIGNALS:
    void showTrayIconChanged();
    void closeToTrayChanged();
    void allowExternalControlChanged();
    void startAtLoginChanged();
    void startMinimizedChanged();
    void showDockBadgeChanged();
    void hideDockIconChanged();
    void trayBadgeStyleChanged();
    void soundThemeChanged();
    void soundVolumeChanged();
    void preAlarmEnabledChanged();
    void preAlarmSoundChanged();
    void focusSoundModeChanged();
    void focusSoundVolumeChanged();
    void keepScreenAwakeChanged();
    void autoPauseOnScreenLockChanged();
    void protectWeekendStreakChanged();
    void firstRunCompletedChanged();
    void promptTaskNoteChanged();
    void colorSchemeChanged();
    void accentColorChanged();
    void updateChannelChanged();
    void autoCheckUpdatesChanged();
    void shareUsageStatsChanged();
    void verboseLoggingChanged();
    void focusOnShortcutChanged();
    void focusOffShortcutChanged();
    /// The list of shortcuts was read again.
    void focusShortcutNamesChanged();
    void focusShortcutsInstallChanged();
    void trayAvailableChanged();
    void keepRunningInTrayChanged();
    void startHiddenChanged();
    /// The login entry could not be changed (read-only home, full disk, run from the disk image,
    /// ...). Emitted after the startAtLoginChanged() that restores the real state.
    void autostartFailed(const QString &message);

private:
    /// The value of every preference when nothing is stored: used when loading the file and by
    /// resetToDefaults().
    struct Defaults {
        static constexpr bool showTrayIcon = true;
        static constexpr bool closeToTray = true;
        static constexpr bool allowExternalControl = true;
        static constexpr bool startMinimized = true;
        static constexpr bool showDockBadge = true;
        static constexpr bool hideDockIcon = false;
        static constexpr int trayBadgeStyle = 0;
        static constexpr QLatin1String soundTheme{"alarm-clock-elapsed"};
        static constexpr int soundVolume = 80;
        static constexpr bool preAlarmEnabled = true;
        static constexpr QLatin1String preAlarmSound{"dialog-information"};
        static constexpr int focusSoundMode = 0;
        static constexpr int focusSoundVolume = 30;
        static constexpr bool keepScreenAwake = true;
        static constexpr bool autoPauseOnScreenLock = true;
        static constexpr bool protectWeekendStreak = true;
        static constexpr bool firstRunCompleted = false;
        static constexpr bool promptTaskNote = true;
        static constexpr QLatin1String colorScheme{"system"};
        static constexpr QLatin1String accentColor{"red"};
        static constexpr QLatin1String updateChannel{"stable"};
        static constexpr bool autoCheckUpdates = true;
        static constexpr bool shareUsageStats = false;
        static constexpr bool verboseLogging = false;
        static constexpr QLatin1String focusOnShortcut{"Sugo Focus On"};
        static constexpr QLatin1String focusOffShortcut{"Sugo Focus Off"};
    };

    void store(const char *key, bool value);
    void storeInt(const char *key, int value);
    void storeString(const char *key, const QString &value);
#ifdef Q_OS_LINUX
    /// Follows the StatusNotifierWatcher on the session bus (the tray host).
    void watchTray();
#endif

    Settings::Ptr m_config;
    std::unique_ptr<Autostart> m_autostart;
    std::function<void(const QString &, int)> m_soundPlayer;
    bool m_trayAvailable;

    bool m_showTrayIcon = Defaults::showTrayIcon;
    bool m_closeToTray = Defaults::closeToTray;
    bool m_allowExternalControl = Defaults::allowExternalControl;
    bool m_startMinimized = Defaults::startMinimized;
    bool m_showDockBadge = Defaults::showDockBadge;
    bool m_hideDockIcon = Defaults::hideDockIcon;
    int m_trayBadgeStyle = Defaults::trayBadgeStyle;
    QString m_soundTheme = QString(Defaults::soundTheme);
    int m_soundVolume = Defaults::soundVolume;
    bool m_preAlarmEnabled = Defaults::preAlarmEnabled;
    QString m_preAlarmSound = QString(Defaults::preAlarmSound);
    int m_focusSoundMode = Defaults::focusSoundMode;
    int m_focusSoundVolume = Defaults::focusSoundVolume;
    bool m_keepScreenAwake = Defaults::keepScreenAwake;
    bool m_autoPauseOnScreenLock = Defaults::autoPauseOnScreenLock;
    bool m_protectWeekendStreak = Defaults::protectWeekendStreak;
    bool m_firstRunCompleted = Defaults::firstRunCompleted;
    bool m_promptTaskNote = Defaults::promptTaskNote;
    QString m_colorScheme = QString(Defaults::colorScheme);
    QString m_accentColor = QString(Defaults::accentColor);
    QString m_updateChannel = QString(Defaults::updateChannel);
    bool m_autoCheckUpdates = Defaults::autoCheckUpdates;
    bool m_shareUsageStats = Defaults::shareUsageStats;
    bool m_verboseLogging = Defaults::verboseLogging;
    QString m_focusOnShortcut = QString(Defaults::focusOnShortcut);
    QString m_focusOffShortcut = QString(Defaults::focusOffShortcut);
    QPointer<ShortcutsFocusInhibitor> m_focusInhibitor;
    QPointer<ShortcutsFocusInstaller> m_focusInstaller;
    QString m_focusInstallStatus;
    QString m_focusInstallDetail;
    bool m_launchedInBackground = false;
};
