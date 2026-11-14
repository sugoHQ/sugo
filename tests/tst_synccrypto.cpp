// SPDX-License-Identifier: GPL-3.0-or-later
#include <QRegularExpression>
#include <QTest>

#include <algorithm>

#include "SyncCrypto.h"

namespace
{
// Fixed inputs and the outputs PyNaCl (an independent binding of libsodium) gives for them, so a
// change of format or construction shows up here rather than as blobs other devices cannot read.
QByteArray range(int first, int count)
{
    QByteArray bytes;
    for (int i = 0; i < count; ++i) {
        bytes.append(char(first + i));
    }
    return bytes;
}

const QByteArray kKey = range(0x80, 32);
const QByteArray kNonce = range(0x40, 24);
const QString kUser = QStringLiteral("usr_test");
const QByteArray kPlain = QByteArrayLiteral(R"({"format":1,"presets":[],"tombstones":[]})");
const QByteArray kBlob = QByteArray::fromHex(
    "5355474f01404142434445464748494a4b4c4d4e4f50515253545556578a2e159b299d952ed92846ffd4deef304122061d"
    "277ec7b1e69881632f377ebc6f52750e0957c4204c07044c96ac6feaab660768fc15eaa2ab");

const QByteArray kSecretA = range(1, 32);
const QByteArray kSecretB = range(33, 32);
const QByteArray kPublicA = QByteArray::fromHex("07a37cbc142093c8b755dc1b10e86cb426374ad16aa853ed0bdfc0b2b86d1c7c");
const QByteArray kPublicB = QByteArray::fromHex("5869aff450549732cbaaed5e5df9b30a6da31cb0e5742bad5ad4a1a768f1a67b");
/// range(0xa0, 32) boxed by A for B with kNonce.
const QByteArray kBoxAtoB = QByteArray::fromHex(
    "404142434445464748494a4b4c4d4e4f50515253545556579ed3d68a9d014d06bceef5b03b24ef006b5e2fe7162d5982"
    "6d0ad6b4a34481676ee7d4fdce2cc37f620a50b892255967");
/// Checked with Python's hashlib.blake2b (16 bytes, person "SugoVerifyCode"), first 80 bits in Base32.
const QString kVerification = QStringLiteral("F4ZR-24T5-W1VE-3V1Z");

const QByteArray kRecoveryBytes = range(0xf0, 16);
const QString kRecoveryText = QStringLiteral("Y3RZ-5WZM-YQVF-FY7S-ZBXZ-SZFY-ZZG0");
/// kKey sealed with kRecoveryBytes and kNonce.
const QByteArray kRecoverySealed = QByteArray::fromHex(
    "01404142434445464748494a4b4c4d4e4f5051525354555657a4165888f00702d7c14fc8a5a3ae6406b719042070c4cdc3"
    "e10c2aac0c938205970263fa74f2ed8d9ad717be172fec78");

SecretBytes secret(const QByteArray &bytes)
{
    return SecretBytes::fromRaw(bytes);
}
} // namespace

class SyncCryptoTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(SyncCrypto::init());
    }

    // --- SecretBytes ---

    void secretBytesMoveCopyAndBase64()
    {
        SecretBytes key = secret(kKey);
        const QByteArray base64 = key.toBase64();
        QCOMPARE(base64, kKey.toBase64());
        QVERIFY(SecretBytes::fromBase64(base64) == key);
        QVERIFY(SecretBytes::fromBase64("not base64!").isEmpty());

        SecretBytes copy = key.copy();
        QVERIFY(copy == key);
        SecretBytes moved = std::move(key);
        QVERIFY(key.isEmpty()); // moved from: empty (and wiped)
        QVERIFY(moved == copy);
        QVERIFY(moved != secret(kSecretA));
    }

    // --- Blobs ---

    void blobMatchesTheKnownVector()
    {
        QCOMPARE(SyncCrypto::encryptBlobWithNonce(secret(kKey), kUser, kPlain, kNonce), kBlob);

        const SyncCrypto::OpenedBlob opened = SyncCrypto::decryptBlob(secret(kKey), kUser, kBlob);
        QVERIFY(opened.ok());
        QCOMPARE(opened.format, SyncCrypto::BlobFormat);
        QCOMPARE(opened.plaintext, kPlain);
    }

    void blobRoundTripsWithARandomNonce()
    {
        const SecretBytes key = SyncCrypto::newDataKey();
        QCOMPARE(key.size(), SyncCrypto::KeyBytes);
        const QByteArray first = SyncCrypto::encryptBlob(key, kUser, kPlain);
        const QByteArray second = SyncCrypto::encryptBlob(key, kUser, kPlain);
        QVERIFY(first.startsWith("SUGO\x01"));
        QVERIFY(first != second); // a new nonce each time
        QCOMPARE(SyncCrypto::decryptBlob(key, kUser, first).plaintext, kPlain);
        QCOMPARE(SyncCrypto::decryptBlob(key, kUser, second).plaintext, kPlain);
        QCOMPARE(SyncCrypto::decryptBlob(key, kUser, SyncCrypto::encryptBlob(key, kUser, {})).error,
                 SyncCrypto::OpenedBlob::Error::None);
    }

    void blobOfAnotherUserOrKindIsRefused()
    {
        using Error = SyncCrypto::OpenedBlob::Error;
        QCOMPARE(SyncCrypto::decryptBlob(secret(kKey), QStringLiteral("usr_other"), kBlob).error, Error::Unreadable);
        QCOMPARE(SyncCrypto::decryptBlob(secret(kKey), kUser, kBlob, QStringLiteral("sessions")).error, Error::Unreadable);
        // The same bytes split another way between user and kind are another blob.
        QCOMPARE(SyncCrypto::decryptBlob(secret(kKey), kUser + QLatin1Char('p'), kBlob, QStringLiteral("resets")).error, Error::Unreadable);
        QVERIFY(SyncCrypto::decryptBlob(secret(kKey), kUser, kBlob).plaintext == kPlain);
    }

    void blobWithWrongKeyOrDamageIsRefused()
    {
        using Error = SyncCrypto::OpenedBlob::Error;
        QCOMPARE(SyncCrypto::decryptBlob(SyncCrypto::newDataKey(), kUser, kBlob).error, Error::Unreadable);

        QByteArray damaged = kBlob;
        damaged[damaged.size() - 1] = char(damaged.at(damaged.size() - 1) ^ 1);
        QCOMPARE(SyncCrypto::decryptBlob(secret(kKey), kUser, damaged).error, Error::Unreadable);

        QByteArray otherNonce = kBlob;
        otherNonce[6] = char(otherNonce.at(6) ^ 1);
        QCOMPARE(SyncCrypto::decryptBlob(secret(kKey), kUser, otherNonce).error, Error::Unreadable);
    }

    void notABlobAndNewerFormatsAreTold()
    {
        using Error = SyncCrypto::OpenedBlob::Error;
        QCOMPARE(SyncCrypto::decryptBlob(secret(kKey), kUser, QByteArrayLiteral("{\"format\":1}")).error, Error::NotABlob);
        QCOMPARE(SyncCrypto::decryptBlob(secret(kKey), kUser, kBlob.left(40)).error, Error::NotABlob);

        QByteArray newer = kBlob;
        newer[4] = char(SyncCrypto::BlobFormat + 1);
        const SyncCrypto::OpenedBlob opened = SyncCrypto::decryptBlob(secret(kKey), kUser, newer);
        QCOMPARE(opened.error, Error::NewerFormat);
        QCOMPARE(opened.format, SyncCrypto::BlobFormat + 1);

        // The format byte is part of the associated data: rewriting it to an older one fails too.
        QByteArray older = kBlob;
        older[4] = char(0);
        QCOMPARE(SyncCrypto::decryptBlob(secret(kKey), kUser, older).error, Error::Unreadable);
    }

    // --- Device keys and boxes ---

    void publicKeyComesFromThePrivateOne()
    {
        QCOMPARE(SyncCrypto::publicKeyFor(secret(kSecretA)), kPublicA);
        QCOMPARE(SyncCrypto::publicKeyFor(secret(kSecretB)), kPublicB);
        QVERIFY(SyncCrypto::publicKeyFor(secret(QByteArray(5, 'x'))).isEmpty());

        const SyncCrypto::KeyPair pair = SyncCrypto::generateKeyPair();
        QCOMPARE(pair.publicKey.size(), SyncCrypto::KeyBytes);
        QCOMPARE(pair.secretKey.size(), SyncCrypto::KeyBytes);
        // What SecretStore keeps (the private key in Base64) gives the same pair back.
        const SecretBytes stored = SecretBytes::fromBase64(pair.secretKey.toBase64());
        QCOMPARE(SyncCrypto::publicKeyFor(stored), pair.publicKey);
    }

    void boxFromTheKnownVectorOpens()
    {
        const std::optional<SecretBytes> opened = SyncCrypto::openBox(kBoxAtoB, kPublicA, secret(kSecretB));
        QVERIFY(opened);
        QVERIFY(*opened == secret(range(0xa0, 32)));
    }

    void boxRoundTripsBetweenTwoDevices()
    {
        const SyncCrypto::KeyPair approver = SyncCrypto::generateKeyPair();
        const SyncCrypto::KeyPair newcomer = SyncCrypto::generateKeyPair();
        const SecretBytes key = SyncCrypto::newDataKey();

        const QByteArray sealed = SyncCrypto::box(key, newcomer.publicKey, approver.secretKey);
        QCOMPARE(sealed.size(), 72); // nonce + key + tag, within the server's 256 bytes
        const std::optional<SecretBytes> opened = SyncCrypto::openBox(sealed, approver.publicKey, newcomer.secretKey);
        QVERIFY(opened);
        QVERIFY(*opened == key);
    }

    void boxFromAnotherSenderIsRefused()
    {
        const SyncCrypto::KeyPair approver = SyncCrypto::generateKeyPair();
        const SyncCrypto::KeyPair newcomer = SyncCrypto::generateKeyPair();
        const SyncCrypto::KeyPair server = SyncCrypto::generateKeyPair(); // a key the server slipped in

        // Made by someone else, claimed to come from the approver.
        const QByteArray forged = SyncCrypto::box(SyncCrypto::newDataKey(), newcomer.publicKey, server.secretKey);
        QVERIFY(!SyncCrypto::openBox(forged, approver.publicKey, newcomer.secretKey));
        // Made by the approver, for another device.
        const QByteArray other = SyncCrypto::box(SyncCrypto::newDataKey(), server.publicKey, approver.secretKey);
        QVERIFY(!SyncCrypto::openBox(other, approver.publicKey, newcomer.secretKey));
        // Damaged.
        QByteArray damaged = SyncCrypto::box(SyncCrypto::newDataKey(), newcomer.publicKey, approver.secretKey);
        damaged[30] = char(damaged.at(30) ^ 1);
        QVERIFY(!SyncCrypto::openBox(damaged, approver.publicKey, newcomer.secretKey));
        QVERIFY(!SyncCrypto::openBox(QByteArray(10, 'x'), approver.publicKey, newcomer.secretKey));
    }

    void verificationCodeIsKnownAndOrderFree()
    {
        QCOMPARE(SyncCrypto::verificationCode(kPublicA, kPublicB), kVerification);
        QCOMPARE(SyncCrypto::verificationCode(kPublicB, kPublicA), kVerification);

        const SyncCrypto::KeyPair other = SyncCrypto::generateKeyPair();
        const QString code = SyncCrypto::verificationCode(other.publicKey, kPublicB);
        // 16 characters of Crockford's Base32 (80 bits) in four groups: a server that relays the
        // keys cannot grind keys of its own until both screens show the same code.
        QCOMPARE(code.size(), 19);
        QVERIFY(QRegularExpression(QStringLiteral("^[0-9A-HJKMNP-TV-Z]{4}(-[0-9A-HJKMNP-TV-Z]{4}){3}$")).match(code).hasMatch());
        QCOMPARE(SyncCrypto::verificationCode(kPublicB, other.publicKey), code);
        QVERIFY(SyncCrypto::verificationCode(kPublicA, QByteArray(31, 'x')).isEmpty());
    }

    // --- Recovery code ---

    void recoveryCodeMatchesTheKnownVector()
    {
        QCOMPARE(SyncCrypto::formatRecoveryCode(secret(kRecoveryBytes)), kRecoveryText);
        const std::optional<SecretBytes> parsed = SyncCrypto::parseRecoveryCode(kRecoveryText);
        QVERIFY(parsed);
        QVERIFY(*parsed == secret(kRecoveryBytes));
    }

    void recoveryCodeIsReadAsTyped()
    {
        // Lower case, no dashes, spaces, O for 0.
        QString typed = kRecoveryText.toLower();
        typed.remove(QLatin1Char('-'));
        typed.replace(QLatin1Char('0'), QLatin1Char('o'));
        typed.insert(8, QStringLiteral("  "));
        const std::optional<SecretBytes> parsed = SyncCrypto::parseRecoveryCode(typed);
        QVERIFY(parsed);
        QVERIFY(*parsed == secret(kRecoveryBytes));

        for (int i = 0; i < 20; ++i) {
            const SecretBytes code = SyncCrypto::newRecoveryCode();
            const QString text = SyncCrypto::formatRecoveryCode(code);
            QCOMPARE(text.size(), 34); // 7 groups of 4 and 6 dashes
            const std::optional<SecretBytes> back = SyncCrypto::parseRecoveryCode(text);
            QVERIFY(back && *back == code);
        }
    }

    void mistypedRecoveryCodesAreRefused()
    {
        QVERIFY(!SyncCrypto::parseRecoveryCode(QString()));
        QVERIFY(!SyncCrypto::parseRecoveryCode(kRecoveryText.left(30)));
        QVERIFY(!SyncCrypto::parseRecoveryCode(kRecoveryText + QStringLiteral("-A")));
        QVERIFY(!SyncCrypto::parseRecoveryCode(kRecoveryText + kRecoveryText)); // twice as long
        QString withU = kRecoveryText;
        withU[0] = QLatin1Char('U'); // not in Crockford's alphabet
        QVERIFY(!SyncCrypto::parseRecoveryCode(withU));
        // One character changed: the check fails.
        QString typo = kRecoveryText;
        typo[1] = typo.at(1) == QLatin1Char('3') ? QLatin1Char('4') : QLatin1Char('3');
        QVERIFY(!SyncCrypto::parseRecoveryCode(typo));
    }

    void dataKeyComesBackWithTheRecoveryCode()
    {
        const std::optional<SecretBytes> known = SyncCrypto::openWithRecovery(kRecoverySealed, secret(kRecoveryBytes));
        QVERIFY(known);
        QVERIFY(*known == secret(kKey));

        const SecretBytes key = SyncCrypto::newDataKey();
        const SecretBytes code = SyncCrypto::newRecoveryCode();
        const QByteArray sealed = SyncCrypto::sealWithRecovery(key, code);
        QVERIFY(sealed.size() >= 32 && sealed.size() <= 512); // what PUT /v1/recovery accepts
        const std::optional<SecretBytes> opened = SyncCrypto::openWithRecovery(sealed, *SyncCrypto::parseRecoveryCode(SyncCrypto::formatRecoveryCode(code)));
        QVERIFY(opened);
        QVERIFY(*opened == key);

        QVERIFY(!SyncCrypto::openWithRecovery(sealed, SyncCrypto::newRecoveryCode()));
        QByteArray damaged = sealed;
        damaged[40] = char(damaged.at(40) ^ 1);
        QVERIFY(!SyncCrypto::openWithRecovery(damaged, code));
    }

    // Known answers computed apart from libsodium's caller here (Python: hashlib.blake2b with
    // person "SugoReadyKey", then Ed25519 from that seed), as docs/api.md of sugo-sync spells them.
    void readyVerifierAndProofHaveKnownAnswers()
    {
        const SecretBytes key = secret(range(0x00, 32));
        QCOMPARE(SyncCrypto::readyVerifier(key).toHex(),
                 QByteArray("f2e219fb75f4dc4ad67e6554f98665b5e746bfbbaeb043d7d4d5e282e547fd86"));
        const QByteArray nonce(32, char(0xaa));
        QCOMPARE(SyncCrypto::readyMessage(nonce, QStringLiteral("dev_1"), QStringLiteral("usr_a")),
                 QByteArray("sugo-ready-v1\0", 14) + nonce + QByteArray("dev_1\0usr_a", 11));
        QCOMPARE(SyncCrypto::signReady(key, nonce, QStringLiteral("dev_1"), QStringLiteral("usr_a")).toHex(),
                 QByteArray("b1a821d20ffc6181531bd253e8983bd48fa40ffe7594d1420fd3df23e6e0ee34"
                            "40823335998578c4f1b0e98fe2fae81ffd1bf211144f523b4a53ad4ddc3e2b0e"));
    }

    void readyProofNamesTheDeviceAndRefusesBadInput()
    {
        const SecretBytes key = SyncCrypto::newDataKey();
        const QByteArray nonce(32, char(1));
        const QByteArray signature = SyncCrypto::signReady(key, nonce, QStringLiteral("dev_1"), QStringLiteral("usr_a"));
        QCOMPARE(signature.size(), SyncCrypto::ReadySignatureBytes);
        QVERIFY(SyncCrypto::signReady(key, nonce, QStringLiteral("dev_2"), QStringLiteral("usr_a")) != signature);
        QVERIFY(SyncCrypto::signReady(key, nonce, QStringLiteral("dev_1"), QStringLiteral("usr_b")) != signature);
        QVERIFY(SyncCrypto::readyVerifier(key) != SyncCrypto::readyVerifier(SyncCrypto::newDataKey()));

        QVERIFY(SyncCrypto::readyVerifier(SecretBytes()).isEmpty());
        QVERIFY(SyncCrypto::readyVerifier(secret(range(0, 16))).isEmpty());
        QVERIFY(SyncCrypto::signReady(key, QByteArray(31, char(1)), QStringLiteral("dev_1"), QStringLiteral("usr_a")).isEmpty());
        QVERIFY(SyncCrypto::signReady(key, nonce, QString(), QStringLiteral("usr_a")).isEmpty());
        QVERIFY(SyncCrypto::signReady(key, nonce, QStringLiteral("dev_1"), QString()).isEmpty());
        // A NUL in an id would let two (device, user) pairs sign the same bytes.
        QVERIFY(SyncCrypto::signReady(key, nonce, QStringLiteral("dev_1") + QChar(0) + QStringLiteral("x"), QStringLiteral("usr_a")).isEmpty());
    }
};

QTEST_GUILESS_MAIN(SyncCryptoTest)
#include "tst_synccrypto.moc"
