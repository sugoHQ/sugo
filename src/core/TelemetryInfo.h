// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>

/**
 * What the anonymous usage statistics (Telemetry) say about this copy of Sugo and the system it
 * runs on. Every value is one of a few fixed words, or a version number: nothing that names the
 * user or the computer. The Worker accepts only these values (sugo-sync, worker/src/telemetry.ts);
 * change both together.
 */
namespace TelemetryInfo
{
/// The version Sugo was built as (PROJECT_VERSION, e.g. "1.2.0" or "1.2.1-nightly.20261108.1").
QString version();
/// "nightly" for a nightly build (its version has "-nightly"), else "stable".
QString channel(const QString &version);
/// "linux" or "macos".
QString os();
/// "appimage", "flatpak", "macos" (the app bundle) or "source" (built from source, or a distribution's package).
QString packaging();
/// The Qt version Sugo runs with, "X.Y.Z" (empty if it has another form).
QString qtVersion();

/// "kde", "gnome", "other", or "none" (macOS).
QString desktop();
/// desktop() for a value of $XDG_CURRENT_DESKTOP (tests).
QString desktopFrom(const QString &xdgCurrentDesktop);

/// The Linux distribution by its os-release ID, if it is one of a known list, else "other"; "none" on macOS.
QString distro();
/// distro() for the contents of an os-release file (tests).
QString distroFrom(const QByteArray &osRelease);

/// The number of timers, in bands: "1", "2-3" or "4+".
QString presetsBand(int count);
} // namespace TelemetryInfo
