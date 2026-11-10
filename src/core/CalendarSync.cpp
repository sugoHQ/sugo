// SPDX-License-Identifier: GPL-3.0-or-later
#include "CalendarSync.h"

#include "SecretStore.h"

#include "Localization.h"
#include "RetryAfter.h"
#include "UrlPolicy.h"

#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUuid>
#include <QXmlStreamReader>

#include <algorithm>

namespace
{
const char kGroup[] = "Calendar";
const char kEnabled[] = "Enabled";
const char kProvider[] = "Provider";
const char kServerUrl[] = "ServerUrl";
const char kUsername[] = "Username";
const char kPassword[] = "AppPassword";
const char kCalendarUrl[] = "CalendarUrl";
const char kCalendarName[] = "CalendarName";
const char kIncludeNote[] = "IncludeNote";
const char kQueueGroup[] = "CalendarQueue";
const char kQueuePending[] = "Pending";
const char kQueueSent[] = "Sent";
const char kQueueLeftoverTests[] = "LeftoverTests";

/// Settings that belong to one provider; they live in the subgroup named after it.
bool isProviderKey(const char *key)
{
    for (const char *k : {kServerUrl, kUsername, kPassword, kCalendarUrl, kCalendarName}) {
        if (qstrcmp(key, k) == 0) {
            return true;
        }
    }
    return false;
}

/// "Calendar/caldav/Username" and the like.
QString settingsKey(const QString &group, const char *key)
{
    return group + QLatin1Char('/') + QLatin1String(key);
}

const QString kNextcloud = QStringLiteral("nextcloud");
const QString kCaldav = QStringLiteral("caldav");

constexpr int kMaxQueue = 100;
constexpr int kMaxSent = 50;
constexpr int kLoginTimeoutMs = 15 * 60 * 1000;

QString appVersion()
{
#ifdef PROJECT_VERSION
    return QStringLiteral(PROJECT_VERSION);
#else
    return QCoreApplication::applicationVersion();
#endif
}

/// The URL as text, without a trailing slash and without query or fragment.
QString withoutTrailingSlash(QUrl url)
{
    url.setQuery(QString());
    url.setFragment(QString());
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    url.setPath(path);
    return url.toString(QUrl::FullyEncoded);
}

/// The port, or the scheme's default one when the URL does not name it.
int effectivePort(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    return url.port(scheme == QLatin1String("https") ? 443 : scheme == QLatin1String("http") ? 80 : -1);
}

/// Same scheme, host and port: credentials for one may go to the other.
bool sameOrigin(const QUrl &a, const QUrl &b)
{
    return a.scheme() == b.scheme() && a.host() == b.host() && effectivePort(a) == effectivePort(b);
}

/// The first href inside the WebDAV property `property` (local name) of a multistatus, resolved
/// against `requestUrl`; empty if there is none or it is on another server.
QUrl hrefInProperty(const QByteArray &data, const QString &property, const QUrl &requestUrl)
{
    QXmlStreamReader xml(data);
    bool inProperty = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QString name = xml.name().toString();
            if (name == property) {
                inProperty = true;
            } else if (inProperty && name == QLatin1String("href")) {
                const QString href = xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                if (href.isEmpty()) {
                    return {};
                }
                const QUrl resolved = requestUrl.resolved(QUrl(href));
                return sameOrigin(resolved, requestUrl) ? resolved : QUrl();
            }
        } else if (xml.isEndElement() && xml.name().toString() == property) {
            inProperty = false;
        }
    }
    return {};
}

QString lastSegment(const QString &url)
{
    const QString path = QUrl(url).path();
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return parts.isEmpty() ? QString() : parts.last();
}

QString formatUtc(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
}

bool isSuccess(int http)
{
    return http >= 200 && http < 300;
}

QByteArray basicAuth(const QString &user, const QString &password)
{
    return "Basic " + (user + QLatin1Char(':') + password).toUtf8().toBase64();
}

/// The colour Nextcloud's calendar app gives a calendar that has none of its own (uidToColor
/// in nextcloud/calendar): one of 18 steps between its red, yellow and blue, picked by the sum
/// of the hex digits of the MD5 of the lower-case name. The same name gets the same colour here
/// as in the browser.
QString nameColor(const QString &name)
{
    QString hash = name.toLower();
    static const QRegularExpression md5Like(QStringLiteral("^([0-9a-f]{4}-?){8}$"));
    if (!md5Like.match(hash).hasMatch()) {
        hash = QString::fromLatin1(QCryptographicHash::hash(hash.toUtf8(), QCryptographicHash::Md5).toHex());
    }
    int sum = 0;
    for (const QChar c : std::as_const(hash)) {
        bool ok = false;
        const int digit = QString(c).toInt(&ok, 16);
        if (ok) {
            sum += digit;
        }
    }
    constexpr int steps = 6;
    static const QColor ends[] = {QColor(182, 70, 157), QColor(221, 203, 85), QColor(0, 130, 201), QColor(182, 70, 157)};
    const int index = sum % (steps * 3);
    const QColor &from = ends[index / steps];
    const QColor &to = ends[index / steps + 1];
    const int i = index % steps;
    // As the JavaScript does: parseInt() of a positive number drops the fraction.
    const auto mix = [i](int a, int b) { return static_cast<int>(a + (b - a) / double(steps) * i); };
    return QColor(mix(from.red(), to.red()), mix(from.green(), to.green()), mix(from.blue(), to.blue())).name(QColor::HexRgb);
}

/// A calendar-color as the interface can show it: "#rrggbb". Servers send it as #RRGGBBAA
/// (Apple's property), which QML would read as #AARRGGBB: the alpha is left out; Nextcloud also
/// keeps colours without their "#". A calendar without a colour QColor can read gets the one
/// Nextcloud's web interface shows for it, made from its name; with no name either, none.
QString calendarColor(const QString &value, const QString &name)
{
    static const QRegularExpression hex(QStringLiteral("^#?([0-9A-Fa-f]{3}|[0-9A-Fa-f]{6}|[0-9A-Fa-f]{8})$"));
    const QRegularExpressionMatch m = hex.match(value);
    const QString digits = m.hasMatch() ? m.captured(1) : QString();
    const QColor color = QColor::fromString(m.hasMatch() ? QLatin1Char('#') + digits.left(6) : value);
    if (color.isValid()) {
        return color.name(QColor::HexRgb);
    }
    return name.isEmpty() ? QString() : nameColor(name);
}

/// A 403 to an event (a real one or the test's): the server knows who we are, but the calendar
/// does not take new events (shared read-only, a full quota). Not a rejected login.
QString calendarRefusesEvents()
{
    return i18n("This calendar does not accept new events (HTTP 403): check that you can write to it, "
                "or choose another calendar.");
}

/// Some servers and authentication proxies answer a wrong password with 403 rather than 401.
/// Trying it again every few minutes for ever could get the account locked: after this many
/// 403s in a row, with nothing accepted between, sending waits for the user as after a 401.
/// With the usual backoff that is about two and a half minutes after the first refusal.
constexpr int kForbiddenBeforePause = 3;

/// Why sending is paused after kForbiddenBeforePause 403s: either cause is possible.
QString forbiddenTooOften()
{
    return i18n("Sending paused: the server refused %1 times in a row (HTTP 403). Either this calendar does not accept "
                "new events, or the server rejects the username and app password this way. Check both, then test "
                "the connection.",
                kForbiddenBeforePause);
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------------------------

QString CalendarSync::uidFor(const SessionRecord &record)
{
    // The app ID before 1.0.3 (now stream.sugo.app), kept: an event's UID never changes, so a
    // session sent again replaces its event instead of adding another.
    return QStringLiteral("sugo-%1-%2@io.github.mineraleyt.sugo").arg(record.id).arg(record.startedAtMs);
}

QString CalendarSync::escapeText(const QString &text)
{
    QString out;
    out.reserve(text.size() + 8);
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\\')) {
            out += QLatin1String("\\\\");
        } else if (c == QLatin1Char(';')) {
            out += QLatin1String("\\;");
        } else if (c == QLatin1Char(',')) {
            out += QLatin1String("\\,");
        } else if (c == QLatin1Char('\r')) {
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n')) {
                ++i;
            }
            out += QLatin1String("\\n");
        } else if (c == QLatin1Char('\n')) {
            out += QLatin1String("\\n");
        } else if (c.unicode() < 0x20 && c != QLatin1Char('\t')) {
            continue; // other control characters are not allowed in TEXT
        } else {
            out += c;
        }
    }
    return out;
}

QByteArray CalendarSync::foldLine(const QString &line)
{
    const QByteArray bytes = line.toUtf8();
    QByteArray out;
    int pos = 0;
    bool first = true;
    while (true) {
        const int limit = first ? 75 : 74; // a continuation line starts with one space
        const int remaining = int(bytes.size()) - pos;
        if (remaining <= limit) {
            out += (first ? "" : " ") + bytes.mid(pos) + "\r\n";
            break;
        }
        int end = pos + limit;
        while (end > pos && (static_cast<uchar>(bytes.at(end)) & 0xC0) == 0x80) {
            --end; // do not cut inside a multi-byte character
        }
        out += (first ? "" : " ") + bytes.mid(pos, end - pos) + "\r\n";
        pos = end;
        first = false;
    }
    return out;
}

