// SPDX-License-Identifier: GPL-3.0-or-later
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

#include <memory>

#include "FakeSecretStore.h"
#include "FakeSyncServer.h"
#include "RedirectServer.h"
#include "SyncApi.h"
#include "SyncCrypto.h"
#include "SyncSecrets.h"

namespace
{
const QString kUser = QStringLiteral("usr_a");

/// A blob the fake server accepts.
QByteArray someBlob()
{
    return SyncCrypto::encryptBlob(SyncCrypto::newDataKey(), kUser, "{}");
}

QJsonObject storedItem(const FakeSecretStore &store)
{
    return QJsonDocument::fromJson(store.secrets->value(SyncSecrets::itemKey()).toUtf8()).object();
}
} // namespace

class SyncApiTest : public QObject
{
    Q_OBJECT

    std::unique_ptr<FakeSyncServer> server;
    FakeSecretStore store;
    std::unique_ptr<SyncSecrets> secrets;
    std::unique_ptr<SyncApi> api;
    QString deviceId;

    /// A device of kUser already linked; its refresh token in the keyring and in memory.
    void link(bool ready = true)
    {
        const SyncCrypto::KeyPair keys = SyncCrypto::generateKeyPair();
        const QString token = server->addLinkedDevice(kUser, keys.publicKey, ready, &deviceId);
        SyncSecrets::Values v;
        v.refreshToken = token;
        v.deviceKey = keys.secretKey.copy();
        v.dataKey = SyncCrypto::newDataKey();
        QVERIFY(secrets->replace(std::move(v)));
        store.log->clear();
        *store.reads = 0;
    }

    /// Runs `call` and waits for its answer.
    SyncReply wait(const std::function<void(SyncApi::Callback)> &call)
    {
        bool done = false;
        SyncReply result;
        call([&](const SyncReply &reply) {
            result = reply;
            done = true;
        });
        if (!QTest::qWaitFor([&]() { return done; }, 10000)) {
            qWarning("no answer");
        }
        return result;
    }

private Q_SLOTS:
    void init()
    {
        server = std::make_unique<FakeSyncServer>();
        store = FakeSecretStore();
        secrets = std::make_unique<SyncSecrets>(&store);
        api = std::make_unique<SyncApi>(secrets.get(), QUrl(server->base()));
        api->setRefreshRetryDelaysMs({20, 50});
    }
    void cleanup()
    {
        api.reset();
        secrets.reset();
        server.reset();
    }

    void errorCodes_data()
    {
        QTest::addColumn<int>("http");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<int>("kind");
        using K = SyncError::Kind;
        QTest::newRow("ok") << 200 << QByteArray("{}") << int(K::None);
        QTest::newRow("no content") << 204 << QByteArray() << int(K::None);
        QTest::newRow("not modified") << 304 << QByteArray() << int(K::None);
        QTest::newRow("no answer") << 0 << QByteArray() << int(K::Network);
        QTest::newRow("unauthorized") << 401 << QByteArray(R"({"error":"unauthorized"})") << int(K::Unauthorized);
        QTest::newRow("401 without body") << 401 << QByteArray() << int(K::Unauthorized);
        QTest::newRow("invalid_grant") << 400 << QByteArray(R"({"error":"invalid_grant"})") << int(K::InvalidGrant);
        QTest::newRow("pending") << 400 << QByteArray(R"({"error":"authorization_pending"})") << int(K::AuthorizationPending);
        QTest::newRow("slow_down") << 400 << QByteArray(R"({"error":"slow_down","interval":10})") << int(K::SlowDown);
        QTest::newRow("expired_token") << 400 << QByteArray(R"({"error":"expired_token"})") << int(K::ExpiredToken);
        QTest::newRow("rate_limited") << 429 << QByteArray(R"({"error":"rate_limited"})") << int(K::RateLimited);
        QTest::newRow("412") << 412 << QByteArray(R"({"error":"precondition_failed","etag":"x"})") << int(K::PreconditionFailed);
        QTest::newRow("428") << 428 << QByteArray(R"({"error":"precondition_required"})") << int(K::PreconditionRequired);
        QTest::newRow("413") << 413 << QByteArray(R"({"error":"too_large"})") << int(K::TooLarge);
        QTest::newRow("plus_required") << 403 << QByteArray(R"({"error":"plus_required"})") << int(K::PlusRequired);
        QTest::newRow("invalid_blob") << 400 << QByteArray(R"({"error":"invalid_blob"})") << int(K::InvalidBlob);
        QTest::newRow("invalid_request") << 400 << QByteArray(R"({"error":"invalid_request"})") << int(K::InvalidRequest);
        QTest::newRow("not_found") << 404 << QByteArray(R"({"error":"not_found"})") << int(K::NotFound);
        QTest::newRow("not_ready") << 403 << QByteArray(R"({"error":"not_ready"})") << int(K::NotReady);
        QTest::newRow("already_ready") << 409 << QByteArray(R"({"error":"already_ready"})") << int(K::AlreadyReady);
        QTest::newRow("key_exists") << 409 << QByteArray(R"({"error":"key_exists"})") << int(K::KeyExists);
        QTest::newRow("server") << 500 << QByteArray(R"({"error":"server"})") << int(K::Server);
        QTest::newRow("502 html") << 502 << QByteArray("<html>") << int(K::Server);
        QTest::newRow("unknown code") << 409 << QByteArray(R"({"error":"something_new"})") << int(K::Server);
    }
    void errorCodes()
    {
        QFETCH(int, http);
        QFETCH(QByteArray, body);
        QFETCH(int, kind);
        const SyncError e = SyncError::fromResponse(http, body, QStringLiteral("down"));
        QCOMPARE(int(e.kind), kind);
        QCOMPARE(e.http, http);
    }

