// SPDX-License-Identifier: GPL-3.0-or-later
#include "SyncCrypto.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstring>

// --- SecretBytes ---------------------------------------------------------------------------------

SecretBytes::SecretBytes(qsizetype size)
    : m_bytes(size_t(std::max<qsizetype>(0, size)), 0)
{
}

SecretBytes::~SecretBytes()
{
    wipe();
}

SecretBytes::SecretBytes(SecretBytes &&other) noexcept
    : m_bytes(std::move(other.m_bytes))
{
    other.m_bytes.clear();
}

SecretBytes &SecretBytes::operator=(SecretBytes &&other) noexcept
{
    if (this != &other) {
        wipe();
        m_bytes = std::move(other.m_bytes);
        other.m_bytes.clear();
    }
    return *this;
}

void SecretBytes::wipe()
{
    if (!m_bytes.empty()) {
        sodium_memzero(m_bytes.data(), m_bytes.size());
        m_bytes.clear();
    }
}

SecretBytes SecretBytes::fromRaw(const QByteArray &bytes)
{
    SecretBytes secret(bytes.size());
    if (!bytes.isEmpty()) {
        std::memcpy(secret.data(), bytes.constData(), size_t(bytes.size()));
    }
    return secret;
}

SecretBytes SecretBytes::fromBase64(const QByteArray &base64)
{
    auto decoded = QByteArray::fromBase64Encoding(base64, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) {
        return {};
    }
    QByteArray &raw = decoded.decoded;
    SecretBytes secret = fromRaw(raw);
    if (!raw.isEmpty()) {
        sodium_memzero(raw.data(), size_t(raw.size()));
    }
    return secret;
}

SecretBytes SecretBytes::copy() const
{
    SecretBytes other(size());
    if (!isEmpty()) {
        std::memcpy(other.data(), data(), m_bytes.size());
    }
    return other;
}

QByteArray SecretBytes::toBase64() const
{
    return QByteArray::fromRawData(reinterpret_cast<const char *>(data()), size()).toBase64();
}

bool SecretBytes::operator==(const SecretBytes &other) const
{
    return size() == other.size() && (isEmpty() || sodium_memcmp(data(), other.data(), m_bytes.size()) == 0);
}

// --- SyncCrypto ----------------------------------------------------------------------------------

