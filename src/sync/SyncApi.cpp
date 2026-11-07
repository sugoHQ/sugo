// SPDX-License-Identifier: GPL-3.0-or-later
#include "SyncApi.h"

#include "RetryAfter.h"
#include "SyncSecrets.h"
#include "UrlPolicy.h"

#include <QCoreApplication>
#include <QHash>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <utility>

namespace
{
constexpr int kTransferTimeoutMs = 30000;
/// An access token is renewed when it has less than this left.
constexpr qint64 kAccessMarginMs = 60 * 1000;

QString appVersion()
{
#ifdef PROJECT_VERSION
    return QStringLiteral(PROJECT_VERSION);
#else
    return QCoreApplication::applicationVersion();
#endif
}

QString defaultServer()
{
    return QStringLiteral("https://api.sugo.stream");
}

/// An ETag without W/ and quotes.
QByteArray bareEtag(QByteArray etag)
{
    etag = etag.trimmed();
    if (etag.startsWith("W/")) {
        etag.remove(0, 2);
    }
    if (etag.size() >= 2 && etag.startsWith('"') && etag.endsWith('"')) {
        etag = etag.mid(1, etag.size() - 2);
    }
    return etag;
}

QByteArray jsonBody(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QString encodedId(const QString &id)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(id));
}
} // namespace

// ---------------------------------------------------------------------------------------------
// SyncError
// ---------------------------------------------------------------------------------------------

SyncError::Kind SyncError::kindForCode(const QString &code, int http)
{
    static const QHash<QString, Kind> kinds = {
        {QStringLiteral("unauthorized"), Kind::Unauthorized},
        {QStringLiteral("invalid_grant"), Kind::InvalidGrant},
        {QStringLiteral("authorization_pending"), Kind::AuthorizationPending},
        {QStringLiteral("slow_down"), Kind::SlowDown},
        {QStringLiteral("expired_token"), Kind::ExpiredToken},
        {QStringLiteral("rate_limited"), Kind::RateLimited},
        {QStringLiteral("precondition_failed"), Kind::PreconditionFailed},
        {QStringLiteral("precondition_required"), Kind::PreconditionRequired},
        {QStringLiteral("too_large"), Kind::TooLarge},
        {QStringLiteral("invalid_blob"), Kind::InvalidBlob},
        {QStringLiteral("invalid_request"), Kind::InvalidRequest},
        {QStringLiteral("not_found"), Kind::NotFound},
        {QStringLiteral("not_ready"), Kind::NotReady},
        {QStringLiteral("already_ready"), Kind::AlreadyReady},
        {QStringLiteral("key_exists"), Kind::KeyExists},
        {QStringLiteral("server"), Kind::Server},
    };
    const auto it = kinds.constFind(code);
    if (it != kinds.constEnd()) {
        return *it;
    }
    switch (http) {
    case 400:
        return Kind::InvalidRequest;
    case 401:
        return Kind::Unauthorized;
    case 404:
        return Kind::NotFound;
    case 412:
        return Kind::PreconditionFailed;
    case 413:
        return Kind::TooLarge;
    case 428:
        return Kind::PreconditionRequired;
    case 429:
        return Kind::RateLimited;
    default:
        return Kind::Server;
    }
}

SyncError SyncError::fromResponse(int http, const QByteArray &body, const QString &networkError)
{
    SyncError e;
    e.http = http;
    if (http == 0) {
        e.kind = Kind::Network;
        e.message = networkError;
        return e;
    }
    if ((http >= 200 && http < 300) || http == 304) {
        return e;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isObject()) {
        e.extra = doc.object();
        e.code = e.extra.value(QLatin1String("error")).toString();
        e.message = e.extra.value(QLatin1String("message")).toString();
        e.reason = e.extra.value(QLatin1String("reason")).toString();
        e.interval = e.extra.value(QLatin1String("interval")).toInt();
    }
    e.kind = kindForCode(e.code, http);
    return e;
}

// ---------------------------------------------------------------------------------------------
// SyncApi
// ---------------------------------------------------------------------------------------------

