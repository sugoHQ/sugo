// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFileDevice>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class QProcess;
class QSaveFile;

/**
 * The part of an update that depends on the package: where the download goes, how it is put in
 * place and how the new version starts. UpdateChecker does the rest (the feed, the download and
 * its checks) for all of them.
 *
 * - AppImageInstaller: the downloaded file replaces the AppImage at once.
 * - MacBundleInstaller: the disk image is opened, its Sugo.app checked and copied next to the
 *   running bundle as ".Sugo.app.new" (hidden); it takes the bundle's place when the program quits.
 */
class UpdateInstaller
{
public:
    /// Called once when install() ends: an empty string on success, else what went wrong.
    using Done = std::function<void(const QString &error)>;

    virtual ~UpdateInstaller() = default;

    /// True when this copy can replace itself (it exists, in a folder the user can write to; an
    /// AppImage must also be writable itself).
    virtual bool canInstall() const = 0;
    /// The feed object that describes this package's download ("macos", "linux-aarch64"), or
    /// empty for the feed's top-level keys (the x86_64 AppImage's).
    virtual QString feedKey() const = 0;
    /// What an update replaces, for messages: the AppImage or the .app bundle.
    virtual QString target() const = 0;
    /// Where the download is written (through a QSaveFile).
    virtual QString downloadPath() const = 0;
    /// Puts the complete, verified download in place. `download` is used only before this
    /// returns; `done` may be called before it returns or later.
    virtual void install(QSaveFile &download, Done done) = 0;
    /// Gives up an install in progress (`done` is not called) and removes what it left.
    virtual void cancel() { }
    /// At the end of the program, after a successful install: finishes it, and when `launch`
    /// starts the new version once process `pid` has ended. True when the new version was started.
    virtual bool finish(bool launch, qint64 pid) = 0;
};

class AppImageInstaller : public UpdateInstaller
{
public:
    /// Sets the mode of the new AppImage; tests make it fail. Empty for QFileDevice::setPermissions().
    using SetPermissions = std::function<bool(QFileDevice &file, QFileDevice::Permissions permissions)>;

    /// `architecture` is that of the AppImage, as QSysInfo::buildCpuArchitecture() names it;
    /// empty for this program's own.
    explicit AppImageInstaller(const QString &appImage, SetPermissions setPermissions = {}, const QString &architecture = QString());

    /// The feed object of the AppImage built for `architecture` (QSysInfo::buildCpuArchitecture()):
    /// the top-level keys (empty) for x86_64, which Sugo 1.0.x AppImages read and which must keep
    /// meaning x86_64; "linux-aarch64" for arm64; "linux-<architecture>" for any other, which no
    /// feed has, so such an AppImage only reports updates and never takes another one's file.
    static QString feedKeyFor(const QString &architecture);

    bool canInstall() const override;
    QString feedKey() const override { return m_feedKey; }
    QString target() const override { return m_path; }
    QString downloadPath() const override { return m_path; }
    void install(QSaveFile &download, Done done) override;
    bool finish(bool launch, qint64 pid) override;

private:
    bool setPermissions(QFileDevice &file, QFileDevice::Permissions permissions) const;

    QString m_path;
    SetPermissions m_setPermissions;
    QString m_feedKey;
};

/// The programs MacBundleInstaller uses; tests put stand-ins in their place.
struct MacUpdateTools {
    QString hdiutil = QStringLiteral("/usr/bin/hdiutil");
    QString codesign = QStringLiteral("/usr/bin/codesign");
    QString ditto = QStringLiteral("/usr/bin/ditto");
    QString xattr = QStringLiteral("/usr/bin/xattr");
    QString open = QStringLiteral("/usr/bin/open");
    /// How long hdiutil attach may take (tests shorten it); 0 for the default, two minutes.
    int attachTimeoutMs = 0;
    /// The process finish() waits for instead of the one it is given (tests); 0 for none.
    qint64 waitPid = 0;
    /// Renames the bundles in finish(); empty for QDir::rename (tests make it fail).
    std::function<bool(const QString &from, const QString &to)> rename;
};

class MacBundleInstaller : public QObject, public UpdateInstaller
{
public:
    using Tools = MacUpdateTools;

    /// The bundle identifier a downloaded Sugo.app must have.
    static constexpr const char *kBundleIdentifier = "stream.sugo.app";

    explicit MacBundleInstaller(const QString &bundle, const Tools &tools = Tools());
    ~MacBundleInstaller() override;

    bool canInstall() const override;
    QString feedKey() const override { return QStringLiteral("macos"); }
    QString target() const override { return m_bundle; }
    QString downloadPath() const override;
    void install(QSaveFile &download, Done done) override;
    void cancel() override;
    bool finish(bool launch, qint64 pid) override;

    /// Where the checked copy of the new version waits for the restart: ".<bundle>.new" in the
    /// bundle's folder, hidden so that Spotlight and launchers do not list it as an app.
    QString newBundlePath() const;

    /// The CFBundleIdentifier of an XML property list, or an empty string.
    static QString bundleIdentifier(const QString &infoPlist);
    /// Whether the Info.plist says this bundle installs updates itself (key SugoInstallsUpdates,
    /// true only in the disk image's build: -DSUGO_INSTALLS_UPDATES=ON).
    static bool installsUpdates(const QString &infoPlist);
    /// An update cut off between the two renames of finish() (a crash, a power cut, a rollback
    /// that failed) leaves Sugo only at "<name>.app.old", which the Dock still opens. When the
    /// program runs from there (`executableDir` is its Contents/MacOS) and "<name>.app" is not
    /// there, the folder gets its name back once process `pid` has ended: renamed now, it would
    /// break what Qt still loads from the bundle. True when that was arranged.
    static bool restoreInterruptedUpdate(const QString &executableDir, qint64 pid);

private:
    static bool restoreWhenEnded(const QString &backup, const QString &bundle, qint64 pid);
    using Then = std::function<void(bool ok, const QString &output)>;
    void run(const QString &program, const QStringList &arguments, int timeoutMs, Then then);
    void attached();
    void verified();
    void copied();
    void fail(const QString &message);
    void detach(const std::function<void()> &then);
    /// After hdiutil attach was killed: `hdiutil detach -force` of the mount point, then `then`.
    void detachCutOffAttach(const std::function<void()> &then);
    void conclude(const QString &error);
    void removeLeftovers();
    bool rename(const QString &from, const QString &to) const;

    QString m_bundle;
    Tools m_tools;
    Done m_done;
    QPointer<QProcess> m_process; ///< The step running; finished steps of a cancelled install are ignored.
    QString m_mountPoint;
    bool m_mounted = false;
    bool m_attaching = false; ///< hdiutil attach is running: if it is killed, the image may be attached.
    bool m_stepTimedOut = false; ///< The last step that ended was killed for taking too long.
};
