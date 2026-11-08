// SPDX-License-Identifier: GPL-3.0-or-later
#include "LegacyAppId.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStringList>

Q_LOGGING_CATEGORY(lcLegacyAppId, "sugo.platform.legacyappid")

namespace
{
/// Copies the files under `from` into `to`, keeping the folders; a file already in `to` stays, and
/// the files named in `skip` (paths relative to `from`) are left behind.
bool copyTree(const QString &from, const QString &to, const QStringList &skip = {})
{
    bool copied = false;
    QDirIterator it(from, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString source = it.next();
        if (it.fileInfo().isSymLink()) {
            continue; // only real files: a link could point anywhere
        }
        const QString relative = QDir(from).relativeFilePath(source);
        if (skip.contains(relative)) {
            continue;
        }
        const QString target = to + QLatin1Char('/') + relative;
        if (QFileInfo::exists(target)) {
            continue;
        }
        QDir().mkpath(QFileInfo(target).absolutePath());
        // QFile::copy keeps the permissions: the settings stay readable by the user only.
        if (QFile::copy(source, target)) {
            copied = true;
        } else {
            qCWarning(lcLegacyAppId) << "Cannot copy" << source << "to" << target;
        }
    }
    return copied;
}
} // namespace

namespace LegacyAppId
{
QString id()
{
    return QStringLiteral("io.github.mineraleyt.sugo");
}

bool moveLoginEntry(const QString &directory, const QString &suffix, const QString &newId)
{
    const QString oldPath = directory + QLatin1Char('/') + id() + suffix;
    const QFileInfo old(oldPath);
    if (!old.exists() && !old.isSymLink()) {
        return false;
    }
    const QString newPath = directory + QLatin1Char('/') + newId + suffix;
    if (QFileInfo::exists(newPath)) {
        qCInfo(lcLegacyAppId) << "Removing the login entry of the old app ID" << oldPath;
        if (!QFile::remove(oldPath)) {
            qCWarning(lcLegacyAppId) << "Cannot remove" << oldPath;
        }
        return true;
    }
    qCInfo(lcLegacyAppId) << "Moving the login entry of the old app ID to" << newPath;
    if (!QFile::rename(oldPath, newPath)) {
        qCWarning(lcLegacyAppId) << "Cannot move" << oldPath << "to" << newPath;
        return false;
    }
    return true;
}

bool importFlatpakData(const QString &oldAppDirectory, const QString &dataDirectory, const QString &configDirectory)
{
    bool copied = false;
    const QString oldData = oldAppDirectory + QStringLiteral("/data/sugo");
    if (QFileInfo::exists(oldData + QStringLiteral("/sugo.db"))
        && !QFileInfo::exists(dataDirectory + QStringLiteral("/sugo/sugo.db"))) {
        qCInfo(lcLegacyAppId) << "Copying the data of the old app ID from" << oldData;
        copied = copyTree(oldData, dataDirectory + QStringLiteral("/sugo")) || copied;
    }
    const QString oldConfig = oldAppDirectory + QStringLiteral("/config/sugo");
    if (QFileInfo::exists(oldConfig + QStringLiteral("/sugo.conf"))
        && !QFileInfo::exists(configDirectory + QStringLiteral("/sugo/sugo.conf"))) {
        qCInfo(lcLegacyAppId) << "Copying the settings of the old app ID from" << oldConfig;
        // Not "portal-autostart" (PortalAutostart's state): it remembers that the Background portal
        // was asked to start the old app ID at login, which says nothing about this one. Without it
        // the settings show start at login off, as it is, and turning it on asks for this app ID.
        copied = copyTree(oldConfig, configDirectory + QStringLiteral("/sugo"), {QStringLiteral("portal-autostart")}) || copied;
    }
    return copied;
}
} // namespace LegacyAppId
