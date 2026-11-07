// SPDX-License-Identifier: GPL-3.0-or-later
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "Database.h"
#include "DataManager.h"
#include "FakeNotifier.h"
#include "FakeSecretStore.h"
#include "FakeSyncServer.h"
#include "PresetModel.h"
#include "PresetPersistence.h"
#include "PresetRepository.h"
#include "PresetSyncFormat.h"
#include "SessionRepository.h"
#include "Settings.h"
#include "SyncAccount.h"
#include "SyncChannel.h"
#include "SyncController.h"
#include "SyncCrypto.h"
#include "SyncSecrets.h"

namespace
{
const QString kUser = QStringLiteral("usr_a");
const QString kOther = QStringLiteral("dev_other"); ///< A device that writes the blob behind the test's back.
constexpr int kUnitMs = 10; ///< One "second" of the controller in the tests.
const QByteArray kBlobPath = "/v1/blob/presets";

/// One computer: keyring, settings, database, timers and Sugo Sync.
struct Device {
    FakeSecretStore store;
    QTemporaryDir dir;
    Settings::Ptr settings;
    std::unique_ptr<Database> db;
    std::unique_ptr<PresetRepository> repo;
    std::unique_ptr<PresetModel> model;
    std::unique_ptr<PresetPersistence> persistence;
    std::unique_ptr<SyncController> sync;

    explicit Device(FakeSyncServer &server)
    {
        settings = Settings::open(dir.filePath(QStringLiteral("sugo.conf")));
        db = std::make_unique<Database>(dir.filePath(QStringLiteral("sugo.db")));
        const bool opened = db->open();
        Q_ASSERT(opened);
        Q_UNUSED(opened);
        build(server);
    }

    ~Device() { tearDown(); }

    void tearDown()
    {
        sync.reset();
        persistence.reset();
        model.reset();
        repo.reset();
    }

    /// New objects on the same keyring, settings and database, like a restart.
    void build(FakeSyncServer &server)
    {
        tearDown();
        repo = std::make_unique<PresetRepository>(db->connectionName());
        model = std::make_unique<PresetModel>();
        persistence = std::make_unique<PresetPersistence>(model.get(), repo.get());
        sync = std::make_unique<SyncController>(settings, &store, QUrl(server.base()));
        sync->setTimeUnitMs(kUnitMs);
        sync->attach(model.get(), persistence.get(), repo.get());
    }

    SyncAccount *account() const { return sync->account(); }
    QStringList names() const
    {
        QStringList out;
        for (const TimerPreset &p : model->presets()) {
            out.append(p.name);
        }
        return out;
    }
    int keyringReads() const { return int(store.accesses(SyncSecrets::itemKey(), FakeSecretStore::Access::Read).size()); }
};

/// A kind of data for the controller's generic part: one text, the newest write wins.
class TextChannel : public SyncChannel
{
public:
    explicit TextChannel(SyncKind kind)
        : m_kind(kind)
    {
    }
    SyncKind kind() const override { return m_kind; }
    bool ready() const override { return true; }
    QByteArray localData(const QString &, qsizetype maxBytes) override
    {
        lastMaxBytes = maxBytes;
        QJsonObject o{{QStringLiteral("format"), 1}, {QStringLiteral("text"), text}, {QStringLiteral("at"), at}};
        return QJsonDocument(o).toJson(QJsonDocument::Compact);
    }
    Merge merge(const QByteArray &plaintext, const QString &) override
    {
        Merge m;
        const QJsonObject o = QJsonDocument::fromJson(plaintext).object();
        if (o.value(QStringLiteral("format")).toInt() != 1) {
            m.result = Merge::Result::Unreadable;
            return m;
        }
        const qint64 remoteAt = o.value(QStringLiteral("at")).toInteger();
        if (remoteAt > at) {
            text = o.value(QStringLiteral("text")).toString();
            at = remoteAt;
        }
        m.changedRemote = at > remoteAt;
        return m;
    }
    /// A change made on this device.
    void edit(const QString &value)
    {
        text = value;
        ++at;
        notifyChanged();
    }

    QString text;
    qint64 at = 0;
    qsizetype lastMaxBytes = 0;

private:
    SyncKind m_kind;
};

TimerPreset remotePreset(const QString &uuid, const QString &name)
{
    TimerPreset p;
    p.uuid = uuid;
    p.name = name;
    p.createdAt = p.updatedAt = QDateTime::currentMSecsSinceEpoch();
    p.updatedBy = kOther;
    return p;
}
} // namespace

class SyncControllerTest : public QObject
{
    Q_OBJECT

    std::unique_ptr<FakeSyncServer> server;

    FakeSyncServer::Blob &blob() { return server->accounts[kUser].blobs["presets"]; }
    int puts() const { return server->count("PUT", kBlobPath); }
    int gets() const { return server->count("GET", kBlobPath); }

    /// What the server holds, opened with `d`'s key.
    PresetSyncFormat::ParseResult remote(const Device &d)
    {
        const SyncCrypto::OpenedBlob opened = SyncCrypto::decryptBlob(d.account()->dataKey(), kUser, blob().blob);
        return PresetSyncFormat::fromJson(opened.plaintext);
    }
    QStringList remoteNames(const Device &d)
    {
        QStringList out;
        for (const TimerPreset &p : remote(d).data.presets) {
            out.append(p.name);
        }
        return out;
    }

    /// Another device (kOther) uploads `data` (in `format`), with `d`'s key unless `userId` differs.
    void remoteWrite(const Device &d, const PresetSyncFormat::Data &data, int format = PresetSyncFormat::Format,
                     const QString &userId = kUser)
    {
        QByteArray json = PresetSyncFormat::toJson(data, kOther);
        if (format != PresetSyncFormat::Format) {
            QJsonObject o = QJsonDocument::fromJson(json).object();
            o.insert(QStringLiteral("format"), format);
            json = QJsonDocument(o).toJson(QJsonDocument::Compact);
        }
        blob().blob = SyncCrypto::encryptBlob(d.account()->dataKey(), userId, json);
        blob().etag = "r" + QByteArray::number(++m_remoteWrites);
    }
    int m_remoteWrites = 0;

    /// The current remote timers, as another device would read them.
    PresetSyncFormat::Data remoteData(const Device &d) { return remote(d).data; }

    /// Signs `d` in as the first device: it creates the key and uploads its timers.
    void signInFirst(Device &d, const QString &name = QStringLiteral("A"))
    {
        d.sync->signIn(name);
        QTRY_VERIFY(!d.sync->userCode().isEmpty());
        QCOMPARE(d.sync->state(), SyncController::SigningIn);
        QVERIFY(server->approve(d.sync->userCode(), kUser).startsWith(QLatin1String("dev_")));
        QTRY_COMPARE(d.sync->state(), SyncController::Idle);
        QTRY_VERIFY(!blob().blob.isEmpty());
        QTRY_COMPARE(d.sync->state(), SyncController::Idle);
    }

