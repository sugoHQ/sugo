// SPDX-License-Identifier: GPL-3.0-or-later
#include "SyncController.h"

#include "Localization.h"
#include "Notifier.h"
#include "PresetSyncChannel.h"
#include "SecretStore.h"
#include "SyncApi.h"
#include "SyncCrypto.h"
#include "SyncSecrets.h"

#include <QLoggingCategory>
#include <QMetaEnum>
#include <QSysInfo>

#include <algorithm>
#include <iterator>

Q_LOGGING_CATEGORY(lcSyncController, "sugo.sync")

namespace
{
const QLatin1String kGroup("Sync");
/// ETag of the blob as last downloaded or uploaded.
const char kEtag[] = "Etag";
const char kLastSync[] = "LastSync";
/// The timers changed since the last upload (survives a restart before it went out).
const char kDirty[] = "Dirty";

QString key(const char *name)
{
    return kGroup + QLatin1Char('/') + QLatin1String(name);
}

// Seconds.
constexpr int kLoadDelay = 3;      ///< After attach(), before the keyring is read.
constexpr int kPushDelay = 3;      ///< After a save, before the upload.
constexpr int kPullAtMostEvery = 60; ///< Window activations closer than this do not pull.
/// Also how soon a device left on (in the tray, or with the window already in front) sees a new
/// device asking for approval: Sugo-Pending-Devices comes with every pull, mostly a 304.
constexpr int kPullEvery = 5 * 60;
constexpr int kConflictRetry = 5 * 60;
constexpr int kMaxConflicts = 3;
constexpr int kRetryDelays[] = {60, 5 * 60, 15 * 60};

/// The user's text of a recovery code group as the code writes it (Crockford Base32).
QString normalizedGroup(const QString &text)
{
    QString out;
    for (const QChar c : text) {
        if (c.isSpace() || c == QLatin1Char('-')) {
            continue;
        }
        const QChar u = c.toUpper();
        if (u == QLatin1Char('O')) {
            out += QLatin1Char('0');
        } else if (u == QLatin1Char('I') || u == QLatin1Char('L')) {
            out += QLatin1Char('1');
        } else {
            out += u;
        }
    }
    return out;
}
} // namespace

SyncController::SyncController(QObject *parent)
    : SyncController(Settings::openDefault(), createSecretStore(), nullptr, SyncApi::defaultBaseUrl(), parent)
{
}

SyncController::SyncController(Settings::Ptr settings, SecretStore *store, const QUrl &baseUrl, QObject *parent)
    : SyncController(std::move(settings), nullptr, store, baseUrl, parent)
{
}

SyncController::SyncController(Settings::Ptr settings, std::unique_ptr<SecretStore> ownedStore, SecretStore *store,
                               const QUrl &baseUrl, QObject *parent)
    : QObject(parent)
    , m_settings(std::move(settings))
    , m_ownedStore(std::move(ownedStore))
{
    m_secrets = std::make_unique<SyncSecrets>(m_ownedStore ? m_ownedStore.get() : store);
    m_api = new SyncApi(m_secrets.get(), baseUrl, this);
    m_account = new SyncAccount(m_api, m_secrets.get(), m_settings, this);

    m_loadTimer.setSingleShot(true);
    connect(&m_loadTimer, &QTimer::timeout, this, &SyncController::loadSecrets);
    connect(&m_pullTimer, &QTimer::timeout, this, &SyncController::requestPull);
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &SyncController::runNext);

    connectAccount();
    m_lastStatusText = statusText();
}

SyncController::~SyncController()
{
    // They use the secrets, which go before the QObject children would.
    delete m_account;
    delete m_api;
}

void SyncController::setTimeUnitMs(int ms)
{
    m_unitMs = std::max(1, ms);
    m_account->setTimeUnitMs(m_unitMs);
}

void SyncController::setDesktopActions(std::function<bool(const QUrl &)> openUrl, std::function<void(const QString &)> copyText)
{
    m_openUrl = std::move(openUrl);
    m_copyText = std::move(copyText);
}

void SyncController::attachNotifier(Notifier *notifier, std::function<bool()> windowVisible)
{
    m_notifier = notifier;
    m_windowVisible = std::move(windowVisible);
}

void SyncController::setTimerBusy(std::function<bool()> busy)
{
    m_timerBusy = std::move(busy);
}

