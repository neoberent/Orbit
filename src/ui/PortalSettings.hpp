#pragma once

#include <QObject>
#include <QString>
#include <QVariant>

class QDBusVariant;

// Reads org.freedesktop.appearance from the desktop's settings portal and passes on its
// changes (Linux only). Values come out unwrapped: color-scheme as uint, accent-color as
// a QVariantList of three doubles.
class PortalSettings final : public QObject {
    Q_OBJECT

public:
    explicit PortalSettings(QObject* parent = nullptr);
    void read(const QString& key);

signals:
    void settingChanged(const QString& key, const QVariant& value);

private slots:
    void onSettingChanged(const QString& group, const QString& key, const QDBusVariant& value);

private:
    void emitValue(const QString& key, const QVariant& raw);
};
