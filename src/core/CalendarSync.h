// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Settings.h"

#include <QByteArray>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>

#include "SessionRepository.h"

class QNetworkAccessManager;
class SecretStore;
class QNetworkReply;

/**
 * Optional calendar sync: every completed Work phase becomes one event on the user's CalDAV
 * calendar (Nextcloud through Login Flow v2, or any CalDAV server with a password).
 *
 * Nothing happens on the network unless the feature is enabled and the user triggers a login,
 * a test, a calendar search, or a work phase completes. Credentials only travel over https
 * (loopback http is allowed so tests and local servers work); TLS errors are never ignored.
 * The app password lives in the desktop keyring (Secret Service) when one answers, and only
 * otherwise in the settings file (group "Calendar", a file private to the user); it is never exposed
 * back, logged or shown in any text.
 *
 * Events waiting to be sent are kept in the settings file too, so they survive a restart. Each one
 * belongs to the account (provider, user name, calendar server) that was set up when its work
 * phase ended, and is only ever sent to that account.
 */
class CalendarSync : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(QString provider READ provider WRITE setProvider NOTIFY providerChanged)
    Q_PROPERTY(QString serverUrl READ serverUrl WRITE setServerUrl NOTIFY serverUrlChanged)
    Q_PROPERTY(QString username READ username WRITE setUsername NOTIFY usernameChanged)
    Q_PROPERTY(bool hasPassword READ hasPassword NOTIFY hasPasswordChanged)
    Q_PROPERTY(QString calendarUrl READ calendarUrl WRITE setCalendarUrl NOTIFY calendarUrlChanged)
    Q_PROPERTY(QString calendarName READ calendarName NOTIFY calendarNameChanged)
    Q_PROPERTY(bool includeNote READ includeNote WRITE setIncludeNote NOTIFY includeNoteChanged)
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QDateTime lastSuccess READ lastSuccess NOTIFY lastSuccessChanged)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY pendingCountChanged)
    Q_PROPERTY(QVariantList calendars READ calendars NOTIFY calendarsChanged)

public:
    enum Status {
        NotConfigured,
        WaitingForBrowser,
        Working,
        Ready,
        Failed,
    };
    Q_ENUM(Status)

    /// A calendar collection found by discovery.
    struct CalendarInfo {
        QString name;
        QString url;
        QString color;
    };

    explicit CalendarSync(QObject *parent = nullptr);
    /// `config` is where the settings (group "Calendar") are kept; tests pass a temporary file.
    explicit CalendarSync(Settings::Ptr config, QObject *parent = nullptr);
    /// `secrets` keeps the password; null keeps it in `config`.
    CalendarSync(Settings::Ptr config, std::unique_ptr<SecretStore> secrets, QObject *parent = nullptr);
    ~CalendarSync() override;

    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    QString provider() const { return m_provider; }
    void setProvider(const QString &provider);
    QString serverUrl() const { return m_serverUrl; }
    void setServerUrl(const QString &url);
    QString username() const { return m_username; }
    void setUsername(const QString &username);
    bool hasPassword() const { return !m_password.isEmpty(); }
    QString calendarUrl() const { return m_calendarUrl; }
    void setCalendarUrl(const QString &url);
    QString calendarName() const;
    bool includeNote() const { return m_includeNote; }
    void setIncludeNote(bool include);
    Status status() const { return m_status; }
    QString statusText() const { return m_statusText; }
    QString lastError() const { return m_lastError; }
    QDateTime lastSuccess() const { return m_lastSuccess; }
    int pendingCount() const { return int(m_queue.size()); }
    QVariantList calendars() const;

    Q_INVOKABLE void startNextcloudLogin();
    Q_INVOKABLE void cancelLogin();
    Q_INVOKABLE void setPassword(const QString &password);
    Q_INVOKABLE void refreshCalendars();
    /// refreshCalendars(), unless the list was fetched less than five minutes ago.
    Q_INVOKABLE void refreshCalendarsIfStale();
    Q_INVOKABLE void selectCalendar(const QString &url, const QString &name);
    Q_INVOKABLE void testConnection();
    Q_INVOKABLE void disconnect();

    // --- Pure helpers (public so they can be unit-tested) ---

    /// Stable id of the event of a session.
    static QString uidFor(const SessionRecord &record);
    /// RFC 5545 text escaping (backslash, semicolon, comma, newline).
    static QString escapeText(const QString &text);
    /// Folds a content line at 75 octets with CRLF + space, never inside a UTF-8 sequence.
    /// The result ends with CRLF.
    static QByteArray foldLine(const QString &line);
    /// The complete VCALENDAR document (CRLF line ends).
    static QByteArray buildIcs(const SessionRecord &record, bool includeNote, const QString &uid,
                               const QDateTime &stamp = QDateTime::currentDateTimeUtc());
    /// Trims and validates a user-entered address; strips trailing slashes. Empty input gives an
    /// empty string and no error. On failure returns an empty string and sets `error`.
    static QString normalizeUrl(const QString &input, QString *error);
    /// https, or http to a loopback host only.
    static bool isUrlAllowed(const QUrl &url, QString *error = nullptr);
    /// Parses a WebDAV multistatus into calendar collections (VEVENT capable). Relative hrefs
    /// are resolved against `requestUrl`; other hosts are ignored.
    static QList<CalendarInfo> parseCalendars(const QByteArray &xml, const QUrl &requestUrl);

    // --- Test hooks ---
    void setRetryDelaysForTesting(const QList<int> &delaysMs);
    void setLoginPollIntervalForTesting(int ms) { m_pollIntervalMs = ms; }
    /// The delay before the next try of a failed send, in ms (0: none waiting).
    int retryIntervalForTesting() const { return m_retryTimer.isActive() ? m_retryTimer.interval() : 0; }
    void setOpenUrlHandler(std::function<void(const QUrl &)> handler) { m_openUrl = std::move(handler); }
    bool isSendingPaused() const { return m_paused; }
    bool isPasswordInKeyring() const { return m_passwordInKeyring; }

