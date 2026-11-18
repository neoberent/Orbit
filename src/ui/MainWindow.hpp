#pragma once

#include "core/DownloadManager.hpp"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

#include <memory>
#include <unordered_map>
#include <vector>

class QTimer;

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
    Q_PROPERTY(bool seedAfterDownload READ seedAfterDownload WRITE setSeedAfterDownload NOTIFY torrentSettingsChanged)
    Q_PROPERTY(bool noCowOnBtrfs READ noCowOnBtrfs WRITE setNoCowOnBtrfs NOTIFY torrentSettingsChanged)
    Q_PROPERTY(int torrentListenPort READ torrentListenPort WRITE setTorrentListenPort NOTIFY torrentSettingsChanged)
    Q_PROPERTY(bool addPublicTrackers READ addPublicTrackers WRITE setAddPublicTrackers NOTIFY torrentSettingsChanged)
    // Failed downloads (dropped connections, timeouts, 5xx) try again by themselves this
    // many times, this many seconds apart. 0 = off.
    Q_PROPERTY(int autoRetryCount READ autoRetryCount WRITE setAutoRetryCount NOTIFY autoRetryChanged)
    Q_PROPERTY(int autoRetryDelaySeconds READ autoRetryDelaySeconds WRITE setAutoRetryDelaySeconds NOTIFY autoRetryChanged)
    Q_PROPERTY(bool downloadsOnBtrfs READ downloadsOnBtrfs NOTIFY downloadDirectoryChanged)

    Q_PROPERTY(QString mediaCookiesBrowser READ mediaCookiesBrowser WRITE setMediaCookiesBrowser NOTIFY mediaSettingsChanged)

    Q_PROPERTY(bool browserIntegrationEnabled READ browserIntegrationEnabled WRITE setBrowserIntegrationEnabled NOTIFY browserIntegrationChanged)
    Q_PROPERTY(bool nativeHostRegistered READ nativeHostRegistered NOTIFY browserIntegrationChanged)
    Q_PROPERTY(QString browserIntegrationStatus READ browserIntegrationStatus NOTIFY browserIntegrationChanged)
    Q_PROPERTY(QString chromeExtensionId READ chromeExtensionId CONSTANT)

    // aria2's JSON-RPC interface for add-ons and apps written for aria2 (off by default).
    Q_PROPERTY(bool aria2Enabled READ aria2Enabled WRITE setAria2Enabled NOTIFY aria2Changed)
    Q_PROPERTY(int aria2Port READ aria2Port WRITE setAria2Port NOTIFY aria2Changed)
    Q_PROPERTY(QString aria2Secret READ aria2Secret NOTIFY aria2Changed)
    Q_PROPERTY(QString aria2Status READ aria2Status NOTIFY aria2Changed)

    Q_PROPERTY(bool portable READ portable CONSTANT)

    // Sort downloads headed for the default folder into one folder per type.
    Q_PROPERTY(bool sortByType READ sortByType WRITE setSortByType NOTIFY afterDownloadChanged)
    // {video, audio, image, document, archive, program} -> folder.
    Q_PROPERTY(QVariantMap typeFolders READ typeFolders NOTIFY afterDownloadChanged)
    // Unpack finished archives into a folder beside them.
    Q_PROPERTY(bool unpackArchives READ unpackArchives WRITE setUnpackArchives NOTIFY afterDownloadChanged)
    // A command run for every finished download; %f = the file, %d = its folder, %n = its name.
    Q_PROPERTY(QString afterCommand READ afterCommand WRITE setAfterCommand NOTIFY afterDownloadChanged)
    // Record each finished file's link and page as xattrs (Linux; file managers show them).
    Q_PROPERTY(bool rememberOrigin READ rememberOrigin WRITE setRememberOrigin NOTIFY afterDownloadChanged)

    // The network interface every download is bound to ("" = any), e.g. a VPN's wg0.
    Q_PROPERTY(QString networkInterface READ networkInterface WRITE setNetworkInterface NOTIFY networkInterfaceChanged)
    // Pauses downloads and seeding while the connection is metered (a phone hotspot, a
    // capped mobile plan), and resumes them once it isn't.
    Q_PROPERTY(bool pauseOnMetered READ pauseOnMetered WRITE setPauseOnMetered NOTIFY meteredChanged)
    Q_PROPERTY(bool meteredConnection READ meteredConnection NOTIFY meteredChanged)
    Q_PROPERTY(bool meteredDetectionAvailable READ meteredDetectionAvailable CONSTANT)
    // The network in use (NetworkManager's name for it) and the download limit that
    // applies only while on it (0 = none).
    Q_PROPERTY(QString currentNetwork READ currentNetwork NOTIFY networkLimitsChanged)
    Q_PROPERTY(int currentNetworkLimitKBps READ currentNetworkLimitKBps WRITE setCurrentNetworkLimitKBps NOTIFY networkLimitsChanged)

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
    // Checks a link that isn't a download yet; the answer arrives through linkInspected
    // with the same fields as linkProbed. Magnet and media links answer at once with
    // their kind and no size.
    Q_INVOKABLE int inspectLink(const QString& url);
    // An earlier download of the same link: {found, id, name, state, when}. Used to warn
    // before downloading something twice.
    Q_INVOKABLE QVariantMap previousDownloadOf(const QString& url) const;
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
    [[nodiscard]] bool seedAfterDownload() const noexcept { return seedAfterDownload_; }
    void setSeedAfterDownload(bool seed);
    [[nodiscard]] bool noCowOnBtrfs() const noexcept { return noCowOnBtrfs_; }
    [[nodiscard]] int torrentListenPort() const noexcept { return torrentListenPort_; }
    void setTorrentListenPort(int port);
    [[nodiscard]] bool addPublicTrackers() const noexcept { return addPublicTrackers_; }
    void setAddPublicTrackers(bool add);
    [[nodiscard]] int autoRetryCount() const noexcept { return autoRetryCount_; }
    void setAutoRetryCount(int count);
    [[nodiscard]] int autoRetryDelaySeconds() const noexcept { return autoRetryDelaySeconds_; }
    void setAutoRetryDelaySeconds(int seconds);
    // Called when a download ends; schedules another try if it failed for a reason a
    // later attempt can fix (see autoRetryCount).
    void retryIfWorthIt(qlonglong id);
    void setNoCowOnBtrfs(bool enabled);
    [[nodiscard]] bool downloadsOnBtrfs() const;
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

    [[nodiscard]] bool aria2Enabled() const noexcept { return aria2Enabled_; }
    void setAria2Enabled(bool enabled);
    [[nodiscard]] int aria2Port() const noexcept { return aria2Port_; }
    void setAria2Port(int port);
    [[nodiscard]] QString aria2Secret() const { return aria2Secret_; }
    [[nodiscard]] QString aria2Status() const { return aria2Status_; }
    Q_INVOKABLE void newAria2Secret();
    // How to install Cloudflare's cloudflared on this system (for Remote Access tunnels):
    // winget on Windows, the distribution's own package or Cloudflare's on Linux.
    Q_INVOKABLE QString cloudflaredInstallCommand() const;

    [[nodiscard]] bool sortByType() const noexcept { return sortByType_; }
    void setSortByType(bool enabled);
    [[nodiscard]] QVariantMap typeFolders() const { return typeFolders_; }
    Q_INVOKABLE void setTypeFolder(const QString& type, const QString& folder);
    [[nodiscard]] bool unpackArchives() const noexcept { return unpackArchives_; }
    void setUnpackArchives(bool enabled);
    [[nodiscard]] QString afterCommand() const { return afterCommand_; }
    void setAfterCommand(const QString& command);
    [[nodiscard]] bool rememberOrigin() const noexcept { return rememberOrigin_; }
    void setRememberOrigin(bool enabled);
    // Records where a download that just finished came from (see rememberOrigin).
    void tagOrigin(qlonglong id);
    // Runs the after-download actions for a download that just finished.
    void runAfterDownload(qlonglong id);

    // Without a window nobody can answer a question, so .torrent files open from outside
    // start with every file, and a file name that's taken gets a new name.
    void setHeadless(bool headless);
    // For the command line's feed commands.
    void setFeedWatcher(class FeedWatcher* watcher) { feedWatcher_ = watcher; }

    [[nodiscard]] QString networkInterface() const { return networkInterface_; }
    void setNetworkInterface(const QString& name);
    // [{name, label, up}] for the interfaces worth binding to (no loopback or container bridges).
    Q_INVOKABLE QVariantList networkInterfaces() const;

    [[nodiscard]] QString currentNetwork() const;
    [[nodiscard]] int currentNetworkLimitKBps() const;
    void setCurrentNetworkLimitKBps(int kbps);

    [[nodiscard]] bool pauseOnMetered() const noexcept { return pauseOnMetered_; }
    void setPauseOnMetered(bool enabled);
    [[nodiscard]] bool meteredConnection() const noexcept { return metered_; }
    [[nodiscard]] bool meteredDetectionAvailable() const noexcept { return meteredDetectionAvailable_; }
    // Resumes what the metered pause stopped and keeps going on this connection.
    Q_INVOKABLE void overrideMeteredPause();

    Q_INVOKABLE QString pickDownloadDirectory();
    Q_INVOKABLE QString pickFolder(const QString& title, const QString& startDir);
    Q_INVOKABLE QString pickTorrentFile();
    Q_INVOKABLE QString pickBackupFileToSave();
    Q_INVOKABLE QString pickBackupFileToOpen();
    Q_INVOKABLE bool isMagnetOrUrl(const QString& text) const;

    Q_INVOKABLE QString toLocalFilePath(const QUrl& url) const { return url.toLocalFile(); }
    // file:// URL of a local path, properly encoded (for drag and drop).
    Q_INVOKABLE QString fileUrl(const QString& path) const { return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded); }
    // Starts dragging a finished file out of Orbit (into a chat, an upload field, the file
    // manager). Returns once it was dropped or let go.
    Q_INVOKABLE void startFileDrag(const QString& path);
    // True while one of Orbit's own files is being dragged out, so Orbit's window doesn't
    // offer itself as the place to drop it.
    Q_PROPERTY(bool draggingOut READ draggingOut NOTIFY draggingOutChanged)
    [[nodiscard]] bool draggingOut() const noexcept { return draggingOut_; }
    Q_INVOKABLE bool openDataFolder() const;
    // Opens the file's folder with the file selected.
    Q_INVOKABLE bool revealFile(const QString& path) const;
    Q_INVOKABLE QString dataFolderPath() const;
    // Asks the compositor to blur the desktop behind a translucent window (KDE Plasma's
    // hint; compositors without it ignore it, Hyprland blurs translucent windows anyway).
    Q_INVOKABLE void setBlurBehind(QObject* window, bool enabled) const;
    // Opens a video or song that's still downloading in a player (mpv, VLC or Celluloid if
    // installed, else the default app). A torrent switches to downloading in order first,
    // so playback can keep up. Returns an error message, or an empty string.
    Q_INVOKABLE QString playWhileDownloading(qlonglong id);

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
    // Handles a request from the `orbit` command line (its own socket, which the browser
    // helper can't reach): add, list, pause, resume, cancel, remove, show, status.
    QJsonObject handleControlRequest(const QJsonObject& request);
    // `orbit config list|get|set`: the settings Settings in the window changes.
    QJsonObject handleConfigRequest(const QJsonObject& request);

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
    void linkInspected(int token, const QVariantMap& result);

    void remoteAccessEnabledChanged();
    void remotePortChanged();
    void remoteTunnelHostChanged();
    void remoteCredentialsChanged();
    void remoteStatusChanged();
    void scheduleChanged();
    void speedRuleActiveChanged();
    void proxyChanged();
    void torrentSettingsChanged();
    void autoRetryChanged();
    void mediaSettingsChanged();
    void browserIntegrationChanged();
    void quitRequested();
    void networkInterfaceChanged();
    void meteredChanged();
    void networkLimitsChanged();
    void afterDownloadChanged();
    void draggingOutChanged();
    void aria2Changed();
    // An after-download action finished: ok, and a message for the user.
    void afterDownloadNotice(const QString& title, const QString& message, bool ok);
    // count downloads were paused because the connection became metered.
    void meteredPaused(int count);
    void showWindowRequested();
    // `orbit new` (the launcher's "New Download" action): open the add dialog.
    void addDialogRequested();

