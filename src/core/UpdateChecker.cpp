// SPDX-License-Identifier: GPL-3.0-or-later
#include "UpdateChecker.h"

#include "AppImage.h"
#include "AppSettings.h"
#include "Localization.h"
#include "MacBundle.h"
#include "UpdateInstaller.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSignalBlocker>

#include <algorithm>

namespace
{
const QString kReleasesPage = QStringLiteral("https://github.com/sugoHQ/sugo/releases");
const QString kDownloadPrefix = QStringLiteral("https://github.com/sugoHQ/sugo/releases/download/");
/// The project's paths on github.com: its own, and the one it had before it moved to the
/// organization (GitHub redirects it, and the feeds keep using it so that older versions, which
/// trust only that one, can still update).
const QLatin1String kProjectPaths[] = {QLatin1String("/sugoHQ/sugo/"), QLatin1String("/MineraleYT/sugo/")};

bool isProjectPath(const QString &path, const QString &below, Qt::CaseSensitivity cs)
{
    return std::any_of(std::begin(kProjectPaths), std::end(kProjectPaths),
                       [&](QLatin1String project) { return path.startsWith(project + below, cs); });
}
/// An AppImage of Sugo is about 50 MB, the macOS disk image about twice that; a feed announcing
/// more than this is not believed.
constexpr qint64 kMaxDownloadSize = 512 * 1024 * 1024;
constexpr int kFirstCheckDelayMs = 60 * 1000;
constexpr int kCheckIntervalMs = 6 * 60 * 60 * 1000;

struct ParsedVersion {
    QStringList numbers; ///< Digit strings, compared by value with compareDigits().
    QStringList preRelease; ///< Empty for a release.
};

ParsedVersion parseVersion(const QString &raw)
{
    QString cleaned = raw.trimmed();
    if (cleaned.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        cleaned.remove(0, 1);
    }
    const int plusIdx = cleaned.indexOf(QLatin1Char('+'));
    if (plusIdx >= 0) {
        cleaned.truncate(plusIdx); // build metadata does not order versions
    }
    ParsedVersion version;
    const int dashIdx = cleaned.indexOf(QLatin1Char('-'));
    if (dashIdx >= 0) {
        version.preRelease = cleaned.mid(dashIdx + 1).split(QLatin1Char('.'), Qt::SkipEmptyParts);
        cleaned.truncate(dashIdx);
    }
    const auto parts = cleaned.split(QLatin1Char('.'));
    version.numbers.reserve(parts.size());
    static const QRegularExpression digitsOnly(QStringLiteral("^[0-9]+$"));
    for (const QString &p : parts) {
        version.numbers.append(digitsOnly.match(p).hasMatch() ? p : QStringLiteral("0"));
    }
    return version;
}

int sign(qint64 n)
{
    return n > 0 ? 1 : (n < 0 ? -1 : 0);
}

/// Compares two runs of ASCII digits by value, however long they are.
int compareDigits(QStringView a, QStringView b)
{
    while (a.size() > 1 && a.front() == QLatin1Char('0')) {
        a = a.mid(1);
    }
    while (b.size() > 1 && b.front() == QLatin1Char('0')) {
        b = b.mid(1);
    }
    if (a.size() != b.size()) {
        return a.size() > b.size() ? 1 : -1;
    }
    const int c = a.compare(b);
    return c > 0 ? 1 : (c < 0 ? -1 : 0);
}

/// Compares runs of digits by value and everything else as text: "rc2" < "rc10".
int naturalCompare(const QString &a, const QString &b)
{
    int i = 0;
    int j = 0;
    while (i < a.size() && j < b.size()) {
        const auto ascii = [](QChar c) {
            return c >= QLatin1Char('0') && c <= QLatin1Char('9');
        };
        if (ascii(a.at(i)) && ascii(b.at(j))) {
            int ei = i;
            int ej = j;
            while (ei < a.size() && ascii(a.at(ei))) {
                ++ei;
            }
            while (ej < b.size() && ascii(b.at(ej))) {
                ++ej;
            }
            const int c = compareDigits(QStringView(a).mid(i, ei - i), QStringView(b).mid(j, ej - j));
            if (c != 0) {
                return c;
            }
            i = ei;
            j = ej;
        } else {
            if (a.at(i) != b.at(j)) {
                // Letters regardless of case first, then by code unit ("A" < "a"): never both ways.
                const QChar la = a.at(i).toLower();
                const QChar lb = b.at(j).toLower();
                if (la != lb) {
                    return la < lb ? -1 : 1;
                }
                return a.at(i) < b.at(j) ? -1 : 1;
            }
            ++i;
            ++j;
        }
    }
    return sign(qint64(a.size() - i) - qint64(b.size() - j));
}
} // namespace

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
    , m_downloadPrefix(kDownloadPrefix)
    , m_firstCheckDelayMs(kFirstCheckDelayMs)
    , m_checkIntervalMs(kCheckIntervalMs)
{
    const QString bundle = MacBundle::path();
    if (!bundle.isEmpty()) {
        m_installer = std::make_unique<MacBundleInstaller>(bundle);
    } else {
        m_installer = std::make_unique<AppImageInstaller>(AppImage::path());
    }
    m_scheduleTimer.setSingleShot(true);
    connect(&m_scheduleTimer, &QTimer::timeout, this, [this]() {
        m_scheduleTimer.start(m_checkIntervalMs);
        // A check never interrupts a download or an update waiting for the restart. An update
        // already found is looked at again quietly: a newer one replaces it.
        if (m_status == Status::Idle || m_status == Status::UpToDate || m_status == Status::Error) {
            checkForUpdates();
        } else if (m_status == Status::UpdateAvailable) {
            refreshFoundUpdate();
        }
    });
}

