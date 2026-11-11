// SPDX-License-Identifier: GPL-3.0-or-later
#include "ShortcutsFocusInhibitor.h"

#include <QFileInfo>
#include <QLoggingCategory>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <memory>

Q_LOGGING_CATEGORY(lcShortcutsInhibitor, "sugo.platform.inhibitor.shortcuts")

ShortcutsFocusInhibitor::ShortcutsFocusInhibitor(const QString &program, QObject *parent)
    : NotificationInhibitor(parent)
    , m_program(program)
{
}

bool ShortcutsFocusInhibitor::toolFound() const
{
    const QFileInfo info(m_program);
    return info.isFile() && info.isExecutable();
}

bool ShortcutsFocusInhibitor::isAvailable() const
{
    return !m_onShortcut.isEmpty() && !m_offShortcut.isEmpty() && m_list.contains(m_onShortcut)
        && m_list.contains(m_offShortcut) && !runArgument(m_onShortcut, m_identifiers).isEmpty()
        && !runArgument(m_offShortcut, m_identifiers).isEmpty() && toolFound();
}

void ShortcutsFocusInhibitor::setShortcutNames(const QString &onShortcut, const QString &offShortcut)
{
    const QString on = onShortcut.trimmed();
    const QString off = offShortcut.trimmed();
    if (on == m_onShortcut && off == m_offShortcut) {
        return;
    }
    m_onShortcut = on;
    m_offShortcut = off;
    m_onFailed = false; // other shortcuts: worth another try
    report();
    refreshShortcutList();
}

QStringList ShortcutsFocusInhibitor::parseShortcutList(const QByteArray &output)
{
    QStringList names;
    for (const QString &line : QString::fromUtf8(output).split(QLatin1Char('\n'))) {
        const QString name = line.trimmed();
        if (!name.isEmpty()) {
            names.append(name);
        }
    }
    names.sort(Qt::CaseInsensitive);
    names.removeDuplicates();
    return names;
}

QHash<QString, QString> ShortcutsFocusInhibitor::parseShortcutIdentifiers(const QByteArray &output)
{
    // The identifier is the last thing on the line: a name may hold parentheses of its own.
    static const QRegularExpression line(QStringLiteral("^(.*\\S)\\s+\\(([0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12})\\)$"));
    QHash<QString, QString> identifiers;
    for (const QString &text : QString::fromUtf8(output).split(QLatin1Char('\n'))) {
        const QRegularExpressionMatch match = line.match(text.trimmed());
        if (match.hasMatch()) {
            const QString name = match.captured(1).trimmed();
            if (!identifiers.contains(name)) {
                identifiers.insert(name, match.captured(2));
            }
        }
    }
    return identifiers;
}

QString ShortcutsFocusInhibitor::runArgument(const QString &name, const QHash<QString, QString> &identifiers)
{
    if (!name.startsWith(QLatin1Char('-'))) {
        return name;
    }
    return identifiers.value(name);
}

void ShortcutsFocusInhibitor::refreshShortcutList()
{
    if (m_listing) {
        m_listAgain = true;
        return;
    }
    if (!toolFound()) {
        m_list.clear();
        report();
        Q_EMIT shortcutListChanged();
        return;
    }
    m_listing = true;
    runProgram({QStringLiteral("list")}, m_listTimeoutMs, [this](bool ok, bool, const QByteArray &output, const QString &error) {
        QStringList list;
        if (ok) {
            list = parseShortcutList(output);
        } else {
            qCWarning(lcShortcutsInhibitor) << "Cannot list the shortcuts:" << error;
        }
        const bool needIdentifiers = std::any_of(list.cbegin(), list.cend(), [](const QString &name) {
            return name.startsWith(QLatin1Char('-'));
        });
        if (!needIdentifiers) {
            finishListing(list, {});
            return;
        }
        // Only these need them: the plain list works with every version of the tool.
        runProgram({QStringLiteral("list"), QStringLiteral("--show-identifiers")}, m_listTimeoutMs,
                   [this, list](bool ok, bool, const QByteArray &output, const QString &error) {
                       QHash<QString, QString> identifiers;
                       if (ok) {
                           identifiers = parseShortcutIdentifiers(output);
                       } else {
                           qCWarning(lcShortcutsInhibitor)
                               << "Cannot read the identifiers of the shortcuts; those whose name starts with \"-\" cannot run:"
                               << error;
                       }
                       finishListing(list, identifiers);
                   });
    });
}

void ShortcutsFocusInhibitor::finishListing(const QStringList &list, const QHash<QString, QString> &identifiers)
{
    m_listing = false;
    if (list != m_list || identifiers != m_identifiers) {
        m_list = list;
        m_identifiers = identifiers;
        // The shortcuts may have been fixed: worth another try. The same list again (read
        // each time the application becomes active) is not, or a broken "on" would run on
        // every activation.
        m_onFailed = false;
    }
    report();
    Q_EMIT shortcutListChanged();
    if (m_listAgain) {
        m_listAgain = false;
        refreshShortcutList();
    } else {
        sync(); // silencing may have been wanted while the shortcuts were not known yet
    }
}

void ShortcutsFocusInhibitor::inhibit(const QString &reason)
{
    Q_UNUSED(reason) // a Focus has no reason to show
    m_wanted = true;
    m_onFailed = false;
    m_offRetried = false; // the next release gets its own second try
    sync();
}

