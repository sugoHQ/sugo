// SPDX-License-Identifier: GPL-3.0-or-later
#include "SyncAccount.h"

#include "SyncCrypto.h"
#include "SyncSecrets.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSysInfo>

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

namespace
{
const QLatin1String kGroup("Sync");
const char kDeviceId[] = "DeviceId";
const char kUserId[] = "UserId";
const char kDeviceName[] = "DeviceName";
/// The server knows this device holds K (recovery copy uploaded, or DELETE /v1/key done).
const char kKeyConfirmed[] = "KeyConfirmed";
/// updated_at of the recovery copy this device wrote last ("pending" while it writes one).
const char kRecoveryUpdatedAt[] = "RecoveryUpdatedAt";
const QLatin1String kPendingStamp("pending");
/// The name this computer last signed in with, kept after sign-out (outside the group `Sync`,
/// which goes) so that signing in again offers it.
const QLatin1String kLastDeviceName("SyncSignIn/DeviceName");
/// Set when the keyring would not delete the secrets item at sign-out (outside the group `Sync`
/// too): the keys stay in the keyring until a later start deletes them.
const QLatin1String kKeyringWipePending("SyncSignIn/KeyringWipePending");

QString key(const char *name)
{
    return kGroup + QLatin1Char('/') + QLatin1String(name);
}

constexpr int kMaxNameLength = 64;
/// Seconds before a failed step of the key phase is tried again.
constexpr int kRetrySeconds = 30;
/// While waiting for the key, the devices are listed again this often: an approver that signed
/// out (or was disconnected) is replaced by whichever device holds the key now.
constexpr int kKeyRelistSeconds = 60;
/// The longest wait between two requests, whatever the server asks (interval, Retry-After): more
/// would overflow the timers' milliseconds, and the polling would stop without a word.
constexpr int kMaxWaitSeconds = 3600;

QDateTime parseTime(const QJsonValue &value)
{
    return value.isString() ? QDateTime::fromString(value.toString(), Qt::ISODateWithMs) : QDateTime();
}
} // namespace

SyncAccount::SyncAccount(SyncApi *api, SyncSecrets *secrets, Settings::Ptr settings, QObject *parent)
    : QObject(parent)
    , m_api(api)
    , m_secrets(secrets)
    , m_settings(std::move(settings))
{
    m_pollTimer.setSingleShot(true);
    // The server answers slow_down (and widens the interval by 5 s) to a poll that comes even a
    // little before the interval; a coarse timer may fire up to 5 % early.
    m_pollTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_pollTimer, &QTimer::timeout, this, &SyncAccount::pollToken);
    m_keyTimer.setSingleShot(true);
    connect(&m_keyTimer, &QTimer::timeout, this, [this]() { runKeyStep(m_keyStep); });
    connect(m_api, &SyncApi::signedOutRemotely, this, &SyncAccount::onSignedOutRemotely);
}

SyncAccount::~SyncAccount() = default;

QString SyncAccount::platformName()
{
    return QSysInfo::kernelType() == QLatin1String("darwin") ? QStringLiteral("macos") : QStringLiteral("linux");
}

bool SyncAccount::isLinked() const
{
    return !deviceId().isEmpty();
}

QString SyncAccount::deviceId() const
{
    return m_settings->value(key(kDeviceId)).toString();
}

QString SyncAccount::userId() const
{
    return m_settings->value(key(kUserId)).toString();
}

QString SyncAccount::deviceName() const
{
    return m_settings->value(key(kDeviceName)).toString();
}

QString SyncAccount::lastDeviceName() const
{
    return m_settings->value(kLastDeviceName).toString();
}

QVariantMap SyncAccount::Device::toVariantMap() const
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("platform"), platform},
        {QStringLiteral("createdAt"), createdAt},
        {QStringLiteral("lastSeen"), lastSeen},
        {QStringLiteral("pending"), pending},
        {QStringLiteral("current"), current},
    };
}

QVariantList SyncAccount::devicesAsVariantList() const
{
    QVariantList list;
    for (const Device &d : m_devices) {
        list.append(d.toVariantMap());
    }
    return list;
}

QString SyncAccount::recoveryCode() const
{
    return m_recoveryShowable ? SyncCrypto::formatRecoveryCode(m_secrets->values().recoveryCode) : QString();
}

const SecretBytes &SyncAccount::dataKey() const
{
    return m_secrets->values().dataKey;
}

void SyncAccount::writeSetting(const char *name, const QVariant &value)
{
    m_settings->setValue(key(name), value);
}

void SyncAccount::setPhase(Phase phase)
{
    if (m_phase == phase) {
        return;
    }
    m_phase = phase;
    Q_EMIT phaseChanged(phase);
}

