// SPDX-License-Identifier: GPL-3.0-or-later
// SecretHandoff: the secrets of one build carried over to the build that updates it.
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "SecretHandoff.h"

namespace
{
/// A login Keychain as an ad hoc signed build sees it (tried on macOS 27): every item belongs to
/// the build that wrote it; another build lists it, but neither reads nor deletes it without the
/// user's leave ("Always Allow", `allowed`), and so cannot replace it either.
struct Keychain {
    struct Item {
        QString secret;
        QString owner;
        QSet<QString> allowed;
    };
    QHash<QString, Item> items;
    bool available = true;  ///< false: every call fails (no login session, say)
    bool failWrites = false; ///< adding items fails (a Keychain locked meanwhile)
};

class BuildStore : public SecretHandoffStore
{
public:
    BuildStore(std::shared_ptr<Keychain> keychain, QString build)
        : m_keychain(std::move(keychain))
        , m_build(std::move(build))
    {
    }

    std::optional<QString> read(const QString &key) override { return readWithoutAsking(key); }
    std::optional<QString> readWithoutAsking(const QString &key) override
    {
        if (!m_keychain->available) {
            return std::nullopt;
        }
        const auto it = m_keychain->items.constFind(key);
        if (it == m_keychain->items.cend()) {
            return QString();
        }
        if (!mayUse(*it)) {
            return std::nullopt;
        }
        return it->secret;
    }
    bool write(const QString &key, const QString &secret) override
    {
        if (!m_keychain->available) {
            return false;
        }
        if (const auto it = m_keychain->items.constFind(key); it != m_keychain->items.cend() && !mayUse(*it)) {
            return false; // errSecInvalidOwnerEdit
        }
        m_keychain->items.remove(key);
        if (secret.isEmpty()) {
            return true;
        }
        if (m_keychain->failWrites) {
            return false;
        }
        m_keychain->items.insert(key, {secret, m_build, {}});
        return true;
    }
    std::optional<QStringList> keys() override
    {
        if (!m_keychain->available) {
            return std::nullopt;
        }
        return m_keychain->items.keys();
    }

private:
    bool mayUse(const Keychain::Item &item) const { return item.owner == m_build || item.allowed.contains(m_build); }

    std::shared_ptr<Keychain> m_keychain;
    QString m_build;
};
} // namespace

class SecretHandoffTest : public QObject
{
    Q_OBJECT

private:
    std::shared_ptr<Keychain> m_keychain;
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;

    const QString calendar = QStringLiteral("calendar-caldav");
    const QString syncToken = QStringLiteral("sync-token");

private Q_SLOTS:
    void init()
    {
        m_keychain = std::make_shared<Keychain>();
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        m_path = m_dir->path() + QStringLiteral("/sugo/keychain-handoff.json"); // its folder is made when needed
    }

    void theNextBuildReadsTheSecretsWithoutAsking()
    {
        BuildStore oldBuild(m_keychain, QStringLiteral("1.0.1"));
        QVERIFY(oldBuild.write(calendar, QStringLiteral("app pässword")));
        QVERIFY(oldBuild.write(syncToken, QStringLiteral("token")));

        BuildStore newBuild(m_keychain, QStringLiteral("1.0.2"));
        QVERIFY(!newBuild.readWithoutAsking(calendar)); // what macOS asks about, without a handoff

        QCOMPARE(SecretHandoff::handOver(oldBuild, m_path), 2);
        QVERIFY(m_keychain->items.isEmpty()); // the old build's items are gone
        QVERIFY(QFileInfo::exists(m_path));
        const QFileDevice::Permissions others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
        QCOMPARE(QFile::permissions(m_path) & others, QFileDevice::Permissions());
        QVERIFY(QFile::permissions(m_path).testFlag(QFileDevice::ReadOwner));

        QCOMPARE(SecretHandoff::takeBack(newBuild, m_path), 2);
        QVERIFY(!QFileInfo::exists(m_path));
        QCOMPARE(newBuild.readWithoutAsking(calendar), std::optional<QString>(QStringLiteral("app pässword")));
        QCOMPARE(newBuild.readWithoutAsking(syncToken), std::optional<QString>(QStringLiteral("token")));

        // And on to the update after it.
        BuildStore nextBuild(m_keychain, QStringLiteral("1.0.3"));
        QCOMPARE(SecretHandoff::handOver(newBuild, m_path), 2);
        QCOMPARE(SecretHandoff::takeBack(nextBuild, m_path), 2);
        QCOMPARE(nextBuild.readWithoutAsking(calendar), std::optional<QString>(QStringLiteral("app pässword")));
    }