QByteArray CalendarSync::buildIcs(const SessionRecord &record, bool includeNote, const QString &uid, const QDateTime &stamp)
{
    const qint64 start = record.startedAtMs;
    qint64 end = record.endedAtMs;
    if (end <= start) {
        end = start + qint64(std::max(record.durationSec, 60)) * 1000;
    }
    const QString summary = record.presetName.trimmed().isEmpty() ? i18n("Focus session") : record.presetName.trimmed();

    QStringList lines;
    lines << QStringLiteral("BEGIN:VCALENDAR") << QStringLiteral("VERSION:2.0") << QStringLiteral("PRODID:-//Sugo//EN")
          << QStringLiteral("CALSCALE:GREGORIAN") << QStringLiteral("BEGIN:VEVENT")
          << QStringLiteral("UID:") + escapeText(uid)
          << QStringLiteral("DTSTAMP:") + formatUtc(stamp.toMSecsSinceEpoch())
          << QStringLiteral("DTSTART:") + formatUtc(start) << QStringLiteral("DTEND:") + formatUtc(end)
          << QStringLiteral("SUMMARY:") + escapeText(summary);
    if (!record.category.trimmed().isEmpty()) {
        lines << QStringLiteral("CATEGORIES:") + escapeText(record.category.trimmed());
    }
    if (includeNote && !record.note.trimmed().isEmpty()) {
        lines << QStringLiteral("DESCRIPTION:") + escapeText(record.note.trimmed());
    }
    lines << QStringLiteral("STATUS:CONFIRMED") << QStringLiteral("TRANSP:OPAQUE") << QStringLiteral("END:VEVENT")
          << QStringLiteral("END:VCALENDAR");

    QByteArray out;
    for (const QString &line : std::as_const(lines)) {
        out += foldLine(line);
    }
    return out;
}

bool CalendarSync::isUrlAllowed(const QUrl &url, QString *error)
{
    if (UrlPolicy::isAllowed(url)) {
        return true;
    }
    const QString scheme = url.scheme().toLower();
    if (error) {
        if (scheme == QLatin1String("http")) {
            *error = i18n("Plain %1 addresses are not allowed because the password would be sent unencrypted. Use an %2 address.", QStringLiteral("http://"), QStringLiteral("https://"));
        } else {
            *error = i18n("This is not a valid %1 address.", QStringLiteral("https://"));
        }
    }
    return false;
}

QString CalendarSync::normalizeUrl(const QString &input, QString *error)
{
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    // An address typed without its scheme ("cloud.example.org") is an https one.
    static const QRegularExpression schemeRe(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*://"));
    const QString full = schemeRe.match(trimmed).hasMatch() ? trimmed : QStringLiteral("https://") + trimmed;
    QUrl url(full, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()) {
        if (error) {
            *error = i18n("This is not a valid web address.");
        }
        return {};
    }
    if (!url.userInfo().isEmpty()) {
        if (error) {
            *error = i18n("Do not put the user name or password in the address; enter them in their own fields.");
        }
        return {};
    }
    if (!isUrlAllowed(url, error)) {
        return {};
    }
    url.setScheme(url.scheme().toLower());
    return withoutTrailingSlash(url);
}

QList<CalendarSync::CalendarInfo> CalendarSync::parseCalendars(const QByteArray &data, const QUrl &requestUrl)
{
    struct Properties {
        QString name;
        QString color;
        bool isCalendar = false;
        bool hasComponentSet = false;
        bool vevent = false;
    };
    struct Response {
        QString href;
        Properties props;
    };

    QList<CalendarInfo> result;
    QXmlStreamReader xml(data);
    Response cur;
    Properties stat; ///< Of the propstat being read; kept only if its status is a success.
    bool inResponse = false;
    bool inPropstat = false;
    bool statOk = true;
    bool inResourceType = false;
    bool inComponentSet = false;

    static const QStringList ignored = {QStringLiteral("inbox"), QStringLiteral("outbox"), QStringLiteral("trashbin"),
                                        QStringLiteral("contact_birthdays")};

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QString name = xml.name().toString();
            Properties &props = inPropstat ? stat : cur.props;
            if (name == QLatin1String("response")) {
                cur = Response();
                inResponse = true;
            } else if (!inResponse) {
                continue;
            } else if (name == QLatin1String("propstat")) {
                stat = Properties();
                statOk = true; // a server that leaves out the status means success
                inPropstat = true;
            } else if (name == QLatin1String("status") && inPropstat) {
                // "HTTP/1.1 404 Not Found": a property asked for that the collection does not have.
                const QString statusLine = xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                statOk = isSuccess(statusLine.section(QLatin1Char(' '), 1, 1, QString::SectionSkipEmpty).toInt());
            } else if (name == QLatin1String("href") && cur.href.isEmpty()) {
                cur.href = xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
            } else if (name == QLatin1String("displayname")) {
                props.name = xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
            } else if (name == QLatin1String("calendar-color")) {
                props.color = xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
            } else if (name == QLatin1String("resourcetype")) {
                inResourceType = true;
            } else if (name == QLatin1String("calendar") && inResourceType) {
                props.isCalendar = true;
            } else if (name == QLatin1String("supported-calendar-component-set")) {
                props.hasComponentSet = true;
                inComponentSet = true;
            } else if (name == QLatin1String("comp") && inComponentSet) {
                if (xml.attributes().value(QLatin1String("name")).compare(QLatin1String("VEVENT"), Qt::CaseInsensitive) == 0) {
                    props.vevent = true;
                }
            }
        } else if (xml.isEndElement()) {
            const QString name = xml.name().toString();
            if (name == QLatin1String("resourcetype")) {
                inResourceType = false;
            } else if (name == QLatin1String("supported-calendar-component-set")) {
                inComponentSet = false;
            } else if (name == QLatin1String("propstat") && inPropstat) {
                inPropstat = false;
                if (statOk) {
                    Properties &props = cur.props;
                    if (!stat.name.isEmpty()) {
                        props.name = stat.name;
                    }
                    if (!stat.color.isEmpty()) {
                        props.color = stat.color;
                    }
                    props.isCalendar = props.isCalendar || stat.isCalendar;
                    props.hasComponentSet = props.hasComponentSet || stat.hasComponentSet;
                    props.vevent = props.vevent || stat.vevent;
                }
            } else if (name == QLatin1String("response") && inResponse) {
                inResponse = false;
                inPropstat = false;
                const Properties &props = cur.props;
                if (!props.isCalendar || (props.hasComponentSet && !props.vevent) || cur.href.isEmpty()) {
                    continue;
                }
                const QUrl resolved = requestUrl.resolved(QUrl(cur.href));
                if (!sameOrigin(resolved, requestUrl)) {
                    continue; // never hand our credentials to another server
                }
                const QString url = withoutTrailingSlash(resolved);
                const QString segment = lastSegment(url);
                if (ignored.contains(segment.toLower())) {
                    continue;
                }
                CalendarInfo info;
                info.url = url;
                info.name = props.name.isEmpty() ? segment : props.name;
                info.color = calendarColor(props.color, props.name);
                result.append(info);
            }
        }
    }
    if (xml.hasError()) {
        return {};
    }
    return result;
}

// ---------------------------------------------------------------------------------------------
// Construction and settings
// ---------------------------------------------------------------------------------------------

CalendarSync::CalendarSync(QObject *parent)
    : CalendarSync(Settings::openDefault(), createSecretStore(), parent)
{
}

CalendarSync::CalendarSync(Settings::Ptr config, QObject *parent)
    : CalendarSync(std::move(config), nullptr, parent)
{
}

CalendarSync::CalendarSync(Settings::Ptr config, std::unique_ptr<SecretStore> secrets, QObject *parent)
    : QObject(parent)
    , m_config(std::move(config))
    , m_secrets(std::move(secrets))
    , m_retryDelays({30 * 1000, 2 * 60 * 1000, 10 * 60 * 1000})
{
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &CalendarSync::trySend);
    connect(&m_pollTimer, &QTimer::timeout, this, &CalendarSync::pollLogin);
    m_openUrl = [](const QUrl &url) { return QDesktopServices::openUrl(url); };
    load();
    refreshIdleStatus();
    // Events left waiting by the last run go out once the program is up.
    if (!m_queue.isEmpty()) {
        QTimer::singleShot(0, this, &CalendarSync::trySend);
    }
}

CalendarSync::~CalendarSync() = default;

void CalendarSync::load()
{
    movePasswordsToKeyring();
    const QString group = QLatin1String(kGroup);
    m_enabled = m_config->value(settingsKey(group, kEnabled), false).toBool();
    m_provider = m_config->value(settingsKey(group, kProvider), kNextcloud).toString();
    if (m_provider != kNextcloud && m_provider != kCaldav) {
        m_provider = kNextcloud;
    }
    m_includeNote = m_config->value(settingsKey(group, kIncludeNote), false).toBool();
    loadProviderFields();
    loadQueue();
}