private:
    void applyRemoteServerState();
    void applyProxy();
    void applySpeedLimits();
    void applySchedule();
    void applyTorrentSettings();
    void setupMeteredDetection();
    void applyMeteredState();
    void watchMeteredPauses();
    void retryAfterReconnect();
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
    bool seedAfterDownload_{false};
    bool noCowOnBtrfs_{true};
    int torrentListenPort_{51413};
    bool addPublicTrackers_{true};
    int autoRetryCount_{3};
    int autoRetryDelaySeconds_{30};

    QString mediaCookiesBrowser_;
    QString fileConflictAction_{QStringLiteral("ask")};
    qlonglong linkCaptureTarget_{0};
    // The link and browser session captured for linkCaptureTarget_.
    QString capturedUrl_;
    orbit::core::DownloadRequest capturedContext_;
    int nextProbeToken_{1};

    bool browserIntegrationEnabled_{true};

    bool headless_{false};
    class FeedWatcher* feedWatcher_{nullptr};
    void applyTypeFolders();

    void applyAria2();
    class Aria2RpcServer* aria2_{nullptr};
    bool aria2Enabled_{false};
    int aria2Port_{6800};
    QString aria2Secret_;
    QString aria2Status_;
    bool draggingOut_{false};
    bool sortByType_{false};
    QVariantMap typeFolders_;
    bool unpackArchives_{false};
    bool rememberOrigin_{true};
    QString afterCommand_;
    QString networkInterface_;
    bool pauseOnMetered_{true};
    bool metered_{false};
    bool meteredDetectionAvailable_{false};
    bool meteredOverridden_{false};
    std::vector<orbit::core::DownloadId> meteredPausedIds_;
    QTimer* meteredWatch_{nullptr};
    class NetworkIdentity* networkIdentity_{nullptr};
    // Download limits per network name, in KB/s.
    QVariantMap networkLimits_;
    std::unordered_map<orbit::core::DownloadId, int> autoRetries_;
    QString browserIntegrationStatus_;
};