namespace SyncCrypto
{
namespace
{
const QByteArray kMagic = QByteArrayLiteral("SUGO");
constexpr int kRecoveryVersion = 1;
constexpr qsizetype kMacBytes = crypto_aead_xchacha20poly1305_ietf_ABYTES;

static_assert(KeyBytes == crypto_aead_xchacha20poly1305_ietf_KEYBYTES, "K is an XChaCha20-Poly1305 key");
static_assert(NonceBytes == crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, "XChaCha20 nonce");
static_assert(NonceBytes == crypto_box_NONCEBYTES, "crypto_box nonce");
static_assert(KeyBytes == crypto_box_PUBLICKEYBYTES && KeyBytes == crypto_box_SECRETKEYBYTES, "X25519 keys");

// Crockford's Base32: no I, L, O, U.
const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
/// The verification code: 16 characters of kAlphabet, 80 bits of the keys' hash.
constexpr int kVerificationChars = 16;
constexpr int kRecoveryChars = 28; // 128 bits of code + 12 bits of check = 140 bits
constexpr int kCheckBits = 12;

const unsigned char *bytes(const QByteArray &array)
{
    return reinterpret_cast<const unsigned char *>(array.constData());
}

unsigned char *bytes(QByteArray &array)
{
    return reinterpret_cast<unsigned char *>(array.data());
}

/// 16 bytes of "personal" for BLAKE2b, so each use of the hash gives unrelated outputs.
QByteArray personal(const char *label)
{
    QByteArray p(label);
    p.resize(crypto_generichash_blake2b_PERSONALBYTES, '\0');
    return p;
}

/// BLAKE2b of `input` with a purpose label, `outputBytes` long (16 to 64).
void hash(unsigned char *out, size_t outputBytes, const unsigned char *input, size_t inputBytes, const char *label)
{
    static const unsigned char salt[crypto_generichash_blake2b_SALTBYTES] = {};
    crypto_generichash_blake2b_salt_personal(out, outputBytes, input, inputBytes, nullptr, 0, salt,
                                             bytes(personal(label)));
}

QByteArray blobAssociatedData(const QString &userId, const QString &kind, int format)
{
    // Each field ends with a NUL (which neither holds), so no two pairs of user and kind run together
    // into the same bytes ("usr_a" + "bc" and "usr_ab" + "c").
    QByteArray ad = userId.toUtf8();
    ad.append('\0');
    ad.append(kind.toUtf8());
    ad.append('\0');
    ad.append(char(format));
    return ad;
}

QByteArray randomNonce()
{
    QByteArray nonce(NonceBytes, Qt::Uninitialized);
    randombytes_buf(nonce.data(), size_t(nonce.size()));
    return nonce;
}

/// The 12-bit check of a recovery code.
int recoveryCheck(const SecretBytes &code)
{
    unsigned char digest[16];
    hash(digest, sizeof digest, code.data(), size_t(code.size()), "SugoRecoveryChk");
    return (digest[0] << 4) | (digest[1] >> 4);
}

SecretBytes recoveryKey(const SecretBytes &code)
{
    SecretBytes key(KeyBytes);
    hash(key.data(), size_t(key.size()), code.data(), size_t(code.size()), "SugoRecoveryKey");
    return key;
}

QByteArray recoveryAssociatedData()
{
    return QByteArrayLiteral("sugo-recovery") + char(kRecoveryVersion);
}
} // namespace

bool init()
{
    static const bool ready = sodium_init() >= 0;
    return ready;
}

SecretBytes newDataKey()
{
    if (!init()) {
        return {};
    }
    SecretBytes key(KeyBytes);
    crypto_aead_xchacha20poly1305_ietf_keygen(key.data());
    return key;
}

KeyPair generateKeyPair()
{
    KeyPair pair;
    if (!init()) {
        return pair;
    }
    pair.publicKey.resize(KeyBytes);
    pair.secretKey = SecretBytes(KeyBytes);
    crypto_box_keypair(bytes(pair.publicKey), pair.secretKey.data());
    return pair;
}

QByteArray publicKeyFor(const SecretBytes &secretKey)
{
    if (!init() || secretKey.size() != KeyBytes) {
        return {};
    }
    QByteArray publicKey(KeyBytes, Qt::Uninitialized);
    if (crypto_scalarmult_base(bytes(publicKey), secretKey.data()) != 0) {
        return {};
    }
    return publicKey;
}

// --- Blobs ---------------------------------------------------------------------------------------

QByteArray encryptBlob(const SecretBytes &key, const QString &userId, const QByteArray &plaintext, const QString &kind)
{
    if (!init()) {
        return {};
    }
    return encryptBlobWithNonce(key, userId, plaintext, randomNonce(), kind);
}

QByteArray encryptBlobWithNonce(const SecretBytes &key, const QString &userId, const QByteArray &plaintext,
                                const QByteArray &nonce, const QString &kind)
{
    if (!init() || key.size() != KeyBytes || nonce.size() != NonceBytes) {
        return {};
    }
    const QByteArray ad = blobAssociatedData(userId, kind, BlobFormat);
    const QByteArray header = kMagic + char(BlobFormat) + nonce;
    QByteArray blob(header.size() + plaintext.size() + kMacBytes, Qt::Uninitialized);
    std::memcpy(blob.data(), header.constData(), size_t(header.size()));
    unsigned long long written = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(bytes(blob) + header.size(), &written, bytes(plaintext),
                                               size_t(plaintext.size()), bytes(ad), size_t(ad.size()), nullptr,
                                               bytes(nonce), key.data());
    blob.resize(header.size() + qsizetype(written));
    return blob;
}

OpenedBlob decryptBlob(const SecretBytes &key, const QString &userId, const QByteArray &blob, const QString &kind)
{
    OpenedBlob opened;
    const qsizetype headerBytes = kMagic.size() + 1 + NonceBytes;
    if (!init() || blob.size() < headerBytes + kMacBytes || !blob.startsWith(kMagic)) {
        return opened; // NotABlob
    }
    opened.format = static_cast<unsigned char>(blob.at(kMagic.size()));
    if (opened.format > BlobFormat) {
        opened.error = OpenedBlob::Error::NewerFormat;
        return opened;
    }
    opened.error = OpenedBlob::Error::Unreadable;
    if (key.size() != KeyBytes || opened.format < 1) {
        return opened;
    }
    const QByteArray ad = blobAssociatedData(userId, kind, opened.format);
    const unsigned char *nonce = bytes(blob) + kMagic.size() + 1;
    const qsizetype cipherBytes = blob.size() - headerBytes;
    QByteArray plaintext(cipherBytes - kMacBytes, Qt::Uninitialized);
    unsigned long long read = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(bytes(plaintext), &read, nullptr, bytes(blob) + headerBytes,
                                                   size_t(cipherBytes), bytes(ad), size_t(ad.size()), nonce,
                                                   key.data()) != 0) {
        return opened;
    }
    plaintext.resize(qsizetype(read));
    opened.plaintext = plaintext;
    opened.error = OpenedBlob::Error::None;
    return opened;
}