void SyncController::connectAccount()
{
    SyncAccount *a = m_account;
    connect(m_api, &SyncApi::pendingDevicesSeen, a, &SyncAccount::checkPendingDevices);
    connect(m_api, &SyncApi::tokenNotSaved, this, [this]() { setError(KeyringWriteFailed); });

    connect(a, &SyncAccount::phaseChanged, this, &SyncController::onPhaseChanged);
    connect(a, &SyncAccount::keyringAvailableChanged, this, [this]() {
        Q_EMIT keyringAvailableChanged();
        updateState();
    });
    connect(a, &SyncAccount::keyringUnavailable, this, [this]() { Q_EMIT message(i18n("Sugo Sync needs the system keyring.")); });
    connect(a, &SyncAccount::keyringWriteFailed, this, [this]() {
        Q_EMIT message(i18n("Sugo could not store the Sugo Sync keys in the system keyring."));
        setError(KeyringWriteFailed);
    });

    connect(a, &SyncAccount::codeReady, this, [this]() {
        Q_EMIT signInCodeChanged();
        updateState();
    });
    connect(a, &SyncAccount::deviceLimitReachedChanged, this, [this]() {
        Q_EMIT deviceLimitReachedChanged();
        updateState();
    });
    connect(a, &SyncAccount::signInExpired, this, [this]() { Q_EMIT message(i18n("The code expired. Sign in again.")); });
    connect(a, &SyncAccount::signInFailed, this, [this](const SyncError &error) {
        Q_EMIT message(error.kind == SyncError::Kind::Network ? i18n("Cannot reach Sugo Sync. Check the connection and sign in again.")
                                                              : i18n("Sign-in did not succeed. Try again."));
    });
    connect(a, &SyncAccount::signedIn, this, &SyncController::deviceNameChanged);
    connect(a, &SyncAccount::signedOutRemotely, this, [this]() {
        setError(SignedOutRemotely);
        Q_EMIT message(i18n("This device was disconnected. Sign in again."));
    });

    connect(a, &SyncAccount::devicesChanged, this, [this]() {
        dropGoneApprovals();
        Q_EMIT devicesChanged();
    });
    connect(a, &SyncAccount::verificationCodeChanged, this, [this]() {
        Q_EMIT verificationCodeChanged();
        updateState();
    });
    connect(a, &SyncAccount::keyRejected, this, [this]() {
        m_keyRejected = true;
        Q_EMIT message(i18n("The key that arrived did not come from the device with this code, so it was refused."));
    });

    connect(a, &SyncAccount::recoveryCodeCreated, this, [this](const QString &code) {
        m_recoveryToConfirm = code;
        Q_EMIT recoveryCodeToConfirm(code);
    });
    connect(a, &SyncAccount::recoveryCodeWrong, this, [this]() { Q_EMIT message(i18n("That recovery code does not open your timers.")); });
    connect(a, &SyncAccount::recoveryCodeShowableChanged, this, &SyncController::recoveryCodeShowableChanged);
    connect(a, &SyncAccount::recoveryCodeFailed, this, [this](const SyncError &error) {
        Q_EMIT message(error.kind == SyncError::Kind::Network ? i18n("Cannot reach Sugo Sync. The old recovery code still works.")
                                                              : i18n("A new recovery code could not be created."));
    });

    connect(a, &SyncAccount::approvalRequested, this, [this](const QString &id, const QString &name, const QString &platform, const QString &code) {
        m_approvals.append({{QStringLiteral("id"), id},
                            {QStringLiteral("name"), name},
                            {QStringLiteral("platform"), platform},
                            {QStringLiteral("verificationCode"), code}});
        if (m_approvals.size() == 1) {
            showNextApproval();
        }
    });
    connect(a, &SyncAccount::deviceApproved, this, [this]() { m_account->refreshDevices(); });
    connect(a, &SyncAccount::approvalFailed, this, [this]() { Q_EMIT message(i18n("The device could not be approved. Try again.")); });
    connect(a, &SyncAccount::disconnectFailed, this, [this]() { Q_EMIT message(i18n("The device could not be disconnected. Try again.")); });
    connect(a, &SyncAccount::signedOut, this, [this](bool remoteDone) {
        onSignedOut();
        Q_EMIT signedOut(remoteDone);
    });
}

// ---------------------------------------------------------------------------------------------
// Start, keyring, account
// ---------------------------------------------------------------------------------------------

void SyncController::attach(PresetModel *presets, PresetPersistence *persistence, PresetRepository *repository)
{
    m_channels.erase(std::remove_if(m_channels.begin(), m_channels.end(),
                                    [](const std::unique_ptr<Channel> &c) { return c->impl->kind() == SyncKind::Presets; }),
                     m_channels.end());
    m_attached = presets && persistence && repository;
    if (m_attached) {
        // (The controller outlives its channels: the lambda may hold `this`.)
        insertChannel(std::make_unique<PresetSyncChannel>(presets, persistence, repository,
                                                          [this]() { return m_timerBusy && m_timerBusy(); }),
                      true);
    }
    if (m_attached && m_load == Load::NotYet && m_account->phase() == SyncAccount::Phase::SignedOut
        && (m_account->isLinked() || m_account->keyringWipePending())) {
        // The keyring may block or ask the user: not while the window first shows.
        m_load = Load::Waiting;
        m_loadTimer.start(kLoadDelay * m_unitMs);
    }
    updateErrorKind();
}

