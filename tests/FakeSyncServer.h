// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimeZone>

#include <algorithm>
#include <functional>
#include <limits>

#include "FakeSecretStore.h" // nextFakeEventSequence()

/**
 * The Sugo Sync API (docs/api.md of sugo-sync) in memory, over a local QTcpServer: sign-in
 * codes, rotating refresh tokens (a reused one blocks the device), devices, boxed keys, the
 * recovery copy and one blob per kind with ETags, the kinds' size limits and Plus. The site's part
 * (approving a code) is approve().
 *
 * Every request is logged with nextFakeEventSequence(), so a test can order it against the
 * keyring accesses of FakeSecretStore. Failures can be programmed per path.
 */
class FakeSyncServer : public QObject
{
public:
    struct Request {
        QByteArray method;
        QByteArray path;
        QByteArray body;
        QHash<QByteArray, QByteArray> headers;
        qint64 sequence = 0;

        QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
    };

    struct Device {
        QString id;
        QString userId;
        QString name;
        QString platform;
        QByteArray publicKey;
        QString createdAt;
        bool collected = false; ///< Sugo took its tokens (last_seen set).
        bool ready = false;     ///< Holds the data key.
    };

    struct Box {
        QByteArray sealed;
        QString fromDeviceId;
        QByteArray fromPublicKey;
    };

    struct Code {
        QString deviceCode;
        QString userCode;
        QString name;
        QString platform;
        QByteArray publicKey;
        QString deviceId;        ///< Set once approved.
        bool limitReached = false;
        bool expired = false;
    };

    struct Blob {
        QByteArray blob;
        QByteArray etag;
        /// Written before a reset (an earlier sync epoch): it counts as no blob, GET answers 404
        /// and If-Match 412 with `etag: null`, until If-None-Match: * replaces it.
        bool stale = false;
    };

    struct Account {
        QByteArray recovery;
        QString recoveryUpdatedAt;
        QString recoveryUpdatedBy;
        QHash<QByteArray, Blob> blobs; ///< By kind ("presets", ...).
        bool plus = false;             ///< The Plus kinds answer 403 plus_required without it.
    };

    /// A failure answered instead of the real thing, once, to the next request whose path
    /// starts with `pathPrefix` (and method, if set). Status 0 closes the connection.
    struct Failure {
        QByteArray pathPrefix;
        int status = 500;
        QByteArray method;
    };

    FakeSyncServer()
    {
        const bool ok = m_server.listen(QHostAddress::LocalHost, 0);
        Q_ASSERT(ok);
        Q_UNUSED(ok);
        connect(&m_server, &QTcpServer::newConnection, this, &FakeSyncServer::onConnection);
    }

    ~FakeSyncServer() override
    {
        for (QTcpSocket *s : m_buffers.keys()) {
            QObject::disconnect(s, nullptr, this, nullptr);
        }
    }

    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

    // --- Settings ---------------------------------------------------------------------------------
    int deviceLimit = 2;
    int interval = 5;          ///< Polling interval of the sign-in, in seconds.
    /// A limit below the kinds' own (blobKinds) for every upload: a larger one is refused with 413.
    int maxBlobSize = std::numeric_limits<int>::max();
    int accessExpiresIn = 3600;
    int slowDownNext = 0;      ///< The next n polls of a pending code answer slow_down.
    QByteArray retryAfter = "60"; ///< The Retry-After header of a programmed 429.
    /// Every request is answered by closing the connection, like a server out of reach.
    bool offline = false;
    /// The blob kinds and their limits; a kind missing here is unknown, 404 to GET and PUT, as
    /// on a Worker older than the kind.
    QHash<QByteArray, int> blobKinds = {
        {"presets", 262144}, {"sessions", 1048576}, {"tasks", 1048576}, {"statistics", 1048576}, {"settings", 8192},
    };
    QList<Failure> failures;
    void failNext(const QByteArray &pathPrefix, int status, const QByteArray &method = QByteArray())
    {
        failures.append(Failure{pathPrefix, status, method});
    }
    std::function<void(const Request &)> onRequest; ///< Called before each request is answered.
    /// Changes GET /v1/key's answer before it goes out (a compromised server).
    std::function<void(QJsonObject &)> keyFilter;