    void errorFields()
    {
        const SyncError pending = SyncError::fromResponse(400, R"({"error":"authorization_pending","reason":"device_limit_reached"})");
        QCOMPARE(pending.reason, QStringLiteral("device_limit_reached"));
        const SyncError slow = SyncError::fromResponse(400, R"({"error":"slow_down","interval":10,"message":"Slow"})");
        QCOMPARE(slow.interval, 10);
        QCOMPARE(slow.message, QStringLiteral("Slow"));
        QCOMPARE(slow.code, QStringLiteral("slow_down"));
        const SyncError conflict = SyncError::fromResponse(412, R"({"error":"precondition_failed","etag":"abc"})");
        QCOMPARE(conflict.extra.value(QLatin1String("etag")).toString(), QStringLiteral("abc"));
    }

    void retryAfter_data()
    {
        QTest::addColumn<QByteArray>("header");
        QTest::addColumn<int>("seconds");
        QTest::newRow("seconds") << QByteArray("60") << 60;
        // More than an hour would overflow the retry timers' milliseconds (2500000 s: 2.5e9 ms).
        QTest::newRow("huge") << QByteArray("2500000") << 3600;
        QTest::newRow("beyond_int") << QByteArray("99999999999") << 0;
        QTest::newRow("negative") << QByteArray("-5") << 0;
        QTest::newRow("http_date") << QByteArray("Wed, 21 Oct 2015 07:28:00 GMT") << 0; // the caller's own delay
    }

    void retryAfter()
    {
        QFETCH(QByteArray, header);
        QFETCH(int, seconds);
        link();
        server->retryAfter = header;
        server->failNext("/v1/devices", 429);
        const SyncReply reply = wait([&](auto cb) { api->listDevices(cb); });
        QCOMPARE(int(reply.error.kind), int(SyncError::Kind::RateLimited));
        QCOMPARE(reply.error.retryAfter, seconds);
    }

