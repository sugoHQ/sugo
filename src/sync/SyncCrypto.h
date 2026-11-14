// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>

#include <optional>
#include <vector>

/**
 * Bytes that must not outlive their use: data keys, private keys, recovery codes. Wiped with
 * sodium_memzero when destroyed or moved from; never copied implicitly (copy() is explicit).
 */
class SecretBytes
{
public:
    SecretBytes() = default;
    explicit SecretBytes(qsizetype size);
    ~SecretBytes();

    SecretBytes(SecretBytes &&other) noexcept;
    SecretBytes &operator=(SecretBytes &&other) noexcept;
    SecretBytes(const SecretBytes &) = delete;
    SecretBytes &operator=(const SecretBytes &) = delete;

    static SecretBytes fromRaw(const QByteArray &bytes);
    /// Standard Base64 with padding (how SecretStore keeps it); empty if not valid Base64.
    static SecretBytes fromBase64(const QByteArray &base64);

    SecretBytes copy() const;
    /// For SecretStore: the returned text is an ordinary QByteArray, not wiped.
    QByteArray toBase64() const;

    unsigned char *data() { return m_bytes.data(); }
    const unsigned char *data() const { return m_bytes.data(); }
    qsizetype size() const { return qsizetype(m_bytes.size()); }
    bool isEmpty() const { return m_bytes.empty(); }

    /// In constant time.
    bool operator==(const SecretBytes &other) const;
    bool operator!=(const SecretBytes &other) const { return !(*this == other); }

private:
    void wipe();

    std::vector<unsigned char> m_bytes;
};

/**
 * Sugo Sync's cryptography, on libsodium, the same on every system (docs/sync-plan.md).
 *
 * - The data key K (32 bytes) encrypts the blobs with XChaCha20-Poly1305. A blob is
 *   "SUGO" | format (1 byte) | nonce (24 bytes) | ciphertext and tag, and its associated data is
 *   the UTF-8 user id, a NUL, the blob's kind ("presets"), a NUL, then the format byte: a blob
 *   cannot be passed off as another user's or another kind's.
 * - Each device has an X25519 key pair. K travels between devices in an authenticated crypto_box
 *   (sender's private key, receiver's public key), never in an anonymous crypto_box_seal: the
 *   receiver checks who sent it, and both show verificationCode() of the two public keys.
 * - The recovery code holds 128 random bits; the server keeps K encrypted with a key derived
 *   from it (BLAKE2b: the code has full entropy, no password hashing needed).
 *
 * Functions return an empty result (or nullopt) when the input is not valid or does not
 * authenticate; they never throw.
 */
namespace SyncCrypto
{
constexpr int BlobFormat = 1;
constexpr qsizetype KeyBytes = 32;       ///< K, and a device's private and public keys
constexpr qsizetype NonceBytes = 24;
constexpr qsizetype RecoveryCodeBytes = 16;

/// Initialises libsodium (once; every function below calls it). False if it cannot be used.
bool init();

/// A new random data key K.
SecretBytes newDataKey();

/// A device's X25519 key pair. Only the private key needs storing (`sync-device-key`, as
/// SecretBytes::toBase64()): publicKeyFor() gives the public one back.
struct KeyPair {
    QByteArray publicKey;  ///< 32 bytes; sent Base64-encoded to the server
    SecretBytes secretKey; ///< 32 bytes
};
KeyPair generateKeyPair();
/// The public key of a private key; empty if `secretKey` is not 32 bytes.
QByteArray publicKeyFor(const SecretBytes &secretKey);

// --- Blobs -------------------------------------------------------------------------------------

/// Encrypts `plaintext` as a blob of `kind` for `userId`, with a random nonce.
QByteArray encryptBlob(const SecretBytes &key, const QString &userId, const QByteArray &plaintext,
                       const QString &kind = QStringLiteral("presets"));
/// The same with a given nonce (NonceBytes long), for known-answer tests only: a nonce must never
/// be used twice with the same key.
QByteArray encryptBlobWithNonce(const SecretBytes &key, const QString &userId, const QByteArray &plaintext,
                                const QByteArray &nonce, const QString &kind = QStringLiteral("presets"));

struct OpenedBlob {
    enum class Error {
        None,
        NotABlob,      ///< No "SUGO" header, or too short.
        NewerFormat,   ///< Written by a newer Sugo: "Update Sugo to keep syncing".
        Unreadable,    ///< Wrong key, user, kind, or damaged.
    };
    Error error = Error::NotABlob;
    int format = 0;
    QByteArray plaintext;