UpdateChecker::~UpdateChecker()
{
    abortDownload();
    if (m_installer) {
        m_installer->cancel();
    }
}

void UpdateChecker::setAppImagePathForTesting(const QString &path)
{
    setInstallerForTesting(std::make_unique<AppImageInstaller>(path));
}

void UpdateChecker::setInstallerForTesting(std::unique_ptr<UpdateInstaller> installer)
{
    m_installer = std::move(installer);
}

void UpdateChecker::attach(AppSettings *settings)
{
    const auto follow = [this, settings]() {
        setChannel(settings->updateChannel());
        setAutoCheck(settings->autoCheckUpdates());
        if (canInstall() && m_autoCheck) {
            if (!m_scheduleTimer.isActive()) {
                m_scheduleTimer.start(m_firstCheckDelayMs);
            }
        } else {
            m_scheduleTimer.stop();
        }
    };
    follow();
    connect(settings, &AppSettings::updateChannelChanged, this, follow);
    connect(settings, &AppSettings::autoCheckUpdatesChanged, this, follow);
    // A new channel is looked at straight away (by hand when the checks are not automatic);
    // setChannel() has already dropped what the old one found.
    connect(settings, &AppSettings::updateChannelChanged, this, [this]() {
        if (canInstall() && m_autoCheck && m_status != Status::Downloading && m_status != Status::ReadyToRestart) {
            checkForUpdates();
        }
    });
}

QString UpdateChecker::currentVersion() const
{
    const QByteArray overrideVer = qgetenv("SUGO_OVERRIDE_VERSION");
    if (!overrideVer.isEmpty()) {
        return QString::fromUtf8(overrideVer);
    }
#ifdef PROJECT_VERSION
    return QStringLiteral(PROJECT_VERSION);
#else
    return QCoreApplication::applicationVersion();
#endif
}

bool UpdateChecker::canInstall() const
{
    return m_installer && m_installer->canInstall();
}

QString UpdateChecker::channel() const
{
    return canInstall() ? m_channel : QStringLiteral("stable");
}

void UpdateChecker::setChannel(const QString &channel)
{
    const QString before = this->channel();
    m_channel = channel == QLatin1String("nightly") ? channel : QStringLiteral("stable");
    if (this->channel() == before || m_status == Status::ReadyToRestart) {
        return; // the same feed; or the update is already in place, which cannot be undone
    }
    // What the other channel found, or is still fetching, is not what the user now follows:
    // never install it. A download of it in progress is given up as well.
    if (m_status == Status::Downloading) {
        abortDownload();
        if (m_installer) {
            m_installer->cancel();
        }
        m_status = Status::Idle; // reset() leaves a download alone
    }
    m_downloadUrl.clear();
    reset();
}