void SyncAccount::setKeyringAvailable(bool available)
{
    if (m_keyringAvailable == available) {
        return;
    }
    m_keyringAvailable = available;
    Q_EMIT keyringAvailableChanged(available);
}

void SyncAccount::setDeviceLimitReached(bool reached)
{
    if (m_deviceLimitReached == reached) {
        return;
    }
    m_deviceLimitReached = reached;
    Q_EMIT deviceLimitReachedChanged(reached);
}

// ---------------------------------------------------------------------------------------------
// Restore
// ---------------------------------------------------------------------------------------------

bool SyncAccount::restore()
{
    if (m_phase != Phase::SignedOut || !isLinked()) {
        return false;
    }
    const SyncSecrets::Values &values = m_secrets->values();
    if (m_secrets->refreshToken().isEmpty() || values.deviceKey.isEmpty()) {
        return false;
    }
    m_publicKey = SyncCrypto::publicKeyFor(values.deviceKey);
    setPhase(Phase::WaitingForKey);
    startKeyPhase();
    return true;
}

// ---------------------------------------------------------------------------------------------
// Sign-in
// ---------------------------------------------------------------------------------------------

void SyncAccount::startSignIn(const QString &deviceName)
{
    if (m_phase != Phase::SignedOut) {
        return;
    }
    // Rule S4: no keyring, no sync. Checked before anything goes out.
    if (!m_secrets->probe()) {
        setKeyringAvailable(false);
        Q_EMIT keyringUnavailable();
        return;
    }
    setKeyringAvailable(true);

    SyncCrypto::KeyPair keys = SyncCrypto::generateKeyPair();
    if (keys.publicKey.isEmpty()) {
        SyncError e;
        e.kind = SyncError::Kind::Format;
        Q_EMIT signInFailed(e);
        return;
    }
    SyncSecrets::Values values;
    values.deviceKey = std::move(keys.secretKey);
    if (!m_secrets->replace(std::move(values))) {
        Q_EMIT keyringWriteFailed();
        return;
    }
    m_publicKey = keys.publicKey;
    m_deviceName = deviceName.simplified().left(kMaxNameLength);
    if (m_deviceName.isEmpty()) {
        m_deviceName = QSysInfo::machineHostName().left(kMaxNameLength);
    }
    if (m_deviceName.isEmpty()) {
        m_deviceName = QStringLiteral("Sugo");
    }
    setPhase(Phase::SigningIn);

    m_api->startDevice(m_deviceName, platformName(), m_publicKey, [this](const SyncReply &reply) {
        if (m_phase != Phase::SigningIn) {
            return;
        }
        const QString deviceCode = reply.json.value(QLatin1String("device_code")).toString();
        if (!reply.ok() || deviceCode.isEmpty()) {
            SyncError error = reply.error;
            if (error.ok()) {
                error.kind = SyncError::Kind::Format;
            }
            abandonSignIn();
            Q_EMIT signInFailed(error);
            return;
        }
        m_deviceCode = deviceCode;
        m_userCode = reply.json.value(QLatin1String("user_code")).toString();
        m_verificationUri = QUrl(reply.json.value(QLatin1String("verification_uri")).toString());
        m_verificationUriComplete = QUrl(reply.json.value(QLatin1String("verification_uri_complete")).toString());
        m_interval = std::max(1, reply.json.value(QLatin1String("interval")).toInt(5));
        m_codeExpiresAt = QDateTime::currentDateTimeUtc().addSecs(reply.json.value(QLatin1String("expires_in")).toInt(600));
        Q_EMIT codeReady(m_userCode, m_verificationUri, m_verificationUriComplete, m_codeExpiresAt);
        schedulePoll(m_interval);
    });
}

void SyncAccount::schedulePoll(int seconds)
{
    m_pollTimer.start(std::clamp(seconds, 1, kMaxWaitSeconds) * m_unitMs);
}

