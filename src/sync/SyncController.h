// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>
#include <vector>

#include "Settings.h"
#include "SyncAccount.h"
#include "SyncChannel.h"

class Notifier;
class PresetModel;
class PresetPersistence;
class PresetRepository;
class SecretStore;
class SyncApi;
class SyncSecrets;

/**
 * Sugo Sync for the interface: the account (SyncAccount) and the sync of its data.
 *
 * Each kind of data is a channel (SyncChannel; the timers are PresetSyncChannel) and travels as
 * one encrypted blob of its kind (SyncCrypto), with its own ETag, "to upload" flag (Dirty),
 * conflict count and last sync, kept in the settings: the timers under Sync/, the other kinds
 * under Sync/<kind>/. One operation runs at a time, for one channel; downloads go first, in the
 * order of the channels (the timers first). For each channel:
 * - Pull: when the device becomes ready, when the window is activated (at most once a minute),
 *   every 5 minutes and on syncNow(), with If-None-Match. A new blob is decrypted, merged with
 *   the local timers, applied here if it changes them, and uploaded again if the merge changes
 *   it. 404: there is no blob yet, the first upload follows.
 * - Push: 3 s after PresetPersistence::saved() (not for what a merge applied), with If-Match,
 *   or If-None-Match: * without a blob. 412: download, merge, apply, try again, three times at
 *   most; then Error, and again in 5 minutes.
 * - A merge that would delete the selected timer while a phase runs or is paused is held back
 *   (no new ETag, Dirty kept, no uploads) and downloaded again on timerIdle().
 * - A blob this version cannot read (newer format: FormatTooNew; wrong key or damaged:
 *   UnreadableData) is never overwritten. Network failures are tried again after 1, 5 and
 *   15 minutes; the program works as usual meanwhile.
 * - A merge the database does not store (disk full, locked) keeps the old ETag and Dirty:
 *   LocalSaveFailed, and the blob is downloaded and merged again on the same retry delays, so
 *   an upload never takes the unsaved state for what the server holds.
 * - A blob over its kind's limit (SyncKinds::maxBytes()) is never sent: TooLarge, and no
 *   upload of that kind is tried before the next change here (a channel that can drop old
 *   data keeps within the limit by itself). A 413 (the server's limit, `max_bytes`) is taken
 *   the same way.
 * - A kind that needs Plus, on an account without it (403 plus_required), is PlusRequired; a
 *   kind the server does not know (an older Worker: 404 to the first upload) is Unavailable.
 *   Neither is an error, and the other kinds go on syncing. Such a kind is asked about again
 *   once a day, on syncNow(), or when a new sign-in starts over; its changes wait (Dirty) and
 *   go up once it syncs.
 *
 * The keyring is read once per run, a few seconds after attach(), and only when the settings say
 * this computer is linked: never while signed out.
 *
 * No Qt Gui here: main.cpp forwards the window's activation (windowActivated()) and gives the
 * desktop actions (setDesktopActions()); tests run with a QCoreApplication.
 */
class SyncController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(State state READ state NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged FINAL)
    /// Every text statusText() can give in the state Error, so the interface can keep room for the
    /// longest of them.
    Q_PROPERTY(QStringList errorTexts READ errorTexts CONSTANT FINAL)
    Q_PROPERTY(ErrorKind errorKind READ errorKind NOTIFY stateChanged FINAL)
    /// Linked with the keys read from the keyring (not "Waiting for the keyring…" nor
    /// KeyringUnavailable): what needs the timers' key, such as a new recovery code, can work.
    Q_PROPERTY(bool keysLoaded READ keysLoaded NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool keyringAvailable READ keyringAvailable NOTIFY keyringAvailableChanged FINAL)
    Q_PROPERTY(QDateTime lastSync READ lastSync NOTIFY lastSyncChanged FINAL)

    Q_PROPERTY(QString userCode READ userCode NOTIFY signInCodeChanged FINAL)
    Q_PROPERTY(QUrl verificationUri READ verificationUri NOTIFY signInCodeChanged FINAL)
    Q_PROPERTY(QUrl verificationUriComplete READ verificationUriComplete NOTIFY signInCodeChanged FINAL)
    Q_PROPERTY(bool deviceLimitReached READ deviceLimitReached NOTIFY deviceLimitReachedChanged FINAL)
    Q_PROPERTY(QString verificationCode READ verificationCode NOTIFY verificationCodeChanged FINAL)

    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged FINAL)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY deviceNameChanged FINAL)
    Q_PROPERTY(bool recoveryCodeShowable READ recoveryCodeShowable NOTIFY recoveryCodeShowableChanged FINAL)
    Q_PROPERTY(QVariantMap pendingApproval READ pendingApproval NOTIFY pendingApprovalChanged FINAL)