    /// Signs `b` in after `a`, which approves it.
    void signInSecond(Device &a, Device &b, const QString &name = QStringLiteral("B"))
    {
        b.sync->signIn(name);
        QTRY_VERIFY(!b.sync->userCode().isEmpty());
        QVERIFY(server->approve(b.sync->userCode(), kUser).startsWith(QLatin1String("dev_")));
        QTRY_COMPARE(b.sync->state(), SyncController::WaitingForApproval);
        QTRY_VERIFY(!b.sync->verificationCode().isEmpty());

        QSignalSpy asked(a.sync.get(), &SyncController::approvalRequested);
        a.sync->syncNow(); // the answer carries Sugo-Pending-Devices: 1
        QVERIFY(asked.wait(5000));
        const QVariantMap pending = a.sync->pendingApproval();
        QCOMPARE(pending.value(QStringLiteral("id")).toString(), b.account()->deviceId());
        QCOMPARE(pending.value(QStringLiteral("name")).toString(), name);
        QCOMPARE(pending.value(QStringLiteral("verificationCode")).toString(), b.sync->verificationCode());
        a.sync->approveDevice(pending.value(QStringLiteral("id")).toString());
        QVERIFY(a.sync->pendingApproval().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(b.sync->state(), SyncController::Idle, 5000);
    }

private Q_SLOTS:
    void init()
    {
        server = std::make_unique<FakeSyncServer>();
        server->interval = 1;
        m_remoteWrites = 0;
    }
    void cleanup() { server.reset(); }

    void firstDeviceUploadsWithIfNoneMatchStar()
    {
        Device a(*server);
        a.model->create({{QStringLiteral("name"), QStringLiteral("Before sign-in")}});
        QCOMPARE(a.sync->state(), SyncController::Off);
        signInFirst(a);

        // 404 (no blob yet), then the first upload with If-None-Match: *.
        const auto get = server->of("GET", kBlobPath);
        QCOMPARE(get.size(), 1);
        QVERIFY(!get.first().headers.contains("if-none-match"));
        const auto put = server->of("PUT", kBlobPath);
        QCOMPARE(put.size(), 1);
        QCOMPARE(put.first().headers.value("if-none-match"), QByteArray("*"));
        QVERIFY(!put.first().headers.contains("if-match"));
        QVERIFY(remoteNames(a).contains(QStringLiteral("Before sign-in")));
        QCOMPARE(a.settings->value(QStringLiteral("Sync/Etag")).toByteArray(), blob().etag);
        QVERIFY(a.sync->lastSync().isValid());
    }

    void pushIsDebounced()
    {
        Device a(*server);
        signInFirst(a);
        const int before = puts();
        const QByteArray etag = blob().etag;

        const QString uuid = a.model->create({{QStringLiteral("name"), QStringLiteral("Study")}});
        a.model->update(uuid, {{QStringLiteral("workSeconds"), 50 * 60}});
        a.model->update(uuid, {{QStringLiteral("name"), QStringLiteral("Deep study")}});
        QTest::qWait(kUnitMs);
        QCOMPARE(puts(), before); // waits 3 s after the last save
        QTRY_COMPARE(puts(), before + 1);
        QTest::qWait(10 * kUnitMs);
        QCOMPARE(puts(), before + 1); // one upload for the three changes

        // If-Match with the ETag of the last upload.
        QCOMPARE(server->of("PUT", kBlobPath).last().headers.value("if-match"), QByteArray('"' + etag + '"'));
        QVERIFY(remoteNames(a).contains(QStringLiteral("Deep study")));
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        QVERIFY(!a.settings->contains(QStringLiteral("Sync/Dirty")));
    }

    void blobOfAnEarlierEpochIsReplaced()
    {
        // An upload that outlived a "Start over" left a blob of the old key: the server counts it
        // as none (GET 404, If-Match 412 with etag null), and If-None-Match: * replaces it.
        Device a(*server);
        signInFirst(a);
        blob().stale = true;
        const int before = puts();

        a.model->create({{QStringLiteral("name"), QStringLiteral("After the reset")}});
        QTRY_VERIFY(!blob().stale);
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        const auto put = server->of("PUT", kBlobPath);
        QCOMPARE(put.size(), before + 2);
        QVERIFY(put.at(before).headers.contains("if-match"));     // 412, etag null
        QCOMPARE(put.last().headers.value("if-none-match"), QByteArray("*")); // after the GET's 404
        QVERIFY(!put.last().headers.contains("if-match"));
        QVERIFY(remoteNames(a).contains(QStringLiteral("After the reset")));
        QCOMPARE(a.settings->value(QStringLiteral("Sync/Etag")).toByteArray(), blob().etag);
        QCOMPARE(a.sync->errorKind(), SyncController::NoError);
        QVERIFY(!a.settings->contains(QStringLiteral("Sync/Dirty")));
    }

    void plusRequiredKeepsTheTimersSyncing()
    {
        Device a(*server);
        auto *sessions = new TextChannel(SyncKind::Sessions);
        a.sync->addChannel(std::unique_ptr<SyncChannel>(sessions));
        QSignalSpy availability(a.sync.get(), &SyncController::availabilityChanged);
        signInFirst(a);
        QTRY_COMPARE(a.sync->availability(SyncKind::Sessions), SyncController::PlusRequired);
        QCOMPARE(a.sync->availability(SyncKind::Presets), SyncController::Available);
        QCOMPARE(availability.size(), 1);
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        QCOMPARE(a.sync->errorKind(), SyncController::NoError);
        QCOMPARE(server->count("GET", "/v1/blob/sessions"), 1);
        QCOMPARE(server->count("PUT", "/v1/blob/sessions"), 0);

        // Changes wait, the timers go on, and the kind is not asked about again by itself.
        sessions->edit(QStringLiteral("Read a chapter"));
        const int before = puts();
        a.model->create({{QStringLiteral("name"), QStringLiteral("Plus or not")}});
        QTRY_COMPARE(puts(), before + 1);
        QTest::qWait(70 * kUnitMs);
        a.sync->windowActivated();
        QTRY_COMPARE(gets(), 3);
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        QCOMPARE(server->count("GET", "/v1/blob/sessions"), 1);
        QCOMPARE(server->count("PUT", "/v1/blob/sessions"), 0);
        QVERIFY(a.settings->value(QStringLiteral("Sync/sessions/Dirty")).toBool());

        // Remembered over a restart.
        a.build(*server);
        auto *again = new TextChannel(SyncKind::Sessions);
        again->text = sessions->text;
        again->at = sessions->at;
        sessions = again;
        a.sync->addChannel(std::unique_ptr<SyncChannel>(sessions));
        QCOMPARE(a.sync->availability(SyncKind::Sessions), SyncController::PlusRequired);
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        QCOMPARE(server->count("GET", "/v1/blob/sessions"), 1);

        // With Plus, Sync Now asks again and uploads what waited.
        server->accounts[kUser].plus = true;
        a.sync->syncNow();
        QTRY_COMPARE(a.sync->availability(SyncKind::Sessions), SyncController::Available);
        QTRY_COMPARE(server->count("PUT", "/v1/blob/sessions"), 1);
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        const SyncCrypto::OpenedBlob opened =
            SyncCrypto::decryptBlob(a.account()->dataKey(), kUser, server->accounts[kUser].blobs["sessions"].blob, QStringLiteral("sessions"));
        QVERIFY(opened.ok());
        QVERIFY(opened.plaintext.contains("Read a chapter"));
        QVERIFY(!a.settings->contains(QStringLiteral("Sync/sessions/Dirty")));
        QVERIFY(!a.settings->contains(QStringLiteral("Sync/sessions/Unavailable")));
        QCOMPARE(sessions->lastMaxBytes, qsizetype(1048576) - SyncKinds::BlobOverhead);
    }

    void unknownKindIsNotRetriedAtEveryPull()
    {
        // A Worker older than the kind answers 404 to the download and to the first upload.
        server->blobKinds.remove("settings");
        server->accounts[kUser].plus = true;
        Device a(*server);
        a.sync->addChannel(std::make_unique<TextChannel>(SyncKind::Settings));
        signInFirst(a);
        QTRY_COMPARE(a.sync->availability(SyncKind::Settings), SyncController::Unavailable);
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        QCOMPARE(a.sync->errorKind(), SyncController::NoError);
        QCOMPARE(server->count("GET", "/v1/blob/settings"), 1);
        QCOMPARE(server->count("PUT", "/v1/blob/settings"), 1);

        QTest::qWait(70 * kUnitMs);
        a.sync->windowActivated();
        QTRY_COMPARE(gets(), 2);
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        QCOMPARE(server->count("GET", "/v1/blob/settings"), 1);

        // A day later it is asked about again, and the newer server takes it.
        server->blobKinds.insert("settings", 8192);
        a.settings->setValue(QStringLiteral("Sync/settings/CheckedAt"), QDateTime::currentDateTimeUtc().addSecs(-2000));
        QTest::qWait(70 * kUnitMs);
        a.sync->windowActivated();
        QTRY_COMPARE(a.sync->availability(SyncKind::Settings), SyncController::Available);
        QTRY_COMPARE(server->count("PUT", "/v1/blob/settings"), 2);
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
    }

    void overTheLimitIsNotSent()
    {
        server->accounts[kUser].plus = true;
        Device a(*server);
        auto *settings = new TextChannel(SyncKind::Settings);
        a.sync->addChannel(std::unique_ptr<SyncChannel>(settings));
        signInFirst(a);
        QTRY_COMPARE(server->count("PUT", "/v1/blob/settings"), 1);
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);

        // Over 8 KB: refused here, before any upload, and the timers still sync.
        settings->edit(QString(9000, QLatin1Char('x')));
        QTRY_COMPARE(a.sync->errorKind(), SyncController::TooLarge);
        QCOMPARE(a.sync->statusText(), QStringLiteral("The settings are too large to sync."));
        QVERIFY(a.sync->errorTexts().contains(a.sync->statusText()));
        QCOMPARE(server->count("PUT", "/v1/blob/settings"), 1);
        const int before = puts();
        a.model->create({{QStringLiteral("name"), QStringLiteral("Still syncing")}});
        QTRY_COMPARE(puts(), before + 1);
        QTRY_VERIFY(a.sync->state() != SyncController::Syncing);
        // A download does not hide it.
        a.sync->syncNow();
        QTRY_COMPARE(server->count("GET", "/v1/blob/settings"), 2);
        QTRY_VERIFY(a.sync->state() != SyncController::Syncing);
        QCOMPARE(a.sync->errorKind(), SyncController::TooLarge);
        QCOMPARE(server->count("PUT", "/v1/blob/settings"), 1);

        // A change that fits goes up and clears it.
        settings->edit(QStringLiteral("small"));
        QTRY_COMPARE(server->count("PUT", "/v1/blob/settings"), 2);
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        QCOMPARE(a.sync->errorKind(), SyncController::NoError);
    }

