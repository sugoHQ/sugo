// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QCryptographicHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <memory>

class AppSettings;
class UpdateInstaller;
class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;

/**
 * Looks for a newer Sugo and, when it runs as an AppImage or a macOS app bundle, installs it.
 *
 * Each release channel has a feed on GitHub, a small JSON file published next to the builds:
 * "stable" with the newest release, "nightly" with the newest build of the branch testing
 * (.github/workflows/release.yml and nightly.yml write them):
 *
 *     { "version": "1.0.1-nightly.20261031.42", "url": "https://github.com/sugoHQ/sugo/releases/download/v1.0.1-nightly.20261031.42/Sugo-1.0.1-nightly.20261031.42-x86_64.AppImage",
 *       "sha256": "<64 hex digits>", "size": 45939192, "page": "https://github.com/sugoHQ/sugo/releases/tag/v1.0.1-nightly.20261031.42" }
 *
 * The feed is newer when its version is (compareVersions(); nightly versions are the next
 * release's pre-releases, so they sit between two releases). When asked to install it
 * (installUpdate(); never by itself), an AppImage downloads the file next to itself, checks its
 * size and SHA-256, and only then puts it in its own place; the new version starts with
 * launchUpdated() after the program quits.
 *
 * On macOS the feed's "macos" object ({ "url", "sha256", "size" }) describes the disk image
 * instead. An app bundle in a folder the user can write to (/Applications for an administrator)
 * downloads it next to itself, checks it the same way, then opens it and copies its Sugo.app,
 * once its identifier and signature are right, next to itself as ".Sugo.app.new" (hidden), which takes the
 * bundle's place when the program quits (MacBundleInstaller). A feed without "macos" is only
 * reported, as for the other packages.
 *
 * Other packages (Flatpak, a distribution's, a build from source) only say that an update
 * exists, with a link to it, and only check when asked.
 */
class UpdateChecker : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool checking READ isChecking NOTIFY statusChanged)
    Q_PROPERTY(bool hasUpdate READ hasUpdate NOTIFY statusChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY statusChanged)
    Q_PROPERTY(QString releaseUrl READ releaseUrl NOTIFY statusChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY statusChanged)
    /// True when this copy can replace itself: an AppImage or a macOS app bundle in a folder the
    /// user can write to.
    Q_PROPERTY(bool canInstall READ canInstall CONSTANT)
    /// True when the update found can be installed from here (installUpdate()); false when it is
    /// only reported (a feed without a download for this package).
    Q_PROPERTY(bool installable READ isInstallable NOTIFY statusChanged)
    /// The part of the download received, 0..1, while Downloading.
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)