    // --- State --------------------------------------------------------------------------------------
    QList<Request> requests;
    QList<Device> devices;               ///< In creation order.
    QHash<QString, Code> codes;          ///< By device code.
    QHash<QString, QString> refreshTokens;     ///< Valid token → device id.
    QHash<QString, QString> spentRefreshTokens; ///< Used token → device id.
    QHash<QString, QString> accessTokens;      ///< Token → device id.
    QHash<QString, Box> boxes;           ///< By receiving device id.
    QHash<QString, Account> accounts;    ///< By user id.

    // --- Helpers ------------------------------------------------------------------------------------
    int count(const QByteArray &method, const QByteArray &path) const
    {
        int n = 0;
        for (const Request &r : requests) {
            if (r.method == method && r.path == path) {
                ++n;
            }
        }
        return n;
    }
    QList<Request> of(const QByteArray &method, const QByteArray &path) const
    {
        QList<Request> out;
        for (const Request &r : requests) {
            if (r.method == method && r.path == path) {
                out.append(r);
            }
        }
        return out;
    }

    Device *device(const QString &id)
    {
        for (Device &d : devices) {
            if (d.id == id) {
                return &d;
            }
        }
        return nullptr;
    }

    QString userCodeFor(const QString &deviceCode) const { return codes.value(deviceCode).userCode; }
    /// The device code of a user code ("" if none).
    QString deviceCodeFor(const QString &userCode) const
    {
        for (const Code &c : codes) {
            if (c.userCode == userCode) {
                return c.deviceCode;
            }
        }
        return {};
    }

    /// The site approves `userCode` for `userId`: the device id, or the error code
    /// ("device_limit_reached", "invalid_code", "expired_code", "already_approved").
    QString approve(const QString &userCode, const QString &userId = QStringLiteral("usr_a"))
    {
        const QString deviceCode = deviceCodeFor(userCode);
        if (deviceCode.isEmpty()) {
            return QStringLiteral("invalid_code");
        }
        Code &code = codes[deviceCode];
        if (code.expired) {
            return QStringLiteral("expired_code");
        }
        if (!code.deviceId.isEmpty()) {
            return QStringLiteral("already_approved");
        }
        if (devicesOf(userId).size() >= deviceLimit) {
            code.limitReached = true;
            return QStringLiteral("device_limit_reached");
        }
        code.limitReached = false;
        Device d;
        d.id = QStringLiteral("dev_%1").arg(++m_counter);
        d.userId = userId;
        d.name = code.name;
        d.platform = code.platform;
        d.publicKey = code.publicKey;
        d.createdAt = stamp();
        devices.append(d);
        code.deviceId = d.id;
        return d.id;
    }

    /// A device already linked and collected, with a refresh token: the token.
    QString addLinkedDevice(const QString &userId, const QByteArray &publicKey, bool ready, QString *deviceId = nullptr,
                            const QString &name = QStringLiteral("other"))
    {
        Device d;
        d.id = QStringLiteral("dev_%1").arg(++m_counter);
        d.userId = userId;
        d.name = name;
        d.platform = QStringLiteral("linux");
        d.publicKey = publicKey;
        d.createdAt = stamp();
        d.collected = true;
        d.ready = ready;
        devices.append(d);
        if (deviceId) {
            *deviceId = d.id;
        }
        const QString token = QStringLiteral("rt_%1").arg(++m_counter);
        refreshTokens.insert(token, d.id);
        return token;
    }

    /// Every access token stops working (as after their hour).
    void expireAccessTokens() { accessTokens.clear(); }

    QList<Device> devicesOf(const QString &userId) const
    {
        QList<Device> out;
        for (const Device &d : devices) {
            if (d.userId == userId) {
                out.append(d);
            }
        }
        return out;
    }

private:
    QString stamp()
    {
        // Distinct and increasing, like the server's millisecond times.
        return QDateTime::fromMSecsSinceEpoch(1793793600000LL + 1000 * ++m_clockTicks, QTimeZone::UTC).toString(Qt::ISODateWithMs);
    }

