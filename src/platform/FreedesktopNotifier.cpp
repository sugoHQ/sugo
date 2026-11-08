// SPDX-License-Identifier: GPL-3.0-or-later
#include "FreedesktopNotifier.h"

#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QLoggingCategory>
#include <QVariantMap>

Q_LOGGING_CATEGORY(lcNotifier, "sugo.platform.notifier")

namespace
{
const QString kService = QStringLiteral("org.freedesktop.Notifications");
const QString kPath = QStringLiteral("/org/freedesktop/Notifications");
const QString kInterface = QStringLiteral("org.freedesktop.Notifications");
constexpr int kCallTimeoutMs = 5000;

const QString kDefaultKey = QStringLiteral("default");
const QString kInlineReplyKey = QStringLiteral("inline-reply");
const QString kReplyFallbackKey = QStringLiteral("reply");

/// The bus says no notification server runs or can be started; a timeout or a server's own
/// error is not that (the notification may still appear, and would then appear twice).
bool noServer(const QDBusError &error)
{
    return error.type() == QDBusError::ServiceUnknown || error.name() == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner")
        || error.name().startsWith(QLatin1String("org.freedesktop.DBus.Error.Spawn."));
}
} // namespace

FreedesktopNotifier::FreedesktopNotifier(const QDBusConnection &bus, const QString &appName, const QString &desktopEntry, QObject *parent)
    : Notifier(parent)
    , m_bus(bus)
    , m_appName(appName)
    , m_desktopEntry(desktopEntry)
    , m_watcher(kService, m_bus, QDBusServiceWatcher::WatchForOwnerChange)
{
    m_bus.connect(kService, kPath, kInterface, QStringLiteral("ActionInvoked"), this, SLOT(onActionInvoked(uint, QString)));
    m_bus.connect(kService, kPath, kInterface, QStringLiteral("ActivationToken"), this, SLOT(onActivationToken(uint, QString)));
    m_bus.connect(kService, kPath, kInterface, QStringLiteral("NotificationReplied"), this, SLOT(onNotificationReplied(uint, QString)));
    m_bus.connect(kService, kPath, kInterface, QStringLiteral("NotificationClosed"), this, SLOT(onNotificationClosed(uint, uint)));

    // A new server knows nothing of the old one's notifications, and may reuse their ids.
    connect(&m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this]() {
        ++m_generation;
        m_shown.clear();
    });
}

void FreedesktopNotifier::setFallback(std::unique_ptr<Notifier> fallback)
{
    m_fallback = std::move(fallback);
}

bool FreedesktopNotifier::showElsewhere(const QDBusError &error, QObject *context, const Notification &notification)
{
    if (!m_fallback || !context || !noServer(error)) {
        return false;
    }
    qCDebug(lcNotifier) << "No notification server, showing it through the fallback:" << error.message();
    m_fallback->notify(context, notification);
    return true;
}

void FreedesktopNotifier::notify(QObject *context, const Notification &notification)
{
    const QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("GetCapabilities"));
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, kCallTimeoutMs), this);
    QPointer<QObject> guard(context);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, guard, notification](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QStringList> reply = *w;
        if (reply.isError()) {
            if (showElsewhere(reply.error(), guard, notification)) {
                return;
            }
            if (noServer(reply.error()) || !guard) {
                qCWarning(lcNotifier) << "No notification server:" << reply.error().message();
                return;
            }
            // A server that is there but did not answer (busy, stuck, broken). Nothing has been
            // sent yet, so the fallback cannot make it appear twice: it is sure to show it.
            // Without one, try the server anyway, with no capabilities known (no actions).
            qCWarning(lcNotifier) << "The notification server did not give its capabilities:" << reply.error().message();
            if (m_fallback) {
                m_fallback->notify(guard, notification);
            } else {
                send(guard, notification, {});
            }
            return;
        }
        if (guard) {
            send(guard, notification, reply.value());
        }
    });
}

void FreedesktopNotifier::send(QObject *context, const Notification &notification, const QStringList &capabilities)
{
    Shown shown;
    shown.context = context;
    QStringList actions; // key, label, key, label...
    QVariantMap hints{
        {QStringLiteral("desktop-entry"), m_desktopEntry},
        // Sugo plays its own sound.
        {QStringLiteral("suppress-sound"), true},
    };

    if (capabilities.contains(QStringLiteral("actions"))) {
        if (notification.defaultAction) {
            actions << kDefaultKey << notification.defaultActionLabel;
            shown.actions.insert(kDefaultKey, notification.defaultAction);
        }
        for (qsizetype i = 0; i < notification.actions.size(); ++i) {
            const QString key = QStringLiteral("action-%1").arg(i);
            actions << key << notification.actions.at(i).label;
            shown.actions.insert(key, notification.actions.at(i).handler);
        }
        if (notification.reply) {
            const Reply &reply = *notification.reply;
            if (capabilities.contains(kInlineReplyKey)) {
                actions << kInlineReplyKey << reply.label;
                hints.insert(QStringLiteral("x-kde-reply-placeholder-text"), reply.placeholder);
                hints.insert(QStringLiteral("x-kde-reply-submit-button-text"), reply.submitLabel);
                hints.insert(QStringLiteral("x-kde-reply-submit-button-icon-name"), reply.submitIconName);
                shown.replied = reply.replied;
            } else {
                actions << kReplyFallbackKey << reply.label;
                shown.actions.insert(kReplyFallbackKey, reply.fallback);
            }
        }
    }

    // A server with body-markup reads the body as markup (the summary never): the text is plain,
    // and a "<" or "&" in it (a timer name, a task note) must stay as it is.
    const QString body = capabilities.contains(QStringLiteral("body-markup")) ? notification.text.toHtmlEscaped() : notification.text;

    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("Notify"));
    message.setArguments({m_appName,
                          QVariant::fromValue(0U), // replaces_id: a new notification
                          notification.iconName,
                          notification.title,
                          body,
                          actions,
                          hints,
                          -1}); // the server's own timeout
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, kCallTimeoutMs), this);
    const quint64 generation = m_generation;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, shown, generation, notification](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<uint> reply = *w;
        if (reply.isError()) {
            // The server may have gone away since it answered GetCapabilities.
            if (!showElsewhere(reply.error(), shown.context, notification)) {
                qCWarning(lcNotifier) << "Cannot show a notification:" << reply.error().message();
            }
            return;
        }
        if (generation == m_generation && (!shown.actions.isEmpty() || shown.replied)) {
            m_shown.insert(reply.value(), shown);
        }
    });
}

void FreedesktopNotifier::onActionInvoked(uint id, const QString &actionKey)
{
    const auto it = m_shown.constFind(id);
    if (it == m_shown.cend() || !it->context) {
        return;
    }
    // Copied: the handler may cause this notification to be dropped.
    const Handler handler = it->actions.value(actionKey);
    const QString token = it->activationToken;
    if (handler) {
        handler(token);
    }
}

void FreedesktopNotifier::onActivationToken(uint id, const QString &token)
{
    const auto it = m_shown.find(id);
    if (it != m_shown.end()) {
        it->activationToken = token;
    }
}

void FreedesktopNotifier::onNotificationReplied(uint id, const QString &text)
{
    const auto it = m_shown.constFind(id);
    if (it == m_shown.cend() || !it->context || !it->replied) {
        return;
    }
    const auto replied = it->replied;
    replied(text);
}

void FreedesktopNotifier::onNotificationClosed(uint id, uint reason)
{
    Q_UNUSED(reason)
    m_shown.remove(id);
}