public:
    enum class Status {
        Idle,
        Checking,
        UpToDate,
        UpdateAvailable,
        Downloading, ///< Also while a macOS update is checked and copied, at 100%.
        ReadyToRestart, ///< The new version is in place (on macOS: when the program quits); it starts with the next launch.
        Error
    };
    Q_ENUM(Status)

    explicit UpdateChecker(QObject *parent = nullptr);
    ~UpdateChecker() override;

    /// Follows the update channel and the automatic checks of `settings`; an AppImage with
    /// automatic checks on looks a minute after the start and every six hours.
    void attach(AppSettings *settings);

    Status status() const { return m_status; }
    bool isChecking() const { return m_status == Status::Checking; }
    bool hasUpdate() const { return m_status == Status::UpdateAvailable; }
    QString currentVersion() const;
    QString latestVersion() const { return m_latestVersion; }
    QString releaseUrl() const { return m_releaseUrl; }
    QString errorMessage() const { return m_errorMessage; }
    bool canInstall() const;
    bool isInstallable() const { return m_status == Status::UpdateAvailable && canInstall() && !m_downloadUrl.isEmpty(); }
    qreal progress() const { return m_progress; }

    /// "stable" or "nightly"; anything else reads as "stable". Only a copy that installs updates
    /// follows nightly.
    QString channel() const;
    /// A change of channel forgets what the old one found or is fetching, and gives up a download
    /// of it (status Idle); an update already in place (ReadyToRestart) stays.
    void setChannel(const QString &channel);
    /// Look for updates by itself (only a copy that installs them); what it finds is installed
    /// only when asked.
    void setAutoCheck(bool enabled) { m_autoCheck = enabled; }

    Q_INVOKABLE void checkForUpdates();
    /// Downloads and puts in place the update a check found.
    Q_INVOKABLE void installUpdate();
    Q_INVOKABLE void reset();
    /// Marks that the new version must start when the program quits (see launchUpdated()), or
    /// no longer must (the quit was cancelled).
    Q_INVOKABLE void setRestartOnQuit(bool restart) { m_restartOnQuit = restart; }
    bool restartOnQuit() const { return m_restartOnQuit && m_status == Status::ReadyToRestart; }
    /// Finishes an installed update (on macOS the new bundle takes the old one's place) and, when
    /// asked to restart, starts the new version once this process has ended. Call at the very end
    /// of main(). True when the new version was started.
    bool launchUpdated() const;

    /// Compare two version strings ("v1.2.3", "1.2.3-rc1", ...), semver style: a release is
    /// newer than any pre-release of the same version, pre-releases compare naturally
    /// ("rc2" < "rc10"), build metadata ("+...") is ignored. Returns:
    /// > 0 if v1 > v2
    /// 0 if v1 == v2
    /// < 0 if v1 < v2
    static int compareVersions(const QString &v1, const QString &v2);

    /// True for a tag that reads as a version ("v1.2.3", "1.2.0-rc.1+b5"), at most 64 characters:
    /// anything else from the server is not shown.
    static bool isPlausibleVersionTag(const QString &tag);

    /// `url` if it is an https link into this project on github.com, else the project's releases page.
    static QString safeReleaseUrl(const QString &url);

    /// The feed of `channel` (see the class description).
    static QUrl feedUrl(const QString &channel);

    // Tests: the AppImage to replace (instead of AppImage::path()), or any installer, the feed to
    // read (instead of the channel's), and where downloads may come from (instead of this
    // project's GitHub releases).
    void setAppImagePathForTesting(const QString &path);
    void setInstallerForTesting(std::unique_ptr<UpdateInstaller> installer);
    void setFeedUrlForTesting(const QUrl &url) { m_feedUrlOverride = url; }
    void setDownloadPrefixForTesting(const QString &prefix) { m_downloadPrefix = prefix; }
    /// Tests: the automatic checks with other delays (as attach() starts them).
    void scheduleChecksForTesting(int firstDelayMs, int intervalMs);

Q_SIGNALS:
    void statusChanged();
    void progressChanged();
    /// An installation asked with installUpdate() ended: put in place (`ok`, stage "install"), or
    /// failed at `stage`: "write" (the file next to the program), "download", "verify" (size or
    /// checksum) or "install". For the anonymous usage statistics.
    void installFinished(bool ok, const QString &stage);

private:
    void setStatus(Status s);
    void fail(const QString &message);
    /// Reads the feed again while an update is offered; a newer one replaces it.
    void refreshFoundUpdate();
    /// Fetches the channel's feed; `quiet` for refreshFoundUpdate().
    void requestFeed(bool quiet);
    void refreshed(QNetworkReply *reply);
    void handleFeed(const QByteArray &data);
    void finishDownload(QNetworkReply *reply);
    void abortDownload();
    QNetworkAccessManager *network();
    /// `url` if it points into the download prefix over https (or anything under a test prefix).
    QString trustedDownloadUrl(const QString &url) const;

    Status m_status = Status::Idle;
    QString m_latestVersion;
    QString m_releaseUrl;
    QString m_errorMessage;
    QNetworkAccessManager *m_nam = nullptr;
    QPointer<QNetworkReply> m_reply; ///< The request in flight; replies of older requests are ignored.

    QString m_channel = QStringLiteral("stable");
    bool m_autoCheck = false;
    bool m_restartOnQuit = false;
    std::unique_ptr<UpdateInstaller> m_installer;
    QUrl m_feedUrlOverride;
    QString m_downloadPrefix;
    QTimer m_scheduleTimer;
    int m_firstCheckDelayMs;
    int m_checkIntervalMs;

    // The update the last check found, and its download.
    QString m_downloadUrl;
    QByteArray m_expectedSha256; ///< Lower-case hex.
    qint64 m_expectedSize = 0;
    std::unique_ptr<QSaveFile> m_saveFile;
    std::unique_ptr<QCryptographicHash> m_hash;
    qint64 m_received = 0;
    qreal m_progress = 0;
};