    void onConnection()
    {
        while (QTcpSocket *s = m_server.nextPendingConnection()) {
            m_buffers[s];
            connect(s, &QTcpSocket::readyRead, this, [this, s]() { process(s); });
            connect(s, &QTcpSocket::disconnected, this, [this, s]() {
                m_buffers.remove(s);
                s->deleteLater();
            });
        }
    }

    void process(QTcpSocket *s)
    {
        QByteArray &buf = m_buffers[s];
        buf += s->readAll();
        while (true) {
            const int idx = buf.indexOf("\r\n\r\n");
            if (idx < 0) {
                return;
            }
            const QList<QByteArray> lines = buf.left(idx).split('\n');
            Request req;
            const QList<QByteArray> first = lines.first().trimmed().split(' ');
            req.method = first.value(0);
            req.path = first.value(1);
            for (int i = 1; i < lines.size(); ++i) {
                const int colon = lines.at(i).indexOf(':');
                if (colon > 0) {
                    req.headers.insert(lines.at(i).left(colon).trimmed().toLower(), lines.at(i).mid(colon + 1).trimmed());
                }
            }
            const int len = req.headers.value("content-length").toInt();
            if (buf.size() < idx + 4 + len) {
                return;
            }
            req.body = buf.mid(idx + 4, len);
            buf.remove(0, idx + 4 + len);
            req.sequence = nextFakeEventSequence();
            requests.append(req);
            if (onRequest) {
                onRequest(req);
            }
            if (offline) {
                buf.clear();
                s->abort();
                return;
            }
            const int failure = failureFor(req);
            if (failure == 0) {
                buf.clear();
                s->abort();
                return;
            }
            if (failure > 0) {
                fail(s, failure);
            } else {
                respond(s, req);
            }
        }
    }

    /// The status of the programmed failure for `req` (0: close the connection), or -1.
    int failureFor(const Request &req)
    {
        for (int i = 0; i < failures.size(); ++i) {
            const Failure f = failures.at(i);
            if (req.path.startsWith(f.pathPrefix) && (f.method.isEmpty() || f.method == req.method)) {
                failures.removeAt(i);
                return f.status;
            }
        }
        return -1;
    }

    void reply(QTcpSocket *s, int status, const QByteArray &body = QByteArray(), const QList<QPair<QByteArray, QByteArray>> &headers = {},
               const QByteArray &type = "application/json")
    {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + " X\r\n";
        if (!body.isEmpty()) {
            out += "Content-Type: " + type + "\r\n";
        }
        for (const auto &h : headers) {
            out += h.first + ": " + h.second + "\r\n";
        }
        out += "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: keep-alive\r\n\r\n" + body;
        s->write(out);
        s->flush();
    }
    void replyJson(QTcpSocket *s, int status, const QJsonObject &json, const QList<QPair<QByteArray, QByteArray>> &headers = {})
    {
        reply(s, status, QJsonDocument(json).toJson(QJsonDocument::Compact), headers);
    }
    void error(QTcpSocket *s, int status, const char *code, QJsonObject extra = {}, const QList<QPair<QByteArray, QByteArray>> &headers = {})
    {
        extra.insert(QStringLiteral("error"), QString::fromLatin1(code));
        extra.insert(QStringLiteral("message"), QStringLiteral("Fake: %1").arg(QString::fromLatin1(code)));
        replyJson(s, status, extra, headers);
    }
    void fail(QTcpSocket *s, int status)
    {
        if (status == 429) {
            error(s, 429, "rate_limited", {}, {{"Retry-After", retryAfter}});
        } else if (status == 401) {
            error(s, 401, "unauthorized");
        } else {
            error(s, status, "server");
        }
    }

    QJsonObject issueTokens(const QString &deviceId)
    {
        const QString access = QStringLiteral("at_%1").arg(++m_counter);
        const QString refresh = QStringLiteral("rt_%1").arg(++m_counter);
        accessTokens.insert(access, deviceId);
        refreshTokens.insert(refresh, deviceId);
        Device *d = device(deviceId);
        QJsonObject json;
        json.insert(QStringLiteral("access_token"), access);
        json.insert(QStringLiteral("token_type"), QStringLiteral("Bearer"));
        json.insert(QStringLiteral("expires_in"), accessExpiresIn);
        json.insert(QStringLiteral("refresh_token"), refresh);
        json.insert(QStringLiteral("refresh_expires_in"), 15552000);
        json.insert(QStringLiteral("user_id"), d ? d->userId : QString());
        json.insert(QStringLiteral("device_id"), deviceId);
        return json;
    }