    void anUpdateThatDidNotGetInPlaceGivesThemBackToTheSameBuild()
    {
        BuildStore build(m_keychain, QStringLiteral("1.0.1"));
        QVERIFY(build.write(calendar, QStringLiteral("secret")));
        QCOMPARE(SecretHandoff::handOver(build, m_path), 1);
        QCOMPARE(SecretHandoff::takeBack(build, m_path), 1);
        QCOMPARE(build.readWithoutAsking(calendar), std::optional<QString>(QStringLiteral("secret")));
        QVERIFY(!QFileInfo::exists(m_path));
    }

    void anItemTheBuildCannotReadWithoutAskingStays()
    {
        // Written by an older build, never allowed to this one: the next build asks, as before.
        BuildStore older(m_keychain, QStringLiteral("1.0.0"));
        QVERIFY(older.write(calendar, QStringLiteral("old")));
        BuildStore build(m_keychain, QStringLiteral("1.0.1"));
        QVERIFY(build.write(syncToken, QStringLiteral("token")));

        QCOMPARE(SecretHandoff::handOver(build, m_path), 1);
        QVERIFY(m_keychain->items.contains(calendar));
        QCOMPARE(m_keychain->items.value(calendar).owner, QStringLiteral("1.0.0"));

        BuildStore newBuild(m_keychain, QStringLiteral("1.0.2"));
        QCOMPARE(SecretHandoff::takeBack(newBuild, m_path), 1);
        QVERIFY(!newBuild.readWithoutAsking(calendar));
        QVERIFY(!QFileInfo::exists(m_path));
    }

    void anItemAllowedWithAlwaysAllowIsCarriedOver()
    {
        BuildStore older(m_keychain, QStringLiteral("1.0.0"));
        QVERIFY(older.write(calendar, QStringLiteral("secret")));
        m_keychain->items[calendar].allowed.insert(QStringLiteral("1.0.1"));

        BuildStore build(m_keychain, QStringLiteral("1.0.1"));
        QCOMPARE(SecretHandoff::handOver(build, m_path), 1);
        BuildStore newBuild(m_keychain, QStringLiteral("1.0.2"));
        QCOMPARE(SecretHandoff::takeBack(newBuild, m_path), 1);
        QCOMPARE(newBuild.readWithoutAsking(calendar), std::optional<QString>(QStringLiteral("secret")));
    }

    void nothingToCarryWritesNoFile()
    {
        BuildStore build(m_keychain, QStringLiteral("1.0.1"));
        QCOMPARE(SecretHandoff::handOver(build, m_path), 0);
        QVERIFY(!QFileInfo::exists(m_path));
        QCOMPARE(SecretHandoff::takeBack(build, m_path), 0); // and no file: nothing happens
    }

    void anUnreachableKeychainIsLeftAlone()
    {
        BuildStore build(m_keychain, QStringLiteral("1.0.1"));
        QVERIFY(build.write(calendar, QStringLiteral("secret")));
        m_keychain->available = false;
        QCOMPARE(SecretHandoff::handOver(build, m_path), 0);
        QVERIFY(!QFileInfo::exists(m_path));
        m_keychain->available = true;
        QVERIFY(m_keychain->items.contains(calendar));
    }