public:
    enum State {
        Off,                ///< Not linked (keyringAvailable says whether signing in can work).
        SigningIn,          ///< A code is shown; deviceLimitReached while waiting for a free slot.
        WaitingForApproval, ///< Linked, without the data key: verificationCode, or a recovery code.
        Idle,
        Syncing,            ///< A transfer runs (or the keyring is being read).
        Error,              ///< errorKind says what; it is tried again by itself.
    };
    Q_ENUM(State)

    /// Whether a kind of data can sync with this account and server.
    enum Availability {
        Available,
        PlusRequired, ///< The account lacks Plus.
        Unavailable,  ///< The server does not know the kind (older than this Sugo).
    };
    Q_ENUM(Availability)

    enum ErrorKind {
        NoError,
        Network,            ///< Out of reach; tried again after 1, 5, 15 minutes.
        SignedOutRemotely,  ///< (state Off) Disconnected on sugo.stream: sign in again.
        FormatTooNew,       ///< A newer Sugo wrote the timers: update to keep syncing.
        UnreadableData,     ///< The blob does not open with this account's key.
        TooLarge,           ///< Over its kind's limit; not sent until a change makes it fit.
        KeyringUnavailable, ///< The keyring did not answer; tried again when the window is activated.
        KeyringWriteFailed,
        RateLimited,
        Server,             ///< The server failed, or the blob kept changing during an upload.
        LocalSaveFailed,    ///< Downloaded timers could not be stored here; downloaded again later.
    };
    Q_ENUM(ErrorKind)

    /// For QML: the settings file, the system keyring and SyncApi::defaultBaseUrl().
    explicit SyncController(QObject *parent = nullptr);
    /// For tests: `store` is not owned and must outlive this object.
    SyncController(Settings::Ptr settings, SecretStore *store, const QUrl &baseUrl, QObject *parent = nullptr);
    ~SyncController() override;

    /**
     * Starts Sugo Sync for these timers (not called in smoke runs: the controller stays Off).
     * Reads the keyring a few seconds later if this computer is linked. Not owned; they must
     * outlive this object, or be detached with attach(nullptr, nullptr, nullptr).
     */
    void attach(PresetModel *presets, PresetPersistence *persistence, PresetRepository *repository);

    /// Syncs another kind of data too, after the timers (one channel per kind: a second one of
    /// the same kind replaces the first).
    void addChannel(std::unique_ptr<SyncChannel> channel);
    /// Available for a kind without a channel.
    Availability availability(SyncKind kind) const;

    /// How the desktop opens a page and copies text (main.cpp; QML may also do it itself).
    void setDesktopActions(std::function<bool(const QUrl &)> openUrl, std::function<void(const QString &)> copyText);

    /**
     * Desktop notifications: a device asking for approval while the window is hidden (as
     * `windowVisible` says) is announced, and a click on it emits showSyncPageRequested().
     * `notifier` is not owned; nullptr stops the notifications.
     */
    void attachNotifier(Notifier *notifier, std::function<bool()> windowVisible);

    /**
     * Whether a timer phase runs or is paused (main.cpp asks the TimerEngine). While it does, a
     * download that would delete the selected timer is not applied (that would stop the phase):
     * it waits, with the old ETag and Dirty, until timerIdle().
     */
    void setTimerBusy(std::function<bool()> busy);

    /// One second in ms for every wait (tests make it short).
    void setTimeUnitMs(int ms);

    State state() const { return m_state; }
    QString statusText() const;
    static QStringList errorTexts();
    ErrorKind errorKind() const { return m_errorKind; }
    bool keysLoaded() const;
    bool keyringAvailable() const;
    QDateTime lastSync() const;

    QString userCode() const;
    QUrl verificationUri() const;
    QUrl verificationUriComplete() const;
    bool deviceLimitReached() const;
    QString verificationCode() const;

    QVariantList devices() const;
    /// This device's name; while signed out, the name sign-in suggests: the one last signed in
    /// with, else the host name.
    QString deviceName() const;
    bool recoveryCodeShowable() const;
    /// The first device asking for the key: id, name, platform, verificationCode; empty if none.
    QVariantMap pendingApproval() const;

    SyncAccount *account() const { return m_account; }
    /// This computer is linked to an account (the settings say so; the keyring is not read).
    bool linked() const;
    /// Whether the server is https://api.sugo.stream (not one set with SUGO_SYNC_URL).
    bool usesDefaultServer() const;

    // --- Sign-in ------------------------------------------------------------------------------
    Q_INVOKABLE void signIn(const QString &deviceName);
    Q_INVOKABLE void cancelSignIn();
    /// Opens sugo.stream with the code filled in; false without a code or a way to open it.
    Q_INVOKABLE bool openVerificationPage();
    /// Copies the code; false without a code or a clipboard.
    Q_INVOKABLE bool copyUserCode();

    // --- Sync ---------------------------------------------------------------------------------
    /// Downloads now (and uploads what waits), also after an error.
    Q_INVOKABLE void syncNow();

    // --- Devices ------------------------------------------------------------------------------
    Q_INVOKABLE void approveDevice(const QString &id);
    Q_INVOKABLE void ignoreDevice(const QString &id);
    /// Asks about a device waiting for approval again, also after "Not Now".
    Q_INVOKABLE void reviewDevice(const QString &id);
    Q_INVOKABLE void disconnectDevice(const QString &id);
    Q_INVOKABLE void refreshDevices();

    // --- Recovery code ------------------------------------------------------------------------
    Q_INVOKABLE void useRecoveryCode(const QString &code);
    /// The recovery code, when recoveryCodeShowable (from memory, no keyring access).
    Q_INVOKABLE QString recoveryCode() const;
    Q_INVOKABLE void regenerateRecoveryCode();
    /// Whether `text` is group `index` (from 0) of the code last given by recoveryCodeToConfirm
    /// (else of recoveryCode()), in any case, with O for 0 and I or L for 1.
    Q_INVOKABLE bool confirmRecoveryGroup(int index, const QString &text) const;

    Q_INVOKABLE void signOut();