    void baseUrl()
    {
        qunsetenv("SUGO_SYNC_URL");
        QCOMPARE(SyncApi::defaultBaseUrl(), QUrl(QStringLiteral("https://api.sugo.stream")));
        QVERIFY(SyncApi::isDefaultBaseUrl(SyncApi::defaultBaseUrl()));

        qputenv("SUGO_SYNC_URL", "http://localhost:8787");
        QCOMPARE(SyncApi::defaultBaseUrl(), QUrl(QStringLiteral("http://localhost:8787")));
        QVERIFY(!SyncApi::isDefaultBaseUrl(SyncApi::defaultBaseUrl()));
        QVERIFY(SyncApi(secrets.get(), SyncApi::defaultBaseUrl()).baseUrlAllowed());

        // Plain http to another computer would send the tokens in clear: refused, nothing sent.
        qputenv("SUGO_SYNC_URL", "http://example.org");
        SyncApi insecure(secrets.get(), SyncApi::defaultBaseUrl());
        QVERIFY(!insecure.baseUrlAllowed());
        const SyncReply reply = wait([&](auto cb) { insecure.startDevice(QStringLiteral("pc"), QStringLiteral("linux"), QByteArray(32, 'k'), cb); });
        QCOMPARE(int(reply.error.kind), int(SyncError::Kind::NotAllowed));
        qunsetenv("SUGO_SYNC_URL");

        QVERIFY(SyncApi(secrets.get(), QUrl(QStringLiteral("https://sugo-sync-dev.minerale.workers.dev"))).baseUrlAllowed());
    }

    void aRedirectedRenewalNeverCarriesTheRefreshToken_data()
    {
        QTest::addColumn<int>("status");
        QTest::newRow("302") << 302;
        QTest::newRow("307") << 307;
        QTest::newRow("308") << 308;
    }
    void aRedirectedRenewalNeverCarriesTheRefreshToken()
    {
        // The sync server sends the renewal elsewhere (another port: another origin): the
        // refresh token in its body goes to the sync server only, and stays the device's.
        QFETCH(int, status);
        link();
        const QString refreshToken = secrets->refreshToken();
        QVERIFY(!refreshToken.isEmpty());
        RedirectServer sync;
        sync.status = status;
        sync.target = server->base();
        SyncApi redirected(secrets.get(), QUrl(sync.base()));

        const SyncReply reply = wait([&](auto cb) { redirected.getBlob(SyncKind::Presets, QByteArray(), cb); });
        QVERIFY(!reply.ok());
        QVERIFY(!redirected.hasAccessToken());
        QCOMPARE(sync.requests.size(), 1);
        QCOMPARE(sync.requests.first().path, QByteArray("/v1/auth/token"));
        QVERIFY(sync.sawAnywhere(refreshToken.toUtf8())); // it was there to leak
        QVERIFY2(server->requests.isEmpty(), "the redirect to another server was followed");
        QCOMPARE(secrets->refreshToken(), refreshToken);
    }

    void aRedirectedRequestNeverCarriesTheAccessToken_data()
    {
        aRedirectedRenewalNeverCarriesTheRefreshToken_data();
    }
    void aRedirectedRequestNeverCarriesTheAccessToken()
    {
        QFETCH(int, status);
        link();
        RedirectServer sync;
        sync.status = status;
        sync.target = server->base();
        sync.answers.append({"/v1/auth/token", 200,
                             R"({"access_token":"acc_redirect_test","refresh_token":"ref_redirect_test","expires_in":3600})"});
        SyncApi redirected(secrets.get(), QUrl(sync.base()));

        const SyncReply reply = wait([&](auto cb) { redirected.getBlob(SyncKind::Presets, QByteArray(), cb); });
        QVERIFY(!reply.ok());
        QCOMPARE(sync.requests.size(), 2);
        QCOMPARE(sync.requests.at(1).path, QByteArray("/v1/blob/presets"));
        QCOMPARE(sync.requests.at(1).headers.value("authorization"), QByteArray("Bearer acc_redirect_test")); // there to leak
        QVERIFY2(server->requests.isEmpty(), "the redirect to another server was followed");
    }

    void aRedirectOnTheSameServerIsFollowed()
    {
        // The control for the tests above: the fake redirect works and is followed when it stays
        // on the sync server, with the token.
        link();
        RedirectServer sync;
        sync.target = sync.base() + QStringLiteral("/moved");
        sync.answers.append({"/v1/auth/token", 200,
                             R"({"access_token":"acc_redirect_test","refresh_token":"ref_redirect_test","expires_in":3600})"});
        sync.answers.append({"/moved/", 200, "{}"});
        SyncApi redirected(secrets.get(), QUrl(sync.base()));

        const SyncReply reply = wait([&](auto cb) { redirected.listDevices(cb); });
        QVERIFY(reply.ok());
        QCOMPARE(sync.requests.size(), 3);
        QCOMPARE(sync.requests.at(2).path, QByteArray("/moved/v1/devices"));
        QCOMPARE(sync.requests.at(2).headers.value("authorization"), QByteArray("Bearer acc_redirect_test"));
    }