void SyncController::addChannel(std::unique_ptr<SyncChannel> channel)
{
    if (!channel) {
        return;
    }
    const SyncKind kind = channel->kind();
    m_channels.erase(std::remove_if(m_channels.begin(), m_channels.end(),
                                    [kind](const std::unique_ptr<Channel> &c) { return c->impl->kind() == kind; }),
                     m_channels.end());
    Channel &c = insertChannel(std::move(channel), false);
    if (readyToSync()) {
        if (dirty(c)) {
            c.pushWanted = true;
        }
        c.pullWanted = true;
        runNext();
    }
}

SyncController::Channel &SyncController::insertChannel(std::unique_ptr<SyncChannel> impl, bool first)
{
    auto c = std::make_unique<Channel>();
    const SyncKind kind = impl->kind();
    impl->setChangedHandler([this, kind]() { onChanged(kind); });
    c->impl = std::move(impl);
    c->pushTimer = std::make_unique<QTimer>();
    c->pushTimer->setSingleShot(true);
    connect(c->pushTimer.get(), &QTimer::timeout, this, [this, kind]() {
        if (Channel *ch = channel(kind)) {
            ch->pushWanted = true;
            runNext();
        }
    });
    Channel &ref = *c;
    m_channels.insert(first ? m_channels.begin() : m_channels.end(), std::move(c));
    return ref;
}

SyncController::Channel *SyncController::channel(SyncKind kind) const
{
    for (const auto &c : m_channels) {
        if (c->impl->kind() == kind) {
            return c.get();
        }
    }
    return nullptr;
}

void SyncController::loadSecrets()
{
    m_loadTimer.stop();
    // Signed out, but the keyring would not delete the keys then: again, now that it is asked
    // late and not while the window first shows.
    m_account->retryKeyringWipe();
    if (!m_attached || m_account->phase() != SyncAccount::Phase::SignedOut || !m_account->isLinked()) {
        m_load = Load::NotYet;
        updateState();
        return;
    }
    const SyncSecrets::LoadResult result = m_secrets->load();
    switch (result) {
    case SyncSecrets::LoadResult::Loaded:
        if (m_account->restore()) {
            m_load = Load::Done;
            break;
        }
        [[fallthrough]];
    case SyncSecrets::LoadResult::Empty:
    case SyncSecrets::LoadResult::Unreadable:
        // The settings say linked, but the keys are gone: nothing can be synced with them.
        qCWarning(lcSyncController) << "The Sugo Sync keys are missing from the keyring; signing out here";
        m_load = Load::NotYet;
        m_account->signOutLocally();
        Q_EMIT message(i18n("Sugo Sync lost this device's keys. Sign in again."));
        break;
    case SyncSecrets::LoadResult::NewerVersion:
        m_load = Load::Failed;
        setError(FormatTooNew);
        break;
    case SyncSecrets::LoadResult::Failed:
        m_load = Load::Failed;
        setError(KeyringUnavailable);
        break;
    }
    updateState();
}

void SyncController::onPhaseChanged()
{
    switch (m_account->phase()) {
    case SyncAccount::Phase::SignedOut:
        onSignedOut();
        break;
    case SyncAccount::Phase::SigningIn:
    case SyncAccount::Phase::WaitingForKey:
        m_load = Load::Done;
        setError(NoError);
        Q_EMIT signInCodeChanged();
        Q_EMIT deviceNameChanged();
        break;
    case SyncAccount::Phase::Ready:
        m_load = Load::Done;
        Q_EMIT signInCodeChanged();
        Q_EMIT deviceNameChanged();
        onReady();
        break;
    }
    updateState();
}

void SyncController::onReady()
{
    setError(NoError);
    m_keyRejected = false;
    m_account->refreshDevices();
    m_account->refreshRecoveryStatus();
    if (!m_attached) {
        return;
    }
    m_pullTimer.start(kPullEvery * m_unitMs);
    for (const auto &c : m_channels) {
        if (dirty(*c)) {
            c->pushWanted = true;
        }
    }
    requestPull();
}

void SyncController::onSignedOut()
{
    resetCycle();
    m_signingOut = false;
    m_keyRejected = false;
    m_load = Load::NotYet;
    m_loadTimer.stop();
    m_recoveryToConfirm.clear();
    if (!m_approvals.isEmpty()) {
        m_approvals.clear();
        Q_EMIT pendingApprovalChanged();
    }
    setError(NoError);
    Q_EMIT signInCodeChanged();
    Q_EMIT deviceNameChanged();
    Q_EMIT devicesChanged();
    Q_EMIT verificationCodeChanged();
    Q_EMIT lastSyncChanged();
    updateState();
}

