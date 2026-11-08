// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

/**
 * Carrying over what Sugo 1.0.2 and older left under their app ID, io.github.mineraleyt.sugo
 * (stream.sugo.app since 1.0.3).
 *
 * The database and the settings outside a Flatpak are named after "sugo", not the app ID, so they
 * stay where they are. What moves: the login entry (an XDG autostart file, a macOS launch agent),
 * named after the app ID, and, inside the Flatpak, the whole data folder, which Flatpak keeps
 * per app ID (~/.var/app/<id>).
 */
namespace LegacyAppId
{
/// "io.github.mineraleyt.sugo".
QString id();

/// Renames `<directory>/<old ID><suffix>` to `<directory>/<newId><suffix>` (".desktop", ".plist").
/// If both exist, the old one is removed: the new one is newer. Returns whether there was an old
/// one; the caller then writes the moved entry anew, under its new name.
bool moveLoginEntry(const QString &directory, const QString &suffix, const QString &newId);

/**
 * Inside the Flatpak: copies the old app's data the first time, when there is none of ours.
 *
 * `oldAppDirectory` is the old app's ~/.var/app folder; `dataDirectory` and `configDirectory` are
 * ours (XDG_DATA_HOME, XDG_CONFIG_HOME). Its data/sugo goes to `dataDirectory`/sugo unless that has
 * a database already; its config/sugo to `configDirectory`/sugo unless that has settings already,
 * all but "portal-autostart": the Background portal's answer was for the old app ID, not ours.
 * Existing files are never overwritten, and the old folder is left as it is. Returns whether
 * anything was copied.
 */
bool importFlatpakData(const QString &oldAppDirectory, const QString &dataDirectory, const QString &configDirectory);
} // namespace LegacyAppId