    void parallelCallsRefreshOnce()
    {
        link();
        int answers = 0;
        api->listDevices([&](const SyncReply &r) {
            QVERIFY(r.ok());
            ++answers;
        });
        api->getRecovery([&](const SyncReply &r) {
            QCOMPARE(int(r.error.kind), int(SyncError::Kind::NotFound));
            ++answers;
        });
        api->ensureAccessToken([&](const SyncError &e) {
            QVERIFY(e.ok());
            ++answers;
        });
        QTRY_COMPARE(answers, 3);
        QCOMPARE(server->count("POST", "/v1/auth/token"), 1);
        QVERIFY(api->hasAccessToken());
    }

    void refreshTokenSavedBeforeTheRetry()
    {
        link();
        QVERIFY(wait([&](auto cb) { api->listDevices(cb); }).ok());
        server->expireAccessTokens();
        server->requests.clear();
        store.log->clear();

        const SyncReply reply = wait([&](auto cb) { api->listDevices(cb); });
        QVERIFY(reply.ok());
        // 401, renewal, the same request again.
        const auto gets = server->of("GET", "/v1/devices");
        QCOMPARE(gets.size(), 2);
        const auto renewals = server->of("POST", "/v1/auth/token");
        QCOMPARE(renewals.size(), 1);

        const auto writes = store.accesses(SyncSecrets::itemKey(), FakeSecretStore::Access::Write);
        QCOMPARE(writes.size(), 1);
        const QString newToken = QJsonDocument::fromJson(writes.first().value.toUtf8()).object().value(QLatin1String("refreshToken")).toString();
        QVERIFY(server->refreshTokens.contains(newToken));
        QVERIFY(renewals.first().sequence < writes.first().sequence);
        QVERIFY(writes.first().sequence < gets.at(1).sequence);
    }

    void eachRenewalWritesOnceAndNeverReads()
    {
        link();
        const QJsonObject before = storedItem(store);
        for (int i = 0; i < 3; ++i) {
            api->dropAccessToken();
            QVERIFY(wait([&](auto cb) { api->listDevices(cb); }).ok());
        }
        QCOMPARE(server->count("POST", "/v1/auth/token"), 3);
        QCOMPARE(store.accesses(SyncSecrets::itemKey(), FakeSecretStore::Access::Write).size(), 3);
        QCOMPARE(*store.reads, 0);
        const QJsonObject after = storedItem(store);
        QVERIFY(after.value(QLatin1String("refreshToken")) != before.value(QLatin1String("refreshToken")));
        QCOMPARE(after.value(QLatin1String("deviceKey")), before.value(QLatin1String("deviceKey")));
        QCOMPARE(after.value(QLatin1String("dataKey")), before.value(QLatin1String("dataKey")));
        QCOMPARE(store.secrets->size(), 1);
    }

    void failedWriteKeepsTheNewTokenInMemory()
    {
        link();
        const QString oldToken = secrets->refreshToken();
        QSignalSpy notSaved(api.get(), &SyncApi::tokenNotSaved);
        *store.failWrites = true;
        QVERIFY(wait([&](auto cb) { api->listDevices(cb); }).ok());
        QCOMPARE(notSaved.size(), 1);
        QVERIFY(secrets->hasUnsavedRefreshToken());
        const QString newToken = secrets->refreshToken();
        QVERIFY(newToken != oldToken);
        QCOMPARE(storedItem(store).value(QLatin1String("refreshToken")).toString(), oldToken);

        // The next renewal uses the token from memory (the old one would be a reuse).
        *store.failWrites = false;
        api->dropAccessToken();
        QVERIFY(wait([&](auto cb) { api->listDevices(cb); }).ok());
        const auto renewals = server->of("POST", "/v1/auth/token");
        QCOMPARE(renewals.last().json().value(QLatin1String("refresh_token")).toString(), newToken);
        QVERIFY(!secrets->hasUnsavedRefreshToken());
        QCOMPARE(storedItem(store).value(QLatin1String("refreshToken")).toString(), secrets->refreshToken());
    }

