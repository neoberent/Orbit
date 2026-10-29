#include "TrayController.hpp"

#include "utils/Logger.hpp"

#include <QAction>
#include <QActionGroup>
#include <QIcon>
#include <QMenu>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QScreen>
#include <QSystemTrayIcon>

TrayController::TrayController(QObject* parent) : QObject(parent) {
    buildTrayIcon();
}

void TrayController::buildTrayIcon() {
    trayIcon_ = new QSystemTrayIcon(QIcon(QStringLiteral(":/images/logo.png")), this);
    trayIcon_->setToolTip(QStringLiteral("Orbit Download Manager"));

    auto* menu = new QMenu();

    QAction* showAction = menu->addAction(QStringLiteral("Show Orbit"));
    connect(showAction, &QAction::triggered, this, &TrayController::showAndRaise);

    menu->addSeparator();

    resumeAllAction_ = menu->addAction(QStringLiteral("Resume All"));
    connect(resumeAllAction_, &QAction::triggered, this, &TrayController::resumeAllRequested);
    pauseAllAction_ = menu->addAction(QStringLiteral("Pause All"));
    connect(pauseAllAction_, &QAction::triggered, this, &TrayController::pauseAllRequested);

    QMenu* whenDone = menu->addMenu(QStringLiteral("When Downloads Finish"));
    whenDoneGroup_ = new QActionGroup(whenDone);
    whenDoneGroup_->setExclusive(true);
    const std::pair<const char*, const char*> actions[] = {
        {"none", "Do Nothing"}, {"exit", "Exit Orbit"}, {"sleep", "Sleep"}, {"hibernate", "Hibernate"}, {"shutdown", "Shut Down"},
    };
    for (const auto& [key, label] : actions) {
        QAction* action = whenDone->addAction(QString::fromLatin1(label));
        action->setCheckable(true);
        action->setData(QString::fromLatin1(key));
        action->setChecked(QLatin1String(key) == QLatin1String("none"));
        whenDoneGroup_->addAction(action);
        connect(action, &QAction::triggered, this, [this, action] { emit whenDoneRequested(action->data().toString()); });
    }

    menu->addSeparator();

    QAction* quitAction = menu->addAction(QStringLiteral("Quit"));
    connect(quitAction, &QAction::triggered, this, [this] { emit quitRequested(); });

    trayIcon_->setContextMenu(menu);

    connect(trayIcon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger) {
            showAndRaise();
        }
    });

    connect(trayIcon_, &QSystemTrayIcon::messageClicked, this, &TrayController::showAndRaise);

    trayIcon_->show();
}

void TrayController::attachWindow(QQuickWindow* window) {
    window_ = window;
}

void TrayController::hideToTray() {
    if (!window_) return;
    window_->hide();

    if (!hasShownTrayHint_ && trayIcon_) {
        trayIcon_->showMessage(
            QStringLiteral("Orbit is still running"),
            QStringLiteral("Downloads keep going in the background. Click the tray icon to reopen Orbit."),
            QSystemTrayIcon::Information, 4000);
        hasShownTrayHint_ = true;
    }
}

void TrayController::showAndRaise() {
    if (!window_) return;
    window_->showNormal();
    window_->raise();
    window_->requestActivate();
}

void TrayController::notify(const QString& title, const QString& message, bool warning) {
    ORBIT_LOG_INFO("tray", "Notification: " + title.toStdString() + " - " + message.toStdString());
    if (!trayIcon_) return;
    trayIcon_->showMessage(title, message, warning ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information, 5000);
}

void TrayController::setStatus(const QString& tooltip, bool canPause, bool canResume) {
    if (trayIcon_) trayIcon_->setToolTip(tooltip);
    if (pauseAllAction_) pauseAllAction_->setEnabled(canPause);
    if (resumeAllAction_) resumeAllAction_->setEnabled(canResume);
}

void TrayController::setWhenDoneAction(const QString& action, bool hibernateAvailable) {
    if (!whenDoneGroup_) return;
    for (QAction* item : whenDoneGroup_->actions()) {
        item->setChecked(item->data().toString() == action);
        if (item->data().toString() == QLatin1String("hibernate")) item->setVisible(hibernateAvailable);
    }
}

QPoint TrayController::cornerPosition(int width, int height) const {
    const QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) return {100, 100};
    const QRect area = screen->availableGeometry();
    constexpr int kMargin = 16;
    return {area.right() - width - kMargin, area.bottom() - height - kMargin};
}

void TrayController::notifyDownloadAdded(const QString& label) {
    if (!trayIcon_) return;
    trayIcon_->showMessage(QStringLiteral("Download added from browser"), label,
                            QSystemTrayIcon::Information, 3500);
}
