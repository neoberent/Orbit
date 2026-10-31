#pragma once

#include "core/DownloadManager.hpp"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

#include <memory>

namespace orbit::core {
class RemoteServer;
class Scheduler;
}

class MainWindow final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool darkTheme READ darkTheme WRITE setDarkTheme NOTIFY darkThemeChanged)
    Q_PROPERTY(QString downloadDirectory READ downloadDirectory WRITE setDownloadDirectory NOTIFY downloadDirectoryChanged)
    Q_PROPERTY(int maxConnectionsPerDownload READ maxConnectionsPerDownload WRITE setMaxConnectionsPerDownload NOTIFY maxConnectionsPerDownloadChanged)
    Q_PROPERTY(int maxConcurrentDownloads READ maxConcurrentDownloads WRITE setMaxConcurrentDownloads NOTIFY maxConcurrentDownloadsChanged)
    Q_PROPERTY(int httpSpeedLimitKBps READ httpSpeedLimitKBps WRITE setHttpSpeedLimitKBps NOTIFY httpSpeedLimitKBpsChanged)
    // "ask", "rename" (keep both) or "overwrite": what a direct download does when its file already exists.
    Q_PROPERTY(QString fileConflictAction READ fileConflictAction WRITE setFileConflictAction NOTIFY fileConflictActionChanged)
    // The download the Refresh Link dialog is collecting a new link for, or 0.
    Q_PROPERTY(qlonglong linkCaptureTarget READ linkCaptureTarget NOTIFY linkCaptureTargetChanged)

    Q_PROPERTY(bool remoteAccessEnabled READ remoteAccessEnabled WRITE setRemoteAccessEnabled NOTIFY remoteAccessEnabledChanged)
    Q_PROPERTY(int remotePort READ remotePort WRITE setRemotePort NOTIFY remotePortChanged)
    Q_PROPERTY(QString remoteUsername READ remoteUsername NOTIFY remoteCredentialsChanged)
    Q_PROPERTY(bool remoteCredentialsConfigured READ remoteCredentialsConfigured NOTIFY remoteCredentialsChanged)
    Q_PROPERTY(bool remoteRunning READ remoteRunning NOTIFY remoteStatusChanged)
    Q_PROPERTY(QString remoteStatusMessage READ remoteStatusMessage NOTIFY remoteStatusChanged)
    Q_PROPERTY(QString remoteLocalUrl READ remoteLocalUrl NOTIFY remoteStatusChanged)
    // Host name(s) of a named Cloudflare tunnel, comma separated, that the dashboard answers to.
    Q_PROPERTY(QString remoteTunnelHost READ remoteTunnelHost WRITE setRemoteTunnelHost NOTIFY remoteTunnelHostChanged)

    Q_PROPERTY(bool scheduleEnabled READ scheduleEnabled WRITE setScheduleEnabled NOTIFY scheduleChanged)
    Q_PROPERTY(QString scheduleStartTime READ scheduleStartTime WRITE setScheduleStartTime NOTIFY scheduleChanged)
    Q_PROPERTY(QString scheduleEndTime READ scheduleEndTime WRITE setScheduleEndTime NOTIFY scheduleChanged)
    Q_PROPERTY(int scheduleDays READ scheduleDays WRITE setScheduleDays NOTIFY scheduleChanged)
    Q_PROPERTY(bool speedRuleEnabled READ speedRuleEnabled WRITE setSpeedRuleEnabled NOTIFY scheduleChanged)
    Q_PROPERTY(QString speedRuleStartTime READ speedRuleStartTime WRITE setSpeedRuleStartTime NOTIFY scheduleChanged)
    Q_PROPERTY(QString speedRuleEndTime READ speedRuleEndTime WRITE setSpeedRuleEndTime NOTIFY scheduleChanged)
    Q_PROPERTY(int speedRuleDays READ speedRuleDays WRITE setSpeedRuleDays NOTIFY scheduleChanged)
    Q_PROPERTY(int speedRuleLimitKBps READ speedRuleLimitKBps WRITE setSpeedRuleLimitKBps NOTIFY scheduleChanged)
    Q_PROPERTY(bool speedRuleActive READ speedRuleActive NOTIFY speedRuleActiveChanged)

    Q_PROPERTY(QString proxyType READ proxyType NOTIFY proxyChanged)
    Q_PROPERTY(QString proxyHost READ proxyHost NOTIFY proxyChanged)
    Q_PROPERTY(int proxyPort READ proxyPort NOTIFY proxyChanged)
    Q_PROPERTY(QString proxyUsername READ proxyUsername NOTIFY proxyChanged)
    Q_PROPERTY(bool proxyHasPassword READ proxyHasPassword NOTIFY proxyChanged)

    Q_PROPERTY(int torrentDownloadLimitKBps READ torrentDownloadLimitKBps WRITE setTorrentDownloadLimitKBps NOTIFY torrentSettingsChanged)
    Q_PROPERTY(int torrentUploadLimitKBps READ torrentUploadLimitKBps WRITE setTorrentUploadLimitKBps NOTIFY torrentSettingsChanged)
    Q_PROPERTY(double seedRatioLimit READ seedRatioLimit WRITE setSeedRatioLimit NOTIFY torrentSettingsChanged)
    Q_PROPERTY(int seedTimeLimitMinutes READ seedTimeLimitMinutes WRITE setSeedTimeLimitMinutes NOTIFY torrentSettingsChanged)

    Q_PROPERTY(QString mediaCookiesBrowser READ mediaCookiesBrowser WRITE setMediaCookiesBrowser NOTIFY mediaSettingsChanged)

    Q_PROPERTY(bool browserIntegrationEnabled READ browserIntegrationEnabled WRITE setBrowserIntegrationEnabled NOTIFY browserIntegrationChanged)
    Q_PROPERTY(bool nativeHostRegistered READ nativeHostRegistered NOTIFY browserIntegrationChanged)
    Q_PROPERTY(QString browserIntegrationStatus READ browserIntegrationStatus NOTIFY browserIntegrationChanged)
    Q_PROPERTY(QString chromeExtensionId READ chromeExtensionId CONSTANT)

    Q_PROPERTY(bool portable READ portable CONSTANT)

