#pragma once

#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

namespace orbit::core {
struct UpdateOffer;
}

class UpdateManager final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY releaseChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY releaseChanged)
    Q_PROPERTY(QString releaseDate READ releaseDate NOTIFY releaseChanged)
    Q_PROPERTY(QStringList releaseNotes READ releaseNotes NOTIFY releaseChanged)
    Q_PROPERTY(QString releasePage READ releasePage NOTIFY releaseChanged)
    Q_PROPERTY(double downloadSize READ downloadSize NOTIFY releaseChanged)
    Q_PROPERTY(double bytesReceived READ bytesReceived NOTIFY progressChanged)
    Q_PROPERTY(double downloadRate READ downloadRate NOTIFY progressChanged)
    Q_PROPERTY(QDateTime lastChecked READ lastChecked NOTIFY lastCheckedChanged)
    Q_PROPERTY(bool autoCheck READ autoCheck WRITE setAutoCheck NOTIFY autoCheckChanged)
    // Windows: a Linux-only release to tell the user about (nothing to install).
    Q_PROPERTY(QString linuxNewsVersion READ linuxNewsVersion NOTIFY linuxNewsFound)
    Q_PROPERTY(QStringList linuxNewsNotes READ linuxNewsNotes NOTIFY linuxNewsFound)
    Q_PROPERTY(QString linuxNewsDate READ linuxNewsDate NOTIFY linuxNewsFound)
    Q_PROPERTY(QString linuxNewsPage READ linuxNewsPage NOTIFY linuxNewsFound)

public:
    explicit UpdateManager(QObject* parent = nullptr);
    ~UpdateManager() override;

    [[nodiscard]] QString state() const { return state_; }
    [[nodiscard]] QString errorMessage() const { return errorMessage_; }
    [[nodiscard]] QString currentVersion() const;
    [[nodiscard]] bool updateAvailable() const { return !latestVersion_.isEmpty(); }
    [[nodiscard]] QString latestVersion() const { return latestVersion_; }
    [[nodiscard]] QString releaseDate() const { return releaseDate_; }
    [[nodiscard]] QStringList releaseNotes() const { return releaseNotes_; }
    [[nodiscard]] QString releasePage() const { return releasePage_; }
    [[nodiscard]] QString linuxNewsVersion() const { return linuxNewsVersion_; }
    [[nodiscard]] QStringList linuxNewsNotes() const { return linuxNewsNotes_; }
    [[nodiscard]] QString linuxNewsDate() const { return linuxNewsDate_; }
    [[nodiscard]] QString linuxNewsPage() const { return linuxNewsPage_; }
    // Called once the user has seen the Linux news, so it isn't shown again.
    Q_INVOKABLE void markLinuxNewsSeen();
    [[nodiscard]] double downloadSize() const { return static_cast<double>(downloadSize_); }
    [[nodiscard]] double bytesReceived() const { return static_cast<double>(bytesReceived_); }
    [[nodiscard]] double downloadRate() const { return downloadRate_; }
    [[nodiscard]] QDateTime lastChecked() const { return lastChecked_; }
    [[nodiscard]] bool autoCheck() const { return autoCheck_; }
    void setAutoCheck(bool enabled);

    void startAutomaticChecks();

    Q_INVOKABLE void checkNow();
    Q_INVOKABLE void downloadUpdate();
    Q_INVOKABLE void cancelDownload();
    Q_INVOKABLE void installUpdate();
    Q_INVOKABLE void skipThisVersion();
    Q_INVOKABLE bool openReleasePage() const;

signals:
    void stateChanged();
    void releaseChanged();
    void progressChanged();
    void lastCheckedChanged();
    void autoCheckChanged();
    void updateFound(bool userInitiated);
    void installerLaunched();
    void linuxNewsFound();

private:
    void check(bool userInitiated);
    void handleFeedReply(QNetworkReply* reply, bool userInitiated);
    void noteLinuxNews(const orbit::core::UpdateOffer& offer);
    void handleDownloadFinished();
    void setState(const QString& state, const QString& error = QString());
    void failDownload(const QString& message);
    void resetDownload();
    [[nodiscard]] QString installerPath() const;
    [[nodiscard]] bool verifyCachedInstaller();
    void removeStaleInstallers() const;
#if !defined(_WIN32)
    void installLinuxUpdate();
#endif
    [[nodiscard]] bool isAllowedDownloadUrl(const QUrl& url) const;
    // The Ed25519 key update offers must be signed with (32 raw bytes).
    [[nodiscard]] QByteArray releasePublicKey() const;

    QNetworkAccessManager* network_{nullptr};
    QPointer<QNetworkReply> feedReply_;
    QPointer<QNetworkReply> downloadReply_;
    std::unique_ptr<QFile> partFile_;
    std::unique_ptr<QCryptographicHash> hasher_;
    QTimer periodicTimer_;
    QTimer rateTimer_;
    QElapsedTimer rateClock_;
    qint64 rateBytesMark_{0};

    QUrl feedUrl_;
    bool feedOverridden_{false};
    QString state_{QStringLiteral("idle")};
    QString errorMessage_;
    QString latestVersion_;
    QString linuxNewsVersion_;
    QStringList linuxNewsNotes_;
    QString linuxNewsDate_;
    QString linuxNewsPage_;
    QString releaseDate_;
    QStringList releaseNotes_;
    QString releasePage_;
    QUrl installerUrl_;
    QByteArray expectedSha256_;
    qint64 downloadSize_{0};
    qint64 bytesReceived_{0};
    double downloadRate_{0.0};
    QDateTime lastChecked_;
    bool autoCheck_{true};
    QString announcedVersion_;
};
