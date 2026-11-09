// SPDX-License-Identifier: GPL-3.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include "Settings.h"
#include "TrayIconStyle.h"

#include <memory>

#include "AppSettings.h"
#include "Autostart.h"
#include "FakeShortcutTools.h"
#include "FakeShortcuts.h"
#include "ShortcutsFocusInhibitor.h"
#include "ShortcutsFocusInstaller.h"

namespace
{
/// Autostart whose state the test controls; shared with the AppSettings under test.
struct FakeState {
    bool supported = true;
    bool enabled = false;
    bool failNext = false;
    bool temporaryLocation = false;
    int setCalls = 0;
    int repairCalls = 0;
};

class FakeAutostart final : public Autostart
{
public:
    explicit FakeAutostart(std::shared_ptr<FakeState> state)
        : m_state(std::move(state))
    {
    }

    bool isSupported() const override { return m_state->supported; }
    bool isEnabled() const override { return m_state->enabled; }
    bool setEnabled(bool enabled) override
    {
        ++m_state->setCalls;
        if (m_state->failNext) {
            m_state->failNext = false;
            return false;
        }
        m_state->enabled = enabled;
        return true;
    }
    void repair() override { ++m_state->repairCalls; }
    bool runsFromTemporaryLocation() const override { return m_state->temporaryLocation; }

    /// As when the Background portal's dialog answers after setEnabled() gave up waiting.
    void changeLater(bool enabled)
    {
        m_state->enabled = enabled;
        notifyChanged();
    }

private:
    std::shared_ptr<FakeState> m_state;
};

/// Stand-ins for pw-play and paplay, first on PATH while the object lives. Each call is logged
/// as "<program> <file name>". Both fail at once (exit 1) on a file whose name contains "bad";
/// after setNoPipeWire(), pw-play fails on every file, as where no PipeWire daemon runs.
class FakeAlertPlayers
{
public:
    FakeAlertPlayers()
        : m_oldPath(qgetenv("PATH"))
    {
        for (const char *name : {"pw-play", "paplay"}) {
            QFile file(m_dir.filePath(QLatin1String(name)));
            if (!file.open(QIODevice::WriteOnly)) {
                return;
            }
            file.write(R"SH(#!/bin/sh
dir=$(dirname "$0")
name=$(basename "$0")
for last; do :; done
echo "$name $(basename "$last")" >> "$dir/calls.log"
case "$last" in *bad*) echo "$name: cannot decode $last" >&2; exit 1 ;; esac
[ "$name" = pw-play ] && [ -f "$dir/no-pipewire" ] && { echo "pw-play: cannot connect" >&2; exit 1; }
exit 0
)SH");
            file.close();
            file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        }
        qputenv("PATH", QByteArray(m_dir.path().toLocal8Bit() + ':' + m_oldPath));
    }
    ~FakeAlertPlayers() { qputenv("PATH", m_oldPath); }
    FakeAlertPlayers(const FakeAlertPlayers &) = delete;
    FakeAlertPlayers &operator=(const FakeAlertPlayers &) = delete;

    bool isValid() const { return m_dir.isValid() && QFileInfo(m_dir.filePath(QStringLiteral("paplay"))).isExecutable(); }
    void setNoPipeWire()
    {
        QFile file(m_dir.filePath(QStringLiteral("no-pipewire")));
        (void)file.open(QIODevice::WriteOnly);
    }
    /// A sound file's path (the fakes never read it).
    QString file(const char *name) const { return m_dir.filePath(QLatin1String(name)); }
    QStringList calls() const
    {
        QFile log(m_dir.filePath(QStringLiteral("calls.log")));
        return log.open(QIODevice::ReadOnly) ? QString::fromUtf8(log.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts) : QStringList();
    }

private:
    QTemporaryDir m_dir;
    QByteArray m_oldPath;
};
} // namespace

class AppSettingsTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir dir;
    std::shared_ptr<FakeState> state;
    FakeAutostart *autostart = nullptr; // owned by the last AppSettings made

    QString configPath() const { return dir.filePath(QStringLiteral("settings.conf")); }

    std::unique_ptr<AppSettings> make(bool trayAvailable = true)
    {
        auto fake = std::make_unique<FakeAutostart>(state);
        autostart = fake.get();
        return std::make_unique<AppSettings>(Settings::open(configPath()), std::move(fake), trayAvailable);
    }

    // Reads the file directly, not through the object under test.
    static QString missing() { return QStringLiteral("<missing>"); }

    QString stored(const char *key) const
    {
        const QSettings config(configPath(), QSettings::IniFormat);
        return config.value(QLatin1String(key), missing()).toString();
    }

