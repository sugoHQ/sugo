// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <memory>
#include <optional>

#include "Settings.h"
#include "SyncApi.h"

class SecretBytes;
class SyncSecrets;

/**
 * The Sugo Sync account of this computer: sign-in with a code shown on sugo.stream, and the
 * data key K.
 *
 * - The first device of an account creates K and the recovery code, and uploads K sealed with
 *   the code (recoveryCodeCreated()).
 * - Any later device waits for a linked device to approve it: both show verificationCode() of
 *   the two public keys, and the linked one sends K in an authenticated crypto_box. The user
 *   confirms on both: on the linked device by approving, here with confirmVerificationCode().
 *   Until then a key that arrives is held, not opened: else a compromised server could list a
 *   device of its own and hand this one a key it knows, with nothing for the user to compare. While
 *   waiting, the device list is read again every minute: the approver stays the same while it
 *   is still listed, holds the key and has the public key the code covers (the code does not
 *   change under the user who compares it); else the code and its approver move to the device
 *   holding the key that was seen last (one that signed out does not leave the wait stuck on
 *   a code of its own). The new device takes K only from the device the code was computed
 *   with (a server that hands it a key of its own is caught: keyRejected(), and the key is
 *   not confirmed). A confirmation holds only for the approver it was given for: when the
 *   code moves to another device, it lapses.
 * - A device can also open K with the recovery code (useRecoveryCode()).
 * - K is stored before the server is told (DELETE /v1/key), so it can never be lost between.
 * - The recovery code stays in the keyring of the device that made it; it can be shown again
 *   only while the server's copy is still the one this device wrote (recoveryCodeShowable()).
 * - signOut() frees the device's slot on the server if it can, and always forgets everything here.
 *
 * Every "store X in the keyring" is a SyncSecrets write of the one `sync-secrets` item. The
 * Settings group `Sync` holds what is not secret: DeviceId, UserId, DeviceName, KeyConfirmed,
 * RecoveryUpdatedAt (the group is removed on sign-out, with whatever else others keep there);
 * `SyncSignIn/DeviceName`, the name last signed in with, stays for the next sign-in.
 *
 * Rule S4: before sign-in sends anything, a test write proves the keyring can keep secrets;
 * without a keyring there is no sync (no file fallback), the same on every system.
 */
class SyncAccount : public QObject
{
    Q_OBJECT

public:
    enum class Phase {
        SignedOut,     ///< Not linked.
        SigningIn,     ///< A code is shown; polling until the site approves it.
        WaitingForKey, ///< Linked, without the data key yet.
        Ready,         ///< Linked, with the data key.
    };
    Q_ENUM(Phase)

    /// A device of the account (GET /v1/devices).
    struct Device {
        QString id;
        QString name;
        QString platform;
        QDateTime createdAt;
        QDateTime lastSeen;    ///< Invalid while approved on the site but never collected.
        bool pending = true;   ///< No data key yet.
        bool current = false;  ///< This computer.
        QByteArray publicKey;  ///< Raw, 32 bytes.

        /// id, name, platform, createdAt, lastSeen, pending, current (for QML).
        QVariantMap toVariantMap() const;
    };

    /// `api` and `secrets` are not owned and must outlive this object.
    SyncAccount(SyncApi *api, SyncSecrets *secrets, Settings::Ptr settings, QObject *parent = nullptr);
    ~SyncAccount() override;

    /// "linux" or "macos", as the server knows platforms.
    static QString platformName();

    Phase phase() const { return m_phase; }
    /// The Settings say this computer is linked (it may still lack the secrets: restore()).
    bool isLinked() const;
    /// False after a failed test write of the keyring (sign-in is then impossible).
    bool keyringAvailable() const { return m_keyringAvailable; }

    QString deviceId() const;
    QString userId() const;
    QString deviceName() const;
    /// The name this computer last signed in with, also after sign-out (empty if it never did).
    QString lastDeviceName() const;

    /// This device's public key (empty when signed out).
    QByteArray publicKey() const { return m_publicKey; }
    /// K (empty until phase() is Ready).
    const SecretBytes &dataKey() const;

    // --- While signing in -------------------------------------------------------------------------
    QString userCode() const { return m_userCode; }
    QUrl verificationUri() const { return m_verificationUri; }
    QUrl verificationUriComplete() const { return m_verificationUriComplete; }
    /// The site could not link this device: the account has its most devices.
    bool deviceLimitReached() const { return m_deviceLimitReached; }

    // --- Key ------------------------------------------------------------------------------------------
    /// While waiting for the key: the code both devices show, "XXXX-XXXX-XXXX-XXXX" (empty if no device can approve).
    QString verificationCode() const { return m_verificationCode; }
    /// The name of the device the code was computed with (empty if none).
    QString approverName() const { return m_approver.name; }
    /// The user confirmed that the approver shows verificationCode(): its key will be taken.
    bool verificationConfirmed() const;
    /// The devices of the last list.
    QList<Device> devices() const { return m_devices; }
    QVariantList devicesAsVariantList() const;

