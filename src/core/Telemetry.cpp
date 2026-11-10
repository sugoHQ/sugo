// SPDX-License-Identifier: GPL-3.0-or-later
#include "Telemetry.h"

#include <utility>

#include "TelemetryInfo.h"
#include "UrlPolicy.h"

#include <QJsonDocument>
#include <QStringList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUuid>

#include <algorithm>

namespace
{
const char kId[] = "Telemetry/Id";
const char kQueue[] = "Telemetry/Queue";
/// sync_error: "area/kind" → the day (yyyy-MM-dd, UTC) it was last recorded, kept across restarts.
const char kSyncErrorDays[] = "Telemetry/SyncErrorDays";
constexpr int kTransferTimeoutMs = 15 * 1000;

QString phaseName(TimerEngine::Phase phase)
{
    switch (phase) {
    case TimerEngine::Phase::Work:
        return QStringLiteral("work");
    case TimerEngine::Phase::ShortBreak:
        return QStringLiteral("short_break");
    case TimerEngine::Phase::LongBreak:
        return QStringLiteral("long_break");
    }
    return QStringLiteral("work");
}

bool isInstallId(const QString &id)
{
    const QUuid uuid(id);
    return !uuid.isNull() && uuid.version() == QUuid::Random && uuid.toString(QUuid::WithoutBraces) == id;
}
} // namespace

// --- HttpTelemetryTransport --------------------------------------------------------------------

HttpTelemetryTransport::HttpTelemetryTransport(const QUrl &endpoint, QObject *parent)
    : TelemetryTransport(parent)
    , m_endpoint(endpoint)
{
}

HttpTelemetryTransport::~HttpTelemetryTransport()
{
    abort();
}

void HttpTelemetryTransport::send(const QByteArray &json, std::function<void(int status)> done)
{
    abort();
    if (!m_endpoint.isValid() || !UrlPolicy::isAllowed(m_endpoint)) {
        const int serial = m_serial;
        QTimer::singleShot(0, this, [this, serial, done]() {
            if (serial == m_serial) {
                done(0);
            }
        });
        return;
    }
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    QNetworkRequest request(m_endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QString(QStringLiteral("Sugo/") + TelemetryInfo::version()));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setTransferTimeout(kTransferTimeoutMs);
    QNetworkReply *reply = m_nam->post(request, json);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, done]() {
        reply->deleteLater();
        if (m_reply != reply) {
            return; // aborted
        }
        m_reply = nullptr;
        done(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
    });
}

void HttpTelemetryTransport::abort()
{
    ++m_serial;
    if (QNetworkReply *reply = m_reply) {
        m_reply = nullptr;
        reply->abort();
    }
}

// --- Telemetry ---------------------------------------------------------------------------------

Telemetry::Telemetry(Settings::Ptr settings, TelemetryTransport *transport, QObject *parent)
    : QObject(parent)
    , m_settings(std::move(settings))
    , m_transport(transport)
    , m_now([]() { return QDateTime::currentDateTimeUtc(); })
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &Telemetry::flush);
    // Left by an earlier run that was on; dropped below if it is off now (setEnabled(false)).
    m_id = m_settings->value(QLatin1String(kId)).toString();
    const QJsonDocument stored = QJsonDocument::fromJson(m_settings->value(QLatin1String(kQueue)).toString().toUtf8());
    for (const QJsonValue &event : stored.array()) {
        if (event.isObject()) {
            m_queue.append(event);
        }
    }
    const QJsonObject days = QJsonDocument::fromJson(m_settings->value(QLatin1String(kSyncErrorDays)).toString().toUtf8()).object();
    for (auto it = days.begin(); it != days.end(); ++it) {
        const QDate day = QDate::fromString(it.value().toString(), Qt::ISODate);
        if (day.isValid()) {
            m_syncErrorsSent.insert(it.key(), day);
        }
    }
}

void Telemetry::setStartProperties(std::function<QJsonObject()> properties)
{
    m_startProperties = std::move(properties);
}