private Q_SLOTS:
    void initTestCase()
    {
        // A developer's exported SUGO_DATA_DIR would turn off the login-entry repair these tests
        // count; the test that needs the variable sets it itself.
        qunsetenv("SUGO_DATA_DIR");
    }

    void init()
    {
        QFile::remove(configPath());
        state = std::make_shared<FakeState>();
    }

    void defaultsKeepTheProgramInTheTray()
    {
        const auto settings = make();
        QVERIFY(settings->showTrayIcon());
        QVERIFY(settings->closeToTray());
        QVERIFY(settings->startMinimized());
        QVERIFY(!settings->startAtLogin());
        QVERIFY(settings->keepRunningInTray());
        QVERIFY(settings->trayAvailable());
        QVERIFY(settings->autostartSupported());
        QCOMPARE(settings->trayBadgeStyle(), 0);
        QVERIFY(settings->showDockBadge());
        QVERIFY(!settings->hideDockIcon());
        QCOMPARE(settings->soundTheme(), QStringLiteral("alarm-clock-elapsed"));
        QVERIFY(settings->protectWeekendStreak());
        QCOMPARE(settings->soundVolume(), 80);
        QVERIFY(settings->preAlarmEnabled());
        QCOMPARE(settings->preAlarmSound(), QStringLiteral("dialog-information"));
        QCOMPARE(settings->focusSoundMode(), 0);
        QCOMPARE(settings->focusSoundVolume(), 30);
        QVERIFY(settings->keepScreenAwake());
    }

    void changesAreWrittenToTheConfigFile()
    {
        {
            const auto settings = make();
            settings->setShowTrayIcon(false);
            settings->setCloseToTray(false);
            settings->setStartMinimized(false);
            settings->setTrayBadgeStyle(1);
            settings->setShowDockBadge(false);
            settings->setHideDockIcon(true);
            settings->setSoundTheme(QStringLiteral("complete"));
            settings->setSoundVolume(12);
            settings->setProtectWeekendStreak(false);
        }
        QCOMPARE(stored("ShowTrayIcon"), QStringLiteral("false"));
        QCOMPARE(stored("CloseToTray"), QStringLiteral("false"));
        QCOMPARE(stored("StartMinimized"), QStringLiteral("false"));
        QCOMPARE(stored("TrayBadgeStyle"), QStringLiteral("1"));
        QCOMPARE(stored("ShowDockBadge"), QStringLiteral("false"));
        QCOMPARE(stored("HideDockIcon"), QStringLiteral("true"));
        QCOMPARE(stored("SoundTheme"), QStringLiteral("complete"));
        QCOMPARE(stored("SoundVolume"), QStringLiteral("12"));
        QCOMPARE(stored("ProtectWeekendStreak"), QStringLiteral("false"));

        const auto reloaded = make();
        QVERIFY(!reloaded->showTrayIcon());
        QVERIFY(!reloaded->closeToTray());
        QVERIFY(!reloaded->startMinimized());
        QCOMPARE(reloaded->trayBadgeStyle(), 1);
        QVERIFY(!reloaded->showDockBadge());
        QVERIFY(reloaded->hideDockIcon());
        QCOMPARE(reloaded->soundTheme(), QStringLiteral("complete"));
        QCOMPARE(reloaded->soundVolume(), 12);
        QVERIFY(!reloaded->protectWeekendStreak());
    }

    void installingFocusShortcutsWithoutAnInstallerFails()
    {
        auto settings = make();
        QVERIFY(!settings->focusShortcutsInstallable());
        QSignalSpy changed(settings.get(), &AppSettings::focusShortcutsInstallChanged);
        settings->installFocusShortcuts();
        QCOMPARE(settings->focusShortcutsInstallStatus(), QStringLiteral("failed"));
        QVERIFY(!settings->focusShortcutsInstallDetail().isEmpty());
        QCOMPARE(changed.count(), 1);
    }

    void installingFocusShortcutsReportsAndUsesTheShippedNames()
    {
        FakeShortcutTools tools;
        QVERIFY(tools.isValid());
        FakeShortcuts shortcuts;
        QVERIFY(shortcuts.isValid());
        ShortcutsFocusInstaller installer(tools.programs(), tools.directory());
        installer.setTimingForTesting(20, 2000, 2000);
        ShortcutsFocusInhibitor inhibitor(shortcuts.program());
        auto settings = make();
        settings->attachFocusShortcuts(&inhibitor);
        settings->attachFocusShortcutsInstaller(&installer);
        QVERIFY(settings->focusShortcutsInstallable());
        QTRY_VERIFY(!shortcuts.calls().isEmpty()); // the first list
        // Other names give way to those of the shipped shortcuts.
        settings->setFocusOnShortcut(QStringLiteral("My Focus"));
        QTRY_VERIFY(shortcuts.calls().size() >= 2);
        shortcuts.setList({QStringLiteral("Sugo Focus On"), QStringLiteral("Sugo Focus Off")});

        QStringList statuses;
        connect(settings.get(), &AppSettings::focusShortcutsInstallChanged, this, [&]() { statuses.append(settings->focusShortcutsInstallStatus()); });
        settings->installFocusShortcuts();
        QCOMPARE(settings->focusOnShortcut(), QStringLiteral("Sugo Focus On"));
        QCOMPARE(settings->focusOffShortcut(), QStringLiteral("Sugo Focus Off"));
        QTRY_COMPARE(settings->focusShortcutsInstallStatus(), QStringLiteral("done"));
        QCOMPARE(statuses, (QStringList{QStringLiteral("installing"), QStringLiteral("waitingOn"), QStringLiteral("waitingOff"), QStringLiteral("done")}));
        QVERIFY(settings->focusShortcutsInstallDetail().isEmpty());
        // Done: the inhibitor reads its list again and finds both.
        QTRY_VERIFY(inhibitor.isAvailable());

        // A failure carries its detail.
        tools.setFails(QStringLiteral("shortcuts"), "broken");
        settings->installFocusShortcuts();
        QTRY_COMPARE(settings->focusShortcutsInstallStatus(), QStringLiteral("failed"));
        QVERIFY(settings->focusShortcutsInstallDetail().contains(QStringLiteral("broken")));
    }

    void focusAudioAndPowerSettingsPersistAndClamp()
    {
        {
            const auto settings = make();
            settings->setSoundVolume(130);
            settings->setPreAlarmEnabled(false);
            settings->setPreAlarmSound(QStringLiteral("bell"));
            settings->setFocusSoundMode(2);
            settings->setFocusSoundVolume(-10);
            settings->setKeepScreenAwake(false);
        }
        QCOMPARE(stored("SoundVolume"), QStringLiteral("100"));
        QCOMPARE(stored("PreAlarmEnabled"), QStringLiteral("false"));
        QCOMPARE(stored("PreAlarmSound"), QStringLiteral("bell"));
        QCOMPARE(stored("FocusSoundMode"), QStringLiteral("2"));
        QCOMPARE(stored("FocusSoundVolume"), QStringLiteral("0"));
        QCOMPARE(stored("KeepScreenAwake"), QStringLiteral("false"));

        const auto reloaded = make();
        QCOMPARE(reloaded->soundVolume(), 100);
        QVERIFY(!reloaded->preAlarmEnabled());
        QCOMPARE(reloaded->preAlarmSound(), QStringLiteral("bell"));
        QCOMPARE(reloaded->focusSoundMode(), 2);
        QCOMPARE(reloaded->focusSoundVolume(), 0);
        QVERIFY(!reloaded->keepScreenAwake());
    }

    void startAtLoginIsNotStoredInTheConfigFile()
    {
        const auto settings = make();
        settings->setStartAtLogin(true);
        settings->setShowTrayIcon(false); // forces a write
        QCOMPARE(stored("StartAtLogin"), missing());
        QCOMPARE(stored("startAtLogin"), missing());
    }

    void signalsFireOnlyOnRealChanges()
    {
        const auto settings = make();
        QSignalSpy tray(settings.get(), &AppSettings::showTrayIconChanged);
        QSignalSpy keep(settings.get(), &AppSettings::keepRunningInTrayChanged);
        QSignalSpy close(settings.get(), &AppSettings::closeToTrayChanged);
        QSignalSpy minimized(settings.get(), &AppSettings::startMinimizedChanged);
        QSignalSpy badge(settings.get(), &AppSettings::trayBadgeStyleChanged);
        QSignalSpy sound(settings.get(), &AppSettings::soundThemeChanged);
        QSignalSpy streak(settings.get(), &AppSettings::protectWeekendStreakChanged);

        settings->setShowTrayIcon(true); // unchanged
        settings->setCloseToTray(true);
        settings->setStartMinimized(true);
        settings->setTrayBadgeStyle(0);
        settings->setSoundTheme(QStringLiteral("alarm-clock-elapsed"));
        settings->setProtectWeekendStreak(true);
        QCOMPARE(tray.count(), 0);
        QCOMPARE(close.count(), 0);
        QCOMPARE(minimized.count(), 0);
        QCOMPARE(badge.count(), 0);
        QCOMPARE(sound.count(), 0);
        QCOMPARE(streak.count(), 0);

        settings->setShowTrayIcon(false);
        QCOMPARE(tray.count(), 1);
        QCOMPARE(keep.count(), 1);
        settings->setCloseToTray(false);
        QCOMPARE(close.count(), 1);
        QCOMPARE(keep.count(), 2);
        settings->setStartMinimized(false);
        QCOMPARE(minimized.count(), 1);
        settings->setTrayBadgeStyle(2);
        QCOMPARE(badge.count(), 1);
        settings->setSoundTheme(QStringLiteral("bell"));
        QCOMPARE(sound.count(), 1);
        settings->setProtectWeekendStreak(false);
        QCOMPARE(streak.count(), 1);
    }

    void keepRunningInTrayNeedsTheIconTheOptionAndATray()
    {
        {
            const auto settings = make(true);
            QVERIFY(settings->keepRunningInTray());
            settings->setCloseToTray(false);
            QVERIFY(!settings->keepRunningInTray());
            settings->setCloseToTray(true);
            settings->setShowTrayIcon(false);
            QVERIFY(!settings->keepRunningInTray());
        }
        QFile::remove(configPath());
        const auto noTray = make(false); // nothing to hide into: closing must quit
        QVERIFY(!noTray->trayAvailable());
        QVERIFY(!noTray->keepRunningInTray());
    }

    void startHiddenNeedsEveryConditionAtOnce()
    {
        const auto settings = make(true);
        QVERIFY(!settings->startHidden()); // not launched with --background

        QSignalSpy spy(settings.get(), &AppSettings::startHiddenChanged);
        settings->setLaunchedInBackground(true);
        QVERIFY(settings->startHidden());
        QCOMPARE(spy.count(), 1);

        settings->setStartMinimized(false);
        QVERIFY(!settings->startHidden());
        settings->setStartMinimized(true);
        QVERIFY(settings->startHidden());
        settings->setShowTrayIcon(false);
        QVERIFY(!settings->startHidden()); // hidden with no icon would leave the program unreachable
    }

    void startHiddenIsOffWithoutATray()
    {
        const auto settings = make(false);
        settings->setLaunchedInBackground(true);
        QVERIFY(!settings->startHidden()); // never start invisible when there is no tray to find it in
    }

    void startAtLoginFollowsTheEntryOnDisk()
    {
        const auto settings = make();
        QVERIFY(!settings->startAtLogin());
        state->enabled = true; // another tool (System Settings) enabled it
        QVERIFY(settings->startAtLogin());
        state->enabled = false;
        QVERIFY(!settings->startAtLogin());
    }

    void settingStartAtLoginCreatesAndRemovesTheEntry()
    {
        const auto settings = make();
        settings->setStartAtLogin(true);
        QVERIFY(state->enabled);
        QVERIFY(settings->startAtLogin());
        settings->setStartAtLogin(false);
        QVERIFY(!state->enabled);
    }

    void turningStartAtLoginOffAlsoTurnsStartMinimizedOff()
    {
        const auto settings = make();
        settings->setStartAtLogin(true);
        settings->setStartMinimized(true);
        settings->setStartAtLogin(false);
        QVERIFY(!settings->startMinimized());
        QCOMPARE(stored("StartMinimized"), QStringLiteral("false"));

        // It stays as chosen while the start at login stays on, and when removing the entry fails.
        settings->setStartAtLogin(true);
        settings->setStartMinimized(true);
        state->failNext = true;
        settings->setStartAtLogin(false);
        QVERIFY(settings->startAtLogin());
        QVERIFY(settings->startMinimized());
    }

    void settingTheCurrentValueDoesNotTouchTheEntry()
    {
        const auto settings = make();
        settings->setStartAtLogin(false);
        QCOMPARE(state->setCalls, 0);
    }

    void aRefusedChangeIsReportedAndTheStateStaysTrue()
    {
        const auto settings = make();
        QSignalSpy failed(settings.get(), &AppSettings::autostartFailed);
        QSignalSpy changed(settings.get(), &AppSettings::startAtLoginChanged);

        state->failNext = true;
        settings->setStartAtLogin(true);

        QCOMPARE(failed.count(), 1);
        QVERIFY(!failed.first().first().toString().isEmpty());
        QVERIFY(!settings->startAtLogin());  // the real state, not what was asked for
        QCOMPARE(changed.count(), 1);        // announced, so a checkbox shows the real state again
    }

    // Opened from the disk image: a message that says what to do, not a generic failure.
    void enablingFromATemporaryLocationSaysToMoveTheApp()
    {
        const auto settings = make();
        QSignalSpy failed(settings.get(), &AppSettings::autostartFailed);
        state->temporaryLocation = true;

        settings->setStartAtLogin(true);

        QCOMPARE(state->setCalls, 0);
        QVERIFY(!settings->startAtLogin());
        QCOMPARE(failed.count(), 1);
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("Applications folder")));

        // Turning off an entry made by the installed copy still works from there.
        state->enabled = true;
        settings->setStartAtLogin(false);
        QVERIFY(!settings->startAtLogin());
        QCOMPARE(failed.count(), 1);
    }

    // The portal's dialog answered after setStartAtLogin() reported a failure.
    void aChangeMadeLaterIsAnnounced()
    {
        const auto settings = make();
        QSignalSpy changed(settings.get(), &AppSettings::startAtLoginChanged);
        QSignalSpy failed(settings.get(), &AppSettings::autostartFailed);

        autostart->changeLater(true);
        QVERIFY(settings->startAtLogin());
        QCOMPARE(changed.count(), 1);

        // Switched off later: "Start minimized" goes with it, as when switched off from here.
        settings->setStartMinimized(true);
        autostart->changeLater(false);
        QVERIFY(!settings->startAtLogin());
        QVERIFY(!settings->startMinimized());
        QCOMPARE(changed.count(), 2);
        QCOMPARE(failed.count(), 0);
    }

    // The welcome tour clears its message on startAtLoginChanged: the failure must come after it.
    void theFailureIsAnnouncedAfterTheStateChange()
    {
        QStringList order;
        const auto settings = make();
        connect(settings.get(), &AppSettings::startAtLoginChanged, this, [&order] { order << QStringLiteral("changed"); });
        connect(settings.get(), &AppSettings::autostartFailed, this, [&order] { order << QStringLiteral("failed"); });

        state->failNext = true;
        settings->setStartAtLogin(true);
        QCOMPARE(order, QStringList({QStringLiteral("changed"), QStringLiteral("failed")}));
    }

    void aStaleEntryIsRepairedAtStartup()
    {
        make();
        QCOMPARE(state->repairCalls, 1);
    }

    // A test run or a build being tried (SUGO_DATA_DIR) must not point the user's login entry at itself.
    void anIsolatedRunLeavesTheLoginEntryAlone()
    {
        const QByteArray saved = qgetenv("SUGO_DATA_DIR");
        const bool wasSet = qEnvironmentVariableIsSet("SUGO_DATA_DIR");
        QTemporaryDir dir;
        qputenv("SUGO_DATA_DIR", QFile::encodeName(dir.path()));
        make();
        if (wasSet) {
            qputenv("SUGO_DATA_DIR", saved);
        } else {
            qunsetenv("SUGO_DATA_DIR");
        }
        QCOMPARE(state->repairCalls, 0);
    }

    void unsupportedPlatformsAreReported()
    {
        state->supported = false;
        const auto settings = make();
        QVERIFY(!settings->autostartSupported());
    }

    void firstRunCompletedDefaultsToFalseAndPersists()
    {
        {
            const auto settings = make();
            QVERIFY(!settings->firstRunCompleted());
            QSignalSpy spy(settings.get(), &AppSettings::firstRunCompletedChanged);
            settings->setFirstRunCompleted(true);
            QVERIFY(settings->firstRunCompleted());
            QCOMPARE(spy.count(), 1);
        }
        // Re-read from disk
        {
            const auto settings = make();
            QVERIFY(settings->firstRunCompleted());
        }
    }

    void autoPauseOnScreenLockDefaultsToTrueAndPersists()
    {
        {
            const auto settings = make();
            QVERIFY(settings->autoPauseOnScreenLock());
            QSignalSpy spy(settings.get(), &AppSettings::autoPauseOnScreenLockChanged);
            settings->setAutoPauseOnScreenLock(false);
            QVERIFY(!settings->autoPauseOnScreenLock());
            QCOMPARE(spy.count(), 1);
        }
        // Re-read from disk
        {
            const auto settings = make();
            QVERIFY(!settings->autoPauseOnScreenLock());
        }
    }

    void promptTaskNoteDefaultsToTrueAndPersists()
    {
        {
            const auto settings = make();
            QVERIFY(settings->promptTaskNote());
            QSignalSpy spy(settings.get(), &AppSettings::promptTaskNoteChanged);
            settings->setPromptTaskNote(false);
            QVERIFY(!settings->promptTaskNote());
            QCOMPARE(spy.count(), 1);
        }
        // Re-read from disk
        {
            const auto settings = make();
            QVERIFY(!settings->promptTaskNote());
        }
    }

    void invalidStoredValuesAreClamped()
    {
        {
            QSettings config(configPath(), QSettings::IniFormat);
            config.setValue(QStringLiteral("TrayBadgeStyle"), 9);
            config.setValue(QStringLiteral("FocusSoundMode"), 99);
            config.setValue(QStringLiteral("SoundVolume"), 500);
            config.setValue(QStringLiteral("ColorScheme"), QStringLiteral("purple"));
            config.setValue(QStringLiteral("AccentColor"), QStringLiteral("purple"));
            config.sync();
        }
        {
            const auto settings = make();
            QCOMPARE(settings->trayBadgeStyle(), 2);
            QCOMPARE(settings->focusSoundMode(), 6);
            QCOMPARE(settings->soundVolume(), 100);
            QCOMPARE(settings->colorScheme(), QStringLiteral("system"));
            QCOMPARE(settings->accentColor(), QStringLiteral("red"));
        } // releases the shared config, so the next one rereads the file

        {
            QSettings config(configPath(), QSettings::IniFormat);
            config.setValue(QStringLiteral("TrayBadgeStyle"), -1);
            config.setValue(QStringLiteral("FocusSoundMode"), -4);
            config.setValue(QStringLiteral("SoundVolume"), -3);
            config.setValue(QStringLiteral("AccentColor"), 7);
            config.sync();
        }
        const auto reloaded = make();
        QCOMPARE(reloaded->trayBadgeStyle(), 0);
        QCOMPARE(reloaded->focusSoundMode(), 0);
        QCOMPARE(reloaded->soundVolume(), 0);
        QCOMPARE(reloaded->accentColor(), QStringLiteral("red"));
    }

    void emptyOrNonNumericValuesUseTheDefaults()
    {
        // A hand-edited or half-written file: such a value must not read as 0 (muting the alerts).
        // A file of its own: QSettings keeps the parse of a path it has seen before when the
        // file's size and time stamp have not changed, which a test writing the same path at
        // once can hit (seen on macOS).
        const QString path = dir.filePath(QStringLiteral("hand-edited.conf"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write("[General]\nSoundVolume=\nFocusSoundVolume=loud\nTrayBadgeStyle=two\nFocusSoundMode=\n");
        }
        const auto settings = std::make_unique<AppSettings>(Settings::open(path), std::make_unique<FakeAutostart>(state), true);
        QCOMPARE(settings->soundVolume(), 80);
        QCOMPARE(settings->focusSoundVolume(), 30);
        QCOMPARE(settings->trayBadgeStyle(), 0);
        QCOMPARE(settings->focusSoundMode(), 0);
    }

    void aFailedSaveIsLoggedOnce()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString folder = parent.filePath(QStringLiteral("read-only"));
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
        const auto restore = qScopeGuard([&folder]() {
            QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        });
        {
            // Root (some containers) writes anywhere: nothing to test there.
            QFile probe(folder + QStringLiteral("/probe"));
            if (probe.open(QIODevice::WriteOnly)) {
                probe.remove();
                QSKIP("The read-only folder can be written to (running as root?)");
            }
        }

        const Settings::Ptr settings = Settings::open(folder + QStringLiteral("/sugo.conf"));
        settings->setValue(QStringLiteral("SoundVolume"), 7);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("^Cannot save the settings to .*sugo\\.conf")));
        Settings::sync(*settings);
        QVERIFY(settings->status() != QSettings::NoError);

        // The next change fails the same way, without a second message.
        QTest::failOnWarning(QRegularExpression(QStringLiteral("^Cannot save the settings")));
        settings->setValue(QStringLiteral("SoundVolume"), 8);
        Settings::sync(*settings);
    }

    void colorSchemeDefaultsToSystemAndPersists()
    {
        {
            const auto settings = make();
            QCOMPARE(settings->colorScheme(), QStringLiteral("system"));
            QSignalSpy spy(settings.get(), &AppSettings::colorSchemeChanged);
            settings->setColorScheme(QStringLiteral("light"));
            QCOMPARE(settings->colorScheme(), QStringLiteral("light"));
            QCOMPARE(spy.count(), 1);
            settings->setColorScheme(QStringLiteral("bogus")); // falls back to the system scheme
            QCOMPARE(settings->colorScheme(), QStringLiteral("system"));
            settings->setColorScheme(QStringLiteral("dark"));
            QCOMPARE(spy.count(), 3);
        }
        QCOMPARE(stored("ColorScheme"), QStringLiteral("dark"));
        QCOMPARE(make()->colorScheme(), QStringLiteral("dark"));
    }

    void accentColorDefaultsToRedAndPersists()
    {
        {
            const auto settings = make();
            QCOMPARE(settings->accentColor(), QStringLiteral("red"));
            QCOMPARE(stored("AccentColor"), missing()); // the default is not written out
            QSignalSpy spy(settings.get(), &AppSettings::accentColorChanged);
            settings->setAccentColor(QStringLiteral("teal"));
            QCOMPARE(settings->accentColor(), QStringLiteral("teal"));
            QCOMPARE(spy.count(), 1);
            settings->setAccentColor(QStringLiteral("teal"));
            QCOMPARE(spy.count(), 1);
            settings->setAccentColor(QStringLiteral(" Violet ")); // like the stored scheme: trimmed, any case
            QCOMPARE(settings->accentColor(), QStringLiteral("violet"));
            QCOMPARE(spy.count(), 2);
            settings->setAccentColor(QStringLiteral("purple")); // not an accent: the default
            QCOMPARE(settings->accentColor(), QStringLiteral("red"));
            QCOMPARE(spy.count(), 3);
            settings->setAccentColor(QString());
            QCOMPARE(spy.count(), 3);
            settings->setAccentColor(QStringLiteral("blue"));
            QCOMPARE(spy.count(), 4);
        }
        QCOMPARE(stored("AccentColor"), QStringLiteral("blue"));
        QCOMPARE(make()->accentColor(), QStringLiteral("blue"));
    }

    void badgeColorsFollowTheAccent()
    {
        // The badge (menu bar icon and its pictures in the interface) is painted in the accent:
        // the shades of every id are the one palette of the interface (Theme.qml) too.
        QCOMPARE(TrayIconStyle::workBadgeColor(QStringLiteral("red")), QColor(0xff, 0x4d, 0x4d)); // red's dark shade
        QCOMPARE(TrayIconStyle::breakBadgeColor(QStringLiteral("red")), TrayIconColors::rest()); // the break green
        QCOMPARE(TrayIconStyle::workBadgeColor(QStringLiteral("teal")), QColor(0x2d, 0xd4, 0xbf)); // teal's dark shade
        QCOMPARE(TrayIconStyle::breakBadgeColor(QStringLiteral("teal")), QColor(0x60, 0xa5, 0xfa)); // blue on a green accent
        QCOMPARE(TrayIconStyle::workBadgeColor(QStringLiteral("nonsense")),
                 TrayIconStyle::workBadgeColor(QStringLiteral("red"))); // red for an unknown id
        // Paused is yellow, unlike work and breaks whatever the accent (grey on a yellow accent).
        QCOMPARE(TrayIconStyle::pausedBadgeColor(QStringLiteral("red")), QColor(0xfa, 0xcc, 0x15));
        for (const QString &id : {QStringLiteral("red"), QStringLiteral("orange"), QStringLiteral("yellow"), QStringLiteral("lime"),
                                  QStringLiteral("green"), QStringLiteral("teal"), QStringLiteral("cyan"), QStringLiteral("blue"),
                                  QStringLiteral("violet"), QStringLiteral("pink")}) {
            QVERIFY2(TrayIconStyle::pausedBadgeColor(id) != TrayIconStyle::workBadgeColor(id), qPrintable(id));
            QVERIFY2(TrayIconStyle::pausedBadgeColor(id) != TrayIconStyle::breakBadgeColor(id), qPrintable(id));
        }

        TrayIconStyle style;
        QSignalSpy spy(&style, &TrayIconStyle::accentColorChanged);
        style.setAccentColor(QStringLiteral("blue"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(style.workColor(), TrayIconStyle::workBadgeColor(QStringLiteral("blue")));
        QCOMPARE(style.breakColor(), TrayIconColors::rest());
        style.setAccentColor(QStringLiteral("blue"));
        QCOMPARE(spy.count(), 1);
    }

    void everyAccentColorHasAName()
    {
        QSet<QString> names;
        for (const QString &id : AppSettings::accentColors()) {
            const QString name = AppSettings::accentColorName(id);
            QVERIFY2(!name.isEmpty() && name != id, qPrintable(id));
            names.insert(name);
        }
        QCOMPARE(names.size(), AppSettings::accentColors().size());
        QCOMPARE(AppSettings::accentColorName(QStringLiteral("red")), QStringLiteral("Red"));
        QCOMPARE(AppSettings::accentColorName(QStringLiteral("purple")), QStringLiteral("purple"));
    }

    void everyAccentColorIsKept()
    {
        const QStringList ids = AppSettings::accentColors();
        QCOMPARE(ids,
                 QStringList({QStringLiteral("red"), QStringLiteral("orange"), QStringLiteral("yellow"), QStringLiteral("lime"),
                              QStringLiteral("green"), QStringLiteral("teal"), QStringLiteral("cyan"), QStringLiteral("blue"),
                              QStringLiteral("violet"), QStringLiteral("pink")}));
        for (const QString &id : ids) {
            {
                const auto settings = make();
                settings->setAccentColor(id);
                QCOMPARE(settings->accentColor(), id);
            }
            QCOMPARE(make()->accentColor(), id);
        }
    }

    void updatesDefaultToStableAndAutomatic()
    {
        {
            const auto settings = make();
            QCOMPARE(settings->updateChannel(), QStringLiteral("stable"));
            QVERIFY(settings->autoCheckUpdates());
            QSignalSpy channelSpy(settings.get(), &AppSettings::updateChannelChanged);
            QSignalSpy autoSpy(settings.get(), &AppSettings::autoCheckUpdatesChanged);
            settings->setUpdateChannel(QStringLiteral("nightly"));
            settings->setUpdateChannel(QStringLiteral("nightly"));
            QCOMPARE(channelSpy.count(), 1);
            settings->setUpdateChannel(QStringLiteral("beta")); // unknown: stable
            QCOMPARE(settings->updateChannel(), QStringLiteral("stable"));
            settings->setUpdateChannel(QStringLiteral("nightly"));
            settings->setAutoCheckUpdates(false);
            QCOMPARE(autoSpy.count(), 1);
        }
        QCOMPARE(stored("UpdateChannel"), QStringLiteral("nightly"));
        const auto reloaded = make();
        QCOMPARE(reloaded->updateChannel(), QStringLiteral("nightly"));
        QVERIFY(!reloaded->autoCheckUpdates());
    }

    void usageStatsAreOffByDefaultAndPersist()
    {
        {
            const auto settings = make();
            QVERIFY(!settings->shareUsageStats());
            QSignalSpy spy(settings.get(), &AppSettings::shareUsageStatsChanged);
            settings->setShareUsageStats(false);
            QCOMPARE(spy.count(), 0);
            settings->setShareUsageStats(true);
            settings->setShareUsageStats(true);
            QCOMPARE(spy.count(), 1);
        }
        QCOMPARE(stored("ShareUsageStats"), QStringLiteral("true"));
        QVERIFY(make()->shareUsageStats());
    }

    void verboseLoggingIsOffByDefaultAndPersists()
    {
        {
            const auto settings = make();
            QVERIFY(!settings->verboseLogging());
            QSignalSpy spy(settings.get(), &AppSettings::verboseLoggingChanged);
            settings->setVerboseLogging(false);
            QCOMPARE(spy.count(), 0);
            settings->setVerboseLogging(true);
            settings->setVerboseLogging(true);
            QCOMPARE(spy.count(), 1);
            QVERIFY(settings->verboseLogging());
        }
        QCOMPARE(stored("VerboseLogging"), QStringLiteral("true"));
        QVERIFY(make()->verboseLogging());
    }

    void resetToDefaultsRestoresEveryPreferenceThroughTheSetters()
    {
        {
            // Not preferences of this class: they must survive a reset.
            QSettings other(configPath(), QSettings::IniFormat);
            other.setValue(QStringLiteral("CalendarSync/Username"), QStringLiteral("anna"));
            other.sync();
        }
        const auto settings = make();
        settings->setShowTrayIcon(false);
        settings->setCloseToTray(false);
        settings->setAllowExternalControl(false);
        settings->setStartAtLogin(true);
        settings->setStartMinimized(false);
        settings->setTrayBadgeStyle(2);
        settings->setShowDockBadge(false);
        settings->setHideDockIcon(true);
        settings->setSoundTheme(QStringLiteral("bell"));
        settings->setSoundVolume(10);
        settings->setPreAlarmEnabled(false);
        settings->setPreAlarmSound(QStringLiteral("bell"));
        settings->setFocusSoundMode(2);
        settings->setFocusSoundVolume(90);
        settings->setKeepScreenAwake(false);
        settings->setAutoPauseOnScreenLock(false);
        settings->setProtectWeekendStreak(false);
        settings->setFirstRunCompleted(true);
        settings->setPromptTaskNote(false);
        settings->setColorScheme(QStringLiteral("light"));
        settings->setAccentColor(QStringLiteral("cyan"));
        settings->setUpdateChannel(QStringLiteral("nightly"));
        settings->setAutoCheckUpdates(false);
        settings->setShareUsageStats(true);
        settings->setVerboseLogging(true);
        settings->setFocusOnShortcut(QStringLiteral("Mine On"));
        settings->setFocusOffShortcut(QStringLiteral("Mine Off"));

        QSignalSpy trayIcon(settings.get(), &AppSettings::showTrayIconChanged);
        QSignalSpy volume(settings.get(), &AppSettings::soundVolumeChanged);
        QSignalSpy scheme(settings.get(), &AppSettings::colorSchemeChanged);
        QSignalSpy accent(settings.get(), &AppSettings::accentColorChanged);
        QSignalSpy verbose(settings.get(), &AppSettings::verboseLoggingChanged);
        QSignalSpy channel(settings.get(), &AppSettings::updateChannelChanged);
        QSignalSpy firstRun(settings.get(), &AppSettings::firstRunCompletedChanged);
        QSignalSpy login(settings.get(), &AppSettings::startAtLoginChanged);

        settings->resetToDefaults();

        QVERIFY(settings->showTrayIcon());
        QVERIFY(settings->closeToTray());
        QVERIFY(settings->allowExternalControl());
        QVERIFY(settings->startMinimized());
        QCOMPARE(settings->trayBadgeStyle(), 0);
        QVERIFY(settings->showDockBadge());
        QVERIFY(!settings->hideDockIcon());
        QCOMPARE(settings->soundTheme(), QStringLiteral("alarm-clock-elapsed"));
        QCOMPARE(settings->soundVolume(), 80);
        QVERIFY(settings->preAlarmEnabled());
        QCOMPARE(settings->preAlarmSound(), QStringLiteral("dialog-information"));
        QCOMPARE(settings->focusSoundMode(), 0);
        QCOMPARE(settings->focusSoundVolume(), 30);
        QVERIFY(settings->keepScreenAwake());
        QVERIFY(settings->autoPauseOnScreenLock());
        QVERIFY(settings->protectWeekendStreak());
        QVERIFY(settings->promptTaskNote());
        QCOMPARE(settings->colorScheme(), QStringLiteral("system"));
        QCOMPARE(settings->accentColor(), QStringLiteral("red"));
        QCOMPARE(settings->updateChannel(), QStringLiteral("stable"));
        QVERIFY(settings->autoCheckUpdates());
        QVERIFY(!settings->shareUsageStats());
        QVERIFY(!settings->verboseLogging());

        // Views are told ...
        QCOMPARE(trayIcon.count(), 1);
        QCOMPARE(volume.count(), 1);
        QCOMPARE(scheme.count(), 1);
        QCOMPARE(accent.count(), 1);
        QCOMPARE(verbose.count(), 1);
        QCOMPARE(channel.count(), 1);
        // ... and what is not a preference of this class stays.
        QVERIFY(settings->firstRunCompleted());
        QCOMPARE(firstRun.count(), 0);
        QVERIFY(settings->startAtLogin());
        QVERIFY(state->enabled);
        QCOMPARE(login.count(), 0);

        // The file has the defaults, not the old values.
        QCOMPARE(stored("ShowTrayIcon"), QStringLiteral("true"));
        QCOMPARE(stored("SoundVolume"), QStringLiteral("80"));
        QCOMPARE(stored("SoundTheme"), QStringLiteral("alarm-clock-elapsed"));
        QCOMPARE(stored("ColorScheme"), QStringLiteral("system"));
        QCOMPARE(stored("AccentColor"), QStringLiteral("red"));
        QCOMPARE(stored("UpdateChannel"), QStringLiteral("stable"));
        QCOMPARE(stored("VerboseLogging"), QStringLiteral("false"));
        QCOMPARE(stored("FirstRunCompleted"), QStringLiteral("true"));
        QCOMPARE(stored("CalendarSync/Username"), QStringLiteral("anna"));
        const auto reloaded = make();
        QCOMPARE(reloaded->soundVolume(), 80);
        QCOMPARE(reloaded->colorScheme(), QStringLiteral("system"));
        QCOMPARE(reloaded->accentColor(), QStringLiteral("red"));
        QVERIFY(reloaded->showTrayIcon());
        QVERIFY(!reloaded->verboseLogging());

        // Nothing left to reset: no change is announced.
        settings->resetToDefaults();
        QCOMPARE(trayIcon.count(), 1);
        QCOMPARE(volume.count(), 1);
        QCOMPARE(scheme.count(), 1);
        QCOMPARE(accent.count(), 1);
    }

    void trayAvailabilityChangesUpdateDependentProperties()
    {
        const auto settings = make(false);
        QVERIFY(!settings->keepRunningInTray());
        QSignalSpy tray(settings.get(), &AppSettings::trayAvailableChanged);
        QSignalSpy keep(settings.get(), &AppSettings::keepRunningInTrayChanged);
        QSignalSpy hidden(settings.get(), &AppSettings::startHiddenChanged);

        settings->setTrayAvailable(true);
        QVERIFY(settings->trayAvailable());
        QVERIFY(settings->keepRunningInTray());
        QCOMPARE(tray.count(), 1);
        QCOMPARE(keep.count(), 1);
        QCOMPARE(hidden.count(), 1);

        settings->setTrayAvailable(true); // no change, no signal
        QCOMPARE(tray.count(), 1);

        settings->setTrayAvailable(false);
        QVERIFY(!settings->keepRunningInTray());
        QCOMPARE(tray.count(), 2);
    }

    void defaultSettingsAreSharedAndPrivate()
    {
        // The default file in a directory of its own: never the user's real settings, and
        // nothing left behind in ~/.qttest.
        QTemporaryDir dataDir;
        QVERIFY(dataDir.isValid());
        qputenv("SUGO_DATA_DIR", QFile::encodeName(dataDir.path()));
        const auto unset = qScopeGuard([] { qunsetenv("SUGO_DATA_DIR"); });
        QCOMPARE(Settings::defaultPath(), dataDir.filePath(QStringLiteral("sugo.conf")));
        QVERIFY(QDir().mkpath(QFileInfo(Settings::defaultPath()).absolutePath()));
        {
            const Settings::Ptr settings = Settings::openDefault();
            QCOMPARE(Settings::openDefault(), settings); // one object for everyone
            settings->setValue(QStringLiteral("SoundVolume"), 7);
            Settings::sync(*settings);
        }
        QCOMPARE(QFile::permissions(Settings::defaultPath()) & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions());
        QCOMPARE(Settings::openDefault()->value(QStringLiteral("SoundVolume")).toInt(), 7);
    }

    void syncKeepsANewFilePrivate()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QFile::Permissions others = QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther;

        // A new file, in a folder that does not exist yet.
        const QString path = dir.filePath(QStringLiteral("sugo/sugo.conf"));
        {
            const Settings::Ptr settings = Settings::open(path);
            settings->setValue(QStringLiteral("SoundVolume"), 7);
            Settings::sync(*settings);
        }
        QVERIFY(QFileInfo::exists(path));
        QCOMPARE(QFile::permissions(path) & others, QFile::Permissions());
        QVERIFY(QFile::permissions(path) & QFile::ReadOwner);
        QCOMPARE(Settings::open(path)->value(QStringLiteral("SoundVolume")).toInt(), 7);

        // An existing file readable by others is made private.
        const QString shared = dir.filePath(QStringLiteral("shared.conf"));
        {
            QFile file(shared);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        QVERIFY(QFile::setPermissions(shared, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther));
        {
            const Settings::Ptr settings = Settings::open(shared);
            settings->setValue(QStringLiteral("SoundVolume"), 9);
            Settings::sync(*settings);
        }
        QCOMPARE(QFile::permissions(shared) & others, QFile::Permissions());
        QCOMPARE(Settings::open(shared)->value(QStringLiteral("SoundVolume")).toInt(), 9);
    }

    void dataDirVariableReplacesDefaultLocation()
    {
        const QByteArray saved = qgetenv("SUGO_DATA_DIR");
        const bool wasSet = qEnvironmentVariableIsSet("SUGO_DATA_DIR");
        QTemporaryDir dir;
        qputenv("SUGO_DATA_DIR", QFile::encodeName(dir.path()));
        QCOMPARE(Settings::defaultPath(), dir.filePath(QStringLiteral("sugo.conf")));
        qunsetenv("SUGO_DATA_DIR");
        QVERIFY(Settings::defaultPath().endsWith(QLatin1String("/sugo/sugo.conf")));
        if (wasSet) {
            qputenv("SUGO_DATA_DIR", saved);
        }
    }

    void onlySoundNamesAndAbsolutePathsArePlayed()
    {
        for (const QString &good : {QStringLiteral("complete"), QStringLiteral("alarm-clock-elapsed"), QStringLiteral("bell.window"),
                                    QStringLiteral("/usr/share/sounds/x.oga")}) {
            QVERIFY2(AppSettings::isUsableSoundName(good), qPrintable(good));
        }
        for (const QString &bad : {QStringLiteral("../../../etc/passwd"), QStringLiteral("a/b"), QStringLiteral("-x"),
                                   QStringLiteral("--file=/etc/passwd"), QStringLiteral(""), QStringLiteral(".hidden"),
                                   QStringLiteral("/usr/share/../../etc/shadow")}) {
            QVERIFY2(!AppSettings::isUsableSoundName(bad), qPrintable(bad));
        }
    }

    void externalControlIsAllowedByDefaultAndRemembered()
    {
        {
            const auto settings = make();
            QVERIFY(settings->allowExternalControl());
            QSignalSpy changed(settings.get(), &AppSettings::allowExternalControlChanged);
            settings->setAllowExternalControl(false);
            QCOMPARE(changed.count(), 1);
        }
        QCOMPARE(stored("AllowExternalControl"), QStringLiteral("false"));
        QVERIFY(!make()->allowExternalControl());
    }

    void canberraGetsTheSoundNameAsOneOptionValue()
    {
        QCOMPARE(AppSettings::canberraArguments(QStringLiteral("complete"), 100),
                 (QStringList{QStringLiteral("--id=complete"), QStringLiteral("--volume=0")}));
        // A name read from the configuration that looks like an option stays a value.
        QCOMPARE(AppSettings::canberraArguments(QStringLiteral("--file=/etc/passwd"), 100).constFirst(),
                 QStringLiteral("--id=--file=/etc/passwd"));
    }

    void canberraGetsTheVolumeInDecibels()
    {
        QCOMPARE(AppSettings::canberraArguments(QStringLiteral("complete"), 50).constLast(), QStringLiteral("--volume=-6"));
        QCOMPARE(AppSettings::canberraArguments(QStringLiteral("complete"), 10).constLast(), QStringLiteral("--volume=-20"));
        QCOMPARE(AppSettings::canberraArguments(QStringLiteral("complete"), 1).constLast(), QStringLiteral("--volume=-40"));
        QCOMPARE(AppSettings::canberraArguments(QStringLiteral("complete"), 150).constLast(), QStringLiteral("--volume=0"));
        // No decimal separator, which strtod would read by the user's locale.
        for (int volume = 1; volume <= 100; ++volume) {
            const QString argument = AppSettings::canberraArguments(QStringLiteral("complete"), volume).constLast();
            QVERIFY2(!argument.contains(QLatin1Char('.')) && !argument.contains(QLatin1Char(',')), qPrintable(argument));
        }
    }

    void alertSoundsFadeInAndOut()
    {
        // A 3 s sound: 300 ms in, 600 ms out.
        QCOMPARE(AppSettings::alertFadeGain(0, 3000), 0.0);
        QCOMPARE(AppSettings::alertFadeGain(150, 3000), 0.5);
        QCOMPARE(AppSettings::alertFadeGain(1500, 3000), 1.0);
        QCOMPARE(AppSettings::alertFadeGain(2700, 3000), 0.5);
        QCOMPARE(AppSettings::alertFadeGain(3000, 3000), 0.0);
        // A 600 ms sound: both fades shrink to 200 ms, so it still reaches full volume.
        QCOMPARE(AppSettings::alertFadeGain(300, 600), 1.0);
        QCOMPARE(AppSettings::alertFadeGain(500, 600), 0.5);
        // The duration is not known yet: only the fade-in applies.
        QCOMPARE(AppSettings::alertFadeGain(150, 0), 0.5);
        QCOMPARE(AppSettings::alertFadeGain(5000, 0), 1.0);
    }
    void soundsAreFoundUnderTheGivenRootsInThemeOrder()
    {
        QTemporaryDir first;
        QTemporaryDir second;
        QVERIFY(first.isValid() && second.isValid());
        const auto touch = [](const QString &file) {
            QVERIFY(QDir().mkpath(QFileInfo(file).absolutePath()));
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
        };
        touch(first.filePath(QStringLiteral("sounds/freedesktop/stereo/bell.oga")));
        touch(second.filePath(QStringLiteral("sounds/ocean/stereo/bell.wav")));
        touch(second.filePath(QStringLiteral("sounds/freedesktop/stereo/complete.oga")));
        const QStringList roots = {first.path(), second.path()};

        // Any theme's file in Ocean comes before freedesktop's, whatever root holds it.
        QCOMPARE(AppSettings::findSoundFile(QStringLiteral("bell"), roots), second.filePath(QStringLiteral("sounds/ocean/stereo/bell.wav")));
        QCOMPARE(AppSettings::findSoundFile(QStringLiteral("complete"), roots),
                 second.filePath(QStringLiteral("sounds/freedesktop/stereo/complete.oga")));
        QVERIFY(AppSettings::findSoundFile(QStringLiteral("missing"), roots).isEmpty());
        QVERIFY(AppSettings::findSoundFile(QStringLiteral("bell"), {}).isEmpty());
        // An absolute path is used as is, if it is a file.
        const QString absolute = first.filePath(QStringLiteral("sounds/freedesktop/stereo/bell.oga"));
        QCOMPARE(AppSettings::findSoundFile(absolute, {}), absolute);
        QVERIFY(AppSettings::findSoundFile(first.filePath(QStringLiteral("nothing.oga")), {}).isEmpty());
    }

    void theSearchRootsIncludeTheSystemSounds()
    {
        const QStringList roots = AppSettings::soundSearchRoots();
        QVERIFY(roots.contains(QStringLiteral("/usr/share")));
        QVERIFY(roots.contains(QStringLiteral("/run/host/usr/share")));
    }

    void everyOfferedSoundIsBundled()
    {
        // The names in the two sound lists of AppSettingsPage.qml; macOS has only these files.
        for (const char *name : {"alarm-clock-elapsed", "complete", "message-new-instant", "dialog-information", "bell",
                                 "completion-success", "service-login", "window-attention", "phone-incoming-call", "message"}) {
            const QString file = AppSettings::findSoundFile(QLatin1String(name), {QStringLiteral(SUGO_BUNDLED_SOUNDS_ROOT)});
            QVERIFY2(!file.isEmpty(), name);
            QVERIFY2(QFileInfo::exists(file + QStringLiteral(".license")), name);
        }
    }

    // The two tests below share the remembered unusable players (process-wide): this one, first,
    // leaves none behind, as nothing in it fails to start or fails where the other one plays.
    void aFileNoPlayerCanPlayDoesNotSilenceTheNextAlert()
    {
#ifndef Q_OS_LINUX
        QSKIP("pw-play and paplay are the Linux players; macOS uses afplay");
#endif
        FakeAlertPlayers players;
        QVERIFY(players.isValid());
        // Both players fail on a broken file: each is tried, for that file only.
        AppSettings::playWithPlayerProgram(players.file("bad.oga"), 50);
        QTRY_COMPARE_WITH_TIMEOUT(players.calls(), (QStringList{QStringLiteral("pw-play bad.oga"), QStringLiteral("paplay bad.oga")}), 3000);
        // pw-play is not given up on: it plays the next, good file.
        AppSettings::playWithPlayerProgram(players.file("bell.oga"), 50);
        QTRY_COMPARE_WITH_TIMEOUT(players.calls(),
                                  (QStringList{QStringLiteral("pw-play bad.oga"), QStringLiteral("paplay bad.oga"), QStringLiteral("pw-play bell.oga")}), 3000);
        QTest::qWait(300); // and nothing else is started for it
        QCOMPARE(players.calls().size(), 3);
    }

    void anAlertPlayerThatCannotPlayHereGivesWayToTheNext()
    {
#ifndef Q_OS_LINUX
        QSKIP("pw-play and paplay are the Linux players; macOS uses afplay");
#endif
        // pw-play is installed but exits at once with an error (no PipeWire daemon): paplay
        // plays the sound instead.
        FakeAlertPlayers players;
        QVERIFY(players.isValid());
        players.setNoPipeWire();
        AppSettings::playWithPlayerProgram(players.file("bell.oga"), 50);
        QTRY_COMPARE_WITH_TIMEOUT(players.calls(), (QStringList{QStringLiteral("pw-play bell.oga"), QStringLiteral("paplay bell.oga")}), 3000);
        // paplay played what pw-play could not: the next alert goes straight to paplay.
        AppSettings::playWithPlayerProgram(players.file("complete.oga"), 50);
        QTRY_COMPARE_WITH_TIMEOUT(players.calls(),
                                  (QStringList{QStringLiteral("pw-play bell.oga"), QStringLiteral("paplay bell.oga"), QStringLiteral("paplay complete.oga")}), 3000);
    }

    void afplayGetsOnlyFormatsItReads()
    {
        QVERIFY(AppSettings::afplayCanPlay(QStringLiteral("/x/bell.wav")));
        QVERIFY(AppSettings::afplayCanPlay(QStringLiteral("/x/bell.AIFF")));
        QVERIFY(AppSettings::afplayCanPlay(QStringLiteral("/x/bell.caf")));
        QVERIFY(!AppSettings::afplayCanPlay(QStringLiteral("/x/bell.oga")));
        QVERIFY(!AppSettings::afplayCanPlay(QStringLiteral("/x/bell.ogg")));
        QVERIFY(!AppSettings::afplayCanPlay(QStringLiteral("/x/bell")));
        QCOMPARE(AppSettings::afplayArguments(QStringLiteral("/x/bell.wav"), 80),
                 (QStringList{QStringLiteral("-v"), QStringLiteral("0.80"), QStringLiteral("/x/bell.wav")}));
        QCOMPARE(AppSettings::afplayArguments(QStringLiteral("/x/bell.wav"), 150).at(1), QStringLiteral("1.00"));
        QCOMPARE(AppSettings::afplayArguments(QStringLiteral("/x/bell.wav"), -5).at(1), QStringLiteral("0.00"));
    }
};

QTEST_GUILESS_MAIN(AppSettingsTest)
#include "tst_appsettings.moc"
