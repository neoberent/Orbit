#pragma once

#include <QObject>
#include <QPoint>
#include <QString>

class QAction;
class QActionGroup;
class QQuickWindow;
class QSystemTrayIcon;

// Owns the system tray icon and menu, and mediates showing/hiding the main QML window
// so the app can live in the tray instead of quitting when the window is closed.
class TrayController final : public QObject {
    Q_OBJECT

public:
    explicit TrayController(QObject* parent = nullptr);

    void attachWindow(QQuickWindow* window);

    Q_INVOKABLE void hideToTray();
    Q_INVOKABLE void showAndRaise();

    Q_INVOKABLE void notifyDownloadAdded(const QString& label);
    Q_INVOKABLE void notify(const QString& title, const QString& message, bool warning = false);
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

private:
    void buildTrayIcon();

    QQuickWindow* window_{nullptr};
    QSystemTrayIcon* trayIcon_{nullptr};
    QAction* pauseAllAction_{nullptr};
    QAction* resumeAllAction_{nullptr};
    QActionGroup* whenDoneGroup_{nullptr};
    bool hasShownTrayHint_{false};
};
