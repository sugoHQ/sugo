// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUrl>

#include <functional>

#include "SyncKind.h"

class QNetworkAccessManager;
class QNetworkReply;
class SyncSecrets;

/// What went wrong with a Sugo Sync request (the server's `error` codes, docs/api.md).
struct SyncError {
    enum class Kind {
        None,
        Network,              ///< No answer (offline, timeout, closed connection).
        NotAllowed,           ///< The server address is neither https nor http to this computer.
        Unauthorized,         ///< 401 even after renewing the access token.
        InvalidGrant,         ///< Device code or refresh token unknown, spent or reused: sign in again.
        AuthorizationPending, ///< Sign-in not approved yet; `reason` may be "device_limit_reached".
        SlowDown,             ///< Polling too fast; `interval` holds the new one.
        ExpiredToken,         ///< The sign-in code expired.
        RateLimited,          ///< 429; `retryAfter` seconds.
        PreconditionFailed,   ///< 412: the blob changed; `extra["etag"]`.
        PreconditionRequired, ///< 428: a blob PUT without a precondition (a bug).
        TooLarge,             ///< 413; `extra["max_bytes"]` holds the kind's limit.
        PlusRequired,         ///< 403: a blob kind that syncs only with Plus, on an account without it.
        InvalidBlob,
        InvalidRequest,
        NotFound,             ///< 404: no such device, key, recovery copy or blob.
        NotReady,             ///< This device has no data key yet.
        AlreadyReady,         ///< The device to approve already has the key.
        KeyExists,            ///< The account already has a data key.
        Server,               ///< 5xx, or an error this version does not know.
        Format,               ///< An answer that cannot be read.
    };

    Kind kind = Kind::None;
    int http = 0;
    QString code;          ///< The server's `error` text, if any.
    QString message;       ///< The server's English sentence, or the network error.
    QString reason;        ///< authorization_pending: "device_limit_reached" when it applies.
    int interval = 0;      ///< slow_down: the new polling interval, in seconds.
    int retryAfter = 0;    ///< Retry-After, in seconds, at most an hour (0: none given or unreadable).
    QJsonObject extra;     ///< The whole error object.

    bool ok() const { return kind == Kind::None; }
    /// Worth trying again later as it is (network, rate limit, server).
    bool transient() const { return kind == Kind::Network || kind == Kind::RateLimited || kind == Kind::Server; }

    /// The error of an HTTP answer (None for 2xx and 304). `networkError` is set when there was
    /// no answer at all (`http` 0).
    static SyncError fromResponse(int http, const QByteArray &body, const QString &networkError = QString());
    static Kind kindForCode(const QString &code, int http);
};

Q_DECLARE_METATYPE(SyncError)

/// An answer of the server.
struct SyncReply {
    SyncError error;
    int http = 0;
    QJsonObject json;       ///< The JSON body, if it was an object.
    QByteArray body;        ///< The raw body (the blob for GET /v1/blob/{kind}).
    QByteArray etag;        ///< ETag without quotes; empty if none.
    int pendingDevices = -1; ///< Sugo-Pending-Devices, or -1 when absent.

    bool ok() const { return error.ok(); }
};

/**
 * The Sugo Sync API over HTTPS (docs/api.md of the sugo-sync repository).
 *
 * - The server is SUGO_SYNC_URL when set, else https://api.sugo.stream; only https, or http to
 *   this computer, is ever contacted. TLS errors are never ignored; 30 s transfer timeout.
 * - The access token lives in memory only. The refresh token comes from SyncSecrets' copy in
 *   memory, never from the keyring at each request.
 * - The access token is renewed in ONE place, ensureAccessToken(): never two renewals at once,
 *   and the new refresh token is written to the keyring before anything else happens (each
 *   refresh token works once). An authenticated request that gets 401 renews once and tries
 *   again once. A renewal that gets no answer (or a server error) is sent again after 2 s and
 *   5 s, with a 15 s timeout: the server takes back a refresh token whose answer was lost only
 *   within a minute, and afterwards treats it as stolen and signs the device out.
 * - reset() (sign out, cancel) drops every pending answer: their callbacks are never called.
 */
class SyncApi : public QObject
{
    Q_OBJECT

public:
    using Callback = std::function<void(const SyncReply &)>;
    using TokenCallback = std::function<void(const SyncError &)>;

    /// `secrets` is not owned and must outlive this object.
    SyncApi(SyncSecrets *secrets, const QUrl &baseUrl, QObject *parent = nullptr);
    ~SyncApi() override;

