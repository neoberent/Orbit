#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

// Keeps yt-dlp current. Video sites change often and an old yt-dlp stops working with
// them, so Orbit fetches new releases from yt-dlp's GitHub page into its own data folder
// (tools/) and uses whichever copy is newer: that one or the one bundled with Orbit.
// Every download is checked against the release's published SHA-256 list and must run
// before it replaces the working copy.
class YtDlpUpdater final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion NOTIFY versionChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY versionChanged)
    Q_PROPERTY(bool usingUpdatedCopy READ usingUpdatedCopy NOTIFY versionChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QDateTime lastChecked READ lastChecked NOTIFY lastCheckedChanged)
    Q_PROPERTY(bool autoUpdate READ autoUpdate WRITE setAutoUpdate NOTIFY autoUpdateChanged)

public:
    explicit YtDlpUpdater(QObject* parent = nullptr);
    ~YtDlpUpdater() override;

    // "detecting", "idle", "checking", "downloading", "installing", "upToDate", "updated", "error"
    [[nodiscard]] QString state() const { return state_; }
    [[nodiscard]] QString errorMessage() const { return errorMessage_; }
    [[nodiscard]] QString currentVersion() const { return currentVersion_; }
    [[nodiscard]] QString latestVersion() const { return latestVersion_; }
    [[nodiscard]] bool usingUpdatedCopy() const { return usingManaged_; }
    [[nodiscard]] double progress() const { return progress_; }
    [[nodiscard]] QDateTime lastChecked() const { return lastChecked_; }
    [[nodiscard]] bool autoUpdate() const { return autoUpdate_; }
    void setAutoUpdate(bool enabled);

    // Finds the installed versions, then checks on a schedule while automatic updates are on.
    void start();

    Q_INVOKABLE void checkNow();

    // A media download failed. yt-dlp being out of date is the usual cause, so an automatic
    // check runs if the last one isn't recent.
    void noteMediaFailure();

signals:
    void stateChanged();
    void versionChanged();
    void progressChanged();
    void lastCheckedChanged();
    void autoUpdateChanged();
    void updated(const QString& version);

private:
    void check(bool userInitiated);
    void handleRelease(QNetworkReply* reply, bool userInitiated);
    void fetchChecksums(const QUrl& sumsUrl, const QUrl& binaryUrl);
    void downloadBinary(const QUrl& url, const QByteArray& expectedSha256);
    void handleDownloadFinished();
    void installDownloaded();
    void detectVersions();
    void chooseCopy(const QString& bundledVersion, const QString& managedVersion);
    void setState(const QString& state, const QString& error = QString());
    void fail(const QString& message);
    void abortTransfers();
    void markChecked();
    void cleanupLeftovers() const;
    [[nodiscard]] bool isDue(qint64 maxAgeSecs) const;
    [[nodiscard]] bool isAllowedUrl(const QUrl& url) const;
    [[nodiscard]] QString toolsDir() const;
    [[nodiscard]] QString managedPath() const;
    [[nodiscard]] QString stagingPath() const;

    // Runs `<exe> --version` and hands back the version it prints, or "" on failure.
    void queryVersion(const QString& exe, std::function<void(const QString&)> done);

    QNetworkAccessManager* network_{nullptr};
    QPointer<QNetworkReply> reply_;
    std::unique_ptr<QFile> stagingFile_;
    std::unique_ptr<QCryptographicHash> hasher_;
    QByteArray expectedSha256_;
    QTimer periodicTimer_;

    QUrl feedUrl_;
    bool feedOverridden_{false};
    QString state_{QStringLiteral("detecting")};
    QString errorMessage_;
    QString currentVersion_;
    QString latestVersion_;
    bool usingManaged_{false};
    double progress_{0.0};
    QDateTime lastChecked_;
    bool autoUpdate_{true};
    bool userInitiated_{false};
};
