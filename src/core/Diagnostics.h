// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QtQml/qqmlregistration.h>

class AppSettings;
class DataManager;
class SyncController;

/**
 * Provides system diagnostics and redacted log export for issue reporting.
 *
 * Automatically redacts usernames, personal paths, and sensitive data so
 * users can safely attach system details and recent application logs
 * to bug reports.
 */
class Diagnostics : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit Diagnostics(QObject *parent = nullptr);
    ~Diagnostics() override;

    void attach(AppSettings *settings, DataManager *dataManager);
    /// Adds the "Sync" section: state, error, keyring, linked, last sync, number of devices and
    /// whether the server is the default one. Never an id, a name, a code, a token or an etag.
    void attachSync(SyncController *sync);

    /// Formats the complete markdown diagnostics report (system + config + logs).
    Q_INVOKABLE QString generateReport() const;

    /// Copies the full markdown report to the system clipboard.
    Q_INVOKABLE void copyToClipboard() const;

    /// The smoke run "inventory" (FeatureInventory.qml): writes `json` to the file named by
    /// $SUGO_INVENTORY. False when that variable is not set or the file cannot be written.
    Q_INVOKABLE bool saveInventory(const QString &json) const;

    /// Returns the concatenated recent logs (already redacted).
    QString recentLogs() const;

    /// Redacts personal paths (the home directory, /home/<name>), the user name and the
    /// user part of user@host from text.
    static QString redact(const QString &text);
    /// Words that identify the user's accounts (the calendar login and server), redacted from
    /// every report and log line from now on; replaces the previous list.
    static void setAccountRedactions(const QStringList &words);

    /// Installs Qt message handler to capture application log messages into a ring buffer.
    static void installLogHandler();

    /// Appends a log line to the internal buffer (for testing and handler).
    static void addLogEntry(const QString &level, const QString &category, const QString &message);
    /// Whether messages of a logging category go into the report. Not those of KDE Frameworks
    /// (kf.*): they are not about Sugo and can contain the names of the user's files.
    static bool isRecordedCategory(const QString &category);

    /// Clears the log buffer (primarily for testing).
    static void clearLogs();

    /// Verbose logging: Sugo's debug messages are let through (sugo.* categories) and the log
    /// buffer holds more lines. Off keeps them out, whatever the distribution's default. Either
    /// way KIO's file dialog model is kept from flooding the terminal (kf.kio.widgets.kdirmodel
    /// warnings). QT_LOGGING_RULES from the environment wins over all of this. Follows
    /// AppSettings::verboseLogging once attached.
    static void setVerboseLogging(bool verbose);
    static bool verboseLogging();
    /// How many log lines the buffer keeps now.
    static int logCapacity();

private:
    AppSettings *m_settings = nullptr;
    DataManager *m_dataManager = nullptr;
    QPointer<SyncController> m_sync;
    QMetaObject::Connection m_verboseConnection;
};