public Q_SLOTS:
    void enqueueWorkSession(const SessionRecord &record);
    void onNoteChanged(qint64 sessionId, const QString &note);
    /// A session was deleted: its event is not sent any more, and with `fromCalendar` the event
    /// already sent is deleted from the calendar (tried again after the next event that goes
    /// through, if the server cannot be reached now).
    void onSessionDeleted(const SessionRecord &record, bool fromCalendar);

Q_SIGNALS:
    void enabledChanged();
    void providerChanged();
    void serverUrlChanged();
    void usernameChanged();
    void hasPasswordChanged();
    void calendarUrlChanged();
    void calendarNameChanged();
    void includeNoteChanged();
    void statusChanged();
    void lastErrorChanged();
    void lastSuccessChanged();
    void pendingCountChanged();
    void calendarsChanged();
    /// An address typed in (setServerUrl, setCalendarUrl) was refused and nothing was stored:
    /// `message` says why, so the view can keep what was typed and explain it.
    void inputRejected(const QString &message);
    /// Sending a session's event failed, by kind: "network" (no answer), "auth" (401), "refused"
    /// (403), "not_found" (404, 409), "rate_limited" (408, 423, 429), "rejected_event" (another
    /// 4xx: the event was skipped) or "server". For the anonymous usage statistics.
    void sendFailed(const QString &kind);

