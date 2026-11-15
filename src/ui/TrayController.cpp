#include "TrayController.hpp"

#include "utils/Logger.hpp"
#if defined(__linux__)
#include "DesktopNotifications.hpp"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QVariantMap>
#endif

#include <QAction>
#include <QActionGroup>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>

#include <algorithm>
#include <utility>
#include <QGuiApplication>
#include <QMetaObject>
#include <QQuickWindow>
#include <QScreen>
#include <QSystemTrayIcon>
#include <QTimer>

TrayController::TrayController(bool showIcon, QObject* parent) : QObject(parent) {
    if (showIcon) buildTrayIcon();
#if defined(__linux__)
    desktopNotifications_ = new DesktopNotifications(this);
    connect(desktopNotifications_, &DesktopNotifications::actionInvoked, this,
            [this](const QString& tag, const QString& key) {
                if (key == QLatin1String("default")) {
                    showAndRaise();
                    return;
                }
                if (tag == QLatin1String("links")) {
                    if (key == QLatin1String("download") || key == QLatin1String("review")) {
                        emit linksActionRequested(key, std::exchange(offeredLinks_, {}));
                    }
                    return;
                }
                if (tag == QLatin1String("metered")) {
                    if (key == QLatin1String("resume")) emit meteredResumeRequested();
                    return;
                }
                bool ok = false;
                const qlonglong id = tag.toLongLong(&ok);
                if (ok && id > 0) emit downloadActionRequested(id, key);
            });
#endif
}

// Through the desktop's notification server where there is one (it knows the app from
// orbit.desktop and can carry buttons), otherwise as a balloon from the tray icon.
void TrayController::showMessage(const QString& title, const QString& message, bool warning, int timeoutMs) {
#if defined(__linux__)
    if (desktopNotifications_ && desktopNotifications_->available()) {
        desktopNotifications_->notify(title, message, warning, {}, {}, {});
        return;
    }
#endif
    if (!trayIcon_) return;
    trayIcon_->showMessage(title, message, warning ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information, timeoutMs);
}

void TrayController::buildTrayIcon() {
    baseIcon_ = QIcon(QStringLiteral(":/images/logo.png"));
    trayIcon_ = new QSystemTrayIcon(baseIcon_, this);
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
        showMessage(QStringLiteral("Orbit is still running"),
                    QStringLiteral("Downloads keep going in the background. Click the tray icon to reopen Orbit."),
                    false, 4000);
        hasShownTrayHint_ = true;
    }
}

void TrayController::showAndRaise() {
    if (!window_) return;
    // A notification's focus token is good for this one raise; programs Orbit starts
    // later (a video player, the file manager) mustn't inherit it.
    if (qEnvironmentVariableIsSet("XDG_ACTIVATION_TOKEN")) {
        QTimer::singleShot(1000, this, [] { qunsetenv("XDG_ACTIVATION_TOKEN"); });
    }
    if (!window_->property("revealed").toBool()) {
        // Started in the tray: the first showing plays the opening animation.
        QMetaObject::invokeMethod(window_, "reveal");
        return;
    }
    window_->showNormal();
    window_->raise();
    window_->requestActivate();
}

void TrayController::notify(const QString& title, const QString& message, bool warning) {
    ORBIT_LOG_INFO("tray", "Notification: " + title.toStdString() + " - " + message.toStdString());
    showMessage(title, message, warning, 5000);
}

void TrayController::notifyTransfer(qlonglong id, const QString& title, const QString& message, bool success) {
    ORBIT_LOG_INFO("tray", "Notification: " + title.toStdString() + " - " + message.toStdString());
#if defined(__linux__)
    if (desktopNotifications_ && desktopNotifications_->available()) {
        QList<DesktopNotifications::Action> actions;
        if (success) {
            actions << DesktopNotifications::Action{QStringLiteral("open"), QStringLiteral("Open")}
                    << DesktopNotifications::Action{QStringLiteral("reveal"), QStringLiteral("Show in Folder")};
        } else {
            actions << DesktopNotifications::Action{QStringLiteral("retry"), QStringLiteral("Retry")};
        }
        desktopNotifications_->notify(title, message, !success,
                                      success ? QStringLiteral("transfer.complete") : QStringLiteral("transfer.error"),
                                      actions, QString::number(id));
        return;
    }
#else
    Q_UNUSED(id)
#endif
    showMessage(title, message, !success, 5000);
}