public:
    explicit MainWindow(orbit::core::DownloadManager& manager, bool portable, QObject* parent = nullptr);
    ~MainWindow() override;

    [[nodiscard]] bool darkTheme() const noexcept { return darkTheme_; }
    void setDarkTheme(bool dark);

    [[nodiscard]] QString downloadDirectory() const;
    void setDownloadDirectory(const QString& dir);

    [[nodiscard]] int maxConnectionsPerDownload() const noexcept { return maxConnectionsPerDownload_; }
    void setMaxConnectionsPerDownload(int n);

    [[nodiscard]] int maxConcurrentDownloads() const;
    void setMaxConcurrentDownloads(int n);

    [[nodiscard]] QString fileConflictAction() const { return fileConflictAction_; }
    void setFileConflictAction(const QString& action);

    // Refreshing an expired link. While a capture is running, the next http(s) link the
    // browser extension (or an orbit:// link) hands to Orbit is not added as a new download
    // but offered to the Refresh Link dialog through linkCaptured, together with the
    // browser's cookies for it.
    [[nodiscard]] qlonglong linkCaptureTarget() const noexcept { return linkCaptureTarget_; }
    Q_INVOKABLE void beginLinkCapture(qlonglong id);
    Q_INVOKABLE void endLinkCapture();
    // Asks the server about a new link; the answer arrives through linkProbed(token, result)
    // with result = {ok, error, size, sizeText, supportsRanges, fileName}.
    Q_INVOKABLE int probeLink(qlonglong id, const QString& url);
    // Returns an error message, or an empty string once the download is queued again.
    Q_INVOKABLE QString replaceLink(qlonglong id, const QString& url, bool startOver);

    // 0 = unlimited. Combined cap across every active HTTP download.
    [[nodiscard]] int httpSpeedLimitKBps() const noexcept { return httpSpeedLimitKBps_; }
    void setHttpSpeedLimitKBps(int kbps);

    [[nodiscard]] bool remoteAccessEnabled() const noexcept { return remoteAccessEnabled_; }
    void setRemoteAccessEnabled(bool enabled);
    [[nodiscard]] int remotePort() const noexcept { return remotePort_; }
    void setRemotePort(int port);
    [[nodiscard]] QString remoteUsername() const;
    [[nodiscard]] bool remoteCredentialsConfigured() const;
    [[nodiscard]] bool remoteRunning() const;
    [[nodiscard]] QString remoteStatusMessage() const { return remoteStatusMessage_; }
    [[nodiscard]] QString remoteLocalUrl() const;
    [[nodiscard]] QString remoteTunnelHost() const { return remoteTunnelHost_; }
    void setRemoteTunnelHost(const QString& hosts);

    Q_INVOKABLE bool setRemoteCredentials(const QString& username, const QString& password);

    [[nodiscard]] bool scheduleEnabled() const noexcept { return scheduleEnabled_; }
    void setScheduleEnabled(bool enabled);
    [[nodiscard]] QString scheduleStartTime() const { return scheduleStartTime_; }
    void setScheduleStartTime(const QString& time);
    [[nodiscard]] QString scheduleEndTime() const { return scheduleEndTime_; }
    void setScheduleEndTime(const QString& time);
    [[nodiscard]] int scheduleDays() const noexcept { return scheduleDays_; }
    void setScheduleDays(int days);
    [[nodiscard]] bool speedRuleEnabled() const noexcept { return speedRuleEnabled_; }
    void setSpeedRuleEnabled(bool enabled);
    [[nodiscard]] QString speedRuleStartTime() const { return speedRuleStart_; }
    void setSpeedRuleStartTime(const QString& time);
    [[nodiscard]] QString speedRuleEndTime() const { return speedRuleEnd_; }
    void setSpeedRuleEndTime(const QString& time);
    [[nodiscard]] int speedRuleDays() const noexcept { return speedRuleDays_; }
    void setSpeedRuleDays(int days);
    [[nodiscard]] int speedRuleLimitKBps() const noexcept { return speedRuleLimitKBps_; }
    void setSpeedRuleLimitKBps(int kbps);
    [[nodiscard]] bool speedRuleActive() const noexcept { return speedRuleActive_; }

    [[nodiscard]] QString proxyType() const { return proxyType_; }
    [[nodiscard]] QString proxyHost() const { return proxyHost_; }
    [[nodiscard]] int proxyPort() const noexcept { return proxyPort_; }
    [[nodiscard]] QString proxyUsername() const { return proxyUsername_; }
    [[nodiscard]] bool proxyHasPassword() const { return !proxyPassword_.isEmpty(); }
    // type: "none", "http" or "socks5". An empty password keeps the saved one unless
    // clearPassword is set. Returns an error message, or an empty string.
    Q_INVOKABLE QString setProxy(const QString& type, const QString& host, int port, const QString& username,
                                 const QString& password, bool clearPassword);

    [[nodiscard]] int torrentDownloadLimitKBps() const noexcept { return torrentDownloadLimitKBps_; }
    void setTorrentDownloadLimitKBps(int kbps);
    [[nodiscard]] int torrentUploadLimitKBps() const noexcept { return torrentUploadLimitKBps_; }
    void setTorrentUploadLimitKBps(int kbps);
    [[nodiscard]] double seedRatioLimit() const noexcept { return seedRatioLimit_; }
    void setSeedRatioLimit(double ratio);
    [[nodiscard]] int seedTimeLimitMinutes() const noexcept { return seedTimeLimitMinutes_; }
    void setSeedTimeLimitMinutes(int minutes);

    [[nodiscard]] QString mediaCookiesBrowser() const { return mediaCookiesBrowser_; }
    void setMediaCookiesBrowser(const QString& browser);

    [[nodiscard]] bool browserIntegrationEnabled() const noexcept { return browserIntegrationEnabled_; }
    void setBrowserIntegrationEnabled(bool enabled);
    [[nodiscard]] bool nativeHostRegistered() const;
    [[nodiscard]] QString browserIntegrationStatus() const { return browserIntegrationStatus_; }
    [[nodiscard]] QString chromeExtensionId() const;
    Q_INVOKABLE void repairBrowserIntegration();

    [[nodiscard]] bool portable() const noexcept { return portable_; }

    Q_INVOKABLE QString pickDownloadDirectory();
    Q_INVOKABLE QString pickFolder(const QString& title, const QString& startDir);
    Q_INVOKABLE QString pickTorrentFile();
    Q_INVOKABLE QString pickBackupFileToSave();
    Q_INVOKABLE QString pickBackupFileToOpen();
    Q_INVOKABLE bool isMagnetOrUrl(const QString& text) const;

    Q_INVOKABLE QString toLocalFilePath(const QUrl& url) const { return url.toLocalFile(); }
    Q_INVOKABLE bool openDataFolder() const;
    // Opens the file's folder with the file selected.
    Q_INVOKABLE bool revealFile(const QString& path) const;
    Q_INVOKABLE QString dataFolderPath() const;

    // Adds a download handed to Orbit from outside the app (browser extension via the
    // orbit:// protocol, or another launched instance). A plain link or magnet starts
    // immediately (externalDownloadAdded); a .torrent file is routed to the Add Download
    // dialog instead (externalTorrentNeedsReview) so the user can still pick which files
    // to fetch rather than everything starting blind. Returns the source, or an empty
    // string if it was rejected.
    Q_INVOKABLE QString addFromExternalSource(const QString& source);

    // Adds links offered by the clipboard monitor with the default settings.
    Q_INVOKABLE int addLinks(const QStringList& links);

    // Handles a request from the local API (the browser extension's native host).
    QJsonObject handleApiRequest(const QJsonObject& request);

    // Writes settings (and optionally the download list) to a JSON file. Passwords and
    // browser cookies are never included. Returns an error message, or an empty string.
    Q_INVOKABLE QString exportBackup(const QString& path, bool includeDownloads);
    // Returns {ok, error, settings, downloads}: how many settings and downloads were restored.
    Q_INVOKABLE QVariantMap importBackup(const QString& path, bool includeDownloads);
    Q_INVOKABLE void restartApp();

