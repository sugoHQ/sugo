// SPDX-License-Identifier: GPL-3.0-or-later
#include "SecretHandoff.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMap>
#include <QSaveFile>
#include <QStandardPaths>

#include <sodium.h>

namespace
{
Q_LOGGING_CATEGORY(lcHandoff, "sugo.platform.secrethandoff")

constexpr int kFormat = 1;

using Secrets = QMap<QString, QString>;

void wipe(QByteArray &bytes)
{
    if (!bytes.isEmpty()) {
        sodium_memzero(bytes.data(), size_t(bytes.size()));
    }
}

/// The secrets waiting at `path`: none (an empty map) when there is no file, nothing when it
/// cannot be read or is not a handoff file.
std::optional<Secrets> readFile(const QString &path)
{
    QFile file(path);
    if (!file.exists()) {
        return Secrets();
    }
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(lcHandoff).noquote() << "Cannot read the secrets left by the previous version at" << path << ":" << file.errorString();
        return std::nullopt;
    }
    QByteArray bytes = file.readAll();
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    wipe(bytes);
    const QJsonObject root = document.object();
    if (!document.isObject() || root.value(QLatin1String("format")).toInt() != kFormat) {
        qCWarning(lcHandoff).noquote() << path << "is not a file of secrets Sugo can read; left as it is";
        return std::nullopt;
    }
    Secrets secrets;
    const QJsonObject entries = root.value(QLatin1String("secrets")).toObject();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (const QString secret = it.value().toString(); !it.key().isEmpty() && !secret.isEmpty()) {
            secrets.insert(it.key(), secret);
        }
    }
    return secrets;
}

/// Replaces the file at `path` with `secrets`, readable by the user only from the first byte on.
bool writeFile(const QString &path, const Secrets &secrets)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        qCWarning(lcHandoff).noquote() << "Cannot create the folder of" << path;
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qCWarning(lcHandoff).noquote() << "Cannot write" << path << ":" << file.errorString();
        return false;
    }
    // The temporary file, before any secret is in it (QSaveFile gives it the mode of the file it replaces).
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        qCWarning(lcHandoff).noquote() << "Cannot make" << path << "private:" << file.errorString();
        file.cancelWriting();
        return false;
    }
    QJsonObject entries;
    for (auto it = secrets.begin(); it != secrets.end(); ++it) {
        entries.insert(it.key(), it.value());
    }
    QByteArray bytes = QJsonDocument(QJsonObject{{QLatin1String("format"), kFormat}, {QLatin1String("secrets"), entries}})
                           .toJson(QJsonDocument::Compact);
    const bool written = file.write(bytes) == bytes.size();
    wipe(bytes);
    if (!written || !file.commit()) {
        qCWarning(lcHandoff).noquote() << "Cannot write" << path << ":" << file.errorString();
        file.cancelWriting();
        return false;
    }
    return true;
}

/// Overwrites the file before removing it, so its secrets are not left in the freed blocks
/// (as far as the file system rewrites in place).
void removeFile(const QString &path)
{
    QFile file(path);
    if (file.open(QIODevice::ReadWrite)) {
        const QByteArray zeros(file.size(), '\0');
        file.write(zeros);
        file.flush();
        file.close();
    }
    if (!file.remove()) {
        qCWarning(lcHandoff).noquote() << "Cannot remove" << path << ":" << file.errorString();
    }
}
} // namespace

namespace SecretHandoff
{
QString defaultPath()
{
    if (!qEnvironmentVariableIsEmpty("SUGO_DATA_DIR")) {
        return QString();
    }
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/sugo/keychain-handoff.json");
}

int handOver(SecretHandoffStore &store, const QString &path)
{
    if (path.isEmpty()) {
        return 0;
    }
    const std::optional<QStringList> keys = store.keys();
    if (!keys) {
        return 0;
    }
    // Those still waiting (a start that could not write them all back) go on waiting with the new ones.
    // A file that cannot be read may be the only copy of those: never replaced, and nothing
    // moved (the next build asks, as before).
    std::optional<Secrets> waiting = readFile(path);
    if (!waiting) {
        return 0;
    }
    Secrets secrets = std::move(*waiting);
    QStringList moved;
    for (const QString &key : *keys) {
        // Unreadable without asking (an item of an older build the user was never asked about,
        // or refused): left in the keyring, where the next build will ask for it as before.
        if (const auto secret = store.readWithoutAsking(key); secret && !secret->isEmpty()) {
            secrets.insert(key, *secret);
            moved.append(key);
        }
    }
    if (moved.isEmpty()) {
        return 0;
    }
    if (!writeFile(path, secrets)) {
        return 0; // nothing deleted: the next build asks, as before
    }
    for (const QString &key : std::as_const(moved)) {
        if (!store.write(key, QString())) {
            // Then the next build cannot replace it either, and takeBack() leaves it alone.
            qCWarning(lcHandoff) << "Cannot remove" << key << "from the keyring after saving it for the update";
        }
    }
    qCInfo(lcHandoff) << "Kept" << moved.size() << "secret(s) for the updated version";
    return int(moved.size());
}

int takeBack(SecretHandoffStore &store, const QString &path)
{
    if (path.isEmpty()) {
        return 0;
    }
    const std::optional<Secrets> waiting = readFile(path);
    if (!waiting || waiting->isEmpty()) {
        if (waiting && QFileInfo::exists(path)) {
            removeFile(path); // a file with nothing in it
        }
        return 0;
    }
    Secrets remaining;
    std::optional<QStringList> stored; // listed once, the first time a write fails
    int written = 0;
    for (auto it = waiting->begin(); it != waiting->end(); ++it) {
        // A secret the user typed again since (the Keychain was locked when this build first
        // started, so the file kept the old one) is newer than the one waiting: it wins.
        if (const auto secret = store.readWithoutAsking(it.key()); secret && !secret->isEmpty()) {
            continue;
        }
        if (store.write(it.key(), it.value())) {
            ++written;
            continue;
        }
        if (!stored) {
            stored = store.keys();
        }
        if (stored && stored->contains(it.key())) {
            // The previous version could not delete its item, so this one cannot replace it;
            // the secret is still there, and macOS asks for it as before.
            qCWarning(lcHandoff) << "Cannot store" << it.key() << "as this version's: an item of the previous version is in the way";
            continue;
        }
        remaining.insert(it.key(), it.value()); // the only copy: tried again at the next start
    }
    if (remaining.isEmpty()) {
        removeFile(path);
    } else {
        qCWarning(lcHandoff) << remaining.size() << "secret(s) left by the previous version could not be stored; trying again at the next start";
        if (remaining.size() != waiting->size()) {
            writeFile(path, remaining);
        }
    }
    if (written > 0) {
        qCInfo(lcHandoff) << "Took back" << written << "secret(s) from the previous version";
    }
    return written;
}
} // namespace SecretHandoff