    void invalidGrantSignsOut()
    {
        link();
        QSignalSpy signedOut(api.get(), &SyncApi::signedOutRemotely);
        server->refreshTokens.clear(); // disconnected on the site
        const SyncReply reply = wait([&](auto cb) { api->listDevices(cb); });
        QCOMPARE(int(reply.error.kind), int(SyncError::Kind::InvalidGrant));
        QCOMPARE(signedOut.size(), 1);
        QVERIFY(!api->hasAccessToken());
        QCOMPARE(server->count("GET", "/v1/devices"), 0);
    }

    void reusedTokenSignsOut()
    {
        link();
        const QString spent = secrets->refreshToken();
        QVERIFY(wait([&](auto cb) { api->listDevices(cb); }).ok());
        // A copy of the old keyring (a restored backup) presents the spent token.
        FakeSecretStore otherStore;
        SyncSecrets other(&otherStore);
        QVERIFY(other.setRefreshToken(spent));
        SyncApi thief(&other, QUrl(server->base()));
        QSignalSpy signedOut(&thief, &SyncApi::signedOutRemotely);
        QCOMPARE(int(wait([&](auto cb) { thief.listDevices(cb); }).error.kind), int(SyncError::Kind::InvalidGrant));
        QCOMPARE(signedOut.size(), 1);
        // Every token of the device is gone: the real one is signed out too.
        QSignalSpy mineOut(api.get(), &SyncApi::signedOutRemotely);
        api->dropAccessToken();
        QCOMPARE(int(wait([&](auto cb) { api->listDevices(cb); }).error.kind), int(SyncError::Kind::InvalidGrant));
        QCOMPARE(mineOut.size(), 1);
    }

    void unauthorizedRetriesOnce()
    {
        link();
        server->failNext("/v1/devices", 401);
        server->failNext("/v1/devices", 401);
        const SyncReply reply = wait([&](auto cb) { api->listDevices(cb); });
        QCOMPARE(int(reply.error.kind), int(SyncError::Kind::Unauthorized));
        QCOMPARE(server->count("GET", "/v1/devices"), 2);
        QCOMPARE(server->count("POST", "/v1/auth/token"), 2);
    }

    void networkErrorKeepsTheRefreshToken()
    {
        link();
        const QString token = secrets->refreshToken();
        server->offline = true;
        const SyncReply reply = wait([&](auto cb) { api->listDevices(cb); });
        QCOMPARE(int(reply.error.kind), int(SyncError::Kind::Network));
        QVERIFY(reply.error.transient());
        QCOMPARE(secrets->refreshToken(), token);
        server->offline = false;
        QVERIFY(wait([&](auto cb) { api->listDevices(cb); }).ok());
    }

    void lostRenewalAnswerIsRetriedWithinTheGrace()
    {
        link();
        const QString first = secrets->refreshToken();
        server->refreshGrace = true;
        server->loseAnswers = 1; // the server takes the token, its answer never comes
        QSignalSpy signedOut(api.get(), &SyncApi::signedOutRemotely);
        QVERIFY(wait([&](auto cb) { api->listDevices(cb); }).ok());
        QCOMPARE(server->count("POST", "/v1/auth/token"), 2);
        QCOMPARE(signedOut.size(), 0);
        // Only the pair given to the retry lives: the one whose answer was lost is revoked.
        QVERIFY(secrets->refreshToken() != first);
        QCOMPARE(server->refreshTokens.size(), 1);
        QVERIFY(server->refreshTokens.contains(secrets->refreshToken()));
        // And the chain goes on.
        api->dropAccessToken();
        QVERIFY(wait([&](auto cb) { api->listDevices(cb); }).ok());
        QCOMPARE(signedOut.size(), 0);
    }

    void renewalServerErrorIsRetried()
    {
        link();
        server->failNext("/v1/auth/token", 503);
        server->failNext("/v1/auth/token", 0);
        QVERIFY(wait([&](auto cb) { api->listDevices(cb); }).ok());
        QCOMPARE(server->count("POST", "/v1/auth/token"), 3);
    }