    void dropTokensOf(const QString &deviceId)
    {
        for (auto it = refreshTokens.begin(); it != refreshTokens.end();) {
            it = it.value() == deviceId ? refreshTokens.erase(it) : std::next(it);
        }
        for (auto it = accessTokens.begin(); it != accessTokens.end();) {
            it = it.value() == deviceId ? accessTokens.erase(it) : std::next(it);
        }
    }

    QJsonObject deviceJson(const Device &d, const QString &caller) const
    {
        QJsonObject o;
        o.insert(QStringLiteral("id"), d.id);
        o.insert(QStringLiteral("name"), d.name);
        o.insert(QStringLiteral("platform"), d.platform);
        o.insert(QStringLiteral("created_at"), d.createdAt);
        o.insert(QStringLiteral("last_seen"), d.collected ? QJsonValue(d.createdAt) : QJsonValue(QJsonValue::Null));
        o.insert(QStringLiteral("pending"), !d.ready);
        o.insert(QStringLiteral("current"), d.id == caller);
        o.insert(QStringLiteral("public_key"), QString::fromLatin1(d.publicKey.toBase64()));
        return o;
    }

    void respond(QTcpSocket *s, const Request &req)
    {
        const QByteArray path = req.path;
        const QJsonObject body = req.json();

        if (req.method == "POST" && path == "/v1/auth/device") {
            Code c;
            c.deviceCode = QStringLiteral("dc_%1").arg(++m_counter);
            c.userCode = QStringLiteral("K7Q-%1").arg(m_counter, 3, 10, QLatin1Char('0'));
            c.name = body.value(QStringLiteral("name")).toString();
            c.platform = body.value(QStringLiteral("platform")).toString();
            c.publicKey = QByteArray::fromBase64(body.value(QStringLiteral("public_key")).toString().toLatin1());
            if (c.name.isEmpty() || c.publicKey.size() != 32) {
                error(s, 400, "invalid_request");
                return;
            }
            codes.insert(c.deviceCode, c);
            QJsonObject json;
            json.insert(QStringLiteral("device_code"), c.deviceCode);
            json.insert(QStringLiteral("user_code"), c.userCode);
            json.insert(QStringLiteral("verification_uri"), QStringLiteral("https://sugo.stream/link"));
            json.insert(QStringLiteral("verification_uri_complete"), QString(QStringLiteral("https://sugo.stream/link?code=") + c.userCode));
            json.insert(QStringLiteral("interval"), interval);
            json.insert(QStringLiteral("expires_in"), 600);
            replyJson(s, 200, json);
            return;
        }

        if (req.method == "POST" && path == "/v1/auth/token") {
            const QString grant = body.value(QStringLiteral("grant_type")).toString();
            if (grant == QLatin1String("device_code")) {
                const QString dc = body.value(QStringLiteral("device_code")).toString();
                if (!codes.contains(dc)) {
                    error(s, 400, "invalid_grant");
                    return;
                }
                Code &c = codes[dc];
                if (c.expired) {
                    codes.remove(dc);
                    error(s, 400, "expired_token");
                    return;
                }
                if (c.deviceId.isEmpty()) {
                    if (slowDownNext > 0) {
                        --slowDownNext;
                        interval += 5;
                        QJsonObject extra;
                        extra.insert(QStringLiteral("interval"), interval);
                        error(s, 400, "slow_down", extra);
                        return;
                    }
                    QJsonObject extra;
                    if (c.limitReached) {
                        extra.insert(QStringLiteral("reason"), QStringLiteral("device_limit_reached"));
                    }
                    error(s, 400, "authorization_pending", extra);
                    return;
                }
                const QString id = c.deviceId;
                codes.remove(dc);
                Device *d = device(id);
                if (!d) {
                    error(s, 400, "invalid_grant"); // disconnected before collection
                    return;
                }
                d->collected = true;
                replyJson(s, 200, issueTokens(id));
                return;
            }
            if (grant == QLatin1String("refresh_token")) {
                const QString token = body.value(QStringLiteral("refresh_token")).toString();
                if (spentRefreshTokens.contains(token)) {
                    dropTokensOf(spentRefreshTokens.value(token)); // reuse: theft
                    error(s, 400, "invalid_grant");
                    return;
                }
                if (!refreshTokens.contains(token)) {
                    error(s, 400, "invalid_grant");
                    return;
                }
                const QString id = refreshTokens.take(token);
                spentRefreshTokens.insert(token, id);
                replyJson(s, 200, issueTokens(id));
                return;
            }
            error(s, 400, "invalid_request");
            return;
        }

        // --- With an access token ---
        const QByteArray auth = req.headers.value("authorization");
        const QString accessToken = auth.startsWith("Bearer ") ? QString::fromLatin1(auth.mid(7)) : QString();
        const QString caller = accessTokens.value(accessToken);
        Device *me = caller.isEmpty() ? nullptr : device(caller);
        if (!me) {
            error(s, 401, "unauthorized");
            return;
        }
        const QString userId = me->userId;
        Account &account = accounts[userId];

        if (req.method == "GET" && path == "/v1/devices") {
            QJsonArray list;
            for (const Device &d : devicesOf(userId)) {
                list.append(deviceJson(d, caller));
            }
            QJsonObject json;
            json.insert(QStringLiteral("device_limit"), deviceLimit);
            json.insert(QStringLiteral("devices"), list);
            replyJson(s, 200, json);
            return;
        }
        if (path.startsWith("/v1/devices/")) {
            QByteArray rest = path.mid(int(qstrlen("/v1/devices/")));
            const bool keyPath = rest.endsWith("/key");
            if (keyPath) {
                rest.chop(4);
            }
            const QString id = QString::fromLatin1(rest);
            Device *target = device(id);
            if (!target || target->userId != userId) {
                error(s, 404, "not_found");
                return;
            }
            if (req.method == "DELETE" && !keyPath) {
                dropTokensOf(id);
                boxes.remove(id);
                for (auto it = codes.begin(); it != codes.end(); ++it) {
                    if (it->deviceId == id) {
                        it->deviceId = QStringLiteral("dev_gone");
                    }
                }
                devices.removeIf([&id](const Device &d) { return d.id == id; });
                reply(s, 204);
                return;
            }
            if (req.method == "PUT" && keyPath) {
                if (!me->ready) {
                    error(s, 403, "not_ready");
                    return;
                }
                if (id == caller) {
                    error(s, 400, "invalid_request");
                    return;
                }
                if (target->ready) {
                    error(s, 409, "already_ready");
                    return;
                }
                const QByteArray sealed = QByteArray::fromBase64(body.value(QStringLiteral("sealed_key")).toString().toLatin1());
                if (sealed.isEmpty() || sealed.size() > 256) {
                    error(s, 400, "invalid_request");
                    return;
                }
                boxes.insert(id, Box{sealed, caller, me->publicKey});
                reply(s, 204);
                return;
            }
        }
        if (path == "/v1/key") {
            if (req.method == "GET") {
                if (!boxes.contains(caller)) {
                    error(s, 404, "not_found");
                    return;
                }
                const Box b = boxes.value(caller);
                QJsonObject json;
                json.insert(QStringLiteral("sealed_key"), QString::fromLatin1(b.sealed.toBase64()));
                json.insert(QStringLiteral("from_device_id"), b.fromDeviceId);
                json.insert(QStringLiteral("from_public_key"), QString::fromLatin1(b.fromPublicKey.toBase64()));
                json.insert(QStringLiteral("created_at"), stamp());
                if (keyFilter) {
                    keyFilter(json);
                }
                replyJson(s, 200, json);
                return;
            }
            if (req.method == "DELETE") {
                me->ready = true;
                boxes.remove(caller);
                reply(s, 204);
                return;
            }
        }
        if (path == "/v1/recovery") {
            if (req.method == "GET") {
                if (account.recovery.isEmpty()) {
                    error(s, 404, "not_found");
                    return;
                }
                QJsonObject json;
                json.insert(QStringLiteral("recovery"), QString::fromLatin1(account.recovery.toBase64()));
                json.insert(QStringLiteral("updated_at"), account.recoveryUpdatedAt);
                json.insert(QStringLiteral("updated_by"), account.recoveryUpdatedBy);
                replyJson(s, 200, json);
                return;
            }
            if (req.method == "PUT") {
                bool anyReady = false;
                for (const Device &d : devicesOf(userId)) {
                    anyReady = anyReady || d.ready;
                }
                // As the Worker: a device without K may only create it, as the very first one.
                if ((anyReady || !account.recovery.isEmpty()) && !me->ready) {
                    error(s, 409, "key_exists");
                    return;
                }
                const QByteArray sealed = QByteArray::fromBase64(body.value(QStringLiteral("recovery")).toString().toLatin1());
                if (sealed.size() < 32 || sealed.size() > 512) {
                    error(s, 400, "invalid_request");
                    return;
                }
                me->ready = true;
                account.recovery = sealed;
                account.recoveryUpdatedAt = stamp();
                account.recoveryUpdatedBy = caller;
                reply(s, 204);
                return;
            }
        }
        if (path.startsWith("/v1/blob/")) {
            const QByteArray kind = path.mid(int(qstrlen("/v1/blob/")));
            int pending = 0;
            for (const Device &d : devicesOf(userId)) {
                if (d.collected && !d.ready && d.id != caller) {
                    ++pending;
                }
            }
            const QPair<QByteArray, QByteArray> pendingHeader{"Sugo-Pending-Devices", QByteArray::number(pending)};
            if (!blobKinds.contains(kind) || (req.method != "GET" && req.method != "PUT")) {
                error(s, 404, "not_found");
                return;
            }
            if (kind != "presets" && !account.plus) {
                error(s, 403, "plus_required");
                return;
            }
            Blob &stored = account.blobs[kind];
            const bool exists = !stored.blob.isEmpty() && !stored.stale;
            const QByteArray quoted = '"' + stored.etag + '"';
            if (req.method == "GET") {
                if (!exists) {
                    error(s, 404, "not_found", {}, {pendingHeader});
                    return;
                }
                if (req.headers.value("if-none-match") == quoted) {
                    reply(s, 304, {}, {pendingHeader, {"ETag", quoted}});
                    return;
                }
                reply(s, 200, stored.blob, {pendingHeader, {"ETag", quoted}}, "application/octet-stream");
                return;
            }
            const QByteArray ifMatch = req.headers.value("if-match");
            const QByteArray ifNoneMatch = req.headers.value("if-none-match");
            // A kind's own limit, or a lower one a test sets for every kind.
            const int limit = std::min(blobKinds.value(kind), maxBlobSize);
            if (req.body.size() > limit) {
                QJsonObject extra;
                extra.insert(QStringLiteral("max_bytes"), limit);
                error(s, 413, "too_large", extra);
                return;
            }
            if (ifMatch.isEmpty() == (ifNoneMatch != "*")) {
                if (ifMatch.isEmpty()) {
                    error(s, 428, "precondition_required");
                } else {
                    error(s, 400, "invalid_request");
                }
                return;
            }
            const QByteArray wanted = QByteArray(ifMatch).replace('"', "");
            if ((ifNoneMatch == "*" && exists) || (!ifMatch.isEmpty() && (!exists || wanted != stored.etag))) {
                QJsonObject extra;
                if (exists) {
                    extra.insert(QStringLiteral("etag"), QString::fromLatin1(stored.etag));
                    error(s, 412, "precondition_failed", extra, {{"ETag", quoted}});
                } else {
                    extra.insert(QStringLiteral("etag"), QJsonValue::Null);
                    error(s, 412, "precondition_failed", extra);
                }
                return;
            }
            if (!req.body.startsWith("SUGO") || req.body.size() < 4 + 1 + 24 + 16) {
                error(s, 400, "invalid_blob");
                return;
            }
            stored.blob = req.body;
            stored.stale = false;
            stored.etag = "e" + QByteArray::number(++m_counter);
            reply(s, exists ? 204 : 201, {}, {{"ETag", '"' + stored.etag + '"'}});
            return;
        }
        error(s, 404, "not_found");
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    int m_counter = 0;
    int m_clockTicks = 0;
};
