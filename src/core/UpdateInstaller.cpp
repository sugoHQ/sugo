// SPDX-License-Identifier: GPL-3.0-or-later
#include "UpdateInstaller.h"

#include "Localization.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QXmlStreamReader>

#include <utility>

namespace
{
constexpr int kAttachTimeoutMs = 2 * 60 * 1000;
constexpr int kVerifyTimeoutMs = 3 * 60 * 1000;
constexpr int kCopyTimeoutMs = 5 * 60 * 1000;
constexpr int kSmallStepTimeoutMs = 60 * 1000;
constexpr int kCancelWaitMs = 10 * 1000;

/// The first line of a program's output, for an error message.
QString firstLine(const QString &output)
{
    const QString line = output.section(QLatin1Char('\n'), 0, 0).trimmed();
    return line.left(300);
}
} // namespace

// --- AppImage ---------------------------------------------------------------------------------

AppImageInstaller::AppImageInstaller(const QString &appImage, SetPermissions setPermissions, const QString &architecture)
    : m_path(appImage)
    , m_setPermissions(std::move(setPermissions))
    , m_feedKey(feedKeyFor(architecture.isEmpty() ? QSysInfo::buildCpuArchitecture() : architecture))
{
}

QString AppImageInstaller::feedKeyFor(const QString &architecture)
{
    if (architecture == QLatin1String("x86_64")) {
        return QString();
    }
    // The name the AppImage files and linuxdeploy use, not Qt's "arm64".
    return QStringLiteral("linux-") + (architecture == QLatin1String("arm64") ? QStringLiteral("aarch64") : architecture);
}

bool AppImageInstaller::setPermissions(QFileDevice &file, QFileDevice::Permissions permissions) const
{
    return m_setPermissions ? m_setPermissions(file, permissions) : file.setPermissions(permissions);
}

bool AppImageInstaller::canInstall() const
{
    if (m_path.isEmpty()) {
        return false;
    }
    // QSaveFile writes next to the file and renames over it, and refuses a file it could not
    // write to itself (read-only, or owned by someone else) even in a folder it can write to.
    const QFileInfo file(m_path);
    return file.isFile() && file.isWritable() && QFileInfo(file.absolutePath()).isWritable();
}

void AppImageInstaller::install(QSaveFile &download, Done done)
{
    // The download was written next to the AppImage; committing it moves it over the AppImage.
    // Its mode is set first, so the rename brings it along: the AppImage on disk always starts,
    // and one that could not be made executable is never swapped in.
    const QFileDevice::Permissions permissions = QFile::permissions(m_path);
    if (!setPermissions(download, permissions | QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        const QString reason = download.errorString();
        download.cancelWriting(); // the temporary file goes with the QSaveFile
        done(i18n("Cannot install the update: %1", reason));
        return;
    }
    if (!download.commit()) {
        done(i18n("Cannot install the update: %1", download.errorString()));
        return;
    }
    done(QString());
}

bool AppImageInstaller::finish(bool launch, qint64 pid)
{
    if (!launch) {
        return false; // already in place: the next start is the new version
    }
    // The new copy starts once this process is gone: it would otherwise find this one still
    // holding the D-Bus name and hand itself over to it.
    return QProcess::startDetached(QStringLiteral("/bin/sh"),
                                   {QStringLiteral("-c"),
                                    QStringLiteral("while kill -0 \"$1\" 2>/dev/null; do sleep 0.2; done; exec \"$0\""),
                                    m_path, QString::number(pid)});
}

// --- macOS app bundle -------------------------------------------------------------------------

MacBundleInstaller::MacBundleInstaller(const QString &bundle, const Tools &tools)
    : m_bundle(bundle.isEmpty() ? QString() : QDir::cleanPath(bundle))
    , m_tools(tools)
{
}

MacBundleInstaller::~MacBundleInstaller()
{
    cancel();
}

bool MacBundleInstaller::canInstall() const
{
    if (m_bundle.isEmpty()) {
        return false;
    }
    const QFileInfo bundle(m_bundle);
    // Only the disk image's app replaces itself (packaging/macos/build.sh): one built from source
    // only reports a new version, as on Linux, rather than swap the code being worked on for a release.
    return bundle.isDir() && QFileInfo(bundle.absolutePath()).isWritable()
        && installsUpdates(m_bundle + QStringLiteral("/Contents/Info.plist"));
}

QString MacBundleInstaller::downloadPath() const
{
    // In the bundle's folder: the same volume as the copy that will replace the bundle.
    const QFileInfo bundle(m_bundle);
    return bundle.absolutePath() + QStringLiteral("/.") + bundle.fileName() + QStringLiteral(".update.dmg");
}

namespace
{
/// The value of `key` in the top-level dict of an XML Info.plist: the name of its element
/// ("string", "true"...) and, for a string, its text. Empty when the file or the key is not there.
struct PlistValue {
    QString type;
    QString text;
};

PlistValue topLevelValue(const QString &infoPlist, QLatin1String key)
{
    QFile file(infoPlist);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QXmlStreamReader xml(&file);
    int dictDepth = 0;
    bool valueNext = false;
    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const auto name = xml.name();
            if (name == QLatin1String("dict")) {
                ++dictDepth;
                valueNext = false;
            } else if (dictDepth == 1 && name == QLatin1String("key")) {
                valueNext = xml.readElementText() == key;
            } else if (valueNext) {
                const bool isString = name == QLatin1String("string");
                const QString type = name.toString();
                return {type, isString ? xml.readElementText() : QString()};
            }
        } else if (token == QXmlStreamReader::EndElement && xml.name() == QLatin1String("dict")) {
            --dictDepth;
        }
    }
    return {};
}
} // namespace