    void renewalRetriesAreFew()
    {
        link();
        server->offline = true;
        const SyncReply reply = wait([&](auto cb) { api->listDevices(cb); });
        QCOMPARE(int(reply.error.kind), int(SyncError::Kind::Network));
        QCOMPARE(server->count("POST", "/v1/auth/token"), 3); // the first try and two retries
    }

    void renewalRetriesStayWithinTheGrace_data()
    {
        QTest::addColumn<qint64>("tryTakesMs");
        QTest::addColumn<int>("tries");
        QTest::newRow("quick") << qint64(1000) << 3;
        QTest::newRow("second retry would start after 40 s") << qint64(20000) << 2;
        QTest::newRow("first retry would start after 40 s") << qint64(40000) << 1;
    }
    void renewalRetriesStayWithinTheGrace()
    {
        // The server takes a spent token back only within a minute of its first use: no retry
        // starts more than 40 s after the first try, however long the tries took.
        QFETCH(qint64, tryTakesMs);
        QFETCH(int, tries);
        link();
        qint64 clock = 0;
        api->setNowProvider([&clock]() { return clock; });
        server->offline = true;
        server->onRequest = [&](const FakeSyncServer::Request &r) {
            if (r.path == "/v1/auth/token") {
                clock += tryTakesMs;
            }
        };
        const SyncReply reply = wait([&](auto cb) { api->listDevices(cb); });
        QCOMPARE(int(reply.error.kind), int(SyncError::Kind::Network));
        QCOMPARE(server->count("POST", "/v1/auth/token"), tries);
    }

    void renewalRateLimitIsNotRetried()
    {
        link();
        server->failNext("/v1/auth/token", 429);
        const SyncReply reply = wait([&](auto cb) { api->listDevices(cb); });
        QCOMPARE(int(reply.error.kind), int(SyncError::Kind::RateLimited));
        QCOMPARE(server->count("POST", "/v1/auth/token"), 1);
    }

    void resetStopsTheRenewalRetries()
    {
        link();
        api->setRefreshRetryDelaysMs({200, 200});
        server->offline = true;
        bool answered = false;
        api->listDevices([&](const SyncReply &) { answered = true; });
        QTRY_COMPARE(server->count("POST", "/v1/auth/token"), 1);
        api->reset();
        QTest::qWait(500);
        QCOMPARE(server->count("POST", "/v1/auth/token"), 1);
        QVERIFY(!answered);
    }

    void noRefreshToken()
    {
        const SyncReply reply = wait([&](auto cb) { api->listDevices(cb); });
        QCOMPARE(int(reply.error.kind), int(SyncError::Kind::Unauthorized));
        QVERIFY(server->requests.isEmpty());
    }

    void blobPreconditions()
    {
        link();
        const QByteArray blob = someBlob();
        SyncReply first = wait([&](auto cb) { api->putBlob(SyncKind::Presets, blob, QByteArray(), cb); });
        QCOMPARE(first.http, 201);
        QVERIFY(!first.etag.isEmpty());
        QCOMPARE(server->of("PUT", "/v1/blob/presets").last().headers.value("if-none-match"), QByteArray("*"));

        SyncReply again = wait([&](auto cb) { api->putBlob(SyncKind::Presets, blob, QByteArray(), cb); });
        QCOMPARE(int(again.error.kind), int(SyncError::Kind::PreconditionFailed));
        QCOMPARE(again.etag, first.etag);

        SyncReply unchanged = wait([&](auto cb) { api->getBlob(SyncKind::Presets, first.etag, cb); });
        QVERIFY(unchanged.ok());
        QCOMPARE(unchanged.http, 304);
        QCOMPARE(unchanged.pendingDevices, 0);

        SyncReply replaced = wait([&](auto cb) { api->putBlob(SyncKind::Presets, blob, first.etag, cb); });
        QCOMPARE(replaced.http, 204);
        QVERIFY(replaced.etag != first.etag);
        QCOMPARE(server->of("PUT", "/v1/blob/presets").last().headers.value("if-match"), '"' + first.etag + '"');

        SyncReply stale = wait([&](auto cb) { api->putBlob(SyncKind::Presets, blob, first.etag, cb); });
        QCOMPARE(int(stale.error.kind), int(SyncError::Kind::PreconditionFailed));
        QCOMPARE(stale.error.extra.value(QLatin1String("etag")).toString().toLatin1(), replaced.etag);

        SyncReply full = wait([&](auto cb) { api->getBlob(SyncKind::Presets, QByteArray(), cb); });
        QCOMPARE(full.http, 200);
        QCOMPARE(full.body, blob);
        QCOMPARE(full.etag, replaced.etag);

        SyncReply huge = wait([&](auto cb) { api->putBlob(SyncKind::Presets, QByteArray(300000, 'x'), replaced.etag, cb); });
        QCOMPARE(int(huge.error.kind), int(SyncError::Kind::TooLarge));
    }

