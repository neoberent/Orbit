#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

// Desktop notifications through org.freedesktop.Notifications, the server every Linux
// desktop (and shells like Noctalia, swaync, mako or dunst) provides. Unlike
// QSystemTrayIcon::showMessage they can carry buttons: "Open", "Show in Folder", "Retry".
// When no server is running, available() is false and the caller falls back to the tray.
class DesktopNotifications final : public QObject {
    Q_OBJECT

public:
    struct Action {
        QString key;
        QString label;
    };

    explicit DesktopNotifications(QObject* parent = nullptr);

    // True once a notification server answered. Buttons only show when it also supports
    // actions; clicking the notification itself always sends "default".
    [[nodiscard]] bool available() const noexcept { return available_; }
    [[nodiscard]] bool supportsActions() const noexcept { return capabilities_.contains(QStringLiteral("actions")); }
    [[nodiscard]] QStringList capabilities() const { return capabilities_; }

    // Shows a notification and remembers `tag` for it, which comes back with any action
    // the user picks. category follows the spec ("transfer.complete", "transfer.error").
    void notify(const QString& title, const QString& body, bool warning, const QString& category,
                const QList<Action>& actions, const QString& tag);

    // Escapes text for servers that render a body as markup (the body-markup capability).
    [[nodiscard]] static QString escapeMarkup(const QString& text);

signals:
    // key is the Action::key the user picked, or "default" for a click on the notification.
    void actionInvoked(const QString& tag, const QString& key);

private slots:
    void onActionInvoked(uint id, const QString& key);
    void onActivationToken(uint id, const QString& token);
    void onNotificationClosed(uint id, uint reason);

private:
    void queryServer();

    bool available_{false};
    QStringList capabilities_;
    QHash<uint, QString> tags_;
};