QUrl UpdateChecker::feedUrl(const QString &channel)
{
    // The newest release's feed is always at the same address; the nightly's is held by the release "nightly".
    return channel == QLatin1String("nightly")
        ? QUrl(QStringLiteral("https://github.com/sugoHQ/sugo/releases/download/nightly/nightly.json"))
        : QUrl(QStringLiteral("https://github.com/sugoHQ/sugo/releases/latest/download/stable.json"));
}

void UpdateChecker::reset()
{
    if (m_status == Status::Downloading || m_status == Status::ReadyToRestart) {
        return; // the file is being replaced, or has been: forgetting that would lie
    }
    // Forget a request in flight: its reply must not overwrite the state later on.
    if (QNetworkReply *reply = m_reply.data()) {
        m_reply = nullptr;
        reply->abort();
    }
    m_status = Status::Idle;
    m_errorMessage.clear();
    Q_EMIT statusChanged();
}

QString UpdateChecker::safeReleaseUrl(const QString &url)
{
    const QUrl parsed(url, QUrl::StrictMode);
    if (parsed.isValid() && parsed.scheme() == QLatin1String("https")
        && parsed.host().compare(QLatin1String("github.com"), Qt::CaseInsensitive) == 0
        && parsed.userInfo().isEmpty() && parsed.port() == -1
        && isProjectPath(parsed.path(), QString(), Qt::CaseInsensitive)) {
        return parsed.toString();
    }
    return kReleasesPage;
}

QString UpdateChecker::trustedDownloadUrl(const QString &url) const
{
    if (m_downloadPrefix != kDownloadPrefix) {
        return url.startsWith(m_downloadPrefix) ? url : QString(); // a test's local files
    }
    const QUrl parsed(url, QUrl::StrictMode);
    if (parsed.isValid() && parsed.scheme() == QLatin1String("https")
        && parsed.host().compare(QLatin1String("github.com"), Qt::CaseInsensitive) == 0
        && parsed.userInfo().isEmpty() && parsed.port() == -1 && !parsed.hasQuery() && !parsed.hasFragment()
        && isProjectPath(parsed.path(), QStringLiteral("releases/download/"), Qt::CaseSensitive)
        && !parsed.path().contains(QLatin1String("/../"))) {
        return parsed.toString();
    }
    return QString();
}

void UpdateChecker::setStatus(Status s)
{
    if (m_status != s) {
        m_status = s;
        Q_EMIT statusChanged();
    }
}

void UpdateChecker::fail(const QString &message)
{
    m_errorMessage = message;
    setStatus(Status::Error);
}

QNetworkAccessManager *UpdateChecker::network()
{
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    return m_nam;
}

int UpdateChecker::compareVersions(const QString &v1, const QString &v2)
{
    const ParsedVersion p1 = parseVersion(v1);
    const ParsedVersion p2 = parseVersion(v2);
    const int maxLen = std::max(p1.numbers.size(), p2.numbers.size());
    const QString zero = QStringLiteral("0");
    for (int i = 0; i < maxLen; ++i) {
        const int c = compareDigits(i < p1.numbers.size() ? p1.numbers.at(i) : zero, i < p2.numbers.size() ? p2.numbers.at(i) : zero);
        if (c != 0) {
            return c;
        }
    }

    // Same version number: a release beats its pre-releases.
    if (p1.preRelease.isEmpty() || p2.preRelease.isEmpty()) {
        return p1.preRelease.isEmpty() ? (p2.preRelease.isEmpty() ? 0 : 1) : -1;
    }
    const int ids = std::min(p1.preRelease.size(), p2.preRelease.size());
    for (int i = 0; i < ids; ++i) {
        const int c = naturalCompare(p1.preRelease.at(i), p2.preRelease.at(i));
        if (c != 0) {
            return c;
        }
    }
    // "rc.1" < "rc.1.1": more identifiers sort later.
    return sign(qint64(p1.preRelease.size()) - qint64(p2.preRelease.size()));
}

bool UpdateChecker::isPlausibleVersionTag(const QString &tag)
{
    static const QRegularExpression pattern(QStringLiteral("^v?[0-9]+(\\.[0-9]+)*(-[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?(\\+[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?$"));
    return tag.size() <= 64 && pattern.match(tag).hasMatch();
}