void SyncAccount::pollToken()
{
    if (m_phase != Phase::SigningIn || m_deviceCode.isEmpty()) {
        return;
    }
    m_api->pollToken(m_deviceCode, [this](const SyncReply &reply) {
        if (m_phase != Phase::SigningIn) {
            return;
        }
        if (reply.ok()) {
            finishSignIn(reply.json);
            return;
        }
        const SyncError &error = reply.error;
        if (error.kind == SyncError::Kind::AuthorizationPending || error.kind == SyncError::Kind::SlowDown) {
            // The code's life left: an approval extends it (to two minutes at least), so the first
            // expires_in may no longer hold.
            const QJsonValue left = error.extra.value(QLatin1String("expires_in"));
            if (left.isDouble()) {
                m_codeExpiresAt = QDateTime::currentDateTimeUtc().addSecs(std::max<qint64>(0, left.toInteger()));
            }
        }
        switch (error.kind) {
        case SyncError::Kind::AuthorizationPending:
            setDeviceLimitReached(error.reason == QLatin1String("device_limit_reached"));
            break;
        case SyncError::Kind::SlowDown:
            m_interval = error.interval > m_interval ? error.interval : m_interval + 5;
            break;
        case SyncError::Kind::ExpiredToken:
            abandonSignIn();
            Q_EMIT signInExpired();
            return;
        case SyncError::Kind::Network:
        case SyncError::Kind::RateLimited:
        case SyncError::Kind::Server:
            Q_EMIT requestFailed(error);
            break;
        default:
            abandonSignIn();
            Q_EMIT signInFailed(error);
            return;
        }
        if (m_codeExpiresAt.isValid() && QDateTime::currentDateTimeUtc() >= m_codeExpiresAt) {
            abandonSignIn();
            Q_EMIT signInExpired();
            return;
        }
        schedulePoll(std::max(m_interval, error.retryAfter));
    });
}

void SyncAccount::finishSignIn(const QJsonObject &json)
{
    // SyncApi::pollToken() has already written the refresh token (first of all).
    if (m_secrets->hasUnsavedRefreshToken()) {
        Q_EMIT keyringWriteFailed();
    }
    writeSetting(kDeviceId, json.value(QLatin1String("device_id")).toString());
    writeSetting(kUserId, json.value(QLatin1String("user_id")).toString());
    writeSetting(kDeviceName, m_deviceName);
    m_settings->setValue(kLastDeviceName, m_deviceName);
    Settings::sync(*m_settings);
    clearSignInCode();
    setDeviceLimitReached(false);
    setPhase(Phase::WaitingForKey);
    Q_EMIT signedIn();
    if (m_phase == Phase::WaitingForKey) {
        startKeyPhase();
    }
}

void SyncAccount::clearSignInCode()
{
    m_pollTimer.stop();
    m_deviceCode.clear();
    m_userCode.clear();
    m_verificationUri.clear();
    m_verificationUriComplete.clear();
    m_codeExpiresAt = QDateTime();
}

void SyncAccount::abandonSignIn()
{
    clearSignInCode();
    m_api->reset();
    m_secrets->clear();
    m_publicKey.clear();
    setDeviceLimitReached(false);
    setPhase(Phase::SignedOut);
}

void SyncAccount::cancelSignIn()
{
    if (m_phase == Phase::SigningIn) {
        abandonSignIn();
    }
}

// ---------------------------------------------------------------------------------------------
// Key
// ---------------------------------------------------------------------------------------------

void SyncAccount::startKeyPhase()
{
    const SyncSecrets::Values &values = m_secrets->values();
    if (values.dataKey.isEmpty()) {
        runKeyStep(KeyStep::List);
    } else if (m_settings->value(key(kKeyConfirmed)).toBool()) {
        becomeReady();
    } else if (!values.recoveryCode.isEmpty()) {
        runKeyStep(KeyStep::UploadRecovery); // created here, upload interrupted
    } else {
        runKeyStep(KeyStep::ConfirmKey); // received, confirmation interrupted
    }
}

void SyncAccount::waitForKey()
{
    if (m_phase == Phase::WaitingForKey) {
        m_keyTimer.stop();
        setApprover(Device()); // a wait started over chooses its approver afresh, and confirms it again
        runKeyStep(KeyStep::List);
    }
}

void SyncAccount::setWindowActive(bool active)
{
    m_windowActive = active;
    if (active && m_phase == Phase::WaitingForKey && m_keyStep == KeyStep::PollKey && m_keyTimer.isActive()
        && m_keyTimer.remainingTime() > keyPollSeconds() * m_unitMs) {
        m_keyTimer.start(keyPollSeconds() * m_unitMs);
    }
}

void SyncAccount::scheduleKeyStep(KeyStep step, int seconds)
{
    m_keyStep = step;
    m_keyTimer.start(std::clamp(seconds, 1, kMaxWaitSeconds) * m_unitMs);
}

void SyncAccount::retryKeyStep(KeyStep step, const SyncError &error)
{
    if (error.transient()) {
        Q_EMIT requestFailed(error);
    }
    scheduleKeyStep(step, std::max(kRetrySeconds, error.retryAfter));
}

