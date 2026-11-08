// SPDX-License-Identifier: GPL-3.0-or-later
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <memory>
#include <optional>

#include "FakeSecretStore.h"
#include "FakeSyncServer.h"
#include "Settings.h"
#include "SyncAccount.h"
#include "SyncApi.h"
#include "SyncCrypto.h"
#include "SyncSecrets.h"

namespace
{
const QString kUser = QStringLiteral("usr_a");
constexpr int kUnitMs = 10; ///< One "second" of SyncAccount in the tests.

/// One computer: its keyring, settings file and Sugo Sync objects.
struct Client {
    FakeSecretStore store;
    QTemporaryDir dir;
    Settings::Ptr settings;
    std::unique_ptr<SyncSecrets> secrets;
    std::unique_ptr<SyncApi> api;
    std::unique_ptr<SyncAccount> account;

    explicit Client(FakeSyncServer &server)
    {
        settings = Settings::open(dir.filePath(QStringLiteral("sugo.conf")));
        build(server);
    }

    /// New objects on the same keyring and settings, like a restart.
    void build(FakeSyncServer &server)
    {
        account.reset();
        api.reset();
        secrets = std::make_unique<SyncSecrets>(&store);
        api = std::make_unique<SyncApi>(secrets.get(), QUrl(server.base()));
        account = std::make_unique<SyncAccount>(api.get(), secrets.get(), settings);
        account->setTimeUnitMs(kUnitMs);
    }

    QJsonObject item() const { return QJsonDocument::fromJson(store.secrets->value(SyncSecrets::itemKey()).toUtf8()).object(); }
    bool hasSyncGroup() const { return settings->childGroups().contains(QStringLiteral("Sync")); }
};

QString waitForCode(Client &c)
{
    QSignalSpy code(c.account.get(), &SyncAccount::codeReady);
    if (!code.wait(5000)) {
        return {};
    }
    return code.first().first().toString();
}
} // namespace

class SyncAccountTest : public QObject
{
    Q_OBJECT

    std::unique_ptr<FakeSyncServer> server;

    /// Signs `c` in and waits for the key phase to settle on `phase`.
    void signIn(Client &c, SyncAccount::Phase phase, const QString &name = QStringLiteral("Desk"))
    {
        c.account->startSignIn(name);
        const QString code = waitForCode(c);
        QVERIFY(!code.isEmpty());
        QVERIFY(server->approve(code, kUser).startsWith(QLatin1String("dev_")));
        QTRY_COMPARE_WITH_TIMEOUT(c.account->phase(), phase, 5000);
    }

    /// A is the first device (Ready, with the recovery code), B waits for the key.
    void twoDevices(Client &a, Client &b, QString *recoveryCode = nullptr)
    {
        QSignalSpy created(a.account.get(), &SyncAccount::recoveryCodeCreated);
        signIn(a, SyncAccount::Phase::Ready, QStringLiteral("A"));
        QCOMPARE(created.size(), 1);
        if (recoveryCode) {
            *recoveryCode = created.first().first().toString();
        }
        signIn(b, SyncAccount::Phase::WaitingForKey, QStringLiteral("B"));
        QTRY_VERIFY(!b.account->verificationCode().isEmpty());
    }

private Q_SLOTS:
    void init()
    {
        server = std::make_unique<FakeSyncServer>();
        server->interval = 1;
    }
    void cleanup() { server.reset(); }

    void noKeyringNoRequest()
    {
        Client c(*server);
        // macOS without access: reads answer "nothing", writes fail.
        *c.store.failWrites = true;
        QSignalSpy unavailable(c.account.get(), &SyncAccount::keyringUnavailable);
        c.account->startSignIn(QStringLiteral("Desk"));
        QCOMPARE(unavailable.size(), 1);
        QVERIFY(!c.account->keyringAvailable());
        QCOMPARE(c.account->phase(), SyncAccount::Phase::SignedOut);
        QTest::qWait(50);
        QVERIFY(server->requests.isEmpty());

        // No keyring at all: the same.
        *c.store.failWrites = false;
        *c.store.available = false;
        c.account->startSignIn(QStringLiteral("Desk"));
        QCOMPARE(unavailable.size(), 2);
        QTest::qWait(50);
        QVERIFY(server->requests.isEmpty());

        *c.store.available = true;
        c.account->startSignIn(QStringLiteral("Desk"));
        QVERIFY(c.account->keyringAvailable());
        QVERIFY(!waitForCode(c).isEmpty());
    }