void SyncController::resetCycle()
{
    m_running = Op::None;
    for (const auto &c : m_channels) {
        c->pullWanted = false;
        c->pullUnconditional = false;
        c->pushWanted = false;
        c->remoteUnreadable = false;
        c->formatTooNew = false;
        c->pushTooLarge = false;
        c->held = false;
        c->conflicts = 0;
        c->error = NoError;
        c->pushTimer->stop();
    }
    m_failures = 0;
    m_pullTimer.stop();
    m_retryTimer.stop();
    m_sinceLastPull.invalidate();
}

void SyncController::windowActivated()
{
    m_account->setWindowActive(true);
    if (m_attached && m_load == Load::Failed && m_errorKind == KeyringUnavailable) {
        loadSecrets();
        return;
    }
    if (m_account->phase() == SyncAccount::Phase::WaitingForKey && m_keyRejected) {
        m_keyRejected = false;
        m_account->waitForKey();
        return;
    }
    if (readyToSync() && (!m_sinceLastPull.isValid() || m_sinceLastPull.elapsed() >= qint64(kPullAtMostEvery) * m_unitMs)) {
        requestPull();
    }
}

void SyncController::windowDeactivated()
{
    m_account->setWindowActive(false);
}

void SyncController::timerIdle()
{
    bool any = false;
    for (const auto &c : m_channels) {
        if (c->held) {
            c->held = false;
            // The held blob is not under the stored ETag: download it whatever the ETag says.
            c->pullUnconditional = true;
            c->pullWanted = true;
            any = true;
        }
    }
    if (any) {
        runNext();
    }
}

// ---------------------------------------------------------------------------------------------
// The cycle
// ---------------------------------------------------------------------------------------------

bool SyncController::readyToSync() const
{
    return m_attached && !m_signingOut && m_account->phase() == SyncAccount::Phase::Ready;
}

bool SyncController::channelReady(const Channel &c) const
{
    return readyToSync() && c.impl->ready();
}

void SyncController::onChanged(SyncKind kind)
{
    Channel *c = channel(kind);
    if (!c || !m_account->isLinked()) {
        return;
    }
    ++c->saves;
    setDirty(*c, true);
    if (readyToSync()) {
        c->pushTimer->start(kPushDelay * m_unitMs);
    }
}

void SyncController::requestPull()
{
    for (const auto &c : m_channels) {
        c->pullWanted = true;
    }
    runNext();
}

void SyncController::runNext()
{
    if (m_running != Op::None || !readyToSync() || m_retryTimer.isActive()) {
        return;
    }
    for (const auto &c : m_channels) {
        if (c->pullWanted && channelReady(*c)) {
            pull(*c, std::exchange(c->pullUnconditional, false));
            updateState();
            return;
        }
    }
    for (const auto &c : m_channels) {
        // (While held, an upload would only meet the newer blob again: 412.)
        if (!c->pushWanted || c->held || !channelReady(*c)) {
            continue;
        }
        if (c->formatTooNew || c->remoteUnreadable) {
            // Never overwrite what this version cannot read.
            c->pushWanted = false;
            continue;
        }
        push(*c);
        break;
    }
    updateState();
}

void SyncController::pull(Channel &c, bool unconditional)
{
    m_running = Op::Pull;
    c.pullWanted = false;
    m_sinceLastPull.start();
    const SyncKind kind = c.impl->kind();
    m_api->getBlob(kind, unconditional ? QByteArray() : etag(c), [this, kind](const SyncReply &reply) {
        m_running = Op::None;
        Channel *c = channel(kind);
        if (!c || !channelReady(*c)) {
            updateState();
            return;
        }
        if (reply.http == 304) {
            setLastSyncNow(*c);
            if (!c->pushWanted) {
                c->conflicts = 0;
            }
            succeeded(*c);
        } else if (reply.error.kind == SyncError::Kind::NotFound) {
            // No blob yet: the first upload (If-None-Match: *).
            setEtag(*c, QByteArray());
            c->pushWanted = true;
            succeeded(*c);
        } else if (reply.ok()) {
            takeRemote(*c, reply);
        } else {
            failed(*c, reply.error, Op::Pull);
        }
        runNext();
        updateState();
    });
}