void SyncAccount::runKeyStep(KeyStep step)
{
    if (m_phase != Phase::WaitingForKey) {
        return;
    }
    m_keyStep = step;
    switch (step) {
    case KeyStep::List:
        m_keyListTaken.start();
        m_api->listDevices([this](const SyncReply &reply) {
            if (m_phase != Phase::WaitingForKey) {
                return;
            }
            if (!takeDevices(reply)) {
                retryKeyStep(KeyStep::List, reply.error);
                return;
            }
            chooseApprover();
        });
        break;
    case KeyStep::PollKey:
        pollKey();
        break;
    case KeyStep::CreateKey:
        createDataKey();
        break;
    case KeyStep::UploadRecovery:
        uploadRecovery();
        break;
    case KeyStep::ConfirmKey:
        confirmKey();
        break;
    }
}

bool SyncAccount::takeDevices(const SyncReply &reply)
{
    if (!reply.ok() || !reply.json.value(QLatin1String("devices")).isArray()) {
        return false;
    }
    QList<Device> devices;
    const QJsonArray list = reply.json.value(QLatin1String("devices")).toArray();
    for (const QJsonValue &value : list) {
        const QJsonObject o = value.toObject();
        Device d;
        d.id = o.value(QLatin1String("id")).toString();
        if (d.id.isEmpty()) {
            continue;
        }
        d.name = o.value(QLatin1String("name")).toString();
        d.platform = o.value(QLatin1String("platform")).toString();
        d.createdAt = parseTime(o.value(QLatin1String("created_at")));
        d.lastSeen = parseTime(o.value(QLatin1String("last_seen")));
        d.pending = o.value(QLatin1String("pending")).toBool(true);
        d.current = o.value(QLatin1String("current")).toBool(false);
        d.publicKey = QByteArray::fromBase64(o.value(QLatin1String("public_key")).toString().toLatin1());
        devices.append(d);
    }
    m_devices = devices;
    Q_EMIT devicesChanged();
    return true;
}

void SyncAccount::chooseApprover()
{
    // The device that can send K: another one that holds it. The one already chosen stays while it
    // is still listed as such with the key the shown code covers: the user may be comparing that
    // code, and the key that device sends must still be taken. Else the most recently seen.
    const Device *approver = nullptr;
    for (const Device &d : std::as_const(m_devices)) {
        if (d.current || d.pending || d.publicKey.size() != SyncCrypto::KeyBytes) {
            continue;
        }
        if (!m_approver.id.isEmpty() && d.id == m_approver.id && d.publicKey == m_approver.publicKey) {
            approver = &d;
            break;
        }
        if (!approver || d.lastSeen > approver->lastSeen) {
            approver = &d;
        }
    }
    bool anyReady = false;
    for (const Device &d : std::as_const(m_devices)) {
        anyReady = anyReady || (!d.current && !d.pending);
    }
    setApprover(approver ? *approver : Device());
    const QString code = approver ? SyncCrypto::verificationCode(m_publicKey, approver->publicKey) : QString();
    if (code != m_verificationCode) {
        m_verificationCode = code;
        Q_EMIT verificationCodeChanged(code);
    }
    if (!anyReady) {
        createDataKeyUnlessRecovery();
        return;
    }
    pollKey();
}

void SyncAccount::createDataKeyUnlessRecovery()
{
    // No other device holds K. The first device of the account creates it; but when the account has a
    // recovery copy (its devices signed out or were disconnected), K exists and only the recovery code
    // opens it: the server would refuse a new one, and a new one would not open the synced timers.
    m_api->getRecovery([this](const SyncReply &reply) {
        if (m_phase != Phase::WaitingForKey) {
            return;
        }
        if (reply.error.kind == SyncError::Kind::NotFound) {
            createDataKey();
            return;
        }
        if (!reply.ok()) {
            retryKeyStep(KeyStep::List, reply.error);
            return;
        }
        scheduleKeyStep(KeyStep::List, kRetrySeconds); // for the recovery code, or a device that gets K
    });
}

void SyncAccount::createDataKey()
{
    // The first device of the account: K and the recovery code, stored together in one write.
    SecretBytes dataKey = SyncCrypto::newDataKey();
    SecretBytes code = SyncCrypto::newRecoveryCode();
    const bool saved = m_secrets->update([&](SyncSecrets::Values &v) {
        v.dataKey = std::move(dataKey);
        v.recoveryCode = std::move(code);
    });
    if (!saved) {
        Q_EMIT keyringWriteFailed();
        scheduleKeyStep(KeyStep::List, kRetrySeconds);
        return;
    }
    uploadRecovery();
}