public Q_SLOTS:
    /// The window became active: key polling speeds up, the timers are pulled (at most once a
    /// minute), and a keyring that failed is read again.
    void windowActivated();
    void windowDeactivated();
    /// The timer went back to idle: a download held back for it (setTimerBusy()) is fetched again.
    void timerIdle();

Q_SIGNALS:
    void stateChanged();
    void statusTextChanged();
    void keyringAvailableChanged();
    void lastSyncChanged();
    void signInCodeChanged();
    void deviceLimitReachedChanged();
    void verificationCodeChanged();
    void devicesChanged();
    void deviceNameChanged();
    void recoveryCodeShowableChanged();
    void pendingApprovalChanged();

    /// A device asks for the key: pendingApproval holds it.
    void approvalRequested();
    /// A new recovery code (first device, or regenerated): show it once and have it confirmed.
    void recoveryCodeToConfirm(const QString &code);
    /// Signed out here; `remoteDone` false when the server could not be told (the device keeps
    /// its slot until disconnected on sugo.stream).
    void signedOut(bool remoteDone);
    /// Something worth a toast.
    void message(const QString &text);
    /// availability() of a kind changed.
    void availabilityChanged();
    /// The approval notification was clicked: show the window on the Sync settings.
    /// `activationToken` (may be empty) lets the window take the focus on Wayland.
    void showSyncPageRequested(const QString &activationToken);