void SyncController::takeRemote(Channel &c, const SyncReply &reply)
{
    const QString kindName = SyncKinds::name(c.impl->kind());
    const SyncCrypto::OpenedBlob opened = SyncCrypto::decryptBlob(m_account->dataKey(), m_account->userId(), reply.body, kindName);
    if (!opened.ok()) {
        if (opened.error == SyncCrypto::OpenedBlob::Error::NewerFormat) {
            c.formatTooNew = true;
            setChannelError(c, FormatTooNew);
        } else {
            qCWarning(lcSyncController) << "The synced" << kindName << "do not open with this account's key";
            c.remoteUnreadable = true;
            setChannelError(c, UnreadableData);
        }
        return;
    }
    const SyncChannel::Merge merged = c.impl->merge(opened.plaintext, m_account->deviceId());
    if (merged.result == SyncChannel::Merge::Result::Unreadable) {
        qCWarning(lcSyncController) << "The synced" << kindName << "cannot be read";
        c.remoteUnreadable = true;
        setChannelError(c, UnreadableData);
        return;
    }
    c.remoteUnreadable = false;
    if (merged.newerFormat) {
        c.formatTooNew = true; // read, never written
    }
    if (merged.result == SyncChannel::Merge::Result::HeldBack) {
        // Keep the old ETag and Dirty, and download again when the timer is idle.
        c.held = true;
        succeeded(c);
        return;
    }
    c.held = false;
    if (merged.result == SyncChannel::Merge::Result::LocalSaveFailed) {
        // Keep the old ETag and Dirty: the next download brings this blob again (not 304) and
        // merges it with what the database holds, and no upload takes the unsaved state as
        // what the server has.
        qCWarning(lcSyncController) << "Cannot store the synced" << kindName << "; downloading them again later";
        c.pullWanted = true;
        setChannelError(c, LocalSaveFailed);
        constexpr int steps = int(std::size(kRetryDelays));
        scheduleRetry(kRetryDelays[std::min(m_failures, steps - 1)]);
        ++m_failures;
        return;
    }
    if (merged.changedRemote && c.formatTooNew) {
        // The server lacks what this computer has, and only a newer Sugo may write it there.
        // Keep the old ETag and Dirty: after the update the first download is not a 304, and
        // its merge uploads this computer's changes.
        qCInfo(lcSyncController) << "Local" << kindName << "changes wait for an update of Sugo to be uploaded";
        c.pushWanted = false;
        c.pushTimer->stop();
        setDirty(c, true);
        setLastSyncNow(c);
        succeeded(c);
        return;
    }
    setEtag(c, reply.etag);
    setLastSyncNow(c);

    if (merged.changedRemote) {
        // A fresh upload with the merged data is due; the too-large refusal of the previous
        // blob no longer says anything about it.
        c.pushTooLarge = false;
        c.pushWanted = true;
    } else {
        // The server holds what this computer has: nothing waits to go out.
        c.pushWanted = false;
        c.pushTimer->stop();
        c.conflicts = 0;
        setDirty(c, false);
    }
    succeeded(c);
}

void SyncController::push(Channel &c)
{
    m_running = Op::Push;
    c.pushWanted = false;
    c.pushTimer->stop();
    const quint64 saves = c.saves;
    const SyncKind kind = c.impl->kind();

    const QByteArray json = c.impl->localData(m_account->deviceId(), SyncKinds::maxBytes(kind) - SyncKinds::BlobOverhead);
    const QByteArray blob =
        json.isEmpty() ? QByteArray() : SyncCrypto::encryptBlob(m_account->dataKey(), m_account->userId(), json, SyncKinds::name(kind));
    if (blob.isEmpty()) {
        m_running = Op::None;
        qCWarning(lcSyncController) << "Cannot encrypt the" << SyncKinds::name(kind);
        setError(Server);
        return;
    }
    m_api->putBlob(kind, blob, etag(c), [this, kind, saves](const SyncReply &reply) {
        m_running = Op::None;
        Channel *c = channel(kind);
        if (!c || !channelReady(*c)) {
            updateState();
            return;
        }
        if (reply.ok()) {
            c->pushTooLarge = false; // an upload finally went through
            setEtag(*c, reply.etag);
            setLastSyncNow(*c);
            if (saves == c->saves) {
                setDirty(*c, false);
            }
            c->conflicts = 0;
            succeeded(*c);
        } else if (reply.error.kind == SyncError::Kind::PreconditionFailed) {
            // Another device uploaded first: take its data, merge, and try again.
            c->pushWanted = true;
            c->pullWanted = true;
            c->pullUnconditional = true;
            if (++c->conflicts >= kMaxConflicts) {
                qCWarning(lcSyncController) << "The synced" << SyncKinds::name(kind) << "kept changing during the upload; trying again later";
                c->conflicts = 0;
                setChannelError(*c, Server);
                scheduleRetry(kConflictRetry);
            }
        } else {
            failed(*c, reply.error, Op::Push);
        }
        runNext();
        updateState();
    });
}