QString MacBundleInstaller::bundleIdentifier(const QString &infoPlist)
{
    const PlistValue value = topLevelValue(infoPlist, QLatin1String("CFBundleIdentifier"));
    return value.type == QLatin1String("string") ? value.text.trimmed() : QString();
}

bool MacBundleInstaller::installsUpdates(const QString &infoPlist)
{
    return topLevelValue(infoPlist, QLatin1String("SugoInstallsUpdates")).type == QLatin1String("true");
}

void MacBundleInstaller::run(const QString &program, const QStringList &arguments, int timeoutMs, Then then)
{
    auto *process = new QProcess(this);
    m_process = process;
    process->setProcessChannelMode(QProcess::MergedChannels);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    auto timedOut = std::make_shared<bool>(false);
    connect(timer, &QTimer::timeout, process, [process, timedOut]() {
        *timedOut = true;
        process->kill();
    });

    auto handled = std::make_shared<bool>(false);
    const auto end = [this, process, then, handled, timedOut](bool ok) {
        if (*handled) {
            return;
        }
        *handled = true;
        const QString output = *timedOut ? i18n("it did not finish in time") : QString::fromLocal8Bit(process->readAll()).trimmed();
        process->deleteLater();
        if (m_process != process) {
            return; // cancelled
        }
        m_process = nullptr;
        m_stepTimedOut = *timedOut;
        then(ok && !*timedOut, output);
    };
    connect(process, &QProcess::finished, this, [end](int code, QProcess::ExitStatus status) {
        end(status == QProcess::NormalExit && code == 0);
    });
    connect(process, &QProcess::errorOccurred, this, [end](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            end(false); // no finished() follows
        }
    });
    process->start(program, arguments);
    timer->start(timeoutMs);
}

void MacBundleInstaller::install(QSaveFile &download, Done done)
{
    cancel();
    m_done = std::move(done);
    if (!download.commit()) {
        conclude(i18n("Cannot write the update: %1", download.errorString()));
        return;
    }

    QTemporaryDir mount(QDir::tempPath() + QStringLiteral("/sugo-update-XXXXXX"));
    if (!mount.isValid()) {
        conclude(i18n("Cannot open the update: %1", mount.errorString()));
        return;
    }
    mount.setAutoRemove(false); // removed with rmdir() only, so never with a disk image still in it
    m_mountPoint = mount.path();
    m_attaching = true;

    run(m_tools.hdiutil,
        {QStringLiteral("attach"), QStringLiteral("-nobrowse"), QStringLiteral("-readonly"), QStringLiteral("-noautoopen"),
         QStringLiteral("-mountpoint"), m_mountPoint, downloadPath()},
        m_tools.attachTimeoutMs > 0 ? m_tools.attachTimeoutMs : kAttachTimeoutMs, [this](bool ok, const QString &output) {
            m_attaching = false;
            if (ok) {
                m_mounted = true;
                attached();
                return;
            }
            const QString message = i18n("Cannot open the downloaded disk image: %1", firstLine(output));
            if (!m_stepTimedOut) {
                fail(message);
                return;
            }
            // Killed, it may have attached the image all the same: off with it, or the disk image
            // would be deleted while still attached and its mount point left behind.
            detachCutOffAttach([this, message]() {
                fail(message);
            });
        });
}

