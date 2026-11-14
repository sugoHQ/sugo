// SPDX-License-Identifier: GPL-3.0-or-later
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
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

    /// Restarts `c` on the same keyring and settings, as at the next launch.
    void restart(Client &c)
    {
        c.build(*server);
        QCOMPARE(int(c.secrets->load()), int(SyncSecrets::LoadResult::Loaded));
        QVERIFY(c.account->restore());
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

    /// A and B hold the key; C waits for it and shows the code of B, the device seen last.
    void thirdDeviceWaits(Client &a, Client &b, Client &c)
    {
        twoDevices(a, b);
        if (QTest::currentTestFailed()) {
            return;
        }
        QSignalSpy asked(a.account.get(), &SyncAccount::approvalRequested);
        a.account->refreshDevices();
        QVERIFY(asked.wait(5000));
        QVERIFY(b.account->confirmVerificationCode(b.account->verificationCode()));
        a.account->approveDevice(b.account->deviceId());
        QTRY_COMPARE_WITH_TIMEOUT(b.account->phase(), SyncAccount::Phase::Ready, 5000);

        server->deviceLimit = 4;
        signIn(c, SyncAccount::Phase::WaitingForKey, QStringLiteral("C"));
        QTRY_COMPARE_WITH_TIMEOUT(c.account->verificationCode(),
                                  SyncCrypto::verificationCode(c.account->publicKey(), b.account->publicKey()), 5000);
    }

    /// When `on` last saw the device of `of`, by the list it holds (invalid if it is not there).
    static QDateTime lastSeen(const Client &on, const Client &of)
    {
        const auto devices = on.account->devices();
        const auto it = std::find_if(devices.cbegin(), devices.cend(), [&](const SyncAccount::Device &d) { return d.id == of.account->deviceId(); });
        return it == devices.cend() ? QDateTime() : it->lastSeen;
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

    void approvalExtendsTheCode()
    {
        // The code said one second at first; the server then says it lives on (an approval
        // extends it), and Sugo keeps polling past the first deadline.
        server->codeExpiresIn = 1;
        server->pendingExpiresIn = 120;
        Client c(*server);
        QSignalSpy expired(c.account.get(), &SyncAccount::signInExpired);
        c.account->startSignIn(QStringLiteral("Desk"));
        const QString code = waitForCode(c);
        QTest::qWait(1500);
        QCOMPARE(expired.size(), 0);
        QCOMPARE(c.account->phase(), SyncAccount::Phase::SigningIn);
        QVERIFY(server->approve(code, kUser).startsWith(QLatin1String("dev_")));
        QTRY_COMPARE_WITH_TIMEOUT(c.account->phase(), SyncAccount::Phase::WaitingForKey, 5000);
    }

    void codeEndsWhenTheServerSaysSo()
    {
        server->pendingExpiresIn = 0;
        Client c(*server);
        QSignalSpy expired(c.account.get(), &SyncAccount::signInExpired);
        c.account->startSignIn(QStringLiteral("Desk"));
        QVERIFY(!waitForCode(c).isEmpty());
        QVERIFY(expired.wait(5000));
        QCOMPARE(c.account->phase(), SyncAccount::Phase::SignedOut);
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
        a.api->getBlob(SyncKind::Presets, QByteArray(), [](const SyncReply &) {});
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
        a.api->getBlob(SyncKind::Presets, QByteArray(), [](const SyncReply &) {}); // carries Sugo-Pending-Devices: 1
        QVERIFY(asked.wait(5000));
        QCOMPARE(asked.first().at(0).toString(), b.account->deviceId());
        QCOMPARE(asked.first().at(1).toString(), QStringLiteral("B"));
        QCOMPARE(asked.first().at(3).toString(), b.account->verificationCode());

        // Asked once per device and session.
        a.account->checkPendingDevices(1);
        QTest::qWait(50);
        QCOMPARE(asked.size(), 1);

        QSignalSpy bReady(b.account.get(), &SyncAccount::keyReady);
        QVERIFY(b.account->confirmVerificationCode(b.account->verificationCode()));
        a.account->approveDevice(b.account->deviceId());
        QVERIFY(approved.wait(5000));
        QTRY_COMPARE_WITH_TIMEOUT(bReady.size(), 1, 5000);
        QCOMPARE(b.account->phase(), SyncAccount::Phase::Ready);
        QVERIFY(b.account->dataKey() == a.account->dataKey());
        QVERIFY(b.account->verificationCode().isEmpty());

        // K was in B's keyring before the server heard of it; A's account has a verifier (A sent it
        // with the recovery copy), so B proved holding K instead of declaring it.
        const auto keyWrites = b.store.accesses(SyncSecrets::itemKey(), FakeSecretStore::Access::Write);
        const auto confirm = server->of("POST", "/v1/key/ready");
        QCOMPARE(confirm.size(), 1);
        QCOMPARE(server->count("DELETE", "/v1/key"), 0);
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
        a.api->getBlob(SyncKind::Presets, QByteArray(), [](const SyncReply &) {}); // Sugo-Pending-Devices: 0
        QTRY_COMPARE_WITH_TIMEOUT(server->count("GET", "/v1/devices"), listed + 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(bOnA() && !bOnA()->pending, 5000);
        // Nothing waits any more: no further listing.
        a.api->getBlob(SyncKind::Presets, QByteArray(), [](const SyncReply &) {});
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
        a.api->getBlob(SyncKind::Presets, QByteArray(), [](const SyncReply &) {});
        QVERIFY(asked.wait(5000));
        QTRY_COMPARE_WITH_TIMEOUT(asked.size(), 2, 5000); // C goes after the still waiting B
        QCOMPARE(asked.at(1).at(0).toString(), c.account->deviceId());
        QVERIFY(c.account->confirmVerificationCode(c.account->verificationCode()));
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

    void approverStaysWhileItCanStillApprove()
    {
        Client a(*server);
        Client b(*server);
        Client c(*server);
        thirdDeviceWaits(a, b, c);
        if (QTest::currentTestFailed()) {
            return;
        }
        const QString codeOfB = c.account->verificationCode();
        QSignalSpy codeChanged(c.account.get(), &SyncAccount::verificationCodeChanged);
        QSignalSpy rejected(c.account.get(), &SyncAccount::keyRejected);

        // A syncs, so now it is the device seen last; B is still there and can still approve.
        // C reads the list again after its minute: the code the user is comparing must not move.
        server->seen(a.account->deviceId());
        QTRY_VERIFY_WITH_TIMEOUT(lastSeen(c, a) > lastSeen(c, b), 5000);
        QCOMPARE(c.account->verificationCode(), codeOfB);
        QCOMPARE(codeChanged.size(), 0);

        // The user found the same code on B and approves there: C takes the key B sends.
        QSignalSpy asked(b.account.get(), &SyncAccount::approvalRequested);
        b.account->refreshDevices();
        QVERIFY(asked.wait(5000));
        QCOMPARE(asked.first().at(0).toString(), c.account->deviceId());
        QCOMPARE(asked.first().at(3).toString(), codeOfB);
        QVERIFY(c.account->confirmVerificationCode(codeOfB));
        b.account->approveDevice(c.account->deviceId());
        QTRY_COMPARE_WITH_TIMEOUT(c.account->phase(), SyncAccount::Phase::Ready, 5000);
        QCOMPARE(rejected.size(), 0);
        QVERIFY(c.account->dataKey() == a.account->dataKey());
    }

    void approverThatCanNoLongerApproveIsReplaced_data()
    {
        QTest::addColumn<bool>("newKey");
        QTest::newRow("another public key") << true;
        QTest::newRow("no longer ready") << false;
    }
    void approverThatCanNoLongerApproveIsReplaced()
    {
        QFETCH(bool, newKey);
        Client a(*server);
        Client b(*server);
        Client c(*server);
        thirdDeviceWaits(a, b, c);
        if (QTest::currentTestFailed()) {
            return;
        }
        const QString codeOfB = c.account->verificationCode();
        server->seen(a.account->deviceId());
        QTRY_VERIFY_WITH_TIMEOUT(lastSeen(c, a) > lastSeen(c, b), 5000);
        QCOMPARE(c.account->verificationCode(), codeOfB);

        // B is still listed, but no longer as the device the code was shown for: the code it
        // covered is worth nothing now, and the device seen last is chosen again.
        if (newKey) {
            server->device(b.account->deviceId())->publicKey = SyncCrypto::generateKeyPair().publicKey;
        } else {
            server->device(b.account->deviceId())->ready = false;
        }
        QTRY_COMPARE_WITH_TIMEOUT(c.account->verificationCode(),
                                  SyncCrypto::verificationCode(c.account->publicKey(), a.account->publicKey()), 5000);
    }

    void waitingAgainChoosesTheApproverAgain()
    {
        Client a(*server);
        Client b(*server);
        Client c(*server);
        thirdDeviceWaits(a, b, c);
        if (QTest::currentTestFailed()) {
            return;
        }
        server->seen(a.account->deviceId());

        // A wait started over (as after a refused key) does not hold on to the approver of the
        // wait before it.
        c.account->waitForKey();
        QTRY_COMPARE_WITH_TIMEOUT(c.account->verificationCode(),
                                  SyncCrypto::verificationCode(c.account->publicKey(), a.account->publicKey()), 5000);
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

    void keyWaitsForTheCodeToBeConfirmed()
    {
        Client a(*server);
        Client b(*server);
        twoDevices(a, b);
        QSignalSpy approved(a.account.get(), &SyncAccount::deviceApproved);
        QSignalSpy confirmed(b.account.get(), &SyncAccount::verificationConfirmedChanged);
        a.account->refreshDevices();
        QTRY_COMPARE(a.account->devices().size(), 2);
        a.account->approveDevice(b.account->deviceId());
        QVERIFY(approved.wait(5000));

        // The box is there, but the user has not said yet that A shows the same code: B holds it
        // unopened, stores nothing and stops asking the server.
        // (B may fetch the box before A even hears its PUT answered: wait for a fetch after the PUT.)
        const QByteArray keyPut = "/v1/devices/" + b.account->deviceId().toUtf8() + "/key";
        const qint64 putAt = server->of("PUT", keyPut).last().sequence;
        const auto fetchedAfterPut = [&]() {
            const auto gets = server->of("GET", "/v1/key");
            return std::any_of(gets.cbegin(), gets.cend(), [&](const FakeSyncServer::Request &r) { return r.sequence > putAt; });
        };
        QTRY_VERIFY_WITH_TIMEOUT(fetchedAfterPut(), 5000);
        const int fetched = server->count("GET", "/v1/key");
        QTest::qWait(30 * kUnitMs);
        QCOMPARE(server->count("GET", "/v1/key"), fetched);
        QCOMPARE(b.account->phase(), SyncAccount::Phase::WaitingForKey);
        QVERIFY(b.account->dataKey().isEmpty());
        QVERIFY(!b.item().contains(QLatin1String("dataKey")));
        QCOMPARE(server->count("DELETE", "/v1/key"), 0);
        QVERIFY(!b.account->verificationConfirmed());

        // A code that is not the one shown confirms nothing.
        QVERIFY(!b.account->confirmVerificationCode(QStringLiteral("AAAA-AAAA-AAAA-AAAA")));
        QVERIFY(!b.account->confirmVerificationCode(QString()));
        QCOMPARE(confirmed.size(), 0);

        // The user confirms: the box held is opened at once.
        QVERIFY(b.account->confirmVerificationCode(b.account->verificationCode()));
        QCOMPARE(confirmed.size(), 1);
        QCOMPARE(confirmed.first().first().toBool(), true);
        QTRY_COMPARE_WITH_TIMEOUT(b.account->phase(), SyncAccount::Phase::Ready, 5000);
        QVERIFY(b.account->dataKey() == a.account->dataKey());
        QCOMPARE(server->count("GET", "/v1/key"), fetched); // the held box, not a new request
    }

    void signOutDropsTheConfirmation()
    {
        Client a(*server);
        Client b(*server);
        twoDevices(a, b);
        QSignalSpy confirmed(b.account.get(), &SyncAccount::verificationConfirmedChanged);
        QVERIFY(b.account->confirmVerificationCode(b.account->verificationCode()));
        QSignalSpy out(b.account.get(), &SyncAccount::signedOut);
        b.account->signOut();
        QVERIFY(out.wait(5000));
        QVERIFY(!b.account->verificationConfirmed());
        QCOMPARE(confirmed.size(), 2);
        QCOMPARE(confirmed.last().first().toBool(), false);
    }

    void keyOfADeviceListedByTheServerIsNotTaken()
    {
        // A compromised server lists a ready device of its own, seen last, and boxes a key it
        // knows for the new device. Before, the new device took it without the user doing
        // anything; now it shows the code of that device, which A cannot show, and waits.
        Client a(*server);
        signIn(a, SyncAccount::Phase::Ready, QStringLiteral("A"));
        server->deviceLimit = 4; // A, the server's own and B
        const SyncCrypto::KeyPair attacker = SyncCrypto::generateKeyPair();
        QString fakeId;
        server->addLinkedDevice(kUser, attacker.publicKey, true, &fakeId, QStringLiteral("A"));
        server->seen(fakeId);

        Client b(*server);
        signIn(b, SyncAccount::Phase::WaitingForKey, QStringLiteral("B"));
        QTRY_COMPARE_WITH_TIMEOUT(b.account->verificationCode(), SyncCrypto::verificationCode(b.account->publicKey(), attacker.publicKey), 5000);
        QCOMPARE(b.account->approverName(), QStringLiteral("A"));
        QVERIFY(b.account->verificationCode() != SyncCrypto::verificationCode(b.account->publicKey(), a.account->publicKey()));

        const SecretBytes attackerKey = SyncCrypto::newDataKey();
        server->boxes.insert(b.account->deviceId(), {SyncCrypto::box(attackerKey, b.account->publicKey(), attacker.secretKey), fakeId, attacker.publicKey});
        const int fetched = server->count("GET", "/v1/key");
        QTRY_VERIFY_WITH_TIMEOUT(server->count("GET", "/v1/key") > fetched, 5000);
        QTest::qWait(30 * kUnitMs);
        QCOMPARE(b.account->phase(), SyncAccount::Phase::WaitingForKey);
        QVERIFY(b.account->dataKey().isEmpty());
        QVERIFY(!b.item().contains(QLatin1String("dataKey")));
        QCOMPARE(server->count("DELETE", "/v1/key"), 0);
    }

    void confirmationLapsesWhenTheApproverChanges()
    {
        Client a(*server);
        Client b(*server);
        Client c(*server);
        thirdDeviceWaits(a, b, c);
        if (QTest::currentTestFailed()) {
            return;
        }
        // The user confirms the code of B; then the list shows B with another key, and the code
        // moves to A. The confirmation was for B's key only: it lapses, and the server cannot
        // carry it over to another device by changing the list after the click.
        const QString codeOfB = c.account->verificationCode();
        QSignalSpy confirmed(c.account.get(), &SyncAccount::verificationConfirmedChanged);
        QVERIFY(c.account->confirmVerificationCode(codeOfB));
        QVERIFY(c.account->verificationConfirmed());
        server->seen(a.account->deviceId());
        server->device(b.account->deviceId())->publicKey = SyncCrypto::generateKeyPair().publicKey;
        const QString codeOfA = SyncCrypto::verificationCode(c.account->publicKey(), a.account->publicKey());
        QTRY_COMPARE_WITH_TIMEOUT(c.account->verificationCode(), codeOfA, 5000);
        QVERIFY(!c.account->verificationConfirmed());
        QCOMPARE(confirmed.size(), 2);
        QCOMPARE(confirmed.last().first().toBool(), false);
        QCOMPARE(c.account->approverName(), QStringLiteral("A"));

        // A approves: the key waits for a confirmation of A's code; the old one is refused.
        QSignalSpy approved(a.account.get(), &SyncAccount::deviceApproved);
        a.account->refreshDevices();
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(a.account->devices().cbegin(), a.account->devices().cend(),
                                             [&](const SyncAccount::Device &d) { return d.id == c.account->deviceId(); }),
                                 5000);
        a.account->approveDevice(c.account->deviceId());
        QVERIFY(approved.wait(5000));
        QTest::qWait(30 * kUnitMs);
        QCOMPARE(c.account->phase(), SyncAccount::Phase::WaitingForKey);
        QVERIFY(!c.account->confirmVerificationCode(codeOfB));
        QVERIFY(c.account->confirmVerificationCode(codeOfA));
        QTRY_COMPARE_WITH_TIMEOUT(c.account->phase(), SyncAccount::Phase::Ready, 5000);
        QVERIFY(c.account->dataKey() == a.account->dataKey());
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
        const auto confirm = server->of("POST", "/v1/key/ready");
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
        server->failNext("/v1/key/ready", 500, "POST"); // the confirmation fails, a retry is planned
        b.account->useRecoveryCode(code);
        QTRY_VERIFY(b.item().contains(QLatin1String("dataKey")));
        QTRY_COMPARE(server->count("POST", "/v1/key/ready"), 1);

        b.build(*server); // restarted before the retry
        QVERIFY(server->device(b.account->deviceId()) && !server->device(b.account->deviceId())->ready);
        QCOMPARE(int(b.secrets->load()), int(SyncSecrets::LoadResult::Loaded));
        QVERIFY(b.account->restore());
        QTRY_COMPARE(b.account->phase(), SyncAccount::Phase::Ready);
        QVERIFY(server->device(b.account->deviceId())->ready);
        QVERIFY(b.account->dataKey() == a.account->dataKey());
    }

    // --- Ready verifier (docs/api.md of sugo-sync, "Proof of holding the key") ---------------------

    void firstDeviceSetsTheVerifierWithTheRecoveryCopy()
    {
        Client a(*server);
        signIn(a, SyncAccount::Phase::Ready, QStringLiteral("A"));
        const QByteArray verifier = SyncCrypto::readyVerifier(a.account->dataKey());
        QCOMPARE(verifier.size(), SyncCrypto::ReadyVerifierBytes);
        const auto puts = server->of("PUT", "/v1/recovery");
        QCOMPARE(puts.size(), 1);
        QCOMPARE(QByteArray::fromBase64(puts.first().json().value(QLatin1String("ready_verifier")).toString().toLatin1()), verifier);
        QCOMPARE(server->accounts.value(kUser).readyVerifier, verifier);
        // Checked once, then never asked again: it changes only when the account starts over.
        QTRY_COMPARE(server->count("GET", "/v1/key/verifier"), 1);
        QCOMPARE(server->count("PUT", "/v1/key/verifier"), 0);
        restart(a);
        QTest::qWait(50);
        QCOMPARE(server->count("GET", "/v1/key/verifier"), 1);
        QVERIFY(!a.account->readyVerifierMismatch());
    }

    void readyDeviceSetsTheVerifierOfAnOlderAccount()
    {
        Client a(*server);
        signIn(a, SyncAccount::Phase::Ready, QStringLiteral("A"));
        QTRY_COMPARE(server->count("GET", "/v1/key/verifier"), 1);
        // An account from before the verifier, and a device that has not checked it yet.
        server->accounts[kUser].readyVerifier.clear();
        a.settings->remove(QStringLiteral("Sync/ReadyVerifierChecked"));
        restart(a);
        QTRY_COMPARE(server->count("PUT", "/v1/key/verifier"), 1);
        QTRY_COMPARE(server->accounts.value(kUser).readyVerifier, SyncCrypto::readyVerifier(a.account->dataKey()));
        QVERIFY(!a.account->readyVerifierMismatch());
    }

    void pendingDeviceOfAnOlderAccountConfirmsWithoutProof()
    {
        Client a(*server);
        Client b(*server);
        QString code;
        twoDevices(a, b, &code);
        server->accounts[kUser].readyVerifier.clear(); // no verifier yet (A has not set it again)
        a.settings->setValue(QStringLiteral("Sync/ReadyVerifierChecked"), true);
        b.account->useRecoveryCode(code);
        QTRY_COMPARE(b.account->phase(), SyncAccount::Phase::Ready);
        QCOMPARE(server->count("DELETE", "/v1/key"), 1);
        QCOMPARE(server->count("POST", "/v1/key/ready"), 0);
        // Ready, B sets the verifier itself.
        QTRY_COMPARE(server->accounts.value(kUser).readyVerifier, SyncCrypto::readyVerifier(a.account->dataKey()));
    }

    void deviceLinkedWithoutTheKeyCannotDeclareItselfReady()
    {
        Client a(*server);
        signIn(a, SyncAccount::Phase::Ready, QStringLiteral("A"));
        // Someone linked a device of their own to the account (a phished sign-in code).
        QString intruder;
        server->addLinkedDevice(kUser, SyncCrypto::generateKeyPair().publicKey, false, &intruder, QStringLiteral("A"));
        const QString access = QStringLiteral("at_intruder");
        server->accessTokens.insert(access, intruder);
        QNetworkAccessManager nam;
        QNetworkRequest request(QUrl(server->base() + QStringLiteral("/v1/key")));
        request.setRawHeader("Authorization", "Bearer " + access.toLatin1());
        QNetworkReply *reply = nam.sendCustomRequest(request, "DELETE");
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QVERIFY(finished.wait(5000));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 403);
        QVERIFY(reply->readAll().contains("proof_required"));
        reply->deleteLater();
        QVERIFY(!server->device(intruder)->ready);
    }

    void keyThatIsNotTheAccountsIsDropped()
    {
        Client a(*server);
        Client b(*server);
        QString code;
        twoDevices(a, b, &code);
        // The account's verifier was made from another K than the one the recovery copy holds.
        server->accounts[kUser].readyVerifier = SyncCrypto::readyVerifier(SyncCrypto::newDataKey());
        QSignalSpy mismatch(b.account.get(), &SyncAccount::readyVerifierMismatchChanged);
        b.account->useRecoveryCode(code);
        QVERIFY(mismatch.wait(5000));
        QCOMPARE(mismatch.first().first().toBool(), true);
        QVERIFY(b.account->readyVerifierMismatch());
        QCOMPARE(b.account->phase(), SyncAccount::Phase::WaitingForKey);
        QVERIFY(!b.item().contains(QLatin1String("dataKey")));
        QCOMPARE(server->count("DELETE", "/v1/key"), 0);
        QCOMPARE(server->count("POST", "/v1/key/challenge"), 0);
        QVERIFY(!server->device(b.account->deviceId())->ready);
        b.account->signOutLocally();
        QVERIFY(!b.account->readyVerifierMismatch());
    }

    void readyDeviceReportsAVerifierThatIsNotItsOwn()
    {
        Client a(*server);
        signIn(a, SyncAccount::Phase::Ready, QStringLiteral("A"));
        QTRY_COMPARE(server->count("GET", "/v1/key/verifier"), 1);
        // A device that became ready before the verifier finds another one set (an intruder set it first).
        const QByteArray theirs = SyncCrypto::readyVerifier(SyncCrypto::newDataKey());
        server->accounts[kUser].readyVerifier = theirs;
        a.settings->remove(QStringLiteral("Sync/ReadyVerifierChecked"));
        restart(a);
        QTRY_VERIFY(a.account->readyVerifierMismatch());
        QCOMPARE(a.account->phase(), SyncAccount::Phase::Ready); // it keeps syncing
        QCOMPARE(server->count("PUT", "/v1/key/verifier"), 0);
        QCOMPARE(server->accounts.value(kUser).readyVerifier, theirs); // never replaced
        // Asked again at the next start: the warning stays.
        restart(a);
        QTRY_COMPARE(server->count("GET", "/v1/key/verifier"), 3);
        QTRY_VERIFY(a.account->readyVerifierMismatch());
    }

    void verifierSetFirstByAnotherDeviceIsReported()
    {
        Client a(*server);
        signIn(a, SyncAccount::Phase::Ready, QStringLiteral("A"));
        QTRY_COMPARE(server->count("GET", "/v1/key/verifier"), 1);
        server->accounts[kUser].readyVerifier.clear();
        a.settings->remove(QStringLiteral("Sync/ReadyVerifierChecked"));
        // Between A's read and A's write, another device sets a verifier of its own.
        const QByteArray theirs = SyncCrypto::readyVerifier(SyncCrypto::newDataKey());
        server->onRequest = [this, theirs](const FakeSyncServer::Request &r) {
            if (r.method == "PUT" && r.path == "/v1/key/verifier") {
                server->accounts[kUser].readyVerifier = theirs;
            }
        };
        restart(a);
        QTRY_VERIFY(a.account->readyVerifierMismatch());
        QCOMPARE(server->accounts.value(kUser).readyVerifier, theirs);
        QVERIFY(!a.settings->value(QStringLiteral("Sync/ReadyVerifierChecked")).toBool());
    }

    void spentNonceIsReplacedOnce()
    {
        Client a(*server);
        Client b(*server);
        QString code;
        twoDevices(a, b, &code);
        // The first nonce is replaced under B (another challenge, a lost answer).
        bool replaced = false;
        server->onRequest = [this, &replaced](const FakeSyncServer::Request &r) {
            if (!replaced && r.method == "POST" && r.path == "/v1/key/ready") {
                replaced = true;
                for (auto it = server->nonces.begin(); it != server->nonces.end(); ++it) {
                    it.value().fill(char(0x5a));
                }
            }
        };
        b.account->useRecoveryCode(code);
        QTRY_COMPARE(b.account->phase(), SyncAccount::Phase::Ready);
        QCOMPARE(server->count("POST", "/v1/key/challenge"), 2);
        QCOMPARE(server->count("POST", "/v1/key/ready"), 2);
        QVERIFY(!b.account->readyVerifierMismatch());
        server->onRequest = nullptr;
    }

    void proofIsBoundToTheDeviceAndAccount()
    {
        Client a(*server);
        Client b(*server);
        QString code;
        twoDevices(a, b, &code);
        b.account->useRecoveryCode(code);
        QTRY_COMPARE(b.account->phase(), SyncAccount::Phase::Ready);
        const auto proofs = server->of("POST", "/v1/key/ready");
        QCOMPARE(proofs.size(), 1);
        const QByteArray nonce = QByteArray::fromBase64(proofs.first().json().value(QLatin1String("nonce")).toString().toLatin1());
        const QByteArray signature = QByteArray::fromBase64(proofs.first().json().value(QLatin1String("signature")).toString().toLatin1());
        QCOMPARE(signature, SyncCrypto::signReady(a.account->dataKey(), nonce, b.account->deviceId(), kUser));
    }

    void aPollAnsweredLateStartsNoSecondConfirmation()
    {
        Client a(*server);
        Client b(*server);
        QString code;
        twoDevices(a, b, &code);
        // B's poll for a box is on its way when the recovery code opens K, and its answer (none
        // yet) comes back only after the confirmation began. Then the proof starts while the next
        // poll is planned, and the proof is slow: the planned poll must not start a second
        // confirmation beside it (two proofs spend each other's nonces).
        QByteArray holding = "GET /v1/key";
        server->holdIf = [&](const FakeSyncServer::Request &r) { return holding == r.method + ' ' + r.path; };
        QTRY_COMPARE(server->held(), 1);
        holding = "GET /v1/key/verifier";
        b.account->useRecoveryCode(code);
        QTRY_COMPARE(server->held(), 2);
        holding = "POST /v1/key/challenge";
        server->release(); // the poll's 404 first, then the verifier
        QTRY_COMPARE(server->held(), 1);
        QTest::qWait(kUnitMs * 20); // longer than the wait before the next poll
        holding.clear();
        server->release();
        QTRY_COMPARE(b.account->phase(), SyncAccount::Phase::Ready);
        QTest::qWait(kUnitMs * 20);
        server->holdIf = nullptr;
        QCOMPARE(server->count("POST", "/v1/key/challenge"), 1);
        QCOMPARE(server->count("POST", "/v1/key/ready"), 1);
    }

    void olderServerStillConfirmsWithoutProof()
    {
        server->oldServer = true;
        Client a(*server);
        Client b(*server);
        QString code;
        twoDevices(a, b, &code);
        QVERIFY(server->accounts.value(kUser).readyVerifier.isEmpty());
        b.account->useRecoveryCode(code);
        QTRY_COMPARE(b.account->phase(), SyncAccount::Phase::Ready);
        QCOMPARE(server->count("DELETE", "/v1/key"), 1);
        QCOMPARE(server->count("POST", "/v1/key/challenge"), 0);
        QTest::qWait(50);
        QCOMPARE(server->count("PUT", "/v1/key/verifier"), 0);
        QVERIFY(!a.account->readyVerifierMismatch());
        QVERIFY(!b.account->readyVerifierMismatch());
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

    void keysLeftByAKeyringThatRefusedToDeleteAreDeletedLater()
    {
        Client c(*server);
        signIn(c, SyncAccount::Phase::Ready);
        QVERIFY(c.store.secrets->contains(SyncSecrets::itemKey()));

        // The keyring refuses the deletion at sign-out: the keys stay, and this is remembered.
        *c.store.failWrites = true;
        c.account->signOutLocally();
        QVERIFY(!c.account->isLinked());
        QVERIFY(c.account->keyringWipePending());
        QVERIFY(c.store.secrets->contains(SyncSecrets::itemKey()));

        // Nothing changes while it still refuses (and the mark survives a restart)...
        c.build(*server);
        QVERIFY(c.account->keyringWipePending());
        c.account->retryKeyringWipe();
        QVERIFY(c.account->keyringWipePending());

        // ...and the next start that finds it working deletes them.
        *c.store.failWrites = false;
        c.account->retryKeyringWipe();
        QVERIFY(!c.account->keyringWipePending());
        QVERIFY(!c.store.secrets->contains(SyncSecrets::itemKey()));
    }

    void quitBeforeTheRegenerationAnswerSendsTheSameCodeAtTheNextStart()
    {
        Client c(*server);
        signIn(c, SyncAccount::Phase::Ready);
        QTRY_VERIFY(c.account->recoveryCodeShowable());
        const QString oldCode = c.account->recoveryCode();
        QSignalSpy retrying(c.account.get(), &SyncAccount::recoveryCodeRetrying);
        // The server stores the copy, its answer is lost, and Sugo quits before the next try.
        server->loseAnswers = 0;
        int puts = 0;
        server->onRequest = [&](const FakeSyncServer::Request &req) {
            if (req.method == "PUT" && req.path == "/v1/recovery" && ++puts == 1) {
                server->loseAnswers = 1;
            }
        };
        c.account->regenerateRecoveryCode();
        QVERIFY(retrying.wait(5000));
        server->onRequest = nullptr; // it holds this function's locals
        const QString kept = c.item().value(QLatin1String("pendingRecoveryCode")).toString();
        QVERIFY(!kept.isEmpty());
        QVERIFY(kept != oldCode);
        QVERIFY(SyncCrypto::openWithRecovery(server->accounts.value(kUser).recovery, *SyncCrypto::parseRecoveryCode(kept)));

        c.build(*server);
        QSignalSpy created(c.account.get(), &SyncAccount::recoveryCodeCreated);
        QCOMPARE(int(c.secrets->load()), int(SyncSecrets::LoadResult::Loaded));
        QVERIFY(c.account->restore());
        QVERIFY(created.wait(5000));
        QCOMPARE(created.first().first().toString(), kept);
        QCOMPARE(c.item().value(QLatin1String("recoveryCode")).toString(), kept);
        QVERIFY(!c.item().contains(QLatin1String("pendingRecoveryCode")));
        QVERIFY(SyncCrypto::openWithRecovery(server->accounts.value(kUser).recovery, *SyncCrypto::parseRecoveryCode(kept)));
        QTRY_VERIFY(c.account->recoveryCodeShowable());
        QCOMPARE(c.account->recoveryCode(), kept);
    }

    void refusedRegenerationForgetsTheKeptCode()
    {
        Client c(*server);
        signIn(c, SyncAccount::Phase::Ready);
        QTRY_VERIFY(c.account->recoveryCodeShowable());
        QSignalSpy failed(c.account.get(), &SyncAccount::recoveryCodeFailed);
        server->device(c.account->deviceId())->ready = false; // key_exists: refused for good
        c.account->regenerateRecoveryCode();
        QVERIFY(failed.wait(5000));
        QVERIFY(!c.item().contains(QLatin1String("pendingRecoveryCode")));
        server->device(c.account->deviceId())->ready = true;
    }

    void refusedCodeTheKeyringKeepsIsNeverSentAgain()
    {
        Client c(*server);
        signIn(c, SyncAccount::Phase::Ready);
        QTRY_VERIFY(c.account->recoveryCodeShowable());
        const QByteArray copy = server->accounts.value(kUser).recovery;
        QSignalSpy failed(c.account.get(), &SyncAccount::recoveryCodeFailed);
        server->device(c.account->deviceId())->ready = false; // key_exists: refused for good
        c.account->regenerateRecoveryCode(); // the pending code is in the keyring now
        *c.store.failWrites = true;          // and its removal is refused
        QVERIFY(failed.wait(5000));
        QVERIFY(c.item().contains(QLatin1String("pendingRecoveryCode")));
        server->device(c.account->deviceId())->ready = true;

        // The next start removes it instead of sending it.
        *c.store.failWrites = false;
        const int puts = server->count("PUT", "/v1/recovery");
        c.build(*server);
        QSignalSpy created(c.account.get(), &SyncAccount::recoveryCodeCreated);
        QCOMPARE(int(c.secrets->load()), int(SyncSecrets::LoadResult::Loaded));
        QVERIFY(c.account->restore());
        QTRY_COMPARE(c.account->phase(), SyncAccount::Phase::Ready);
        QTRY_VERIFY(!c.item().contains(QLatin1String("pendingRecoveryCode")));
        QTest::qWait(30 * kUnitMs);
        QCOMPARE(server->count("PUT", "/v1/recovery"), puts);
        QCOMPARE(created.size(), 0);
        QCOMPARE(server->accounts.value(kUser).recovery, copy);
    }

    void lostRegenerationAnswerSendsTheSameCodeAgain()
    {
        Client c(*server);
        signIn(c, SyncAccount::Phase::Ready);
        QTRY_VERIFY(c.account->recoveryCodeShowable());
        const QString oldCode = c.account->recoveryCode();
        QSignalSpy created(c.account.get(), &SyncAccount::recoveryCodeCreated);
        QSignalSpy retrying(c.account.get(), &SyncAccount::recoveryCodeRetrying);
        QSignalSpy failed(c.account.get(), &SyncAccount::recoveryCodeFailed);

        // A server error, then a try the server stores but whose answer is lost. Before, the code
        // was dropped at the first failure, and a lost answer left the server with a copy nobody
        // could open.
        // The answer lost is the PUT's own, not that of a request the sign-in left on its way.
        int putsSeen = 0;
        int loseAtPut = 2;
        server->onRequest = [&](const FakeSyncServer::Request &req) {
            if (req.method == "PUT" && req.path == "/v1/recovery" && ++putsSeen == loseAtPut) {
                server->loseAnswers = 1;
            }
        };
        server->failures.append({"/v1/recovery", 503, "PUT"});
        const int puts = server->count("PUT", "/v1/recovery");
        c.account->regenerateRecoveryCode();
        QVERIFY(created.wait(5000));
        QCOMPARE(retrying.size(), 1);
        QCOMPARE(failed.size(), 0);

        // The same copy went three times, and its code is the one shown, stored and on the server.
        const auto sent = server->of("PUT", "/v1/recovery").mid(puts);
        QCOMPARE(sent.size(), 3);
        QCOMPARE(sent.at(1).body, sent.at(0).body);
        QCOMPARE(sent.at(2).body, sent.at(0).body);
        const QString newCode = created.first().first().toString();
        QVERIFY(newCode != oldCode);
        QVERIFY(SyncCrypto::openWithRecovery(server->accounts.value(kUser).recovery, *SyncCrypto::parseRecoveryCode(newCode)));
        QCOMPARE(c.item().value(QLatin1String("recoveryCode")).toString(), newCode);
        QTRY_VERIFY(c.account->recoveryCodeShowable());

        // A second regeneration is not started while one is on its way.
        loseAtPut = putsSeen + 1;
        c.account->regenerateRecoveryCode();
        c.account->regenerateRecoveryCode();
        QVERIFY(created.wait(5000));
        QCOMPARE(created.size(), 2);
        QTest::qWait(30 * kUnitMs);
        QCOMPARE(created.size(), 2);
        server->onRequest = nullptr; // it holds this function's locals
    }

    void unreadableRegenerationAnswerSendsTheSameCodeAgain()
    {
        Client c(*server);
        signIn(c, SyncAccount::Phase::Ready);
        QTRY_VERIFY(c.account->recoveryCodeShowable());
        QSignalSpy created(c.account.get(), &SyncAccount::recoveryCodeCreated);
        QSignalSpy failed(c.account.get(), &SyncAccount::recoveryCodeFailed);

        // The server stores the copy, but its answer cannot be read: the code is not dropped (the
        // server would hold a copy nobody can open), the same copy goes again.
        server->garbleAnswers = 1;
        const int puts = server->count("PUT", "/v1/recovery");
        c.account->regenerateRecoveryCode();
        QVERIFY(created.wait(5000));
        QCOMPARE(failed.size(), 0);
        const auto sent = server->of("PUT", "/v1/recovery").mid(puts);
        QCOMPARE(sent.size(), 2);
        QCOMPARE(sent.at(1).body, sent.at(0).body);
        const QString newCode = created.first().first().toString();
        QVERIFY(SyncCrypto::openWithRecovery(server->accounts.value(kUser).recovery, *SyncCrypto::parseRecoveryCode(newCode)));
    }

    void regenerationRefusedSaysWhetherTheOldCodeStillWorks()
    {
        Client c(*server);
        signIn(c, SyncAccount::Phase::Ready);
        QTRY_VERIFY(c.account->recoveryCodeShowable());
        QSignalSpy failed(c.account.get(), &SyncAccount::recoveryCodeFailed);
        QSignalSpy created(c.account.get(), &SyncAccount::recoveryCodeCreated);

        // Refused at once (key_exists, as to a device the server does not count as ready): the
        // server kept its copy, and the old code is shown again.
        const auto refuse = [&](bool on) { server->device(c.account->deviceId())->ready = !on; };
        refuse(true);
        c.account->regenerateRecoveryCode();
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.last().at(1).toBool(), true);
        refuse(false);
        QTRY_VERIFY(c.account->recoveryCodeShowable());

        // Refused after a try whose answer was lost: the server may hold a copy nobody can open.
        int puts = 0;
        server->onRequest = [&](const FakeSyncServer::Request &req) {
            if (req.method == "PUT" && req.path == "/v1/recovery") {
                if (++puts == 1) {
                    server->loseAnswers = 1;
                } else if (puts == 2) {
                    refuse(true);
                }
            }
        };
        c.account->regenerateRecoveryCode();
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.last().at(1).toBool(), false);
        QCOMPARE(created.size(), 0);
        QTest::qWait(30 * kUnitMs);
        QVERIFY(!c.account->recoveryCodeShowable());
        server->onRequest = nullptr; // it holds this function's locals
    }

    void signOutStopsTheRegeneration()
    {
        Client c(*server);
        signIn(c, SyncAccount::Phase::Ready);
        server->failNext("/v1/recovery", 503, "PUT");
        QSignalSpy retrying(c.account.get(), &SyncAccount::recoveryCodeRetrying);
        QSignalSpy created(c.account.get(), &SyncAccount::recoveryCodeCreated);
        c.account->regenerateRecoveryCode();
        QVERIFY(retrying.wait(5000));
        c.account->signOutLocally();
        const int puts = server->count("PUT", "/v1/recovery");
        QTest::qWait(60 * kUnitMs);
        QCOMPARE(server->count("PUT", "/v1/recovery"), puts);
        QCOMPARE(created.size(), 0);
    }

    void slowSignOutStopsTheRegeneration()
    {
        Client c(*server);
        signIn(c, SyncAccount::Phase::Ready);
        const QByteArray recovery = server->accounts.value(kUser).recovery;
        server->failNext("/v1/recovery", 503, "PUT");
        QSignalSpy retrying(c.account.get(), &SyncAccount::recoveryCodeRetrying);
        QSignalSpy created(c.account.get(), &SyncAccount::recoveryCodeCreated);
        QSignalSpy out(c.account.get(), &SyncAccount::signedOut);
        c.account->regenerateRecoveryCode();
        QVERIFY(retrying.wait(5000));

        // The disconnect takes longer than the next try's wait: no try goes out meanwhile.
        int putsDuringSignOut = -1;
        server->onRequest = [&](const FakeSyncServer::Request &req) {
            if (req.method == "DELETE" && putsDuringSignOut < 0) {
                const int before = server->count("PUT", "/v1/recovery");
                QTest::qWait(60 * kUnitMs);
                putsDuringSignOut = server->count("PUT", "/v1/recovery") - before;
            }
        };
        c.account->signOut();
        QVERIFY(out.wait(5000));
        server->onRequest = nullptr; // it holds this function's locals
        QCOMPARE(putsDuringSignOut, 0);
        QCOMPARE(created.size(), 0);
        QCOMPARE(server->accounts.value(kUser).recovery, recovery);
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