    void blobPerKind()
    {
        link();
        server->accounts[kUser].plus = true;
        const QByteArray presets = someBlob();
        const QByteArray sessions = someBlob();
        const SyncReply first = wait([&](auto cb) { api->putBlob(SyncKind::Presets, presets, QByteArray(), cb); });
        QCOMPARE(first.http, 201);
        const SyncReply other = wait([&](auto cb) { api->putBlob(SyncKind::Sessions, sessions, QByteArray(), cb); });
        QCOMPARE(other.http, 201);
        QCOMPARE(server->count("PUT", "/v1/blob/sessions"), 1);
        QVERIFY(other.etag != first.etag);

        const SyncReply got = wait([&](auto cb) { api->getBlob(SyncKind::Sessions, QByteArray(), cb); });
        QCOMPARE(got.body, sessions);
        QCOMPARE(got.etag, other.etag);
        const SyncReply settings = wait([&](auto cb) { api->getBlob(SyncKind::Settings, QByteArray(), cb); });
        QCOMPARE(int(settings.error.kind), int(SyncError::Kind::NotFound));
        QCOMPARE(server->count("GET", "/v1/blob/settings"), 1);

        // Each kind has its own limit.
        const SyncReply huge = wait([&](auto cb) { api->putBlob(SyncKind::Settings, QByteArray(9000, 'x'), QByteArray(), cb); });
        QCOMPARE(int(huge.error.kind), int(SyncError::Kind::TooLarge));
        QCOMPARE(huge.error.extra.value(QLatin1String("max_bytes")).toInt(), 8192);
    }

    void plusRequired()
    {
        link();
        const SyncReply get = wait([&](auto cb) { api->getBlob(SyncKind::Tasks, QByteArray(), cb); });
        QCOMPARE(int(get.error.kind), int(SyncError::Kind::PlusRequired));
        QCOMPARE(get.http, 403);
        QVERIFY(!get.error.transient());
        const SyncReply put = wait([&](auto cb) { api->putBlob(SyncKind::Tasks, someBlob(), QByteArray(), cb); });
        QCOMPARE(int(put.error.kind), int(SyncError::Kind::PlusRequired));
        // The timers sync on every plan.
        QCOMPARE(wait([&](auto cb) { api->putBlob(SyncKind::Presets, someBlob(), QByteArray(), cb); }).http, 201);
    }

    void unknownKind()
    {
        // A Worker older than the kind: 404 to both GET and PUT.
        link();
        server->accounts[kUser].plus = true;
        server->blobKinds.remove("sessions");
        const SyncReply get = wait([&](auto cb) { api->getBlob(SyncKind::Sessions, QByteArray(), cb); });
        QCOMPARE(int(get.error.kind), int(SyncError::Kind::NotFound));
        const SyncReply put = wait([&](auto cb) { api->putBlob(SyncKind::Sessions, someBlob(), QByteArray(), cb); });
        QCOMPARE(int(put.error.kind), int(SyncError::Kind::NotFound));
    }

