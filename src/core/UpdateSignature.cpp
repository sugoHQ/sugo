// SPDX-License-Identifier: GPL-3.0-or-later
#include "UpdateSignature.h"

#include <sodium.h>

namespace UpdateSignature
{
QByteArray message(const QString &version, const QString &url, const QString &sha256, qint64 size)
{
    QByteArray out = QByteArrayLiteral("sugo-update-v1\n");
    out += version.toUtf8();
    out += '\n';
    out += url.toUtf8();
    out += '\n';
    out += sha256.toUtf8();
    out += '\n';
    out += QByteArray::number(size);
    return out;
}

bool verify(const QByteArray &publicKeyBase64, const QByteArray &message, const QString &signatureBase64)
{
    static const bool ready = sodium_init() >= 0;
    if (!ready) {
        return false;
    }
    const auto key = QByteArray::fromBase64Encoding(publicKeyBase64.trimmed(), QByteArray::AbortOnBase64DecodingErrors);
    const auto signature =
        QByteArray::fromBase64Encoding(signatureBase64.trimmed().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!key || !signature || key.decoded.size() != int(crypto_sign_PUBLICKEYBYTES)
        || signature.decoded.size() != int(crypto_sign_BYTES)) {
        return false;
    }
    return crypto_sign_verify_detached(reinterpret_cast<const unsigned char *>(signature.decoded.constData()),
                                       reinterpret_cast<const unsigned char *>(message.constData()),
                                       static_cast<unsigned long long>(message.size()),
                                       reinterpret_cast<const unsigned char *>(key.decoded.constData()))
        == 0;
}

QByteArray compiledPublicKey()
{
#ifdef SUGO_UPDATE_PUBLIC_KEY
    return QByteArray(SUGO_UPDATE_PUBLIC_KEY).trimmed();
#else
    return {};
#endif
}
}