    bool ok() const { return error == Error::None; }
};
OpenedBlob decryptBlob(const SecretBytes &key, const QString &userId, const QByteArray &blob,
                       const QString &kind = QStringLiteral("presets"));

// --- Between devices ---------------------------------------------------------------------------

/// Boxes `message` (K) for `theirPublicKey`, signed by `mySecretKey`: nonce | crypto_box_easy.
/// 72 bytes for a 32-byte key (the server takes up to 256).
QByteArray box(const SecretBytes &message, const QByteArray &theirPublicKey, const SecretBytes &mySecretKey);
/// Opens a box only if `fromPublicKey`'s owner made it for `mySecretKey`'s.
std::optional<SecretBytes> openBox(const QByteArray &sealed, const QByteArray &fromPublicKey,
                                   const SecretBytes &mySecretKey);

/// 80 bits of the two public keys' hash as 16 characters of Crockford's Base32 in four groups
/// ("7KQ2-M4DX-…"), the same whichever key comes first; empty if a key is not 32 bytes. Both
/// devices show it before K is sent: too long for a server relaying the keys to grind keys of
/// its own until the two codes agree.
QString verificationCode(const QByteArray &publicKeyA, const QByteArray &publicKeyB);

// --- Recovery code -----------------------------------------------------------------------------

/// 128 new random bits.
SecretBytes newRecoveryCode();
/// The code as the user writes it: 28 Crockford Base32 characters (the 128 bits and a 12-bit
/// check) in groups of four, "7KQ2-M4DX-…". Empty if `code` is not RecoveryCodeBytes long.
QString formatRecoveryCode(const SecretBytes &code);
/// Reads what the user typed: any case, with or without dashes and spaces, O read as 0 and I/L
/// as 1. nullopt for a wrong length, a character outside the alphabet, or a failed check.
std::optional<SecretBytes> parseRecoveryCode(const QString &text);

/// K encrypted with a key derived from the recovery code: version | nonce | ciphertext and tag.
QByteArray sealWithRecovery(const SecretBytes &dataKey, const SecretBytes &recoveryCode);
std::optional<SecretBytes> openWithRecovery(const QByteArray &sealed, const SecretBytes &recoveryCode);

// --- Proof of holding the key ------------------------------------------------------------------
//
// An Ed25519 key pair derived from K (BLAKE2b with "SugoReadyKey", then crypto_sign_seed_keypair):
// the server keeps only its public key, the account's ready verifier, and makes a pending device
// ready only when it signs the server's nonce with the private one (docs/api.md of sugo-sync,
// "Proof of holding the key"). A device linked by someone who does not hold K stays pending.

constexpr qsizetype ReadyVerifierBytes = 32;
constexpr qsizetype ReadyNonceBytes = 32;
constexpr qsizetype ReadySignatureBytes = 64;

/// The account's ready verifier for `dataKey`; empty if K is not KeyBytes long.
QByteArray readyVerifier(const SecretBytes &dataKey);
/// What signReady() signs: "sugo-ready-v1" NUL | nonce | device id NUL | user id (UTF-8).
QByteArray readyMessage(const QByteArray &nonce, const QString &deviceId, const QString &userId);
/// The detached signature of readyMessage() with the key derived from `dataKey`; empty if K is not
/// KeyBytes long, the nonce not ReadyNonceBytes long, or an id empty or holding a NUL.
QByteArray signReady(const SecretBytes &dataKey, const QByteArray &nonce, const QString &deviceId,
                     const QString &userId);
} // namespace SyncCrypto
