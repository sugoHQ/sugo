// SPDX-License-Identifier: GPL-3.0-or-later
#include "TelemetryInfo.h"

#include "platform/AppImage.h"

#include <QFile>
#include <QRegularExpression>
#include <QStringList>

namespace TelemetryInfo
{
QString version()
{
    return QStringLiteral(PROJECT_VERSION);
}

QString channel(const QString &version)
{
    return version.contains(QLatin1String("-nightly")) ? QStringLiteral("nightly") : QStringLiteral("stable");
}

QString os()
{
#ifdef Q_OS_MACOS
    return QStringLiteral("macos");
#else
    return QStringLiteral("linux");
#endif
}

QString packaging()
{
#ifdef Q_OS_MACOS
    return QStringLiteral("macos");
#else
    if (!AppImage::path().isEmpty()) {
        return QStringLiteral("appimage");
    }
    if (QFile::exists(QStringLiteral("/.flatpak-info"))) {
        return QStringLiteral("flatpak");
    }
    return QStringLiteral("source");
#endif
}

QString qtVersion()
{
    const QString v = QString::fromLatin1(qVersion());
    static const QRegularExpression form(QStringLiteral("^\\d+\\.\\d+\\.\\d+$"));
    return form.match(v).hasMatch() ? v : QString();
}

QString desktopFrom(const QString &xdgCurrentDesktop)
{
    // A list such as "ubuntu:GNOME" or "KDE".
    const QStringList names = xdgCurrentDesktop.toLower().split(QLatin1Char(':'), Qt::SkipEmptyParts);
    if (names.contains(QLatin1String("kde"))) {
        return QStringLiteral("kde");
    }
    if (names.contains(QLatin1String("gnome"))) {
        return QStringLiteral("gnome");
    }
    return QStringLiteral("other");
}

QString desktop()
{
#ifdef Q_OS_MACOS
    return QStringLiteral("none");
#else
    return desktopFrom(qEnvironmentVariable("XDG_CURRENT_DESKTOP"));
#endif
}

QString distroFrom(const QByteArray &osRelease)
{
    // The same list as the Worker's DISTROS (sugo-sync, worker/src/telemetry.ts).
    static const QStringList known = {
        QStringLiteral("ubuntu"),      QStringLiteral("kubuntu"),
        QStringLiteral("debian"),      QStringLiteral("fedora"),
        QStringLiteral("arch"),        QStringLiteral("manjaro"),
        QStringLiteral("endeavouros"), QStringLiteral("cachyos"),
        QStringLiteral("opensuse-tumbleweed"), QStringLiteral("opensuse-leap"),
        QStringLiteral("linuxmint"),   QStringLiteral("pop"),
        QStringLiteral("neon"),        QStringLiteral("elementary"),
        QStringLiteral("zorin"),       QStringLiteral("nixos"),
        QStringLiteral("gentoo"),      QStringLiteral("void"),
        QStringLiteral("alpine"),      QStringLiteral("rhel"),
        QStringLiteral("centos"),      QStringLiteral("rocky"),
        QStringLiteral("almalinux"),   QStringLiteral("steamos"),
        QStringLiteral("bazzite"),     QStringLiteral("kinoite"),
    };
    for (const QByteArray &raw : osRelease.split('\n')) {
        const QByteArray line = raw.trimmed();
        if (!line.startsWith("ID=")) {
            continue;
        }
        QString id = QString::fromUtf8(line.mid(3)).trimmed();
        if (id.size() >= 2 && (id.startsWith(QLatin1Char('"')) || id.startsWith(QLatin1Char('\''))) && id.endsWith(id.at(0))) {
            id = id.mid(1, id.size() - 2);
        }
        id = id.toLower();
        return known.contains(id) ? id : QStringLiteral("other");
    }
    return QStringLiteral("other");
}

QString distro()
{
#ifdef Q_OS_MACOS
    return QStringLiteral("none");
#else
    // Inside the Flatpak, /etc/os-release is the runtime's: the system's is under /run/host.
    QStringList paths = {QStringLiteral("/etc/os-release"), QStringLiteral("/usr/lib/os-release")};
    if (QFile::exists(QStringLiteral("/.flatpak-info"))) {
        paths = {QStringLiteral("/run/host/os-release"), QStringLiteral("/run/host/etc/os-release"), QStringLiteral("/run/host/usr/lib/os-release")};
    }
    for (const QString &path : std::as_const(paths)) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            return distroFrom(file.read(64 * 1024));
        }
    }
    return QStringLiteral("other");
#endif
}

QString presetsBand(int count)
{
    if (count <= 1) {
        return QStringLiteral("1");
    }
    return count <= 3 ? QStringLiteral("2-3") : QStringLiteral("4+");
}
} // namespace TelemetryInfo
