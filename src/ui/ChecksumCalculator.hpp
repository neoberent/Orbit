#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <thread>

// Computes MD5, SHA-1 and SHA-256 of a file in one pass on a background thread, for the
// "Verify checksum" dialog.
class ChecksumCalculator : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString filePath READ filePath NOTIFY changed)
    Q_PROPERTY(QString md5 READ md5 NOTIFY changed)
    Q_PROPERTY(QString sha1 READ sha1 NOTIFY changed)
    Q_PROPERTY(QString sha256 READ sha256 NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)

public:
    explicit ChecksumCalculator(QObject* parent = nullptr);
    ~ChecksumCalculator() override;

    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] double progress() const noexcept { return progress_; }
    [[nodiscard]] QString filePath() const { return filePath_; }
    [[nodiscard]] QString md5() const { return md5_; }
    [[nodiscard]] QString sha1() const { return sha1_; }
    [[nodiscard]] QString sha256() const { return sha256_; }
    [[nodiscard]] QString error() const { return error_; }

    Q_INVOKABLE void start(const QString& path);
    Q_INVOKABLE void cancel();
    // "match", "mismatch", or "invalid" (not a recognisable digest) against the results.
    Q_INVOKABLE QString compare(const QString& expected) const;

signals:
    void changed();

private:
    void stopWorker();

    bool running_{false};
    double progress_{0};
    QString filePath_, md5_, sha1_, sha256_, error_;
    std::jthread worker_;
    std::shared_ptr<std::atomic<int>> generation_{std::make_shared<std::atomic<int>>(0)};
};