void SyncAccount::uploadRecovery()
{
    const SyncSecrets::Values &values = m_secrets->values();
    const QByteArray sealed = SyncCrypto::sealWithRecovery(values.dataKey, values.recoveryCode);
    m_api->putRecovery(sealed, [this](const SyncReply &reply) {
        if (m_phase != Phase::WaitingForKey) {
            return;
        }
        if (reply.error.kind == SyncError::Kind::KeyExists) {
            // Another device created K first: drop ours and wait for theirs.
            m_secrets->update([](SyncSecrets::Values &v) {
                v.dataKey = SecretBytes();
                v.recoveryCode = SecretBytes();
            });
            runKeyStep(KeyStep::List);
            return;
        }
        if (!reply.ok()) {
            retryKeyStep(KeyStep::UploadRecovery, reply.error); // K stays stored
            return;
        }
        writeSetting(kKeyConfirmed, true);
        Settings::sync(*m_settings);
        const QString code = SyncCrypto::formatRecoveryCode(m_secrets->values().recoveryCode);
        becomeReady();
        Q_EMIT recoveryCodeCreated(code);
        recordRecoveryUpload();
    });
}

void SyncAccount::pollKey()
{
    m_keyStep = KeyStep::PollKey;
    m_api->getKey([this](const SyncReply &reply) {
        if (m_phase != Phase::WaitingForKey) {
            return;
        }
        if (reply.ok()) {
            receiveKey(reply.json);
            return;
        }
        if (reply.error.kind == SyncError::Kind::NotFound) {
            // Not sent yet. The approver may be gone meanwhile (signed out, disconnected): the
            // devices are listed again after a while, so the code moves to a device that can
            // approve now if its own no longer can.
            if (m_keyListTaken.elapsed() >= kKeyRelistSeconds * m_unitMs) {
                runKeyStep(KeyStep::List);
                return;
            }
            scheduleKeyStep(KeyStep::PollKey, keyPollSeconds()); // not sent yet
            return;
        }
        retryKeyStep(KeyStep::PollKey, reply.error);
    });
}

void SyncAccount::receiveKey(const QJsonObject &json)
{
    // Only from the device whose public key the shown code covers.
    const QString fromId = json.value(QLatin1String("from_device_id")).toString();
    const QByteArray fromKey = QByteArray::fromBase64(json.value(QLatin1String("from_public_key")).toString().toLatin1());
    if (m_approver.id.isEmpty() || fromId != m_approver.id || fromKey != m_approver.publicKey) {
        rejectKey();
        return;
    }
    // ...and only once the user saw the same code there. Until then the box waits here, unopened,
    // and the server is not asked again: the approver cannot change under the code meanwhile.
    if (m_confirmedKey != fromKey) {
        m_keyTimer.stop();
        m_heldKey = json;
        return;
    }
    const QByteArray sealed = QByteArray::fromBase64(json.value(QLatin1String("sealed_key")).toString().toLatin1());
    std::optional<SecretBytes> dataKey = SyncCrypto::openBox(sealed, fromKey, m_secrets->values().deviceKey);
    if (!dataKey || dataKey->size() != SyncCrypto::KeyBytes) {
        rejectKey();
        return;
    }
    // Stored before the server hears of it: K can never be lost in between.
    if (!m_secrets->update([&](SyncSecrets::Values &v) { v.dataKey = std::move(*dataKey); })) {
        Q_EMIT keyringWriteFailed();
        scheduleKeyStep(KeyStep::PollKey, kRetrySeconds); // the box can be read again
        return;
    }
    confirmKey();
}

bool SyncAccount::verificationConfirmed() const
{
    return !m_approver.publicKey.isEmpty() && m_confirmedKey == m_approver.publicKey;
}

bool SyncAccount::confirmVerificationCode(const QString &code)
{
    if (m_phase != Phase::WaitingForKey || m_approver.id.isEmpty() || code.isEmpty() || code != m_verificationCode) {
        return false;
    }
    if (!verificationConfirmed()) {
        m_confirmedKey = m_approver.publicKey;
        Q_EMIT verificationConfirmedChanged(true);
    }
    if (m_heldKey) {
        const QJsonObject json = *std::exchange(m_heldKey, std::nullopt);
        receiveKey(json);
    } else if (m_keyStep == KeyStep::PollKey && m_keyTimer.isActive()) {
        m_keyTimer.stop();
        pollKey(); // the key may be there already: no need to wait for the next poll
    }
    return true;
}

void SyncAccount::setApprover(const Device &approver)
{
    if (approver.id != m_approver.id || approver.publicKey != m_approver.publicKey) {
        m_heldKey.reset();
        const bool wasConfirmed = verificationConfirmed();
        m_confirmedKey.clear();
        m_approver = approver;
        if (wasConfirmed) {
            Q_EMIT verificationConfirmedChanged(false);
        }
        return;
    }
    m_approver = approver; // the same device: its name or last seen may be newer
}

