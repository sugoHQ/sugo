// SPDX-License-Identifier: GPL-3.0-or-later
#include "Localization.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "AppSettings.h"
#include "Autostart.h"
#include "DataManager.h"
#include "Database.h"
#include "Diagnostics.h"
#include "FakeSecretStore.h"
#include "FakeSyncServer.h"
#include "PresetModel.h"
#include "PresetPersistence.h"
#include "PresetRepository.h"
#include "SessionRepository.h"
#include "Settings.h"
#include "StatsModel.h"
#include "SyncAccount.h"
#include "SyncController.h"

class DiagnosticsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
    }

    void init()
    {
        Diagnostics::clearLogs();
    }

    void cleanup()
    {
        Diagnostics::setVerboseLogging(false);
    }

    void testRedaction()
    {
        const QString home = QDir::homePath();
        const QString rawPath = home + QStringLiteral("/.local/share/sugo/sugo.db");
        const QString redacted = Diagnostics::redact(rawPath);
        QVERIFY(!redacted.contains(home));
        QVERIFY(redacted.startsWith(QStringLiteral("~")));

        const QString genericHome = QStringLiteral("/home/john_doe/Documents/file.txt");
        QCOMPARE(Diagnostics::redact(genericHome), QStringLiteral("/home/<user>/Documents/file.txt"));

        const QString silverblueHome = QStringLiteral("/var/home/alice/test.log");
        QCOMPARE(Diagnostics::redact(silverblueHome), QStringLiteral("/var/home/<user>/test.log"));
    }

    void testLogCapturingAndRedaction()
    {
        Diagnostics::clearLogs();
        const QString home = QDir::homePath();

        Diagnostics::addLogEntry(QStringLiteral("INFO"), QStringLiteral("test"),
                                 QStringLiteral("Loaded database from %1/sugo.db").arg(home));

        const QString logs = Diagnostics().recentLogs();
        QVERIFY(logs.contains(QStringLiteral("[INFO]")));
        QVERIFY(logs.contains(QStringLiteral("[test]")));
        QVERIFY(logs.contains(QStringLiteral("~/sugo.db")));
        QVERIFY(!logs.contains(home));
    }

    void kdeFrameworksMessagesStayOutOfTheReport()
    {
        Diagnostics::clearLogs();
        Diagnostics::addLogEntry(QStringLiteral("WARN"), QStringLiteral("kf.kio.widgets.kdirmodel"),
                                 QStringLiteral("No node found for item that was just removed: QUrl(\"file:///home/someone/Downloads/medical-report.pdf\")"));
        Diagnostics::addLogEntry(QStringLiteral("WARN"), QStringLiteral("sugo.data"), QStringLiteral("Cannot detach the restored backup"));
        Diagnostics::addLogEntry(QStringLiteral("WARN"), QStringLiteral("qt.qpa.wayland"), QStringLiteral("Wayland message"));

        const QString logs = Diagnostics().recentLogs();
        QVERIFY(!logs.contains(QStringLiteral("kf.kio")));
        QVERIFY(!logs.contains(QStringLiteral("medical-report.pdf")));
        QVERIFY(logs.contains(QStringLiteral("[sugo.data]")));
        QVERIFY(logs.contains(QStringLiteral("[qt.qpa.wayland]")));

        // Also through the real message handler.
        Diagnostics::installLogHandler();
        Diagnostics::clearLogs();
        QTest::ignoreMessage(QtWarningMsg, "file:///home/someone/Downloads/secret.txt");
        QTest::ignoreMessage(QtWarningMsg, "kept");
        QMessageLogger(nullptr, 0, nullptr, "kf.kio.core").warning("file:///home/someone/Downloads/secret.txt");
        QMessageLogger(nullptr, 0, nullptr, "sugo.test").warning("kept");
        const QString handled = Diagnostics().recentLogs();
        QVERIFY(!handled.contains(QStringLiteral("secret.txt")));
        QVERIFY(handled.contains(QStringLiteral("kept")));
    }

    void kioDirModelWarningsAreSilencedInBothModes()
    {
        // The rule is installed by setVerboseLogging, which attach() calls at startup.
        const QLoggingCategory dirModel("kf.kio.widgets.kdirmodel");
        const QLoggingCategory otherKde("kf.kio.core");
        for (const bool verbose : {false, true}) {
            Diagnostics::setVerboseLogging(verbose);
            QVERIFY(!dirModel.isWarningEnabled());
            QVERIFY(otherKde.isWarningEnabled());
        }
        Diagnostics::setVerboseLogging(false);
    }

    void testCircularBufferCap()
    {
        Diagnostics::clearLogs();
        for (int i = 0; i < 150; ++i) {
            Diagnostics::addLogEntry(QStringLiteral("DEBUG"), QStringLiteral("cat"),
                                     QStringLiteral("Message number %1").arg(i));
        }

        const QString logs = Diagnostics().recentLogs();
        const QStringList lines = logs.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 120);
        QVERIFY(lines.first().contains(QStringLiteral("Message number 30")));
        QVERIFY(lines.last().contains(QStringLiteral("Message number 149")));
    }

    void verboseLoggingKeepsMoreLines()
    {
        QCOMPARE(Diagnostics::logCapacity(), 120);
        Diagnostics::setVerboseLogging(true);
        QVERIFY(Diagnostics::verboseLogging());
        QCOMPARE(Diagnostics::logCapacity(), 500);
        for (int i = 0; i < 600; ++i) {
            Diagnostics::addLogEntry(QStringLiteral("DEBUG"), QStringLiteral("cat"), QStringLiteral("Message number %1").arg(i));
        }
        QStringList lines = Diagnostics().recentLogs().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 500);
        QVERIFY(lines.first().contains(QStringLiteral("Message number 100")));

        // Back to normal: the buffer shrinks to the newest lines.
        Diagnostics::setVerboseLogging(false);
        lines = Diagnostics().recentLogs().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 120);
        QVERIFY(lines.first().contains(QStringLiteral("Message number 480")));
        QVERIFY(lines.last().contains(QStringLiteral("Message number 599")));
    }

    void verboseLoggingLetsSugoDebugMessagesThrough()
    {
        if (!qEnvironmentVariableIsEmpty("QT_LOGGING_RULES") || !qEnvironmentVariableIsEmpty("QT_LOGGING_CONF")) {
            QSKIP("The environment sets its own logging rules, which win over these.");
        }
        const QLoggingCategory category("sugo.test.verbose");
        Diagnostics::setVerboseLogging(true);
        QVERIFY(category.isDebugEnabled());
        Diagnostics::setVerboseLogging(false);
        QVERIFY(!category.isDebugEnabled());
    }

    void reportStatesVerboseLogging()
    {
        QTemporaryDir configDir;
        AppSettings settings(Settings::open(configDir.filePath(QStringLiteral("settings.conf"))), nullptr, false);
        Diagnostics diag;
        diag.attach(&settings, nullptr);
        QVERIFY(diag.generateReport().contains(QStringLiteral("- Verbose Logging: Off")));

        // The setting is followed while attached.
        settings.setVerboseLogging(true);
        QVERIFY(Diagnostics::verboseLogging());
        QVERIFY(diag.generateReport().contains(QStringLiteral("- Verbose Logging: On")));
        settings.setVerboseLogging(false);
        QVERIFY(!Diagnostics::verboseLogging());
        QVERIFY(diag.generateReport().contains(QStringLiteral("- Verbose Logging: Off")));

        // A saved choice applies as soon as Diagnostics is attached.
        settings.setVerboseLogging(true);
        Diagnostics::setVerboseLogging(false);
        Diagnostics other;
        other.attach(&settings, nullptr);
        QVERIFY(Diagnostics::verboseLogging());
    }

    void testGenerateReport()
    {
        Diagnostics diag;
        const QString report = diag.generateReport();

        QVERIFY(report.contains(QStringLiteral("# Sugo Diagnostics Report")));
        QVERIFY(report.contains(QStringLiteral("## Application")));
        QVERIFY(report.contains(QStringLiteral("## System & Desktop")));
        QVERIFY(report.contains(QStringLiteral("## Configuration")));
        QVERIFY(report.contains(QStringLiteral("## Storage")));
        QVERIFY(report.contains(QStringLiteral("## Recent Logs (Redacted)")));
        QVERIFY(report.contains(QStringLiteral("Version:")));
#ifdef Q_OS_MACOS
        // A test binary is not inside an .app bundle; there are no XDG variables to report.
        QVERIFY(report.contains(QStringLiteral("- App Bundle: No")));
        QVERIFY(report.contains(QStringLiteral("- Login Item (LaunchAgent): Unknown")));
        QVERIFY(!report.contains(QStringLiteral("XDG_")));
#else
        QVERIFY(report.contains(QStringLiteral("- Build Type: ")));
        QVERIFY(report.contains(QStringLiteral("XDG_CURRENT_DESKTOP")));
        QVERIFY(report.contains(QStringLiteral("- Autostart Supported: No")));
#endif
    }

    void testRedactsUserAtHostAndUserName()
    {
        QCOMPARE(Diagnostics::redact(QStringLiteral("ssh alice@workstation.local failed")),
                 QStringLiteral("ssh <user>@workstation.local failed"));
        QCOMPARE(Diagnostics::redact(QStringLiteral("owner /home/bob")), QStringLiteral("owner /home/<user>"));

        const QString user = qEnvironmentVariable("USER");
        if (user.length() > 2) {
            // Compared whole: a user called "user" is still found inside the "<user>" placeholder.
            QCOMPARE(Diagnostics::redact(QStringLiteral("started by %1 today").arg(user)), QStringLiteral("started by <user> today"));
        }
    }

    void testRedactionLeavesItsOwnPlaceholdersAlone()
    {
        // A user called "user" (the home folder /home/user): the word is redacted wherever it
        // stands, but never inside the placeholders the other steps put in.
        const QByteArray savedUser = qgetenv("USER");
        const auto restoreUser = qScopeGuard([&savedUser] { qputenv("USER", savedUser); });
        qputenv("USER", "user");
        QCOMPARE(Diagnostics::redact(QStringLiteral("/home/john_doe/file by alice@host for user")),
                 QStringLiteral("/home/<user>/file by <user>@host for <user>"));
        QCOMPARE(Diagnostics::redact(QStringLiteral("token=abc")), QStringLiteral("token=<redacted>"));

        // An account word that is also a placeholder's.
        Diagnostics::setAccountRedactions({QStringLiteral("redacted"), QStringLiteral("account")});
        QCOMPARE(Diagnostics::redact(QStringLiteral("token: abc\naccount redacted")),
                 QStringLiteral("token: <redacted>\n<account> <account>"));
        Diagnostics::setAccountRedactions({});
    }

    void testRedactsCalendarSecrets()
    {
        const QStringList inputs = {
            QStringLiteral("Authorization: Basic YWxpY2U6c2VjcmV0cGFzcw=="),
            QStringLiteral("PUT https://alice:hunter2pass@cloud.example.org/remote.php/dav failed"),
            QStringLiteral("AppPassword=hunter2pass saved"),
            QStringLiteral("{\"appPassword\":\"hunter2pass\",\"loginName\":\"x\"}"),
            QStringLiteral("body token=hunter2pass&x=1"),
        };
        for (const QString &input : inputs) {
            const QString redacted = Diagnostics::redact(input);
            QVERIFY2(!redacted.contains(QStringLiteral("hunter2pass")), qPrintable(redacted));
            QVERIFY2(!redacted.contains(QStringLiteral("YWxpY2U6")), qPrintable(redacted));
            QVERIFY2(!redacted.contains(QStringLiteral("alice:")), qPrintable(redacted));
        }
        // Ordinary text is left alone.
        QCOMPARE(Diagnostics::redact(QStringLiteral("Connected to https://cloud.example.org/x")),
                 QStringLiteral("Connected to https://cloud.example.org/x"));
    }

    void testRedactsPrefixedSecretKeysAndBearerTokens()
    {
        // A key that only ends in a secret word is a secret too (the '_' or '-' before the word
        // is no word boundary).
        for (const QString &input : {QStringLiteral("access_token=hunter2pass"),
                                     QStringLiteral("refresh_token: hunter2pass"),
                                     QStringLiteral("client_secret=hunter2pass&x=1"),
                                     QStringLiteral("{\"sync_token\": \"hunter2pass\"}"),
                                     QStringLiteral("sync_password=hunter2pass"),
                                     QStringLiteral("X-Auth-Token: hunter2pass"),
                                     QStringLiteral("csrftoken=hunter2pass"),
                                     QStringLiteral("GET /x with Bearer hunter2pass failed")}) {
            const QString redacted = Diagnostics::redact(input);
            QVERIFY2(!redacted.contains(QStringLiteral("hunter2pass")), qPrintable(redacted));
            QVERIFY2(redacted.contains(QStringLiteral("<redacted>")), qPrintable(redacted));
        }
        QCOMPARE(Diagnostics::redact(QStringLiteral("access_token=abc")), QStringLiteral("access_token=<redacted>"));
        QCOMPARE(Diagnostics::redact(QStringLiteral("sent Bearer abc.def-1 now")), QStringLiteral("sent Bearer <redacted> now"));

        // Words that merely contain a secret word, and secret words not assigned anything, stay.
        for (const QString &input : {QStringLiteral("tokens: 5"),
                                     QStringLiteral("Tokenizer=fast"),
                                     QStringLiteral("passwordless login: yes"),
                                     QStringLiteral("token refresh scheduled")}) {
            QCOMPARE(Diagnostics::redact(input), input);
        }
    }

    void testHomePathIsRedactedOnlyAtAPathBoundary()
    {
        const QString home = QDir::homePath();
        QVERIFY(!home.isEmpty() && home != QLatin1String("/"));
        QCOMPARE(Diagnostics::redact(home + QStringLiteral("/notes.txt")), QStringLiteral("~/notes.txt"));
        QCOMPARE(Diagnostics::redact(QStringLiteral("in ") + home), QStringLiteral("in ~"));
        QCOMPARE(Diagnostics::redact(home + QStringLiteral(": ok")), QStringLiteral("~: ok"));
        // A sibling folder whose name starts with the home folder's ("/home/alice" next to
        // "/home/al") is not turned into "~ice".
        const QString sibling = Diagnostics::redact(home + QStringLiteral("ice/notes.txt"));
        QVERIFY2(!sibling.startsWith(QLatin1Char('~')), qPrintable(sibling));
    }

    void testRedactsPasswordsWithSpacesAndTheCalendarAccount()
    {
        // A password with spaces is redacted whole, quoted or not.
        for (const QString &input : {QStringLiteral("password=correct horse battery"),
                                     QStringLiteral("{\"password\": \"correct horse battery\"}"),
                                     QStringLiteral("password='correct horse battery' then")}) {
            const QString redacted = Diagnostics::redact(input);
            QVERIFY2(!redacted.contains(QStringLiteral("horse")) && !redacted.contains(QStringLiteral("battery")), qPrintable(redacted));
        }

        // The calendar account (its login can differ from $USER) and server are redacted too.
        Diagnostics::setAccountRedactions({QStringLiteral("nc-login-42"), QStringLiteral("cloud.example.org")});
        const QString redacted = Diagnostics::redact(
            QStringLiteral("PUT https://cloud.example.org/remote.php/dav/calendars/nc-login-42/personal/x.ics: 500"));
        QVERIFY2(!redacted.contains(QStringLiteral("nc-login-42")), qPrintable(redacted));
        QVERIFY2(!redacted.contains(QStringLiteral("cloud.example.org")), qPrintable(redacted));
        QVERIFY(redacted.contains(QStringLiteral("/remote.php/dav/calendars/")));
        Diagnostics::setAccountRedactions({});
        QVERIFY(Diagnostics::redact(QStringLiteral("cloud.example.org")).contains(QStringLiteral("cloud.example.org")));

        // An account word is redacted only where it stands on its own, in any case.
        Diagnostics::setAccountRedactions({QStringLiteral("tim")});
        QCOMPARE(Diagnostics::redact(QStringLiteral("Timer for tim started")), QStringLiteral("Timer for <account> started"));
        QCOMPARE(Diagnostics::redact(QStringLiteral("Tim: login failed")), QStringLiteral("<account>: login failed"));
        Diagnostics::setAccountRedactions({});
    }

    void testAttachedReport()
    {
        std::unique_ptr<Database> db = std::make_unique<Database>(QStringLiteral(":memory:"));
        QVERIFY(db->open());
        SessionRepository sessions(db->connectionName());
        PresetRepository presetsRepo(db->connectionName());
        PresetModel presets;
        StatsModel stats;
        DataManager dataManager(db.get(), &sessions, &presetsRepo, &presets, &stats);
        QTemporaryDir configDir;
        AppSettings settings(Settings::open(configDir.filePath(QStringLiteral("settings.conf"))), nullptr, false);

        Diagnostics diag;
        diag.attach(&settings, &dataManager);

        const QString report = diag.generateReport();
        QVERIFY(report.contains(QStringLiteral("Total Recorded Sessions: 0")));
        QVERIFY(report.contains(QStringLiteral("Sound Theme: ")));
        QVERIFY(report.contains(QStringLiteral("- Sync: (Not attached)")));
    }

    // The Sync section says how things stand, never who or with what: no account or device id,
    // device name, sign-in, verification or recovery code, token or etag.
    void syncSectionHasNoPersonalData()
    {
        FakeSyncServer server;
        server.interval = 1;
        server.deviceLimit = 3;
        QTemporaryDir dir;
        Database db(dir.filePath(QStringLiteral("sugo.db")));
        QVERIFY(db.open());
        PresetRepository repo(db.connectionName());
        PresetModel model;
        PresetPersistence persistence(&model, &repo);
        FakeSecretStore storeA;
        FakeSecretStore storeB;
        SyncController a(Settings::open(dir.filePath(QStringLiteral("a.conf"))), &storeA, QUrl(server.base()));
        SyncController b(Settings::open(dir.filePath(QStringLiteral("b.conf"))), &storeB, QUrl(server.base()));
        a.setTimeUnitMs(10);
        b.setTimeUnitMs(10);
        a.attach(&model, &persistence, &repo);

        Diagnostics diag;
        diag.attachSync(&a);
        QString report = diag.generateReport();
        QVERIFY(report.contains(QStringLiteral("## Sync\n- State: Off\n- Error: NoError\n- Keyring Available: Yes\n- Linked: No\n"
                                               "- Last Sync: Never\n- Devices: 0\n- Server: custom\n")));

        // A linked first device, a second one waiting with its codes, a recovery code.
        QSignalSpy recovery(&a, &SyncController::recoveryCodeToConfirm);
        a.signIn(QStringLiteral("Marios Secret Laptop"));
        QTRY_VERIFY(!a.userCode().isEmpty());
        const QString userCodeA = a.userCode();
        QVERIFY(server.approve(userCodeA, QStringLiteral("usr_private_42")).startsWith(QLatin1String("dev_")));
        QTRY_COMPARE_WITH_TIMEOUT(a.state(), SyncController::Idle, 5000);
        QTRY_VERIFY(a.lastSync().isValid());
        b.signIn(QStringLiteral("Marios Other Desktop"));
        QTRY_VERIFY(!b.userCode().isEmpty());
        const QString userCodeB = b.userCode();
        QVERIFY(server.approve(userCodeB, QStringLiteral("usr_private_42")).startsWith(QLatin1String("dev_")));
        QTRY_VERIFY(!b.verificationCode().isEmpty());
        a.refreshDevices();
        QTRY_COMPARE(a.devices().size(), 2);
        QCOMPARE(recovery.size(), 1);

        report = diag.generateReport();
        QVERIFY(report.contains(QStringLiteral("- State: Idle\n")));
        QVERIFY(report.contains(QStringLiteral("- Linked: Yes\n")));
        QVERIFY(report.contains(QStringLiteral("- Devices: 2\n")));
        QVERIFY(report.contains(QStringLiteral("- Last Sync: ") + a.lastSync().toUTC().toString(Qt::ISODate)));

        QStringList secrets{QStringLiteral("Marios"), QStringLiteral("usr_private_42"), a.account()->deviceId(),
                            b.account()->deviceId(), userCodeA, userCodeB, b.verificationCode(),
                            recovery.first().first().toString(), a.recoveryCode()};
        for (const QString &secret : storeA.secrets->values()) {
            const QJsonObject values = QJsonDocument::fromJson(secret.toUtf8()).object();
            for (auto it = values.begin(); it != values.end(); ++it) {
                if (it.value().isString() && it.value().toString().size() > 6) {
                    secrets << it.value().toString(); // refresh token, keys
                }
            }
        }
        const QString etag = QString::fromLatin1(server.accounts.value(QStringLiteral("usr_private_42")).blobs.value("presets").etag);
        QVERIFY(!etag.isEmpty());
        secrets << etag;
        for (const QString &secret : std::as_const(secrets)) {
            QVERIFY2(!secret.isEmpty(), "a value to look for is empty");
            QVERIFY2(!report.contains(secret, Qt::CaseInsensitive), qPrintable(QStringLiteral("The report shows ") + secret));
        }
        QVERIFY(!report.contains(QStringLiteral("dev_")));
    }

    void testReportRedactsSoundFilePaths()
    {
        QTemporaryDir configDir;
        AppSettings settings(Settings::open(configDir.filePath(QStringLiteral("settings.conf"))), nullptr, false);
        // Sounds may be files of the user's, given by their absolute path.
        settings.setSoundTheme(QStringLiteral("/home/alice/Music/bell.oga"));
        settings.setPreAlarmSound(QDir::homePath() + QStringLiteral("/Sounds/chime.ogg"));

        Diagnostics diag;
        diag.attach(&settings, nullptr);
        const QString report = diag.generateReport();
        QVERIFY(report.contains(QStringLiteral("Sound Theme: /home/<user>/Music/bell.oga")));
        QVERIFY(!report.contains(QStringLiteral("alice")));
        QVERIFY(report.contains(QStringLiteral("~/Sounds/chime.ogg")));
        QVERIFY(!report.contains(QDir::homePath() + QStringLiteral("/Sounds")));
    }
};

QTEST_MAIN(DiagnosticsTest)
#include "tst_diagnostics.moc"
