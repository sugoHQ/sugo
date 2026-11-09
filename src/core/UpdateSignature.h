// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>

/**
 * The signature of an update. The checksum in the feed only says the file arrived whole: whoever
 * can change the feed can change both. So every download entry of the feed (the top-level one for
 * the x86_64 AppImage, "linux-aarch64", "macos") also carries "signature", an Ed25519 signature
 * of message() made by the release workflow with a key that is not in the repository, and the
 * program carries the public key (packaging/update-public-key.txt, base64 of its 32 bytes).
 *
 * With no key compiled in, nothing is checked (builds from source and releases before the key was
 * set); with one, an entry without a valid signature is only reported, never installed.
 */
namespace UpdateSignature
{
/// What is signed, byte for byte (the workflow's .github/scripts/sign-update-feed.sh builds the same):
/// "sugo-update-v1", version, url, sha256 and size, one per line, no line after the last.
QByteArray message(const QString &version, const QString &url, const QString &sha256, qint64 size);

/// True if `signatureBase64` is a valid Ed25519 signature of `message` for `publicKeyBase64`.
bool verify(const QByteArray &publicKeyBase64, const QByteArray &message, const QString &signatureBase64);

/// The public key this program was built with (base64), empty if none.
QByteArray compiledPublicKey();
}
