#pragma once

#include <QFont>
#include <QImage>
#include <QObject>
#include <QPoint>
#include <QString>

#include <atomic>
#include <memory>
#include <thread>

class SplashScreen final : public QObject {
    Q_OBJECT

public:
    explicit SplashScreen(const QString& version, QObject* parent = nullptr);
    ~SplashScreen() override;

    void show();
    void setStatus(const QString& text, qreal progress);
    void finish();
    void fadeOut();
    void dismiss();

signals:
    void revealRequested();

private:
    struct Shared;

    void run();
    void computePlacement();
    [[nodiscard]] QImage renderChrome(int width, int height) const;

    std::shared_ptr<Shared> shared_;
    std::thread thread_;
    std::atomic<bool> revealPosted_{false};
    QString version_;
    QFont baseFont_;
    QPoint nativeCenter_;
    qreal scale_{1.0};
};
