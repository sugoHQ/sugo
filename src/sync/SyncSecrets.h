// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>

#include <functional>
#include <optional>

#include "SyncCrypto.h"

class SecretStore;

/**
 * Sugo Sync's secrets, kept in ONE keyring item, `sync-secrets`, as a JSON object:
 *
 *     { "v": 1, "refreshToken": "…", "deviceKey": "<Base64>", "dataKey": "<Base64>", "recoveryCode": "XXXX-…" }
 *
 * One item, because macOS asks again for each item after an update of an ad hoc signed build,
 * and because one write is atomic: there are never two items of which one is saved and the
 * other is not. `dataKey` is missing while the device waits for the key, `recoveryCode` on
 * devices that did not create it. The access token never reaches the keyring.
 *
 * This is the only place that touches the keyring for Sugo Sync. The keyring is read once, by
 * load(); requests read the copy kept here. Writes are rare: sign-in, key received or created,
 * new recovery code, token renewal (about once an hour).
 *
 * Keyring calls are synchronous and may block while the system asks the user: call load() and
 * probe() only when the user asked for sync (never while starting the program while signed
 * out), and never from a loop.
 */
class SyncSecrets
{
public:
    static constexpr int Version = 1;
    static QString itemKey() { return QStringLiteral("sync-secrets"); }
    static QString probeKey() { return QStringLiteral("sync-keyring-probe"); }

    struct Values {
        QString refreshToken;
        SecretBytes deviceKey;    ///< X25519 private key (32 bytes); its public key is derived.
        SecretBytes dataKey;      ///< K (32 bytes); empty while waiting for it.
        SecretBytes recoveryCode; ///< The 16 raw bytes; empty if this device did not create it.
        /// A new recovery code on its way to the server (SyncAccount::regenerateRecoveryCode()): kept
        /// so that a restart sends it again instead of losing it. Older versions ignore it.
        SecretBytes pendingRecoveryCode;

        Values copy() const;
        bool isEmpty() const
        {
            return refreshToken.isEmpty() && deviceKey.isEmpty() && dataKey.isEmpty() && recoveryCode.isEmpty()
                && pendingRecoveryCode.isEmpty();
        }
    };

    enum class LoadResult {
        Loaded,       ///< values() holds what the keyring had.
        Empty,        ///< No item (or an empty one).
        Unreadable,   ///< An item that is not our JSON, or holds invalid keys.
        NewerVersion, ///< Written by a newer Sugo ("v" above Version); left alone.
        Failed,       ///< The keyring did not answer (missing, locked, access refused).
    };

    /// `store` is not owned and must outlive this object.
    explicit SyncSecrets(SecretStore *store);

    /**
     * Whether the keyring can keep a secret: writes a random value to `sync-keyring-probe` and
     * removes it. Only a write proves it: on macOS reading a missing secret is not an error.
     * Never leaves the probe item behind when the keyring works. Remembered in available().
     */
    bool probe();
    /// The result of the last probe() (true until one fails).
    bool available() const { return m_available; }

    /// Reads `sync-secrets` once and keeps its values. On anything but Loaded, values() is empty.
    LoadResult load();

    /// The values as last written (or loaded). The refresh token may be newer: refreshToken().
    const Values &values() const { return m_values; }
    /// The refresh token to use: one whose write failed (unsavedRefreshToken()) if any, else the saved one.
    QString refreshToken() const;
    /// A refresh token the server gave that could not be written to the keyring yet. The next
    /// successful write includes it.
    bool hasUnsavedRefreshToken() const { return !m_unsavedRefreshToken.isEmpty(); }

    /**
     * Rewrites the whole item with the current values (including an unsaved refresh token)
     * changed by `change`. Only after a successful write do values() take the new values.
     * When the write fails, values() stay as they were, but a new refresh token is kept as
     * unsaved (the server already spent the old one), so it is used and written next time.
     */
    bool update(const std::function<void(Values &)> &change);
    /// update() of the refresh token alone.
    bool setRefreshToken(const QString &token);
    /// Replaces every value (a new sign-in).
    bool replace(Values values);

    /// Removes the item (write of an empty secret) and forgets every value, even if the write fails.
    bool clear();

    /// The JSON text of the item.
    static QByteArray toJson(const Values &values);
    static std::optional<Values> fromJson(const QByteArray &json, LoadResult *result = nullptr);

private:
    bool write(Values values);

    SecretStore *m_store;
    Values m_values;
    QString m_unsavedRefreshToken;
    bool m_available = true;
};
