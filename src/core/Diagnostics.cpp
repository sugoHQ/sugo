// SPDX-License-Identifier: GPL-3.0-or-later
#include "Diagnostics.h"

#include "AppSettings.h"
#include "DataManager.h"
#include "SyncController.h"

#include "Localization.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSysInfo>
#include <QTime>

#include <QLoggingCategory>
#include <QMetaEnum>

#include <algorithm>
#include <atomic>
#include <deque>

namespace
{
// Allocated once and never destroyed: the message handler stays installed until the process ends,
// and a warning printed during static destruction must still find a live buffer and mutex.
QMutex &logMutex()
{
    static QMutex *mutex = new QMutex;
    return *mutex;
}
std::deque<QString> &logBuffer()
{
    static auto *buffer = new std::deque<QString>;
    return *buffer;
}
constexpr size_t kMaxLogEntries = 120;
// Reproducing a problem with verbose logging on produces many more lines.
constexpr size_t kMaxVerboseLogEntries = 500;
std::atomic<bool> s_verbose{false};

size_t maxLogEntries()
{
    return s_verbose.load() ? kMaxVerboseLogEntries : kMaxLogEntries;
}
QtMessageHandler s_originalHandler = nullptr;

void diagnosticsMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    QString level;
    switch (type) {
    case QtDebugMsg:    level = QStringLiteral("DEBUG"); break;
    case QtInfoMsg:     level = QStringLiteral("INFO");  break;
    case QtWarningMsg:  level = QStringLiteral("WARN");  break;
    case QtCriticalMsg: level = QStringLiteral("CRIT");  break;
    case QtFatalMsg:    level = QStringLiteral("FATAL"); break;
    }

    const QString category = context.category ? QString::fromUtf8(context.category) : QStringLiteral("default");
    Diagnostics::addLogEntry(level, category, msg);

    if (s_originalHandler) {
        s_originalHandler(type, context, msg);
    }
}
} // namespace

Diagnostics::Diagnostics(QObject *parent)
    : QObject(parent)
{
}

Diagnostics::~Diagnostics() = default;

void Diagnostics::attachSync(SyncController *sync)
{
    m_sync = sync;
}

void Diagnostics::attach(AppSettings *settings, DataManager *dataManager)
{
    disconnect(m_verboseConnection);
    m_settings = settings;
    m_dataManager = dataManager;
    if (m_settings) {
        // The saved choice applies from now on, and the setting is followed while the program runs.
        setVerboseLogging(m_settings->verboseLogging());
        m_verboseConnection = connect(m_settings, &AppSettings::verboseLoggingChanged, this, [this]() {
            setVerboseLogging(m_settings->verboseLogging());
        });
    }
}

void Diagnostics::setVerboseLogging(bool verbose)
{
    // Rules from QT_LOGGING_RULES are evaluated after these, so a developer's own rules still win.
    // Always: KIO's file dialog model (loaded by Plasma's platform theme) warns once per file of
    // the folder when that folder changes while the dialog closes, as a backup or an export
    // written there does. Harmless, but it floods the terminal with the user's file names.
    // Sugo's debug messages are set both ways: whether Qt shows debug messages by default
    // depends on the distribution (Fedora turns them off, Ubuntu leaves them on).
    QString rules = QStringLiteral("kf.kio.widgets.kdirmodel.warning=false\nsugo.*.debug=");
    rules += verbose ? QStringLiteral("true") : QStringLiteral("false");
    QLoggingCategory::setFilterRules(rules);
    QMutexLocker locker(&logMutex());
    s_verbose = verbose;
    while (logBuffer().size() > maxLogEntries()) {
        logBuffer().pop_front();
    }
}

bool Diagnostics::verboseLogging()
{
    return s_verbose;
}

int Diagnostics::logCapacity()
{
    return int(maxLogEntries());
}

void Diagnostics::installLogHandler()
{
    if (!s_originalHandler) {
        s_originalHandler = qInstallMessageHandler(diagnosticsMessageHandler);
    }
}