void CalendarSync::loadProviderFields()
{
    const QString group = QLatin1String(kGroup) + QLatin1Char('/') + m_provider;
    const auto read = [&](const char *key) {
        return m_config->value(settingsKey(group, key)).toString();
    };
    // Anything hand-edited into an unsafe form is dropped rather than used.
    m_serverUrl = normalizeUrl(read(kServerUrl), nullptr);
    m_username = read(kUsername).trimmed();
    m_password = read(kPassword);
    m_passwordInKeyring = false;
    m_passwordUnread = false;
    // No account, no reason to wake the keyring up.
    if (m_secrets && !m_username.isEmpty()) {
        if (!m_password.isEmpty()) {
            // Kept in the file while no keyring answered: move it.
            if (m_secrets->write(secretKey(), m_password)) {
                m_passwordInKeyring = true;
                save(kPassword, QString());
            }
        } else if (const auto secret = m_secrets->read(secretKey())) {
            m_password = *secret;
            m_passwordInKeyring = !m_password.isEmpty();
        } else {
            m_passwordUnread = true;
        }
    }
    m_calendarUrl = normalizeUrl(read(kCalendarUrl), nullptr);
    m_calendarName = read(kCalendarName);
}

void CalendarSync::movePasswordsToKeyring()
{
    // A password kept in the file (no keyring at the time) moves to the keyring
    // for every service, not only the selected one and not only with a user name: otherwise a
    // service that is not in use keeps its password readable in the settings file indefinitely.
    if (!m_secrets) {
        return;
    }
    bool moved = false;
    for (const QString &provider : {kNextcloud, kCaldav}) {
        const QString key = settingsKey(QLatin1String(kGroup) + QLatin1Char('/') + provider, kPassword);
        const QString password = m_config->value(key).toString();
        if (!password.isEmpty() && m_secrets->write(QStringLiteral("calendar-") + provider, password)) {
            m_config->remove(key);
            moved = true;
        }
    }
    if (moved) {
        Settings::sync(*m_config);
    }
}

void CalendarSync::save(const char *key, const QVariant &value)
{
    QString group = QLatin1String(kGroup);
    if (isProviderKey(key)) {
        group += QLatin1Char('/') + m_provider;
    }
    if (value.typeId() == QMetaType::QString && value.toString().isEmpty()) {
        m_config->remove(settingsKey(group, key));
    } else {
        m_config->setValue(settingsKey(group, key), value);
    }
    Settings::sync(*m_config);
}

QString CalendarSync::secretKey() const
{
    return QStringLiteral("calendar-") + m_provider;
}

void CalendarSync::storePassword(const QString &password)
{
    m_passwordUnread = false;
    m_password = password;
    if (m_secrets && m_secrets->write(secretKey(), password)) {
        m_passwordInKeyring = !password.isEmpty();
        save(kPassword, QString());
        return;
    }
    // No keyring: the user's private settings file it is.
    m_passwordInKeyring = false;
    save(kPassword, password);
}

void CalendarSync::rereadPassword()
{
    // A keyring started after us (at login) has it by now; the store limits how often it asks.
    if (!m_passwordUnread) {
        return;
    }
    if (const auto secret = m_secrets->read(secretKey())) {
        m_passwordUnread = false;
        m_password = *secret;
        m_passwordInKeyring = !m_password.isEmpty();
        if (hasPassword()) {
            Q_EMIT hasPasswordChanged();
            dropOtherAccounts(); // adopts what was queued while the account looked incomplete
            refreshIdleStatus();
        }
    }
}

QString CalendarSync::currentAccount() const
{
    const QUrl server = QUrl(m_calendarUrl).adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo);
    return m_provider + QLatin1Char('\n') + m_username + QLatin1Char('\n') + server.toString();
}

void CalendarSync::loadQueue()
{
    const QString group = QLatin1String(kQueueGroup);
    const QJsonArray pending = QJsonDocument::fromJson(m_config->value(settingsKey(group, kQueuePending)).toString().toUtf8()).array();
    for (const QJsonValue &value : pending) {
        const QJsonObject o = value.toObject();
        Pending item;
        item.uid = o.value(QLatin1String("uid")).toString();
        item.account = o.value(QLatin1String("account")).toString();
        for (const QJsonValue &url : o.value(QLatin1String("urls")).toArray()) {
            item.urls.append(url.toString());
        }
        SessionRecord &r = item.record;
        r.id = o.value(QLatin1String("id")).toInteger();
        r.kind = SessionKind::Work;
        r.startedAtMs = o.value(QLatin1String("startedAtMs")).toInteger();
        r.endedAtMs = o.value(QLatin1String("endedAtMs")).toInteger();
        r.durationSec = o.value(QLatin1String("durationSec")).toInt();
        r.plannedSec = o.value(QLatin1String("plannedSec")).toInt();
        r.completed = true;
        r.presetUuid = o.value(QLatin1String("presetUuid")).toString();
        r.presetName = o.value(QLatin1String("presetName")).toString();
        r.category = o.value(QLatin1String("category")).toString();
        r.note = o.value(QLatin1String("note")).toString();
        if (!item.uid.isEmpty() && !item.account.isEmpty() && m_queue.size() < kMaxQueue) {
            m_queue.append(item);
        }
    }
    const QJsonArray sent = QJsonDocument::fromJson(m_config->value(settingsKey(group, kQueueSent)).toString().toUtf8()).array();
    for (const QJsonValue &value : sent) {
        const QJsonObject o = value.toObject();
        Pending item;
        item.uid = o.value(QLatin1String("uid")).toString();
        item.account = o.value(QLatin1String("account")).toString();
        for (const QJsonValue &url : o.value(QLatin1String("urls")).toArray()) {
            item.urls.append(url.toString());
        }
        SessionRecord &r = item.record;
        r.id = o.value(QLatin1String("id")).toInteger();
        r.kind = SessionKind::Work;
        r.startedAtMs = o.value(QLatin1String("startedAtMs")).toInteger();
        r.endedAtMs = o.value(QLatin1String("endedAtMs")).toInteger();
        r.durationSec = o.value(QLatin1String("durationSec")).toInt();
        r.plannedSec = o.value(QLatin1String("plannedSec")).toInt();
        r.completed = true;
        r.presetUuid = o.value(QLatin1String("presetUuid")).toString();
        r.presetName = o.value(QLatin1String("presetName")).toString();
        r.category = o.value(QLatin1String("category")).toString();
        r.note = o.value(QLatin1String("note")).toString();
        if (!item.uid.isEmpty() && !item.account.isEmpty() && m_sent.size() < kMaxSent) {
            m_sent.append(item);
        }
    }
    const QJsonArray leftovers =
        QJsonDocument::fromJson(m_config->value(settingsKey(group, kQueueLeftoverTests)).toString().toUtf8()).array();
    for (const QJsonValue &value : leftovers) {
        const QJsonObject o = value.toObject();
        m_leftoverTests.append({o.value(QLatin1String("uid")).toString(), o.value(QLatin1String("account")).toString(),
                                o.value(QLatin1String("url")).toString()});
    }
}

void CalendarSync::saveQueue()
{
    QJsonArray pending;
    for (const Pending &item : std::as_const(m_queue)) {
        const SessionRecord &r = item.record;
        pending.append(QJsonObject{
            {QStringLiteral("uid"), item.uid},
            {QStringLiteral("account"), item.account},
            {QStringLiteral("urls"), QJsonArray::fromStringList(item.urls)},
            {QStringLiteral("id"), r.id},
            {QStringLiteral("startedAtMs"), r.startedAtMs},
            {QStringLiteral("endedAtMs"), r.endedAtMs},
            {QStringLiteral("durationSec"), r.durationSec},
            {QStringLiteral("plannedSec"), r.plannedSec},
            {QStringLiteral("presetUuid"), r.presetUuid},
            {QStringLiteral("presetName"), r.presetName},
            {QStringLiteral("category"), r.category},
            {QStringLiteral("note"), r.note},
        });
    }
    QJsonArray sent;
    for (const Pending &item : std::as_const(m_sent)) {
        const SessionRecord &r = item.record;
        sent.append(QJsonObject{
            {QStringLiteral("uid"), item.uid},
            {QStringLiteral("account"), item.account},
            {QStringLiteral("urls"), QJsonArray::fromStringList(item.urls)},
            {QStringLiteral("id"), r.id},
            {QStringLiteral("startedAtMs"), r.startedAtMs},
            {QStringLiteral("endedAtMs"), r.endedAtMs},
            {QStringLiteral("durationSec"), r.durationSec},
            {QStringLiteral("plannedSec"), r.plannedSec},
            {QStringLiteral("presetUuid"), r.presetUuid},
            {QStringLiteral("presetName"), r.presetName},
            {QStringLiteral("category"), r.category},
            {QStringLiteral("note"), r.note},
        });
    }
    QJsonArray leftovers;
    for (const auto &leftover : std::as_const(m_leftoverTests)) {
        leftovers.append(QJsonObject{{QStringLiteral("uid"), leftover.uid},
                                     {QStringLiteral("account"), leftover.account},
                                     {QStringLiteral("url"), leftover.url}});
    }
    const QString group = QLatin1String(kQueueGroup);
    if (pending.isEmpty()) {
        m_config->remove(settingsKey(group, kQueuePending));
    } else {
        m_config->setValue(settingsKey(group, kQueuePending), QString::fromUtf8(QJsonDocument(pending).toJson(QJsonDocument::Compact)));
    }
    if (sent.isEmpty()) {
        m_config->remove(settingsKey(group, kQueueSent));
    } else {
        m_config->setValue(settingsKey(group, kQueueSent), QString::fromUtf8(QJsonDocument(sent).toJson(QJsonDocument::Compact)));
    }
    if (leftovers.isEmpty()) {
        m_config->remove(settingsKey(group, kQueueLeftoverTests));
    } else {
        m_config->setValue(settingsKey(group, kQueueLeftoverTests),
                           QString::fromUtf8(QJsonDocument(leftovers).toJson(QJsonDocument::Compact)));
    }
    Settings::sync(*m_config);
}

