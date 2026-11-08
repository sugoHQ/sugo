// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Settings.h"
#include "TimerEngine.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * Sends one batch of events. `done` gets the HTTP status, or 0 when the server could not be
 * reached; it is not called after abort().
 */
class TelemetryTransport : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void send(const QByteArray &json, std::function<void(int status)> done) = 0;
    virtual void abort() = 0;
};

/// POST to Sugo's server (`<base>/v1/telemetry`), which forwards the events to PostHog.
class HttpTelemetryTransport : public TelemetryTransport
{
    Q_OBJECT
public:
    HttpTelemetryTransport(const QUrl &endpoint, QObject *parent = nullptr);
    ~HttpTelemetryTransport() override;
    void send(const QByteArray &json, std::function<void(int status)> done) override;
    void abort() override;

private:
    QUrl m_endpoint;
    /// Bumped by abort(): a refusal scheduled before is not reported.
    int m_serial = 0;
    QNetworkAccessManager *m_nam = nullptr;
    QPointer<QNetworkReply> m_reply;
};

/**
 * Anonymous usage statistics, opt-in (AppSettings::shareUsageStats, off by default).
 *
 * While off, nothing is recorded, queued or sent, and there is no install id. Turned on, it makes a
 * random id (Telemetry/Id in the settings file), records `app_started` (once per run, and when turned
 * on, at the first send so that what it reports has settled) and `session_ended` for every phase that
 * ends, and sends them in batches of at most 50 a
 * minute after starting and then every 15 minutes. Times are cut to the hour. The queue (at most
 * 200 events, the oldest dropped first) is kept in the settings file, so sessions done offline are
 * sent later; a failed send is retried after 1, 10, then 60 minutes. Turning it off forgets the id
 * and the queue and cancels a send in progress.
 *
 * The list of events and their properties is in docs/USAGE.md and must stay what the Worker accepts
 * (sugo-sync, worker/src/telemetry.ts).
 */
class Telemetry : public QObject
{
    Q_OBJECT
public:
    static constexpr int maxQueued = 200;
    static constexpr int maxBatch = 50;

    Telemetry(Settings::Ptr settings, TelemetryTransport *transport, QObject *parent = nullptr);

    /// The properties of `app_started` (TelemetryInfo and the features in use), asked when it is recorded.
    void setStartProperties(std::function<QJsonObject()> properties);
    /// The wall clock (tests).
    void setNow(std::function<QDateTime()> now);
    /// First send after `firstMs`, then every `intervalMs`; retries after `retryMs` (tests).
    void setTimingForTesting(int firstMs, int intervalMs, const QList<int> &retryMs);

    bool isEnabled() const { return m_enabled; }
    /// Turns sending on or off (from AppSettings::shareUsageStats).
    void setEnabled(bool enabled);

    QString installId() const { return m_id; }
    QJsonArray queue() const { return m_queue; }
    bool isSending() const { return m_sending; }

    /// A phase ended (TimerEngine::sessionEnded).
    void recordSession(TimerEngine::Phase phase, qint64 plannedMs, bool completed);
    /// Sends the next batch now, if there is one and none is on its way.
    void flush();

private:
    void record(const QString &event, const QJsonObject &properties);
    void saveQueue();
    void schedule(int ms);
    void finished(int generation, int count, int status);
    QString hourNow() const;

    Settings::Ptr m_settings;
    TelemetryTransport *m_transport;
    std::function<QJsonObject()> m_startProperties;
    std::function<QDateTime()> m_now;
    QTimer m_timer;
    int m_firstMs = 60 * 1000;
    int m_intervalMs = 15 * 60 * 1000;
    QList<int> m_retryMs = {60 * 1000, 10 * 60 * 1000, 60 * 60 * 1000};
    int m_failures = 0;

    bool m_enabled = false;
    bool m_startedRecorded = false;
    bool m_sending = false;
    /// Events of the batch on its way that the queue's cap pushed out meanwhile.
    int m_droppedWhileSending = 0;
    /// Bumped when turned off: the answer to a batch sent before is ignored.
    int m_generation = 0;
    QString m_id;
    QJsonArray m_queue;
};