SyncApi::SyncApi(SyncSecrets *secrets, const QUrl &baseUrl, QObject *parent)
    : QObject(parent)
    , m_secrets(secrets)
    , m_baseUrl(baseUrl)
{
    m_now = []() {
        return qint64(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    };
}

SyncApi::~SyncApi()
{
    reset();
}

QUrl SyncApi::defaultBaseUrl()
{
    const QString custom = qEnvironmentVariable("SUGO_SYNC_URL").trimmed();
    return QUrl(custom.isEmpty() ? defaultServer() : custom);
}

bool SyncApi::isDefaultBaseUrl(const QUrl &url)
{
    return url.adjusted(QUrl::StripTrailingSlash) == QUrl(defaultServer());
}

bool SyncApi::baseUrlAllowed() const
{
    return m_baseUrl.isValid() && UrlPolicy::isAllowed(m_baseUrl);
}

void SyncApi::setNowProvider(std::function<qint64()> nowMs)
{
    m_now = std::move(nowMs);
}

qint64 SyncApi::now() const
{
    return m_now();
}

QNetworkAccessManager *SyncApi::network()
{
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    return m_nam;
}

// --- Calls -------------------------------------------------------------------------------------

void SyncApi::startDevice(const QString &name, const QString &platform, const QByteArray &publicKey, Callback done)
{
    QJsonObject body;
    body.insert(QLatin1String("name"), name);
    body.insert(QLatin1String("platform"), platform);
    body.insert(QLatin1String("public_key"), QString::fromLatin1(publicKey.toBase64()));
    Request r;
    r.verb = "POST";
    r.path = QStringLiteral("/v1/auth/device");
    r.body = jsonBody(body);
    r.auth = false;
    call(r, std::move(done));
}

void SyncApi::pollToken(const QString &deviceCode, Callback done)
{
    QJsonObject body;
    body.insert(QLatin1String("grant_type"), QStringLiteral("device_code"));
    body.insert(QLatin1String("device_code"), deviceCode);
    Request r;
    r.verb = "POST";
    r.path = QStringLiteral("/v1/auth/token");
    r.body = jsonBody(body);
    r.auth = false;
    call(r, [this, done](const SyncReply &reply) {
        if (!reply.ok() || takeTokens(reply.json)) {
            done(reply);
            return;
        }
        SyncReply bad = reply;
        bad.error.kind = SyncError::Kind::Format;
        done(bad);
    });
}

void SyncApi::listDevices(Callback done)
{
    Request r;
    r.verb = "GET";
    r.path = QStringLiteral("/v1/devices");
    call(r, std::move(done));
}

void SyncApi::disconnectDevice(const QString &id, Callback done)
{
    Request r;
    r.verb = "DELETE";
    r.path = QStringLiteral("/v1/devices/") + encodedId(id);
    call(r, std::move(done));
}

void SyncApi::putDeviceKey(const QString &id, const QByteArray &sealed, Callback done)
{
    QJsonObject body;
    body.insert(QLatin1String("sealed_key"), QString::fromLatin1(sealed.toBase64()));
    Request r;
    r.verb = "PUT";
    r.path = QStringLiteral("/v1/devices/") + encodedId(id) + QStringLiteral("/key");
    r.body = jsonBody(body);
    call(r, std::move(done));
}

void SyncApi::getKey(Callback done)
{
    Request r;
    r.verb = "GET";
    r.path = QStringLiteral("/v1/key");
    call(r, std::move(done));
}

void SyncApi::confirmKey(Callback done)
{
    Request r;
    r.verb = "DELETE";
    r.path = QStringLiteral("/v1/key");
    call(r, std::move(done));
}

void SyncApi::getRecovery(Callback done)
{
    Request r;
    r.verb = "GET";
    r.path = QStringLiteral("/v1/recovery");
    call(r, std::move(done));
}

void SyncApi::putRecovery(const QByteArray &sealed, Callback done)
{
    QJsonObject body;
    body.insert(QLatin1String("recovery"), QString::fromLatin1(sealed.toBase64()));
    Request r;
    r.verb = "PUT";
    r.path = QStringLiteral("/v1/recovery");
    r.body = jsonBody(body);
    call(r, std::move(done));
}

void SyncApi::getBlob(SyncKind kind, const QByteArray &etag, Callback done)
{
    Request r;
    r.verb = "GET";
    r.path = QStringLiteral("/v1/blob/") + SyncKinds::name(kind);
    r.expectJson = false;
    if (!etag.isEmpty()) {
        r.headers.append({QByteArrayLiteral("If-None-Match"), '"' + etag + '"'});
    }
    call(r, std::move(done));
}

void SyncApi::putBlob(SyncKind kind, const QByteArray &blob, const QByteArray &etag, Callback done)
{
    Request r;
    r.verb = "PUT";
    r.path = QStringLiteral("/v1/blob/") + SyncKinds::name(kind);
    r.body = blob;
    r.contentType = "application/octet-stream";
    r.expectJson = false;
    if (etag.isEmpty()) {
        r.headers.append({QByteArrayLiteral("If-None-Match"), QByteArrayLiteral("*")});
    } else {
        r.headers.append({QByteArrayLiteral("If-Match"), '"' + etag + '"'});
    }
    call(r, std::move(done));
}

// --- Tokens ------------------------------------------------------------------------------------

bool SyncApi::hasAccessToken() const
{
    return !m_accessToken.isEmpty() && m_accessExpiresAt - now() > kAccessMarginMs;
}

void SyncApi::dropAccessToken()
{
    m_accessToken.clear();
    m_accessExpiresAt = 0;
}

void SyncApi::ensureAccessToken(TokenCallback done)
{
    if (hasAccessToken()) {
        done(SyncError());
        return;
    }
    m_refreshWaiters.append(std::move(done));
    if (m_refreshInFlight) {
        return; // never a second POST: wait for the one under way
    }
    const QString refreshToken = m_secrets->refreshToken();
    if (refreshToken.isEmpty()) {
        SyncError e;
        e.kind = SyncError::Kind::Unauthorized;
        finishRefresh(e);
        return;
    }
    m_refreshInFlight = true;
    QJsonObject body;
    body.insert(QLatin1String("grant_type"), QStringLiteral("refresh_token"));
    body.insert(QLatin1String("refresh_token"), refreshToken);
    Request r;
    r.verb = "POST";
    r.path = QStringLiteral("/v1/auth/token");
    r.body = jsonBody(body);
    r.auth = false;
    send(r, QString(), [this](const SyncReply &reply) {
        m_refreshInFlight = false;
        if (reply.ok()) {
            SyncError e;
            if (!takeTokens(reply.json)) {
                e.kind = SyncError::Kind::Format;
                e.http = reply.http;
            }
            finishRefresh(e);
            return;
        }
        if (reply.error.kind == SyncError::Kind::InvalidGrant) {
            dropAccessToken();
            // First: whoever handles it may reset(), and then no waiter runs after the sign-out.
            const quint64 gen = m_generation;
            Q_EMIT signedOutRemotely();
            if (gen != m_generation) {
                return;
            }
        }
        // Network, rate limit, server: the refresh token in memory stays as it is.
        finishRefresh(reply.error);
    });
}

bool SyncApi::takeTokens(const QJsonObject &json)
{
    const QString refreshToken = json.value(QLatin1String("refresh_token")).toString();
    const QString accessToken = json.value(QLatin1String("access_token")).toString();
    if (refreshToken.isEmpty() || accessToken.isEmpty()) {
        return false;
    }
    // Each refresh token works once and the old one is spent now: the new one goes to the
    // keyring before anything else (SyncSecrets keeps it in memory if that fails).
    const quint64 gen = m_generation;
    if (!m_secrets->setRefreshToken(refreshToken)) {
        Q_EMIT tokenNotSaved();
        if (gen != m_generation) {
            return false;
        }
    }
    m_accessToken = accessToken;
    // Within reason whatever the server says: not renewed before every request, not kept for days.
    const qint64 expiresIn = std::clamp<qint64>(json.value(QLatin1String("expires_in")).toInteger(3600), 60, 86400);
    m_accessExpiresAt = now() + expiresIn * 1000;
    return true;
}

void SyncApi::finishRefresh(const SyncError &error)
{
    const QList<TokenCallback> waiters = std::exchange(m_refreshWaiters, {});
    const quint64 gen = m_generation;
    for (const TokenCallback &waiter : waiters) {
        if (gen != m_generation) {
            return;
        }
        waiter(error);
    }
}

// --- Requests ----------------------------------------------------------------------------------

void SyncApi::call(const Request &request, Callback done)
{
    if (request.auth) {
        callAuthorized(request, false, std::move(done));
    } else {
        send(request, QString(), std::move(done));
    }
}

void SyncApi::callAuthorized(const Request &request, bool retried, Callback done)
{
    ensureAccessToken([this, request, retried, done](const SyncError &error) {
        if (!error.ok()) {
            SyncReply reply;
            reply.error = error;
            reply.http = error.http;
            done(reply);
            return;
        }
        const QString token = m_accessToken;
        send(request, token, [this, request, retried, done, token](const SyncReply &reply) {
            if (reply.http == 401 && !retried) {
                // Only the token this request used is stale: another request may have renewed it.
                if (m_accessToken == token) {
                    dropAccessToken();
                }
                callAuthorized(request, true, done);
                return;
            }
            done(reply);
        });
    });
}

void SyncApi::send(const Request &request, const QString &accessToken, Callback done)
{
    const quint64 gen = m_generation;
    if (!baseUrlAllowed()) {
        SyncReply reply;
        reply.error.kind = SyncError::Kind::NotAllowed;
        reply.error.message = QStringLiteral("The Sugo Sync address must use https: %1").arg(m_baseUrl.toString());
        QTimer::singleShot(0, this, [this, gen, done, reply]() {
            if (gen == m_generation) {
                done(reply);
            }
        });
        return;
    }

    QUrl url = m_baseUrl;
    QString basePath = url.path();
    while (basePath.endsWith(QLatin1Char('/'))) {
        basePath.chop(1);
    }
    url.setPath(basePath + request.path, QUrl::TolerantMode);
    url.setQuery(QString());
    url.setFragment(QString());

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Sugo/%1").arg(appVersion()));
    req.setRawHeader("Accept", "application/json");
    if (!accessToken.isEmpty()) {
        req.setRawHeader("Authorization", "Bearer " + accessToken.toUtf8());
    }
    if (!request.body.isEmpty() || request.verb == "PUT" || request.verb == "POST") {
        req.setHeader(QNetworkRequest::ContentTypeHeader, request.contentType.isEmpty() ? QByteArray("application/json") : request.contentType);
    }
    for (const auto &h : request.headers) {
        req.setRawHeader(h.first, h.second);
    }
    // Same-origin redirects only: the token never follows a redirect to another server, and
    // https never falls back to http.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    req.setTransferTimeout(kTransferTimeoutMs);

    QNetworkReply *reply = network()->sendCustomRequest(req, request.verb, request.body);
    m_replies.insert(reply);
    const bool expectJson = request.expectJson;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen, done, expectJson]() {
        m_replies.remove(reply);
        reply->deleteLater();
        if (gen != m_generation) {
            return;
        }
        SyncReply r;
        r.http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r.body = reply->readAll();
        r.etag = bareEtag(reply->rawHeader("ETag"));
        const QByteArray pending = reply->rawHeader("Sugo-Pending-Devices");
        bool pendingOk = false;
        const int pendingCount = pending.trimmed().toInt(&pendingOk);
        r.pendingDevices = pendingOk ? pendingCount : -1;
        // A transfer cut off after the status line (closed, timed out) has a status but not all of its
        // body: it fails as the network does, not as a good answer with half a blob.
        const QNetworkReply::NetworkError transport = reply->error();
        const bool cutOff = transport != QNetworkReply::NoError && transport < QNetworkReply::ContentAccessDenied;
        r.error = cutOff ? SyncError::fromResponse(0, {}, reply->errorString())
                         : SyncError::fromResponse(r.http, r.body, r.http == 0 ? reply->errorString() : QString());
        if (!r.error.ok()) {
            // An HTTP date (or anything unreadable) gives 0: the caller's own delay.
            r.error.retryAfter = RetryAfter::seconds(reply->rawHeader("Retry-After"));
            r.json = r.error.extra;
        } else if (expectJson && !r.body.isEmpty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(r.body);
            if (doc.isObject()) {
                r.json = doc.object();
            } else {
                r.error.kind = SyncError::Kind::Format;
                r.error.http = r.http;
            }
        }
        if (r.pendingDevices >= 0) {
            Q_EMIT pendingDevicesSeen(r.pendingDevices);
            if (gen != m_generation) {
                return;
            }
        }
        done(r);
    });
}

void SyncApi::reset()
{
    ++m_generation;
    dropAccessToken();
    m_refreshInFlight = false;
    m_refreshWaiters.clear();
    const QSet<QNetworkReply *> replies = std::exchange(m_replies, {});
    for (QNetworkReply *reply : replies) {
        reply->abort();
        reply->deleteLater();
    }
}
