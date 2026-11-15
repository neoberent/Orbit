#include "PortalSettings.hpp"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QVariantList>

namespace {
const QString kService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kInterface = QStringLiteral("org.freedesktop.portal.Settings");
const QString kAppearance = QStringLiteral("org.freedesktop.appearance");
}

PortalSettings::PortalSettings(QObject* parent) : QObject(parent) {
    QDBusConnection::sessionBus().connect(kService, kPath, kInterface, QStringLiteral("SettingChanged"), this,
                                          SLOT(onSettingChanged(QString, QString, QDBusVariant)));
}

void PortalSettings::read(const QString& key) {
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("ReadOne"));
    call << kAppearance << key;
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, key](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        const QDBusPendingReply<QDBusVariant> reply = *w;
        if (!reply.isError()) emitValue(key, reply.value().variant());  // An error: no portal, or no such key.
    });
}

void PortalSettings::onSettingChanged(const QString& group, const QString& key, const QDBusVariant& value) {
    if (group == kAppearance) emitValue(key, value.variant());
}

void PortalSettings::emitValue(const QString& key, const QVariant& raw) {
    QVariant value = raw;
    // Some portal versions wrap the value in one more variant.
    while (value.canConvert<QDBusVariant>()) value = value.value<QDBusVariant>().variant();
    if (value.canConvert<QDBusArgument>()) {
        // accent-color is a (ddd) structure.
        const QDBusArgument arg = value.value<QDBusArgument>();
        if (arg.currentType() == QDBusArgument::StructureType) {
            double r = -1, g = -1, b = -1;
            arg.beginStructure();
            arg >> r >> g >> b;
            arg.endStructure();
            value = QVariantList{r, g, b};
        }
    }
    emit settingChanged(key, value);
}
