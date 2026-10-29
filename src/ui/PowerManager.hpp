#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#if defined(__linux__)
#include "LinuxIntegration.hpp"
#endif

class DownloadListModel;

// Keeps the PC (Windows, or Linux through logind / the desktop portal) awake while downloads run, and carries out the one-shot "when
// downloads finish" action (exit Orbit, sleep, hibernate, shut down).
class PowerManager final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool preventSleep READ preventSleep WRITE setPreventSleep NOTIFY preventSleepChanged)
    Q_PROPERTY(bool sleepBlocked READ sleepBlocked NOTIFY sleepBlockedChanged)
    Q_PROPERTY(QString whenDoneAction READ whenDoneAction WRITE setWhenDoneAction NOTIFY whenDoneActionChanged)
    Q_PROPERTY(bool hibernateAvailable READ hibernateAvailable CONSTANT)

public:
    explicit PowerManager(DownloadListModel& model, QObject* parent = nullptr);
    ~PowerManager() override;

    [[nodiscard]] bool preventSleep() const noexcept { return preventSleep_; }
    void setPreventSleep(bool enabled);
    [[nodiscard]] bool sleepBlocked() const noexcept { return sleepBlocked_; }

    // "none", "exit", "sleep", "hibernate" or "shutdown".
    [[nodiscard]] QString whenDoneAction() const { return whenDoneAction_; }
    void setWhenDoneAction(const QString& action);
    [[nodiscard]] bool hibernateAvailable() const;

    Q_INVOKABLE QString actionLabel(const QString& action) const;
    Q_INVOKABLE void executeAction(const QString& action);

signals:
    void preventSleepChanged();
    void sleepBlockedChanged();
    void whenDoneActionChanged();
    // All downloads finished with an action armed; the UI shows a cancellable countdown
    // and then calls executeAction().
    void countdownRequested(const QString& action);
    void quitRequested();

private:
    void evaluate();
    void applySleepBlock(bool block);
    void refreshSleepBlock();
    void releasePowerRequest();

    DownloadListModel& model_;
    bool preventSleep_{true};
    bool sleepBlocked_{false};
    void* powerRequest_{nullptr};  // HANDLE from PowerCreateRequest on Windows
#if defined(__linux__)
    linux_integration::SleepInhibitor inhibitor_;
    bool hibernateAvailable_{false};
#endif
    QTimer refreshTimer_;
    QString whenDoneAction_{"none"};
    bool sawActivity_{false};
};