void SyncController::succeeded(Channel &c)
{
    m_failures = 0;
    // A refused upload stays the worst news: a later pull, even a 304, must not clear it while
    // what the server has is not what this computer shows.
    setChannelError(c, c.pushTooLarge ? TooLarge : c.formatTooNew ? FormatTooNew : NoError);
    setError(m_secrets->hasUnsavedRefreshToken() ? KeyringWriteFailed : NoError);
}

void SyncController::failed(Channel &c, const SyncError &error, Op op)
{
    if (op == Op::Pull) {
        c.pullWanted = true;
    } else {
        c.pushWanted = true;
    }
    switch (error.kind) {
    case SyncError::Kind::InvalidGrant:
        // SyncAccount signs out (signedOutRemotely).
        return;
    case SyncError::Kind::TooLarge:
        c.pushWanted = false;  // the next change tries again
        c.pushTooLarge = true; // pulls and 304 answers must not clear the error meanwhile
        setChannelError(c, TooLarge);
        return;
    case SyncError::Kind::RateLimited:
        setError(RateLimited);
        scheduleRetry(std::max(error.retryAfter, kRetryDelays[0]));
        return;
    case SyncError::Kind::Network:
        setError(Network);
        break;
    default:
        qCWarning(lcSyncController) << "Sync failed:" << SyncKinds::name(c.impl->kind()) << error.http << error.code << error.message;
        setError(Server);
        break;
    }
    constexpr int steps = int(std::size(kRetryDelays));
    scheduleRetry(kRetryDelays[std::min(m_failures, steps - 1)]);
    ++m_failures;
}

void SyncController::scheduleRetry(int seconds)
{
    m_retryTimer.start(qint64(seconds) * m_unitMs);
}

void SyncController::syncNow()
{
    if (m_attached && m_load == Load::Failed && m_errorKind == KeyringUnavailable) {
        loadSecrets();
        return;
    }
    if (!readyToSync()) {
        return;
    }
    m_retryTimer.stop();
    for (const auto &c : m_channels) {
        c->remoteUnreadable = false;
        if (c->pushTimer->isActive()) {
            c->pushTimer->stop();
            c->pushWanted = true;
        }
    }
    requestPull();
}

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------

void SyncController::setError(ErrorKind kind)
{
    m_globalError = kind;
    updateErrorKind();
}

void SyncController::setChannelError(Channel &c, ErrorKind kind)
{
    c.error = kind;
    updateErrorKind();
}

void SyncController::updateErrorKind()
{
    ErrorKind kind = m_globalError;
    for (auto it = m_channels.cbegin(); kind == NoError && it != m_channels.cend(); ++it) {
        kind = (*it)->error;
    }
    m_errorKind = kind;
    updateState();
}

void SyncController::updateState()
{
    State state = Off;
    switch (m_account->phase()) {
    case SyncAccount::Phase::SigningIn:
        state = SigningIn;
        break;
    case SyncAccount::Phase::WaitingForKey:
        state = WaitingForApproval;
        break;
    case SyncAccount::Phase::Ready:
        if (m_running != Op::None) {
            state = Syncing;
        } else if (m_errorKind != NoError) {
            state = Error;
        } else {
            state = Idle;
        }
        break;
    case SyncAccount::Phase::SignedOut:
        if (!m_attached || !m_account->isLinked()) {
            state = Off;
        } else if (m_load == Load::Waiting) {
            state = Syncing; // "Waiting for the keyring…"
        } else if (m_errorKind != NoError) {
            state = Error;
        }
        break;
    }
    if (state != m_state || m_errorKind != m_lastErrorKind || keysLoaded() != m_lastKeysLoaded) {
        m_state = state;
        m_lastErrorKind = m_errorKind; // errorKind and keysLoaded share the notification
        m_lastKeysLoaded = keysLoaded();
        Q_EMIT stateChanged();
    }
    const QString text = statusText();
    if (text != m_lastStatusText) {
        m_lastStatusText = text;
        Q_EMIT statusTextChanged();
    }
}

bool SyncController::keysLoaded() const
{
    return m_account->phase() == SyncAccount::Phase::Ready;
}

QString SyncController::statusText() const
{
    switch (m_state) {
    case Off:
        if (m_errorKind == SignedOutRemotely) {
            return i18n("This device was disconnected. Sign in again.");
        }
        if (!keyringAvailable()) {
            return i18n("Sugo Sync needs the system keyring.");
        }
        return i18n("Not signed in.");
    case SigningIn:
        if (deviceLimitReached()) {
            return i18n("Your account has as many devices as it can. Disconnect one on sugo.stream.");
        }
        if (userCode().isEmpty()) {
            return i18n("Asking for a code…");
        }
        return i18n("Enter this code on sugo.stream.");
    case WaitingForApproval:
        if (verificationCode().isEmpty()) {
            return i18n("Waiting for another device to approve this one, or for the recovery code.");
        }
        return i18n("Check that your other device shows the same code, then approve it there.");
    case Idle:
        return i18n("Up to date.");
    case Syncing:
        if (m_account->phase() == SyncAccount::Phase::SignedOut) {
            return i18n("Waiting for the keyring…");
        }
        return i18n("Syncing…");
    case Error:
        break;
    }
    return errorText(m_errorKind);
}