void Telemetry::setNow(std::function<QDateTime()> now)
{
    m_now = std::move(now);
}

void Telemetry::setTimingForTesting(int firstMs, int intervalMs, const QList<int> &retryMs)
{
    m_firstMs = firstMs;
    m_intervalMs = intervalMs;
    m_retryMs = retryMs;
}

void Telemetry::setEnabled(bool enabled, const QString &source)
{
    if (!enabled) {
        const bool hadState = m_enabled || !m_id.isEmpty() || !m_queue.isEmpty()
            || m_settings->contains(QLatin1String(kId)) || m_settings->contains(QLatin1String(kQueue))
            || m_settings->contains(QLatin1String(kSyncErrorDays));
        m_enabled = false;
        m_startedRecorded = false;
        m_timer.stop();
        if (m_sending) {
            m_transport->abort();
            m_sending = false;
        }
        ++m_generation;
        m_failures = 0;
        m_id.clear();
        m_queue = QJsonArray();
        m_syncErrorsSent.clear();
        if (hadState) {
            m_settings->remove(QLatin1String("Telemetry"));
            Settings::sync(*m_settings);
        }
        return;
    }
    if (m_enabled) {
        return;
    }
    m_enabled = true;
    if (!isInstallId(m_id)) {
        m_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_settings->setValue(QLatin1String(kId), m_id);
        Settings::sync(*m_settings);
    }
    if (source == QLatin1String("welcome") || source == QLatin1String("settings")) {
        record(QStringLiteral("stats_enabled"), {{QStringLiteral("source"), source}});
    }
    schedule(m_firstMs);
}

void Telemetry::recordSession(TimerEngine::Phase phase, qint64 plannedMs, bool completed)
{
    if (std::exchange(m_skipNextSession, false) || !m_enabled) {
        m_lastSessionRecorded = false;
        return;
    }
    m_lastSessionRecorded = true;
    const qint64 minutes = std::clamp<qint64>((plannedMs + 30 * 1000) / (60 * 1000), 1, 240);
    record(QStringLiteral("session_ended"),
           {{QStringLiteral("phase"), phaseName(phase)},
            {QStringLiteral("planned_min"), static_cast<int>(minutes)},
            {QStringLiteral("completed"), completed}});
}

void Telemetry::recordExtend(TimerEngine::Phase phase, bool afterEnd, TimerEngine::ExtendSource source, int seconds)
{
    if (!m_enabled) {
        return;
    }
    QString from;
    switch (source) {
    case TimerEngine::ExtendSource::App:
        from = QStringLiteral("app");
        break;
    case TimerEngine::ExtendSource::Notification:
        from = QStringLiteral("notification");
        break;
    case TimerEngine::ExtendSource::Menu:
        from = QStringLiteral("menu");
        break;
    case TimerEngine::ExtendSource::Command:
        from = QStringLiteral("command");
        break;
    }
    record(QStringLiteral("timer_extended"),
           {{QStringLiteral("phase"), phaseName(phase)},
            {QStringLiteral("moment"), afterEnd ? QStringLiteral("ended") : QStringLiteral("running")},
            {QStringLiteral("source"), from},
            {QStringLiteral("minutes"), std::clamp((seconds + 30) / 60, 1, 60)}});
}

void Telemetry::tourEnded(int step, bool skipped)
{
    if (!m_enabled) {
        return;
    }
    record(QStringLiteral("onboarding_finished"),
           {{QStringLiteral("result"), skipped ? QStringLiteral("skipped") : QStringLiteral("done")},
            {QStringLiteral("step"), std::clamp(step, 0, 9)}});
}

void Telemetry::recordUpdateInstall(bool ok, const QString &stage)
{
    static const QStringList stages = {QStringLiteral("write"), QStringLiteral("download"), QStringLiteral("verify"), QStringLiteral("install")};
    if (!m_enabled || !stages.contains(stage)) {
        return;
    }
    record(QStringLiteral("update_install"), {{QStringLiteral("ok"), ok}, {QStringLiteral("stage"), stage}});
}

