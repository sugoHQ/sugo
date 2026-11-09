// SPDX-License-Identifier: GPL-3.0-or-later
#include <QTest>

#include "UpdateSignature.h"

namespace
{
// Made with OpenSSL 3 (`openssl genpkey -algorithm ed25519`, `openssl pkeyutl -sign -rawin`), which
// is what the release workflow uses, so a difference of format between the signer and libsodium
// shows up here and not as updates nobody can install. A key made only for this test.
const QByteArray kPublicKey = QByteArrayLiteral("KtvE6yqmOclupAX+5mKnsx7M2aciC2wDDMb5DRCIO08=");
const QString kVersion = QStringLiteral("1.2.3");
const QString kUrl = QStringLiteral("https://github.com/MineraleYT/sugo/releases/download/v1.2.3/Sugo-1.2.3-x86_64.AppImage");
const QString kSha256 = QStringLiteral("ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb");
const qint64 kSize = 12345;
const QString kSignature = QStringLiteral(
    "H9wXekKKCfKR/itneXIAnrSjzQOLswiFQo3aAgLLcPpAAdbHXnIdaNxETKG2wIeQwRYoUiHch2x535rmIZ3uBg==");
}

class UpdateSignatureTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void messageIsTheSignedText()
    {
        QCOMPARE(UpdateSignature::message(kVersion, kUrl, kSha256, kSize),
                 QByteArrayLiteral("sugo-update-v1\n1.2.3\n") + kUrl.toUtf8() + '\n' + kSha256.toUtf8() + "\n12345");
    }

    void aSignatureMadeByOpenSslIsAccepted()
    {
        QVERIFY(UpdateSignature::verify(kPublicKey, UpdateSignature::message(kVersion, kUrl, kSha256, kSize), kSignature));
    }

    void anyChangeOfWhatIsSignedIsRefused()
    {
        QVERIFY(!UpdateSignature::verify(kPublicKey, UpdateSignature::message(QStringLiteral("1.2.4"), kUrl, kSha256, kSize), kSignature));
        QVERIFY(!UpdateSignature::verify(kPublicKey, UpdateSignature::message(kVersion, kUrl + QLatin1Char('x'), kSha256, kSize), kSignature));
        QVERIFY(!UpdateSignature::verify(kPublicKey, UpdateSignature::message(kVersion, kUrl, QString(kSha256).replace(0, 1, QLatin1Char('d')), kSize), kSignature));
        QVERIFY(!UpdateSignature::verify(kPublicKey, UpdateSignature::message(kVersion, kUrl, kSha256, kSize + 1), kSignature));
    }

    void malformedInputIsRefused()
    {
        const QByteArray message = UpdateSignature::message(kVersion, kUrl, kSha256, kSize);
        QVERIFY(!UpdateSignature::verify(kPublicKey, message, QString()));
        QVERIFY(!UpdateSignature::verify(kPublicKey, message, QStringLiteral("not base64 !!")));
        QVERIFY(!UpdateSignature::verify(kPublicKey, message, kSignature.left(40)));
        QVERIFY(!UpdateSignature::verify(QByteArray(), message, kSignature));
        QVERIFY(!UpdateSignature::verify(QByteArrayLiteral("c2hvcnQ="), message, kSignature));
        // Another key does not accept it either.
        QVERIFY(!UpdateSignature::verify(QByteArrayLiteral("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="), message, kSignature));
    }
};

QTEST_MAIN(UpdateSignatureTest)
#include "tst_updatesignature.moc"
