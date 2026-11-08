// SPDX-License-Identifier: GPL-3.0-or-later
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QLoggingCategory>
#include <QTest>

#include <memory>

#include "FakeNotifier.h"
#include "FreedesktopNotifier.h"
#include "PrivateDBus.h"

namespace
{
const QString kService = QStringLiteral("org.freedesktop.Notifications");
const QString kPath = QStringLiteral("/org/freedesktop/Notifications");
const QString kInterface = QStringLiteral("org.freedesktop.Notifications");

/// Stand-in for a notification server (Plasma's, when it offers "inline-reply").
class FakeNotificationServer : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")

public:
    struct Call {
        QString appName;
        uint replacesId = 0;
        QString icon;
        QString summary;
        QString body;
        QStringList actions;
        QVariantMap hints;
        int timeout = 0;
    };

    explicit FakeNotificationServer(const QDBusConnection &bus)
        : m_bus(bus)
    {
    }

    QStringList capabilities{QStringLiteral("body"), QStringLiteral("actions")};
    QList<Call> calls;
    uint nextId = 1;
    /// GetCapabilities answers with a server error (a broken or overloaded server).
    bool capabilitiesFail = false;

    /// Sends one of the server's signals, as if the user had done something.
    void emitSignal(const QString &name, const QVariantList &arguments)
    {
        QDBusMessage message = QDBusMessage::createSignal(kPath, kInterface, name);
        message.setArguments(arguments);
        m_bus.send(message);
    }

public Q_SLOTS:
    QStringList GetCapabilities()
    {
        if (capabilitiesFail) {
            sendErrorReply(QDBusError::Failed, QStringLiteral("The server is busy"));
            return {};
        }
        return capabilities;
    }

    uint Notify(const QString &appName, uint replacesId, const QString &icon, const QString &summary, const QString &body,
                const QStringList &actions, const QVariantMap &hints, int timeout)
    {
        calls.append({appName, replacesId, icon, summary, body, actions, hints, timeout});
        return nextId++;
    }

private:
    QDBusConnection m_bus;
};
} // namespace

class FreedesktopNotifierTest : public QObject
{
    Q_OBJECT

private:
    PrivateDBus m_bus;
    std::unique_ptr<FakeNotificationServer> m_server;
    std::unique_ptr<FreedesktopNotifier> m_notifier;
    std::unique_ptr<QObject> m_context;

    QDBusConnection serverBus() const { return QDBusConnection(QStringLiteral("server")); }
    QDBusConnection clientBus() const { return QDBusConnection(QStringLiteral("client")); }

    void publishServer()
    {
        QVERIFY(serverBus().registerObject(kPath, m_server.get(), QDBusConnection::ExportAllSlots));
        QVERIFY(serverBus().registerService(kService));
    }

    void unpublishServer()
    {
        serverBus().unregisterService(kService);
        serverBus().unregisterObject(kPath);
    }

    /// Shows `notification` and waits until the server has it; returns its id.
    uint show(const Notifier::Notification &notification)
    {
        const qsizetype before = m_server->calls.size();
        m_notifier->notify(m_context.get(), notification);
        if (!QTest::qWaitFor([&]() { return m_server->calls.size() > before; }, 5000)) {
            return 0;
        }
        // The id reaches the notifier a moment after the server handed it out.
        QTest::qWait(50);
        return m_server->nextId - 1;
    }

    /// Sends a signal and lets it arrive.
    void serverSays(const QString &name, const QVariantList &arguments)
    {
        m_server->emitSignal(name, arguments);
        QTest::qWait(100);
    }

