// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "SecretStore.h"

#include <QString>
#include <QStringList>

#include <optional>

/**
 * What SecretHandoff needs of a keyring besides read() and write() (MacKeychainStore).
 */
class SecretHandoffStore : public SecretStore
{
public:
    /// The keys this application has stored, without their secrets (listing never makes the
    /// system ask the user, even for items another build wrote). Nothing on failure.
    virtual std::optional<QStringList> keys() = 0;
    /// The secret only if the system gives it without asking the user (an item this build wrote,
    /// or one the user let it read with "Always Allow"); nothing otherwise, and on any failure.
    virtual std::optional<QString> readWithoutAsking(const QString &key) = 0;
};

/**
 * Carries the secrets of an ad hoc signed macOS build over to the build that updates it.
 *
 * A Keychain item stays bound to the code hash of the build that wrote it: an update (another
 * hash) may list it but not read it nor delete it without macOS asking the user ("Sugo wants to
 * use your confidential information..."), at every update. Only a Team ID (a paid Developer ID)
 * would let the builds share their items. So the build that installed an update, when it quits,
 * reads its own secrets (it may, without asking), writes them to a file only the user can read
 * and deletes its items; the next build to start writes them back as its own items and deletes
 * the file. The file is the only copy of the secrets until then: it is never dropped because of
 * its age, only once its secrets are back in the keyring.
 *
 * Only updates installed by Sugo itself carry the secrets over: after a manual install macOS asks
 * as before. Neither function ever makes the system ask the user.
 */
namespace SecretHandoff
{
/// Where the secrets wait for the next build: next to the settings and the database
/// ("<data dir>/sugo/keychain-handoff.json"). Empty in an isolated run (SUGO_DATA_DIR), which
/// shares the user's Keychain but must not take the user's secrets away.
QString defaultPath();

/// At the end of a build that installed an update: moves to `path` (mode 0600) every secret of
/// `store` it can read without asking, adding them to those already waiting there, then deletes
/// their items. Nothing is deleted unless the file was written; a file at `path` that cannot be
/// read, or is not a handoff file, is left as it is and nothing is moved. Returns how many were moved.
int handOver(SecretHandoffStore &store, const QString &path);

/// At the start: writes the secrets waiting at `path` into `store` as this build's items, and
/// deletes the file once none is left. A secret `store` already holds is left as it is (the user
/// typed it again since, while the file was the only copy: the newer one wins). A secret that
/// cannot be written stays in the file for the next start, unless `store` still has an item for
/// it (the old build could not delete it: the user will be asked for that one, as before).
/// Returns how many were written.
int takeBack(SecretHandoffStore &store, const QString &path);
} // namespace SecretHandoff