// --- Between devices -----------------------------------------------------------------------------

QByteArray box(const SecretBytes &message, const QByteArray &theirPublicKey, const SecretBytes &mySecretKey)
{
    if (!init() || theirPublicKey.size() != KeyBytes || mySecretKey.size() != KeyBytes || message.isEmpty()) {
        return {};
    }
    const QByteArray nonce = randomNonce();
    QByteArray sealed(NonceBytes + message.size() + qsizetype(crypto_box_MACBYTES), Qt::Uninitialized);
    std::memcpy(sealed.data(), nonce.constData(), size_t(NonceBytes));
    if (crypto_box_easy(bytes(sealed) + NonceBytes, message.data(), size_t(message.size()), bytes(nonce),
                        bytes(theirPublicKey), mySecretKey.data()) != 0) {
        return {};
    }
    return sealed;
}

std::optional<SecretBytes> openBox(const QByteArray &sealed, const QByteArray &fromPublicKey, const SecretBytes &mySecretKey)
{
    const qsizetype overhead = NonceBytes + qsizetype(crypto_box_MACBYTES);
    if (!init() || sealed.size() <= overhead || fromPublicKey.size() != KeyBytes || mySecretKey.size() != KeyBytes) {
        return std::nullopt;
    }
    SecretBytes message(sealed.size() - overhead);
    if (crypto_box_open_easy(message.data(), bytes(sealed) + NonceBytes, size_t(sealed.size() - NonceBytes),
                             bytes(sealed), bytes(fromPublicKey), mySecretKey.data()) != 0) {
        return std::nullopt;
    }
    return message;
}

QString verificationCode(const QByteArray &publicKeyA, const QByteArray &publicKeyB)
{
    if (!init() || publicKeyA.size() != KeyBytes || publicKeyB.size() != KeyBytes) {
        return {};
    }
    // Sorted, so that both devices hash the same bytes whichever key is "theirs".
    const QByteArray input = std::min(publicKeyA, publicKeyB) + std::max(publicKeyA, publicKeyB);
    // 80 bits: a server that relays the keys and swaps in keys of its own must find a pair whose
    // codes collide, about 2^40 key pairs per side within one approval (six digits took ~2000).
    unsigned char digest[16];
    hash(digest, sizeof digest, bytes(input), size_t(input.size()), "SugoVerifyCode");
    QString text;
    for (int c = 0; c < kVerificationChars; ++c) {
        if (c > 0 && c % 4 == 0) {
            text += QLatin1Char('-');
        }
        int value = 0;
        for (int b = c * 5; b < c * 5 + 5; ++b) {
            value = (value << 1) | ((digest[b / 8] >> (7 - b % 8)) & 1);
        }
        text += QLatin1Char(kAlphabet[value]);
    }
    return text;
}