void CalendarSync::queueChanged()
{
    saveQueue();
    Q_EMIT pendingCountChanged();
}

bool CalendarSync::configured() const
{
    return !m_username.isEmpty() && !m_password.isEmpty() && !m_calendarUrl.isEmpty();
}

QString CalendarSync::calendarName() const
{
    if (!m_calendarName.isEmpty()) {
        return m_calendarName;
    }
    return m_calendarUrl.isEmpty() ? QString() : lastSegment(m_calendarUrl);
}

QVariantList CalendarSync::calendars() const
{
    QVariantList list;
    for (const CalendarInfo &c : m_calendars) {
        list.append(QVariantMap{{QStringLiteral("name"), c.name}, {QStringLiteral("url"), c.url}, {QStringLiteral("color"), c.color}});
    }
    return list;
}

void CalendarSync::setEnabled(bool enabled)
{
    if (m_enabled == enabled) {
        return;
    }
    m_enabled = enabled;
    save(kEnabled, enabled);
    if (!enabled) {
        // Nothing may be sent later for what happened while it was off.
        abortAll();
        clearQueue();
        m_retryTimer.stop();
        m_retryStep = 0;
        m_sendFailing = false;
        refreshIdleStatus();
    } else {
        trySend();
    }
    Q_EMIT enabledChanged();
}

void CalendarSync::setProvider(const QString &provider)
{
    const QString p = provider.trimmed().toLower();
    if (p != kNextcloud && p != kCaldav) {
        return;
    }
    switchProvider(p);
}

void CalendarSync::switchProvider(const QString &provider)
{
    if (provider == m_provider) {
        return;
    }
    // Whatever is running (login flow, discovery, test) belongs to the provider being left.
    abortAll();
    m_provider = provider;
    save(kProvider, provider);

    const QString oldServer = m_serverUrl;
    const QString oldUser = m_username;
    const bool hadPassword = hasPassword();
    const QString oldCalendarUrl = m_calendarUrl;
    const QString oldCalendarName = calendarName();
    m_calendars.clear();
    m_calendarsFresh = QDeadlineTimer();
    loadProviderFields();

    Q_EMIT providerChanged();
    Q_EMIT calendarsChanged();
    if (m_serverUrl != oldServer) {
        Q_EMIT serverUrlChanged();
    }
    if (m_username != oldUser) {
        Q_EMIT usernameChanged();
    }
    if (hasPassword() != hadPassword) {
        Q_EMIT hasPasswordChanged();
    }
    if (m_calendarUrl != oldCalendarUrl) {
        Q_EMIT calendarUrlChanged();
    }
    if (calendarName() != oldCalendarName) {
        Q_EMIT calendarNameChanged();
    }
    credentialsChanged();
}

void CalendarSync::setServerUrl(const QString &url)
{
    QString error;
    const QString normalized = normalizeUrl(url, &error);
    if (!error.isEmpty()) {
        rejectInput(error);
        return;
    }
    if (normalized == m_serverUrl) {
        return;
    }
    m_serverUrl = normalized;
    m_calendars.clear();
    m_calendarsFresh = QDeadlineTimer();
    save(kServerUrl, normalized);
    // The calendar chosen on the old server is not on this one.
    const bool hadCalendar = !m_calendarUrl.isEmpty();
    m_calendarUrl.clear();
    m_calendarName.clear();
    save(kCalendarUrl, QString());
    save(kCalendarName, QString());
    Q_EMIT serverUrlChanged();
    Q_EMIT calendarsChanged();
    if (hadCalendar) {
        Q_EMIT calendarUrlChanged();
        Q_EMIT calendarNameChanged();
    }
    credentialsChanged();
}

void CalendarSync::setUsername(const QString &username)
{
    const QString name = username.trimmed();
    if (name == m_username) {
        return;
    }
    m_username = name;
    save(kUsername, name);
    Q_EMIT usernameChanged();
    // A password stored before the user name (e.g. moved to the keyring at start) is read now.
    if (m_secrets && m_password.isEmpty() && !name.isEmpty()) {
        m_passwordUnread = true;
        rereadPassword();
    }
    credentialsChanged();
}

void CalendarSync::setPassword(const QString &password)
{
    // Stored as typed: a space at either end may be part of it.
    const bool had = hasPassword();
    storePassword(password);
    if (had != hasPassword()) {
        Q_EMIT hasPasswordChanged();
    }
    credentialsChanged();
}

void CalendarSync::setCalendarUrl(const QString &url)
{
    QString error;
    const QString normalized = normalizeUrl(url, &error);
    if (!error.isEmpty() || !calendarOnServer(normalized, &error)) {
        rejectInput(error);
        return;
    }
    if (normalized == m_calendarUrl) {
        return;
    }
    m_calendarUrl = normalized;
    m_calendarName.clear();
    save(kCalendarUrl, normalized);
    save(kCalendarName, QString());
    Q_EMIT calendarUrlChanged();
    Q_EMIT calendarNameChanged();
    credentialsChanged();
}

void CalendarSync::setIncludeNote(bool include)
{
    if (include == m_includeNote) {
        return;
    }
    m_includeNote = include;
    save(kIncludeNote, include);
    Q_EMIT includeNoteChanged();
}

void CalendarSync::selectCalendar(const QString &url, const QString &name)
{
    QString error;
    const QString normalized = normalizeUrl(url, &error);
    if (normalized.isEmpty()) {
        setLastError(error.isEmpty() ? i18n("Choose a calendar first.") : error);
        return;
    }
    if (!calendarOnServer(normalized, &error)) {
        setLastError(error);
        return;
    }
    const bool urlChanged = normalized != m_calendarUrl;
    m_calendarUrl = normalized;
    m_calendarName = name.trimmed();
    save(kCalendarUrl, normalized);
    save(kCalendarName, m_calendarName);
    if (urlChanged) {
        Q_EMIT calendarUrlChanged();
    }
    Q_EMIT calendarNameChanged();
    credentialsChanged();
}

void CalendarSync::disconnect()
{
    abortAll();
    clearQueue();
    m_sent.clear();
    m_leftoverTests.clear();
    saveQueue();
    m_retryTimer.stop();
    m_retryStep = 0;
    m_paused = false;
    m_forbiddenStreak = 0;
    m_sendFailing = false;

    const bool hadPassword = hasPassword();
    storePassword(QString());
    m_username.clear();
    m_calendarUrl.clear();
    m_calendarName.clear();
    m_calendars.clear();
    m_calendarsFresh = QDeadlineTimer();
    const bool wasEnabled = m_enabled;
    m_enabled = false;
    save(kUsername, QString());
    save(kCalendarUrl, QString());
    save(kCalendarName, QString());
    save(kEnabled, false);
    setLastError(QString());
    if (hadPassword) {
        Q_EMIT hasPasswordChanged();
    }
    Q_EMIT usernameChanged();
    Q_EMIT calendarUrlChanged();
    Q_EMIT calendarNameChanged();
    Q_EMIT calendarsChanged();
    if (wasEnabled) {
        Q_EMIT enabledChanged();
    }
    refreshIdleStatus();
}

void CalendarSync::credentialsChanged()
{
    // New credentials (or calendar) deserve a fresh attempt, immediately.
    m_paused = false;
    m_forbiddenStreak = 0;
    m_sendFailing = false;
    m_retryTimer.stop();
    m_retryStep = 0;
    dropOtherAccounts();
    refreshIdleStatus();
    trySend();
}

void CalendarSync::dropOtherAccounts()
{
    // Once this provider is set up for another account, what the previous one left waiting
    // can never be delivered: it must not go to the new account either.
    if (!configured()) {
        return;
    }
    const QString account = currentAccount();
    const QString prefix = m_provider + QLatin1Char('\n');

    // Events from before this account was complete: adopted if nothing says they belong to
    // someone else (no user name given then, or this one), dropped otherwise.
    const QString unboundPrefix = QStringLiteral("unbound\n");
    bool adopted = false;
    for (Pending &item : m_queue) {
        if (item.account.startsWith(unboundPrefix)) {
            const QString user = item.account.mid(unboundPrefix.size());
            if (user.isEmpty() || user == m_username) {
                item.account = account;
                adopted = true;
            }
        }
    }
    const auto stale = [&](const Pending &item) {
        return (item.account.startsWith(prefix) || item.account.startsWith(unboundPrefix)) && item.account != account;
    };
    const auto removed = m_queue.removeIf(stale);
    m_sent.removeIf(stale);
    m_leftoverTests.removeIf([&](const LeftoverTest &leftover) {
        return leftover.account.startsWith(prefix) && leftover.account != account;
    });
    if (removed > 0 || adopted) {
        queueChanged();
    } else {
        saveQueue();
    }
}