    void tooLargeFromTheServerIsNotRetried()
    {
        server->accounts[kUser].plus = true;
        server->blobKinds["sessions"] = 100; // a server with a smaller limit than this Sugo knows
        Device a(*server);
        auto *sessions = new TextChannel(SyncKind::Sessions);
        a.sync->addChannel(std::unique_ptr<SyncChannel>(sessions));
        sessions->text = QString(200, QLatin1Char('y'));
        a.sync->signIn(QStringLiteral("A"));
        QTRY_VERIFY(!a.sync->userCode().isEmpty());
        QVERIFY(server->approve(a.sync->userCode(), kUser).startsWith(QLatin1String("dev_")));
        QTRY_COMPARE(a.sync->errorKind(), SyncController::TooLarge);
        QVERIFY(!blob().blob.isEmpty()); // the timers went up
        QCOMPARE(a.sync->state(), SyncController::Error);
        QCOMPARE(a.sync->statusText(), QStringLiteral("The session history is too large to sync."));
        QCOMPARE(server->count("PUT", "/v1/blob/sessions"), 1);
        // Not at the next downloads either (each 404 asks for a first upload).
        a.sync->syncNow();
        QTRY_COMPARE(server->count("GET", "/v1/blob/sessions"), 2);
        QTest::qWait(400 * kUnitMs); // past a periodic pull
        QVERIFY(server->count("GET", "/v1/blob/sessions") >= 3);
        QCOMPARE(server->count("PUT", "/v1/blob/sessions"), 1);
        QCOMPARE(a.sync->errorKind(), SyncController::TooLarge);

        // A change here tries again.
        sessions->edit(QStringLiteral("short"));
        QTRY_COMPARE(server->count("PUT", "/v1/blob/sessions"), 2);
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
    }

    void notModified()
    {
        Device a(*server);
        signInFirst(a);
        const int before = puts();
        const QByteArray etag = blob().etag;
        QSignalSpy lastSync(a.sync.get(), &SyncController::lastSyncChanged);

        a.sync->syncNow();
        QVERIFY(lastSync.wait(5000));
        QCOMPARE(server->of("GET", kBlobPath).last().headers.value("if-none-match"), QByteArray('"' + etag + '"'));
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        QTest::qWait(5 * kUnitMs);
        QCOMPARE(puts(), before);
    }

