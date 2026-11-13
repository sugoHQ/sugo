// SPDX-License-Identifier: GPL-3.0-or-later
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include "FakeSecretStore.h"
#include "SyncCrypto.h"
#include "SyncSecrets.h"

namespace
{
SyncSecrets::Values sample()
{
    SyncSecrets::Values v;
    v.refreshToken = QStringLiteral("rt_one");
    v.deviceKey = SyncCrypto::generateKeyPair().secretKey;
    v.dataKey = SyncCrypto::newDataKey();
    v.recoveryCode = SyncCrypto::newRecoveryCode();
    return v;
}

QJsonObject stored(const FakeSecretStore &store)
{
    return QJsonDocument::fromJson(store.secrets->value(SyncSecrets::itemKey()).toUtf8()).object();
}
} // namespace

class SyncSecretsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void jsonRoundTrip()
    {
        const SyncSecrets::Values v = sample();
        const QByteArray json = SyncSecrets::toJson(v);
        const QJsonObject object = QJsonDocument::fromJson(json).object();
        QCOMPARE(object.value(QLatin1String("v")).toInt(), 1);
        QCOMPARE(object.value(QLatin1String("refreshToken")).toString(), QStringLiteral("rt_one"));
        QCOMPARE(object.value(QLatin1String("recoveryCode")).toString(), SyncCrypto::formatRecoveryCode(v.recoveryCode));

        SyncSecrets::LoadResult result = SyncSecrets::LoadResult::Failed;
        const std::optional<SyncSecrets::Values> back = SyncSecrets::fromJson(json, &result);
        QVERIFY(back);
        QCOMPARE(int(result), int(SyncSecrets::LoadResult::Loaded));
        QCOMPARE(back->refreshToken, v.refreshToken);
        QVERIFY(back->deviceKey == v.deviceKey);
        QVERIFY(back->dataKey == v.dataKey);
        QVERIFY(back->recoveryCode == v.recoveryCode);
    }

    void pendingRecoveryCodeRoundTrip()
    {
        SyncSecrets::Values v = sample();
        v.pendingRecoveryCode = SyncCrypto::newRecoveryCode();
        const QByteArray json = SyncSecrets::toJson(v);
        const std::optional<SyncSecrets::Values> back = SyncSecrets::fromJson(json);
        QVERIFY(back);
        QVERIFY(back->pendingRecoveryCode == v.pendingRecoveryCode);
        QVERIFY(back->recoveryCode == v.recoveryCode);
        QVERIFY(!SyncSecrets::toJson(sample()).contains("pendingRecoveryCode"));

        // Only a convenience: an unreadable one is dropped, the keys stay.
        QJsonObject object = QJsonDocument::fromJson(json).object();
        object.insert(QLatin1String("pendingRecoveryCode"), QStringLiteral("1234"));
        SyncSecrets::LoadResult result = SyncSecrets::LoadResult::Failed;
        const std::optional<SyncSecrets::Values> bad = SyncSecrets::fromJson(QJsonDocument(object).toJson(), &result);
        QVERIFY(bad);
        QCOMPARE(int(result), int(SyncSecrets::LoadResult::Loaded));
        QVERIFY(bad->pendingRecoveryCode.isEmpty());
        QVERIFY(bad->dataKey == v.dataKey);
    }

    void missingFieldsStayEmpty()
    {
        SyncSecrets::Values v;
        v.refreshToken = QStringLiteral("rt");
        v.deviceKey = SyncCrypto::generateKeyPair().secretKey;
        const QByteArray json = SyncSecrets::toJson(v);
        QVERIFY(!json.contains("dataKey"));
        QVERIFY(!json.contains("recoveryCode"));
        const auto back = SyncSecrets::fromJson(json);
        QVERIFY(back);
        QVERIFY(back->dataKey.isEmpty());
        QVERIFY(back->recoveryCode.isEmpty());
    }

    void loadReadsOnce()
    {
        FakeSecretStore store;
        store.secrets->insert(SyncSecrets::itemKey(), QString::fromUtf8(SyncSecrets::toJson(sample())));
        SyncSecrets secrets(&store);
        QCOMPARE(int(secrets.load()), int(SyncSecrets::LoadResult::Loaded));
        QCOMPARE(secrets.refreshToken(), QStringLiteral("rt_one"));
        QCOMPARE(*store.reads, 1);
        QCOMPARE(store.log->size(), 1);
    }

    void loadResults_data()
    {
        QTest::addColumn<QString>("item");
        QTest::addColumn<int>("result");
        QTest::newRow("missing") << QString() << int(SyncSecrets::LoadResult::Empty);
        QTest::newRow("no values") << QStringLiteral("{\"v\":1}") << int(SyncSecrets::LoadResult::Empty);
        QTest::newRow("not json") << QStringLiteral("hello") << int(SyncSecrets::LoadResult::Unreadable);
        QTest::newRow("no version") << QStringLiteral("{\"refreshToken\":\"x\"}") << int(SyncSecrets::LoadResult::Unreadable);
        QTest::newRow("bad key") << QStringLiteral("{\"v\":1,\"deviceKey\":\"AAAA\"}") << int(SyncSecrets::LoadResult::Unreadable);
        QTest::newRow("bad code") << QStringLiteral("{\"v\":1,\"recoveryCode\":\"1234\"}") << int(SyncSecrets::LoadResult::Unreadable);
        QTest::newRow("newer") << QStringLiteral("{\"v\":2,\"refreshToken\":\"x\"}") << int(SyncSecrets::LoadResult::NewerVersion);
    }
    void loadResults()
    {
        QFETCH(QString, item);
        QFETCH(int, result);
        FakeSecretStore store;
        if (!item.isEmpty()) {
            store.secrets->insert(SyncSecrets::itemKey(), item);
        }
        SyncSecrets secrets(&store);
        QCOMPARE(int(secrets.load()), result);
        QVERIFY(secrets.values().isEmpty());
        QVERIFY(store.accesses(SyncSecrets::itemKey(), FakeSecretStore::Access::Write).isEmpty()); // never "repaired"
    }

    void loadFailsWithoutKeyring()
    {
        FakeSecretStore store;
        *store.available = false;
        SyncSecrets secrets(&store);
        QCOMPARE(int(secrets.load()), int(SyncSecrets::LoadResult::Failed));
    }

    void probeNeedsAWrite()
    {
        // macOS: a read of a missing item answers "nothing" even when writes are refused.
        FakeSecretStore store;
        *store.failWrites = true;
        SyncSecrets secrets(&store);
        QVERIFY(!secrets.probe());
        QVERIFY(!secrets.available());
        QVERIFY(store.accesses(SyncSecrets::probeKey(), FakeSecretStore::Access::Read).isEmpty());

        *store.failWrites = false;
        QVERIFY(secrets.probe());
        QVERIFY(secrets.available());
    }

    void probeLeavesNothing()
    {
        FakeSecretStore store;
        SyncSecrets secrets(&store);
        QVERIFY(secrets.probe());
        QVERIFY(store.secrets->isEmpty());
        const auto writes = store.accesses(SyncSecrets::probeKey(), FakeSecretStore::Access::Write);
        QCOMPARE(writes.size(), 2);
        QVERIFY(!writes.at(0).value.isEmpty());
        QVERIFY(writes.at(1).value.isEmpty());
        QVERIFY(store.accesses(SyncSecrets::itemKey(), FakeSecretStore::Access::Write).isEmpty());
    }

    void probeWithoutKeyring()
    {
        FakeSecretStore store;
        *store.available = false;
        SyncSecrets secrets(&store);
        QVERIFY(!secrets.probe());
    }

    void updateWritesTheWholeItem()
    {
        FakeSecretStore store;
        SyncSecrets secrets(&store);
        QVERIFY(secrets.replace(sample()));
        const QJsonObject before = stored(store);
        QVERIFY(secrets.setRefreshToken(QStringLiteral("rt_two")));
        const QJsonObject after = stored(store);
        QCOMPARE(after.value(QLatin1String("refreshToken")).toString(), QStringLiteral("rt_two"));
        for (const char *field : {"deviceKey", "dataKey", "recoveryCode", "v"}) {
            QCOMPARE(after.value(QLatin1String(field)), before.value(QLatin1String(field)));
        }
        QCOMPARE(store.secrets->size(), 1);
        QCOMPARE(*store.reads, 0);
    }

    void failedWriteKeepsValuesAndTheNewToken()
    {
        FakeSecretStore store;
        SyncSecrets secrets(&store);
        QVERIFY(secrets.replace(sample()));
        const SecretBytes dataKey = secrets.values().dataKey.copy();

        *store.failWrites = true;
        QVERIFY(!secrets.update([](SyncSecrets::Values &v) {
            v.refreshToken = QStringLiteral("rt_two");
            v.dataKey = SecretBytes();
        }));
        // Memory unchanged, but the new (already issued) token is kept.
        QVERIFY(secrets.values().dataKey == dataKey);
        QCOMPARE(secrets.values().refreshToken, QStringLiteral("rt_one"));
        QVERIFY(secrets.hasUnsavedRefreshToken());
        QCOMPARE(secrets.refreshToken(), QStringLiteral("rt_two"));

        // Another failed write that does not touch the token keeps it unsaved.
        QVERIFY(!secrets.update([](SyncSecrets::Values &) {}));
        QCOMPARE(secrets.refreshToken(), QStringLiteral("rt_two"));

        // The next write that works includes it.
        *store.failWrites = false;
        QVERIFY(secrets.update([](SyncSecrets::Values &v) { v.recoveryCode = SecretBytes(); }));
        QVERIFY(!secrets.hasUnsavedRefreshToken());
        QCOMPARE(stored(store).value(QLatin1String("refreshToken")).toString(), QStringLiteral("rt_two"));
        QVERIFY(!stored(store).contains(QLatin1String("recoveryCode")));
        QVERIFY(secrets.values().dataKey == dataKey);
    }

    void clearRemovesTheItem()
    {
        FakeSecretStore store;
        SyncSecrets secrets(&store);
        QVERIFY(secrets.replace(sample()));
        QVERIFY(secrets.clear());
        QVERIFY(store.secrets->isEmpty());
        QVERIFY(secrets.values().isEmpty());
        QVERIFY(secrets.refreshToken().isEmpty());
    }
};

QTEST_GUILESS_MAIN(SyncSecretsTest)
#include "tst_syncsecrets.moc"