QString CalendarSync::unboundAccount() const
{
    return QStringLiteral("unbound\n") + m_username;
}

// ---------------------------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------------------------

void CalendarSync::setStatus(Status status, const QString &text)
{
    const QString scrubbed = scrub(text);
    if (status == m_status && scrubbed == m_statusText) {
        return;
    }
    m_status = status;
    m_statusText = scrubbed;
    Q_EMIT statusChanged();
}

void CalendarSync::setLastError(const QString &error)
{
    const QString scrubbed = scrub(error);
    if (scrubbed == m_lastError) {
        return;
    }
    m_lastError = scrubbed;
    Q_EMIT lastErrorChanged();
}

void CalendarSync::rejectInput(const QString &error)
{
    setLastError(error);
    Q_EMIT inputRejected(scrub(error));
}

QString CalendarSync::scrub(const QString &text) const
{
    QString out = text;
    if (m_password.size() >= 4) {
        out.replace(m_password, QStringLiteral("***"));
    }
    return out;
}

void CalendarSync::refreshIdleStatus()
{
    if (m_loginActive || m_busy > 0) {
        return;
    }
    if (m_paused) {
        setStatus(Failed, m_forbiddenStreak >= kForbiddenBeforePause ? forbiddenTooOften()
                                                                     : i18n("Login rejected: check username and app password"));
    } else if (!configured()) {
        if (!m_username.isEmpty() && !m_password.isEmpty()) {
            setStatus(NotConfigured, i18n("Signed in as %1. Choose a calendar to finish.", m_username));
        } else {
            setStatus(NotConfigured, i18n("Not configured"));
        }
    } else if (m_sendFailing) {
        setStatus(Failed, i18n("Could not send events to the calendar; trying again later"));
    } else {
        setStatus(Ready, i18n("Connected as %1", m_username));
    }
}

QString CalendarSync::describeFailure(const Result &result) const
{
    QString text;
    if (result.http == 401) {
        // Not 403: that one is the server refusing the request to someone it knows (a calendar
        // shared read-only, an event that may not be removed), handled where it can happen.
        text = i18n("Login rejected: check username and app password");
    } else if (result.http == 404) {
        text = i18n("Calendar not found");
    } else if (result.http == 409) {
        text = i18n("Calendar not found (HTTP 409): it may have been deleted or renamed");
    } else if (result.http == 423) {
        text = i18n("The calendar is locked by another client (HTTP 423)");
    } else if (result.http > 0) {
        text = i18n("Server answered HTTP %1", result.http);
    } else {
        text = result.networkError.isEmpty() ? i18n("The server could not be reached") : result.networkError;
    }
    return scrub(text);
}

void CalendarSync::setRetryDelaysForTesting(const QList<int> &delaysMs)
{
    if (!delaysMs.isEmpty()) {
        m_retryDelays = delaysMs;
    }
}

// ---------------------------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------------------------

QNetworkAccessManager *CalendarSync::network()
{
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    return m_nam;
}

void CalendarSync::request(const QByteArray &verb, const QUrl &url, const QByteArray &body,
                           const QList<QPair<QByteArray, QByteArray>> &headers, bool withAuth,
                           std::function<void(const Result &)> done)
{
    QString error;
    // The password only ever goes to the server it belongs to.
    if (!isUrlAllowed(url, &error) || (withAuth && !calendarOnServer(url.toString(), &error))) {
        Result r;
        r.networkError = error;
        const quint64 gen = m_generation;
        QTimer::singleShot(0, this, [this, done, r, gen]() {
            if (gen == m_generation) {
                done(r);
            }
        });
        return;
    }

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Sugo/%1").arg(appVersion()));
    if (withAuth) {
        req.setRawHeader("Authorization", basicAuth(m_username, m_password));
    }
    for (const auto &h : headers) {
        req.setRawHeader(h.first, h.second);
    }
    // Only same-origin redirects (same scheme, host and port): the credentials never follow
    // a redirect to another server, and https never falls back to http.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    req.setTransferTimeout(20000);

    QNetworkReply *reply = network()->sendCustomRequest(req, verb, body);
    m_replies.insert(reply);
    const quint64 gen = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen, done]() {
        m_replies.remove(reply);
        reply->deleteLater();
        if (gen != m_generation) {
            return;
        }
        Result r;
        r.http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r.body = reply->readAll();
        if (r.http == 0) {
            r.networkError = reply->errorString();
        }
        if (!isSuccess(r.http)) {
            r.retryAfter = RetryAfter::seconds(reply->rawHeader("Retry-After"));
        }
        done(r);
    });
}

void CalendarSync::abortAll()
{
    ++m_generation;
    ++m_loginGeneration;
    const auto replies = m_replies;
    m_replies.clear();
    for (QNetworkReply *reply : replies) {
        reply->abort();
        reply->deleteLater();
    }
    m_busy = 0;
    m_inFlight = false;
    m_inFlightUid.clear();
    m_loginStarting = false;
    endLogin();
}

QUrl CalendarSync::eventUrl(const QString &uid) const
{
    return QUrl(m_calendarUrl + QLatin1Char('/') + QString::fromLatin1(QUrl::toPercentEncoding(uid, "@")) + QLatin1String(".ics"));
}

QUrl CalendarSync::discoveryUrl() const
{
    if (m_provider == kNextcloud) {
        if (m_serverUrl.isEmpty() || m_username.isEmpty()) {
            return {};
        }
        return QUrl(m_serverUrl + QLatin1String("/remote.php/dav/calendars/")
                    + QString::fromLatin1(QUrl::toPercentEncoding(m_username)) + QLatin1Char('/'));
    }
    const QString base = m_serverUrl.isEmpty() ? m_calendarUrl : m_serverUrl;
    return base.isEmpty() ? QUrl() : QUrl(base + QLatin1Char('/'));
}

// ---------------------------------------------------------------------------------------------
// Queue
// ---------------------------------------------------------------------------------------------

void CalendarSync::clearQueue()
{
    if (m_queue.isEmpty()) {
        return;
    }
    m_queue.clear();
    queueChanged();
}

bool CalendarSync::sameEvent(const SessionRecord &a, const SessionRecord &b) const
{
    return a.startedAtMs == b.startedAtMs && a.endedAtMs == b.endedAtMs && a.presetName == b.presetName
        && a.category == b.category && (!m_includeNote || a.note == b.note);
}

bool CalendarSync::calendarOnServer(const QString &calendarUrl, QString *error) const
{
    if (m_provider != kNextcloud || m_serverUrl.isEmpty() || calendarUrl.isEmpty()) {
        return true;
    }
    const QUrl calendar(calendarUrl);
    const QUrl server(m_serverUrl);
    const int defaultPort = server.scheme() == QLatin1String("https") ? 443 : 80;
    if (calendar.scheme() == server.scheme() && calendar.host().compare(server.host(), Qt::CaseInsensitive) == 0
        && calendar.port(defaultPort) == server.port(defaultPort)) {
        return true;
    }
    if (error) {
        *error = i18n("The calendar must be on the Nextcloud server you signed in to.");
    }
    return false;
}

int CalendarSync::inFlightIndex() const
{
    if (!m_inFlight) {
        return -1;
    }
    for (int i = 0; i < m_queue.size(); ++i) {
        if (m_queue.at(i).uid == m_inFlightUid) {
            return i;
        }
    }
    return -1;
}

void CalendarSync::appendBounded(const Pending &item)
{
    if (m_queue.size() >= kMaxQueue) {
        // Never drop the one being sent right now.
        m_queue.removeAt(inFlightIndex() == 0 && m_queue.size() > 1 ? 1 : 0);
        setLastError(i18n("Too many calendar events are waiting to be sent: the oldest one was dropped."));
    }
    m_queue.append(item);
    queueChanged();
}

void CalendarSync::enqueueWorkSession(const SessionRecord &record)
{
    if (!m_enabled) {
        return;
    }
    if (record.kind != SessionKind::Work || !record.completed) {
        return;
    }
    // Before an account is complete (signing in, a calendar not chosen yet) the event waits,
    // bound to the user name only, if one is given; the account set up next adopts it.
    Pending item{record, uidFor(record), configured() ? currentAccount() : unboundAccount(), QStringList()};
    for (Pending &queued : m_queue) {
        if (queued.uid == item.uid) {
            queued.record = record;
            saveQueue();
            return;
        }
    }
    appendBounded(item);
    // This runs while the timer is ending the phase. Asking the keyring for a password it could
    // not give yet may show its unlock prompt and wait for the answer: that is left to the event
    // loop, so the next phase and its notification are not held up meanwhile.
    if (m_passwordUnread) {
        trySendLater();
    } else {
        trySend();
    }
}