signals:
    void darkThemeChanged();
    void downloadDirectoryChanged();
    void maxConnectionsPerDownloadChanged();
    void maxConcurrentDownloadsChanged();
    void httpSpeedLimitKBpsChanged();
    void externalDownloadAdded(const QString& label);
    void externalTorrentNeedsReview(const QString& source);
    void fileConflictActionChanged();
    void linkCaptureTargetChanged();
    void linkCaptured(qlonglong id, const QString& url, bool withBrowserSession);
    void linkProbed(int token, const QVariantMap& result);

    void remoteAccessEnabledChanged();
    void remotePortChanged();
    void remoteTunnelHostChanged();
    void remoteCredentialsChanged();
    void remoteStatusChanged();
    void scheduleChanged();
    void speedRuleActiveChanged();
    void proxyChanged();
    void torrentSettingsChanged();
    void mediaSettingsChanged();
    void browserIntegrationChanged();
    void quitRequested();

private:
    void applyRemoteServerState();
    void applyProxy();
    void applySpeedLimits();
    void applySchedule();
    void applyTorrentSettings();
    orbit::core::DownloadId addLink(const QString& link, orbit::core::DownloadRequest request);
    // Hands an incoming link to a running Refresh Link capture. False when none is running.
    bool offerToLinkCapture(const QString& url, const orbit::core::DownloadRequest& context);
    [[nodiscard]] orbit::core::DownloadRequest captureContextFor(const QString& url) const;

    orbit::core::DownloadManager& manager_;
    bool portable_{false};
    bool darkTheme_{true};
    int maxConnectionsPerDownload_{8};
    int httpSpeedLimitKBps_{0};

    std::unique_ptr<orbit::core::RemoteServer> remoteServer_;
    bool remoteAccessEnabled_{false};
    int remotePort_{8765};
    QString remoteTunnelHost_;
    QString remoteStatusMessage_;

    std::unique_ptr<orbit::core::Scheduler> scheduler_;
    bool scheduleEnabled_{false};
    QString scheduleStartTime_{"22:00"};
    QString scheduleEndTime_{"07:00"};
    int scheduleDays_{0x7f};
    bool speedRuleEnabled_{false};
    QString speedRuleStart_{"09:00"};
    QString speedRuleEnd_{"18:00"};
    int speedRuleDays_{0x1f};
    int speedRuleLimitKBps_{512};
    bool speedRuleActive_{false};

    QString proxyType_{"none"};
    QString proxyHost_;
    int proxyPort_{0};
    QString proxyUsername_;
    QString proxyPassword_;

    int torrentDownloadLimitKBps_{0};
    int torrentUploadLimitKBps_{0};
    double seedRatioLimit_{0};
    int seedTimeLimitMinutes_{0};

    QString mediaCookiesBrowser_;
    QString fileConflictAction_{QStringLiteral("ask")};
    qlonglong linkCaptureTarget_{0};
    // The link and browser session captured for linkCaptureTarget_.
    QString capturedUrl_;
    orbit::core::DownloadRequest capturedContext_;
    int nextProbeToken_{1};

    bool browserIntegrationEnabled_{true};
    QString browserIntegrationStatus_;
};