    void pendingThenDeviceLimitThenApproved()
    {
        QString first;
        server->addLinkedDevice(kUser, SyncCrypto::generateKeyPair().publicKey, true, &first);
        server->addLinkedDevice(kUser, SyncCrypto::generateKeyPair().publicKey, true);

        Client c(*server);
        QSignalSpy limit(c.account.get(), &SyncAccount::deviceLimitReachedChanged);
        c.account->startSignIn(QStringLiteral("  New   laptop "));
        const QString code = waitForCode(c);
        QCOMPARE(c.account->userCode(), code);
        QCOMPARE(c.account->verificationUri(), QUrl(QStringLiteral("https://sugo.stream/link")));
        QCOMPARE(c.account->phase(), SyncAccount::Phase::SigningIn);
        QTRY_VERIFY(server->count("POST", "/v1/auth/token") >= 2); // pending, still polling
        QVERIFY(!c.account->deviceLimitReached());

        QCOMPARE(server->approve(code, kUser), QStringLiteral("device_limit_reached"));
        QTRY_VERIFY(c.account->deviceLimitReached());
        QCOMPARE(limit.size(), 1);

        // The user frees a slot on the site and submits the code again.
        server->devices.removeIf([&first](const FakeSyncServer::Device &d) { return d.id == first; });
        QVERIFY(server->approve(code, kUser).startsWith(QLatin1String("dev_")));
        QTRY_COMPARE(c.account->phase(), SyncAccount::Phase::WaitingForKey);
        QVERIFY(!c.account->deviceLimitReached());
        QCOMPARE(c.account->deviceName(), QStringLiteral("New laptop"));
        QCOMPARE(c.account->userId(), kUser);
        QVERIFY(server->device(c.account->deviceId()));
        QCOMPARE(server->device(c.account->deviceId())->name, QStringLiteral("New laptop"));
        QCOMPARE(server->device(c.account->deviceId())->platform, SyncAccount::platformName());

        // The refresh token is stored with the device key, in the one item.
        const QJsonObject item = c.item();
        QVERIFY(server->refreshTokens.contains(item.value(QLatin1String("refreshToken")).toString()));
        QVERIFY(item.contains(QLatin1String("deviceKey")));
        QVERIFY(!item.contains(QLatin1String("dataKey")));
    }

    void slowDown()
    {
        Client c(*server);
        server->slowDownNext = 1;
        QList<qint64> polls;
        QElapsedTimer clock;
        clock.start();
        server->onRequest = [&](const FakeSyncServer::Request &r) {
            if (r.path == "/v1/auth/token") {
                polls.append(clock.elapsed());
            }
        };
        c.account->startSignIn(QStringLiteral("Desk"));
        QVERIFY(!waitForCode(c).isEmpty());
        QTRY_VERIFY(polls.size() >= 3);
        // slow_down came with interval 6 (1 + 5): the next poll waits that long.
        QVERIFY2(polls.at(1) - polls.at(0) >= 6 * kUnitMs - 2, qPrintable(QString::number(polls.at(1) - polls.at(0))));
        QCOMPARE(c.account->phase(), SyncAccount::Phase::SigningIn);
    }

    void expiredCode()
    {
        Client c(*server);
        QSignalSpy expired(c.account.get(), &SyncAccount::signInExpired);
        c.account->startSignIn(QStringLiteral("Desk"));
        const QString code = waitForCode(c);
        server->codes[server->deviceCodeFor(code)].expired = true;
        QVERIFY(expired.wait(5000));
        QCOMPARE(c.account->phase(), SyncAccount::Phase::SignedOut);
        QVERIFY(c.store.secrets->isEmpty());
        QVERIFY(!c.hasSyncGroup());
    }

    void invalidGrantWhilePolling()
    {
        Client c(*server);
        QSignalSpy failed(c.account.get(), &SyncAccount::signInFailed);
        c.account->startSignIn(QStringLiteral("Desk"));
        const QString code = waitForCode(c);
        server->codes.remove(server->deviceCodeFor(code));
        QVERIFY(failed.wait(5000));
        QCOMPARE(int(failed.first().first().value<SyncError>().kind), int(SyncError::Kind::InvalidGrant));
        QCOMPARE(c.account->phase(), SyncAccount::Phase::SignedOut);
        QVERIFY(c.store.secrets->isEmpty());
    }