void CalendarSync::onNoteChanged(qint64 sessionId, const QString &note)
{
    if (!m_includeNote || !m_enabled) {
        return;
    }
    for (Pending &queued : m_queue) {
        if (queued.record.id == sessionId) {
            queued.record.note = note;
            saveQueue();
            return;
        }
    }
    for (Pending &sent : m_sent) {
        if (sent.record.id == sessionId) {
            // With the queue full, the note waits rather than push out an event never sent:
            // the sent copy keeps its old note, so the next change of it is tried again.
            if (sent.record.note == note || m_queue.size() >= kMaxQueue) {
                return;
            }
            sent.record.note = note;
            appendBounded(sent);
            trySend();
            return;
        }
    }
}

void CalendarSync::onSessionDeleted(const SessionRecord &record, bool fromCalendar)
{
    if (record.kind != SessionKind::Work || !record.completed) {
        return; // never sent
    }
    const QString uid = uidFor(record);
    const auto sameUid = [&uid](const Pending &item) {
        return item.uid == uid;
    };
    // Waiting: it never goes. On its way: onSendFinished() finds it gone from the queue.
    const bool inFlight = m_inFlight && m_inFlightUid == uid;
    QString account;
    // Everywhere it may be: where it went or was tried (the calendar chosen then, which may not be
    // the one chosen now), and where it is on its way to.
    QStringList urls;
    const auto addUrl = [&urls](const QString &url) {
        if (!url.isEmpty() && !urls.contains(url)) {
            urls.append(url);
        }
    };
    if (inFlight) {
        addUrl(m_inFlightUrl);
    }
    for (const Pending &item : std::as_const(m_sent)) {
        if (item.uid == uid) {
            account = item.account;
            for (const QString &url : item.urls) {
                addUrl(url);
            }
        }
    }
    for (const Pending &item : std::as_const(m_queue)) {
        // Tried (it has a place): queued for the account it was sent with. Never tried: nowhere yet.
        if (item.uid == uid && !item.urls.isEmpty()) {
            if (account.isEmpty()) {
                account = item.account;
            }
            for (const QString &url : item.urls) {
                addUrl(url);
            }
        }
    }
    m_queue.removeIf(sameUid);
    m_sent.removeIf(sameUid);
    // The keyring was not ready at startup: asked again now, at the user's request, rather than
    // taking the account for incomplete and leaving the event behind.
    if (fromCalendar && m_enabled) {
        rereadPassword();
    }
    // Even an event still waiting may be on the server: an upload the server took but whose answer
    // never came (Sugo quit, the network dropped) leaves it queued. A DELETE of an event that is
    // not there is answered 404, which counts as done.
    if (fromCalendar && m_enabled && configured()) {
        if (account.isEmpty()) {
            account = currentAccount();
        }
        // Sent too long ago to be remembered (or by a version of Sugo that did not keep where):
        // the calendar chosen now.
        if (urls.isEmpty()) {
            urls.append(eventUrl(uid).toString());
        }
        for (const QString &url : std::as_const(urls)) {
            m_leftoverTests.removeIf([&uid, &url](const LeftoverTest &leftover) {
                return leftover.uid == uid && leftover.url == url;
            });
            m_leftoverTests.append({uid, account, url});
        }
        // An event still on its way is deleted once its upload has finished.
        if (!inFlight) {
            removeLeftoverTestEvents();
        }
    }
    queueChanged();
    refreshIdleStatus();
}

void CalendarSync::trySend()
{
    rereadPassword();
    if (!m_enabled || !configured() || m_paused || m_inFlight || m_queue.isEmpty() || m_retryTimer.isActive()) {
        return;
    }
    if (!isUrlAllowed(QUrl(m_calendarUrl))) {
        return;
    }
    // Only what was queued for the account set up now; the rest waits for its own account.
    const QString account = currentAccount();
    const auto it = std::find_if(m_queue.cbegin(), m_queue.cend(), [&](const Pending &item) {
        return item.account == account;
    });
    if (it == m_queue.cend()) {
        return;
    }
    m_inFlight = true;
    m_inFlightUid = it->uid;
    m_inFlightRecord = it->record;
    m_inFlightUrl = eventUrl(it->uid).toString();
    // Kept before the request goes: if its answer is lost (Sugo quits), the event may be there.
    if (!it->urls.contains(m_inFlightUrl)) {
        m_queue[it - m_queue.cbegin()].urls.append(m_inFlightUrl);
        saveQueue();
    }
    const QByteArray ics = buildIcs(it->record, m_includeNote, it->uid);
    request("PUT", QUrl(m_inFlightUrl), ics, {{"Content-Type", "text/calendar; charset=utf-8"}}, true,
            [this, account, password = m_password, calendar = m_calendarUrl](const Result &r) {
                onSendFinished(r, account != currentAccount() || password != m_password || calendar != m_calendarUrl);
            });
}

void CalendarSync::trySendLater()
{
    if (m_sendLater) {
        return;
    }
    m_sendLater = true;
    QTimer::singleShot(0, this, [this]() {
        m_sendLater = false;
        trySend();
    });
}

void CalendarSync::scheduleRetry(int retryAfterSeconds)
{
    const int index = std::min(m_retryStep, int(m_retryDelays.size()) - 1);
    ++m_retryStep;
    // A server asking for a longer pause (429, 503) is not asked again sooner.
    m_retryTimer.start(std::max(m_retryDelays.at(index), retryAfterSeconds * 1000));
}

void CalendarSync::rememberSent(const Pending &item)
{
    for (Pending &sent : m_sent) {
        if (sent.uid == item.uid) {
            sent = item;
            return;
        }
    }
    m_sent.append(item);
    while (m_sent.size() > kMaxSent) {
        m_sent.removeFirst();
    }
}

void CalendarSync::onSendFinished(const Result &r, bool credentialsChangedMeanwhile)
{
    const int index = inFlightIndex();
    m_inFlight = false;
    m_inFlightUid.clear();
    if (index < 0) {
        // Removed meanwhile (disabled, disconnected, another account, its session deleted):
        // nothing to account for. The event of a deleted session may have just been put on the
        // server: it goes now, with the other events waiting to be removed.
        if (isSuccess(r.http) && !credentialsChangedMeanwhile) {
            removeLeftoverTestEvents();
        }
        trySend();
        return;
    }
    if (isSuccess(r.http)) {
        Pending sentItem = m_queue.at(index);
        sentItem.record = m_inFlightRecord;
        if (!sentItem.urls.contains(m_inFlightUrl)) {
            sentItem.urls.append(m_inFlightUrl);
        }
        rememberSent(sentItem);
        // Changed while this request was on its way (a note, or the session itself): send it again.
        const bool changedMeanwhile = !sameEvent(m_queue.at(index).record, m_inFlightRecord);
        if (!changedMeanwhile) {
            m_queue.removeAt(index);
        }
        m_retryStep = 0;
        // The server took an event: a pause set meanwhile (refused tests, a lookup) is over.
        m_paused = false;
        m_forbiddenStreak = 0;
        m_sendFailing = false;
        m_lastSuccess = QDateTime::currentDateTime();
        setLastError(QString());
        Q_EMIT lastSuccessChanged();
        queueChanged();
        refreshIdleStatus();
        removeLeftoverTestEvents();
        trySend();
        return;
    }

    if (credentialsChangedMeanwhile) {
        // Refused with the old credentials or by the calendar chosen before: says nothing about
        // the new ones, which get their turn now (not paused, skipped or delayed for it).
        trySend();
        return;
    }
    Q_EMIT sendFailed(r.http == 0                                                   ? QStringLiteral("network")
                          : r.http == 401                                           ? QStringLiteral("auth")
                          : r.http == 403                                           ? QStringLiteral("refused")
                          : r.http == 404 || r.http == 409                          ? QStringLiteral("not_found")
                          : r.http == 408 || r.http == 423 || r.http == 429         ? QStringLiteral("rate_limited")
                          : r.http >= 400 && r.http < 500                           ? QStringLiteral("rejected_event")
                                                                                    : QStringLiteral("server"));
    if (r.http == 401) {
        const QString message = describeFailure(r);
        m_paused = true;
        m_forbiddenStreak = 0;
        setLastError(message);
        setStatus(Failed, message);
        return;
    }
    if (r.http == 403 && ++m_forbiddenStreak >= kForbiddenBeforePause) {
        const QString message = forbiddenTooOften();
        m_paused = true;
        setLastError(message);
        setStatus(Failed, message);
        return;
    }
    // Refusals about the calendar rather than the event keep the queue and are tried again:
    // 403 (read-only, quota), 404 (calendar not there), 408/429 (slow down), 409 (RFC 4918: the
    // collection the event goes into does not exist, a calendar deleted or renamed) and 423
    // (locked by another client). Skipping on those would drop every queued event in turn.
    const bool aboutTheCalendar = r.http == 403 || r.http == 404 || r.http == 408 || r.http == 409 || r.http == 423
        || r.http == 429;
    if (r.http >= 400 && r.http < 500 && !aboutTheCalendar) {
        // This event will never be accepted (400, 412, 413, 415...); do not let it hold back the others.
        m_queue.removeAt(index);
        setLastError(i18n("The server refused an event (HTTP %1); it was skipped.", r.http));
        queueChanged();
        trySend();
        return;
    }
    m_sendFailing = true;
    // 403: most likely the server knows who we are, but the calendar does not take new events
    // (shared read-only, a full quota). Sending is not paused on the first ones: the events wait,
    // and go once the calendar accepts them or another one is chosen (see kForbiddenBeforePause).
    setLastError(r.http == 403 ? calendarRefusesEvents() : describeFailure(r));
    refreshIdleStatus();
    scheduleRetry(r.retryAfter);
}

