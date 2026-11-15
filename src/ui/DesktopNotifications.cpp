#include "DesktopNotifications.hpp"

#include "utils/Logger.hpp"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QVariantMap>

namespace {

const QString kService = QStringLiteral("org.freedesktop.Notifications");
const QString kPath = QStringLiteral("/org/freedesktop/Notifications");
const QString kInterface = QStringLiteral("org.freedesktop.Notifications");

// Notifications whose action never came keep their tag only this long; a server that
// never sends NotificationClosed (some don't for persistent ones) can't grow the map forever.
constexpr int kMaxTrackedNotifications = 200;

}

DesktopNotifications::DesktopNotifications(QObject* parent) : QObject(parent) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return;

    bus.connect(kService, kPath, kInterface, QStringLiteral("ActionInvoked"), this,
                SLOT(onActionInvoked(uint, QString)));
    bus.connect(kService, kPath, kInterface, QStringLiteral("ActivationToken"), this,
                SLOT(onActivationToken(uint, QString)));
    bus.connect(kService, kPath, kInterface, QStringLiteral("NotificationClosed"), this,
                SLOT(onNotificationClosed(uint, uint)));

    // The shell can be restarted (or swapped) while Orbit runs; ask again when it comes back.
    auto* watcher = new QDBusServiceWatcher(kService, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString&, const QString&, const QString& newOwner) {
                tags_.clear();
                if (newOwner.isEmpty()) {
                    available_ = false;
                    capabilities_.clear();
                } else {
                    queryServer();
                }
            });
    queryServer();
}

void DesktopNotifications::queryServer() {
    const QDBusMessage call =
        QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("GetCapabilities"));
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        const QDBusPendingReply<QStringList> reply = *w;
        if (reply.isError()) {
            available_ = false;
            ORBIT_LOG_INFO("notify", "No desktop notification server: " + reply.error().message().toStdString());
            return;
        }
        available_ = true;
        capabilities_ = reply.value();
        ORBIT_LOG_INFO("notify", "Notification server capabilities: " + capabilities_.join(QLatin1Char(',')).toStdString());
    });
}

QString DesktopNotifications::escapeMarkup(const QString& text) {
    return text.toHtmlEscaped();
}

void DesktopNotifications::notify(const QString& title, const QString& body, bool warning, const QString& category,
                                  const QList<Action>& actions, const QString& tag) {
    QStringList actionList;
    // "default" is what a click on the notification sends; it has no button of its own.
    actionList << QStringLiteral("default") << QStringLiteral("Show Orbit");
    if (supportsActions()) {
        for (const Action& action : actions) actionList << action.key << action.label;
    }

    QVariantMap hints;
    hints.insert(QStringLiteral("desktop-entry"), QStringLiteral("orbit"));
    hints.insert(QStringLiteral("urgency"), QVariant::fromValue<uchar>(warning ? 2 : 1));
    if (!category.isEmpty()) hints.insert(QStringLiteral("category"), category);

    const bool markup = capabilities_.contains(QStringLiteral("body-markup"));
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("Notify"));
    call << QCoreApplication::applicationName() << uint(0) << QStringLiteral("orbit") << title
         << (markup ? escapeMarkup(body) : body) << actionList << hints << int(-1);

    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, tag](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        const QDBusPendingReply<uint> reply = *w;
        if (reply.isError()) {
            ORBIT_LOG_WARN("notify", "Notification failed: " + reply.error().message().toStdString());
            return;
        }
        if (tags_.size() >= kMaxTrackedNotifications) tags_.clear();
        tags_.insert(reply.value(), tag);
    });
}

void DesktopNotifications::onActionInvoked(uint id, const QString& key) {
    const auto it = tags_.constFind(id);
    if (it == tags_.constEnd()) return;  // Another app's notification.
    const QString tag = it.value();
    ORBIT_LOG_INFO("notify", "Notification action: " + key.toStdString());
    emit actionInvoked(tag, key);
}

void DesktopNotifications::onActivationToken(uint id, const QString& token) {
    // Comes just before ActionInvoked. A native Wayland window may only take focus with
    // it (xdg-activation); Qt reads it from the environment when the window is raised.
    if (tags_.contains(id) && !token.isEmpty()) qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
}

void DesktopNotifications::onNotificationClosed(uint id, uint) {
    tags_.remove(id);
}
