#include "NetworkIdentity.hpp"

#include "utils/Logger.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QVariantMap>

namespace {
const QString kService = QStringLiteral("org.freedesktop.NetworkManager");
const QString kPath = QStringLiteral("/org/freedesktop/NetworkManager");
const QString kProperties = QStringLiteral("org.freedesktop.DBus.Properties");

QDBusMessage getProperty(const QString& path, const QString& interface, const QString& name) {
    QDBusMessage call = QDBusMessage::createMethodCall(kService, path, kProperties, QStringLiteral("Get"));
    call << interface << name;
    return call;
}
}

NetworkIdentity::NetworkIdentity(QObject* parent) : QObject(parent) {
    QDBusConnection::systemBus().connect(kService, kPath, kProperties, QStringLiteral("PropertiesChanged"), this,
                                         SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    refresh();
}

void NetworkIdentity::onPropertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList&) {
    if (interface == kService && (changed.contains(QStringLiteral("PrimaryConnection")) || changed.contains(QStringLiteral("State")))) {
        refresh();
    }
}

void NetworkIdentity::refresh() {
    QDBusConnection bus = QDBusConnection::systemBus();
    auto* watcher = new QDBusPendingCallWatcher(
        bus.asyncCall(getProperty(kPath, kService, QStringLiteral("PrimaryConnection")), 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        const QDBusPendingReply<QDBusVariant> reply = *w;
        const QString path = reply.isError() ? QString() : reply.value().variant().value<QDBusObjectPath>().path();
        if (path.isEmpty() || path == QLatin1String("/")) {
            if (!name_.isEmpty()) {
                name_.clear();
                emit changed();
            }
            return;
        }
        auto* idWatcher = new QDBusPendingCallWatcher(
            QDBusConnection::systemBus().asyncCall(
                getProperty(path, QStringLiteral("org.freedesktop.NetworkManager.Connection.Active"), QStringLiteral("Id")), 3000),
            this);
        connect(idWatcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* iw) {
            iw->deleteLater();
            const QDBusPendingReply<QDBusVariant> idReply = *iw;
            const QString next = idReply.isError() ? QString() : idReply.value().variant().toString().left(128);
            if (next == name_) return;
            name_ = next;
            ORBIT_LOG_INFO("network", name_.isEmpty() ? std::string("No active network") : "On network \"" + name_.toStdString() + "\"");
            emit changed();
        });
    });
}
