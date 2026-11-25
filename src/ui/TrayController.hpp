#pragma once

#include <QObject>
#include <QColor>
#include <QIcon>
#include <QPoint>
#include <QString>

class QAction;
class QActionGroup;
class QQuickWindow;
class QSystemTrayIcon;
class DesktopNotifications;

// Owns the system tray icon and menu, and mediates showing/hiding the main QML window
// so the app can live in the tray instead of quitting when the window is closed.
class TrayController final : public QObject {
    Q_OBJECT

public:
    // Without showIcon (headless) there's no tray icon, only desktop notifications.
    explicit TrayController(bool showIcon = true, QObject* parent = nullptr);

    void attachWindow(QQuickWindow* window);

    Q_INVOKABLE void hideToTray();
    Q_INVOKABLE void showAndRaise();

    Q_INVOKABLE void notifyDownloadAdded(const QString& label);
    Q_INVOKABLE void notify(const QString& title, const QString& message, bool warning = false);
    // A finished or failed download. On Linux the notification gets buttons (Open, Show in
    // Folder, or Retry) that come back through downloadActionRequested.
    Q_INVOKABLE void notifyTransfer(qlonglong id, const QString& title, const QString& message, bool success);
    // Downloads were paused because the connection is metered; offers to continue anyway.
    void notifyMetered(int count);
    // A finished download is the same file as originalPath: offers to trash the new copy.
    void notifyDuplicate(qlonglong id, const QString& name, const QString& originalPath);
    // Offers copied links as a notification with Download and Review buttons (used where
    // Orbit can't place its own prompt by the tray, i.e. under Wayland). False when the
    // desktop has no notification server with buttons; the caller shows its prompt then.
    Q_INVOKABLE bool offerLinks(const QStringList& links);
    // Draws a thin ring around the tray icon that fills with the overall progress of what's
    // downloading (0..1), in the accent colour; a negative value removes it.
    Q_INVOKABLE void setProgress(double progress, const QColor& color);
    Q_INVOKABLE void setStatus(const QString& tooltip, bool canPause, bool canResume);
    // Reflects the armed "when downloads finish" action in the tray menu.
    void setWhenDoneAction(const QString& action, bool hibernateAvailable);
    // Where a small popup of this size sits in the corner by the notification area.
    Q_INVOKABLE QPoint cornerPosition(int width, int height) const;

signals:
    void quitRequested();
    void pauseAllRequested();
    void resumeAllRequested();
    void whenDoneRequested(const QString& action);
    // "open", "reveal" or "retry", picked on a download's notification.
    void downloadActionRequested(qlonglong id, const QString& action);
    void meteredResumeRequested();
    // "download" or "review", picked on a copied-links notification.
    void linksActionRequested(const QString& action, const QStringList& links);

private:
    void buildTrayIcon();
    void showMessage(const QString& title, const QString& message, bool warning, int timeoutMs);

    QQuickWindow* window_{nullptr};
    QSystemTrayIcon* trayIcon_{nullptr};
    QAction* pauseAllAction_{nullptr};
    QAction* resumeAllAction_{nullptr};
    QActionGroup* whenDoneGroup_{nullptr};
    bool hasShownTrayHint_{false};
    QIcon baseIcon_;
    int shownProgressStep_{-1};
    int launcherStep_{-2};  // last progress step sent to the taskbar entry (-2 = none yet)
    QColor shownProgressColor_;
    QStringList offeredLinks_;
    DesktopNotifications* desktopNotifications_{nullptr};
};