    /// $SUGO_SYNC_URL if set (even if not allowed: requests then fail with NotAllowed), else
    /// https://api.sugo.stream.
    static QUrl defaultBaseUrl();
    /// Whether the default server is in use (for diagnostics: "default" or "custom").
    static bool isDefaultBaseUrl(const QUrl &url);
    QUrl baseUrl() const { return m_baseUrl; }
    bool baseUrlAllowed() const;

    // --- Without a token -------------------------------------------------------------------------
    /// POST /v1/auth/device. `publicKey` raw (32 bytes).
    void startDevice(const QString &name, const QString &platform, const QByteArray &publicKey, Callback done);
    /// POST /v1/auth/token with the device code. On 200 the tokens are taken as by a renewal
    /// (refresh token written first, tokenNotSaved() if that fails) before `done` runs;
    /// `json` still holds `user_id` and `device_id`.
    void pollToken(const QString &deviceCode, Callback done);

    // --- With the access token ---------------------------------------------------------------------
    void listDevices(Callback done);
    void disconnectDevice(const QString &id, Callback done);
    /// PUT /v1/devices/{id}/key; `sealed` raw.
    void putDeviceKey(const QString &id, const QByteArray &sealed, Callback done);
    void getKey(Callback done);
    /// DELETE /v1/key.
    void confirmKey(Callback done);
    void getRecovery(Callback done);
    /// PUT /v1/recovery; `sealed` raw.
    void putRecovery(const QByteArray &sealed, Callback done);
    /// GET /v1/blob/{kind}, with If-None-Match when `etag` is not empty (304 is not an error).
    void getBlob(SyncKind kind, const QByteArray &etag, Callback done);
    /// PUT /v1/blob/{kind}: If-Match `etag`, or If-None-Match: * when it is empty.
    void putBlob(SyncKind kind, const QByteArray &blob, const QByteArray &etag, Callback done);

    /**
     * Calls `done` with no error once there is an access token valid for over a minute,
     * renewing it if needed. While a renewal runs, later callers wait for it (no second POST).
     * Errors: Unauthorized (no refresh token), InvalidGrant (signedOutRemotely() is emitted too),
     * Network/RateLimited/Server (the refresh token is kept; try again later). Network and Server
     * come only after the quick retries of the renewal.
     */
    void ensureAccessToken(TokenCallback done);
    bool hasAccessToken() const;
    /// Forgets the access token (the next request renews it).
    void dropAccessToken();

    /// Aborts every request, drops the access token and every pending callback.
    void reset();

    /// The monotonic clock for the access token's expiry, in ms (tests set their own).
    void setNowProvider(std::function<qint64()> nowMs);
    /// The waits before the retries of a renewal without an answer, in ms (tests set shorter ones).
    void setRefreshRetryDelaysMs(const QList<int> &delays);

Q_SIGNALS:
    /// The refresh token was refused (invalid_grant): the device was disconnected or its token reused.
    void signedOutRemotely();
    /// A new refresh token could not be written to the keyring; it stays in memory and is
    /// written with the next change.
    void tokenNotSaved();
    /// Sugo-Pending-Devices of an answer (devices waiting for the key, excluding this one).
    void pendingDevicesSeen(int count);

private:
    struct Request {
        QByteArray verb;
        QString path;
        QByteArray body;
        QByteArray contentType;
        QList<QPair<QByteArray, QByteArray>> headers;
        bool auth = true;
        bool expectJson = true;
        int timeoutMs = 0; ///< 0: the usual transfer timeout.
    };

    void call(const Request &request, Callback done);
    void callAuthorized(const Request &request, bool retried, Callback done);
    void send(const Request &request, const QString &accessToken, Callback done);
    /// Takes the tokens of a 200 of /v1/auth/token; false if the answer lacks them.
    bool takeTokens(const QJsonObject &json);
    /// POSTs the refresh token; `attempt` 0 is the first try, sent at `firstSentAt` (now()).
    void sendRefresh(const QString &refreshToken, int attempt, qint64 firstSentAt);
    void finishRefresh(const SyncError &error);
    qint64 now() const;
    QNetworkAccessManager *network();

    SyncSecrets *m_secrets;
    QUrl m_baseUrl;
    QNetworkAccessManager *m_nam = nullptr;
    QSet<QNetworkReply *> m_replies;
    quint64 m_generation = 0;

    QString m_accessToken;
    qint64 m_accessExpiresAt = 0;
    bool m_refreshInFlight = false;
    QList<TokenCallback> m_refreshWaiters;
    QList<int> m_refreshRetryDelaysMs{2000, 5000};
    std::function<qint64()> m_now;
};
