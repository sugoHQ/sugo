// SPDX-License-Identifier: GPL-3.0-or-later
#include "Localization.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <functional>
#include <memory>
#include <utility>

#include "CalendarSync.h"
#include "FakeSecretStore.h"
#include "RedirectServer.h"
#include "SecretStore.h"
#include "Settings.h"

namespace
{
/// A group of the settings file, read and written directly (not through the object under test).
class DiskGroup
{
public:
    DiskGroup(Settings::Ptr settings, const QString &path)
        : m_settings(std::move(settings))
        , m_path(path)
    {
    }

    DiskGroup group(const QString &name) const { return {m_settings, m_path + QLatin1Char('/') + name}; }
    DiskGroup parent() const { return {m_settings, m_path.section(QLatin1Char('/'), 0, -2)}; }
    bool hasKey(const char *key) const { return m_settings->contains(path(key)); }
    QString readEntry(const char *key, const QString &fallback) const { return m_settings->value(path(key), fallback).toString(); }
    bool readEntry(const char *key, bool fallback) const { return m_settings->value(path(key), fallback).toBool(); }
    void writeEntry(const char *key, const QString &value) { m_settings->setValue(path(key), value); }
    void writeEntry(const char *key, const char *value) { writeEntry(key, QString::fromUtf8(value)); }
    void writeEntry(const char *key, bool value) { m_settings->setValue(path(key), value); }
    void sync() { Settings::sync(*m_settings); }

private:
    QString path(const char *key) const { return m_path + QLatin1Char('/') + QLatin1String(key); }

    Settings::Ptr m_settings;
    QString m_path;
};

const QString kPassword = QStringLiteral("s3cretPW-xyz");

QByteArray basic(const QString &user, const QString &pw)
{
    return "Basic " + (user + QLatin1Char(':') + pw).toUtf8().toBase64();
}

QByteArray multistatus(const QString &prefix)
{
    // Relative hrefs, special Nextcloud collections, a task-only list and a non-calendar entry.
    auto p = [&](const char *name) {
        const QString n = QString::fromLatin1(name);
        return prefix.isEmpty() ? n : prefix + QLatin1Char(':') + n;
    };
    auto collection = [&](const QString &href, const QString &display, const QString &comps, bool calendar) {
        QString s = QStringLiteral("<%1><%2>%3</%2><%4><%5>").arg(p("response"), p("href"), href, p("propstat"), p("prop"));
        s += QStringLiteral("<%1>").arg(p("resourcetype")) + QStringLiteral("<%1/>").arg(p("collection"));
        if (calendar) {
            s += QStringLiteral("<cal:calendar xmlns:cal=\"urn:ietf:params:xml:ns:caldav\"/>");
        }
        s += QStringLiteral("</%1>").arg(p("resourcetype"));
        if (!display.isEmpty()) {
            s += QStringLiteral("<%1>%2</%1>").arg(p("displayname"), display);
        }
        if (!comps.isEmpty()) {
            s += QStringLiteral("<cal:supported-calendar-component-set xmlns:cal=\"urn:ietf:params:xml:ns:caldav\">%1"
                                "</cal:supported-calendar-component-set>")
                     .arg(comps);
        }
        s += QStringLiteral("<ic:calendar-color xmlns:ic=\"http://apple.com/ns/ical/\">#0082c9FF</ic:calendar-color>");
        s += QStringLiteral("</%1><%2>HTTP/1.1 200 OK</%2></%3></%4>").arg(p("prop"), p("status"), p("propstat"), p("response"));
        return s;
    };
    const QString vevent = QStringLiteral("<cal:comp xmlns:cal=\"urn:ietf:params:xml:ns:caldav\" name=\"VEVENT\"/>");
    const QString vtodo = QStringLiteral("<cal:comp xmlns:cal=\"urn:ietf:params:xml:ns:caldav\" name=\"VTODO\"/>");
    QString xml = QStringLiteral("<?xml version=\"1.0\"?><%1 xmlns:%2=\"DAV:\">").arg(p("multistatus"), prefix.isEmpty() ? QStringLiteral("x") : prefix);
    if (prefix.isEmpty()) {
        xml = QStringLiteral("<?xml version=\"1.0\"?><multistatus xmlns=\"DAV:\">");
    }
    xml += collection(QStringLiteral("/remote.php/dav/calendars/alice/"), QString(), QString(), false);
    xml += collection(QStringLiteral("/remote.php/dav/calendars/alice/inbox/"), QString(), vevent, true);
    xml += collection(QStringLiteral("/remote.php/dav/calendars/alice/outbox/"), QString(), vevent, true);
    xml += collection(QStringLiteral("/remote.php/dav/calendars/alice/trashbin/"), QString(), vevent, true);
    xml += collection(QStringLiteral("/remote.php/dav/calendars/alice/personal/"), QStringLiteral("Personal"), vevent + vtodo, true);
    xml += collection(QStringLiteral("work-team/"), QStringLiteral("Work"), vevent, true);
    xml += collection(QStringLiteral("/remote.php/dav/calendars/alice/tasks/"), QStringLiteral("Tasks"), vtodo, true);
    xml += collection(QStringLiteral("https://other.example.org/remote.php/dav/calendars/alice/evil/"), QStringLiteral("Evil"), vevent, true);
    xml += prefix.isEmpty() ? QStringLiteral("</multistatus>") : QStringLiteral("</%1>").arg(p("multistatus"));
    return xml.toUtf8();
}
} // namespace

class FakeServer : public QObject
{
public:
    struct Request {
        QByteArray method;
        QByteArray path;
        QByteArray body;
        QHash<QByteArray, QByteArray> headers;
        qint64 at = 0;
    };

    FakeServer()
    {
        m_clock.start();
        const bool ok = m_server.listen(QHostAddress::LocalHost, 0);
        Q_ASSERT(ok);
        Q_UNUSED(ok);
        connect(&m_server, &QTcpServer::newConnection, this, &FakeServer::onConnection);
    }

    ~FakeServer() override
    {
        for (QTcpSocket *s : m_buffers.keys()) {
            QObject::disconnect(s, nullptr, this, nullptr);
        }
    }

    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

    int count(const QByteArray &method, const QByteArray &prefix = QByteArray()) const
    {
        int n = 0;
        for (const Request &r : requests) {
            if (r.method == method && r.path.startsWith(prefix)) {
                ++n;
            }
        }
        return n;
    }
    QList<Request> of(const QByteArray &method) const
    {
        QList<Request> out;
        for (const Request &r : requests) {
            if (r.method == method) {
                out.append(r);
            }
        }
        return out;
    }

    QList<Request> requests;
    QByteArray expectedAuth;           ///< Required on the DAV paths when not empty.
    QList<int> putStatuses;            ///< Consumed one per PUT, then defaultPut.
    int defaultPut = 201;
    QByteArray putRetryAfter;          ///< Retry-After of a PUT that is not accepted, when not empty.
    QByteArray putAnswer;              ///< The body of every PUT's answer.
    int deleteStatus = 204;
    int propfindStatus = 0;            ///< Answers every PROPFIND with this status when not 0.
    QByteArray forbiddenPropfindPath;  ///< A PROPFIND of this path is answered with 403.
    QString prefix;                    ///< XML namespace prefix of the multistatus.
    int pollsBefore200 = 2;
    int polls = 0;
    QString loginServer;               ///< `server` in the final login answer.
    QString loginPage;                 ///< `login` (the page to open) in the first answer; empty: on this server.
    QString loginName = QStringLiteral("alice");
    QString userId;                    ///< Names the DAV folders; empty: the login name.
    bool principalLookup = true;       ///< Says where the user's calendars are, as Nextcloud does.
    QString appPassword = kPassword;
    QByteArray token = "tok123";
    bool onlyOneCalendar = false;
    bool noPersonal = false;
    QHash<QByteArray, QByteArray> stored;
    std::function<void(const Request &)> onRequest; ///< Called before each request is answered.
    std::function<bool(const Request &)> hold;      ///< A request it returns true for waits for releaseHeld().
    int heldCount() const { return int(m_held.size()); }
    /// Answers the requests held so far, as they would have been answered now.
    void releaseHeld()
    {
        const auto held = std::exchange(m_held, {});
        for (const auto &[s, req] : held) {
            if (s) {
                respond(s, req);
            }
        }
    }

private:
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
            req.at = m_clock.elapsed();
            requests.append(req);
            if (onRequest) {
                onRequest(req);
            }
            if (hold && hold(req)) {
                m_held.append({QPointer<QTcpSocket>(s), req});
                continue;
            }
            respond(s, req);
        }
    }

    void reply(QTcpSocket *s, int status, const QByteArray &body = QByteArray(), const QByteArray &type = "application/json",
               const QByteArray &retryAfter = QByteArray())
    {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Type: " + type + "\r\n";
        if (!retryAfter.isEmpty()) {
            out += "Retry-After: " + retryAfter + "\r\n";
        }
        out += "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: keep-alive\r\n\r\n" + body;
        s->write(out);
        s->flush();
    }

    void respond(QTcpSocket *s, const Request &req)
    {
        const QByteArray path = req.path;
        if (path == "/index.php/login/v2") {
            const QByteArray json = "{\"poll\":{\"token\":\"" + token + "\",\"endpoint\":\"" + base().toUtf8()
                + "/login/v2/poll\"},\"login\":\"" + (loginPage.isEmpty() ? base() + QStringLiteral("/login/v2/flow/abc") : loginPage).toUtf8() + "\"}";
            reply(s, 200, json);
            return;
        }
        if (path == "/login/v2/poll") {
            ++polls;
            if (req.body != "token=" + token || polls <= pollsBefore200) {
                reply(s, 404, "{}");
                return;
            }
            const QString server = loginServer.isEmpty() ? base() : loginServer;
            reply(s, 200,
                  QStringLiteral("{\"server\":\"%1\",\"loginName\":\"%2\",\"appPassword\":\"%3\"}").arg(server, loginName, appPassword).toUtf8());
            return;
        }
        if (!expectedAuth.isEmpty() && req.headers.value("authorization") != expectedAuth) {
            reply(s, 401, "no");
            return;
        }
        if (req.method == "PROPFIND" && propfindStatus != 0) {
            reply(s, propfindStatus);
            return;
        }
        if (req.method == "PROPFIND" && !forbiddenPropfindPath.isEmpty() && path == forbiddenPropfindPath) {
            reply(s, 403);
            return;
        }
        if (req.method == "PROPFIND") {
            QByteArray body;
            const QByteArray principal = "/remote.php/dav/principals/users/" + user() + "/";
            if (path.startsWith("/custom/calendar")) {
                body = "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>/custom/calendar/</d:href><d:propstat><d:prop>"
                       "<d:resourcetype><d:collection/><c:calendar xmlns:c=\"urn:ietf:params:xml:ns:caldav\"/></d:resourcetype>"
                       "<d:displayname>Mine</d:displayname></d:prop></d:propstat></d:response>"
                       "<d:response><d:href>/custom/calendar/one.ics</d:href><d:propstat><d:prop><d:resourcetype/></d:prop></d:propstat></d:response>"
                       "</d:multistatus>";
            } else if (path == "/remote.php/dav/" && principalLookup) {
                body = "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>/remote.php/dav/</d:href><d:propstat><d:prop>"
                       "<d:current-user-principal><d:href>" + principal + "</d:href></d:current-user-principal>"
                       "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>";
            } else if (path == principal) {
                const QByteArray home = "/remote.php/dav/calendars/" + user() + "/";
                body = "<d:multistatus xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:response><d:href>" + principal
                    + "</d:href><d:propstat><d:prop><c:calendar-home-set><d:href>" + home + "</d:href></c:calendar-home-set>"
                      "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>";
            } else if (path == "/remote.php/dav/calendars/alice/") {
                body = multistatus(prefix);
                if (noPersonal) {
                    body.replace("/personal/", "/private/");
                    body.replace("Personal", "Private");
                }
                if (onlyOneCalendar) {
                    body.replace("work-team/", "inbox/");
                }
            } else {
                reply(s, 404);
                return;
            }
            reply(s, 207, body, "application/xml");
            return;
        }
        if (req.method == "PUT") {
            const int status = putStatuses.isEmpty() ? defaultPut : putStatuses.takeFirst();
            if (status >= 200 && status < 300) {
                stored.insert(path, req.body);
            }
            reply(s, status, putAnswer, "application/json", status >= 200 && status < 300 ? QByteArray() : putRetryAfter);
            return;
        }
        if (req.method == "DELETE") {
            if (deleteStatus >= 200 && deleteStatus < 300) {
                stored.remove(path);
            }
            reply(s, deleteStatus);
            return;
        }
        reply(s, 405);
    }

    QByteArray user() const { return (userId.isEmpty() ? loginName : userId).toUtf8(); }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QList<std::pair<QPointer<QTcpSocket>, Request>> m_held;
    QElapsedTimer m_clock;
};