void CalendarSync::removeLeftoverTestEvents()
{
    const QString account = currentAccount();
    for (const auto &leftover : std::as_const(m_leftoverTests)) {
        if (leftover.account != account) {
            continue;
        }
        const QString uid = leftover.uid;
        // Its upload is still on its way: a DELETE now could find nothing (404) and forget it,
        // and the event would then arrive. It goes once the upload has finished.
        if (m_inFlight && m_inFlightUid == uid) {
            continue;
        }
        const QString where = leftover.url;
        const QUrl url = where.isEmpty() ? eventUrl(uid) : QUrl(where);
        request("DELETE", url, QByteArray(), {}, true, [this, uid, where](const Result &del) {
            if (isSuccess(del.http) || del.http == 404) {
                // Only this place: the same event may still wait to go from another calendar.
                m_leftoverTests.removeIf([&](const LeftoverTest &l) {
                    return l.uid == uid && l.url == where;
                });
                saveQueue();
            }
        });
    }
}

// ---------------------------------------------------------------------------------------------
// Test connection and discovery
// ---------------------------------------------------------------------------------------------

void CalendarSync::testConnection()
{
    rereadPassword();
    if (!configured()) {
        setLastError(i18n("Choose a calendar first."));
        refreshIdleStatus();
        return;
    }
    if (m_busy > 0) {
        return;
    }
    ++m_busy;
    setLastError(QString());
    setStatus(Working, i18n("Testing the connection…"));

    const QString uid = QStringLiteral("sugo-test-") + QUuid::createUuid().toString(QUuid::Id128);
    SessionRecord record;
    record.startedAtMs = QDateTime::currentMSecsSinceEpoch();
    record.endedAtMs = record.startedAtMs + 60 * 1000;
    record.presetName = i18n("Sugo connection test");
    const QByteArray ics = buildIcs(record, false, uid);

    auto fail = [this](const Result &r) {
        --m_busy;
        // As for a real event (onSendFinished()): a 401 is the login and pauses sending, a 403
        // only once it comes kForbiddenBeforePause times in a row. Before that, a 403 lifts the
        // pause of an earlier 401: this server answers a wrong password with 401, so it took the
        // login this time, and the events waiting are tried again.
        if (r.http == 401) {
            m_paused = true;
            m_forbiddenStreak = 0;
        } else if (r.http == 403) {
            ++m_forbiddenStreak;
            m_paused = m_forbiddenStreak >= kForbiddenBeforePause;
        }
        QString message = describeFailure(r);
        if (r.http == 403) {
            message = m_paused ? forbiddenTooOften() : calendarRefusesEvents();
        }
        setLastError(message);
        setStatus(Failed, message);
        if (r.http == 403) {
            trySend();
        }
    };

    // The test event is removed from where it was put, even if the calendar changes meanwhile.
    const QUrl url = eventUrl(uid);
    const QString account = currentAccount();
    request("PUT", url, ics, {{"Content-Type", "text/calendar; charset=utf-8"}}, true,
            [this, uid, url, fail, account, password = m_password](const Result &put) {
        const auto stale = [this, account, password] {
            return account != currentAccount() || password != m_password;
        };
        // Another account (or password) was set up meanwhile: this result says nothing about it.
        // An event left on the server stays listed for the old account, removed if that account
        // is the one set up when the next event goes through; dropOtherAccounts() forgets it
        // once another account of this provider is changed.
        const auto abandon = [this, uid, url, account](bool leftover) {
            --m_busy;
            if (leftover) {
                m_leftoverTests.append({uid, account, url.toString()});
                saveQueue();
            }
            refreshIdleStatus();
            trySend();
        };
        if (stale()) {
            // Its credentials must not go to the old calendar.
            abandon(isSuccess(put.http));
            return;
        }
        if (!isSuccess(put.http)) {
            fail(put);
            return;
        }
        request("DELETE", url, QByteArray(), {}, true, [this, uid, url, account, stale, abandon](const Result &del) {
            const bool removed = isSuccess(del.http) || del.http == 404;
            if (stale()) {
                abandon(!removed);
                return;
            }
            --m_busy;
            m_paused = false;
            m_forbiddenStreak = 0;
            m_sendFailing = false;
            m_retryTimer.stop();
            m_retryStep = 0;
            if (removed) {
                setLastError(QString());
            } else {
                // Tried again after the next event that goes through.
                m_leftoverTests.append({uid, account, url.toString()});
                saveQueue();
                setLastError(i18n("The connection works, but the test event could not be removed yet (%1). Sugo will try again later.",
                                  describeFailure(del)));
            }
            setStatus(Ready, i18n("Connection test succeeded"));
            trySend();
        });
    });
}

void CalendarSync::discover(std::function<void(bool)> done)
{
    const QUrl url = discoveryUrl();
    if (!url.isValid() || m_username.isEmpty() || m_password.isEmpty()) {
        setLastError(i18n("Enter the server address, user name and password first."));
        refreshIdleStatus();
        done(false);
        return;
    }
    ++m_busy;
    setLastError(QString());
    setStatus(Working, i18n("Looking for calendars…"));
    // Asked for another server or account than the one set up now: neither its calendars nor a
    // refusal apply to this one.
    const std::function<bool()> stale = [this, url, username = m_username, password = m_password]() {
        return url != discoveryUrl() || username != m_username || password != m_password;
    };
    if (m_provider != kNextcloud) {
        listCalendars(url, stale, done);
        return;
    }

    // Nextcloud names its DAV folders after the user id, which is not always the login name (an
    // e-mail address, an LDAP login): the server says where the calendars are. A server that
    // does not say, or does not let this account ask (403), gets the usual place of the login
    // name's calendars (discoveryUrl()); one that refuses the login or cannot be reached is not
    // asked again.
    const auto abandon = [this, done]() {
        --m_busy;
        refreshIdleStatus();
        done(false);
    };
    const std::function<void(const Result &)> failed = [this, stale, done, abandon](const Result &r) {
        if (stale()) {
            abandon();
            return;
        }
        --m_busy;
        discoveryFailed(r);
        done(false);
    };
    findHref(
        QUrl(m_serverUrl + QLatin1String("/remote.php/dav/")), "<d:current-user-principal/>", QStringLiteral("current-user-principal"),
        [this, url, stale, done, abandon, failed](const QUrl &principal) {
            if (stale()) {
                abandon();
                return;
            }
            if (principal.isEmpty()) {
                listCalendars(url, stale, done);
                return;
            }
            findHref(
                principal, "<c:calendar-home-set/>", QStringLiteral("calendar-home-set"),
                [this, url, stale, done, abandon](const QUrl &home) {
                    if (stale()) {
                        abandon();
                        return;
                    }
                    listCalendars(home.isEmpty() ? url : home, stale, done);
                },
                failed);
        },
        [url, stale, done, failed, this](const Result &r) {
            // A 403 to the root is the server keeping it from this account (an ACL, a proxy
            // rule), not a refused login: the login name's calendars may still be readable.
            if (r.http == 403 && !stale()) {
                listCalendars(url, stale, done);
                return;
            }
            failed(r);
        });
}

void CalendarSync::findHref(const QUrl &url, const QByteArray &property, const QString &name, std::function<void(const QUrl &)> found,
                            std::function<void(const Result &)> failed)
{
    const QByteArray body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                            "<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:prop>"
        + property + "</d:prop></d:propfind>";
    request("PROPFIND", url, body, {{"Depth", "0"}, {"Content-Type", "application/xml; charset=utf-8"}}, true,
            [url, name, found, failed](const Result &r) {
                if (r.http == 207) {
                    found(hrefInProperty(r.body, name, url));
                } else if (r.http == 404 || r.http == 405 || r.http == 501) {
                    found(QUrl()); // not a place or a request this server knows
                } else {
                    failed(r); // refused, unreachable, broken: the fallback would fare no better
                }
            });
}

void CalendarSync::listCalendars(const QUrl &url, std::function<bool()> stale, std::function<void(bool)> done)
{
    const QByteArray body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\" xmlns:a=\"http://apple.com/ns/ical/\">"
        "<d:prop><d:resourcetype/><d:displayname/><c:supported-calendar-component-set/><a:calendar-color/></d:prop>"
        "</d:propfind>";
    request("PROPFIND", url, body, {{"Depth", "1"}, {"Content-Type", "application/xml; charset=utf-8"}}, true,
            [this, url, stale, done](const Result &r) {
                --m_busy;
                if (stale()) {
                    refreshIdleStatus();
                    done(false);
                    return;
                }
                if (r.http == 207) {
                    m_calendars = parseCalendars(r.body, url);
                    m_calendarsFresh = QDeadlineTimer(5 * 60 * 1000);
                    Q_EMIT calendarsChanged();
                    setLastError(QString());
                    m_paused = false;
                    m_forbiddenStreak = 0; // the login was taken: the 403s were the calendar's
                    refreshIdleStatus();
                    done(true);
                    return;
                }
                discoveryFailed(r);
                done(false);
            });
}