bool Diagnostics::isRecordedCategory(const QString &category)
{
    // KDE Frameworks are loaded into Sugo by Plasma's platform theme (the file dialog, for one).
    // Their messages say nothing about Sugo and can name the user's files ("No node found for
    // item that was just removed: file:///home/.../Downloads/..."), so they stay out of a report
    // meant to be shared; they are still printed as usual.
    return !category.startsWith(QLatin1String("kf."));
}

void Diagnostics::addLogEntry(const QString &level, const QString &category, const QString &message)
{
    if (!isRecordedCategory(category)) {
        return;
    }
    const QString time = QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    const QString redactedMsg = redact(message);
    const QString line = QStringLiteral("[%1] [%2] [%3] %4").arg(time, level, category, redactedMsg);

    QMutexLocker locker(&logMutex());
    if (logBuffer().size() >= maxLogEntries()) {
        logBuffer().pop_front();
    }
    logBuffer().push_back(line);
}

void Diagnostics::clearLogs()
{
    QMutexLocker locker(&logMutex());
    logBuffer().clear();
}

QString Diagnostics::recentLogs() const
{
    QMutexLocker locker(&logMutex());
    QStringList lines;
    for (const QString &line : logBuffer()) {
        lines.append(line);
    }
    return lines.join(QLatin1Char('\n'));
}

namespace
{
// One pattern per account word, compiled when the words are set. Leaked like the buffer:
// redact() runs from the message handler, also during static destruction.
QList<QRegularExpression> &accountRedactions()
{
    static auto *patterns = new QList<QRegularExpression>;
    return *patterns;
}

// Puts `placeholder` in place of each match of `word`, but leaves the placeholders already put in
// alone: a user named "user" (or an account word "account") must not turn "<user>" into
// "<<user>>".
void replaceWord(QString &text, const QRegularExpression &word, const QString &placeholder)
{
    static const QRegularExpression &placeholders = *new QRegularExpression(QStringLiteral("<(?:user|account|redacted)>"));
    QList<std::pair<qsizetype, qsizetype>> kept;
    for (auto it = placeholders.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        kept.append({match.capturedStart(), match.capturedEnd()});
    }
    QString result;
    qsizetype done = 0;
    for (auto it = word.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        const bool inPlaceholder = std::any_of(kept.cbegin(), kept.cend(), [&match](const std::pair<qsizetype, qsizetype> &span) {
            return match.capturedStart() < span.second && match.capturedEnd() > span.first;
        });
        if (!inPlaceholder) {
            result.append(QStringView(text).mid(done, match.capturedStart() - done));
            result.append(placeholder);
            done = match.capturedEnd();
        }
    }
    if (done > 0) {
        result.append(QStringView(text).mid(done));
        text = result;
    }
}
} // namespace

void Diagnostics::setAccountRedactions(const QStringList &words)
{
    QList<QRegularExpression> patterns;
    for (const QString &word : words) {
        // Very short words would blank out ordinary text.
        const QString trimmed = word.trimmed();
        if (trimmed.size() > 2) {
            // Only where the word stands on its own: a login "tim" leaves "Timer" alone.
            QRegularExpression pattern(QStringLiteral(R"((?<![\p{L}\p{N}_])%1(?![\p{L}\p{N}_]))")
                                           .arg(QRegularExpression::escape(trimmed)),
                                       QRegularExpression::CaseInsensitiveOption);
            pattern.optimize();
            patterns.append(pattern);
        }
    }
    QMutexLocker locker(&logMutex());
    accountRedactions() = patterns;
}