    void nothingIsDeletedWhenTheFileCannotBeWritten()
    {
        BuildStore build(m_keychain, QStringLiteral("1.0.1"));
        QVERIFY(build.write(calendar, QStringLiteral("secret")));
        // A folder where the file's folder should be created.
        QFile blocker(m_dir->path() + QStringLiteral("/sugo"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();

        QCOMPARE(SecretHandoff::handOver(build, m_path), 0);
        QCOMPARE(build.readWithoutAsking(calendar), std::optional<QString>(QStringLiteral("secret")));
    }

    void secretsThatCannotBeStoredWaitForTheNextStart()
    {
        BuildStore oldBuild(m_keychain, QStringLiteral("1.0.1"));
        QVERIFY(oldBuild.write(calendar, QStringLiteral("secret")));
        QVERIFY(oldBuild.write(syncToken, QStringLiteral("token")));
        QCOMPARE(SecretHandoff::handOver(oldBuild, m_path), 2);

        // The file is the only copy: a Keychain that cannot store them keeps them waiting.
        BuildStore newBuild(m_keychain, QStringLiteral("1.0.2"));
        m_keychain->failWrites = true;
        QCOMPARE(SecretHandoff::takeBack(newBuild, m_path), 0);
        QVERIFY(QFileInfo::exists(m_path));
        m_keychain->available = false;
        QCOMPARE(SecretHandoff::takeBack(newBuild, m_path), 0);
        QVERIFY(QFileInfo::exists(m_path));

        m_keychain->available = true;
        m_keychain->failWrites = false;
        QCOMPARE(SecretHandoff::takeBack(newBuild, m_path), 2);
        QVERIFY(!QFileInfo::exists(m_path));
        QCOMPARE(newBuild.readWithoutAsking(syncToken), std::optional<QString>(QStringLiteral("token")));
    }

    void aSecretTypedAgainSinceIsNotOverwritten()
    {
        // The update's first start found the Keychain locked, so the file kept the secret; the
        // calendar then asked for the password and the user typed a new one.
        BuildStore oldBuild(m_keychain, QStringLiteral("1.0.1"));
        QVERIFY(oldBuild.write(calendar, QStringLiteral("old")));
        QCOMPARE(SecretHandoff::handOver(oldBuild, m_path), 1);

        BuildStore newBuild(m_keychain, QStringLiteral("1.0.2"));
        m_keychain->failWrites = true;
        QCOMPARE(SecretHandoff::takeBack(newBuild, m_path), 0);
        QVERIFY(QFileInfo::exists(m_path));
        m_keychain->failWrites = false;
        QVERIFY(newBuild.write(calendar, QStringLiteral("typed again")));

        // The newer secret wins: the file's copy is not written over it, and the file goes.
        QCOMPARE(SecretHandoff::takeBack(newBuild, m_path), 0);
        QVERIFY(!QFileInfo::exists(m_path));
        QCOMPARE(newBuild.readWithoutAsking(calendar), std::optional<QString>(QStringLiteral("typed again")));
    }

    void aSecretTheKeychainDoesNotHaveIsWrittenAsBefore()
    {
        BuildStore oldBuild(m_keychain, QStringLiteral("1.0.1"));
        QVERIFY(oldBuild.write(calendar, QStringLiteral("secret")));
        QCOMPARE(SecretHandoff::handOver(oldBuild, m_path), 1);

        BuildStore newBuild(m_keychain, QStringLiteral("1.0.2"));
        QCOMPARE(SecretHandoff::takeBack(newBuild, m_path), 1);
        QVERIFY(!QFileInfo::exists(m_path));
        QCOMPARE(newBuild.readWithoutAsking(calendar), std::optional<QString>(QStringLiteral("secret")));
    }

    void secretsStillWaitingAreKeptByTheNextHandOver()
    {
        BuildStore oldBuild(m_keychain, QStringLiteral("1.0.1"));
        QVERIFY(oldBuild.write(calendar, QStringLiteral("secret")));
        QCOMPARE(SecretHandoff::handOver(oldBuild, m_path), 1);

        // 1.0.2 cannot store it (and so has no item for it), but installs 1.0.3 meanwhile.
        BuildStore newBuild(m_keychain, QStringLiteral("1.0.2"));
        m_keychain->failWrites = true;
        QCOMPARE(SecretHandoff::takeBack(newBuild, m_path), 0);
        m_keychain->failWrites = false;
        QVERIFY(newBuild.write(syncToken, QStringLiteral("token")));
        QCOMPARE(SecretHandoff::handOver(newBuild, m_path), 1);

        BuildStore nextBuild(m_keychain, QStringLiteral("1.0.3"));
        QCOMPARE(SecretHandoff::takeBack(nextBuild, m_path), 2);
        QCOMPARE(nextBuild.readWithoutAsking(calendar), std::optional<QString>(QStringLiteral("secret")));
        QCOMPARE(nextBuild.readWithoutAsking(syncToken), std::optional<QString>(QStringLiteral("token")));
    }

    void anItemTheOldBuildCouldNotDeleteIsNotKeptWaiting()
    {
        // The old build saved the secret but its item stayed (the delete failed): the new build
        // cannot replace it, and the secret is still in the Keychain, so the file goes.
        BuildStore oldBuild(m_keychain, QStringLiteral("1.0.1"));
        QVERIFY(oldBuild.write(calendar, QStringLiteral("secret")));
        QCOMPARE(SecretHandoff::handOver(oldBuild, m_path), 1);
        m_keychain->items.insert(calendar, {QStringLiteral("secret"), QStringLiteral("1.0.1"), {}});

        BuildStore newBuild(m_keychain, QStringLiteral("1.0.2"));
        QCOMPARE(SecretHandoff::takeBack(newBuild, m_path), 0);
        QVERIFY(!QFileInfo::exists(m_path));
        QCOMPARE(m_keychain->items.value(calendar).owner, QStringLiteral("1.0.1"));
    }

    void aFileThatIsNotAHandoffIsLeftAlone()
    {
        QVERIFY(QDir().mkpath(QFileInfo(m_path).absolutePath()));
        QFile file(m_path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not json");
        file.close();

        BuildStore build(m_keychain, QStringLiteral("1.0.2"));
        QCOMPARE(SecretHandoff::takeBack(build, m_path), 0);
        QVERIFY(QFileInfo::exists(m_path));
        QVERIFY(m_keychain->items.isEmpty());
    }

    void aHandOverDoesNotReplaceAFileItCannotRead()
    {
        // Still waiting, in a format of a newer Sugo (the user went back to an older one): the
        // only copy of its secrets, so the handover leaves it, and the Keychain, as they are.
        QVERIFY(QDir().mkpath(QFileInfo(m_path).absolutePath()));
        const QByteArray waiting = R"({"format":2,"secrets":{"calendar-caldav":"waiting"}})";
        QFile file(m_path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(waiting);
        file.close();

        BuildStore build(m_keychain, QStringLiteral("1.0.1"));
        QVERIFY(build.write(syncToken, QStringLiteral("token")));
        QCOMPARE(SecretHandoff::handOver(build, m_path), 0);

        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), waiting);
        file.close();
        QCOMPARE(build.readWithoutAsking(syncToken), std::optional<QString>(QStringLiteral("token")));
    }

    void anIsolatedRunHasNoHandoff()
    {
        qputenv("SUGO_DATA_DIR", m_dir->path().toLocal8Bit());
        QVERIFY(SecretHandoff::defaultPath().isEmpty());
        qunsetenv("SUGO_DATA_DIR");
        QVERIFY(SecretHandoff::defaultPath().endsWith(QStringLiteral("/sugo/keychain-handoff.json")));

        BuildStore build(m_keychain, QStringLiteral("1.0.1"));
        QVERIFY(build.write(calendar, QStringLiteral("secret")));
        QCOMPARE(SecretHandoff::handOver(build, QString()), 0);
        QVERIFY(m_keychain->items.contains(calendar));
    }
};

QTEST_GUILESS_MAIN(SecretHandoffTest)
#include "tst_secrethandoff.moc"