void CalendarSync::discoveryFailed(const Result &r)
{
    // Only a 401 is the login. A 403 to a PROPFIND is the server not letting this account list
    // that place (an ACL on the principal or the calendar home, an address of someone else's):
    // it says nothing about the calendar chosen, which keeps getting the events.
    if (r.http == 401) {
        m_paused = true;
        m_forbiddenStreak = 0;
    } else if (r.http == 403) {
        // Lifts the pause of an earlier 401 (this server took the login this time), not the one
        // of 403s in a row: a refused lookup says nothing more about those.
        m_paused = m_forbiddenStreak >= kForbiddenBeforePause;
    }
    const QString message = r.http == 403
        ? i18n("The server does not let this account list the calendars at this address (HTTP 403): check the address.")
        : describeFailure(r);
    setLastError(message);
    setStatus(Failed, message);
    if (r.http == 403) {
        trySend();
    }
}

void CalendarSync::refreshCalendars()
{
    rereadPassword();
    if (m_busy > 0) {
        return;
    }
    discover([this](bool ok) {
        if (!ok) {
            return;
        }
        if (m_calendars.isEmpty()) {
            setStatus(NotConfigured, i18n("No calendars were found at this address."));
            return;
        }
        for (const CalendarInfo &c : std::as_const(m_calendars)) {
            if (c.url == m_calendarUrl && m_calendarName != c.name) {
                m_calendarName = c.name;
                save(kCalendarName, c.name);
                Q_EMIT calendarNameChanged();
            }
        }
        trySend();
    });
}

void CalendarSync::refreshCalendarsIfStale()
{
    if (!m_calendars.isEmpty() && !m_calendarsFresh.hasExpired()) {
        return;
    }
    refreshCalendars();
}

void CalendarSync::autoSelectCalendar()
{
    const CalendarInfo *pick = nullptr;
    if (m_calendars.size() == 1) {
        pick = &m_calendars.first();
    } else {
        for (const CalendarInfo &c : std::as_const(m_calendars)) {
            if (c.name.compare(QLatin1String("personal"), Qt::CaseInsensitive) == 0
                || lastSegment(c.url).compare(QLatin1String("personal"), Qt::CaseInsensitive) == 0) {
                pick = &c;
                break;
            }
        }
    }
    if (pick) {
        const QString url = pick->url;
        const QString name = pick->name;
        selectCalendar(url, name);
    } else if (m_calendars.isEmpty()) {
        setStatus(NotConfigured, i18n("No calendars were found at this address."));
    } else {
        setStatus(NotConfigured, i18n("Signed in as %1. Choose a calendar to finish.", m_username));
    }
}

// ---------------------------------------------------------------------------------------------
// Nextcloud Login Flow v2
// ---------------------------------------------------------------------------------------------

void CalendarSync::startNextcloudLogin()
{
    cancelLogin();
    switchProvider(kNextcloud);
    if (m_serverUrl.isEmpty()) {
        setLastError(i18n("Enter the Nextcloud address first, for example %1", QStringLiteral("https://cloud.example.org")));
        refreshIdleStatus();
        return;
    }
    setLastError(QString());
    m_loginStarting = true;
    ++m_busy;
    setStatus(Working, i18n("Contacting the server…"));
    const quint64 loginGen = m_loginGeneration;
    const QUrl serverUrl(m_serverUrl);

    // Nextcloud names the new app password after this request's User-Agent, and shows that name in
    // Settings > Security > Devices & sessions: plain "Sugo", without a version number.
    request("POST", QUrl(m_serverUrl + QLatin1String("/index.php/login/v2")), QByteArray(), {{"User-Agent", "Sugo"}}, false,
            [this, loginGen, serverUrl](const Result &r) {
                if (loginGen != m_loginGeneration) {
                    return;
                }
                m_loginStarting = false;
                --m_busy;
                auto failWith = [this](const QString &message) {
                    setLastError(message);
                    setStatus(Failed, message);
                };
                if (r.http != 200) {
                    failWith(r.http == 404 ? i18n("This does not look like a Nextcloud server.") : describeFailure(r));
                    return;
                }
                const QJsonObject root = QJsonDocument::fromJson(r.body).object();
                const QJsonObject poll = root.value(QLatin1String("poll")).toObject();
                const QString token = poll.value(QLatin1String("token")).toString();
                const QUrl endpoint(poll.value(QLatin1String("endpoint")).toString());
                const QUrl login(root.value(QLatin1String("login")).toString());
                if (token.isEmpty() || !endpoint.isValid() || !login.isValid()) {
                    failWith(i18n("The server gave an unexpected answer to the login request."));
                    return;
                }
                QString error;
                if (!isUrlAllowed(login, &error) || !isUrlAllowed(endpoint, &error)) {
                    failWith(error);
                    return;
                }
                // Both the page the user signs in on and the poll endpoint must be on the server
                // that was entered: a misconfigured or hostile server cannot send them elsewhere.
                if (endpoint.host().compare(serverUrl.host(), Qt::CaseInsensitive) != 0
                    || login.host().compare(serverUrl.host(), Qt::CaseInsensitive) != 0) {
                    failWith(i18n("The server redirected the login to a different host; it was refused."));
                    return;
                }
                m_pollEndpoint = endpoint;
                m_pollToken = token.toUtf8();
                m_loginActive = true;
                m_pollInFlight = false;
                m_loginDeadline = QDeadlineTimer(kLoginTimeoutMs);
                m_pollTimer.start(m_pollIntervalMs);
                setStatus(WaitingForBrowser, i18n("Waiting for you to authorize Sugo in the browser…"));
                if (m_openUrl) {
                    m_openUrl(login);
                }
            });
}

void CalendarSync::endLogin()
{
    m_pollTimer.stop();
    m_loginActive = false;
    m_pollInFlight = false;
    m_pollToken.clear();
}

void CalendarSync::cancelLogin()
{
    const bool wasActive = m_loginActive || m_loginStarting;
    ++m_loginGeneration;
    if (m_loginStarting) {
        m_loginStarting = false;
        --m_busy;
    }
    endLogin();
    if (wasActive) {
        refreshIdleStatus();
    }
}

void CalendarSync::pollLogin()
{
    if (!m_loginActive) {
        return;
    }
    if (m_loginDeadline.hasExpired()) {
        endLogin();
        setLastError(i18n("The login was not completed in time. Please try again."));
        refreshIdleStatus();
        return;
    }
    if (m_pollInFlight) {
        return;
    }
    m_pollInFlight = true;
    const quint64 loginGen = m_loginGeneration;
    const QByteArray body = "token=" + QUrl::toPercentEncoding(QString::fromUtf8(m_pollToken));
    request("POST", m_pollEndpoint, body, {{"Content-Type", "application/x-www-form-urlencoded"}, {"User-Agent", "Sugo"}}, false,
            [this, loginGen](const Result &r) {
                if (loginGen != m_loginGeneration || !m_loginActive) {
                    return;
                }
                m_pollInFlight = false;
                if (r.http == 200) {
                    finishLogin(r.body);
                } else if (r.http >= 400 && r.http < 500 && r.http != 404 && r.http != 408 && r.http != 429) {
                    endLogin();
                    const QString message = describeFailure(r);
                    setLastError(message);
                    setStatus(Failed, message);
                }
                // 404 means "not yet"; network errors and 5xx are retried until the deadline.
            });
}

void CalendarSync::finishLogin(const QByteArray &json)
{
    endLogin();
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QUrl server(root.value(QLatin1String("server")).toString());
    const QString loginName = root.value(QLatin1String("loginName")).toString().trimmed();
    const QString appPassword = root.value(QLatin1String("appPassword")).toString();
    const QUrl entered(m_serverUrl);

    QString error;
    if (!server.isValid() || !isUrlAllowed(server, &error) || server.host().compare(entered.host(), Qt::CaseInsensitive) != 0) {
        const QString message = i18n("The server returned an address that does not match the one you entered; the login was refused.");
        setLastError(message);
        setStatus(Failed, message);
        return;
    }
    if (loginName.isEmpty() || appPassword.isEmpty()) {
        const QString message = i18n("The server gave an unexpected answer to the login request.");
        setLastError(message);
        setStatus(Failed, message);
        return;
    }

    // A new account: what is still on its way for the old one (a 401 would pause the new
    // account) is abandoned, and the old calendar choice forgotten.
    abortAll();
    const bool hadCalendar = !m_calendarUrl.isEmpty();
    m_calendarUrl.clear();
    m_calendarName.clear();
    save(kCalendarUrl, QString());
    save(kCalendarName, QString());
    if (hadCalendar) {
        Q_EMIT calendarUrlChanged();
        Q_EMIT calendarNameChanged();
    }
    const bool hadPassword = hasPassword();
    m_username = loginName;
    save(kUsername, m_username);
    storePassword(appPassword);
    Q_EMIT usernameChanged();
    if (!hadPassword) {
        Q_EMIT hasPasswordChanged();
    }
    m_paused = false;
    m_forbiddenStreak = 0;
    m_sendFailing = false;

    discover([this](bool ok) {
        if (ok) {
            autoSelectCalendar();
        }
    });
}