void UpdateChecker::scheduleChecksForTesting(int firstDelayMs, int intervalMs)
{
    m_firstCheckDelayMs = firstDelayMs;
    m_checkIntervalMs = intervalMs;
    m_scheduleTimer.start(m_firstCheckDelayMs);
}

void UpdateChecker::checkForUpdates()
{
    if (m_status == Status::Checking || m_status == Status::Downloading || m_status == Status::ReadyToRestart) {
        return;
    }

    m_latestVersion.clear();
    m_releaseUrl.clear();
    m_errorMessage.clear();
    m_downloadUrl.clear();
    setStatus(Status::Checking);
    requestFeed(false);
}

void UpdateChecker::refreshFoundUpdate()
{
    if (m_status != Status::UpdateAvailable) {
        return;
    }
    // The update stays offered (and installable) while the feed is read again.
    requestFeed(true);
}

void UpdateChecker::requestFeed(bool quiet)
{
    QNetworkRequest request(m_feedUrlOverride.isEmpty() ? feedUrl(channel()) : m_feedUrlOverride);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Sugo/%1").arg(currentVersion()));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // The feeds change with every build: never an answer from a cache.
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(10000);

    QNetworkReply *reply = network()->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, quiet]() {
        reply->deleteLater();
        if (reply != m_reply) {
            return; // superseded by reset(), a newer request or the download
        }
        m_reply = nullptr;
        if (quiet) {
            refreshed(reply);
        } else if (reply->error() == QNetworkReply::ContentNotFoundError) {
            fail(i18n("No release has been published on this channel yet."));
        } else if (reply->error() != QNetworkReply::NoError) {
            fail(reply->errorString());
        } else {
            handleFeed(reply->read(64 * 1024));
        }
    });
}

void UpdateChecker::refreshed(QNetworkReply *reply)
{
    if (m_status != Status::UpdateAvailable || reply->error() != QNetworkReply::NoError) {
        return; // a failed refresh keeps the update found before
    }
    const QString version = m_latestVersion;
    const QString releaseUrl = m_releaseUrl;
    const QString downloadUrl = m_downloadUrl;
    const QByteArray sha256 = m_expectedSha256;
    const qint64 size = m_expectedSize;
    m_downloadUrl.clear();
    {
        // Quietly: a feed that cannot be read now must not show an error in passing.
        const QSignalBlocker silent(this);
        handleFeed(reply->read(64 * 1024));
    }
    if (m_status == Status::Error) {
        // A feed that cannot be read now does not take back what a good one offered.
        m_latestVersion = version;
        m_releaseUrl = releaseUrl;
        m_downloadUrl = downloadUrl;
        m_expectedSha256 = sha256;
        m_expectedSize = size;
        m_errorMessage.clear();
        m_status = Status::UpdateAvailable; // as it was: nothing to announce
        return;
    }
    Q_EMIT statusChanged(); // still available (the version and the download may have changed), or up to date
}

void UpdateChecker::handleFeed(const QByteArray &data)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        fail(i18n("Failed to parse release information."));
        return;
    }

    const QJsonObject feed = doc.object();
    const QString version = feed.value(QStringLiteral("version")).toString();
    if (!isPlausibleVersionTag(version)) {
        fail(i18n("No version information found in release."));
        return;
    }
    m_latestVersion = version;
    m_releaseUrl = safeReleaseUrl(feed.value(QStringLiteral("page")).toString());

    if (compareVersions(m_latestVersion, currentVersion()) <= 0) {
        setStatus(Status::UpToDate);
        return;
    }

    if (canInstall()) {
        // The AppImage reads the top-level keys, the macOS app its own object; a feed without
        // that object (a release whose disk image failed to build) is only reported.
        QJsonObject download = feed;
        const QString key = m_installer->feedKey();
        if (!key.isEmpty()) {
            const QJsonValue entry = feed.value(key);
            if (entry.isUndefined() || entry.isNull()) {
                setStatus(Status::UpdateAvailable);
                return;
            }
            download = entry.toObject();
        }
        static const QRegularExpression sha256(QStringLiteral("^[0-9a-f]{64}$"));
        const QString hash = download.value(QStringLiteral("sha256")).toString().toLower();
        const qint64 size = download.value(QStringLiteral("size")).toInteger();
        m_downloadUrl = trustedDownloadUrl(download.value(QStringLiteral("url")).toString());
        if (m_downloadUrl.isEmpty() || !sha256.match(hash).hasMatch() || size <= 0 || size > kMaxDownloadSize) {
            fail(i18n("The update information is incomplete."));
            return;
        }
        m_expectedSha256 = hash.toLatin1();
        m_expectedSize = size;
    }
    // Never downloaded unasked: installUpdate() waits for the user's click.
    setStatus(Status::UpdateAvailable);
}