    void kindNames()
    {
        QCOMPARE(SyncKinds::name(SyncKind::Presets), QLatin1String("presets"));
        QCOMPARE(SyncKinds::name(SyncKind::Statistics), QLatin1String("statistics"));
        QCOMPARE(SyncKinds::maxBytes(SyncKind::Presets), qsizetype(262144));
        QCOMPARE(SyncKinds::maxBytes(SyncKind::Tasks), qsizetype(1048576));
        QCOMPARE(SyncKinds::maxBytes(SyncKind::Settings), qsizetype(8192));
        QVERIFY(!SyncKinds::needsPlus(SyncKind::Presets));
        QVERIFY(SyncKinds::needsPlus(SyncKind::Sessions));
        QCOMPARE(SyncCrypto::encryptBlob(SyncCrypto::newDataKey(), kUser, "abc").size(), 3 + SyncKinds::BlobOverhead);
    }

    void anOversizedAnswerIsCutOff()
    {
        link();
        // A server that sends more than any blob may hold: placed on it directly, as no upload
        // that large is let in.
        server->accounts[kUser].blobs["presets"] = {QByteArrayLiteral("SUGO") + QByteArray(9 * 1024 * 1024, 'x'), "e1"};

        const SyncReply reply = wait([&](auto cb) { api->getBlob(SyncKind::Presets, QByteArray(), cb); });
        QCOMPARE(int(reply.error.kind), int(SyncError::Kind::Network));
        QCOMPARE(reply.http, 0);
        QVERIFY(reply.body.isEmpty());
    }

    void missingBlob()
    {
        link();
        const SyncReply reply = wait([&](auto cb) { api->getBlob(SyncKind::Presets, QByteArray(), cb); });
        QCOMPARE(int(reply.error.kind), int(SyncError::Kind::NotFound));
    }

    void pendingDevicesHeader()
    {
        link();
        server->addLinkedDevice(kUser, SyncCrypto::generateKeyPair().publicKey, false);
        QSignalSpy pending(api.get(), &SyncApi::pendingDevicesSeen);
        const SyncReply reply = wait([&](auto cb) { api->getBlob(SyncKind::Presets, QByteArray(), cb); });
        QCOMPARE(reply.pendingDevices, 1);
        QCOMPARE(pending.size(), 1);
        QCOMPARE(pending.first().first().toInt(), 1);
    }

    void resetDropsAnswers()
    {
        link();
        bool called = false;
        api->listDevices([&](const SyncReply &) { called = true; });
        api->reset();
        QTest::qWait(200);
        QVERIFY(!called);
        // The next call works again.
        QVERIFY(wait([&](auto cb) { api->listDevices(cb); }).ok());
    }

    void signInCodeAndTokens()
    {
        const SyncCrypto::KeyPair keys = SyncCrypto::generateKeyPair();
        QVERIFY(secrets->replace([&]() {
            SyncSecrets::Values v;
            v.deviceKey = keys.secretKey.copy();
            return v;
        }()));
        const SyncReply start = wait([&](auto cb) { api->startDevice(QStringLiteral("pc"), QStringLiteral("linux"), keys.publicKey, cb); });
        QVERIFY(start.ok());
        const QString deviceCode = start.json.value(QLatin1String("device_code")).toString();
        QCOMPARE(server->codes.value(deviceCode).publicKey, keys.publicKey);

        SyncReply poll = wait([&](auto cb) { api->pollToken(deviceCode, cb); });
        QCOMPARE(int(poll.error.kind), int(SyncError::Kind::AuthorizationPending));

        QVERIFY(server->approve(start.json.value(QLatin1String("user_code")).toString()).startsWith(QLatin1String("dev_")));
        poll = wait([&](auto cb) { api->pollToken(deviceCode, cb); });
        QVERIFY(poll.ok());
        QCOMPARE(poll.json.value(QLatin1String("user_id")).toString(), kUser);
        QVERIFY(api->hasAccessToken());
        // The refresh token joined the device key in the one item.
        const QJsonObject item = storedItem(store);
        QCOMPARE(item.value(QLatin1String("refreshToken")).toString(), poll.json.value(QLatin1String("refresh_token")).toString());
        QCOMPARE(item.value(QLatin1String("deviceKey")).toString(), QString::fromLatin1(keys.secretKey.toBase64()));
    }
};

QTEST_GUILESS_MAIN(SyncApiTest)
#include "tst_syncapi.moc"