void MacBundleInstaller::attached()
{
    const QString app = m_mountPoint + QStringLiteral("/Sugo.app");
    if (bundleIdentifier(app + QStringLiteral("/Contents/Info.plist")) != QLatin1String(kBundleIdentifier)) {
        fail(i18n("The downloaded disk image does not contain Sugo."));
        return;
    }
    run(m_tools.codesign, {QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"), app}, kVerifyTimeoutMs,
        [this](bool ok, const QString &output) {
            if (!ok) {
                fail(i18n("The signature of the downloaded Sugo is not valid: %1", firstLine(output)));
                return;
            }
            verified();
        });
}

void MacBundleInstaller::verified()
{
    const QString copy = newBundlePath();
    if (QFileInfo::exists(copy) && !QDir(copy).removeRecursively()) {
        fail(i18n("Cannot remove the earlier update at %1.", copy));
        return;
    }
    run(m_tools.ditto, {m_mountPoint + QStringLiteral("/Sugo.app"), copy}, kCopyTimeoutMs, [this](bool ok, const QString &output) {
        if (!ok) {
            fail(i18n("Cannot copy the update next to %1: %2", m_bundle, firstLine(output)));
            return;
        }
        // Nothing we download is quarantined, but a stray attribute must not stop the new version.
        run(m_tools.xattr, {QStringLiteral("-cr"), newBundlePath()}, kSmallStepTimeoutMs, [this](bool, const QString &) {
            copied();
        });
    });
}

void MacBundleInstaller::copied()
{
    if (!QFileInfo(newBundlePath() + QStringLiteral("/Contents/Info.plist")).isFile()) {
        fail(i18n("Cannot copy the update next to %1: %2", m_bundle, i18n("the copy is incomplete")));
        return;
    }
    detach([this]() {
        conclude(QString());
    });
}

void MacBundleInstaller::detach(const std::function<void()> &then)
{
    if (!m_mounted) {
        then();
        return;
    }
    run(m_tools.hdiutil, {QStringLiteral("detach"), m_mountPoint}, kSmallStepTimeoutMs, [this, then](bool ok, const QString &) {
        if (ok) {
            m_mounted = false;
            then();
            return;
        }
        run(m_tools.hdiutil, {QStringLiteral("detach"), m_mountPoint, QStringLiteral("-force")}, kSmallStepTimeoutMs,
            [this, then](bool forced, const QString &) {
                m_mounted = !forced;
                then();
            });
    });
}

QString MacBundleInstaller::newBundlePath() const
{
    // Next to the bundle, on its volume, so that finish() can rename it in place; hidden, or
    // Spotlight and launchers such as Raycast list "Sugo.app.new" and keep that name afterwards.
    const QFileInfo bundle(m_bundle);
    return bundle.absolutePath() + QStringLiteral("/.") + bundle.fileName() + QStringLiteral(".new");
}

void MacBundleInstaller::detachCutOffAttach(const std::function<void()> &then)
{
    // Whatever the answer: when nothing was attached, there is nothing to detach either, and the
    // mount point is removed only if empty (rmdir), never with an image still in it.
    run(m_tools.hdiutil, {QStringLiteral("detach"), m_mountPoint, QStringLiteral("-force")}, kSmallStepTimeoutMs,
        [then](bool, const QString &) {
            then();
        });
}

void MacBundleInstaller::fail(const QString &message)
{
    detach([this, message]() {
        QDir(newBundlePath()).removeRecursively();
        conclude(message);
    });
}

void MacBundleInstaller::conclude(const QString &error)
{
    removeLeftovers();
    Done done = std::move(m_done);
    m_done = nullptr;
    if (done) {
        done(error);
    }
}

void MacBundleInstaller::removeLeftovers()
{
    QFile::remove(downloadPath());
    if (!m_mounted && !m_mountPoint.isEmpty()) {
        QDir().rmdir(m_mountPoint);
        m_mountPoint.clear();
    }
}

void MacBundleInstaller::cancel()
{
    if (!m_done && !m_process) {
        return;
    }
    m_done = nullptr;
    if (QProcess *process = m_process.data()) {
        m_process = nullptr;
        process->kill();
        process->waitForFinished(kCancelWaitMs);
    }
    // An attach cut off may have attached the image all the same (see detachCutOffAttach()).
    const bool attaching = std::exchange(m_attaching, false);
    if (m_mounted || attaching) {
        QProcess detach;
        detach.start(m_tools.hdiutil, {QStringLiteral("detach"), m_mountPoint, QStringLiteral("-force")});
        const bool detached = detach.waitForFinished(kCancelWaitMs) && detach.exitStatus() == QProcess::NormalExit && detach.exitCode() == 0;
        m_mounted = m_mounted && !detached;
    }
    QDir(newBundlePath()).removeRecursively();
    removeLeftovers();
}

bool MacBundleInstaller::rename(const QString &from, const QString &to) const
{
    return m_tools.rename ? m_tools.rename(from, to) : QDir().rename(from, to);
}

bool MacBundleInstaller::finish(bool launch, qint64 pid)
{
    const QString fresh = newBundlePath();
    if (m_bundle.isEmpty() || !QFileInfo(fresh + QStringLiteral("/Contents/Info.plist")).isFile()) {
        return false;
    }
    // The running bundle steps aside, the new one takes its place; if that fails, the old one
    // comes back. A running program keeps its files open whatever their bundle is called.
    const QString backup = m_bundle + QStringLiteral(".old");
    if (!QFileInfo(m_bundle).isDir()) {
        // Moved or removed while running: a "<bundle>.old" may then be the only copy of Sugo.
        qWarning().noquote() << "Cannot install the update:" << m_bundle << "is not there any more; the update stays at" << fresh;
        return false;
    }
    if (QFileInfo::exists(backup) && !QDir(backup).removeRecursively()) {
        qWarning().noquote() << "Cannot install the update: cannot remove" << backup;
        return false;
    }
    if (!rename(m_bundle, backup)) {
        qWarning().noquote() << "Cannot install the update: cannot move" << m_bundle << "to" << backup;
        return false;
    }
    const qint64 waitFor = m_tools.waitPid > 0 ? m_tools.waitPid : pid;
    if (!rename(fresh, m_bundle)) {
        if (rename(backup, m_bundle)) {
            qWarning().noquote() << "Cannot install the update: cannot move" << fresh << "to" << m_bundle
                                 << "; the installed version was kept";
        } else {
            // No Sugo.app any more: say where it is, it is the only copy left; and try once more
            // when this process has ended.
            qCritical().noquote() << "Cannot install the update, nor put the installed version back: Sugo is now"
                                  << backup << "(rename it to" << m_bundle << "), the update" << fresh;
            restoreWhenEnded(backup, m_bundle, waitFor);
        }
        return false;
    }
    // The old bundle goes, and the new one starts, once this process has ended (a second Sugo
    // would hand itself over to this one).
    const QString script = launch ? QStringLiteral("while kill -0 \"$1\" 2>/dev/null; do sleep 0.2; done; rm -rf \"$2\"; exec \"$3\" -n \"$0\"")
                                  : QStringLiteral("while kill -0 \"$1\" 2>/dev/null; do sleep 0.2; done; rm -rf \"$2\"");
    const bool started = QProcess::startDetached(QStringLiteral("/bin/sh"),
                                                 {QStringLiteral("-c"), script, m_bundle, QString::number(waitFor), backup, m_tools.open});
    return launch && started;
}

bool MacBundleInstaller::restoreInterruptedUpdate(const QString &executableDir, qint64 pid)
{
    if (executableDir.isEmpty()) {
        return false;
    }
    const QDir macos(QDir::cleanPath(executableDir));
    if (macos.dirName() != QLatin1String("MacOS")) {
        return false;
    }
    const QString backup = QDir::cleanPath(macos.absolutePath() + QStringLiteral("/../.."));
    if (!backup.endsWith(QLatin1String(".app.old"))
        || bundleIdentifier(backup + QStringLiteral("/Contents/Info.plist")) != QLatin1String(kBundleIdentifier)) {
        return false;
    }
    const QString bundle = backup.chopped(4);
    const QFileInfo target(bundle);
    if (target.exists() || target.isSymLink()) {
        // The swap went through (finish() removes the backup when the old version has quit), or
        // something else is there: either way not ours to replace.
        return false;
    }
    qWarning().noquote() << QStringLiteral("Sugo runs from %1, left there by an update that did not finish; it goes back to %2 when Sugo quits")
                                .arg(backup, bundle);
    return restoreWhenEnded(backup, bundle, pid);
}

bool MacBundleInstaller::restoreWhenEnded(const QString &backup, const QString &bundle, qint64 pid)
{
    // Checked again then: the bundle may have come back meanwhile, and mv would put the backup in it.
    return QProcess::startDetached(QStringLiteral("/bin/sh"),
                                   {QStringLiteral("-c"),
                                    QStringLiteral("while kill -0 \"$1\" 2>/dev/null; do sleep 0.2; done; "
                                                   "[ -e \"$2\" ] || [ -L \"$2\" ] || mv \"$0\" \"$2\""),
                                    backup, QString::number(pid), bundle});
}