private:
    struct Result {
        int http = 0;
        QByteArray body;
        QString networkError; ///< Empty when the server answered.
        int retryAfter = 0;   ///< Retry-After of a failure, in seconds (RetryAfter::seconds).
    };
    struct Pending {
        SessionRecord record;
        QString uid;
        QString account; ///< currentAccount() when it was queued.
        QString url;     ///< Where it was put (sent events only): a later calendar is another place.
    };
    /// An event Sugo put on the server that must go: a connection test's, or a deleted session's.
    struct LeftoverTest {
        QString uid;
        QString account;
        QString url; ///< Where it was put; if missing, eventUrl(uid).
    };

    void load();
    void movePasswordsToKeyring();
    void loadProviderFields();
    void switchProvider(const QString &provider);
    void save(const char *key, const QVariant &value);
    QString secretKey() const;
    void storePassword(const QString &password);
    /// Asks the keyring again for a password it could not give at startup.
    void rereadPassword();
    QString currentAccount() const;
    /// The account of an event queued before the account was complete (see dropOtherAccounts()).
    QString unboundAccount() const;
    void loadQueue();
    void saveQueue();
    /// Persists the queue and announces the new count.
    void queueChanged();
    void appendBounded(const Pending &item);
    void dropOtherAccounts();
    int inFlightIndex() const;
    /// True if both give the same event (the note counts only when it is sent).
    bool sameEvent(const SessionRecord &a, const SessionRecord &b) const;
    /// For Nextcloud, the calendar must be on the server that was signed in to.
    bool calendarOnServer(const QString &calendarUrl, QString *error) const;
    void removeLeftoverTestEvents();
    bool configured() const;
    void credentialsChanged();
    void refreshIdleStatus();
    void setStatus(Status status, const QString &text);
    void setLastError(const QString &error);
    void rejectInput(const QString &error);
    QString scrub(const QString &text) const;
    QString describeFailure(const Result &result) const;

    QNetworkAccessManager *network();
    void request(const QByteArray &verb, const QUrl &url, const QByteArray &body, const QList<QPair<QByteArray, QByteArray>> &headers,
                 bool withAuth, std::function<void(const Result &)> done);
    QUrl eventUrl(const QString &uid) const;
    /// Where discovery looks for calendars (for Nextcloud, if the server does not say).
    QUrl discoveryUrl() const;

    void trySend();
    /// trySend() from the event loop, once however often it is asked for meanwhile.
    void trySendLater();
    /// Starts the backoff's next delay, or the server's Retry-After (in seconds) if longer.
    void scheduleRetry(int retryAfterSeconds = 0);
    void onSendFinished(const Result &result, bool credentialsChangedMeanwhile);
    void rememberSent(const Pending &item);
    void clearQueue();
    void abortAll();

    void discover(std::function<void(bool)> done);
    /// PROPFIND (depth 0) of one property whose value is an href, e.g. the user's principal.
    /// `found` gets the address, or an empty one if the server does not give it (no such
    /// property, 404, 405, 501); `failed` gets any other answer (a refused login, no connection).
    void findHref(const QUrl &url, const QByteArray &property, const QString &name, std::function<void(const QUrl &)> found,
                  std::function<void(const Result &)> failed);
    /// The calendars in the collection `url`; `stale` says the account changed meanwhile.
    void listCalendars(const QUrl &url, std::function<bool()> stale, std::function<void(bool)> done);
    /// Reports a failed discovery request; a refused login pauses sending.
    void discoveryFailed(const Result &r);
    void autoSelectCalendar();

    void pollLogin();
    void finishLogin(const QByteArray &json);
    void endLogin();

    Settings::Ptr m_config;
    std::unique_ptr<SecretStore> m_secrets;
    bool m_passwordInKeyring = false;
    bool m_passwordUnread = false; ///< The keyring did not answer when the password was loaded.
    QNetworkAccessManager *m_nam = nullptr;
    QSet<QNetworkReply *> m_replies;
    quint64 m_generation = 0; ///< Bumped to ignore answers to requests made before disconnect()/cancelLogin().

    bool m_enabled = false;
    QString m_provider;
    QString m_serverUrl;
    QString m_username;
    QString m_password;
    QString m_calendarUrl;
    QString m_calendarName;
    bool m_includeNote = false;

    Status m_status = NotConfigured;
    QString m_statusText;
    QString m_lastError;
    QDateTime m_lastSuccess;
    QList<CalendarInfo> m_calendars;
    QDeadlineTimer m_calendarsFresh; ///< Running while the fetched list counts as recent.
    int m_busy = 0;

    QList<Pending> m_queue;
    QList<Pending> m_sent; ///< The last events sent, oldest first, bounded.
    bool m_inFlight = false;
    bool m_sendLater = false; ///< A trySendLater() call is waiting for the event loop.
    QString m_inFlightUid;
    /// Connection-test events the server did not let us remove yet.
    QList<LeftoverTest> m_leftoverTests;
    SessionRecord m_inFlightRecord; ///< What the request on its way carries.
    QString m_inFlightUrl;          ///< Where the request on its way puts it.
    bool m_paused = false;
    /// 403s in a row to the events sent and the test's, with nothing accepted between.
    int m_forbiddenStreak = 0;
    bool m_sendFailing = false; ///< The last attempt failed for a reason that may pass.
    bool m_loginStarting = false;
    QTimer m_retryTimer;
    QList<int> m_retryDelays;
    int m_retryStep = 0;

    QTimer m_pollTimer;
    int m_pollIntervalMs = 2000;
    bool m_loginActive = false;
    bool m_pollInFlight = false;
    QUrl m_pollEndpoint;
    QByteArray m_pollToken;
    QDeadlineTimer m_loginDeadline;
    quint64 m_loginGeneration = 0;

    std::function<void(const QUrl &)> m_openUrl;
};