// --- Recovery code -------------------------------------------------------------------------------

SecretBytes newRecoveryCode()
{
    if (!init()) {
        return {};
    }
    SecretBytes code(RecoveryCodeBytes);
    randombytes_buf(code.data(), size_t(code.size()));
    return code;
}

QString formatRecoveryCode(const SecretBytes &code)
{
    if (!init() || code.size() != RecoveryCodeBytes) {
        return {};
    }
    const int check = recoveryCheck(code);
    const auto bit = [&](int index) {
        if (index < RecoveryCodeBytes * 8) {
            return (code.data()[index / 8] >> (7 - index % 8)) & 1;
        }
        return (check >> (kCheckBits - 1 - (index - int(RecoveryCodeBytes) * 8))) & 1;
    };
    QString text;
    for (int c = 0; c < kRecoveryChars; ++c) {
        if (c > 0 && c % 4 == 0) {
            text += QLatin1Char('-');
        }
        int value = 0;
        for (int b = 0; b < 5; ++b) {
            value = (value << 1) | bit(c * 5 + b);
        }
        text += QLatin1Char(kAlphabet[value]);
    }
    return text;
}

std::optional<SecretBytes> parseRecoveryCode(const QString &text)
{
    if (!init()) {
        return std::nullopt;
    }
    // The code's characters as values, wiped on every way out (a mistyped code is most of one).
    // Fixed size: a growing vector would leave copies behind in the memory it gave up.
    struct Values
    {
        std::array<int, kRecoveryChars> at{};
        size_t count = 0;
        ~Values() { sodium_memzero(at.data(), sizeof(at)); }
    } values;
    for (QChar ch : text) {
        if (ch == QLatin1Char('-') || ch.isSpace()) {
            continue;
        }
        char c = char(ch.toUpper().toLatin1());
        if (c == 'O') {
            c = '0';
        } else if (c == 'I' || c == 'L') {
            c = '1';
        }
        const char *found = c ? std::strchr(kAlphabet, c) : nullptr;
        if (!found || values.count == values.at.size()) {
            return std::nullopt;
        }
        values.at[values.count++] = int(found - kAlphabet);
    }
    if (values.count != values.at.size()) {
        return std::nullopt;
    }
    SecretBytes code(RecoveryCodeBytes);
    int check = 0;
    for (int index = 0; index < kRecoveryChars * 5; ++index) {
        const int bit = (values.at[size_t(index / 5)] >> (4 - index % 5)) & 1;
        if (index < RecoveryCodeBytes * 8) {
            code.data()[index / 8] = static_cast<unsigned char>(code.data()[index / 8] | (bit << (7 - index % 8)));
        } else {
            check = (check << 1) | bit;
        }
    }
    if (check != recoveryCheck(code)) {
        return std::nullopt;
    }
    return code;
}

QByteArray sealWithRecovery(const SecretBytes &dataKey, const SecretBytes &recoveryCode)
{
    if (!init() || dataKey.isEmpty() || recoveryCode.size() != RecoveryCodeBytes) {
        return {};
    }
    const SecretBytes key = recoveryKey(recoveryCode);
    const QByteArray nonce = randomNonce();
    const QByteArray ad = recoveryAssociatedData();
    QByteArray sealed(1 + NonceBytes + dataKey.size() + kMacBytes, Qt::Uninitialized);
    sealed[0] = char(kRecoveryVersion);
    std::memcpy(sealed.data() + 1, nonce.constData(), size_t(NonceBytes));
    unsigned long long written = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(bytes(sealed) + 1 + NonceBytes, &written, dataKey.data(),
                                               size_t(dataKey.size()), bytes(ad), size_t(ad.size()), nullptr,
                                               bytes(nonce), key.data());
    return sealed;
}