    // --- Recovery code -------------------------------------------------------------------------------
    /// This device holds the recovery code the server has now (refreshRecoveryStatus()).
    bool recoveryCodeShowable() const { return m_recoveryShowable; }
    /// The code as the user writes it, from memory (no keyring access); empty if not showable.
    QString recoveryCode() const;

    /// One second in ms for every wait (polling, retries); tests make it short.
    void setTimeUnitMs(int ms) { m_unitMs = ms; }

public Q_SLOTS:
    /**
     * After SyncSecrets::load() of a linked computer: takes the secrets in memory and resumes
     * where the account was. False (nothing changes) when the Settings say signed out or the
     * secrets lack the refresh token or device key: then they are lost, signOutLocally().
     */
    bool restore();

    /**
     * Proves the keyring (keyringUnavailable() and nothing sent if it fails), makes the device's
     * key pair, stores it, and asks the server for a code: codeReady(), then polling.
     */
    void startSignIn(const QString &deviceName);
    /// Stops polling and removes the secrets of the sign-in.
    void cancelSignIn();

    /// Polls for the key every 5 s while the window is active, every 30 s otherwise.
    void setWindowActive(bool active);
    /// Starts waiting for the key again (after keyRejected(), or when another device was linked).
    void waitForKey();
    /**
     * The user saw `code` on the approver too. Taken only while it is verificationCode(): a code
     * the user read before it moved to another device confirms nothing. A key already held is
     * opened now; else the next one from that approver is. False if not taken.
     */
    bool confirmVerificationCode(const QString &code);

    /// Sugo-Pending-Devices: lists the devices and asks about each new one (approvalRequested());
    /// with none waiting, lists them again if the list still shows one waiting.
    void checkPendingDevices(int count);
    /// Sends K to a pending device, boxed for its public key by this device's key. Only for the
    /// key whose verification code was shown (approvalRequested()): if the list now shows another
    /// one, approvalFailed() and the device is asked about again, with the new code.
    void approveDevice(const QString &id);
    /// Asks no more about this device until the program restarts, or askAgain().
    void ignoreDevice(const QString &id);
    /// Asks about this pending device again (approvalRequested), even if it was ignored.
    void askAgain(const QString &id);

    /// While waiting for the key: opens the server's recovery copy with `text` (recoveryCodeWrong()
    /// if it does not), stores K, confirms it.
    void useRecoveryCode(const QString &text);

    /// Lists the devices again (devicesChanged()).
    void refreshDevices();
    /// Disconnects another device of the account (this one signs out instead).
    void disconnectDevice(const QString &id);

    /// Asks the server whose recovery copy it holds (recoveryCodeShowableChanged()).
    void refreshRecoveryStatus();
    /**
     * A new recovery code replaces the old one, which stops working: recoveryCodeCreated(). Without a
     * certain answer (no answer, a server error, a rate limit, an unreadable reply) the same copy is sent again, with the
     * same code, until there is one: the first try may have reached the server, which would then hold
     * a copy of a code nobody knows (recoveryCodeRetrying() on the first such failure).
     */
    void regenerateRecoveryCode();

    /// Disconnects this device on the server if it can, then signOutLocally(): signedOut().
    void signOut();

    /// Forgets everything here: secrets item, Settings group `Sync`, pending requests.
    void signOutLocally();

    /// The keyring refused to delete the secrets item at the last sign-out: the keys are still
    /// there, though this device is signed out.
    bool keyringWipePending() const;
    /// Tries again to delete the item; the mark stays until it works.
    void retryKeyringWipe();

Q_SIGNALS:
    void phaseChanged(SyncAccount::Phase phase);
    void keyringAvailableChanged(bool available);
    /// The keyring test write failed: "Sugo Sync needs the system keyring".
    void keyringUnavailable();
    /// A write of the secrets failed after the keyring had worked.
    void keyringWriteFailed();

    void codeReady(const QString &userCode, const QUrl &verificationUri, const QUrl &verificationUriComplete, const QDateTime &expiresAt);
    void deviceLimitReachedChanged(bool reached);
    /// The code expired before the site approved it.
    void signInExpired();
    /// Sign-in ended without success (invalid_grant, or the server refused to start).
    void signInFailed(const SyncError &error);
    /// The device is linked: the key phase begins.
    void signedIn();
    /// The server refused the refresh token (disconnected on the site, or token reused): this
    /// computer was signed out locally.
    void signedOutRemotely();
    void devicesChanged();
    /// The code, and with it approverName(), changed; verificationConfirmed() is false then.
    void verificationCodeChanged(const QString &code);
    void verificationConfirmedChanged(bool confirmed);
    /// The key that arrived does not come from the device the code was shown for, or does not
    /// open: refused, not confirmed. Polling stops until waitForKey().
    void keyRejected();
    /// K is stored and confirmed: phase() is Ready.
    void keyReady();
    /// This device created K: the code to show once and have the user confirm.
    void recoveryCodeCreated(const QString &code);

