// SPDX-License-Identifier: GPL-3.0-or-later
#include "SyncSecrets.h"

#include "SecretStore.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <sodium.h>

namespace
{
const QLatin1String kVersion("v");
const QLatin1String kRefreshToken("refreshToken");
const QLatin1String kDeviceKey("deviceKey");
const QLatin1String kDataKey("dataKey");
const QLatin1String kRecoveryCode("recoveryCode");
const QLatin1String kPendingRecoveryCode("pendingRecoveryCode");

void wipe(QByteArray &bytes)
{
    if (!bytes.isEmpty()) {
        bytes.detach();
        sodium_memzero(bytes.data(), size_t(bytes.size()));
    }
    bytes.clear();
}

/// A key field: absent is fine (empty), present must be 32 bytes of Base64.
bool readKey(const QJsonObject &object, QLatin1String name, SecretBytes *out)
{
    const QJsonValue value = object.value(name);
    if (value.isUndefined() || value.isNull()) {
        return true;
    }
    if (!value.isString()) {
        return false;
    }
    QByteArray text = value.toString().toLatin1();
    *out = SecretBytes::fromBase64(text);
    wipe(text);
    return out->size() == SyncCrypto::KeyBytes;
}
} // namespace

SyncSecrets::Values SyncSecrets::Values::copy() const
{
    Values v;
    v.refreshToken = refreshToken;
    v.deviceKey = deviceKey.copy();
    v.dataKey = dataKey.copy();
    v.recoveryCode = recoveryCode.copy();
    v.pendingRecoveryCode = pendingRecoveryCode.copy();
    return v;
}

SyncSecrets::SyncSecrets(SecretStore *store)
    : m_store(store)
{
}

bool SyncSecrets::probe()
{
    const QString value = QString::fromLatin1(SyncCrypto::newDataKey().toBase64());
    if (value.isEmpty()) {
        // libsodium did not start: an empty value would be a removal, which "works" without a keyring.
        m_available = false;
        return false;
    }
    const bool written = m_store->write(probeKey(), value);
    // Removed even when the first write seemed to fail: it may have stored something.
    const bool removed = m_store->write(probeKey(), QString());
    m_available = written && removed;
    return m_available;
}

SyncSecrets::LoadResult SyncSecrets::load()
{
    m_values = Values();
    m_unsavedRefreshToken.clear();
    const std::optional<QString> text = m_store->read(itemKey());
    if (!text) {
        return LoadResult::Failed;
    }
    if (text->isEmpty()) {
        return LoadResult::Empty;
    }
    QByteArray json = text->toUtf8();
    LoadResult result = LoadResult::Unreadable;
    std::optional<Values> values = fromJson(json, &result);
    wipe(json);
    if (values) {
        if (values->isEmpty()) {
            return LoadResult::Empty;
        }
        m_values = std::move(*values);
    }
    return result;
}

QString SyncSecrets::refreshToken() const
{
    return m_unsavedRefreshToken.isEmpty() ? m_values.refreshToken : m_unsavedRefreshToken;
}

bool SyncSecrets::update(const std::function<void(Values &)> &change)
{
    Values next = m_values.copy();
    next.refreshToken = refreshToken();
    change(next);
    return write(std::move(next));
}

bool SyncSecrets::setRefreshToken(const QString &token)
{
    return update([&token](Values &v) { v.refreshToken = token; });
}

bool SyncSecrets::replace(Values values)
{
    m_unsavedRefreshToken.clear();
    return write(std::move(values));
}

bool SyncSecrets::write(Values values)
{
    QByteArray json = toJson(values);
    const bool ok = m_store->write(itemKey(), QString::fromUtf8(json));
    wipe(json);
    if (ok) {
        m_values = std::move(values);
        m_unsavedRefreshToken.clear();
    } else if (values.refreshToken != m_values.refreshToken) {
        // The server has spent the old token: the new one must not be lost with the write.
        m_unsavedRefreshToken = values.refreshToken;
    }
    return ok;
}

bool SyncSecrets::clear()
{
    m_values = Values();
    m_unsavedRefreshToken.clear();
    return m_store->write(itemKey(), QString());
}

QByteArray SyncSecrets::toJson(const Values &values)
{
    QJsonObject object;
    object.insert(kVersion, Version);
    if (!values.refreshToken.isEmpty()) {
        object.insert(kRefreshToken, values.refreshToken);
    }
    if (!values.deviceKey.isEmpty()) {
        object.insert(kDeviceKey, QString::fromLatin1(values.deviceKey.toBase64()));
    }
    if (!values.dataKey.isEmpty()) {
        object.insert(kDataKey, QString::fromLatin1(values.dataKey.toBase64()));
    }
    if (!values.recoveryCode.isEmpty()) {
        object.insert(kRecoveryCode, SyncCrypto::formatRecoveryCode(values.recoveryCode));
    }
    if (!values.pendingRecoveryCode.isEmpty()) {
        object.insert(kPendingRecoveryCode, SyncCrypto::formatRecoveryCode(values.pendingRecoveryCode));
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

std::optional<SyncSecrets::Values> SyncSecrets::fromJson(const QByteArray &json, LoadResult *result)
{
    auto fail = [result](LoadResult r) {
        if (result) {
            *result = r;
        }
        return std::nullopt;
    };
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        return fail(LoadResult::Unreadable);
    }
    const QJsonObject object = doc.object();
    const QJsonValue version = object.value(kVersion);
    if (!version.isDouble() || version.toDouble() < 1) {
        return fail(LoadResult::Unreadable);
    }
    if (version.toDouble() > Version) {
        return fail(LoadResult::NewerVersion);
    }
    Values values;
    const QJsonValue token = object.value(kRefreshToken);
    if (!token.isUndefined() && !token.isString()) {
        return fail(LoadResult::Unreadable);
    }
    values.refreshToken = token.toString();
    if (!readKey(object, kDeviceKey, &values.deviceKey) || !readKey(object, kDataKey, &values.dataKey)) {
        return fail(LoadResult::Unreadable);
    }
    const QJsonValue code = object.value(kRecoveryCode);
    if (!code.isUndefined() && !code.isNull()) {
        std::optional<SecretBytes> parsed = code.isString() ? SyncCrypto::parseRecoveryCode(code.toString()) : std::nullopt;
        if (!parsed) {
            return fail(LoadResult::Unreadable);
        }
        values.recoveryCode = std::move(*parsed);
    }
    // Only a convenience: an unreadable pending code is dropped, never the keys with it.
    const QJsonValue pendingCode = object.value(kPendingRecoveryCode);
    if (pendingCode.isString()) {
        if (std::optional<SecretBytes> parsed = SyncCrypto::parseRecoveryCode(pendingCode.toString())) {
            values.pendingRecoveryCode = std::move(*parsed);
        }
    }
    if (result) {
        *result = LoadResult::Loaded;
    }
    return values;
}