bool TrayController::offerLinks(const QStringList& links) {
#if defined(__linux__)
    if (links.isEmpty() || !desktopNotifications_ || !desktopNotifications_->available() ||
        !desktopNotifications_->supportsActions()) {
        return false;
    }
    offeredLinks_ = links;
    const QString title = links.size() == 1 ? QStringLiteral("Download the copied link?")
                                            : QStringLiteral("Download %1 copied links?").arg(links.size());
    QList<DesktopNotifications::Action> actions{{QStringLiteral("download"), QStringLiteral("Download")}};
    if (links.size() == 1) actions << DesktopNotifications::Action{QStringLiteral("review"), QStringLiteral("Review…")};
    desktopNotifications_->notify(title, links.first(), false, QStringLiteral("transfer"), actions, QStringLiteral("links"));
    return true;
#else
    Q_UNUSED(links)
    return false;
#endif
}

void TrayController::notifyMetered(int count) {
    const QString title = QStringLiteral("Paused on a metered connection");
    const QString message = count == 1 ? QStringLiteral("1 download will continue once you're off this connection.")
                                       : QStringLiteral("%1 downloads will continue once you're off this connection.").arg(count);
    ORBIT_LOG_INFO("tray", "Notification: " + title.toStdString());
#if defined(__linux__)
    if (desktopNotifications_ && desktopNotifications_->available()) {
        desktopNotifications_->notify(title, message, false, QStringLiteral("network"),
                                      {DesktopNotifications::Action{QStringLiteral("resume"), QStringLiteral("Download Anyway")}},
                                      QStringLiteral("metered"));
        return;
    }
#endif
    showMessage(title, message, false, 6000);
}

void TrayController::setProgress(double progress, const QColor& color) {
    // 2% steps: smooth enough to watch, without redrawing the icon every second.
    const int step = progress < 0 ? -1 : static_cast<int>(std::clamp(progress, 0.0, 1.0) * 50.0 + 0.5);
#if defined(__linux__)
    if (step != launcherStep_) {
        // The same progress on Orbit's taskbar or dock entry (KDE's task manager, Dash to
        // Dock, Plank and others listen for this signal).
        launcherStep_ = step;
        QDBusMessage update = QDBusMessage::createSignal(QStringLiteral("/com/canonical/unity/launcherentry/orbit"),
                                                         QStringLiteral("com.canonical.Unity.LauncherEntry"),
                                                         QStringLiteral("Update"));
        update << QStringLiteral("application://%1.desktop").arg(QGuiApplication::desktopFileName())
               << QVariantMap{{QStringLiteral("progress"), step < 0 ? 0.0 : step / 50.0},
                              {QStringLiteral("progress-visible"), step >= 0}};
        QDBusConnection::sessionBus().send(update);
    }
#endif
    if (!trayIcon_) return;
    if (step == shownProgressStep_ && color == shownProgressColor_) return;
    shownProgressStep_ = step;
    shownProgressColor_ = color;
    if (step < 0) {
        trayIcon_->setIcon(baseIcon_);
        return;
    }

    // Drawn large so trays on HiDPI screens scale it down rather than up.
    constexpr int kSize = 128;
    QPixmap canvas(kSize, kSize);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing);
    // The logo shrinks a little to make room for the ring around it.
    constexpr int kRing = 9;
    constexpr int kInset = kRing + 3;
    const QRect logoRect(kInset, kInset, kSize - 2 * kInset, kSize - 2 * kInset);
    painter.drawPixmap(logoRect, baseIcon_.pixmap(logoRect.size() * 2));
    const QRectF ringRect(kRing / 2.0 + 0.5, kRing / 2.0 + 0.5, kSize - kRing - 1, kSize - kRing - 1);
    QPen track(QColor(128, 128, 128, 80), kRing);
    painter.setPen(track);
    painter.drawEllipse(ringRect);
    QPen fill(color.isValid() ? color : QColor(0x5b, 0x6e, 0xf2), kRing, Qt::SolidLine, Qt::RoundCap);
    painter.setPen(fill);
    // Clockwise from twelve o'clock (Qt's angles run counter-clockwise in 1/16 degrees).
    painter.drawArc(ringRect, 90 * 16, -static_cast<int>(step / 50.0 * 360.0 * 16));
    painter.end();
    trayIcon_->setIcon(QIcon(canvas));
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
    showMessage(QStringLiteral("Download added from browser"), label, false, 3500);
}