void SyncAccount::rejectKey()
{
    m_keyTimer.stop();
    Q_EMIT keyRejected();
}

void SyncAccount::confirmKey()
{
    m_keyStep = KeyStep::ConfirmKey;
    m_api->confirmKey([this](const SyncReply &reply) {
        if (m_phase != Phase::WaitingForKey) {
            return;
        }
        if (!reply.ok()) {
            retryKeyStep(KeyStep::ConfirmKey, reply.error);
            return;
        }
        writeSetting(kKeyConfirmed, true);
        Settings::sync(*m_settings);
        becomeReady();
    });
}

void SyncAccount::becomeReady()
{
    m_keyTimer.stop();
    setApprover(Device());
    if (!m_verificationCode.isEmpty()) {
        m_verificationCode.clear();
        Q_EMIT verificationCodeChanged(QString());
    }
    setPhase(Phase::Ready);
    Q_EMIT keyReady();
}

// ---------------------------------------------------------------------------------------------
// Approving other devices
// ---------------------------------------------------------------------------------------------

void SyncAccount::checkPendingDevices(int count)
{
    if (m_phase != Phase::Ready || m_checkingPending) {
        return;
    }
    // None waiting: listed again only if the list still shows one waiting (it got its key
    // since), so the device list stops saying "Waiting for approval".
    if (count <= 0 && std::none_of(m_devices.cbegin(), m_devices.cend(), [](const Device &d) { return d.pending && !d.current; })) {
        return;
    }
    m_checkingPending = true;
    m_api->listDevices([this](const SyncReply &reply) {
        m_checkingPending = false;
        if (m_phase != Phase::Ready) {
            return;
        }
        if (takeDevices(reply)) {
            askAboutPendingDevices();
        } else if (reply.error.transient()) {
            Q_EMIT requestFailed(reply.error);
        }
    });
}

void SyncAccount::askAboutPendingDevices()
{
    for (const Device &d : std::as_const(m_devices)) {
        // A device approved on the site but never collected cannot show a code yet.
        if (!d.pending || d.current || !d.lastSeen.isValid() || m_ignored.contains(d.id) || m_asked.contains(d.id)) {
            continue;
        }
        const QString code = SyncCrypto::verificationCode(m_publicKey, d.publicKey);
        if (code.isEmpty()) {
            continue;
        }
        m_asked.insert(d.id, d.publicKey);
        Q_EMIT approvalRequested(d.id, d.name, d.platform, code);
    }
}

void SyncAccount::approveDevice(const QString &id)
{
    const auto it = std::find_if(m_devices.cbegin(), m_devices.cend(), [&id](const Device &d) { return d.id == id; });
    if (m_phase != Phase::Ready || it == m_devices.cend() || it->current || dataKey().isEmpty()) {
        SyncError e;
        e.kind = SyncError::Kind::NotFound;
        Q_EMIT approvalFailed(id, e);
        return;
    }
    // The user compared the code of the key it was shown for; a server that swaps the key since
    // must not get K boxed for its own.
    const auto shown = m_asked.constFind(id);
    if (shown == m_asked.cend() || shown.value() != it->publicKey) {
        m_asked.remove(id);
        SyncError e;
        e.kind = SyncError::Kind::NotFound;
        Q_EMIT approvalFailed(id, e);
        // Asked again with the new code once the caller is done with this answer.
        QMetaObject::invokeMethod(this, [this]() {
            if (m_phase == Phase::Ready) {
                askAboutPendingDevices();
            }
        }, Qt::QueuedConnection);
        return;
    }
    const QByteArray sealed = SyncCrypto::box(dataKey(), it->publicKey, m_secrets->values().deviceKey);
    if (sealed.isEmpty()) {
        SyncError e;
        e.kind = SyncError::Kind::Format;
        Q_EMIT approvalFailed(id, e);
        return;
    }
    m_api->putDeviceKey(id, sealed, [this, id](const SyncReply &reply) {
        if (m_phase != Phase::Ready) {
            return;
        }
        if (reply.ok() || reply.error.kind == SyncError::Kind::AlreadyReady) {
            Q_EMIT deviceApproved(id);
            return;
        }
        m_asked.remove(id); // ask again when it next shows up
        Q_EMIT approvalFailed(id, reply.error);
    });
}

void SyncAccount::ignoreDevice(const QString &id)
{
    m_ignored.insert(id);
}

void SyncAccount::askAgain(const QString &id)
{
    if (m_phase != Phase::Ready) {
        return;
    }
    m_ignored.remove(id);
    m_asked.remove(id);
    askAboutPendingDevices();
}

// ---------------------------------------------------------------------------------------------
// Recovery code
// ---------------------------------------------------------------------------------------------