std::optional<SecretBytes> openWithRecovery(const QByteArray &sealed, const SecretBytes &recoveryCode)
{
    const qsizetype overhead = 1 + NonceBytes + kMacBytes;
    if (!init() || sealed.size() <= overhead || sealed.at(0) != char(kRecoveryVersion)
        || recoveryCode.size() != RecoveryCodeBytes) {
        return std::nullopt;
    }
    const SecretBytes key = recoveryKey(recoveryCode);
    const QByteArray ad = recoveryAssociatedData();
    SecretBytes dataKey(sealed.size() - overhead);
    unsigned long long read = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(dataKey.data(), &read, nullptr, bytes(sealed) + 1 + NonceBytes,
                                                   size_t(sealed.size() - 1 - NonceBytes), bytes(ad), size_t(ad.size()),
                                                   bytes(sealed) + 1, key.data()) != 0) {
        return std::nullopt;
    }
    return dataKey;
}

// --- Proof of holding the key --------------------------------------------------------------------

namespace
{
static_assert(ReadyVerifierBytes == crypto_sign_PUBLICKEYBYTES, "Ed25519 public key");
static_assert(ReadyNonceBytes == 32, "the server's challenge");
static_assert(ReadySignatureBytes == crypto_sign_BYTES, "Ed25519 signature");

/// The Ed25519 key pair derived from K (docs/api.md, "Proof of holding the key"); false if K is
/// not KeyBytes long.
bool readyKeyPair(const SecretBytes &dataKey, unsigned char *publicKey, SecretBytes &secretKey)
{
    if (!init() || dataKey.size() != KeyBytes) {
        return false;
    }
    SecretBytes seed(crypto_sign_SEEDBYTES);
    hash(seed.data(), size_t(seed.size()), dataKey.data(), size_t(dataKey.size()), "SugoReadyKey");
    secretKey = SecretBytes(crypto_sign_SECRETKEYBYTES);
    return crypto_sign_seed_keypair(publicKey, secretKey.data(), seed.data()) == 0;
}
} // namespace

QByteArray readyVerifier(const SecretBytes &dataKey)
{
    QByteArray publicKey(ReadyVerifierBytes, Qt::Uninitialized);
    SecretBytes secretKey;
    if (!readyKeyPair(dataKey, bytes(publicKey), secretKey)) {
        return {};
    }
    return publicKey;
}

QByteArray readyMessage(const QByteArray &nonce, const QString &deviceId, const QString &userId)
{
    QByteArray message = QByteArrayLiteral("sugo-ready-v1");
    message.append('\0');
    message.append(nonce);
    message.append(deviceId.toUtf8());
    message.append('\0');
    message.append(userId.toUtf8());
    return message;
}

QByteArray signReady(const SecretBytes &dataKey, const QByteArray &nonce, const QString &deviceId, const QString &userId)
{
    // An id holding a NUL would let two (device, user) pairs sign the same bytes.
    if (nonce.size() != ReadyNonceBytes || deviceId.isEmpty() || userId.isEmpty() || deviceId.contains(QChar(0))
        || userId.contains(QChar(0))) {
        return {};
    }
    QByteArray publicKey(ReadyVerifierBytes, Qt::Uninitialized);
    SecretBytes secretKey;
    if (!readyKeyPair(dataKey, bytes(publicKey), secretKey)) {
        return {};
    }
    const QByteArray message = readyMessage(nonce, deviceId, userId);
    QByteArray signature(ReadySignatureBytes, Qt::Uninitialized);
    crypto_sign_detached(bytes(signature), nullptr, bytes(message), size_t(message.size()), secretKey.data());
    return signature;
}
} // namespace SyncCrypto
