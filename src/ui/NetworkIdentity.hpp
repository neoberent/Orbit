#pragma once

#include <QObject>
#include <QString>

// The name NetworkManager gives the connection in use (the Wi-Fi network's name, "Wired
// connection 1", a VPN's name...), so settings can follow the network you're on. Linux
// only; empty when NetworkManager isn't running or nothing is connected.
class NetworkIdentity final : public QObject {
    Q_OBJECT

public:
    explicit NetworkIdentity(QObject* parent = nullptr);
    [[nodiscard]] QString name() const { return name_; }

signals:
    void changed();

private slots:
    void onPropertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);

private:
    void refresh();

    QString name_;
};