void Telemetry::recordSyncError(const QString &area, const QString &kind)
{
    if (!m_enabled || kind.isEmpty()) {
        return;
    }
    const QString key = area + QLatin1Char('/') + kind;
    const QDate today = m_now().toUTC().date();
    if (m_syncErrorsSent.value(key) == today) {
        return;
    }
    m_syncErrorsSent.insert(key, today);
    QJsonObject days;
    for (auto it = m_syncErrorsSent.cbegin(); it != m_syncErrorsSent.cend(); ++it) {
        if (it.value() == today) { // older days no longer matter
            days.insert(it.key(), it.value().toString(Qt::ISODate));
        }
    }
    m_settings->setValue(QLatin1String(kSyncErrorDays), QString::fromUtf8(QJsonDocument(days).toJson(QJsonDocument::Compact)));
    record(QStringLiteral("sync_error"), {{QStringLiteral("area"), area}, {QStringLiteral("kind"), kind}});
}

QString Telemetry::hourNow() const
{
    QDateTime t = m_now().toUTC();
    t.setTime(QTime(t.time().hour(), 0));
    return t.toString(Qt::ISODate);
}

void Telemetry::record(const QString &event, const QJsonObject &properties)
{
    m_queue.append(QJsonObject{{QStringLiteral("event"), event}, {QStringLiteral("ts"), hourNow()}, {QStringLiteral("props"), properties}});
    while (m_queue.size() > maxQueued) {
        m_queue.removeFirst();
        if (m_sending) {
            ++m_droppedWhileSending;
        }
    }
    saveQueue();
}

void Telemetry::saveQueue()
{
    if (m_queue.isEmpty()) {
        m_settings->remove(QLatin1String(kQueue));
    } else {
        m_settings->setValue(QLatin1String(kQueue), QString::fromUtf8(QJsonDocument(m_queue).toJson(QJsonDocument::Compact)));
    }
    Settings::sync(*m_settings);
}

void Telemetry::schedule(int ms)
{
    if (m_enabled) {
        m_timer.start(std::max(0, ms));
    }
}

void Telemetry::flush()
{
    if (!m_enabled || m_sending) {
        return;
    }
    // At the first send rather than at startup, when the tray and Sugo Sync have settled.
    if (!m_startedRecorded) {
        m_startedRecorded = true;
        record(QStringLiteral("app_started"), m_startProperties ? m_startProperties() : QJsonObject());
    }
    if (m_queue.isEmpty()) {
        schedule(m_intervalMs);
        return;
    }
    m_timer.stop();
    const int count = std::min<int>(int(m_queue.size()), maxBatch);
    QJsonArray events;
    for (int i = 0; i < count; ++i) {
        events.append(m_queue.at(i));
    }
    const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("id"), m_id}, {QStringLiteral("events"), events}}).toJson(QJsonDocument::Compact);
    m_sending = true;
    m_droppedWhileSending = 0;
    const int generation = m_generation;
    m_transport->send(body, [this, generation, count](int status) { finished(generation, count, status); });
}

void Telemetry::finished(int generation, int count, int status)
{
    if (generation != m_generation) {
        return; // turned off meanwhile
    }
    m_sending = false;
    // A redirect elsewhere (not followed: SameOriginRedirectPolicy) is not a refusal: keep the events.
    const bool retry = status == 0 || (status >= 300 && status < 400) || status == 408 || status == 429 || status >= 500;
    if (retry) {
        const int delay = m_retryMs.isEmpty() ? m_intervalMs : m_retryMs.at(std::min<int>(m_failures, int(m_retryMs.size()) - 1));
        ++m_failures;
        schedule(delay);
        return;
    }
    // Delivered (2xx), or refused for good (another 4xx): either way these events are done.
    m_failures = 0;
    for (int i = m_droppedWhileSending; i < count && !m_queue.isEmpty(); ++i) {
        m_queue.removeFirst();
    }
    saveQueue();
    schedule(m_queue.isEmpty() ? m_intervalMs : 0);
}