    void cancelSignIn()
    {
        Client c(*server);
        c.account->startSignIn(QStringLiteral("Desk"));
        QVERIFY(!waitForCode(c).isEmpty());
        c.account->cancelSignIn();
        QCOMPARE(c.account->phase(), SyncAccount::Phase::SignedOut);
        QVERIFY(c.store.secrets->isEmpty());
        const int before = server->requests.size();
        QTest::qWait(50);
        QCOMPARE(server->requests.size(), before); // polling stopped
    }

    void firstDeviceCreatesTheKey()
    {
        Client c(*server);
        QSignalSpy created(c.account.get(), &SyncAccount::recoveryCodeCreated);
        QSignalSpy ready(c.account.get(), &SyncAccount::keyReady);
        signIn(c, SyncAccount::Phase::Ready);
        QCOMPARE(created.size(), 1);
        QCOMPARE(ready.size(), 1);
        const QString code = created.first().first().toString();

        // K and the code were stored (together) before the upload.
        const QJsonObject item = c.item();
        QVERIFY(item.contains(QLatin1String("dataKey")));
        QCOMPARE(item.value(QLatin1String("recoveryCode")).toString(), code);
        const auto writes = c.store.accesses(SyncSecrets::itemKey(), FakeSecretStore::Access::Write);
        const auto upload = server->of("PUT", "/v1/recovery");
        QCOMPARE(upload.size(), 1);
        QVERIFY(writes.last().sequence < upload.first().sequence);

        // The server's copy opens with the code to the same K.
        const auto opened = SyncCrypto::openWithRecovery(server->accounts.value(kUser).recovery, *SyncCrypto::parseRecoveryCode(code));
        QVERIFY(opened);
        QVERIFY(*opened == c.account->dataKey());
        QVERIFY(server->device(c.account->deviceId())->ready);
        QTRY_VERIFY(c.account->recoveryCodeShowable());
        QCOMPARE(c.account->recoveryCode(), code);
    }

    void keyExistsMeansWait()
    {
        Client c(*server);
        // Another device creates K between our list and our upload.
        server->onRequest = [this](const FakeSyncServer::Request &r) {
            if (r.method == "PUT" && r.path == "/v1/recovery") {
                server->addLinkedDevice(kUser, SyncCrypto::generateKeyPair().publicKey, true);
                server->onRequest = nullptr;
            }
        };
        QSignalSpy created(c.account.get(), &SyncAccount::recoveryCodeCreated);
        signIn(c, SyncAccount::Phase::WaitingForKey);
        QTRY_VERIFY(!c.account->verificationCode().isEmpty());
        QCOMPARE(server->count("PUT", "/v1/recovery"), 1);
        QVERIFY(created.isEmpty());
        QVERIFY(!c.item().contains(QLatin1String("dataKey")));
        QVERIFY(!c.item().contains(QLatin1String("recoveryCode")));
        QVERIFY(c.account->dataKey().isEmpty());
    }

    void signedOutAccountWaitsForTheRecoveryCode()
    {
        // The only device signs out: K and its recovery copy stay on the account.
        Client a(*server);
        QSignalSpy created(a.account.get(), &SyncAccount::recoveryCodeCreated);
        signIn(a, SyncAccount::Phase::Ready, QStringLiteral("A"));
        const QString code = created.first().first().toString();
        const QByteArray dataKey = a.account->dataKey().toBase64();
        QSignalSpy out(a.account.get(), &SyncAccount::signedOut);
        a.account->signOut();
        QVERIFY(out.wait(5000));

        // The next device does not make a new K (the server would refuse it, over and over): it waits.
        const int checks = server->count("GET", "/v1/recovery");
        Client b(*server);
        QSignalSpy bCreated(b.account.get(), &SyncAccount::recoveryCodeCreated);
        signIn(b, SyncAccount::Phase::WaitingForKey, QStringLiteral("B"));
        QTRY_VERIFY(server->count("GET", "/v1/recovery") > checks);
        QTest::qWait(kUnitMs * 70); // two retries of 30 "seconds"
        QVERIFY(server->count("GET", "/v1/recovery") <= checks + 3);
        QCOMPARE(server->count("PUT", "/v1/recovery"), 1); // A's
        QVERIFY(bCreated.isEmpty());
        QVERIFY(!b.item().contains(QLatin1String("dataKey")));
        QVERIFY(b.account->verificationCode().isEmpty());

        QSignalSpy ready(b.account.get(), &SyncAccount::keyReady);
        b.account->useRecoveryCode(code);
        QVERIFY(ready.wait(5000));
        QCOMPARE(b.account->dataKey().toBase64(), dataKey);
    }