    static Notifier::Notification sample(QStringList *log)
    {
        Notifier::Notification notification;
        notification.title = QStringLiteral("Work session finished");
        notification.text = QStringLiteral("Time for a short break: 5 minutes.");
        notification.iconName = QStringLiteral("stream.sugo.app");
        notification.actions.append({QStringLiteral("Start Short Break"), [log](const QString &token) {
                                         *log << QStringLiteral("start:") + token;
                                     }});
        notification.actions.append({QStringLiteral("Skip Break"), [log](const QString &token) {
                                         *log << QStringLiteral("skip:") + token;
                                     }});
        notification.defaultActionLabel = QStringLiteral("View");
        notification.defaultAction = [log](const QString &token) {
            *log << QStringLiteral("default:") + token;
        };
        return notification;
    }

    static Notifier::Reply sampleReply(QStringList *log)
    {
        Notifier::Reply reply;
        reply.label = QStringLiteral("Log Task");
        reply.placeholder = QStringLiteral("What did you accomplish?");
        reply.submitLabel = QStringLiteral("Save");
        reply.submitIconName = QStringLiteral("document-save");
        reply.replied = [log](const QString &text) {
            *log << QStringLiteral("replied:") + text;
        };
        reply.fallback = [log](const QString &token) {
            *log << QStringLiteral("fallback:") + token;
        };
        return reply;
    }

private Q_SLOTS:
    void initTestCase()
    {
        if (!m_bus.start()) {
            QSKIP("dbus-daemon is not available");
        }
        QDBusConnection::connectToBus(m_bus.address(), QStringLiteral("server"));
        QDBusConnection::connectToBus(m_bus.address(), QStringLiteral("client"));
        QVERIFY(serverBus().isConnected());
        QVERIFY(clientBus().isConnected());
        // A missing server is provoked on purpose below; keep the log readable.
        QLoggingCategory::setFilterRules(QStringLiteral("sugo.platform.notifier.warning=false"));
    }

    void cleanupTestCase()
    {
        m_notifier.reset();
        QDBusConnection::disconnectFromBus(QStringLiteral("client"));
        QDBusConnection::disconnectFromBus(QStringLiteral("server"));
    }

    void init()
    {
        m_server = std::make_unique<FakeNotificationServer>(serverBus());
        publishServer();
        m_notifier = std::make_unique<FreedesktopNotifier>(clientBus(), QStringLiteral("Sugo"), QStringLiteral("stream.sugo.app"));
        m_context = std::make_unique<QObject>();
    }

    void cleanup()
    {
        m_notifier.reset();
        m_context.reset();
        unpublishServer();
        m_server.reset();
    }

    void sendsTheNotificationWithItsActions()
    {
        QStringList log;
        QVERIFY(show(sample(&log)) > 0);
        const auto &call = m_server->calls.constLast();
        QCOMPARE(call.appName, QStringLiteral("Sugo"));
        QCOMPARE(call.replacesId, 0U);
        QCOMPARE(call.icon, QStringLiteral("stream.sugo.app"));
        QCOMPARE(call.summary, QStringLiteral("Work session finished"));
        QCOMPARE(call.body, QStringLiteral("Time for a short break: 5 minutes."));
        QCOMPARE(call.actions,
                 (QStringList{QStringLiteral("default"), QStringLiteral("View"), QStringLiteral("action-0"), QStringLiteral("Start Short Break"),
                              QStringLiteral("action-1"), QStringLiteral("Skip Break")}));
        QCOMPARE(call.hints.value(QStringLiteral("desktop-entry")).toString(), QStringLiteral("stream.sugo.app"));
        // Sugo plays its own sound.
        QCOMPARE(call.hints.value(QStringLiteral("suppress-sound")).toBool(), true);
        QCOMPARE(call.timeout, -1);
        QVERIFY(log.isEmpty());
    }

    void theBodyIsEscapedWhereTheServerReadsMarkup_data()
    {
        QTest::addColumn<bool>("markup");
        QTest::addColumn<QString>("body");
        const QString text = QStringLiteral("Next: <Deep work> & \"notes\"");
        QTest::newRow("body-markup") << true << QStringLiteral("Next: &lt;Deep work&gt; &amp; &quot;notes&quot;");
        QTest::newRow("plain text") << false << text;
    }