QString Diagnostics::redact(const QString &text)
{
    QString result = text;

    // 0. Secrets first: Authorization headers, user:password@host in URLs, password/token
    // assignments (the calendar sync keeps an app password; none of it may leave in a report).
    // The expressions are never destroyed: the message handler calls this function, and Qt can
    // still print a message after function-local statics are destroyed at exit.
    static const QRegularExpression &authHeaderRegex = *new QRegularExpression(
        QStringLiteral(R"((authorization\s*[:=]\s*)[^\r\n]*)"), QRegularExpression::CaseInsensitiveOption);
    result.replace(authHeaderRegex, QStringLiteral("\\1<redacted>"));
    static const QRegularExpression &urlCredentialsRegex =
        *new QRegularExpression(QStringLiteral(R"(\b([A-Za-z][A-Za-z0-9+.-]*://)[^/\s@]+@)"));
    result.replace(urlCredentialsRegex, QStringLiteral("\\1<redacted>@"));
    static const QRegularExpression &secretAssignmentRegex = *new QRegularExpression(
        // A quoted value up to its closing quote; an unquoted one up to the end of the line or a
        // query-string '&' (a password may contain spaces, so a space does not end it). The key
        // may carry a prefix (access_token, client_secret, X-Auth-Token) but must end in the
        // secret word, so "tokens: 5" or "passwordless login: yes" stay.
        QStringLiteral(R"re(("?[\p{L}\p{N}_-]*(?:password|passwd|secret|token)"?\s*[:=]\s*)(?:"[^"\r\n]*"?|'[^'\r\n]*'?|[^\r\n&]+))re"),
        QRegularExpression::CaseInsensitiveOption);
    result.replace(secretAssignmentRegex, QStringLiteral("\\1<redacted>"));
    // A bearer token outside an Authorization header.
    static const QRegularExpression &bearerRegex = *new QRegularExpression(
        QStringLiteral(R"(\b(bearer\s+)[A-Za-z0-9._~+/=-]+)"), QRegularExpression::CaseInsensitiveOption);
    result.replace(bearerRegex, QStringLiteral("\\1<redacted>"));

    // The accounts the user set up (the calendar login can differ from the system user name).
    QList<QRegularExpression> accounts;
    {
        QMutexLocker locker(&logMutex());
        accounts = accountRedactions();
    }
    for (const QRegularExpression &pattern : std::as_const(accounts)) {
        replaceWord(result, pattern, QStringLiteral("<account>"));
    }
    // The user in a CalDAV path, which can differ from the login (Nextcloud signed in with an
    // e-mail address, a server that names principals by id): /calendars/<user>/,
    // /principals/<user>/, /principals/users/<user>/. In URLs Sugo or Qt log. The whole segment
    // goes, also when the login was only part of it ("<account>.smith"); a segment that is the
    // login alone keeps its "<account>".
    static const QRegularExpression &davUserRegex = *new QRegularExpression(QStringLiteral(
        R"((/(?:calendars|principals/users|principals)/)(?!<account>(?:[/\s?#'"]|$))[^/\s?#'"]+)"));
    result.replace(davUserRegex, QStringLiteral("\\1<user>"));

    // 1. Redact the home directory path (and its resolved form, if it is a symlink), only where
    // the path ends: /home/alice next to the home /home/al does not become "~ice".
    const QString home = QDir::homePath();
    for (const QString &path : {home, QDir(home).canonicalPath()}) {
        if (!path.isEmpty() && path != QLatin1String("/")) {
            const QRegularExpression pathRegex(
                QStringLiteral(R"(%1(?![\p{L}\p{N}_.-]))").arg(QRegularExpression::escape(path)));
            result.replace(pathRegex, QStringLiteral("~"));
        }
    }

    // 2. Redact /home/<username> and /var/home/<username> (Fedora Silverblue/Kinoite) of anyone
    static const QRegularExpression &homeUserRegex = *new QRegularExpression(QStringLiteral(R"((/(?:var/)?home/)[^/\s'"]+)"));
    result.replace(homeUserRegex, QStringLiteral("\\1<user>"));

    // 3. Redact the user part of user@host (shell prompts, ssh, e-mail addresses)
    static const QRegularExpression &userAtHostRegex =
        *new QRegularExpression(QStringLiteral(R"([A-Za-z0-9._%+-]+@([A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*))"));
    result.replace(userAtHostRegex, QStringLiteral("<user>@\\1"));

    // 4. Redact the current user name wherever it stands as a word of its own
    QString user = qEnvironmentVariable("USER");
    if (user.isEmpty()) {
        user = QFileInfo(home).fileName();
    }
    if (user.length() > 2) {
        const QRegularExpression userRegex(QStringLiteral(R"((?<![A-Za-z0-9_.-])%1(?![A-Za-z0-9_-]))")
                                               .arg(QRegularExpression::escape(user)));
        replaceWord(result, userRegex, QStringLiteral("<user>"));
    }

    return result;
}

QString Diagnostics::generateReport() const
{
    QStringList lines;
    lines << QStringLiteral("# Sugo Diagnostics Report");
    lines << QStringLiteral("Generated: ") + QDateTime::currentDateTimeUtc().toString(Qt::ISODate) + QStringLiteral(" (UTC)");
    lines << QString();

    lines << QStringLiteral("## Application");
#ifdef PROJECT_VERSION
    lines << QStringLiteral("- Version: ") + QString::fromLatin1(PROJECT_VERSION);
#else
    lines << QStringLiteral("- Version: ") + QCoreApplication::applicationVersion();
#endif
#ifdef Q_OS_MACOS
    {
        // Sugo.app/Contents/MacOS/Sugo: the bundle is two levels above the executable's folder.
        QDir bundle(QCoreApplication::applicationDirPath());
        const bool inBundle = bundle.dirName() == QLatin1String("MacOS") && bundle.cdUp()
            && bundle.dirName() == QLatin1String("Contents") && bundle.cdUp()
            && bundle.dirName().endsWith(QLatin1String(".app"));
        lines << QStringLiteral("- App Bundle: ") + (inBundle ? QStringLiteral("Yes (") + redact(bundle.absolutePath()) + QStringLiteral(")") : QStringLiteral("No"));
    }
#else
    lines << QStringLiteral("- Build Type: ") + (QFile::exists(QStringLiteral("/.flatpak-info")) ? QStringLiteral("Flatpak (Sandboxed)") : QStringLiteral("Native"));
#endif
    lines << QStringLiteral("- Qt Runtime: ") + QString::fromUtf8(qVersion()) + QStringLiteral(" (compiled with ") + QStringLiteral(QT_VERSION_STR) + QStringLiteral(")");
    lines << QStringLiteral("- QPA Platform: ") + QGuiApplication::platformName();
    lines << QStringLiteral("- Verbose Logging: ") + (verboseLogging() ? QStringLiteral("On (debug messages are recorded, up to %1 lines)").arg(logCapacity()) : QStringLiteral("Off"));
    lines << QString();

    lines << QStringLiteral("## System & Desktop");
    lines << QStringLiteral("- OS: ") + QSysInfo::prettyProductName();
    lines << QStringLiteral("- Kernel: ") + QSysInfo::kernelType() + QStringLiteral(" ") + QSysInfo::kernelVersion();
    lines << QStringLiteral("- Architecture: ") + QSysInfo::currentCpuArchitecture();
#ifndef Q_OS_MACOS
    lines << QStringLiteral("- Desktop Environment (XDG_CURRENT_DESKTOP): ") + QString::fromLocal8Bit(qgetenv("XDG_CURRENT_DESKTOP"));
    lines << QStringLiteral("- Session Type (XDG_SESSION_TYPE): ") + QString::fromLocal8Bit(qgetenv("XDG_SESSION_TYPE"));
#endif
    lines << QStringLiteral("- System Tray Available: ") + ((m_settings && m_settings->trayAvailable()) ? QStringLiteral("Yes") : QStringLiteral("No"));
#ifdef Q_OS_MACOS
    lines << QStringLiteral("- Login Item (LaunchAgent): ") + (m_settings ? (m_settings->startAtLogin() ? QStringLiteral("Enabled") : QStringLiteral("Disabled")) : QStringLiteral("Unknown"));
#else
    lines << QStringLiteral("- Autostart Supported: ") + ((m_settings && m_settings->autostartSupported()) ? QStringLiteral("Yes") : QStringLiteral("No"));
#endif
    lines << QString();

    lines << QStringLiteral("## Configuration");
    if (m_settings) {
        // A sound may be an absolute path to a file of the user's: redacted like every other path.
        lines << QStringLiteral("- Sound Theme: ") + redact(m_settings->soundTheme());
        lines << QStringLiteral("- Sound Volume: ") + QString::number(m_settings->soundVolume()) + QStringLiteral("%");
        lines << QStringLiteral("- Pre-Alarm: ") + (m_settings->preAlarmEnabled() ? QStringLiteral("Enabled") : QStringLiteral("Disabled")) + QStringLiteral(" (") + redact(m_settings->preAlarmSound()) + QStringLiteral(")");
        lines << QStringLiteral("- Focus Sound Mode: ") + QString::number(m_settings->focusSoundMode()) + QStringLiteral(" (Volume: ") + QString::number(m_settings->focusSoundVolume()) + QStringLiteral("%)");
        lines << QStringLiteral("- Screen Lock Auto-Pause: ") + (m_settings->autoPauseOnScreenLock() ? QStringLiteral("Enabled") : QStringLiteral("Disabled"));
        lines << QStringLiteral("- Keep Screen Awake: ") + (m_settings->keepScreenAwake() ? QStringLiteral("Enabled") : QStringLiteral("Disabled"));
        lines << QStringLiteral("- Protect Weekend Streak: ") + (m_settings->protectWeekendStreak() ? QStringLiteral("Yes") : QStringLiteral("No"));
        lines << QStringLiteral("- Prompt Task Note: ") + (m_settings->promptTaskNote() ? QStringLiteral("Yes") : QStringLiteral("No"));
        lines << QStringLiteral("- Close To Tray: ") + (m_settings->closeToTray() ? QStringLiteral("Yes") : QStringLiteral("No"));
        lines << QStringLiteral("- Start Minimized: ") + (m_settings->startMinimized() ? QStringLiteral("Yes") : QStringLiteral("No"));
    } else {
        lines << QStringLiteral("- Settings: (Not attached)");
    }
    lines << QString();

    lines << QStringLiteral("## Storage");
    if (m_dataManager) {
        lines << QStringLiteral("- Database Path: ") + redact(m_dataManager->databasePath());
        lines << QStringLiteral("- Total Recorded Sessions: ") + QString::number(m_dataManager->sessionCount());
        lines << QStringLiteral("- Timers / Presets Count: ") + QString::number(m_dataManager->presetCount());
    } else {
        lines << QStringLiteral("- Storage: (Not attached)");
    }
    lines << QString();

    lines << QStringLiteral("## Sync");
    if (m_sync) {
        const auto key = [](const char *enumName, int value) {
            const QMetaObject &meta = SyncController::staticMetaObject;
            return QString::fromLatin1(meta.enumerator(meta.indexOfEnumerator(enumName)).valueToKey(value));
        };
        const auto yesNo = [](bool value) { return value ? QStringLiteral("Yes") : QStringLiteral("No"); };
        const QDateTime lastSync = m_sync->lastSync();
        lines << QStringLiteral("- State: ") + key("State", m_sync->state());
        lines << QStringLiteral("- Error: ") + key("ErrorKind", m_sync->errorKind());
        lines << QStringLiteral("- Keyring Available: ") + yesNo(m_sync->keyringAvailable());
        lines << QStringLiteral("- Linked: ") + yesNo(m_sync->linked());
        lines << QStringLiteral("- Last Sync: ")
                + (lastSync.isValid() ? lastSync.toUTC().toString(Qt::ISODate) + QStringLiteral(" (UTC)") : QStringLiteral("Never"));
        lines << QStringLiteral("- Devices: ") + QString::number(m_sync->devices().size());
        lines << QStringLiteral("- Server: ") + (m_sync->usesDefaultServer() ? QStringLiteral("default") : QStringLiteral("custom"));
    } else {
        lines << QStringLiteral("- Sync: (Not attached)");
    }
    lines << QString();

    lines << QStringLiteral("## Recent Logs (Redacted)");
    lines << QStringLiteral("```");
    const QString logs = recentLogs();
    if (logs.trimmed().isEmpty()) {
        lines << QStringLiteral("(No recent logs recorded)");
    } else {
        lines << logs;
    }
    lines << QStringLiteral("```");
    lines << QString();

    return lines.join(QLatin1Char('\n'));
}

void Diagnostics::copyToClipboard() const
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (clipboard) {
        clipboard->setText(generateReport());
    }
}

bool Diagnostics::saveInventory(const QString &json) const
{
    const QString path = qEnvironmentVariable("SUGO_INVENTORY");
    if (path.isEmpty()) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(json.toUtf8()) >= 0;
}