    void ignoredDeviceCanBeAskedAgain()
    {
        Client a(*server);
        Client b(*server);
        twoDevices(a, b);

        QSignalSpy asked(a.account.get(), &SyncAccount::approvalRequested);
        connect(a.api.get(), &SyncApi::pendingDevicesSeen, a.account.get(), &SyncAccount::checkPendingDevices);
        a.api->getBlob(QByteArray(), [](const SyncReply &) {});
        QVERIFY(asked.wait(5000));

        // "Not Now": not asked again by itself...
        a.account->ignoreDevice(b.account->deviceId());
        a.account->checkPendingDevices(1);
        QTest::qWait(50);
        QCOMPARE(asked.size(), 1);

        // ...but when the user asks for it, from the list of devices.
        a.account->askAgain(b.account->deviceId());
        QCOMPARE(asked.size(), 2);
        QCOMPARE(asked.last().at(0).toString(), b.account->deviceId());
        QCOMPARE(asked.last().at(3).toString(), b.account->verificationCode());
    }

    void secondDeviceGetsTheKeyWithTheSameCode()
    {
        Client a(*server);
        Client b(*server);
        twoDevices(a, b);

        QSignalSpy asked(a.account.get(), &SyncAccount::approvalRequested);
        QSignalSpy approved(a.account.get(), &SyncAccount::deviceApproved);
        connect(a.api.get(), &SyncApi::pendingDevicesSeen, a.account.get(), &SyncAccount::checkPendingDevices);
        a.api->getBlob(QByteArray(), [](const SyncReply &) {}); // carries Sugo-Pending-Devices: 1
        QVERIFY(asked.wait(5000));
        QCOMPARE(asked.first().at(0).toString(), b.account->deviceId());
        QCOMPARE(asked.first().at(1).toString(), QStringLiteral("B"));
        QCOMPARE(asked.first().at(3).toString(), b.account->verificationCode());

        // Asked once per device and session.
        a.account->checkPendingDevices(1);
        QTest::qWait(50);
        QCOMPARE(asked.size(), 1);

        QSignalSpy bReady(b.account.get(), &SyncAccount::keyReady);
        a.account->approveDevice(b.account->deviceId());
        QVERIFY(approved.wait(5000));
        QTRY_COMPARE_WITH_TIMEOUT(bReady.size(), 1, 5000);
        QCOMPARE(b.account->phase(), SyncAccount::Phase::Ready);
        QVERIFY(b.account->dataKey() == a.account->dataKey());
        QVERIFY(b.account->verificationCode().isEmpty());

        // K was in B's keyring before the server heard of it.
        const auto keyWrites = b.store.accesses(SyncSecrets::itemKey(), FakeSecretStore::Access::Write);
        const auto confirm = server->of("DELETE", "/v1/key");
        QCOMPARE(confirm.size(), 1);
        QVERIFY(QJsonDocument::fromJson(keyWrites.last().value.toUtf8()).object().contains(QLatin1String("dataKey")));
        QVERIFY(keyWrites.last().sequence < confirm.first().sequence);
        QVERIFY(!b.item().contains(QLatin1String("recoveryCode")));

        // A listed B still waits (listed before it took its key); the next download, with none
        // waiting, lists the devices again.
        const auto bOnA = [&]() {
            const auto devices = a.account->devices();
            const auto it = std::find_if(devices.cbegin(), devices.cend(), [&](const SyncAccount::Device &d) { return d.id == b.account->deviceId(); });
            return it == devices.cend() ? std::optional<SyncAccount::Device>() : std::optional(*it);
        };
        QVERIFY(bOnA() && bOnA()->pending);
        const int listed = server->count("GET", "/v1/devices");
        a.api->getBlob(QByteArray(), [](const SyncReply &) {}); // Sugo-Pending-Devices: 0
        QTRY_COMPARE_WITH_TIMEOUT(server->count("GET", "/v1/devices"), listed + 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(bOnA() && !bOnA()->pending, 5000);
        // Nothing waits any more: no further listing.
        a.api->getBlob(QByteArray(), [](const SyncReply &) {});
        QTest::qWait(100);
        QCOMPARE(server->count("GET", "/v1/devices"), listed + 1);

        // Approving again is harmless (already_ready).
        a.account->approveDevice(b.account->deviceId());
        QVERIFY(approved.wait(5000));
    }

    void keySwappedAfterTheCodeIsNotApproved()
    {
        Client a(*server);
        Client b(*server);
        twoDevices(a, b);

        QSignalSpy asked(a.account.get(), &SyncAccount::approvalRequested);
        QSignalSpy approved(a.account.get(), &SyncAccount::deviceApproved);
        QSignalSpy failed(a.account.get(), &SyncAccount::approvalFailed);
        a.account->refreshDevices();
        QVERIFY(asked.wait(5000));
        QCOMPARE(asked.first().at(3).toString(), b.account->verificationCode());

        // The user compared the code; before they approve, a compromised server lists the same
        // waiting device with a key of its own.
        const SyncCrypto::KeyPair attacker = SyncCrypto::generateKeyPair();
        server->device(b.account->deviceId())->publicKey = attacker.publicKey;
        QSignalSpy listed(a.account.get(), &SyncAccount::devicesChanged);
        a.account->refreshDevices();
        QVERIFY(listed.wait(5000));

        const QByteArray keyPut = "/v1/devices/" + b.account->deviceId().toUtf8() + "/key";
        a.account->approveDevice(b.account->deviceId());
        QTest::qWait(100);
        QCOMPARE(failed.size(), 1);
        QCOMPARE(approved.size(), 0);
        QCOMPARE(server->count("PUT", keyPut), 0);

        // The device is asked about again, with the code of the key it now shows.
        QTRY_COMPARE_WITH_TIMEOUT(asked.size(), 2, 5000);
        QCOMPARE(asked.last().at(3).toString(), SyncCrypto::verificationCode(a.account->publicKey(), attacker.publicKey));
        QVERIFY(asked.last().at(3).toString() != b.account->verificationCode());
    }

    void approverGoneIsReplacedWhileWaiting()
    {
        Client a(*server);
        Client b(*server);
        twoDevices(a, b);
        server->deviceLimit = 4; // A, B and now C
        const QString codeOfA = b.account->verificationCode();
        QCOMPARE(codeOfA, SyncCrypto::verificationCode(b.account->publicKey(), a.account->publicKey()));

        // C joins and takes the key from A too.
        Client c(*server);
        signIn(c, SyncAccount::Phase::WaitingForKey, QStringLiteral("C"));
        QSignalSpy asked(a.account.get(), &SyncAccount::approvalRequested);
        connect(a.api.get(), &SyncApi::pendingDevicesSeen, a.account.get(), &SyncAccount::checkPendingDevices);
        a.api->getBlob(QByteArray(), [](const SyncReply &) {});
        QVERIFY(asked.wait(5000));
        QTRY_COMPARE_WITH_TIMEOUT(asked.size(), 2, 5000); // C goes after the still waiting B
        QCOMPARE(asked.at(1).at(0).toString(), c.account->deviceId());
        a.account->approveDevice(c.account->deviceId());
        QTRY_COMPARE_WITH_TIMEOUT(c.account->phase(), SyncAccount::Phase::Ready, 5000);

        // A signs out: its device is gone from the account, without ever having sent K.
        QSignalSpy out(a.account.get(), &SyncAccount::signedOut);
        a.account->signOut();
        QVERIFY(out.wait(5000));
        QVERIFY(!server->device(a.account->deviceId()));

        // Without the re-list, B would keep A's code (and A's list) for ever: now the code
        // and the approver follow whichever device holds the key.
        QTRY_VERIFY_WITH_TIMEOUT(b.account->verificationCode() != codeOfA, 5000);
        QCOMPARE(b.account->verificationCode(), SyncCrypto::verificationCode(b.account->publicKey(), c.account->publicKey()));
        const auto devices = b.account->devices();
        QVERIFY(std::none_of(devices.cbegin(), devices.cend(), [&](const SyncAccount::Device &d) { return d.id == a.account->deviceId(); }));
        QVERIFY(std::any_of(devices.cbegin(), devices.cend(), [&](const SyncAccount::Device &d) { return d.id == c.account->deviceId(); }));
    }

    void forgedSenderIsRejected_data()
    {
        QTest::addColumn<bool>("forgeKey");
        QTest::addColumn<bool>("forgeId");
        QTest::newRow("public key") << true << false;
        QTest::newRow("device id") << false << true;
        QTest::newRow("both") << true << true;
    }
    void forgedSenderIsRejected()
    {
        QFETCH(bool, forgeKey);
        QFETCH(bool, forgeId);
        Client a(*server);
        Client b(*server);
        twoDevices(a, b);

        // A compromised server hands B a key of its own, boxed by a key pair of its own.
        const SyncCrypto::KeyPair attacker = SyncCrypto::generateKeyPair();
        const SecretBytes attackerKey = SyncCrypto::newDataKey();
        const QByteArray bPublic = b.account->publicKey();
        server->keyFilter = [&](QJsonObject &json) {
            if (forgeKey) {
                json.insert(QStringLiteral("from_public_key"), QString::fromLatin1(attacker.publicKey.toBase64()));
                json.insert(QStringLiteral("sealed_key"),
                            QString::fromLatin1(SyncCrypto::box(attackerKey, bPublic, attacker.secretKey).toBase64()));
            }
            if (forgeId) {
                json.insert(QStringLiteral("from_device_id"), QStringLiteral("dev_attacker"));
            }
        };
        QSignalSpy rejected(b.account.get(), &SyncAccount::keyRejected);
        a.account->refreshDevices();
        QTRY_COMPARE(a.account->devices().size(), 2);
        a.account->approveDevice(b.account->deviceId());
        QVERIFY(rejected.wait(5000));
        QTest::qWait(100);
        QCOMPARE(server->count("DELETE", "/v1/key"), 0);
        QCOMPARE(b.account->phase(), SyncAccount::Phase::WaitingForKey);
        QVERIFY(b.account->dataKey().isEmpty());
        QVERIFY(!b.item().contains(QLatin1String("dataKey")));
    }

    void recoveryCode()
    {
        Client a(*server);
        Client b(*server);
        QString code;
        twoDevices(a, b, &code);

        QSignalSpy wrong(b.account.get(), &SyncAccount::recoveryCodeWrong);
        b.account->useRecoveryCode(QStringLiteral("not a code"));
        QCOMPARE(wrong.size(), 1);
        // A's check before creating K (none yet), then its own check after creating it.
        QCOMPARE(server->count("GET", "/v1/recovery"), 2);

        b.account->useRecoveryCode(SyncCrypto::formatRecoveryCode(SyncCrypto::newRecoveryCode()));
        QVERIFY(wrong.wait(5000));
        QCOMPARE(b.account->phase(), SyncAccount::Phase::WaitingForKey);
        QCOMPARE(server->count("DELETE", "/v1/key"), 0);

        QSignalSpy ready(b.account.get(), &SyncAccount::keyReady);
        b.account->useRecoveryCode(code.toLower().remove(QLatin1Char('-')));
        QVERIFY(ready.wait(5000));
        QVERIFY(b.account->dataKey() == a.account->dataKey());
        const auto keyWrites = b.store.accesses(SyncSecrets::itemKey(), FakeSecretStore::Access::Write);
        const auto confirm = server->of("DELETE", "/v1/key");
        QCOMPARE(confirm.size(), 1);
        QVERIFY(keyWrites.last().sequence < confirm.first().sequence);
        // B did not create the code: it does not keep it.
        QVERIFY(!b.item().contains(QLatin1String("recoveryCode")));
        QVERIFY(server->device(b.account->deviceId())->ready);
    }

    void oneKeyringItem()
    {
        Client a(*server);
        Client b(*server);
        QString code;
        twoDevices(a, b, &code);
        b.account->useRecoveryCode(code);
        QTRY_COMPARE(b.account->phase(), SyncAccount::Phase::Ready);
        a.account->regenerateRecoveryCode();
        QTRY_VERIFY(a.account->recoveryCodeShowable());
        for (Client *c : {&a, &b}) {
            QCOMPARE(c->store.secrets->keys(), QStringList{SyncSecrets::itemKey()});
            for (const FakeSecretStore::Access &access : std::as_const(*c->store.log)) {
                QVERIFY2(access.key == SyncSecrets::itemKey() || access.key == SyncSecrets::probeKey(), qPrintable(access.key));
                QVERIFY(access.kind == FakeSecretStore::Access::Write); // never read while running
            }
        }
    }

    void restoreReadsOnceAndResumes()
    {
        Client c(*server);
        signIn(c, SyncAccount::Phase::Ready);
        const SecretBytes key = c.account->dataKey().copy();
        server->requests.clear();
        c.store.log->clear();

        c.build(*server);
        QCOMPARE(int(c.secrets->load()), int(SyncSecrets::LoadResult::Loaded));
        QSignalSpy ready(c.account.get(), &SyncAccount::keyReady);
        QVERIFY(c.account->restore());
        QCOMPARE(ready.size(), 1);
        QCOMPARE(c.account->phase(), SyncAccount::Phase::Ready);
        QVERIFY(c.account->dataKey() == key);
        QVERIFY(server->requests.isEmpty());
        QCOMPARE(c.store.log->size(), 1);

        // Secrets gone while the settings say linked: restore() refuses.
        c.build(*server);
        c.store.secrets->clear();
        QCOMPARE(int(c.secrets->load()), int(SyncSecrets::LoadResult::Empty));
        QVERIFY(!c.account->restore());
    }

    void restoreFinishesAnInterruptedConfirmation()
    {
        Client a(*server);
        Client b(*server);
        QString code;
        twoDevices(a, b, &code);
        server->failNext("/v1/key", 500, "DELETE"); // the confirmation fails, a retry is planned
        b.account->useRecoveryCode(code);
        QTRY_VERIFY(b.item().contains(QLatin1String("dataKey")));
        QTRY_COMPARE(server->count("DELETE", "/v1/key"), 1);

        b.build(*server); // restarted before the retry
        QVERIFY(server->device(b.account->deviceId()) && !server->device(b.account->deviceId())->ready);
        QCOMPARE(int(b.secrets->load()), int(SyncSecrets::LoadResult::Loaded));
        QVERIFY(b.account->restore());
        QTRY_COMPARE(b.account->phase(), SyncAccount::Phase::Ready);
        QVERIFY(server->device(b.account->deviceId())->ready);
        QVERIFY(b.account->dataKey() == a.account->dataKey());
    }

    void signOutWithNetwork()
    {
        Client c(*server);
        signIn(c, SyncAccount::Phase::Ready);
        const QString id = c.account->deviceId();
        QSignalSpy out(c.account.get(), &SyncAccount::signedOut);
        c.account->signOut();
        QVERIFY(out.wait(5000));
        QCOMPARE(out.first().first().toBool(), true);
        QVERIFY(!server->device(id));
        QCOMPARE(c.account->phase(), SyncAccount::Phase::SignedOut);
        QVERIFY(c.store.secrets->isEmpty());
        QVERIFY(!c.hasSyncGroup());
        // The name stays for the next sign-in.
        QVERIFY(c.account->deviceName().isEmpty());
        QCOMPARE(c.account->lastDeviceName(), QStringLiteral("Desk"));
    }

    void signOutWithoutNetwork()
    {
        Client c(*server);
        signIn(c, SyncAccount::Phase::Ready);
        const QString id = c.account->deviceId();
        server->offline = true;
        QSignalSpy out(c.account.get(), &SyncAccount::signedOut);
        c.account->signOut();
        QVERIFY(out.wait(10000));
        QCOMPARE(out.first().first().toBool(), false);
        QVERIFY(server->device(id)); // the slot stays taken until disconnected on the site
        QCOMPARE(c.account->phase(), SyncAccount::Phase::SignedOut);
        QVERIFY(c.store.secrets->isEmpty());
        QVERIFY(!c.hasSyncGroup());
    }

    void disconnectedOnTheSite()
    {
        Client c(*server);
        signIn(c, SyncAccount::Phase::Ready);
        server->devices.clear();
        server->refreshTokens.clear();
        server->expireAccessTokens();
        QSignalSpy remote(c.account.get(), &SyncAccount::signedOutRemotely);
        c.account->refreshDevices();
        QVERIFY(remote.wait(5000));
        QCOMPARE(c.account->phase(), SyncAccount::Phase::SignedOut);
        QVERIFY(c.store.secrets->isEmpty());
        QVERIFY(!c.hasSyncGroup());
    }

    void devicesAndDisconnect()
    {
        Client a(*server);
        Client b(*server);
        twoDevices(a, b);
        QSignalSpy changed(a.account.get(), &SyncAccount::devicesChanged);
        a.account->refreshDevices();
        QVERIFY(changed.wait(5000));
        const QList<SyncAccount::Device> devices = a.account->devices();
        QCOMPARE(devices.size(), 2);
        QVERIFY(devices.at(0).current);
        QVERIFY(!devices.at(0).pending);
        QCOMPARE(devices.at(1).name, QStringLiteral("B"));
        QVERIFY(devices.at(1).pending);
        QVERIFY(devices.at(1).lastSeen.isValid());
        QCOMPARE(a.account->devicesAsVariantList().at(1).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("B"));

        // This device cannot be disconnected from here (sign out instead).
        a.account->disconnectDevice(a.account->deviceId());
        QTest::qWait(50);
        QVERIFY(server->device(a.account->deviceId()));

        QSignalSpy gone(a.account.get(), &SyncAccount::deviceDisconnected);
        a.account->disconnectDevice(b.account->deviceId());
        QVERIFY(gone.wait(5000));
        QVERIFY(!server->device(b.account->deviceId()));
        QCOMPARE(a.account->devices().size(), 1);
    }

    void showRecoveryCodeOnlyWhileItIsTheServers()
    {
        Client a(*server);
        Client b(*server);
        QString code;
        twoDevices(a, b, &code);
        QTRY_VERIFY(a.account->recoveryCodeShowable());
        b.account->useRecoveryCode(code);
        QTRY_COMPARE(b.account->phase(), SyncAccount::Phase::Ready);
        QVERIFY(!b.account->recoveryCodeShowable());

        // B makes a new code: A's is no longer the server's.
        QSignalSpy created(b.account.get(), &SyncAccount::recoveryCodeCreated);
        b.account->regenerateRecoveryCode();
        QVERIFY(created.wait(5000));
        const QString newCode = created.first().first().toString();
        QVERIFY(newCode != code);
        QTRY_VERIFY(b.account->recoveryCodeShowable());
        QCOMPARE(b.account->recoveryCode(), newCode);
        QCOMPARE(b.item().value(QLatin1String("recoveryCode")).toString(), newCode);

        QSignalSpy showable(a.account.get(), &SyncAccount::recoveryCodeShowableChanged);
        a.account->refreshRecoveryStatus();
        QVERIFY(showable.wait(5000));
        QVERIFY(!a.account->recoveryCodeShowable());
        QVERIFY(a.account->recoveryCode().isEmpty());

        // The old code no longer opens the server's copy; the new one does.
        const QByteArray copy = server->accounts.value(kUser).recovery;
        QVERIFY(!SyncCrypto::openWithRecovery(copy, *SyncCrypto::parseRecoveryCode(code)));
        QVERIFY(SyncCrypto::openWithRecovery(copy, *SyncCrypto::parseRecoveryCode(newCode)));
    }

    void regenerationNotStoredIsNotShowable()
    {
        Client c(*server);
        signIn(c, SyncAccount::Phase::Ready);
        QTRY_VERIFY(c.account->recoveryCodeShowable());
        *c.store.failWrites = true;
        QSignalSpy created(c.account.get(), &SyncAccount::recoveryCodeCreated);
        QSignalSpy failed(c.account.get(), &SyncAccount::keyringWriteFailed);
        c.account->regenerateRecoveryCode();
        QVERIFY(created.wait(5000));
        QCOMPARE(failed.size(), 1);
        QVERIFY(!c.account->recoveryCodeShowable());
        *c.store.failWrites = false;
        c.account->refreshRecoveryStatus();
        QTest::qWait(100);
        QVERIFY(!c.account->recoveryCodeShowable()); // the keyring holds the old code
    }
};

QTEST_GUILESS_MAIN(SyncAccountTest)
#include "tst_syncaccount.moc"