private:
    /// What statusText() says in the state Error about `kind`, of the data of `dataKind` when it
    /// is a channel's error (empty for NoError).
    static QString errorText(ErrorKind kind, SyncKind dataKind = SyncKind::Presets);
    enum class Op { None, Pull, Push };

    SyncController(Settings::Ptr settings, std::unique_ptr<SecretStore> ownedStore, SecretStore *store, const QUrl &baseUrl,
                   QObject *parent);
    void connectAccount();
    void onSignedOut();
    void loadSecrets();
    void onPhaseChanged();
    void onReady();
    void resetCycle();

    /// A channel and its place in the cycle (in memory; ETag, Dirty and last sync are in the settings).
    struct Channel {
        std::unique_ptr<SyncChannel> impl;
        std::unique_ptr<QTimer> pushTimer;
        bool pullWanted = false;
        bool pullUnconditional = false; ///< The next pull downloads even an unchanged blob (after 412).
        bool pushWanted = false;
        bool remoteUnreadable = false;  ///< The blob does not open: never overwritten until it does.
        bool formatTooNew = false;
        bool held = false;              ///< A download waits for timerIdle(); no uploads meanwhile.
        bool tooLarge = false;          ///< The last upload was over the limit: TooLarge stays until one fits.
        quint64 tooLargeSaves = 0;      ///< `saves` then: no upload is tried again before a change.
        int conflicts = 0;
        quint64 saves = 0;              ///< Changes, so an upload clears Dirty only if nothing came after.
        ErrorKind error = NoError;      ///< This channel's own error (errorKind() shows the first).
        Availability availability = Available;
        bool recheck = false;           ///< Ask the server about an unavailable kind at the next pull.
    };

    Channel &insertChannel(std::unique_ptr<SyncChannel> impl, bool first);
    Channel *channel(SyncKind kind) const;
    bool channelReady(const Channel &c) const;
    void onChanged(SyncKind kind);
    void requestPull();
    /// Whether `c` takes part in transfers now (available, or due to be asked again).
    bool channelOpen(const Channel &c) const;
    void setAvailability(Channel &c, Availability availability);
    void runNext();
    void pull(Channel &c, bool unconditional);
    /// False if nothing was sent (the data cannot be written, or is over the limit).
    bool push(Channel &c);
    void takeRemote(Channel &c, const SyncReply &reply);
    void succeeded(Channel &c);
    void failed(Channel &c, const SyncError &error, Op op);
    void scheduleRetry(int seconds);

    /// The error of the account or of the transfers (network, keyring, server).
    void setError(ErrorKind kind);
    void setChannelError(Channel &c, ErrorKind kind);
    void updateErrorKind();
    void updateState();
    void setLastSyncNow(const Channel &c);
    /// The settings key of `name` for `c`'s kind.
    QString channelKey(const Channel &c, const char *name) const;
    QByteArray etag(const Channel &c) const;
    void setEtag(const Channel &c, const QByteArray &etag);
    bool dirty(const Channel &c) const;
    void setDirty(const Channel &c, bool dirty);
    void writeSetting(const char *name, const QVariant &value);
    bool readyToSync() const;
    void showNextApproval();
    void notifyApproval();
    void dropApproval(const QString &id);
    /// Drops the devices the device list no longer shows waiting (approved elsewhere, gone).
    void dropGoneApprovals();

    Settings::Ptr m_settings;
    std::unique_ptr<SecretStore> m_ownedStore;
    std::unique_ptr<SyncSecrets> m_secrets;
    SyncApi *m_api = nullptr;
    SyncAccount *m_account = nullptr;

    std::vector<std::unique_ptr<Channel>> m_channels; ///< The timers first.
    bool m_attached = false;

    std::function<bool(const QUrl &)> m_openUrl;
    std::function<void(const QString &)> m_copyText;
    QPointer<Notifier> m_notifier;
    std::function<bool()> m_windowVisible;
    std::function<bool()> m_timerBusy;
    int m_unitMs = 1000;

    State m_state = Off;
    ErrorKind m_errorKind = NoError;      ///< m_globalError, else the first channel's error.
    ErrorKind m_globalError = NoError;
    ErrorKind m_lastErrorKind = NoError; ///< As last announced by stateChanged().
    bool m_lastKeysLoaded = false;       ///< As last announced by stateChanged().
    QString m_lastStatusText;

    enum class Load { NotYet, Waiting, Done, Failed };
    Load m_load = Load::NotYet;
    QTimer m_loadTimer;

    Op m_running = Op::None;
    bool m_signingOut = false;
    bool m_keyRejected = false;
    int m_failures = 0;
    QTimer m_pullTimer;
    QTimer m_retryTimer;
    QElapsedTimer m_sinceLastPull;

    QList<QVariantMap> m_approvals;
    QString m_recoveryToConfirm;
};