class CalendarSyncTest : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<FakeServer> m_server;
    std::unique_ptr<CalendarSync> m_sync;
    QList<QUrl> m_opened;
    int m_counter = 0;

    QString configPath() const { return m_dir->filePath(QStringLiteral("settings.conf")); }

    std::unique_ptr<CalendarSync> makeSync()
    {
        auto config = Settings::open(configPath());
        auto sync = std::make_unique<CalendarSync>(config);
        sync->setRetryDelaysForTesting({40, 80});
        sync->setLoginPollIntervalForTesting(20);
        sync->setOpenUrlHandler([this](const QUrl &url) { m_opened.append(url); });
        return sync;
    }

    std::unique_ptr<CalendarSync> makeSyncWithKeyring(const FakeSecretStore &keyring)
    {
        auto config = Settings::open(configPath());
        // As createSecretStore() gives it: a refused secret is not asked for again.
        auto store = std::make_unique<AskOnceSecretStore>(std::make_unique<FakeSecretStore>(keyring));
        auto sync = std::make_unique<CalendarSync>(config, std::move(store));
        sync->setRetryDelaysForTesting({40, 80});
        sync->setLoginPollIntervalForTesting(20);
        sync->setOpenUrlHandler([this](const QUrl &url) { m_opened.append(url); });
        return sync;
    }

    QByteArray configFileContents() const
    {
        QFile f(configPath());
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    /// A configured, enabled custom CalDAV account pointing at the fake server.
    void configureCustom()
    {
        m_server->expectedAuth = basic(QStringLiteral("alice"), kPassword);
        m_sync->setProvider(QStringLiteral("caldav"));
        m_sync->setUsername(QStringLiteral("alice"));
        m_sync->setPassword(kPassword);
        m_sync->setCalendarUrl(m_server->base() + QStringLiteral("/cal/personal/"));
        m_sync->setEnabled(true);
    }

    static SessionRecord record(qint64 id = 7)
    {
        SessionRecord r;
        r.id = id;
        r.kind = SessionKind::Work;
        r.startedAtMs = 1700000000000;
        r.endedAtMs = r.startedAtMs + 25 * 60 * 1000;
        r.durationSec = 25 * 60;
        r.plannedSec = 25 * 60;
        r.completed = true;
        r.presetName = QStringLiteral("Deep work");
        r.category = QStringLiteral("Study");
        return r;
    }

    static QString unfold(const QByteArray &ics)
    {
        QByteArray copy = ics;
        copy.replace("\r\n ", "");
        return QString::fromUtf8(copy);
    }

    void checkNoSecrets()
    {
        QVERIFY(!m_sync->statusText().contains(kPassword));
        QVERIFY(!m_sync->lastError().contains(kPassword));
        QVERIFY(!m_sync->statusText().contains(QString::fromLatin1(basic(QStringLiteral("alice"), kPassword).mid(6))));
        QVERIFY(!m_sync->lastError().contains(QString::fromLatin1(basic(QStringLiteral("alice"), kPassword).mid(6))));
    }

    DiskGroup diskGroup(const QString &sub = QString())
    {
        const DiskGroup g(Settings::open(configPath()), QStringLiteral("Calendar"));
        return sub.isEmpty() ? g : g.group(sub);
    }

    void fillNextcloud(CalendarSync *s = nullptr)
    {
        s = s ? s : m_sync.get();
        s->setProvider(QStringLiteral("nextcloud"));
        s->setServerUrl(QStringLiteral("https://cloud.example.org"));
        s->setUsername(QStringLiteral("alice"));
        s->setPassword(QStringLiteral("nc-password-1"));
        s->selectCalendar(QStringLiteral("https://cloud.example.org/cal/personal"), QStringLiteral("Personal"));
    }

    void fillCaldav(CalendarSync *s = nullptr)
    {
        s = s ? s : m_sync.get();
        s->setProvider(QStringLiteral("caldav"));
        s->setServerUrl(QStringLiteral("https://dav.example.net"));
        s->setUsername(QStringLiteral("bob"));
        s->setPassword(QStringLiteral("cd-password-2"));
        s->selectCalendar(QStringLiteral("https://dav.example.net/cal/work"), QStringLiteral("Work"));
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        qRegisterMetaType<SessionRecord>();
    }

    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        m_server = std::make_unique<FakeServer>();
        m_sync = makeSync();
        m_opened.clear();
    }

    void cleanup()
    {
        m_sync.reset();
        m_server.reset();
        m_dir.reset();
    }

    // ----- iCalendar -----

    void icsBasics()
    {
        SessionRecord r = record();
        r.note = QStringLiteral("a;b,c\\d\nline2");
        const QDateTime stamp = QDateTime::fromMSecsSinceEpoch(1700000100000, QTimeZone::UTC);
        const QString uid = CalendarSync::uidFor(r);
        QCOMPARE(uid, QStringLiteral("sugo-7-1700000000000@io.github.mineraleyt.sugo"));
        QCOMPARE(CalendarSync::uidFor(r), uid); // stable

        const QByteArray ics = CalendarSync::buildIcs(r, false, uid, stamp);
        QVERIFY(ics.startsWith("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Sugo//EN\r\n"));
        QVERIFY(ics.endsWith("END:VEVENT\r\nEND:VCALENDAR\r\n"));
        QVERIFY(ics.contains("UID:sugo-7-1700000000000@io.github.mineraleyt.sugo\r\n"));
        QVERIFY(ics.contains("DTSTAMP:20231114T221500Z\r\n"));
        QVERIFY(ics.contains("DTSTART:20231114T221320Z\r\n"));
        QVERIFY(ics.contains("DTEND:20231114T223820Z\r\n"));
        QVERIFY(ics.contains("SUMMARY:Deep work\r\n"));
        QVERIFY(ics.contains("CATEGORIES:Study\r\n"));
        QVERIFY(ics.contains("STATUS:CONFIRMED\r\n"));
        QVERIFY(ics.contains("TRANSP:OPAQUE\r\n"));
        QVERIFY(!ics.contains("DESCRIPTION"));
        QVERIFY(!ics.contains("a;b"));
        QCOMPARE(CalendarSync::buildIcs(r, false, uid, stamp), ics);
        // No bare line feeds.
        QByteArray stripped = ics;
        stripped.replace("\r\n", "");
        QVERIFY(!stripped.contains('\n') && !stripped.contains('\r'));

        const QByteArray withNote = CalendarSync::buildIcs(r, true, uid, stamp);
        QVERIFY2(withNote.contains("DESCRIPTION:a\\;b\\,c\\\\d\\nline2\r\n"), withNote.constData());

        r.presetName.clear();
        r.category.clear();
        const QByteArray fallback = CalendarSync::buildIcs(r, true, uid, stamp);
        QVERIFY(fallback.contains("SUMMARY:Focus session\r\n"));
        QVERIFY(!fallback.contains("CATEGORIES"));
    }

    void icsEscapeText()
    {
        QCOMPARE(CalendarSync::escapeText(QStringLiteral("a\\b;c,d\r\ne\rf\ng")), QStringLiteral("a\\\\b\\;c\\,d\\ne\\nf\\ng"));
    }

    void icsFoldingMultibyte()
    {
        for (const QString &unit : {QStringLiteral("è"), QStringLiteral("日本"), QStringLiteral("😀"), QStringLiteral("a")}) {
            SessionRecord r = record();
            r.presetName = unit.repeated(120) + QStringLiteral(" end");
            const QString uid = CalendarSync::uidFor(r);
            const QByteArray ics = CalendarSync::buildIcs(r, false, uid);
            const QList<QByteArray> lines = ics.split('\n');
            for (QByteArray line : lines) {
                line.chop(line.endsWith('\r') ? 1 : 0);
                QVERIFY2(line.size() <= 75, "line too long");
                // Every physical line is valid UTF-8 on its own.
                QCOMPARE(QString::fromUtf8(line).toUtf8(), line);
            }
            QVERIFY(unfold(ics).contains(QStringLiteral("SUMMARY:") + r.presetName + QStringLiteral("\r\n")));
        }
        const QByteArray folded = CalendarSync::foldLine(QStringLiteral("X:") + QString(200, QLatin1Char('a')));
        QVERIFY(folded.startsWith("X:"));
        QCOMPARE(folded.indexOf("\r\n"), 75);
        QVERIFY(folded.contains("\r\n a"));
    }

    // ----- URLs -----

    void urlRules()
    {
        QString error;
        QCOMPARE(CalendarSync::normalizeUrl(QStringLiteral("  https://cloud.example.org/// "), &error), QStringLiteral("https://cloud.example.org"));
        QVERIFY(error.isEmpty());
        QCOMPARE(CalendarSync::normalizeUrl(QStringLiteral("https://h.org/dav/cal/"), &error), QStringLiteral("https://h.org/dav/cal"));
        QCOMPARE(CalendarSync::normalizeUrl(QStringLiteral("http://127.0.0.1:5232/"), &error), QStringLiteral("http://127.0.0.1:5232"));
        QCOMPARE(CalendarSync::normalizeUrl(QStringLiteral("http://localhost/x/"), &error), QStringLiteral("http://localhost/x"));
        QCOMPARE(CalendarSync::normalizeUrl(QStringLiteral(""), &error), QString());
        QVERIFY(error.isEmpty());

        // Typed without its scheme: an https address.
        QCOMPARE(CalendarSync::normalizeUrl(QStringLiteral(" cloud.example.org/ "), &error), QStringLiteral("https://cloud.example.org"));
        QCOMPARE(CalendarSync::normalizeUrl(QStringLiteral("cloud.example.org:8443/nc"), &error), QStringLiteral("https://cloud.example.org:8443/nc"));
        QVERIFY(error.isEmpty());

        for (const QString &bad : {QStringLiteral("http://cloud.example.org"),
                                   QStringLiteral("ftp://cloud.example.org"), QStringLiteral("https://alice:pw@cloud.example.org"),
                                   QStringLiteral("https://")}) {
            error.clear();
            QVERIFY2(CalendarSync::normalizeUrl(bad, &error).isEmpty(), qPrintable(bad));
            QVERIFY2(!error.isEmpty(), qPrintable(bad));
        }
        QVERIFY(CalendarSync::isUrlAllowed(QUrl(QStringLiteral("https://x.org/"))));
        QVERIFY(CalendarSync::isUrlAllowed(QUrl(QStringLiteral("http://[::1]:8080/"))));
        QVERIFY(!CalendarSync::isUrlAllowed(QUrl(QStringLiteral("http://x.org/"))));
        QVERIFY(!CalendarSync::isUrlAllowed(QUrl(QStringLiteral("http://127.0.0.1.evil.org/"))));
    }

    void settersRejectInsecureUrls()
    {
        QSignalSpy rejected(m_sync.get(), &CalendarSync::inputRejected);
        m_sync->setServerUrl(QStringLiteral("http://cloud.example.org"));
        QVERIFY(m_sync->serverUrl().isEmpty());
        QVERIFY(!m_sync->lastError().isEmpty());
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(rejected.takeFirst().at(0).toString(), m_sync->lastError());
        m_sync->setCalendarUrl(QStringLiteral("http://cloud.example.org/dav"));
        QVERIFY(m_sync->calendarUrl().isEmpty());
        QCOMPARE(rejected.count(), 1);
        QVERIFY(!rejected.takeFirst().at(0).toString().isEmpty());
        // The same refusal twice is announced twice: lastErrorChanged alone would not tell.
        m_sync->setCalendarUrl(QStringLiteral("http://cloud.example.org/dav"));
        QCOMPARE(rejected.count(), 1);
        rejected.clear();
        m_sync->setServerUrl(QStringLiteral("https://cloud.example.org/"));
        QCOMPARE(m_sync->serverUrl(), QStringLiteral("https://cloud.example.org"));
        QCOMPARE(rejected.count(), 0);
    }

    void plainHttpToRemoteHostSendsNothing()
    {
        // A hand-edited config with an insecure address is ignored on load.
        {
            DiskGroup g = diskGroup();
            g.writeEntry("Provider", "caldav");
            g = g.group(QStringLiteral("caldav"));
            g.writeEntry("CalendarUrl", "http://cloud.example.org/cal");
            g.writeEntry("Username", "alice");
            g.writeEntry("AppPassword", kPassword);
            g.parent().writeEntry("Enabled", true);
            g.sync();
        }
        auto sync = makeSync();
        QVERIFY(sync->calendarUrl().isEmpty());
        sync->enqueueWorkSession(record());
        // Kept for a proper (https) calendar, but nothing goes anywhere.
        QTest::qWait(150);
        QVERIFY(m_server->requests.isEmpty());
        QCOMPARE(sync->status(), CalendarSync::NotConfigured);
    }

    // ----- Discovery -----

    void parseCalendarsColours_data()
    {
        QTest::addColumn<QString>("sent");
        QTest::addColumn<QString>("shown");
        // CalDAV's #RRGGBBAA would be #AARRGGBB to QML: the alpha is left out.
        QTest::newRow("rgba") << QStringLiteral("#0082C9FF") << QStringLiteral("#0082c9");
        QTest::newRow("rgba transparent") << QStringLiteral("#FF000000") << QStringLiteral("#ff0000");
        QTest::newRow("rgb") << QStringLiteral("#3a87ad") << QStringLiteral("#3a87ad");
        QTest::newRow("spaces") << QStringLiteral("  #3A87AD ") << QStringLiteral("#3a87ad");
        QTest::newRow("short") << QStringLiteral("#f0f") << QStringLiteral("#ff00ff");
        // Nextcloud keeps what a client sent, "#" or not.
        QTest::newRow("rgb without #") << QStringLiteral("3A87AD") << QStringLiteral("#3a87ad");
        QTest::newRow("rgba without #") << QStringLiteral("0082c9ff") << QStringLiteral("#0082c9");
        QTest::newRow("name") << QStringLiteral("teal") << QStringLiteral("#008080");
        // None, or none that can be read: the one Nextcloud's web interface makes from the name "Home".
        QTest::newRow("none") << QString() << QStringLiteral("#bc5c91");
        QTest::newRow("not a colour") << QStringLiteral("#12345") << QStringLiteral("#bc5c91");
        QTest::newRow("garbage") << QStringLiteral("blue-ish") << QStringLiteral("#bc5c91");
    }
    void parseCalendarsColours()
    {
        QFETCH(QString, sent);
        QFETCH(QString, shown);
        const QString xml =
            QStringLiteral("<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\" "
                           "xmlns:a=\"http://apple.com/ns/ical/\"><d:response><d:href>/dav/cal/home/</d:href><d:propstat><d:prop>"
                           "<d:resourcetype><d:collection/><c:calendar/></d:resourcetype><d:displayname>Home</d:displayname>"
                           "<a:calendar-color>%1</a:calendar-color></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
                           "</d:response></d:multistatus>")
                .arg(sent);
        const auto cals = CalendarSync::parseCalendars(xml.toUtf8(), QUrl(QStringLiteral("https://dav.example.net/dav/cal/")));
        QCOMPARE(cals.size(), 1);
        QCOMPARE(cals.at(0).color, shown);
    }

    void parseCalendarsColourFromName()
    {
        // Values worked out with the JavaScript of nextcloud/calendar (uidToColor).
        const auto colourOf = [](const QString &name) {
            const QString xml = QStringLiteral("<d:multistatus xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:response>"
                                               "<d:href>/dav/cal/x/</d:href><d:propstat><d:prop><d:resourcetype><c:calendar/></d:resourcetype>"
                                               "<d:displayname>%1</d:displayname></d:prop></d:propstat></d:response></d:multistatus>")
                                    .arg(name);
            const auto cals = CalendarSync::parseCalendars(xml.toUtf8(), QUrl(QStringLiteral("https://dav.example.net/dav/cal/")));
            return cals.size() == 1 ? cals.at(0).color : QStringLiteral("missing");
        };
        QCOMPARE(colourOf(QStringLiteral("Orari dottore")), QStringLiteral("#d09e6d"));
        QCOMPARE(colourOf(QStringLiteral("Home")), QStringLiteral("#bc5c91"));
        QCOMPARE(colourOf(QString()), QString()); // no name, no colour
    }

    void parseCalendarsTolerant()
    {
        for (const QString &prefix : {QString(), QStringLiteral("d"), QStringLiteral("D"), QStringLiteral("ns0")}) {
            const QUrl request(QStringLiteral("https://cloud.example.org/remote.php/dav/calendars/alice/"));
            const auto cals = CalendarSync::parseCalendars(multistatus(prefix), request);
            QCOMPARE(cals.size(), 2);
            QCOMPARE(cals.at(0).name, QStringLiteral("Personal"));
            QCOMPARE(cals.at(0).url, QStringLiteral("https://cloud.example.org/remote.php/dav/calendars/alice/personal"));
            QCOMPARE(cals.at(0).color, QStringLiteral("#0082c9")); // sent as #0082c9FF
            QCOMPARE(cals.at(1).name, QStringLiteral("Work"));
            QCOMPARE(cals.at(1).url, QStringLiteral("https://cloud.example.org/remote.php/dav/calendars/alice/work-team"));
        }
        QVERIFY(CalendarSync::parseCalendars("<not xml", QUrl(QStringLiteral("https://x.org/"))).isEmpty());
    }

    void parseCalendarsSkipsPropertiesTheServerDoesNotHave()
    {
        // Properties asked for but not defined come back in a propstat of their own, with status
        // 404. An empty component set there says nothing: absent means any component (RFC 4791).
        const QByteArray xml =
            "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
            "<d:response><d:href>/dav/cal/home/</d:href>"
            "<d:propstat><d:prop><d:resourcetype><d:collection/><c:calendar/></d:resourcetype>"
            // (A status line with more than one space between its words is still read.)
            "<d:displayname>Home</d:displayname></d:prop><d:status>HTTP/1.1  200 OK</d:status></d:propstat>"
            "<d:propstat><d:prop><c:supported-calendar-component-set/><d:displayname>Not this</d:displayname></d:prop>"
            "<d:status>HTTP/1.1 404 Not Found</d:status></d:propstat>"
            "</d:response>"
            // Only a failed propstat says it is a calendar: it is not one.
            "<d:response><d:href>/dav/cal/other/</d:href>"
            "<d:propstat><d:prop><d:displayname>Other</d:displayname></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
            "<d:propstat><d:prop><d:resourcetype><c:calendar/></d:resourcetype></d:prop>"
            "<d:status>HTTP/1.1 403 Forbidden</d:status></d:propstat>"
            "</d:response>"
            "</d:multistatus>";
        const auto cals = CalendarSync::parseCalendars(xml, QUrl(QStringLiteral("https://dav.example.net/dav/cal/")));
        QCOMPARE(cals.size(), 1);
        QCOMPARE(cals.at(0).name, QStringLiteral("Home"));
        QCOMPARE(cals.at(0).url, QStringLiteral("https://dav.example.net/dav/cal/home"));
    }

    void parseCalendarsTakesTheDefaultPortForTheSameServer()
    {
        const auto response = [](const QString &href) {
            return QStringLiteral("<d:response><d:href>%1</d:href><d:propstat><d:prop><d:resourcetype><d:collection/>"
                                  "<c:calendar/></d:resourcetype></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
                                  "</d:response>")
                .arg(href);
        };
        const QString xml = QStringLiteral("<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\" "
                                           "xmlns:c=\"urn:ietf:params:xml:ns:caldav\">")
            + response(QStringLiteral("https://cloud.example.org/dav/cal/plain/"))
            + response(QStringLiteral("https://cloud.example.org:443/dav/cal/explicit/"))
            + response(QStringLiteral("https://cloud.example.org:8443/dav/cal/elsewhere/"))
            + QStringLiteral("</d:multistatus>");
        // The port written out or not, https://host and https://host:443 are the same server;
        // another port is not.
        for (const QString &request : {QStringLiteral("https://cloud.example.org/dav/cal/"), QStringLiteral("https://cloud.example.org:443/dav/cal/")}) {
            const auto cals = CalendarSync::parseCalendars(xml.toUtf8(), QUrl(request));
            QCOMPARE(cals.size(), 2);
            QVERIFY(cals.at(0).url.endsWith(QStringLiteral("/dav/cal/plain")));
            QVERIFY(cals.at(1).url.endsWith(QStringLiteral("/dav/cal/explicit")));
        }
    }

    void discoveryNextcloud()
    {
        m_server->expectedAuth = basic(QStringLiteral("alice"), kPassword);
        m_sync->setServerUrl(m_server->base());
        m_sync->setUsername(QStringLiteral("alice"));
        m_sync->setPassword(kPassword);
        QVERIFY(m_sync->hasPassword());
        QSignalSpy spy(m_sync.get(), &CalendarSync::calendarsChanged);
        m_sync->refreshCalendars();
        QCOMPARE(m_sync->status(), CalendarSync::Working);
        QTRY_VERIFY(!spy.isEmpty());
        const QVariantList cals = m_sync->calendars();
        QCOMPARE(cals.size(), 2);
        QCOMPARE(cals.at(0).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Personal"));
        QVERIFY(cals.at(1).toMap().value(QStringLiteral("url")).toString().endsWith(QStringLiteral("/work-team")));
        // The server says where the user's calendars are: principal, then calendar home.
        const auto propfind = m_server->of("PROPFIND");
        QCOMPARE(propfind.size(), 3);
        QCOMPARE(propfind.at(0).path, QByteArray("/remote.php/dav/"));
        QCOMPARE(propfind.at(0).headers.value("depth"), QByteArray("0"));
        QVERIFY(propfind.at(0).body.contains("current-user-principal"));
        QCOMPARE(propfind.at(1).path, QByteArray("/remote.php/dav/principals/users/alice/"));
        QCOMPARE(propfind.at(1).headers.value("depth"), QByteArray("0"));
        QVERIFY(propfind.at(1).body.contains("calendar-home-set"));
        QCOMPARE(propfind.at(2).path, QByteArray("/remote.php/dav/calendars/alice/"));
        QCOMPARE(propfind.at(2).headers.value("depth"), QByteArray("1"));
        QVERIFY(propfind.at(2).headers.value("user-agent").startsWith("Sugo/"));
        QVERIFY(propfind.at(2).body.contains("resourcetype"));
        for (const auto &req : propfind) {
            QCOMPARE(req.headers.value("authorization"), basic(QStringLiteral("alice"), kPassword));
        }
        // Not configured until a calendar is chosen.
        QCOMPARE(m_sync->status(), CalendarSync::NotConfigured);
        m_sync->selectCalendar(cals.at(1).toMap().value(QStringLiteral("url")).toString(), QStringLiteral("Work"));
        QCOMPARE(m_sync->status(), CalendarSync::Ready);
        QCOMPARE(m_sync->calendarName(), QStringLiteral("Work"));
        checkNoSecrets();
    }

    void discoveryNextcloudWithoutAPrincipalLooksUnderTheLoginName()
    {
        m_server->principalLookup = false; // the server does not say where the calendars are
        m_server->expectedAuth = basic(QStringLiteral("alice"), kPassword);
        m_sync->setServerUrl(m_server->base());
        m_sync->setUsername(QStringLiteral("alice"));
        m_sync->setPassword(kPassword);
        m_sync->refreshCalendars();
        QTRY_COMPARE(m_sync->calendars().size(), 2);
        const auto propfind = m_server->of("PROPFIND");
        QCOMPARE(propfind.size(), 2);
        QCOMPARE(propfind.at(0).path, QByteArray("/remote.php/dav/"));
        QCOMPARE(propfind.at(1).path, QByteArray("/remote.php/dav/calendars/alice/"));
    }

    void discoveryNextcloudForbiddenAtTheRootLooksUnderTheLoginName()
    {
        // The root is kept from this account (an ACL, a proxy rule): not a refused login, and
        // the calendars are where Nextcloud usually puts the login name's.
        m_server->forbiddenPropfindPath = "/remote.php/dav/";
        m_server->expectedAuth = basic(QStringLiteral("alice"), kPassword);
        m_sync->setServerUrl(m_server->base());
        m_sync->setUsername(QStringLiteral("alice"));
        m_sync->setPassword(kPassword);
        m_sync->refreshCalendars();
        QTRY_COMPARE(m_sync->calendars().size(), 2);
        const auto propfind = m_server->of("PROPFIND");
        QCOMPARE(propfind.size(), 2);
        QCOMPARE(propfind.at(0).path, QByteArray("/remote.php/dav/"));
        QCOMPARE(propfind.at(1).path, QByteArray("/remote.php/dav/calendars/alice/"));
        QVERIFY(m_sync->lastError().isEmpty());
        QVERIFY(!m_sync->isSendingPaused());
    }

    void discoveryNextcloudRefusedAtThePrincipalStopsThere()
    {
        // A wrong password: one refused request, not a second one at the fallback address (the
        // server's brute-force protection counts each).
        m_server->expectedAuth = basic(QStringLiteral("alice"), QStringLiteral("other"));
        m_sync->setServerUrl(m_server->base());
        m_sync->setUsername(QStringLiteral("alice"));
        m_sync->setPassword(kPassword);
        m_sync->refreshCalendars();
        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QVERIFY(m_sync->lastError().startsWith(QStringLiteral("Login rejected")));
        const auto propfind = m_server->of("PROPFIND");
        QCOMPARE(propfind.size(), 1);
        QCOMPARE(propfind.first().path, QByteArray("/remote.php/dav/"));
        QVERIFY(m_sync->calendars().isEmpty());
        checkNoSecrets();

        // The right password finds the calendars.
        m_sync->setPassword(QStringLiteral("other"));
        m_sync->refreshCalendars();
        QTRY_COMPARE(m_sync->calendars().size(), 2);
        QVERIFY(m_sync->lastError().isEmpty());
    }

    void discoveryCustomIsCalendar()
    {
        m_server->expectedAuth = basic(QStringLiteral("bob"), kPassword);
        m_sync->setProvider(QStringLiteral("caldav"));
        m_sync->setServerUrl(m_server->base() + QStringLiteral("/custom/calendar/"));
        m_sync->setUsername(QStringLiteral("bob"));
        m_sync->setPassword(kPassword);
        m_sync->refreshCalendars();
        QTRY_COMPARE(m_sync->calendars().size(), 1);
        QCOMPARE(m_sync->calendars().first().toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Mine"));
        QCOMPARE(m_sync->calendars().first().toMap().value(QStringLiteral("url")).toString(), m_server->base() + QStringLiteral("/custom/calendar"));
    }

    void discovery403DoesNotPauseSending()
    {
        // The server does not let this account list the place asked (an ACL on the principal or
        // the calendar home): a refusal of the address, not of the login. The calendar chosen
        // still gets the events.
        configureCustom();
        m_server->propfindStatus = 403;
        m_sync->refreshCalendars();
        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QVERIFY(!m_sync->isSendingPaused());
        QVERIFY(m_sync->lastError().contains(QStringLiteral("403")));
        QVERIFY(!m_sync->lastError().startsWith(QStringLiteral("Login rejected")));
        QVERIFY(!m_sync->statusText().startsWith(QStringLiteral("Login rejected")));
        checkNoSecrets();

        m_sync->enqueueWorkSession(record());
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QCOMPARE(m_server->stored.size(), 1);
    }

    void discoveryWrongPassword()
    {
        m_server->expectedAuth = basic(QStringLiteral("alice"), QStringLiteral("other"));
        m_sync->setServerUrl(m_server->base());
        m_sync->setUsername(QStringLiteral("alice"));
        m_sync->setPassword(kPassword);
        m_sync->refreshCalendars();
        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QVERIFY(m_sync->lastError().startsWith(QStringLiteral("Login rejected")));
        checkNoSecrets();
    }

    // ----- Sending -----

    void sendsEventOnCompletion()
    {
        configureCustom();
        QCOMPARE(m_sync->status(), CalendarSync::Ready);
        QVERIFY(!m_sync->lastSuccess().isValid());
        m_sync->enqueueWorkSession(record());
        QCOMPARE(m_sync->pendingCount(), 1);
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QVERIFY(m_sync->lastSuccess().isValid());
        const auto puts = m_server->of("PUT");
        QCOMPARE(puts.size(), 1);
        QCOMPARE(puts.first().path, QByteArray("/cal/personal/sugo-7-1700000000000@io.github.mineraleyt.sugo.ics"));
        QCOMPARE(puts.first().headers.value("content-type"), QByteArray("text/calendar; charset=utf-8"));
        QVERIFY(puts.first().headers.value("user-agent").startsWith("Sugo/"));
        QVERIFY(puts.first().body.contains("SUMMARY:Deep work"));
        QCOMPARE(m_sync->status(), CalendarSync::Ready);
        QVERIFY(m_sync->statusText().contains(QStringLiteral("alice")));
        checkNoSecrets();
    }

    void onlyCompletedWorkIsSent()
    {
        configureCustom();
        SessionRecord brk = record(1);
        brk.kind = SessionKind::ShortBreak;
        SessionRecord interrupted = record(2);
        interrupted.completed = false;
        m_sync->enqueueWorkSession(brk);
        m_sync->enqueueWorkSession(interrupted);
        QCOMPARE(m_sync->pendingCount(), 0);
        QTest::qWait(100);
        QCOMPARE(m_server->count("PUT"), 0);
    }

    void disabledSendsAndQueuesNothing()
    {
        configureCustom();
        m_sync->setEnabled(false);
        m_sync->enqueueWorkSession(record());
        QCOMPARE(m_sync->pendingCount(), 0);
        QTest::qWait(100);
        QVERIFY(m_server->requests.isEmpty());

        // Switching off drops what was waiting.
        m_server->defaultPut = 500;
        m_sync->setEnabled(true);
        m_sync->enqueueWorkSession(record());
        QTRY_VERIFY(m_server->count("PUT") >= 1);
        QCOMPARE(m_sync->pendingCount(), 1);
        m_sync->setEnabled(false);
        QCOMPARE(m_sync->pendingCount(), 0);
        const int puts = m_server->count("PUT");
        QTest::qWait(250);
        QCOMPARE(m_server->count("PUT"), puts);
    }

    void eventsFromBeforeTheAccountWasSetUpAreSentToIt()
    {
        // Turned on before signing in: the work done meanwhile is not lost...
        m_sync->setEnabled(true);
        m_sync->enqueueWorkSession(record(1));
        QCOMPARE(m_sync->pendingCount(), 1);
        QCOMPARE(m_sync->status(), CalendarSync::NotConfigured);
        QVERIFY(m_server->requests.isEmpty()); // ...and nothing is sent anywhere yet

        // It survives a restart, and goes to the account once that is complete.
        m_sync = makeSync();
        QCOMPARE(m_sync->pendingCount(), 1);
        configureCustom();
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QCOMPARE(m_server->stored.size(), 1);
        QVERIFY(m_server->of("PUT").first().path.contains("sugo-1-"));
    }

    void unboundEventsOfAnotherUserAreNotAdopted()
    {
        // Alice's user name was entered, but her password was never given.
        m_sync->setProvider(QStringLiteral("caldav"));
        m_sync->setUsername(QStringLiteral("alice"));
        m_sync->setEnabled(true);
        m_sync->enqueueWorkSession(record(1));
        QCOMPARE(m_sync->pendingCount(), 1);

        // Bob completes an account instead: Alice's event must not go to him.
        m_server->expectedAuth = basic(QStringLiteral("bob"), QStringLiteral("bobs-password"));
        m_sync->setUsername(QStringLiteral("bob"));
        m_sync->setPassword(QStringLiteral("bobs-password"));
        m_sync->setCalendarUrl(m_server->base() + QStringLiteral("/cal/personal/"));
        QCOMPARE(m_sync->pendingCount(), 0);
        QTest::qWait(150);
        QVERIFY(m_server->stored.isEmpty());
    }

    void turnedOffNothingIsQueued()
    {
        m_sync->enqueueWorkSession(record());
        QCOMPARE(m_sync->pendingCount(), 0);
        QVERIFY(m_server->requests.isEmpty());
    }

    void putAnswers_data()
    {
        QTest::addColumn<int>("http");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("201") << 201 << true;
        QTest::newRow("204") << 204 << true;
        QTest::newRow("200") << 200 << true;
    }
    void putAnswers()
    {
        QFETCH(int, http);
        QFETCH(bool, accepted);
        configureCustom();
        m_server->defaultPut = http;
        m_sync->enqueueWorkSession(record());
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QVERIFY(accepted);
    }

    void put401PausesUntilCredentialsChange()
    {
        configureCustom();
        m_server->expectedAuth = basic(QStringLiteral("alice"), QStringLiteral("rotated"));
        QSignalSpy failures(m_sync.get(), &CalendarSync::sendFailed);
        m_sync->enqueueWorkSession(record(1));
        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QCOMPARE(failures.count(), 1);
        QCOMPARE(failures.at(0).at(0).toString(), QStringLiteral("auth"));
        QVERIFY(m_sync->isSendingPaused());
        QVERIFY(m_sync->lastError().startsWith(QStringLiteral("Login rejected")));
        QVERIFY(m_sync->statusText().startsWith(QStringLiteral("Login rejected")));
        QCOMPARE(m_sync->pendingCount(), 1);

        m_sync->enqueueWorkSession(record(2));
        QCOMPARE(m_sync->pendingCount(), 2);
        QTest::qWait(250);
        QCOMPARE(m_server->count("PUT"), 1); // refused once, and not retried while paused
        QCOMPARE(m_server->requests.size(), 1);
        checkNoSecrets();

        m_sync->setPassword(QStringLiteral("rotated"));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QCOMPARE(m_server->count("PUT"), 3); // the refused one and the two sent
        QCOMPARE(m_sync->status(), CalendarSync::Ready);
    }

    void put401ResumesAfterSuccessfulTest()
    {
        configureCustom();
        m_server->expectedAuth = basic(QStringLiteral("alice"), QStringLiteral("rotated"));
        m_sync->enqueueWorkSession(record(1));
        QTRY_VERIFY(m_sync->isSendingPaused());
        m_server->expectedAuth = basic(QStringLiteral("alice"), kPassword); // the server side was fixed
        m_sync->testConnection();
        QTRY_COMPARE(m_sync->status(), CalendarSync::Ready);
        QVERIFY(!m_sync->isSendingPaused());
        QTRY_COMPARE(m_sync->pendingCount(), 0);
    }

    void a403TestAfterARejectedLoginLiftsThePause()
    {
        // Paused on a 401; then the password works on the server, but the calendar refuses the
        // test event. The login was taken: no "Login rejected" coming back, the event is tried.
        configureCustom();
        m_sync->setRetryDelaysForTesting({600000});
        m_server->expectedAuth = basic(QStringLiteral("alice"), QStringLiteral("rotated"));
        m_sync->enqueueWorkSession(record(1));
        QTRY_VERIFY(m_sync->isSendingPaused());
        m_server->expectedAuth = basic(QStringLiteral("alice"), kPassword);
        m_server->defaultPut = 403;
        m_sync->testConnection();
        QTRY_COMPARE(m_server->count("PUT"), 3); // refused (401), the test's, the event again
        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QVERIFY(!m_sync->isSendingPaused());
        QVERIFY(m_sync->lastError().contains(QStringLiteral("does not accept new events")));
        QTest::qWait(100);
        QVERIFY2(!m_sync->statusText().startsWith(QStringLiteral("Login rejected")), qPrintable(m_sync->statusText()));
        QCOMPARE(m_sync->pendingCount(), 1);
    }

    void a403LookupAfterARejectedLoginLiftsThePause()
    {
        configureCustom();
        m_server->expectedAuth = basic(QStringLiteral("alice"), QStringLiteral("rotated"));
        m_sync->enqueueWorkSession(record(1));
        QTRY_VERIFY(m_sync->isSendingPaused());
        m_server->expectedAuth = basic(QStringLiteral("alice"), kPassword);
        m_server->propfindStatus = 403;
        m_sync->refreshCalendars();
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QVERIFY(!m_sync->isSendingPaused());
        QTRY_COMPARE(m_sync->status(), CalendarSync::Ready);
    }

    void a403LookupKeepsThePauseOf403sInARow()
    {
        configureCustom();
        m_server->defaultPut = 403;
        m_sync->enqueueWorkSession(record(1));
        QTRY_VERIFY(m_sync->isSendingPaused());
        m_server->propfindStatus = 403;
        m_sync->refreshCalendars();
        QTRY_COMPARE(m_server->count("PROPFIND"), 1);
        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QTest::qWait(100);
        QVERIFY(m_sync->isSendingPaused());
        QCOMPARE(m_server->count("PUT"), 3); // nothing sent again with the credentials in doubt
    }

    void anEventAcceptedWhilePausedLiftsThePause()
    {
        // An event is on its way when three refused tests pause sending; the server then takes
        // that event: the pause is lifted and the events waiting behind it go too.
        configureCustom();
        m_server->hold = [](const FakeServer::Request &req) {
            return req.method == "PUT" && !req.path.contains("sugo-test-");
        };
        m_sync->enqueueWorkSession(record(1));
        QTRY_COMPARE(m_server->heldCount(), 1);
        m_server->hold = nullptr;
        m_sync->enqueueWorkSession(record(2));
        m_server->defaultPut = 403;
        for (int i = 0; i < 3; ++i) {
            m_sync->testConnection();
            QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        }
        QVERIFY(m_sync->isSendingPaused());
        QCOMPARE(m_sync->pendingCount(), 2);

        m_server->defaultPut = 201;
        m_server->releaseHeld();
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QVERIFY(!m_sync->isSendingPaused());
        QTRY_COMPARE(m_sync->status(), CalendarSync::Ready);
        QCOMPARE(m_server->count("PUT"), 5); // the held one, three tests, the second event
    }

    void put403IsTheCalendarRefusingNotTheLogin()
    {
        // A calendar shared read-only: the login is fine, the calendar does not take events.
        configureCustom();
        m_server->defaultPut = 403;
        m_sync->enqueueWorkSession(record());
        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QVERIFY(!m_sync->isSendingPaused());
        QVERIFY(m_sync->lastError().contains(QStringLiteral("403")));
        QVERIFY(!m_sync->lastError().startsWith(QStringLiteral("Login rejected")));
        QVERIFY(!m_sync->statusText().startsWith(QStringLiteral("Login rejected")));
        QCOMPARE(m_sync->pendingCount(), 1); // kept, not skipped
        checkNoSecrets();

        // Tried again later: once the calendar takes it, it is sent.
        m_server->defaultPut = 201;
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QTRY_COMPARE(m_sync->status(), CalendarSync::Ready);
        QVERIFY(m_sync->lastError().isEmpty());
    }

    void put403InARowPausesSending()
    {
        // Some servers answer a wrong password with 403: not retried for ever (an account lock),
        // but paused after three in a row, with a message that names both possible causes.
        configureCustom();
        m_server->defaultPut = 403;
        m_sync->enqueueWorkSession(record());
        QTRY_VERIFY(m_sync->isSendingPaused());
        QCOMPARE(m_server->count("PUT"), 3);
        QCOMPARE(m_sync->status(), CalendarSync::Failed);
        for (const QString &text : {m_sync->statusText(), m_sync->lastError()}) {
            QVERIFY2(text.contains(QStringLiteral("403")), qPrintable(text));
            QVERIFY2(text.contains(QStringLiteral("does not accept new events")), qPrintable(text));
            QVERIFY2(text.contains(QStringLiteral("app password")), qPrintable(text));
            QVERIFY2(!text.startsWith(QStringLiteral("Login rejected")), qPrintable(text));
        }
        QTest::qWait(250);
        QCOMPARE(m_server->count("PUT"), 3); // not retried while paused
        QCOMPARE(m_sync->pendingCount(), 1);
        checkNoSecrets();

        // A test that is refused too keeps it paused, with the same message.
        m_sync->testConnection();
        QTRY_COMPARE(m_server->count("PUT"), 4);
        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QVERIFY(m_sync->isSendingPaused());
        QVERIFY(m_sync->statusText().contains(QStringLiteral("app password")));

        // A new password is tried at once.
        m_server->expectedAuth = basic(QStringLiteral("alice"), QStringLiteral("rotated"));
        m_server->defaultPut = 201;
        m_sync->setPassword(QStringLiteral("rotated"));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QVERIFY(!m_sync->isSendingPaused());
        QTRY_COMPARE(m_sync->status(), CalendarSync::Ready);
    }

    void anAcceptedEventStartsThe403CountAgain()
    {
        configureCustom();
        m_sync->setRetryDelaysForTesting({150, 300});
        m_server->putStatuses = {403, 403, 201};
        m_server->defaultPut = 403;
        m_sync->enqueueWorkSession(record(1));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QCOMPARE(m_server->count("PUT"), 3);
        QVERIFY(!m_sync->isSendingPaused());

        // Two refusals after the one accepted: still waiting with the backoff, not paused.
        m_sync->enqueueWorkSession(record(2));
        QTRY_COMPARE(m_server->count("PUT"), 5);
        QVERIFY(!m_sync->isSendingPaused());
        QTRY_VERIFY(m_sync->isSendingPaused());
        QCOMPARE(m_server->count("PUT"), 6);
    }

    void aRefusalByThePreviousCalendarDoesNotHoldBackTheNewOne_data()
    {
        QTest::addColumn<int>("http");
        QTest::newRow("400 (would skip the event)") << 400;
        QTest::newRow("403 (would delay it)") << 403;
        QTest::newRow("404 (would delay it)") << 404;
    }
    void aRefusalByThePreviousCalendarDoesNotHoldBackTheNewOne()
    {
        QFETCH(int, http);
        configureCustom();
        m_sync->setRetryDelaysForTesting({600000});
        m_server->putStatuses = {http};
        m_sync->enqueueWorkSession(record());
        QCOMPARE(m_server->count("PUT"), 0); // on its way to the first calendar
        m_sync->selectCalendar(m_server->base() + QStringLiteral("/cal/work/"), QStringLiteral("Work"));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        const auto puts = m_server->of("PUT");
        QCOMPARE(puts.size(), 2);
        QVERIFY(puts.at(0).path.startsWith("/cal/personal/"));
        QVERIFY(puts.at(1).path.startsWith("/cal/work/"));
        QCOMPARE(m_server->stored.size(), 1);
        QVERIFY(m_server->stored.cbegin().key().startsWith("/cal/work/"));
        QVERIFY(!m_sync->isSendingPaused());
        QCOMPARE(m_sync->status(), CalendarSync::Ready);
        QVERIFY(m_sync->lastError().isEmpty());
    }

    void put404And500RetryWithBackoff()
    {
        configureCustom();
        m_server->putStatuses = {404, 500};
        m_sync->enqueueWorkSession(record());
        QTRY_VERIFY(m_server->count("PUT") >= 1);
        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QCOMPARE(m_sync->lastError(), QStringLiteral("Calendar not found"));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        const auto puts = m_server->of("PUT");
        QCOMPARE(puts.size(), 3);
        QVERIFY2(puts.at(1).at - puts.at(0).at >= 35, "first retry came too early");
        QVERIFY2(puts.at(2).at - puts.at(1).at >= 70, "second retry came too early");
        QCOMPARE(m_sync->status(), CalendarSync::Ready);
        QVERIFY(m_sync->lastError().isEmpty());
    }

    void aRedirectToAnotherServerNeverCarriesThePassword_data()
    {
        QTest::addColumn<int>("status");
        QTest::newRow("302") << 302;
        QTest::newRow("307") << 307;
        QTest::newRow("308") << 308;
    }
    void aRedirectToAnotherServerNeverCarriesThePassword()
    {
        // The calendar's server sends the event elsewhere (another port: another origin). The
        // password goes to the calendar's server only: the redirect is not followed, the event
        // stays queued and is tried again later, still never at the other server.
        QFETCH(int, status);
        RedirectServer calendar;
        calendar.status = status;
        calendar.target = m_server->base();
        m_sync->setProvider(QStringLiteral("caldav"));
        m_sync->setUsername(QStringLiteral("alice"));
        m_sync->setPassword(kPassword);
        m_sync->setCalendarUrl(calendar.base() + QStringLiteral("/cal/personal/"));
        m_sync->setEnabled(true);
        m_sync->enqueueWorkSession(record());

        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QTRY_VERIFY(calendar.requests.size() >= 2); // tried again, with the backoff
        const QByteArray auth = basic(QStringLiteral("alice"), kPassword);
        QCOMPARE(calendar.requests.first().method, QByteArray("PUT"));
        QCOMPARE(calendar.requests.first().headers.value("authorization"), auth); // the password was there to leak
        QVERIFY2(m_server->requests.isEmpty(), "the redirect to another server was followed");
        QCOMPARE(m_sync->pendingCount(), 1);
        QVERIFY(m_server->stored.isEmpty());
        QVERIFY(!m_sync->isSendingPaused());
        checkNoSecrets();
    }

    void aRedirectOnTheSameServerIsFollowed()
    {
        // The control for the test above: the fake redirect works and is followed when it stays
        // on the calendar's server, with the password.
        RedirectServer calendar;
        calendar.target = calendar.base() + QStringLiteral("/moved");
        calendar.answers.append({"/moved/", 201, QByteArray()});
        m_sync->setProvider(QStringLiteral("caldav"));
        m_sync->setUsername(QStringLiteral("alice"));
        m_sync->setPassword(kPassword);
        m_sync->setCalendarUrl(calendar.base() + QStringLiteral("/cal/personal/"));
        m_sync->setEnabled(true);
        m_sync->enqueueWorkSession(record());

        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QCOMPARE(calendar.requests.size(), 2);
        const RedirectServer::Request &followed = calendar.requests.at(1);
        QCOMPARE(followed.method, QByteArray("PUT"));
        QVERIFY(followed.path.startsWith("/moved/cal/personal/"));
        QCOMPARE(followed.headers.value("authorization"), basic(QStringLiteral("alice"), kPassword));
        QVERIFY(followed.body.contains("SUMMARY:Deep work"));
    }

    void putRefusedForTheCalendarKeepsTheQueue_data()
    {
        QTest::addColumn<int>("http");
        QTest::newRow("409 (the calendar is gone)") << 409;
        QTest::newRow("423 (the calendar is locked)") << 423;
    }
    void putRefusedForTheCalendarKeepsTheQueue()
    {
        // The problem is the calendar, not the event: skipping would drop every queued session
        // one after the other while the calendar is away or locked.
        QFETCH(int, http);
        configureCustom();
        m_server->defaultPut = http;
        m_sync->enqueueWorkSession(record(1));
        m_sync->enqueueWorkSession(record(2));
        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QVERIFY(m_sync->lastError().contains(QString::number(http)));
        QVERIFY(!m_sync->lastError().contains(QStringLiteral("skipped")));
        QVERIFY(!m_sync->isSendingPaused());
        QTRY_VERIFY(m_server->count("PUT") >= 2); // tried again, with the backoff
        QCOMPARE(m_sync->pendingCount(), 2);      // kept, not skipped
        QVERIFY(m_server->stored.isEmpty());

        m_server->defaultPut = 201;
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QCOMPARE(m_server->stored.size(), 2);
        QTRY_COMPARE(m_sync->status(), CalendarSync::Ready);
        QVERIFY(m_sync->lastError().isEmpty());
    }

    void retryAfterIsHonoured_data()
    {
        QTest::addColumn<int>("http");
        QTest::addColumn<QByteArray>("header");
        QTest::addColumn<int>("expectedMs");
        QTest::newRow("429, 15 minutes") << 429 << QByteArray("900") << 900 * 1000;
        QTest::newRow("503, 2 minutes") << 503 << QByteArray("120") << 120 * 1000;
        // At most an hour, as Sugo Sync: more would overflow the timer's milliseconds.
        QTest::newRow("huge") << 429 << QByteArray("2500000") << 3600 * 1000;
        // Shorter than the backoff, or unreadable: the backoff's own delay.
        QTest::newRow("shorter") << 429 << QByteArray("30") << 60 * 1000;
        QTest::newRow("zero") << 429 << QByteArray("0") << 60 * 1000;
        QTest::newRow("http date") << 503 << QByteArray("Wed, 21 Oct 2015 07:28:00 GMT") << 60 * 1000;
    }
    void retryAfterIsHonoured()
    {
        // A server asking for a longer pause (429, 503) is not asked again sooner.
        QFETCH(int, http);
        QFETCH(QByteArray, header);
        QFETCH(int, expectedMs);
        // A backoff of a minute: no retry can go out while the test looks (40 ms could, on a slow
        // machine), and Retry-After shorter than it is told apart from the backoff.
        m_sync->setRetryDelaysForTesting({60 * 1000});
        configureCustom();
        m_server->defaultPut = http;
        m_server->putRetryAfter = header;
        m_sync->enqueueWorkSession(record());
        QTRY_VERIFY(m_sync->retryIntervalForTesting() > 0);
        QCOMPARE(m_sync->status(), CalendarSync::Failed);
        QCOMPARE(m_server->count("PUT"), 1);
        QCOMPARE(m_sync->retryIntervalForTesting(), expectedMs);
        QCOMPARE(m_sync->pendingCount(), 1);
    }

    void serverErrorMessage()
    {
        configureCustom();
        m_server->defaultPut = 503;
        m_sync->enqueueWorkSession(record());
        QTRY_VERIFY(!m_sync->lastError().isEmpty());
        QCOMPARE(m_sync->lastError(), QStringLiteral("Server answered HTTP 503"));
        QCOMPARE(m_sync->pendingCount(), 1); // kept for another try
    }

    void permanentRejectionDoesNotBlockQueue()
    {
        configureCustom();
        m_server->putStatuses = {400};
        m_sync->enqueueWorkSession(record(1));
        m_sync->enqueueWorkSession(record(2));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QCOMPARE(m_server->stored.size(), 1);
    }

    void queueIsBounded()
    {
        configureCustom();
        m_sync->setRetryDelaysForTesting({600000});
        m_server->defaultPut = 500;
        m_sync->enqueueWorkSession(record(1));
        QTRY_VERIFY(m_server->count("PUT") == 1);
        QTRY_VERIFY(!m_sync->lastError().isEmpty());
        for (int i = 2; i <= 105; ++i) {
            m_sync->enqueueWorkSession(record(i));
        }
        QCOMPARE(m_sync->pendingCount(), 100);
        QVERIFY(m_sync->lastError().contains(QStringLiteral("oldest")));
    }

    void testConnectionResults()
    {
        configureCustom();
        m_sync->testConnection();
        QTRY_COMPARE(m_sync->status(), CalendarSync::Ready);
        QCOMPARE(m_server->count("PUT"), 1);
        QCOMPARE(m_server->count("DELETE"), 1);
        QVERIFY(m_server->stored.isEmpty());
        QVERIFY(m_server->of("PUT").first().path.contains("/cal/personal/sugo-test-"));
        QVERIFY(m_sync->lastError().isEmpty());

        m_server->defaultPut = 404;
        m_sync->testConnection();
        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QCOMPARE(m_sync->lastError(), QStringLiteral("Calendar not found"));

        m_server->defaultPut = 502;
        m_sync->testConnection();
        QTRY_COMPARE(m_sync->lastError(), QStringLiteral("Server answered HTTP 502"));

        m_server->defaultPut = 201;
        m_server->expectedAuth = basic(QStringLiteral("x"), QStringLiteral("y"));
        m_sync->testConnection();
        QTRY_VERIFY(m_sync->lastError().startsWith(QStringLiteral("Login rejected: check username and app password")));
        QCOMPARE(m_sync->status(), CalendarSync::Failed);
        checkNoSecrets();
    }

    void testConnection403IsTheCalendarRefusingNotTheLogin()
    {
        // A calendar shared read-only answers the test event with 403: the login is fine, so
        // sending is not paused and the message is the one a refused event gets.
        configureCustom();
        m_sync->setRetryDelaysForTesting({600000});
        m_server->defaultPut = 403;
        m_sync->testConnection();
        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QVERIFY(!m_sync->isSendingPaused());
        QVERIFY(m_sync->lastError().contains(QStringLiteral("does not accept new events")));
        QVERIFY(m_sync->lastError().contains(QStringLiteral("403")));
        QVERIFY(!m_sync->lastError().startsWith(QStringLiteral("Login rejected")));
        QVERIFY(!m_sync->statusText().startsWith(QStringLiteral("Login rejected")));
        checkNoSecrets();

        // A finished session is still tried (and waits, refused like the test event).
        m_sync->enqueueWorkSession(record());
        QTRY_COMPARE(m_server->count("PUT"), 2);
        QCOMPARE(m_sync->pendingCount(), 1);
        QVERIFY(!m_sync->isSendingPaused());

        // Once the calendar takes events, it goes without a new password.
        m_server->defaultPut = 201;
        m_sync->testConnection();
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QTRY_COMPARE(m_sync->status(), CalendarSync::Ready);
    }

    void anOversizedAnswerIsCutOff()
    {
        configureCustom();
        m_server->putAnswer = QByteArray(9 * 1024 * 1024, 'x');
        m_sync->testConnection();
        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QVERIFY(m_sync->lastError().contains(QStringLiteral("The server's answer is too large")));
    }

    void networkErrorIsReported()
    {
        configureCustom();
        const QString base = m_server->base();
        m_server.reset(); // nothing listens any more
        m_sync->testConnection();
        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QVERIFY(!m_sync->lastError().isEmpty());
        QVERIFY(!m_sync->lastError().contains(kPassword));
        Q_UNUSED(base);
    }

    // ----- Notes -----

    void noteUpdateResendsSameUid()
    {
        configureCustom();
        m_sync->setIncludeNote(true);
        m_sync->enqueueWorkSession(record(9));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QVERIFY(!m_server->of("PUT").first().body.contains("DESCRIPTION"));
        m_sync->onNoteChanged(9, QStringLiteral("wrote the report, finally"));
        QCOMPARE(m_sync->pendingCount(), 1);
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        const auto puts = m_server->of("PUT");
        QCOMPARE(puts.size(), 2);
        QCOMPARE(puts.at(1).path, puts.at(0).path);
        QVERIFY(puts.at(1).body.contains("DESCRIPTION:wrote the report\\, finally"));
        // The same note again changes nothing.
        m_sync->onNoteChanged(9, QStringLiteral("wrote the report, finally"));
        QCOMPARE(m_sync->pendingCount(), 0);
        // Unknown sessions are ignored.
        m_sync->onNoteChanged(12345, QStringLiteral("x"));
        QCOMPARE(m_sync->pendingCount(), 0);
    }

    void noteUpdateAfterRestartResendsSameUid()
    {
        configureCustom();
        m_sync->setIncludeNote(true);
        m_sync->enqueueWorkSession(record(9));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QCOMPARE(m_server->count("PUT"), 1);
        QVERIFY(!m_server->of("PUT").first().body.contains("DESCRIPTION"));

        // What was sent is remembered across a restart, so the note still reaches the event.
        m_sync.reset();
        m_sync = makeSync();
        m_sync->onNoteChanged(9, QStringLiteral("wrote the report, finally"));
        QCOMPARE(m_sync->pendingCount(), 1);
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        const auto puts = m_server->of("PUT");
        QCOMPARE(puts.size(), 2);
        QCOMPARE(puts.at(1).path, puts.at(0).path);
        QVERIFY(puts.at(1).body.contains("DESCRIPTION:wrote the report\\, finally"));
    }

    void deletedSessionLeavesTheCalendarWhenAsked()
    {
        configureCustom();
        m_sync->enqueueWorkSession(record(9));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QCOMPARE(m_server->stored.size(), 1);

        // Kept on the calendar: nothing is asked of the server.
        m_sync->onSessionDeleted(record(9), false);
        QTest::qWait(50);
        QCOMPARE(m_server->count("DELETE"), 0);
        QCOMPARE(m_server->stored.size(), 1);

        m_sync->enqueueWorkSession(record(10));
        QTRY_COMPARE(m_server->stored.size(), 2);
        m_sync->onSessionDeleted(record(10), true);
        QTRY_COMPARE(m_server->count("DELETE"), 1);
        QCOMPARE(m_server->of("DELETE").first().path, m_server->of("PUT").last().path);
        QCOMPARE(m_server->stored.size(), 1);
        // A note for it later has no event to go to.
        m_sync->setIncludeNote(true);
        m_sync->onNoteChanged(10, QStringLiteral("too late"));
        QCOMPARE(m_sync->pendingCount(), 0);
    }

    void deletedSessionNotSentYetIsNeverSentButStillDeleted()
    {
        configureCustom();
        m_sync->setRetryDelaysForTesting({60'000}); // the event waits for its next try
        m_server->putStatuses = {500};
        m_sync->enqueueWorkSession(record(3));
        QTRY_VERIFY(m_sync->retryIntervalForTesting() > 0);
        m_sync->onSessionDeleted(record(3), true);
        QCOMPARE(m_sync->pendingCount(), 0);
        // It may be on the server all the same (an answer lost): asked to go, and not there (404).
        QTRY_COMPARE(m_server->count("DELETE"), 1);
        QTest::qWait(100);
        QCOMPARE(m_server->count("PUT"), 1);
        QCOMPARE(m_server->stored.size(), 0);
    }

    void deletedSessionWhoseUploadAnswerWasLostIsStillDeleted()
    {
        configureCustom();
        m_server->hold = [](const FakeServer::Request &req) {
            return req.method == "PUT";
        };
        m_sync->enqueueWorkSession(record(15));
        QTRY_COMPARE(m_server->heldCount(), 1);
        // The server took it, but Sugo quit before the answer came: still queued at the restart.
        const QByteArray path = "/cal/personal/" + CalendarSync::uidFor(record(15)).toUtf8() + ".ics";
        m_server->stored.insert(path, QByteArray("BEGIN:VCALENDAR"));
        m_sync.reset();
        m_server->hold = nullptr;
        m_sync = makeSync();
        m_sync->onSessionDeleted(record(15), true);
        QTRY_COMPARE(m_server->count("DELETE"), 1);
        QCOMPARE(m_server->of("DELETE").first().path, path);
        QTRY_COMPARE(m_server->stored.size(), 0);
        QCOMPARE(m_sync->pendingCount(), 0);
    }

    void deletedSessionOnItsWayIsRemovedOnceItArrives()
    {
        configureCustom();
        m_server->hold = [](const FakeServer::Request &req) {
            return req.method == "PUT";
        };
        m_sync->enqueueWorkSession(record(4));
        QTRY_COMPARE(m_server->heldCount(), 1);
        m_sync->onSessionDeleted(record(4), true);
        QCOMPARE(m_server->count("DELETE"), 0); // not before the upload has finished
        m_server->hold = nullptr;
        m_server->releaseHeld();
        QTRY_COMPARE(m_server->count("DELETE"), 1);
        QTRY_COMPARE(m_server->stored.size(), 0);
        QCOMPARE(m_sync->pendingCount(), 0);
    }

    void deletedSessionLeavesTheCalendarItWasSentTo()
    {
        configureCustom();
        m_sync->enqueueWorkSession(record(11));
        QTRY_COMPARE(m_sync->pendingCount(), 0); // its answer arrived: Sugo knows where it went
        const QByteArray sentTo = m_server->of("PUT").first().path;

        // Another calendar of the same account is chosen afterwards, and Sugo restarts.
        m_sync->setCalendarUrl(m_server->base() + QStringLiteral("/cal/work/"));
        m_sync.reset();
        m_sync = makeSync();
        m_sync->onSessionDeleted(record(11), true);
        QTRY_COMPARE(m_server->count("DELETE"), 1);
        QCOMPARE(m_server->of("DELETE").first().path, sentTo);
        QCOMPARE(m_server->stored.size(), 0);
    }

    void anUploadOnItsWayIsNotDeletedBeforeItArrives()
    {
        configureCustom();
        m_sync->enqueueWorkSession(record(12));
        QTRY_COMPARE(m_server->stored.size(), 1);
        m_server->hold = [](const FakeServer::Request &req) {
            return req.method == "PUT";
        };
        m_sync->enqueueWorkSession(record(13));
        QTRY_COMPARE(m_server->heldCount(), 1);
        // 13 is deleted while on its way, then 12 (sent): only 12's event may go now.
        m_sync->onSessionDeleted(record(13), true);
        m_sync->onSessionDeleted(record(12), true);
        QTRY_COMPARE(m_server->count("DELETE"), 1);
        QTest::qWait(50);
        QCOMPARE(m_server->count("DELETE"), 1);
        m_server->hold = nullptr;
        m_server->releaseHeld();
        QTRY_COMPARE(m_server->count("DELETE"), 2);
        QTRY_COMPARE(m_server->stored.size(), 0);
    }

    void deletingFromAnUnreachableCalendarIsTriedAgain()
    {
        configureCustom();
        m_sync->enqueueWorkSession(record(5));
        QTRY_COMPARE(m_server->stored.size(), 1);
        m_server->deleteStatus = 503;
        m_sync->onSessionDeleted(record(5), true);
        QTRY_COMPARE(m_server->count("DELETE"), 1);
        QCOMPARE(m_server->stored.size(), 1);

        // Kept across a restart, and tried again once the server takes an event.
        m_sync.reset();
        m_sync = makeSync();
        m_server->deleteStatus = 204;
        m_sync->enqueueWorkSession(record(6));
        QTRY_COMPARE(m_server->count("DELETE"), 2);
        QTRY_COMPARE(m_server->stored.size(), 1);
    }

    void noteUpdatesQueuedItem()
    {
        configureCustom();
        m_sync->setIncludeNote(true);
        m_server->putStatuses = {500};
        m_sync->enqueueWorkSession(record(3));
        QTRY_VERIFY(m_server->count("PUT") >= 1);
        m_sync->onNoteChanged(3, QStringLiteral("queued note"));
        QCOMPARE(m_sync->pendingCount(), 1);
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QVERIFY(m_server->of("PUT").last().body.contains("DESCRIPTION:queued note"));
        QCOMPARE(m_server->stored.size(), 1);
    }

    void noteNeverSentWhenIncludeNoteOff()
    {
        configureCustom();
        QVERIFY(!m_sync->includeNote());
        m_sync->enqueueWorkSession(record(4));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        m_sync->onNoteChanged(4, QStringLiteral("private note"));
        QCOMPARE(m_sync->pendingCount(), 0);
        QTest::qWait(100);
        QCOMPARE(m_server->count("PUT"), 1);
        for (const auto &req : std::as_const(m_server->requests)) {
            QVERIFY(!req.body.contains("private note"));
        }
        SessionRecord withNote = record(5);
        withNote.note = QStringLiteral("also private");
        m_sync->enqueueWorkSession(withNote);
        QTRY_COMPARE(m_server->count("PUT"), 2);
        QVERIFY(!m_server->of("PUT").last().body.contains("also private"));
    }

    // ----- Login Flow v2 -----

    void loginFlowHappyPath()
    {
        m_server->expectedAuth = basic(QStringLiteral("alice"), kPassword);
        m_sync->setServerUrl(m_server->base());
        m_sync->startNextcloudLogin();
        QTRY_COMPARE(m_sync->status(), CalendarSync::WaitingForBrowser);
        QCOMPARE(m_opened.size(), 1);
        QCOMPARE(m_opened.first().path(), QStringLiteral("/login/v2/flow/abc"));
        QVERIFY(m_sync->statusText().contains(QStringLiteral("browser")));
        QTRY_COMPARE(m_sync->status(), CalendarSync::Ready);
        QCOMPARE(m_sync->username(), QStringLiteral("alice"));
        QVERIFY(m_sync->hasPassword());
        QCOMPARE(m_sync->calendarName(), QStringLiteral("Personal"));
        QVERIFY(m_sync->calendarUrl().endsWith(QStringLiteral("/remote.php/dav/calendars/alice/personal")));
        QVERIFY(m_server->polls >= 3);
        // The login requests carried no credentials, and the poll sent the token as a form.
        for (const auto &req : std::as_const(m_server->requests)) {
            if (req.path == "/index.php/login/v2" || req.path == "/login/v2/poll") {
                QVERIFY(!req.headers.contains("authorization"));
                // Nextcloud uses this as the name of the new app password: just "Sugo".
                QCOMPARE(req.headers.value("user-agent"), QByteArray("Sugo"));
            }
            if (req.path == "/login/v2/poll") {
                QCOMPARE(req.headers.value("content-type"), QByteArray("application/x-www-form-urlencoded"));
                QCOMPARE(req.body, QByteArray("token=tok123"));
            }
        }
        checkNoSecrets();

        // The new account works, and persists.
        m_sync->setEnabled(true);
        m_sync->enqueueWorkSession(record());
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        auto again = makeSync();
        QVERIFY(again->hasPassword());
        QCOMPARE(again->username(), QStringLiteral("alice"));
        QCOMPARE(again->calendarUrl(), m_sync->calendarUrl());
        QVERIFY(again->enabled());
    }

    void loginFlowWithALoginNameThatIsNotTheUserId()
    {
        // Signed in with an e-mail address: the DAV folders are named after the user id.
        m_server->loginName = QStringLiteral("alice@example.org");
        m_server->userId = QStringLiteral("alice");
        m_server->expectedAuth = basic(QStringLiteral("alice@example.org"), kPassword);
        m_sync->setServerUrl(m_server->base());
        m_sync->startNextcloudLogin();
        QTRY_COMPARE(m_sync->status(), CalendarSync::Ready);
        QCOMPARE(m_sync->username(), QStringLiteral("alice@example.org"));
        QCOMPARE(m_sync->calendarName(), QStringLiteral("Personal"));
        QCOMPARE(m_sync->calendarUrl(), m_server->base() + QStringLiteral("/remote.php/dav/calendars/alice/personal"));
        QCOMPARE(m_server->of("PROPFIND").last().path, QByteArray("/remote.php/dav/calendars/alice/"));

        // Events go there, signed in with the login name.
        m_sync->setEnabled(true);
        m_sync->enqueueWorkSession(record());
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QVERIFY(m_server->of("PUT").first().path.startsWith("/remote.php/dav/calendars/alice/personal/"));
    }

    void loginFlowSingleCalendarIsPicked()
    {
        m_server->onlyOneCalendar = true;
        m_sync->setServerUrl(m_server->base());
        m_sync->startNextcloudLogin();
        QTRY_COMPARE(m_sync->status(), CalendarSync::Ready);
    }

    void loginFlowLeavesChoiceToUser()
    {
        m_server->noPersonal = true;
        m_server->expectedAuth = basic(QStringLiteral("alice"), kPassword);
        m_sync->setServerUrl(m_server->base());
        m_sync->startNextcloudLogin();
        QTRY_COMPARE(m_sync->calendars().size(), 2);
        QTRY_COMPARE(m_sync->status(), CalendarSync::NotConfigured);
        QVERIFY(m_sync->hasPassword());
        QVERIFY(m_sync->calendarUrl().isEmpty());
        QVERIFY(m_sync->statusText().contains(QStringLiteral("calendar")));
        m_sync->selectCalendar(m_sync->calendars().first().toMap().value(QStringLiteral("url")).toString(), QStringLiteral("Private"));
        QCOMPARE(m_sync->status(), CalendarSync::Ready);
    }

    void loginFlowHostMismatchRejected()
    {
        m_server->loginServer = QStringLiteral("https://evil.example.org");
        m_sync->setServerUrl(m_server->base());
        m_sync->startNextcloudLogin();
        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QVERIFY(!m_sync->hasPassword());
        QVERIFY(m_sync->username().isEmpty());
        QVERIFY(!m_sync->lastError().isEmpty());
        QVERIFY(!m_sync->lastError().contains(kPassword));
        QCOMPARE(m_server->count("PROPFIND"), 0);
    }

    void loginPageOnAnotherHostIsNeverOpened()
    {
        m_server->loginPage = QStringLiteral("https://phishing.example.net/login/v2/flow/abc");
        m_sync->setServerUrl(m_server->base());
        m_sync->startNextcloudLogin();
        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QVERIFY(m_opened.isEmpty());
        QCOMPARE(m_server->polls, 0);
    }

    void nextcloudCalendarMustBeOnTheSignedInServer()
    {
        fillNextcloud(); // server https://cloud.example.org, calendar on it
        const QString before = m_sync->calendarUrl();
        m_sync->selectCalendar(QStringLiteral("https://elsewhere.example.com/cal/x"), QStringLiteral("X"));
        QCOMPARE(m_sync->calendarUrl(), before);
        QVERIFY(!m_sync->lastError().isEmpty());
        QSignalSpy rejected(m_sync.get(), &CalendarSync::inputRejected);
        m_sync->setCalendarUrl(QStringLiteral("https://elsewhere.example.com/cal/x"));
        QCOMPARE(m_sync->calendarUrl(), before);
        // Typed in, the refusal is announced so the view can keep the address and say why.
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(rejected.takeFirst().at(0).toString(), m_sync->lastError());
        // The same server with its port written out is still the same server.
        m_sync->selectCalendar(QStringLiteral("https://cloud.example.org:443/cal/work"), QStringLiteral("Work"));
        QVERIFY(m_sync->calendarUrl().contains(QStringLiteral("/cal/work")));

        // Custom CalDAV has no separate server: any (secure) calendar address is the account.
        fillCaldav();
        m_sync->selectCalendar(QStringLiteral("https://elsewhere.example.com/cal/x"), QStringLiteral("X"));
        QCOMPARE(m_sync->calendarUrl(), QStringLiteral("https://elsewhere.example.com/cal/x"));
    }

    void passwordIsNotSentWhenTheServerChangedUnderTheCalendar()
    {
        // Nextcloud on the fake server, then the server address is edited to another host:
        // the stored calendar still points at the old one and must not get the password.
        m_server->expectedAuth = basic(QStringLiteral("alice"), kPassword);
        m_sync->setProvider(QStringLiteral("nextcloud"));
        m_sync->setServerUrl(m_server->base());
        m_sync->setUsername(QStringLiteral("alice"));
        m_sync->setPassword(kPassword);
        m_sync->selectCalendar(m_server->base() + QStringLiteral("/remote.php/dav/calendars/alice/personal"), QStringLiteral("Personal"));
        m_sync->setEnabled(true);
        QSignalSpy calendarChanged(m_sync.get(), &CalendarSync::calendarUrlChanged);
        m_sync->setServerUrl(QStringLiteral("https://other.example.org"));
        // The old calendar is forgotten (and stays so after a restart): it is not on the new server.
        QCOMPARE(calendarChanged.count(), 1);
        QVERIFY(m_sync->calendarUrl().isEmpty());
        QVERIFY(makeSync()->calendarUrl().isEmpty());
        m_sync->enqueueWorkSession(record());
        m_sync->testConnection();
        QTest::qWait(250);
        QCOMPARE(m_server->count("PUT"), 0);
    }

    void theTestEventIsRemovedFromTheCalendarItWasPutIn()
    {
        configureCustom();
        m_sync->testConnection();
        // Another calendar is chosen while the test is on its way.
        m_sync->setCalendarUrl(m_server->base() + QStringLiteral("/cal/other/"));
        QTRY_COMPARE(m_server->count("DELETE"), 1);
        QVERIFY(m_server->of("DELETE").first().path.startsWith("/cal/personal/sugo-test-"));
        QVERIFY(m_server->stored.isEmpty());
    }

    void aRefusalOfTheOldPasswordDoesNotPauseTheNewOne()
    {
        configureCustom();
        m_server->expectedAuth = basic(QStringLiteral("alice"), QStringLiteral("rotated"));
        m_sync->enqueueWorkSession(record(1));
        QCOMPARE(m_server->count("PUT"), 0); // on its way with the old password
        m_sync->setPassword(QStringLiteral("rotated"));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QVERIFY(!m_sync->isSendingPaused());
        QCOMPARE(m_server->count("PUT"), 2); // refused with the old one, sent with the new one
        QCOMPARE(m_sync->status(), CalendarSync::Ready);
    }

    void anotherAccountsCredentialsNeverRemoveTheTestEvent()
    {
        configureCustom();
        m_sync->testConnection();
        // Another user is set up while the test is on its way.
        m_sync->setUsername(QStringLiteral("mallory"));
        QTRY_COMPARE(m_server->count("PUT"), 1);
        QTest::qWait(100);
        QCOMPARE(m_server->count("DELETE"), 0);
        QTRY_VERIFY(configFileContents().contains("LeftoverTests"));

        // Back to the account it belongs to: removed with its own credentials.
        m_sync->setUsername(QStringLiteral("alice"));
        m_sync->enqueueWorkSession(record(2));
        QTRY_COMPARE(m_server->count("DELETE"), 1);
        QCOMPARE(m_server->of("DELETE").first().headers.value("authorization"), basic(QStringLiteral("alice"), kPassword));
        QTRY_COMPARE(m_server->stored.size(), 1);
    }

    void aRefusedDiscoveryForTheOldPasswordDoesNotPauseTheNewOne()
    {
        configureCustom();
        m_server->expectedAuth = basic(QStringLiteral("alice"), QStringLiteral("rotated"));
        m_sync->refreshCalendars();
        m_sync->setPassword(QStringLiteral("rotated")); // while the old one is on its way
        QTRY_COMPARE(m_server->count("PROPFIND"), 1);
        QTRY_COMPARE(m_sync->status(), CalendarSync::Ready);
        QVERIFY(!m_sync->isSendingPaused());
        QVERIFY(m_sync->lastError().isEmpty());
    }

    void calendarsOfTheOldServerNeverFillTheNewOnesList()
    {
        m_server->expectedAuth = basic(QStringLiteral("alice"), kPassword);
        m_sync->setServerUrl(m_server->base());
        m_sync->setUsername(QStringLiteral("alice"));
        m_sync->setPassword(kPassword);
        m_sync->refreshCalendars();
        m_sync->setServerUrl(QStringLiteral("https://other.example.org")); // while the list is on its way
        QTRY_COMPARE(m_server->count("PROPFIND"), 1);
        QTRY_VERIFY(m_sync->status() != CalendarSync::Working);
        QTest::qWait(50);
        QVERIFY(m_sync->calendars().isEmpty());
    }

    void aTestThatOutlivesItsAccountKeepsItsLeftoverForThatAccount()
    {
        configureCustom();
        m_server->deleteStatus = 500;
        m_server->onRequest = [this](const FakeServer::Request &req) {
            if (req.method == "DELETE" && m_server->count("DELETE") == 1) {
                m_sync->setUsername(QStringLiteral("mallory")); // while the test's DELETE is on its way
            }
        };
        m_sync->testConnection();
        QTRY_COMPARE(m_server->count("DELETE"), 1);
        QTRY_VERIFY(m_sync->status() != CalendarSync::Working);
        QVERIFY(m_sync->statusText() != QStringLiteral("Connection test succeeded"));
        QVERIFY(m_sync->lastError().isEmpty());

        // Its own account removes it, with its own credentials.
        m_server->onRequest = nullptr;
        m_server->deleteStatus = 204;
        m_sync->setUsername(QStringLiteral("alice"));
        m_sync->enqueueWorkSession(record(2));
        QTRY_COMPARE(m_server->count("DELETE"), 2);
        QCOMPARE(m_server->of("DELETE").last().headers.value("authorization"), basic(QStringLiteral("alice"), kPassword));
        QTRY_COMPARE(m_server->stored.size(), 1);
    }

    void aLeftoverTestEventIsRemovedFromTheCalendarItWasPutIn()
    {
        configureCustom();
        m_server->deleteStatus = 500;
        m_sync->testConnection();
        QTRY_COMPARE(m_server->count("DELETE"), 1);
        QTRY_COMPARE(m_sync->status(), CalendarSync::Ready);

        // Another calendar on the same server (the same account) is chosen before it goes.
        m_server->deleteStatus = 204;
        m_sync->setCalendarUrl(m_server->base() + QStringLiteral("/cal/other/"));
        m_sync->enqueueWorkSession(record(2));
        QTRY_COMPARE(m_server->count("DELETE"), 2);
        QVERIFY(m_server->of("DELETE").last().path.startsWith("/cal/personal/sugo-test-"));
        QTRY_COMPARE(m_server->stored.size(), 1);
    }

    void changeMadeWhileTheEventIsOnItsWayIsSentToo()
    {
        configureCustom();
        SessionRecord first = record(5);
        m_sync->enqueueWorkSession(first);
        QCOMPARE(m_server->count("PUT"), 0); // the request is on its way, not answered yet
        SessionRecord renamed = first;
        renamed.presetName = QStringLiteral("Renamed while sending");
        m_sync->enqueueWorkSession(renamed);
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QCOMPARE(m_server->count("PUT"), 2);
        QVERIFY(m_server->of("PUT").last().body.contains("SUMMARY:Renamed while sending"));
    }

    void openingTheSettingsAgainDoesNotRefetchARecentList()
    {
        m_server->expectedAuth = basic(QStringLiteral("alice"), kPassword);
        m_sync->setProvider(QStringLiteral("nextcloud"));
        m_sync->setServerUrl(m_server->base());
        m_sync->setUsername(QStringLiteral("alice"));
        m_sync->setPassword(kPassword);
        m_sync->refreshCalendarsIfStale();
        QTRY_VERIFY(!m_sync->calendars().isEmpty());
        const int fetches = m_server->count("PROPFIND");
        QVERIFY(fetches >= 1);

        m_sync->refreshCalendarsIfStale(); // the page opened again
        QTest::qWait(150);
        QCOMPARE(m_server->count("PROPFIND"), fetches);

        m_sync->refreshCalendars(); // the refresh button always asks
        QTRY_VERIFY(m_server->count("PROPFIND") > fetches);
    }

        void loginFlowInsecureServerRejected()
    {
        m_server->loginServer = QStringLiteral("http://example.org");
        m_sync->setServerUrl(m_server->base());
        m_sync->startNextcloudLogin();
        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QVERIFY(!m_sync->hasPassword());
    }

    void loginFlowCancel()
    {
        m_server->pollsBefore200 = 1000000;
        m_sync->setServerUrl(m_server->base());
        m_sync->startNextcloudLogin();
        QTRY_COMPARE(m_sync->status(), CalendarSync::WaitingForBrowser);
        QTRY_VERIFY(m_server->polls >= 2);
        const int pollsAtCancel = m_server->polls;
        m_sync->cancelLogin();
        QCOMPARE(m_sync->status(), CalendarSync::NotConfigured);
        // The one poll already on its way may still reach the server, however slow the machine;
        // a poll timer left running would add one every 20 ms.
        QTest::qWait(300);
        QVERIFY2(m_server->polls <= pollsAtCancel + 1,
                 qPrintable(QStringLiteral("%1 polls after the cancel").arg(m_server->polls - pollsAtCancel)));
        QVERIFY(!m_sync->hasPassword());
    }

    void loginFlowNeedsServerUrl()
    {
        m_sync->startNextcloudLogin();
        QVERIFY(!m_sync->lastError().isEmpty());
        QVERIFY(m_server->requests.isEmpty());
        QVERIFY(m_opened.isEmpty());
    }

    void loginFlowNotNextcloud()
    {
        // The fake answers 404 for unknown paths: use the custom path prefix as the "server".
        m_sync->setServerUrl(m_server->base() + QStringLiteral("/nothing"));
        m_sync->startNextcloudLogin();
        QTRY_COMPARE(m_sync->status(), CalendarSync::Failed);
        QVERIFY(m_opened.isEmpty());
    }

    // ----- Storage -----

    void passwordStorageAndDisconnect()
    {
        configureCustom();
        m_sync->setIncludeNote(true);
        QVERIFY(QFile::exists(configPath()));
        const QFileInfo info(configPath());
        QVERIFY2(!(info.permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther)),
                 "the settings file must not be readable by others");
        DiskGroup shared = diskGroup();
        DiskGroup g = shared.group(QStringLiteral("caldav"));
        QCOMPARE(g.readEntry("AppPassword", QString()), kPassword);
        QVERIFY(!shared.hasKey("AppPassword"));
        QCOMPARE(shared.readEntry("Provider", QString()), QStringLiteral("caldav"));
        QVERIFY(shared.readEntry("Enabled", false));
        QVERIFY(shared.readEntry("IncludeNote", false));

        m_sync->enqueueWorkSession(record());
        m_sync->disconnect();
        QVERIFY(!m_sync->hasPassword());
        QVERIFY(!m_sync->enabled());
        QVERIFY(m_sync->username().isEmpty());
        QVERIFY(m_sync->calendarUrl().isEmpty());
        QVERIFY(m_sync->calendars().isEmpty());
        QCOMPARE(m_sync->pendingCount(), 0);
        QCOMPARE(m_sync->status(), CalendarSync::NotConfigured);
        QVERIFY(!g.hasKey("AppPassword"));
        QVERIFY(!g.hasKey("Username"));
        QVERIFY(!g.hasKey("CalendarUrl"));
        QVERIFY(!shared.readEntry("Enabled", false));
        QFile f(configPath());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(!f.readAll().contains(kPassword.toUtf8()));
    }

    void passwordIsStoredAsTyped()
    {
        configureCustom();
        const QString spaced = QStringLiteral(" pass word ");
        m_server->expectedAuth = basic(QStringLiteral("alice"), spaced);
        m_sync->setPassword(spaced);
        QCOMPARE(diskGroup().group(QStringLiteral("caldav")).readEntry("AppPassword", QString()), spaced);
        m_sync->enqueueWorkSession(record());
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QCOMPARE(m_server->of("PUT").first().headers.value("authorization"), basic(QStringLiteral("alice"), spaced));
    }

    // ----- Per-provider settings -----

    void providersAreSeparate()
    {
        fillNextcloud();
        m_sync->setProvider(QStringLiteral("caldav"));
        QVERIFY(m_sync->serverUrl().isEmpty());
        QVERIFY(m_sync->username().isEmpty());
        QVERIFY(!m_sync->hasPassword());
        QVERIFY(m_sync->calendarUrl().isEmpty());
        QVERIFY(m_sync->calendarName().isEmpty());
        QCOMPARE(m_sync->status(), CalendarSync::NotConfigured);
        QVERIFY(!diskGroup(QStringLiteral("caldav")).hasKey("Username"));

        fillCaldav();
        m_sync->setProvider(QStringLiteral("nextcloud"));
        QCOMPARE(m_sync->serverUrl(), QStringLiteral("https://cloud.example.org"));
        QCOMPARE(m_sync->username(), QStringLiteral("alice"));
        QVERIFY(m_sync->hasPassword());
        QCOMPARE(m_sync->calendarUrl(), QStringLiteral("https://cloud.example.org/cal/personal"));
        QCOMPARE(m_sync->calendarName(), QStringLiteral("Personal"));
        QCOMPARE(m_sync->status(), CalendarSync::Ready);
        m_sync->setProvider(QStringLiteral("caldav"));
        QCOMPARE(m_sync->serverUrl(), QStringLiteral("https://dav.example.net"));
        QCOMPARE(m_sync->username(), QStringLiteral("bob"));
        QCOMPARE(m_sync->calendarName(), QStringLiteral("Work"));

        const DiskGroup nc = diskGroup(QStringLiteral("nextcloud"));
        const DiskGroup cd = diskGroup(QStringLiteral("caldav"));
        QCOMPARE(nc.readEntry("ServerUrl", QString()), QStringLiteral("https://cloud.example.org"));
        QCOMPARE(nc.readEntry("Username", QString()), QStringLiteral("alice"));
        QCOMPARE(nc.readEntry("AppPassword", QString()), QStringLiteral("nc-password-1"));
        QCOMPARE(nc.readEntry("CalendarUrl", QString()), QStringLiteral("https://cloud.example.org/cal/personal"));
        QCOMPARE(nc.readEntry("CalendarName", QString()), QStringLiteral("Personal"));
        QCOMPARE(cd.readEntry("ServerUrl", QString()), QStringLiteral("https://dav.example.net"));
        QCOMPARE(cd.readEntry("Username", QString()), QStringLiteral("bob"));
        QCOMPARE(cd.readEntry("AppPassword", QString()), QStringLiteral("cd-password-2"));
        QCOMPARE(cd.readEntry("CalendarName", QString()), QStringLiteral("Work"));
        const DiskGroup shared = diskGroup();
        for (const char *key : {"ServerUrl", "Username", "AppPassword", "CalendarUrl", "CalendarName"}) {
            QVERIFY2(!shared.hasKey(key), key);
        }
        QCOMPARE(shared.readEntry("Provider", QString()), QStringLiteral("caldav"));

        // A new instance sees the same thing.
        auto again = makeSync();
        QCOMPARE(again->provider(), QStringLiteral("caldav"));
        QCOMPARE(again->username(), QStringLiteral("bob"));
        again->setProvider(QStringLiteral("nextcloud"));
        QCOMPARE(again->username(), QStringLiteral("alice"));
    }

    void switchClearsCalendars()
    {
        m_server->expectedAuth = basic(QStringLiteral("alice"), kPassword);
        m_sync->setServerUrl(m_server->base());
        m_sync->setUsername(QStringLiteral("alice"));
        m_sync->setPassword(kPassword);
        m_sync->refreshCalendars();
        QTRY_VERIFY(!m_sync->calendars().isEmpty());
        QSignalSpy spy(m_sync.get(), &CalendarSync::calendarsChanged);
        m_sync->setProvider(QStringLiteral("caldav"));
        QVERIFY(m_sync->calendars().isEmpty());
        QCOMPARE(spy.count(), 1);
    }

    void switchEmitsSignalsOnce()
    {
        fillNextcloud();
        m_sync->setProvider(QStringLiteral("caldav"));
        fillCaldav();
        m_sync->setProvider(QStringLiteral("nextcloud"));

        QSignalSpy provider(m_sync.get(), &CalendarSync::providerChanged);
        QSignalSpy server(m_sync.get(), &CalendarSync::serverUrlChanged);
        QSignalSpy user(m_sync.get(), &CalendarSync::usernameChanged);
        QSignalSpy pw(m_sync.get(), &CalendarSync::hasPasswordChanged);
        QSignalSpy calUrl(m_sync.get(), &CalendarSync::calendarUrlChanged);
        QSignalSpy calName(m_sync.get(), &CalendarSync::calendarNameChanged);
        QSignalSpy cals(m_sync.get(), &CalendarSync::calendarsChanged);
        QSignalSpy enabled(m_sync.get(), &CalendarSync::enabledChanged);
        m_sync->setProvider(QStringLiteral("caldav"));
        QCOMPARE(provider.count(), 1);
        QCOMPARE(server.count(), 1);
        QCOMPARE(user.count(), 1);
        QCOMPARE(pw.count(), 0); // both have a password
        QCOMPARE(calUrl.count(), 1);
        QCOMPARE(calName.count(), 1);
        QCOMPARE(cals.count(), 1);
        QCOMPARE(enabled.count(), 0);

        // To an empty provider: the password signal fires too.
        m_sync->disconnect();
        m_sync->setProvider(QStringLiteral("nextcloud"));
        provider.clear();
        pw.clear();
        user.clear();
        m_sync->setProvider(QStringLiteral("caldav"));
        QCOMPARE(provider.count(), 1);
        QCOMPARE(pw.count(), 1); // caldav was disconnected above, nextcloud has a password
        m_sync->setProvider(QStringLiteral("nextcloud"));
        QCOMPARE(pw.count(), 2);
        QCOMPARE(user.count(), 2);

        // Same provider again: nothing at all.
        provider.clear();
        cals.clear();
        m_sync->setProvider(QStringLiteral("nextcloud"));
        QCOMPARE(provider.count(), 0);
        QCOMPARE(cals.count(), 0);
    }

    void disconnectWipesOnlyActiveProvider()
    {
        fillNextcloud();
        fillCaldav();
        m_sync->setEnabled(true);
        m_sync->disconnect();
        QVERIFY(!m_sync->enabled());
        QVERIFY(m_sync->username().isEmpty());
        QVERIFY(!m_sync->hasPassword());
        for (const char *key : {"ServerUrl", "Username", "AppPassword", "CalendarUrl", "CalendarName"}) {
            if (QByteArray(key) != "ServerUrl") { // the address is not wiped by disconnect(), as before
                QVERIFY2(!diskGroup(QStringLiteral("caldav")).hasKey(key), key);
            }
            QVERIFY2(diskGroup(QStringLiteral("nextcloud")).hasKey(key), key);
        }
        m_sync->setProvider(QStringLiteral("nextcloud"));
        QCOMPARE(m_sync->username(), QStringLiteral("alice"));
        QVERIFY(m_sync->hasPassword());
        QCOMPARE(m_sync->calendarName(), QStringLiteral("Personal"));
        QVERIFY(!m_sync->enabled());
    }

    void switchCancelsLogin()
    {
        m_server->pollsBefore200 = 1000000;
        m_sync->setServerUrl(m_server->base());
        m_sync->startNextcloudLogin();
        QTRY_COMPARE(m_sync->status(), CalendarSync::WaitingForBrowser);
        QTRY_VERIFY(m_server->polls >= 2);
        m_sync->setProvider(QStringLiteral("caldav"));
        QCOMPARE(m_sync->status(), CalendarSync::NotConfigured);
        QTest::qWait(60);
        const int polls = m_server->polls;
        QTest::qWait(200);
        QCOMPARE(m_server->polls, polls);
        QVERIFY(!m_sync->hasPassword());
        m_sync->setProvider(QStringLiteral("nextcloud"));
        QCOMPARE(m_sync->serverUrl(), m_server->base());
        QVERIFY(!m_sync->hasPassword());
    }

    void passwordOnlyInSubgroupAndFileIsPrivate()
    {
        fillNextcloud();
        fillCaldav();
        const DiskGroup shared = diskGroup();
        QVERIFY(!shared.hasKey("AppPassword"));
        QCOMPARE(diskGroup(QStringLiteral("nextcloud")).readEntry("AppPassword", QString()), QStringLiteral("nc-password-1"));
        QCOMPARE(diskGroup(QStringLiteral("caldav")).readEntry("AppPassword", QString()), QStringLiteral("cd-password-2"));
        const QFileInfo info(configPath());
        QVERIFY(!(info.permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther)));
    }

    void queuedEventsSurviveSwitch()
    {
        configureCustom();
        m_server->defaultPut = 500;
        m_sync->enqueueWorkSession(record(1));
        m_sync->enqueueWorkSession(record(2));
        QTRY_VERIFY(m_server->count("PUT") >= 1);
        QCOMPARE(m_sync->pendingCount(), 2);
        m_sync->setProvider(QStringLiteral("nextcloud"));
        QCOMPARE(m_sync->pendingCount(), 2);
        QVERIFY(m_sync->enabled());
        QCOMPARE(m_sync->status(), CalendarSync::NotConfigured);
        const int puts = m_server->count("PUT");
        QTest::qWait(250);
        QCOMPARE(m_server->count("PUT"), puts); // nothing is sent until configured
        QCOMPARE(m_sync->pendingCount(), 2);

        // Back to the configured provider: the queue drains there.
        m_server->defaultPut = 201;
        m_sync->setProvider(QStringLiteral("caldav"));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
    }

    // ----- Accounts, persistence and the keyring -----

    void queuedEventsNeverGoToAnotherAccount()
    {
        // Events of alice's account are waiting when the same provider is set up for bob.
        configureCustom();
        m_server->defaultPut = 500;
        m_sync->setRetryDelaysForTesting({600000});
        m_sync->enqueueWorkSession(record(1));
        m_sync->enqueueWorkSession(record(2));
        QTRY_VERIFY(m_server->count("PUT") >= 1);
        QCOMPARE(m_sync->pendingCount(), 2);

        m_server->defaultPut = 201;
        m_server->expectedAuth = basic(QStringLiteral("bob"), QStringLiteral("bobs-password"));
        m_sync->setUsername(QStringLiteral("bob"));
        m_sync->setPassword(QStringLiteral("bobs-password"));
        QCOMPARE(m_sync->pendingCount(), 0);
        const int puts = m_server->count("PUT");
        QTest::qWait(250);
        QCOMPARE(m_server->count("PUT"), puts);
        QVERIFY(m_server->stored.isEmpty());

        // Bob's own events go to bob.
        m_sync->enqueueWorkSession(record(3));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QCOMPARE(m_server->stored.size(), 1);
        QVERIFY(m_server->of("PUT").last().path.contains("sugo-3-"));
    }

    void otherProvidersQueueWaitsForItsOwnAccount()
    {
        configureCustom();
        m_server->defaultPut = 500;
        m_sync->setRetryDelaysForTesting({600000});
        m_sync->enqueueWorkSession(record(1));
        QTRY_VERIFY(m_server->count("PUT") >= 1);

        // A Nextcloud account on the same server must not receive the CalDAV account's event.
        m_server->defaultPut = 201;
        m_sync->setProvider(QStringLiteral("nextcloud"));
        m_sync->setServerUrl(m_server->base());
        m_sync->setUsername(QStringLiteral("alice"));
        m_sync->setPassword(kPassword);
        m_sync->selectCalendar(m_server->base() + QStringLiteral("/remote.php/dav/calendars/alice/personal"), QStringLiteral("Personal"));
        const int puts = m_server->count("PUT");
        QTest::qWait(250);
        QCOMPARE(m_server->count("PUT"), puts);
        QCOMPARE(m_sync->pendingCount(), 1);

        m_sync->setProvider(QStringLiteral("caldav"));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QVERIFY(m_server->of("PUT").last().path.startsWith("/cal/personal/"));
    }

    void loginAsAnotherUserDropsTheOldAccountsEvents()
    {
        // Nextcloud: alice has events waiting, then the user signs in again as bob.
        m_server->expectedAuth = basic(QStringLiteral("alice"), kPassword);
        m_sync->setServerUrl(m_server->base());
        m_sync->startNextcloudLogin();
        QTRY_COMPARE(m_sync->status(), CalendarSync::Ready);
        m_sync->setEnabled(true);
        m_sync->setRetryDelaysForTesting({600000});
        m_server->defaultPut = 500;
        m_sync->enqueueWorkSession(record(1));
        QTRY_VERIFY(m_server->count("PUT") >= 1);
        QCOMPARE(m_sync->pendingCount(), 1);

        m_server->defaultPut = 201;
        m_server->loginName = QStringLiteral("bob");
        m_server->expectedAuth = basic(QStringLiteral("bob"), kPassword);
        m_server->polls = 0;
        m_sync->startNextcloudLogin();
        QTRY_COMPARE(m_sync->username(), QStringLiteral("bob"));
        m_sync->selectCalendar(m_server->base() + QStringLiteral("/remote.php/dav/calendars/bob/personal"), QStringLiteral("Personal"));
        QCOMPARE(m_sync->pendingCount(), 0);
        QTest::qWait(250);
        QVERIFY(m_server->stored.isEmpty());
    }

    void queueSurvivesRestart()
    {
        configureCustom();
        m_sync->setIncludeNote(true);
        m_server->defaultPut = 500;
        m_sync->setRetryDelaysForTesting({600000});
        SessionRecord first = record(1);
        first.note = QStringLiteral("note, with; specials\nand a newline");
        m_sync->enqueueWorkSession(first);
        m_sync->enqueueWorkSession(record(2));
        QTRY_VERIFY(m_server->count("PUT") >= 1);
        QCOMPARE(m_sync->pendingCount(), 2);
        m_sync.reset();

        m_server->defaultPut = 201;
        m_sync = makeSync();
        QCOMPARE(m_sync->pendingCount(), 2);
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QCOMPARE(m_server->stored.size(), 2);
        const auto puts = m_server->of("PUT");
        QVERIFY(std::any_of(puts.cbegin(), puts.cend(), [](const auto &put) {
            return put.path.contains("sugo-1-1700000000000") && put.body.contains("DESCRIPTION:note\\, with\\; specials");
        }));
        // Nothing is left behind for the next start.
        m_sync.reset();
        m_sync = makeSync();
        QCOMPARE(m_sync->pendingCount(), 0);
    }

    void noteResendRespectsTheQueueBound()
    {
        configureCustom();
        m_sync->setIncludeNote(true);
        m_sync->enqueueWorkSession(record(1));
        QTRY_COMPARE(m_sync->pendingCount(), 0);

        m_server->defaultPut = 500;
        m_sync->setRetryDelaysForTesting({600000});
        for (int i = 2; i <= 101; ++i) {
            m_sync->enqueueWorkSession(record(i));
        }
        QCOMPARE(m_sync->pendingCount(), 100);
        m_sync->onNoteChanged(1, QStringLiteral("late note"));
        QCOMPARE(m_sync->pendingCount(), 100);

        // A note for an event already sent never pushes out one that never got there.
        m_sync.reset();
        m_server->defaultPut = 201;
        m_sync = makeSync();
        // A hundred events go one after another: more than the default 5 s on a slow runner.
        QTRY_COMPARE_WITH_TIMEOUT(m_sync->pendingCount(), 0, 60000);
        const QList<QByteArray> stored = m_server->stored.keys();
        for (int i = 2; i <= 101; ++i) {
            const QByteArray uid = "sugo-" + QByteArray::number(i) + '-';
            QVERIFY2(std::any_of(stored.cbegin(), stored.cend(), [&uid](const QByteArray &path) { return path.contains(uid); }),
                     uid.constData());
        }
    }

    void leftoverTestEventIsRemovedLater()
    {
        configureCustom();
        m_server->deleteStatus = 500;
        m_sync->testConnection();
        QTRY_COMPARE(m_sync->status(), CalendarSync::Ready);
        QVERIFY(m_sync->lastError().contains(QStringLiteral("try again later")));
        QCOMPARE(m_server->stored.size(), 1);

        // It is remembered across a restart and removed after the next event goes through.
        m_sync.reset();
        m_sync = makeSync();
        m_server->deleteStatus = 204;
        m_sync->enqueueWorkSession(record(4));
        QTRY_COMPARE(m_server->count("DELETE"), 2);
        QTRY_COMPARE(m_server->stored.size(), 1);
        QVERIFY(m_server->stored.keys().first().contains("sugo-4-"));
        QTRY_VERIFY(!configFileContents().contains("LeftoverTests"));
        m_sync.reset();
        m_sync = makeSync();
        m_sync->enqueueWorkSession(record(5));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QCOMPARE(m_server->count("DELETE"), 2);
    }

    void aTestEventThatMayNotBeRemovedIsNotARejectedLogin()
    {
        // The PUT went through with these credentials: a 403 to its DELETE is the calendar
        // keeping it (no right to remove), not the login.
        configureCustom();
        m_server->deleteStatus = 403;
        m_sync->testConnection();
        QTRY_COMPARE(m_sync->status(), CalendarSync::Ready);
        QVERIFY(m_sync->lastError().contains(QStringLiteral("could not be removed yet")));
        QVERIFY(m_sync->lastError().contains(QStringLiteral("403")));
        QVERIFY(!m_sync->lastError().contains(QStringLiteral("Login rejected")));
        QVERIFY(!m_sync->isSendingPaused());
    }

    void passwordIsKeptInTheKeyring()
    {
        FakeSecretStore keyring;
        m_sync = makeSyncWithKeyring(keyring);
        configureCustom();
        QVERIFY(m_sync->isPasswordInKeyring());
        QCOMPARE(keyring.secrets->value(QStringLiteral("calendar-caldav")), kPassword);
        QVERIFY(!configFileContents().contains(kPassword.toUtf8()));

        // It works from the keyring, and after a restart.
        m_sync->enqueueWorkSession(record());
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        m_sync = makeSyncWithKeyring(keyring);
        QVERIFY(m_sync->hasPassword());
        m_sync->enqueueWorkSession(record(8));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QCOMPARE(m_server->stored.size(), 2);

        m_sync->disconnect();
        QVERIFY(keyring.secrets->isEmpty());
    }

    void deletedSessionWhoseUploadAnswerWasLostLeavesTheCalendarItWasTriedIn()
    {
        configureCustom();
        m_server->hold = [](const FakeServer::Request &req) {
            return req.method == "PUT";
        };
        m_sync->enqueueWorkSession(record(16));
        QTRY_COMPARE(m_server->heldCount(), 1);
        const QByteArray path = "/cal/personal/" + CalendarSync::uidFor(record(16)).toUtf8() + ".ics";
        m_server->stored.insert(path, QByteArray("BEGIN:VCALENDAR")); // the server took it
        // Another calendar of the same account is chosen, and Sugo restarts before the answer came.
        m_sync->setCalendarUrl(m_server->base() + QStringLiteral("/cal/work/"));
        m_sync.reset();
        m_server->hold = nullptr;
        // At the restart it is sent again, to the calendar chosen now, and this time the answer comes.
        m_sync = makeSync();
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        const QByteArray now = "/cal/work/" + CalendarSync::uidFor(record(16)).toUtf8() + ".ics";
        QVERIFY(m_server->stored.contains(now));
        // Deleting the session deletes it from both.
        m_sync->onSessionDeleted(record(16), true);
        QTRY_COMPARE(m_server->count("DELETE"), 2);
        QTRY_VERIFY(!m_server->stored.contains(path));
        QVERIFY(!m_server->stored.contains(now));
    }

    void deletedSessionSentBeforeTheAddressesWereAListLeavesItsCalendar()
    {
        configureCustom();
        m_sync->enqueueWorkSession(record(17));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        const QByteArray sentTo = m_server->of("PUT").first().path;
        m_sync->setCalendarUrl(m_server->base() + QStringLiteral("/cal/work/"));
        m_sync.reset();

        // As a development build saved it: one address under "url".
        {
            Settings::Ptr config = Settings::open(configPath());
            const QString key = QStringLiteral("CalendarQueue/Sent");
            QJsonArray sent = QJsonDocument::fromJson(config->value(key).toString().toUtf8()).array();
            QCOMPARE(sent.size(), 1);
            QJsonObject entry = sent.at(0).toObject();
            const QJsonArray urls = entry.take(QStringLiteral("urls")).toArray();
            QCOMPARE(urls.size(), 1);
            entry.insert(QStringLiteral("url"), urls.at(0));
            sent[0] = entry;
            config->setValue(key, QString::fromUtf8(QJsonDocument(sent).toJson(QJsonDocument::Compact)));
            Settings::sync(*config);
        }

        m_sync = makeSync();
        m_sync->onSessionDeleted(record(17), true);
        QTRY_COMPARE(m_server->count("DELETE"), 1);
        QCOMPARE(m_server->of("DELETE").first().path, sentTo);
        QCOMPARE(m_server->stored.size(), 0);
    }

    void deletedSessionReadsAKeyringThatWasNotReadyAtStartup()
    {
        FakeSecretStore keyring;
        m_sync = makeSyncWithKeyring(keyring);
        configureCustom();
        m_sync->enqueueWorkSession(record(14));
        QTRY_COMPARE(m_sync->pendingCount(), 0);

        // Restarted while the keyring did not answer yet (still locked at login, say).
        *keyring.available = false;
        m_sync = makeSyncWithKeyring(keyring);
        *keyring.available = true;
        m_sync->onSessionDeleted(record(14), true);
        QTRY_COMPARE(m_server->count("DELETE"), 1);
        QTRY_COMPARE(m_server->stored.size(), 0);
    }

    void passwordInTheFileMovesToTheKeyring()
    {
        configureCustom(); // no keyring: written to the private settings file
        QVERIFY(configFileContents().contains(kPassword.toUtf8()));
        m_sync.reset();

        FakeSecretStore keyring;
        m_sync = makeSyncWithKeyring(keyring);
        QVERIFY(m_sync->hasPassword());
        QVERIFY(m_sync->isPasswordInKeyring());
        QCOMPARE(keyring.secrets->value(QStringLiteral("calendar-caldav")), kPassword);
        QVERIFY(!configFileContents().contains(kPassword.toUtf8()));
    }

    void thePasswordOfAServiceNotInUseLeavesTheFileToo()
    {
        // Left in the file for Custom CalDAV, without a user name, while Nextcloud is selected.
        {
            DiskGroup calendar = diskGroup();
            calendar.writeEntry("Provider", QStringLiteral("nextcloud"));
            calendar.group(QStringLiteral("caldav")).writeEntry("AppPassword", kPassword);
            calendar.sync();
        }
        FakeSecretStore keyring;
        m_sync = makeSyncWithKeyring(keyring);
        QVERIFY(!configFileContents().contains(kPassword.toUtf8()));
        QCOMPARE(keyring.secrets->value(QStringLiteral("calendar-caldav")), kPassword);

        // It is still there once the service is chosen and a user name entered.
        m_sync->setProvider(QStringLiteral("caldav"));
        m_sync->setUsername(QStringLiteral("bob"));
        QVERIFY(m_sync->hasPassword());
    }

    void withoutAKeyringThePasswordStaysInThePrivateFile()
    {
        FakeSecretStore keyring;
        *keyring.available = false;
        m_sync = makeSyncWithKeyring(keyring);
        QVERIFY(!m_sync->passwordOutsideKeyring());
        QSignalSpy storage(m_sync.get(), &CalendarSync::passwordStorageChanged);
        configureCustom();
        QVERIFY(!m_sync->isPasswordInKeyring());
        // The settings tell the user, from the property this sets.
        QVERIFY(m_sync->passwordOutsideKeyring());
        QVERIFY(configFileContents().contains(kPassword.toUtf8()));
        const QFileInfo info(configPath());
        QVERIFY(!(info.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)));

        // Saving the password again tells the settings where it went, though there was one
        // before and there is one after (hasPasswordChanged() is not emitted).
        storage.clear();
        *keyring.available = true;
        m_sync->setPassword(kPassword);
        QVERIFY(m_sync->isPasswordInKeyring());
        QVERIFY(!m_sync->passwordOutsideKeyring());
        QCOMPARE(storage.count(), 1);
        *keyring.available = false;
        m_sync->setPassword(kPassword);
        QVERIFY(m_sync->passwordOutsideKeyring());
        QCOMPARE(storage.count(), 2);
        QVERIFY(configFileContents().contains(kPassword.toUtf8()));

        // The keyring comes back: the next start moves it there.
        *keyring.available = true;
        m_sync = makeSyncWithKeyring(keyring);
        QVERIFY(m_sync->isPasswordInKeyring());
        QVERIFY(!m_sync->passwordOutsideKeyring());
        QVERIFY(!configFileContents().contains(kPassword.toUtf8()));
    }

    void aKeyringThatStartsAfterUsStillGivesThePassword()
    {
        FakeSecretStore keyring;
        m_sync = makeSyncWithKeyring(keyring);
        configureCustom();
        QVERIFY(m_sync->isPasswordInKeyring());

        *keyring.available = false; // the next start comes before the keyring (autostart at login)
        m_sync = makeSyncWithKeyring(keyring);
        QVERIFY(!m_sync->hasPassword());
        *keyring.available = true;
        m_sync->enqueueWorkSession(record());
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QVERIFY(m_sync->hasPassword());
        QCOMPARE(m_server->stored.size(), 1);
    }

    void aRefusedKeyringIsNotAskedAgainAtEverySend()
    {
        FakeSecretStore keyring;
        m_sync = makeSyncWithKeyring(keyring);
        configureCustom();
        QVERIFY(m_sync->isPasswordInKeyring());

        // An update of the macOS app: the next start asks, and the user answers "Deny".
        *keyring.refuses = true;
        *keyring.reads = 0;
        m_sync = makeSyncWithKeyring(keyring);
        QVERIFY(!m_sync->hasPassword());
        const int asked = *keyring.reads;
        QCOMPARE(asked, 1);

        // Work sessions, a connection test, a calendar refresh: no new question, nothing sent.
        m_sync->enqueueWorkSession(record());
        m_sync->enqueueWorkSession(record(8));
        m_sync->testConnection();
        m_sync->refreshCalendars();
        QTest::qWait(200);
        QCOMPARE(*keyring.reads, asked);
        QVERIFY(!m_sync->hasPassword());
        QCOMPARE(m_sync->pendingCount(), 2);
        QCOMPARE(m_server->stored.size(), 0);

        // Entering the password again stores it anew, and what was queued goes out.
        m_sync->setPassword(kPassword);
        QVERIFY(m_sync->hasPassword());
        QVERIFY(m_sync->isPasswordInKeyring());
        m_sync->enqueueWorkSession(record(9));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QCOMPARE(m_server->stored.size(), 3);
    }

    void finishedWorkSessionsDoNotWaitForTheKeyring()
    {
        FakeSecretStore keyring;
        m_sync = makeSyncWithKeyring(keyring);
        configureCustom();
        QVERIFY(m_sync->isPasswordInKeyring());

        // The keyring does not give the password (its unlock prompt was dismissed).
        *keyring.available = false;
        m_sync = makeSyncWithKeyring(keyring);
        QVERIFY(!m_sync->hasPassword());
        QTest::qWait(50);
        const int reads = *keyring.reads;

        // Several sessions end in a row: the keyring is not asked while each one is recorded
        // (the timer is ending the phase then), and is asked only once afterwards.
        m_sync->enqueueWorkSession(record(1));
        m_sync->enqueueWorkSession(record(2));
        m_sync->enqueueWorkSession(record(3));
        QCOMPARE(*keyring.reads, reads);
        QCOMPARE(m_sync->pendingCount(), 3);
        QTRY_COMPARE(*keyring.reads, reads + 1);
        QTest::qWait(100);
        QCOMPARE(*keyring.reads, reads + 1);
        QVERIFY(m_server->requests.isEmpty());

        // Once the keyring answers, the next session sends them all.
        *keyring.available = true;
        m_sync->enqueueWorkSession(record(4));
        QTRY_COMPARE(m_sync->pendingCount(), 0);
        QCOMPARE(m_server->stored.size(), 4);
    }

    void defaults()
    {
        QVERIFY(!m_sync->enabled());
        QCOMPARE(m_sync->provider(), QStringLiteral("nextcloud"));
        QVERIFY(!m_sync->includeNote());
        QVERIFY(!m_sync->hasPassword());
        QCOMPARE(m_sync->status(), CalendarSync::NotConfigured);
        QVERIFY(!m_sync->lastSuccess().isValid());
        QCOMPARE(m_sync->pendingCount(), 0);
        m_sync->setProvider(QStringLiteral("bogus"));
        QCOMPARE(m_sync->provider(), QStringLiteral("nextcloud"));
        QVERIFY(m_server->requests.isEmpty()); // nothing happens on its own
    }
};

QTEST_GUILESS_MAIN(CalendarSyncTest)
#include "tst_calendarsync.moc"