QString SyncController::errorText(ErrorKind kind)
{
    switch (kind) {
    case Network:
        return i18n("Cannot reach Sugo Sync. Trying again later.");
    case FormatTooNew:
        return i18n("Update Sugo to keep syncing.");
    case UnreadableData:
        return i18n("The synced timers do not open with this device's key.");
    case TooLarge:
        return i18n("The timers are too large to sync.");
    case KeyringUnavailable:
        return i18n("The system keyring did not answer. Sugo tries again when you return to the window.");
    case KeyringWriteFailed:
        return i18n("Sugo could not store the Sugo Sync keys in the system keyring.");
    case RateLimited:
        return i18n("Sugo Sync is busy. Trying again later.");
    case Server:
        return i18n("Sugo Sync could not save the timers. Trying again later.");
    case LocalSaveFailed:
        return i18n("Sugo could not save the synced timers on this device. Trying again later.");
    case SignedOutRemotely:
        return i18n("This device was disconnected. Sign in again.");
    case NoError:
        break;
    }
    return QString();
}

QStringList SyncController::errorTexts()
{
    QStringList texts;
    const QMetaEnum kinds = QMetaEnum::fromType<ErrorKind>();
    for (int i = 0; i < kinds.keyCount(); ++i) {
        const QString text = errorText(static_cast<ErrorKind>(kinds.value(i)));
        if (!text.isEmpty() && !texts.contains(text)) {
            texts.append(text);
        }
    }
    return texts;
}

bool SyncController::keyringAvailable() const
{
    return m_account->keyringAvailable();
}

QDateTime SyncController::lastSync() const
{
    return m_settings->value(key(kLastSync)).toDateTime();
}

void SyncController::setLastSyncNow(const Channel &c)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (c.impl->kind() != SyncKind::Presets) {
        m_settings->setValue(channelKey(c, kLastSync), now);
    }
    // Sync/LastSync: the last sync of any kind.
    writeSetting(kLastSync, now);
    Settings::sync(*m_settings);
    Q_EMIT lastSyncChanged();
}

QString SyncController::channelKey(const Channel &c, const char *name) const
{
    // The timers keep the keys they had before there were other kinds.
    const SyncKind kind = c.impl->kind();
    return kind == SyncKind::Presets ? key(name) : key(name).insert(kGroup.size() + 1, SyncKinds::name(kind) + QLatin1Char('/'));
}

QByteArray SyncController::etag(const Channel &c) const
{
    return m_settings->value(channelKey(c, kEtag)).toByteArray();
}

void SyncController::setEtag(const Channel &c, const QByteArray &etag)
{
    m_settings->setValue(channelKey(c, kEtag), etag);
}

bool SyncController::dirty(const Channel &c) const
{
    return m_settings->value(channelKey(c, kDirty), false).toBool();
}

void SyncController::setDirty(const Channel &c, bool dirty)
{
    if (dirty == this->dirty(c)) {
        return;
    }
    if (dirty) {
        m_settings->setValue(channelKey(c, kDirty), true);
    } else {
        m_settings->remove(channelKey(c, kDirty));
    }
    Settings::sync(*m_settings);
}

void SyncController::writeSetting(const char *name, const QVariant &value)
{
    m_settings->setValue(key(name), value);
}

// ---------------------------------------------------------------------------------------------
// Account, for QML
// ---------------------------------------------------------------------------------------------

bool SyncController::linked() const
{
    return m_account->isLinked();
}

bool SyncController::usesDefaultServer() const
{
    return SyncApi::isDefaultBaseUrl(m_api->baseUrl());
}

QString SyncController::userCode() const
{
    return m_account->userCode();
}

QUrl SyncController::verificationUri() const
{
    return m_account->verificationUri();
}

QUrl SyncController::verificationUriComplete() const
{
    return m_account->verificationUriComplete();
}

bool SyncController::deviceLimitReached() const
{
    return m_account->deviceLimitReached();
}

QString SyncController::verificationCode() const
{
    return m_account->verificationCode();
}

QVariantList SyncController::devices() const
{
    return m_account->devicesAsVariantList();
}