void SyncAccount::useRecoveryCode(const QString &text)
{
    if (m_phase != Phase::WaitingForKey || m_usingRecoveryCode) {
        return;
    }
    std::optional<SecretBytes> parsed = SyncCrypto::parseRecoveryCode(text);
    if (!parsed) {
        Q_EMIT recoveryCodeWrong();
        return;
    }
    m_usingRecoveryCode = true;
    auto code = std::make_shared<SecretBytes>(std::move(*parsed));
    m_api->getRecovery([this, code](const SyncReply &reply) {
        m_usingRecoveryCode = false;
        if (m_phase != Phase::WaitingForKey) {
            return;
        }
        if (!reply.ok()) {
            if (reply.error.kind == SyncError::Kind::NotFound) {
                Q_EMIT recoveryCodeWrong(); // the account has no recovery copy
            } else {
                Q_EMIT requestFailed(reply.error);
            }
            return;
        }
        const QByteArray sealed = QByteArray::fromBase64(reply.json.value(QLatin1String("recovery")).toString().toLatin1());
        std::optional<SecretBytes> dataKey = SyncCrypto::openWithRecovery(sealed, *code);
        if (!dataKey || dataKey->size() != SyncCrypto::KeyBytes) {
            Q_EMIT recoveryCodeWrong();
            return;
        }
        m_keyTimer.stop();
        // Stored before the server hears of it, as a key from another device.
        if (!m_secrets->update([&](SyncSecrets::Values &v) { v.dataKey = std::move(*dataKey); })) {
            Q_EMIT keyringWriteFailed();
            scheduleKeyStep(KeyStep::PollKey, keyPollSeconds());
            return;
        }
        confirmKey();
    });
}

void SyncAccount::setRecoveryShowable(bool showable)
{
    if (m_recoveryShowable == showable) {
        return;
    }
    m_recoveryShowable = showable;
    Q_EMIT recoveryCodeShowableChanged(showable);
}

void SyncAccount::recordRecoveryUpload()
{
    m_api->getRecovery([this](const SyncReply &reply) {
        if (m_phase != Phase::Ready || !reply.ok()) {
            return;
        }
        if (reply.json.value(QLatin1String("updated_by")).toString() == deviceId()) {
            writeSetting(kRecoveryUpdatedAt, reply.json.value(QLatin1String("updated_at")).toString());
            Settings::sync(*m_settings);
            setRecoveryShowable(!m_secrets->values().recoveryCode.isEmpty());
        }
    });
}

void SyncAccount::refreshRecoveryStatus()
{
    if (m_phase != Phase::Ready) {
        return;
    }
    m_api->getRecovery([this](const SyncReply &reply) {
        if (m_phase != Phase::Ready) {
            return;
        }
        if (reply.error.kind == SyncError::Kind::NotFound) {
            setRecoveryShowable(false);
            return;
        }
        if (!reply.ok()) {
            return;
        }
        const QString updatedBy = reply.json.value(QLatin1String("updated_by")).toString();
        const QString updatedAt = reply.json.value(QLatin1String("updated_at")).toString();
        const QString mine = m_settings->value(key(kRecoveryUpdatedAt)).toString();
        const bool hasCode = !m_secrets->values().recoveryCode.isEmpty();
        if (hasCode && updatedBy == deviceId() && mine.isEmpty()) {
            // Uploaded at creation, but its time never arrived: no other device writes as this one.
            writeSetting(kRecoveryUpdatedAt, updatedAt);
            Settings::sync(*m_settings);
            setRecoveryShowable(true);
            return;
        }
        setRecoveryShowable(hasCode && updatedBy == deviceId() && updatedAt == mine);
    });
}

void SyncAccount::regenerateRecoveryCode()
{
    if (m_phase != Phase::Ready || m_regenerating || dataKey().isEmpty()) {
        return;
    }
    m_regenerating = true;
    auto code = std::make_shared<SecretBytes>(SyncCrypto::newRecoveryCode());
    const QByteArray sealed = SyncCrypto::sealWithRecovery(dataKey(), *code);
    // Until the new code is stored, the code in the keyring may not be the server's.
    const QVariant previousStamp = m_settings->value(key(kRecoveryUpdatedAt));
    writeSetting(kRecoveryUpdatedAt, kPendingStamp);
    Settings::sync(*m_settings);
    setRecoveryShowable(false);
    m_api->putRecovery(sealed, [this, code, previousStamp](const SyncReply &reply) {
        m_regenerating = false;
        if (m_phase != Phase::Ready) {
            return;
        }
        if (!reply.ok()) {
            if (!reply.error.transient()) {
                writeSetting(kRecoveryUpdatedAt, previousStamp); // the server kept the old copy
                Settings::sync(*m_settings);
            }
            Q_EMIT recoveryCodeFailed(reply.error);
            refreshRecoveryStatus();
            return;
        }
        const QString text = SyncCrypto::formatRecoveryCode(*code);
        const bool saved = m_secrets->update([&](SyncSecrets::Values &v) { v.recoveryCode = std::move(*code); });
        if (!saved) {
            Q_EMIT keyringWriteFailed();
        }
        // The old code no longer works: the new one must be seen even if it was not stored.
        Q_EMIT recoveryCodeCreated(text);
        if (saved) {
            recordRecoveryUpload();
        }
    });
}