void UpdateChecker::installUpdate()
{
    if (m_status != Status::UpdateAvailable || !canInstall() || m_downloadUrl.isEmpty()) {
        return;
    }

    // Written next to the AppImage (or the app bundle) and put in place only once complete and
    // verified: a failure leaves the running copy as it was.
    m_saveFile = std::make_unique<QSaveFile>(m_installer->downloadPath());
    if (!m_saveFile->open(QIODevice::WriteOnly)) {
        const QString reason = m_saveFile->errorString();
        m_saveFile.reset();
        fail(i18n("Cannot write the update next to %1: %2", m_installer->target(), reason));
        Q_EMIT installFinished(false, QStringLiteral("write"));
        return;
    }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    m_received = 0;
    m_progress = 0;
    Q_EMIT progressChanged();
    setStatus(Status::Downloading);

    QNetworkRequest request{QUrl(m_downloadUrl)};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Sugo/%1").arg(currentVersion()));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(60000); // without a byte for a minute
    QNetworkReply *reply = network()->get(request);
    m_reply = reply;

    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        if (reply != m_reply || !m_saveFile) {
            return;
        }
        const QByteArray chunk = reply->readAll();
        m_received += chunk.size();
        if (m_received > m_expectedSize) {
            abortDownload();
            fail(i18n("The update is larger than announced."));
            Q_EMIT installFinished(false, QStringLiteral("verify"));
            return;
        }
        m_hash->addData(chunk);
        if (m_saveFile->write(chunk) != chunk.size()) {
            const QString reason = m_saveFile->errorString();
            abortDownload();
            fail(i18n("Cannot write the update: %1", reason));
            Q_EMIT installFinished(false, QStringLiteral("write"));
            return;
        }
        m_progress = qreal(m_received) / qreal(m_expectedSize);
        Q_EMIT progressChanged();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply == m_reply) {
            m_reply = nullptr;
            finishDownload(reply);
        }
    });
}

void UpdateChecker::finishDownload(QNetworkReply *reply)
{
    if (!m_saveFile) {
        return; // already given up on
    }
    if (reply->error() != QNetworkReply::NoError) {
        const QString reason = reply->errorString();
        abortDownload();
        fail(i18n("Could not download the update: %1", reason));
        Q_EMIT installFinished(false, QStringLiteral("download"));
        return;
    }
    if (m_received != m_expectedSize || m_hash->result().toHex() != m_expectedSha256) {
        abortDownload();
        fail(i18n("The downloaded update is damaged; it was not installed."));
        Q_EMIT installFinished(false, QStringLiteral("verify"));
        return;
    }

    m_hash.reset();
    const std::unique_ptr<QSaveFile> download = std::move(m_saveFile);
    // Still Downloading until the installer is done (at once for the AppImage).
    m_installer->install(*download, [this](const QString &error) {
        if (error.isEmpty()) {
            setStatus(Status::ReadyToRestart);
            Q_EMIT installFinished(true, QStringLiteral("install"));
        } else {
            fail(error);
            Q_EMIT installFinished(false, QStringLiteral("install"));
        }
    });
}

void UpdateChecker::abortDownload()
{
    if (m_saveFile) {
        m_saveFile->cancelWriting();
        m_saveFile.reset(); // removes the partial file
    }
    m_hash.reset();
    if (QNetworkReply *reply = m_reply.data()) {
        m_reply = nullptr;
        reply->abort();
    }
}

bool UpdateChecker::launchUpdated() const
{
    if (m_status != Status::ReadyToRestart || !m_installer) {
        return false;
    }
    return m_installer->finish(restartOnQuit(), QCoreApplication::applicationPid());
}