QString SyncController::deviceName() const
{
    // Signed out: the name last signed in with, so signing in again keeps it.
    QString name = m_account->deviceName();
    if (name.isEmpty()) {
        name = m_account->lastDeviceName();
    }
    return name.isEmpty() ? QSysInfo::machineHostName() : name;
}

bool SyncController::recoveryCodeShowable() const
{
    return m_account->recoveryCodeShowable();
}

QVariantMap SyncController::pendingApproval() const
{
    return m_approvals.value(0);
}

void SyncController::signIn(const QString &deviceName)
{
    setError(NoError);
    m_account->startSignIn(deviceName);
    updateState();
}

void SyncController::cancelSignIn()
{
    m_account->cancelSignIn();
}

bool SyncController::openVerificationPage()
{
    QUrl url = verificationUriComplete();
    if (!url.isValid() || url.isEmpty()) {
        url = verificationUri();
    }
    if (!m_openUrl || url.isEmpty() || (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http"))) {
        return false;
    }
    return m_openUrl(url);
}

bool SyncController::copyUserCode()
{
    const QString code = userCode();
    if (!m_copyText || code.isEmpty()) {
        return false;
    }
    m_copyText(code);
    return true;
}

void SyncController::approveDevice(const QString &id)
{
    m_account->approveDevice(id);
    dropApproval(id);
}

void SyncController::ignoreDevice(const QString &id)
{
    m_account->ignoreDevice(id);
    dropApproval(id);
}

void SyncController::reviewDevice(const QString &id)
{
    m_account->askAgain(id);
}

void SyncController::showNextApproval()
{
    Q_EMIT pendingApprovalChanged();
    if (!m_approvals.isEmpty()) {
        Q_EMIT approvalRequested();
        notifyApproval();
    }
}

void SyncController::notifyApproval()
{
    // The window asks by itself when it is on screen.
    if (!m_notifier || (m_windowVisible && m_windowVisible())) {
        return;
    }
    const QString name = m_approvals.constFirst().value(QStringLiteral("name")).toString();
    Notifier::Notification notification;
    notification.title = i18n("New device wants to sync");
    notification.text = i18n("%1 asks for your timers. Open Sugo to compare the verification code and approve it.", name);
    notification.iconName = QStringLiteral("stream.sugo.app");
    const Notifier::Handler review = [this](const QString &token) { Q_EMIT showSyncPageRequested(token); };
    notification.actions.append({i18n("Review"), review});
    notification.defaultActionLabel = i18n("Review");
    notification.defaultAction = review;
    m_notifier->notify(this, notification);
}

void SyncController::dropApproval(const QString &id)
{
    const bool wasFirst = !m_approvals.isEmpty() && m_approvals.first().value(QStringLiteral("id")).toString() == id;
    m_approvals.removeIf([&id](const QVariantMap &m) { return m.value(QStringLiteral("id")).toString() == id; });
    if (wasFirst) {
        showNextApproval();
    }
}

void SyncController::dropGoneApprovals()
{
    const QList<SyncAccount::Device> devices = m_account->devices();
    const auto gone = [&devices](const QVariantMap &approval) {
        const QString id = approval.value(QStringLiteral("id")).toString();
        return std::none_of(devices.cbegin(), devices.cend(),
                            [&id](const SyncAccount::Device &d) { return d.id == id && d.pending && !d.current; });
    };
    const bool firstGone = !m_approvals.isEmpty() && gone(m_approvals.constFirst());
    m_approvals.removeIf(gone);
    if (firstGone) {
        showNextApproval();
    }
}

void SyncController::disconnectDevice(const QString &id)
{
    m_account->disconnectDevice(id);
}

void SyncController::refreshDevices()
{
    if (m_account->phase() == SyncAccount::Phase::Ready || m_account->phase() == SyncAccount::Phase::WaitingForKey) {
        m_account->refreshDevices();
    }
}

void SyncController::useRecoveryCode(const QString &code)
{
    m_account->useRecoveryCode(code);
}

QString SyncController::recoveryCode() const
{
    return m_account->recoveryCode();
}

void SyncController::regenerateRecoveryCode()
{
    m_account->regenerateRecoveryCode();
}

bool SyncController::confirmRecoveryGroup(int index, const QString &text) const
{
    const QString code = m_recoveryToConfirm.isEmpty() ? recoveryCode() : m_recoveryToConfirm;
    const QStringList groups = code.split(QLatin1Char('-'), Qt::SkipEmptyParts);
    if (index < 0 || index >= groups.size()) {
        return false;
    }
    return normalizedGroup(text) == groups.at(index).toUpper();
}

void SyncController::signOut()
{
    // Pending transfers go with the account's requests (SyncApi::reset()).
    m_signingOut = true;
    resetCycle();
    updateState();
    m_account->signOut();
}