    void theBodyIsEscapedWhereTheServerReadsMarkup()
    {
        // A server that lists body-markup reads the body as markup: a timer name or a task note
        // with "<" or "&" would be cut or mangled. The summary is plain text in either case.
        QFETCH(bool, markup);
        QFETCH(QString, body);
        if (markup) {
            m_server->capabilities << QStringLiteral("body-markup");
        }
        QStringList log;
        auto notification = sample(&log);
        notification.title = QStringLiteral("Break <is> over & out");
        notification.text = QStringLiteral("Next: <Deep work> & \"notes\"");
        QVERIFY(show(notification) > 0);
        const auto &call = m_server->calls.constLast();
        QCOMPARE(call.body, body);
        QCOMPARE(call.summary, QStringLiteral("Break <is> over & out"));
    }

    void anInvokedActionRunsItsHandlerWithTheActivationToken()
    {
        QStringList log;
        const uint id = show(sample(&log));
        QVERIFY(id > 0);
        serverSays(QStringLiteral("ActivationToken"), {id, QStringLiteral("token-1")});
        serverSays(QStringLiteral("ActionInvoked"), {id, QStringLiteral("action-1")});
        QCOMPARE(log, QStringList{QStringLiteral("skip:token-1")});
        serverSays(QStringLiteral("ActionInvoked"), {id, QStringLiteral("default")});
        QCOMPARE(log.constLast(), QStringLiteral("default:token-1"));
    }

    void theReplyFieldGoesToAServerThatHasIt()
    {
        m_server->capabilities << QStringLiteral("inline-reply");
        QStringList log;
        auto notification = sample(&log);
        notification.reply = sampleReply(&log);
        const uint id = show(notification);
        QVERIFY(id > 0);

        const auto &call = m_server->calls.constLast();
        QCOMPARE(call.actions.mid(6), (QStringList{QStringLiteral("inline-reply"), QStringLiteral("Log Task")}));
        QCOMPARE(call.hints.value(QStringLiteral("x-kde-reply-placeholder-text")).toString(), QStringLiteral("What did you accomplish?"));
        QCOMPARE(call.hints.value(QStringLiteral("x-kde-reply-submit-button-text")).toString(), QStringLiteral("Save"));
        QCOMPARE(call.hints.value(QStringLiteral("x-kde-reply-submit-button-icon-name")).toString(), QStringLiteral("document-save"));

        serverSays(QStringLiteral("NotificationReplied"), {id, QStringLiteral("Chapter two")});
        QCOMPARE(log, QStringList{QStringLiteral("replied:Chapter two")});
    }

    void elsewhereTheReplyIsAnOrdinaryAction()
    {
        QStringList log;
        auto notification = sample(&log);
        notification.reply = sampleReply(&log);
        const uint id = show(notification);
        QVERIFY(id > 0);

        const auto &call = m_server->calls.constLast();
        QCOMPARE(call.actions.mid(6), (QStringList{QStringLiteral("reply"), QStringLiteral("Log Task")}));
        QVERIFY(!call.hints.contains(QStringLiteral("x-kde-reply-placeholder-text")));

        serverSays(QStringLiteral("ActionInvoked"), {id, QStringLiteral("reply")});
        QCOMPARE(log, QStringList{QStringLiteral("fallback:")});
    }

    void aServerWithoutActionsGetsNone()
    {
        m_server->capabilities = QStringList{QStringLiteral("body")};
        QStringList log;
        auto notification = sample(&log);
        notification.reply = sampleReply(&log);
        QVERIFY(show(notification) > 0);
        QVERIFY(m_server->calls.constLast().actions.isEmpty());
    }

    void aClosedNotificationForgetsItsActions()
    {
        QStringList log;
        const uint id = show(sample(&log));
        QVERIFY(id > 0);
        serverSays(QStringLiteral("NotificationClosed"), {id, 2U});
        serverSays(QStringLiteral("ActionInvoked"), {id, QStringLiteral("action-0")});
        QVERIFY(log.isEmpty());
    }