    /// The recovery code typed does not open the server's copy.
    void recoveryCodeWrong();
    void recoveryCodeShowableChanged(bool showable);
    /// regenerateRecoveryCode() was refused for good. `oldCodeWorks`: no try can have replaced the
    /// server's copy; else it may hold the new copy, whose code is lost.
    void recoveryCodeFailed(const SyncError &error, bool oldCodeWorks);
    /// regenerateRecoveryCode() got no certain answer: the new copy is sent again until it gets one.
    void recoveryCodeRetrying(const SyncError &error);

    /// A pending device asks for the key; `verificationCode` must match what it shows.
    void approvalRequested(const QString &id, const QString &name, const QString &platform, const QString &verificationCode);
    void deviceApproved(const QString &id);
    void approvalFailed(const QString &id, const SyncError &error);
    void deviceDisconnected(const QString &id);
    void disconnectFailed(const QString &id, const SyncError &error);

    /// Signed out here; `remoteDone` false if the server could not be told: the device keeps its
    /// slot until disconnected on sugo.stream.
    void signedOut(bool remoteDone);

    /// A request failed in a way worth showing (network, rate limit, server); it is retried.
    void requestFailed(const SyncError &error);

private:
    void setPhase(Phase phase);
    void setKeyringAvailable(bool available);
    void setDeviceLimitReached(bool reached);
    void pollToken();
    void schedulePoll(int seconds);
    void finishSignIn(const QJsonObject &json);
    void abandonSignIn();
    void clearSignInCode();
    enum class KeyStep { List, PollKey, CreateKey, UploadRecovery, ConfirmKey };

    void startKeyPhase();
    void runKeyStep(KeyStep step);
    void scheduleKeyStep(KeyStep step, int seconds);
    void retryKeyStep(KeyStep step, const SyncError &error);
    void chooseApprover();
    void createDataKeyUnlessRecovery();
    void createDataKey();
    void uploadRecovery();
    void pollKey();
    void receiveKey(const QJsonObject &json);
    void rejectKey();
    /// The approver the code is shown for; a confirmation of another one lapses.
    void setApprover(const Device &approver);
    void confirmKey();
    void becomeReady();
    int keyPollSeconds() const { return m_windowActive ? 5 : 30; }
    /// Reads a device list; false if the answer is not one.
    bool takeDevices(const SyncReply &reply);
    void askAboutPendingDevices();
    void setRecoveryShowable(bool showable);
    /// Remembers the time of the server's recovery copy if this device wrote it.
    void recordRecoveryUpload();
    void onSignedOutRemotely();
    void writeSetting(const char *key, const QVariant &value);

    SyncApi *m_api;
    SyncSecrets *m_secrets;
    Settings::Ptr m_settings;
    Phase m_phase = Phase::SignedOut;
    bool m_keyringAvailable = true;
    int m_unitMs = 1000;

    QByteArray m_publicKey;

    // Sign-in
    QTimer m_pollTimer;
    QString m_deviceName;
    QString m_deviceCode;
    QString m_userCode;
    QUrl m_verificationUri;
    QUrl m_verificationUriComplete;
    QDateTime m_codeExpiresAt;
    int m_interval = 5;
    bool m_deviceLimitReached = false;

    // Key
    QTimer m_keyTimer;
    KeyStep m_keyStep = KeyStep::List;
    bool m_windowActive = true;
    QList<Device> m_devices;
    Device m_approver;
    QString m_verificationCode;
    QByteArray m_confirmedKey; ///< The approver's public key the user confirmed the code of.
    std::optional<QJsonObject> m_heldKey; ///< A box from the approver, before the user confirmed.
    QElapsedTimer m_keyListTaken; ///< When the devices were last read: paces the re-list while waiting.

    // Approval of other devices
    bool m_checkingPending = false;
    QHash<QString, QByteArray> m_asked; ///< Device id → the public key its code was shown for.
    QSet<QString> m_ignored;

    /// A new recovery code on its way to the server, sent again until the answer is certain.
    struct PendingRecovery;
    void sendRecovery();
    /// Sends again a new code an earlier run kept in the keyring without a certain answer.
    void resumePendingRecovery();
    /// Removes the pending code from the keyring; a refused removal keeps it from being sent again.
    void dropPendingRecovery();
    std::shared_ptr<PendingRecovery> m_pendingRecovery;
    QTimer m_recoveryTimer;

    bool m_recoveryShowable = false;
    bool m_usingRecoveryCode = false;
    bool m_regenerating = false;
    bool m_signingOut = false;
};