void ShortcutsFocusInhibitor::release()
{
    m_wanted = false;
    sync();
}

// With no run in flight, make the Focus match what is wanted. Every finished run ends by
// calling this again, so a change of mind during a run is picked up as soon as it returns.
void ShortcutsFocusInhibitor::sync()
{
    if (m_busy || m_pausing) {
        return;
    }
    if (m_wanted && !m_on && !m_onFailed) {
        if (!isAvailable()) {
            if (!m_warnedUnavailable) {
                m_warnedUnavailable = true;
                qCInfo(lcShortcutsInhibitor) << "Cannot turn the Focus on: the shortcuts" << m_onShortcut << "and"
                                             << m_offShortcut << "are not both available";
            }
            return;
        }
        m_warnedUnavailable = false;
        m_busy = true;
        // The names may change while the Focus is on (the settings, "Install shortcuts"): the
        // release must still run the "off" that goes with this "on".
        m_releaseShortcut = runArgument(m_offShortcut, m_identifiers);
        report();
        runProgram({QStringLiteral("run"), runArgument(m_onShortcut, m_identifiers)}, m_runTimeoutMs,
                   [this](bool ok, bool timedOut, const QByteArray &, const QString &error) {
                       m_busy = false;
                       if (!ok) {
                           qCWarning(lcShortcutsInhibitor) << "Cannot turn the Focus on:" << error;
                           // No retry loop: the next inhibit() or a new list of shortcuts tries again.
                           m_onFailed = !timedOut;
                       }
                       // A run that timed out may still have turned it on.
                       m_on = ok || timedOut;
                       report();
                       if (timedOut) {
                           // Shortcuts may still be running it: an "off" at once could come first.
                           pauseThenSync();
                       } else {
                           sync(); // release() may have been called meanwhile
                       }
                   });
    } else if (!m_wanted && m_on) {
        m_busy = true;
        report();
        runProgram({QStringLiteral("run"), m_releaseShortcut}, m_runTimeoutMs,
                   [this](bool ok, bool timedOut, const QByteArray &, const QString &error) {
                       m_busy = false;
                       if (timedOut && !m_offRetried) {
                           // Unknown outcome (a permission prompt, a hang): the Focus may still be
                           // on. Try once more after a pause, still counted as on until then.
                           qCWarning(lcShortcutsInhibitor) << "Cannot turn the Focus off:" << error << "- trying again";
                           m_offRetried = true;
                           pauseThenSync();
                           return;
                       }
                       if (!ok) {
                           // No more tries: a shortcut that fails now will fail again, and quitting
                           // must not wait for it. The user can still turn the Focus off.
                           qCWarning(lcShortcutsInhibitor) << "Cannot turn the Focus off:" << error;
                       }
                       m_on = false;
                       m_offRetried = false;
                       report();
                       sync(); // inhibit() may have been called meanwhile
                   });
    }
}

// After a timed-out run: no run for retryDelayMs, then whatever is wanted by then.
void ShortcutsFocusInhibitor::pauseThenSync()
{
    m_pausing = true;
    QTimer::singleShot(m_retryDelayMs, this, [this]() {
        m_pausing = false;
        sync();
    });
}

void ShortcutsFocusInhibitor::runProgram(const QStringList &arguments, int timeoutMs, Done done)
{
    auto *process = new QProcess(this);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    // finished, errorOccurred and the timeout may each fire; only the first one counts.
    auto handled = std::make_shared<bool>(false);
    const auto finish = [this, process, timer, handled, done = std::move(done)](bool ok, bool timedOut, const QString &error) {
        if (*handled) {
            return;
        }
        *handled = true;
        timer->stop();
        disconnect(process, nullptr, this, nullptr);
        const QByteArray output = process->readAllStandardOutput();
        if (process->state() == QProcess::NotRunning) {
            process->deleteLater();
        } else {
            // Killed after a timeout: gone once the system has reaped it.
            connect(process, &QProcess::finished, process, &QObject::deleteLater);
        }
        done(ok, timedOut, output, error);
    };

    connect(process, &QProcess::finished, this, [process, finish](int exitCode, QProcess::ExitStatus status) {
        if (status == QProcess::NormalExit && exitCode == 0) {
            finish(true, false, {});
            return;
        }
        QString error = QString::fromUtf8(process->readAllStandardError()).trimmed();
        if (error.isEmpty()) {
            error = status == QProcess::NormalExit ? QStringLiteral("exit code %1").arg(exitCode) : QStringLiteral("crashed");
        }
        finish(false, false, error);
    });
    connect(process, &QProcess::errorOccurred, this, [process, finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finish(false, false, process->errorString());
        }
    });
    connect(timer, &QTimer::timeout, this, [process, finish, timeoutMs]() {
        process->kill();
        finish(false, true, QStringLiteral("no answer within %1 ms").arg(timeoutMs));
    });

    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->setStandardInputFile(QProcess::nullDevice());
    timer->start(timeoutMs);
    process->start(m_program, arguments);
}

void ShortcutsFocusInhibitor::report()
{
    const bool available = isAvailable();
    if (available != m_wasAvailable) {
        m_wasAvailable = available;
        Q_EMIT availableChanged();
    }
    const bool now = isInhibited();
    if (now != m_reported) {
        m_reported = now;
        Q_EMIT inhibitedChanged();
    }
}