// ---------------------------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------------------------

void SyncAccount::refreshDevices()
{
    if (m_phase != Phase::Ready && m_phase != Phase::WaitingForKey) {
        return;
    }
    m_api->listDevices([this](const SyncReply &reply) {
        if (m_phase != Phase::Ready && m_phase != Phase::WaitingForKey) {
            return;
        }
        if (!takeDevices(reply)) {
            if (reply.error.transient()) {
                Q_EMIT requestFailed(reply.error);
            }
            return;
        }
        if (m_phase == Phase::Ready) {
            askAboutPendingDevices();
        }
    });
}

void SyncAccount::disconnectDevice(const QString &id)
{
    if ((m_phase != Phase::Ready && m_phase != Phase::WaitingForKey) || id.isEmpty() || id == deviceId()) {
        return;
    }
    m_api->disconnectDevice(id, [this, id](const SyncReply &reply) {
        if (m_phase != Phase::Ready && m_phase != Phase::WaitingForKey) {
            return;
        }
        if (!reply.ok() && reply.error.kind != SyncError::Kind::NotFound) {
            Q_EMIT disconnectFailed(id, reply.error);
            return;
        }
        m_devices.removeIf([&id](const Device &d) { return d.id == id; });
        Q_EMIT devicesChanged();
        Q_EMIT deviceDisconnected(id);
    });
}

// ---------------------------------------------------------------------------------------------
// Sign out
// ---------------------------------------------------------------------------------------------

void SyncAccount::signOut()
{
    if (m_signingOut) {
        return; // already on its way: a second reset would drop the first request
    }
    if (m_phase == Phase::SigningIn) {
        abandonSignIn();
        Q_EMIT signedOut(true);
        return;
    }
    if (m_phase == Phase::SignedOut || m_secrets->refreshToken().isEmpty()) {
        signOutLocally();
        Q_EMIT signedOut(false);
        return;
    }
    // Nothing else may run meanwhile; then, success or not, everything here goes.
    m_keyTimer.stop();
    m_api->reset();
    m_signingOut = true;
    m_api->disconnectDevice(deviceId(), [this](const SyncReply &reply) {
        m_signingOut = false;
        const bool remoteDone = reply.ok() || reply.error.kind == SyncError::Kind::NotFound;
        signOutLocally();
        Q_EMIT signedOut(remoteDone);
    });
}

void SyncAccount::signOutLocally()
{
    m_signingOut = false;
    m_usingRecoveryCode = false;
    m_regenerating = false;
    m_checkingPending = false;
    setRecoveryShowable(false);
    clearSignInCode();
    m_keyTimer.stop();
    m_devices.clear();
    m_approver = Device();
    m_verificationCode.clear();
    m_confirmedKey.clear();
    m_heldKey.reset();
    m_asked.clear();
    m_ignored.clear();
    m_api->reset();
    // A keyring that refuses to delete would keep the keys of a device shown as signed out.
    if (m_secrets->clear()) {
        m_settings->remove(kKeyringWipePending);
    } else {
        m_settings->setValue(kKeyringWipePending, true);
    }
    m_settings->remove(kGroup);
    Settings::sync(*m_settings);
    m_publicKey.clear();
    setDeviceLimitReached(false);
    setPhase(Phase::SignedOut);
}

bool SyncAccount::keyringWipePending() const
{
    return m_settings->value(kKeyringWipePending, false).toBool();
}

void SyncAccount::retryKeyringWipe()
{
    if (!keyringWipePending() || isLinked() || m_phase != Phase::SignedOut) {
        return;
    }
    if (m_secrets->clear()) {
        m_settings->remove(kKeyringWipePending);
        Settings::sync(*m_settings);
    }
}

void SyncAccount::onSignedOutRemotely()
{
    if (m_phase == Phase::SignedOut || m_phase == Phase::SigningIn || m_signingOut) {
        return;
    }
    signOutLocally();
    Q_EMIT signedOutRemotely();
}