    void actionsDoNothingOnceTheirContextIsGone()
    {
        QStringList log;
        const uint id = show(sample(&log));
        QVERIFY(id > 0);
        m_context.reset();
        serverSays(QStringLiteral("ActionInvoked"), {id, QStringLiteral("action-0")});
        QVERIFY(log.isEmpty());
    }

    void otherNotificationsAreIgnored()
    {
        QStringList log;
        const uint id = show(sample(&log));
        QVERIFY(id > 0);
        serverSays(QStringLiteral("ActionInvoked"), {id + 100, QStringLiteral("action-0")});
        serverSays(QStringLiteral("NotificationReplied"), {id, QStringLiteral("no reply field here")});
        QVERIFY(log.isEmpty());
    }

    void aRestartedServerDoesNotRunTheOldActions()
    {
        QStringList log;
        const uint id = show(sample(&log));
        QVERIFY(id > 0);
        unpublishServer();
        publishServer();
        QTest::qWait(100);
        serverSays(QStringLiteral("ActionInvoked"), {id, QStringLiteral("action-0")});
        QVERIFY(log.isEmpty());
    }

    void withoutAServerNothingIsShown()
    {
        unpublishServer();
        QStringList log;
        m_notifier->notify(m_context.get(), sample(&log));
        QTest::qWait(200);
        QVERIFY(m_server->calls.isEmpty());
        publishServer(); // for cleanup()
    }

    void withoutAServerTheFallbackShowsIt()
    {
        // A window manager with a tray but no notification daemon: the phase-end message
        // goes to the tray (the fallback) instead of being lost.
        auto fallback = std::make_unique<FakeNotifier>();
        FakeNotifier *tray = fallback.get();
        m_notifier->setFallback(std::move(fallback));
        unpublishServer();
        QStringList log;
        m_notifier->notify(m_context.get(), sample(&log));
        QTRY_COMPARE(tray->shown.size(), 1);
        QCOMPARE(tray->last().title, QStringLiteral("Work session finished"));
        QCOMPARE(tray->lastContext.data(), m_context.get());
        // Its handlers are the notification's own: a click on the message runs the default action.
        tray->last().defaultAction(QString());
        QCOMPARE(log, QStringList{QStringLiteral("default:")});

        // With a server again, the server shows it and the fallback is left alone.
        publishServer();
        QVERIFY(show(sample(&log)) > 0);
        QCOMPARE(tray->shown.size(), 1);
    }

    void aFailedCapabilitiesCallGoesToTheFallback()
    {
        // A server that is there but cannot answer (Plasma busy at login, a stuck daemon):
        // nothing has been sent yet, so the tray shows the message instead of nobody.
        auto fallback = std::make_unique<FakeNotifier>();
        FakeNotifier *tray = fallback.get();
        m_notifier->setFallback(std::move(fallback));
        m_server->capabilitiesFail = true;
        QStringList log;
        m_notifier->notify(m_context.get(), sample(&log));
        QTRY_COMPARE(tray->shown.size(), 1);
        QCOMPARE(tray->last().title, QStringLiteral("Work session finished"));
        QCOMPARE(tray->lastContext.data(), m_context.get());
        QTest::qWait(100);
        QVERIFY(m_server->calls.isEmpty());
    }

    void aFailedCapabilitiesCallStillSendsItWithoutAFallback()
    {
        // With nowhere else to go, the server still gets it, without the actions it may lack.
        m_server->capabilitiesFail = true;
        QStringList log;
        m_notifier->notify(m_context.get(), sample(&log));
        QTRY_COMPARE(m_server->calls.size(), 1);
        QCOMPARE(m_server->calls.at(0).summary, QStringLiteral("Work session finished"));
        QVERIFY(m_server->calls.at(0).actions.isEmpty());
    }
};

QTEST_GUILESS_MAIN(FreedesktopNotifierTest)
#include "tst_freedesktopnotifier.moc"