    void windowActivationPullsAtMostOnceAMinute()
    {
        Device a(*server);
        signInFirst(a);
        const int pulled = gets();
        a.sync->syncNow();
        QTRY_COMPARE(gets(), pulled + 1);
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        const int before = gets();
        a.sync->windowActivated(); // the last pull was just now
        QTest::qWait(5 * kUnitMs);
        QCOMPARE(gets(), before);
        QTest::qWait(60 * kUnitMs);
        a.sync->windowActivated();
        QTRY_COMPARE(gets(), before + 1);
    }

    void conflictMergesAndRetries()
    {
        Device a(*server);
        signInFirst(a);
        const int before = puts();

        // Another device uploads while this one sends its change: 412.
        server->onRequest = [&](const FakeSyncServer::Request &r) {
            if (r.method == "PUT" && r.path == kBlobPath) {
                PresetSyncFormat::Data data = remoteData(a);
                data.presets.append(remotePreset(QStringLiteral("remote-1"), QStringLiteral("From elsewhere")));
                remoteWrite(a, data);
                server->onRequest = nullptr;
            }
        };
        a.model->create({{QStringLiteral("name"), QStringLiteral("Local")}});
        QTRY_COMPARE(puts(), before + 2);
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);

        // Downloaded without If-None-Match, merged, applied, sent again with the new ETag.
        const auto get = server->of("GET", kBlobPath).last();
        QVERIFY(!get.headers.contains("if-none-match"));
        QCOMPARE(server->of("PUT", kBlobPath).last().headers.value("if-match"), QByteArray("\"r1\""));
        QVERIFY(a.names().contains(QStringLiteral("From elsewhere")));
        QVERIFY(a.names().contains(QStringLiteral("Local")));
        QVERIFY(remoteNames(a).contains(QStringLiteral("From elsewhere")));
        QVERIFY(remoteNames(a).contains(QStringLiteral("Local")));
        QCOMPARE(a.sync->errorKind(), SyncController::NoError);
    }

    void conflictStopsAfterThreeTries()
    {
        Device a(*server);
        signInFirst(a);
        const int before = puts();
        int n = 0;
        server->onRequest = [&](const FakeSyncServer::Request &r) {
            if (r.method == "PUT" && r.path == kBlobPath) {
                PresetSyncFormat::Data data = remoteData(a);
                ++n;
                data.presets.append(remotePreset(QStringLiteral("remote-%1").arg(n), QStringLiteral("Busy %1").arg(n)));
                remoteWrite(a, data);
            }
        };
        a.model->create({{QStringLiteral("name"), QStringLiteral("Local")}});
        QTRY_COMPARE(a.sync->state(), SyncController::Error);
        QCOMPARE(a.sync->errorKind(), SyncController::Server);
        QVERIFY(SyncController::errorTexts().contains(a.sync->statusText())); // room kept for it
        QCOMPARE(puts(), before + 3);
        QTest::qWait(30 * kUnitMs);
        QCOMPARE(puts(), before + 3); // no loop: again in 5 minutes

        server->onRequest = nullptr;
        QTRY_COMPARE_WITH_TIMEOUT(puts(), before + 4, 10000);
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        QVERIFY(remoteNames(a).contains(QStringLiteral("Local")));
        QVERIFY(remoteNames(a).contains(QStringLiteral("Busy 3")));
    }

    void futureFormatIsNeverOverwritten_data()
    {
        QTest::addColumn<bool>("blobFormat");
        QTest::newRow("blob") << true;
        QTest::newRow("timers") << false;
    }
    void futureFormatIsNeverOverwritten()
    {
        QFETCH(bool, blobFormat);
        Device a(*server);
        signInFirst(a);
        PresetSyncFormat::Data data = remoteData(a);
        data.presets.append(remotePreset(QStringLiteral("remote-1"), QStringLiteral("From the future")));
        remoteWrite(a, data, blobFormat ? PresetSyncFormat::Format : PresetSyncFormat::Format + 1);
        if (blobFormat) {
            blob().blob[4] = char(SyncCrypto::BlobFormat + 1);
        }
        const QByteArray written = blob().blob;
        const int before = puts();

        a.sync->syncNow();
        QTRY_COMPARE(a.sync->state(), SyncController::Error);
        QCOMPARE(a.sync->errorKind(), SyncController::FormatTooNew);
        // A newer format of the timers is read (never written); a newer blob cannot be.
        QCOMPARE(a.names().contains(QStringLiteral("From the future")), !blobFormat);

        a.model->create({{QStringLiteral("name"), QStringLiteral("Local")}});
        QTest::qWait(20 * kUnitMs);
        a.sync->syncNow();
        QTest::qWait(20 * kUnitMs);
        QCOMPARE(puts(), before);
        QCOMPARE(blob().blob, written);
        QCOMPARE(a.sync->errorKind(), SyncController::FormatTooNew);
    }

    void futureFormatKeepsLocalChangesForTheUpdate()
    {
        Device a(*server);
        signInFirst(a);
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);

        // A change made here meets a blob that a newer Sugo wrote meanwhile.
        a.model->create({{QStringLiteral("name"), QStringLiteral("Local")}});
        PresetSyncFormat::Data data = remoteData(a);
        data.presets.append(remotePreset(QStringLiteral("remote-1"), QStringLiteral("From the future")));
        remoteWrite(a, data, PresetSyncFormat::Format + 1);
        const QByteArray newerEtag = blob().etag;
        const QByteArray written = blob().blob;
        QTRY_COMPARE(a.sync->errorKind(), SyncController::FormatTooNew);
        QTRY_VERIFY(a.names().contains(QStringLiteral("From the future")));
        QTest::qWait(20 * kUnitMs);
        QCOMPARE(blob().blob, written); // its upload, if one was on the way, met a 412

        // The server does not have "Local": it still waits to go out, and the next download
        // must not be a 304.
        QVERIFY(a.settings->value(QStringLiteral("Sync/Dirty")).toBool());
        QVERIFY(a.settings->value(QStringLiteral("Sync/Etag")).toByteArray() != newerEtag);

        // Sugo is updated: it reads the same blob (same ETag) in its own format, and uploads.
        remoteWrite(a, data);
        blob().etag = newerEtag;
        a.build(*server);
        a.sync->syncNow();
        QTRY_VERIFY(remoteNames(a).contains(QStringLiteral("Local")));
        QVERIFY(remoteNames(a).contains(QStringLiteral("From the future")));
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        QVERIFY(!a.settings->contains(QStringLiteral("Sync/Dirty")));
    }

    void blobOfAnotherAccountIsUnreadable()
    {
        Device a(*server);
        signInFirst(a);
        PresetSyncFormat::Data data = remoteData(a);
        data.presets.append(remotePreset(QStringLiteral("remote-1"), QStringLiteral("Foreign")));
        remoteWrite(a, data, PresetSyncFormat::Format, QStringLiteral("usr_other")); // wrong associated data
        const QByteArray written = blob().blob;
        const int before = puts();
        const QStringList names = a.names();

        a.sync->syncNow();
        QTRY_COMPARE(a.sync->state(), SyncController::Error);
        QCOMPARE(a.sync->errorKind(), SyncController::UnreadableData);
        QCOMPARE(a.names(), names);

        a.model->create({{QStringLiteral("name"), QStringLiteral("Local")}});
        QTest::qWait(20 * kUnitMs);
        QCOMPARE(puts(), before);
        QCOMPARE(blob().blob, written);
    }

    void tokenExpiresDuringSync()
    {
        Device a(*server);
        signInFirst(a);
        const int refreshes = server->count("POST", "/v1/auth/token");
        const int reads = a.keyringReads();
        server->onRequest = [&](const FakeSyncServer::Request &r) {
            if (r.method == "GET" && r.path == kBlobPath) {
                server->expireAccessTokens(); // the hour ends with this request
                server->onRequest = nullptr;
            }
        };
        PresetSyncFormat::Data data = remoteData(a);
        data.presets.append(remotePreset(QStringLiteral("remote-1"), QStringLiteral("From elsewhere")));
        remoteWrite(a, data);
        a.model->create({{QStringLiteral("name"), QStringLiteral("Local")}});
        a.sync->syncNow();
        QTRY_VERIFY(remoteNames(a).contains(QStringLiteral("Local")));
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        QVERIFY(a.names().contains(QStringLiteral("From elsewhere")));
        QCOMPARE(server->count("POST", "/v1/auth/token"), refreshes + 1);
        QCOMPARE(a.keyringReads(), reads); // the new token is written, never read back
    }

    void unreachableServerThenRecovery()
    {
        Device a(*server);
        signInFirst(a);
        server->offline = true;
        a.model->create({{QStringLiteral("name"), QStringLiteral("Offline edit")}});
        QTRY_COMPARE(a.sync->state(), SyncController::Error);
        QCOMPARE(a.sync->errorKind(), SyncController::Network);
        const int tries = server->requests.size();
        QTest::qWait(20 * kUnitMs);
        QCOMPARE(server->requests.size(), tries); // the next try is a minute away

        // The program keeps working meanwhile.
        a.model->create({{QStringLiteral("name"), QStringLiteral("Second offline edit")}});
        QVERIFY(a.names().contains(QStringLiteral("Second offline edit")));

        server->offline = false;
        QTRY_COMPARE_WITH_TIMEOUT(a.sync->state(), SyncController::Idle, 5000);
        QCOMPARE(a.sync->errorKind(), SyncController::NoError);
        QTRY_VERIFY(remoteNames(a).contains(QStringLiteral("Second offline edit")));
        QVERIFY(remoteNames(a).contains(QStringLiteral("Offline edit")));
    }

    void hugeRetryAfterDoesNotRetryAtOnce()
    {
        Device a(*server);
        signInFirst(a);
        const int before = puts();
        // 429496730 "seconds" of kUnitMs is 2^32 + 4 ms: a timer that took it as an int would fire
        // after 4 ms, and hammer a server that asked to be left alone.
        server->retryAfter = "429496730";
        server->failNext(kBlobPath, 429, "PUT");
        a.model->create({{QStringLiteral("name"), QStringLiteral("Rate limited")}});
        QTRY_COMPARE(a.sync->state(), SyncController::Error);
        QCOMPARE(a.sync->errorKind(), SyncController::RateLimited);
        QCOMPARE(puts(), before + 1);
        QTest::qWait(30 * kUnitMs);
        QCOMPARE(puts(), before + 1); // the next try waits for the server (at most an hour)
        QCOMPARE(a.sync->state(), SyncController::Error);
    }

    void tooLargeErrorSurvivesPullsUntilAnUploadSucceeds()
    {
        Device a(*server);
        signInFirst(a);

        // The server refuses the upload: the timers are too large (413).
        server->maxBlobSize = 1;
        const int putsBefore = puts();
        const QString focus = a.model->create({{QStringLiteral("name"), QStringLiteral("Deep focus")}});
        QTRY_COMPARE(a.sync->state(), SyncController::Error);
        QCOMPARE(a.sync->errorKind(), SyncController::TooLarge);
        QCOMPARE(puts(), putsBefore + 1);
        QVERIFY(!remoteNames(a).contains(QStringLiteral("Deep focus"))); // still not uploaded

        // A pull that brings nothing (304, as every 5 minutes) must not clear the error.
        const QDateTime lastSyncBefore = a.sync->lastSync();
        a.sync->syncNow();
        QTRY_VERIFY(a.sync->lastSync() > lastSyncBefore); // the 304 was handled
        QCOMPARE(a.sync->state(), SyncController::Error);
        QCOMPARE(a.sync->errorKind(), SyncController::TooLarge);

        // An upload the server accepts clears the error again.
        server->maxBlobSize = 262144;
        a.model->update(focus, {{QStringLiteral("name"), QStringLiteral("Deeper focus")}});
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        QTRY_COMPARE(a.sync->errorKind(), SyncController::NoError);
        QTRY_VERIFY(remoteNames(a).contains(QStringLiteral("Deeper focus")));
    }

    void mergeEqualToRemoteIsNotUploaded()
    {
        Device a(*server);
        signInFirst(a);
        PresetSyncFormat::Data data = remoteData(a);
        data.presets.append(remotePreset(QStringLiteral("remote-1"), QStringLiteral("From elsewhere")));
        remoteWrite(a, data);
        const int before = puts();

        a.sync->syncNow();
        QTRY_VERIFY(a.names().contains(QStringLiteral("From elsewhere")));
        QTest::qWait(20 * kUnitMs); // longer than the push delay
        QCOMPARE(puts(), before);
        QTRY_COMPARE(a.sync->state(), SyncController::Idle); // (a periodic pull may be running)
        QCOMPARE(puts(), before);
        QCOMPARE(a.repo->loadAll().size(), a.model->count()); // applied and stored
    }

    void mergeThatCannotBeSavedIsDownloadedAgain()
    {
        Device a(*server);
        signInFirst(a);
        const QString doomed = a.model->create({{QStringLiteral("name"), QStringLiteral("Doomed")}});
        const QString kept = a.model->create({{QStringLiteral("name"), QStringLiteral("Kept")}});
        QTRY_VERIFY(remoteNames(a).contains(QStringLiteral("Kept")));
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);

        // Another device deletes one timer and renames the other.
        PresetSyncFormat::Data data = remoteData(a);
        data.presets.removeIf([&doomed](const TimerPreset &p) { return p.uuid == doomed; });
        const qint64 later = QDateTime::currentMSecsSinceEpoch() + 1000;
        for (TimerPreset &p : data.presets) {
            if (p.uuid == kept) {
                p.name = QStringLiteral("Renamed elsewhere");
                p.updatedAt = later;
                p.updatedBy = kOther;
            }
        }
        data.tombstones.append({doomed, later, kOther});
        remoteWrite(a, data);

        // The database refuses every write (disk full, locked): the merge never reaches it.
        auto setQueryOnly = [&a](bool on) {
            QSqlQuery query(QSqlDatabase::database(a.db->connectionName()));
            return query.exec(on ? QStringLiteral("PRAGMA query_only = ON") : QStringLiteral("PRAGMA query_only = OFF"));
        };
        QVERIFY(setQueryOnly(true));
        a.sync->syncNow();
        QTRY_VERIFY(!a.names().contains(QStringLiteral("Doomed")));
        QTRY_VERIFY(a.persistence->isSaveFailing());
        QTRY_COMPARE(a.sync->state(), SyncController::Error);
        QCOMPARE(a.sync->errorKind(), SyncController::LocalSaveFailed);

        // A restart finds the old timers in the database.
        QVERIFY(setQueryOnly(false));
        a.build(*server);
        QVERIFY(a.names().contains(QStringLiteral("Doomed")));
        QVERIFY(a.names().contains(QStringLiteral("Kept")));

        // The blob is downloaded again (not 304) and merged once more.
        QTRY_VERIFY_WITH_TIMEOUT(!a.names().contains(QStringLiteral("Doomed")), 5000);
        QTRY_VERIFY(a.names().contains(QStringLiteral("Renamed elsewhere")));
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);

        // A later edit here keeps the other device's changes on the server.
        a.model->create({{QStringLiteral("name"), QStringLiteral("New here")}});
        QTRY_VERIFY(remoteNames(a).contains(QStringLiteral("New here")));
        QVERIFY(!remoteNames(a).contains(QStringLiteral("Doomed")));
        QVERIFY(remoteNames(a).contains(QStringLiteral("Renamed elsewhere")));
        QVERIFY(!remoteNames(a).contains(QStringLiteral("Kept")));
    }

    void selectedTimerDeletedElsewhere()
    {
        Device a(*server);
        signInFirst(a);
        const QString uuid = a.model->create({{QStringLiteral("name"), QStringLiteral("Doomed")}});
        a.model->setCurrentUuid(uuid);
        QTRY_VERIFY(remoteNames(a).contains(QStringLiteral("Doomed")));
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);

        PresetSyncFormat::Data data = remoteData(a);
        data.presets.removeIf([&uuid](const TimerPreset &p) { return p.uuid == uuid; });
        data.tombstones.append({uuid, QDateTime::currentMSecsSinceEpoch() + 1000, kOther});
        remoteWrite(a, data);
        const int before = puts();

        a.sync->syncNow();
        QTRY_COMPARE(a.model->currentUuid(), PresetModel::DefaultUuid);
        QVERIFY(!a.names().contains(QStringLiteral("Doomed")));
        QCOMPARE(a.repo->currentUuid(), PresetModel::DefaultUuid);
        bool remembered = false;
        for (const PresetTombstone &t : a.repo->tombstones()) {
            remembered = remembered || (t.uuid == uuid && t.deletedBy == kOther);
        }
        QVERIFY(remembered);
        QTest::qWait(20 * kUnitMs);
        QCOMPARE(puts(), before);
    }

    void selectedTimerDeletedElsewhereWaitsForTheTimer()
    {
        Device a(*server);
        bool busy = false;
        a.sync->setTimerBusy([&busy]() { return busy; });
        signInFirst(a);
        const QString uuid = a.model->create({{QStringLiteral("name"), QStringLiteral("Running")}});
        a.model->setCurrentUuid(uuid);
        QTRY_VERIFY(remoteNames(a).contains(QStringLiteral("Running")));
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        const QByteArray etag = a.settings->value(QStringLiteral("Sync/Etag")).toByteArray();

        // A phase runs here while another device deletes its timer.
        busy = true;
        PresetSyncFormat::Data data = remoteData(a);
        data.presets.removeIf([&uuid](const TimerPreset &p) { return p.uuid == uuid; });
        data.tombstones.append({uuid, QDateTime::currentMSecsSinceEpoch() + 1000, kOther});
        remoteWrite(a, data);
        const int pulled = gets();
        const int before = puts();

        a.sync->syncNow();
        QTRY_COMPARE(gets(), pulled + 1);
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        QCOMPARE(a.model->currentUuid(), uuid);
        QVERIFY(a.names().contains(QStringLiteral("Running")));
        QCOMPARE(a.settings->value(QStringLiteral("Sync/Etag")).toByteArray(), etag);

        // A change made meanwhile waits too: an upload would only meet the newer blob.
        a.model->create({{QStringLiteral("name"), QStringLiteral("Made while running")}});
        QTest::qWait(20 * kUnitMs); // longer than the push delay
        QCOMPARE(puts(), before);
        QVERIFY(a.settings->value(QStringLiteral("Sync/Dirty")).toBool());
        QCOMPARE(a.model->currentUuid(), uuid);

        // The timer stops: the deletion arrives, and the local change goes out with it.
        busy = false;
        a.sync->timerIdle();
        QTRY_COMPARE(a.model->currentUuid(), PresetModel::DefaultUuid);
        QVERIFY(!a.names().contains(QStringLiteral("Running")));
        QVERIFY(a.names().contains(QStringLiteral("Made while running")));
        QTRY_VERIFY(remoteNames(a).contains(QStringLiteral("Made while running")));
        QVERIFY(!remoteNames(a).contains(QStringLiteral("Running")));
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        QCOMPARE(a.settings->value(QStringLiteral("Sync/Etag")).toByteArray(), blob().etag);
        QVERIFY(!a.settings->contains(QStringLiteral("Sync/Dirty")));
    }

    void restoredBackupIsPublished()
    {
        Device a(*server);
        signInFirst(a);
        const QString uuid = a.model->create({{QStringLiteral("name"), QStringLiteral("Original")}});
        QTRY_VERIFY(remoteNames(a).contains(QStringLiteral("Original")));
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);

        SessionRepository sessions(a.db->connectionName());
        DataManager manager(a.db.get(), &sessions, a.repo.get(), a.model.get(), nullptr);
        manager.setPresetPersistence(a.persistence.get());
        const QUrl backup = QUrl::fromLocalFile(a.dir.filePath(QStringLiteral("backup.db")));
        QVERIFY(manager.backupDatabase(backup));

        a.model->update(uuid, {{QStringLiteral("name"), QStringLiteral("Edited later")}});
        QTRY_VERIFY(remoteNames(a).contains(QStringLiteral("Edited later")));
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);

        // The restore is the user's choice: its timers win over what the server holds.
        QVERIFY(manager.restoreDatabase(backup));
        QVERIFY(a.names().contains(QStringLiteral("Original")));
        QTRY_VERIFY(remoteNames(a).contains(QStringLiteral("Original")));
        QVERIFY(!remoteNames(a).contains(QStringLiteral("Edited later")));
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        a.sync->syncNow();
        QTRY_COMPARE(a.sync->state(), SyncController::Idle); // the download is done
        QVERIFY(a.names().contains(QStringLiteral("Original")));
        QVERIFY(!a.names().contains(QStringLiteral("Edited later")));
    }

    void twoDevices()
    {
        Device a(*server);
        Device b(*server);
        a.model->create({{QStringLiteral("name"), QStringLiteral("Made on A")}});
        b.model->create({{QStringLiteral("name"), QStringLiteral("Made on B")}});
        signInFirst(a);
        signInSecond(a, b);

        // B merged both sides and uploaded the result; A takes it at its next pull.
        QTRY_VERIFY(remoteNames(b).contains(QStringLiteral("Made on B")));
        QVERIFY(b.names().contains(QStringLiteral("Made on A")));
        QTRY_COMPARE(b.sync->state(), SyncController::Idle);
        a.sync->syncNow();
        QTRY_VERIFY(a.names().contains(QStringLiteral("Made on B")));

        // An edit on A appears on B.
        const QString uuid = a.model->create({{QStringLiteral("name"), QStringLiteral("Edited on A")}});
        QTRY_VERIFY(remoteNames(a).contains(QStringLiteral("Edited on A")));
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        b.sync->syncNow();
        QTRY_VERIFY(b.names().contains(QStringLiteral("Edited on A")));
        QTRY_COMPARE(b.sync->state(), SyncController::Idle);

        // A deletion on B disappears from A.
        QVERIFY(b.model->remove(uuid));
        QTRY_VERIFY(!remoteNames(b).contains(QStringLiteral("Edited on A")));
        QTRY_COMPARE(b.sync->state(), SyncController::Idle);
        a.sync->syncNow();
        QTRY_VERIFY(!a.names().contains(QStringLiteral("Edited on A")));
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);

        // Both settle on the same timers (each in its own order), with no upload going back and forth.
        QStringList onA = a.names();
        QStringList onB = b.names();
        onA.sort();
        onB.sort();
        QCOMPARE(onA, onB);
        const int settled = puts();
        const int pulled = gets();
        a.sync->syncNow();
        b.sync->syncNow();
        // Idle only once no request runs (an upload follows its download at once): wait for it
        // rather than a fixed time, which a loaded machine outlasts.
        QTRY_COMPARE_WITH_TIMEOUT(a.sync->state(), SyncController::Idle, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(b.sync->state(), SyncController::Idle, 10000);
        QVERIFY(gets() >= pulled + 2); // both asked the server
        QTest::qWait(10 * kUnitMs); // longer than the push delay: nothing is left to go out
        QCOMPARE(puts(), settled);
        // (The periodic pull may have started meanwhile: wait for it, it must not fail either.)
        QTRY_COMPARE_WITH_TIMEOUT(a.sync->state(), SyncController::Idle, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(b.sync->state(), SyncController::Idle, 10000);
        QCOMPARE(puts(), settled);
    }

    void keyringIsReadOncePerStartAndNeverWhileOff()
    {
        Device a(*server);
        QTest::qWait(10 * kUnitMs); // past the delay of the keyring read
        QCOMPARE(*a.store.reads, 0);
        QCOMPARE(a.sync->state(), SyncController::Off);

        signInFirst(a);
        QCOMPARE(a.keyringReads(), 0); // sign-in proves the keyring by writing

        a.build(*server); // restart
        QCOMPARE(a.keyringReads(), 0); // not while the window first shows
        QCOMPARE(a.sync->state(), SyncController::Syncing);
        QCOMPARE(a.sync->statusText(), QStringLiteral("Waiting for the keyring…"));
        QVERIFY(!a.sync->keysLoaded());
        QSignalSpy stateChanged(a.sync.get(), &SyncController::stateChanged);
        QTRY_VERIFY(a.sync->keysLoaded());
        QVERIFY(!stateChanged.isEmpty()); // announced, also while the state stays Syncing
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        QCOMPARE(a.keyringReads(), 1);

        // Uploads, downloads and a token renewal later: still the one read.
        server->expireAccessTokens();
        a.model->create({{QStringLiteral("name"), QStringLiteral("After restart")}});
        QTRY_VERIFY(remoteNames(a).contains(QStringLiteral("After restart")));
        a.sync->syncNow();
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        QCOMPARE(a.keyringReads(), 1);
    }

    void keyringMissingAtStart()
    {
        Device a(*server);
        signInFirst(a);
        *a.store.available = false;
        a.build(*server);
        QTRY_COMPARE(a.sync->state(), SyncController::Error);
        QCOMPARE(a.sync->errorKind(), SyncController::KeyringUnavailable);
        QVERIFY(!a.sync->keysLoaded()); // no new recovery code without the key
        QCOMPARE(a.keyringReads(), 1);
        QVERIFY(a.settings->contains(QStringLiteral("Sync/DeviceId"))); // still linked

        *a.store.available = true;
        a.sync->windowActivated(); // tried again when the user comes back
        QTRY_COMPARE(a.sync->state(), SyncController::Idle);
        QCOMPARE(a.keyringReads(), 2);
    }

    void lostKeysSignOut()
    {
        Device a(*server);
        signInFirst(a);
        a.model->create({{QStringLiteral("name"), QStringLiteral("Mine")}});
        a.store.secrets->clear();
        a.build(*server);
        QSignalSpy message(a.sync.get(), &SyncController::message);
        QVERIFY(message.wait(5000));
        QCOMPARE(a.sync->state(), SyncController::Off);
        QVERIFY(!a.settings->childGroups().contains(QStringLiteral("Sync")));
        QVERIFY(a.names().contains(QStringLiteral("Mine"))); // the timers stay
    }

    void pendingDeviceAsksForApproval()
    {
        Device a(*server);
        Device b(*server);
        signInFirst(a);
        b.sync->signIn(QStringLiteral("B"));
        QTRY_VERIFY(!b.sync->userCode().isEmpty());
        QVERIFY(server->approve(b.sync->userCode(), kUser).startsWith(QLatin1String("dev_")));
        QTRY_VERIFY(!b.sync->verificationCode().isEmpty());

        QSignalSpy asked(a.sync.get(), &SyncController::approvalRequested);
        a.sync->syncNow();
        QVERIFY(asked.wait(5000));
        QCOMPARE(a.sync->pendingApproval().value(QStringLiteral("verificationCode")).toString(), b.sync->verificationCode());

        // "Not now": asked no more in this session.
        a.sync->ignoreDevice(b.account()->deviceId());
        QVERIFY(a.sync->pendingApproval().isEmpty());
        a.sync->syncNow();
        QTest::qWait(20 * kUnitMs);
        QCOMPARE(asked.size(), 1);
        QCOMPARE(b.sync->state(), SyncController::WaitingForApproval);
    }

    void pendingDeviceIsFoundWithoutTheWindow()
    {
        // A device left on, its window never activated: the periodic pull finds the new one within
        // 5 minutes (here 300 * kUnitMs), not half an hour later.
        Device a(*server);
        Device b(*server);
        signInFirst(a);
        QSignalSpy asked(a.sync.get(), &SyncController::approvalRequested);
        b.sync->signIn(QStringLiteral("B"));
        QTRY_VERIFY(!b.sync->userCode().isEmpty());
        QVERIFY(server->approve(b.sync->userCode(), kUser).startsWith(QLatin1String("dev_")));
        QTRY_VERIFY(!b.sync->verificationCode().isEmpty());
        QVERIFY(asked.wait(400 * kUnitMs));
        QCOMPARE(a.sync->pendingApproval().value(QStringLiteral("verificationCode")).toString(), b.sync->verificationCode());
    }

    void approvalGoesAwayWithTheDevice()
    {
        Device a(*server);
        Device b(*server);
        signInFirst(a);
        b.sync->signIn(QStringLiteral("B"));
        QTRY_VERIFY(!b.sync->userCode().isEmpty());
        QVERIFY(server->approve(b.sync->userCode(), kUser).startsWith(QLatin1String("dev_")));
        QTRY_VERIFY(!b.sync->verificationCode().isEmpty());

        QSignalSpy asked(a.sync.get(), &SyncController::approvalRequested);
        a.sync->syncNow();
        QVERIFY(asked.wait(5000));
        QCOMPARE(a.sync->pendingApproval().value(QStringLiteral("id")).toString(), b.account()->deviceId());

        // B gives up and signs out: once the list no longer shows it, nobody is asked about it.
        QSignalSpy signedOut(b.sync.get(), &SyncController::signedOut);
        b.sync->signOut();
        QVERIFY(signedOut.wait(5000));
        QSignalSpy changed(a.sync.get(), &SyncController::pendingApprovalChanged);
        a.sync->refreshDevices();
        QTRY_VERIFY(a.sync->pendingApproval().isEmpty());
        QCOMPARE(changed.size(), 1);
        QCOMPARE(asked.size(), 1);
    }

    void approvalIsNotifiedOnlyWhileTheWindowIsHidden()
    {
        server->deviceLimit = 3;
        Device a(*server);
        Device b(*server);
        Device c(*server);
        FakeNotifier notifier;
        bool windowVisible = true;
        a.sync->attachNotifier(&notifier, [&windowVisible]() { return windowVisible; });
        signInFirst(a);

        // On screen: the window asks by itself, no notification.
        b.sync->signIn(QStringLiteral("Laptop"));
        QTRY_VERIFY(!b.sync->userCode().isEmpty());
        QVERIFY(server->approve(b.sync->userCode(), kUser).startsWith(QLatin1String("dev_")));
        QTRY_VERIFY(!b.sync->verificationCode().isEmpty());
        QSignalSpy asked(a.sync.get(), &SyncController::approvalRequested);
        a.sync->syncNow();
        QVERIFY(asked.wait(5000));
        QVERIFY(notifier.shown.isEmpty());
        a.sync->ignoreDevice(b.account()->deviceId());

        // Hidden: a notification names the device; a click asks for the Sync page.
        windowVisible = false;
        c.sync->signIn(QStringLiteral("Desktop"));
        QTRY_VERIFY(!c.sync->userCode().isEmpty());
        QVERIFY(server->approve(c.sync->userCode(), kUser).startsWith(QLatin1String("dev_")));
        QTRY_VERIFY(!c.sync->verificationCode().isEmpty());
        a.sync->syncNow();
        QVERIFY(asked.wait(5000));
        QCOMPARE(notifier.shown.size(), 1);
        QVERIFY(notifier.last().text.contains(QStringLiteral("Desktop")));
        // Nothing secret in it: the code is compared in the window.
        QVERIFY(!notifier.last().text.contains(c.sync->verificationCode()));
        QVERIFY(!notifier.last().title.contains(c.sync->verificationCode()));
        QVERIFY(notifier.last().defaultAction);
        QSignalSpy showPage(a.sync.get(), &SyncController::showSyncPageRequested);
        notifier.last().defaultAction(QStringLiteral("token"));
        QCOMPARE(showPage.size(), 1);
        QCOMPARE(showPage.first().first().toString(), QStringLiteral("token"));
        QVERIFY(notifier.invoke(notifier.actionLabels().value(0)));
        QCOMPARE(showPage.size(), 2);

        // Detached: no more notifications.
        a.sync->attachNotifier(nullptr, {});
        a.sync->ignoreDevice(c.account()->deviceId());
        QCOMPARE(notifier.shown.size(), 1);
    }

    void signOutStopsSyncing()
    {
        Device a(*server);
        signInFirst(a);
        QSignalSpy signedOut(a.sync.get(), &SyncController::signedOut);
        a.sync->signOut();
        QVERIFY(signedOut.wait(5000));
        QCOMPARE(signedOut.first().first().toBool(), true);
        QCOMPARE(a.sync->state(), SyncController::Off);
        QVERIFY(!a.settings->childGroups().contains(QStringLiteral("Sync")));
        QCOMPARE(a.sync->deviceName(), QStringLiteral("A")); // offered again, not the host name
        const int before = server->requests.size();
        a.model->create({{QStringLiteral("name"), QStringLiteral("After sign-out")}});
        a.sync->windowActivated();
        QTest::qWait(20 * kUnitMs);
        QCOMPARE(server->requests.size(), before);
        QVERIFY(!a.settings->childGroups().contains(QStringLiteral("Sync")));
    }

    void recoveryCodeGroups()
    {
        Device a(*server);
        QSignalSpy toConfirm(a.sync.get(), &SyncController::recoveryCodeToConfirm);
        signInFirst(a);
        QCOMPARE(toConfirm.size(), 1);
        const QString code = toConfirm.first().first().toString();
        const QStringList groups = code.split(QLatin1Char('-'));
        QVERIFY(groups.size() > 2);
        QVERIFY(a.sync->confirmRecoveryGroup(2, groups.at(2).toLower()));
        QVERIFY(!a.sync->confirmRecoveryGroup(2, groups.at(1) == groups.at(2) ? QStringLiteral("ZZZZ") : groups.at(1)));
        QVERIFY(!a.sync->confirmRecoveryGroup(groups.size(), groups.at(0)));
        QTRY_VERIFY(a.sync->recoveryCodeShowable());
        QCOMPARE(a.sync->recoveryCode(), code);
    }
};

QTEST_MAIN(SyncControllerTest)
#include "tst_synccontroller.moc"
