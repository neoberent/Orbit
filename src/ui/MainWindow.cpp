#include "MainWindow.hpp"

#include "BrowserIntegration.hpp"
#include "core/ProtocolHandler.hpp"
#include "core/RemoteServer.hpp"
#include "core/Scheduler.hpp"
#include "core/TorrentEngine.hpp"
#include "core/MediaDownloader.hpp"
#include "utils/FileUtils.hpp"
#include "utils/Logger.hpp"
#include "utils/StringUtils.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QPointer>
#include <QProcess>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

#if defined(_WIN32)
#include <windows.h>
#include <dpapi.h>
#elif defined(__linux__)
#include "LinuxIntegration.hpp"
#endif

namespace {

// Encrypts a secret for the current user (DPAPI on Windows, a per-user key on Linux), so a
// copied settings file or registry export doesn't reveal it.
QString protectSecret(const QString& plain) {
    if (plain.isEmpty()) return {};
#if defined(_WIN32)
    QByteArray input = plain.toUtf8();
    DATA_BLOB in{static_cast<DWORD>(input.size()), reinterpret_cast<BYTE*>(input.data())};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Orbit", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    const QByteArray encrypted(reinterpret_cast<const char*>(out.pbData), static_cast<qsizetype>(out.cbData));
    LocalFree(out.pbData);
    return QString::fromLatin1(encrypted.toBase64());
#elif defined(__linux__)
    return linux_integration::protectSecret(plain);
#else
    return QString::fromLatin1(plain.toUtf8().toBase64());
#endif
}

QString unprotectSecret(const QString& stored) {
    if (stored.isEmpty()) return {};
#if defined(__linux__)
    return linux_integration::unprotectSecret(stored);
#endif
    QByteArray encrypted = QByteArray::fromBase64(stored.toLatin1());
#if defined(_WIN32)
    DATA_BLOB in{static_cast<DWORD>(encrypted.size()), reinterpret_cast<BYTE*>(encrypted.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    const QString plain = QString::fromUtf8(reinterpret_cast<const char*>(out.pbData), static_cast<qsizetype>(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
#else
    return QString::fromUtf8(encrypted);
#endif
}

bool isExcludedFromBackup(const QString& key) {
    static const QStringList kExcluded = {
        "remote/username", "remote/passwordHash", "remote/passwordSalt", "remote/iterations",
        "network/proxyPassword", "updates/lastRunVersion", "updates/lastChecked",
        "ui/windowX", "ui/windowY",
    };
    return kExcluded.contains(key);
}

bool isHttpUrl(const QString& text) {
    return text.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) ||
           text.startsWith(QLatin1String("https://"), Qt::CaseInsensitive);
}

std::vector<orbit::core::HttpCookie> cookiesFromJson(const QJsonArray& array) {
    std::vector<orbit::core::HttpCookie> cookies;
    for (const QJsonValue& value : array) {
        if (cookies.size() >= 300) break;
        const QJsonObject c = value.toObject();
        orbit::core::HttpCookie cookie;
        cookie.name = c.value("name").toString().toStdString();
        cookie.value = c.value("value").toString().toStdString();
        cookie.domain = c.value("domain").toString().toStdString();
        cookie.path = c.value("path").toString(QStringLiteral("/")).toStdString();
        cookie.secure = c.value("secure").toBool();
        cookie.includeSubdomains = !c.value("hostOnly").toBool(!cookie.domain.empty() && cookie.domain.front() == '.');
        const double expires = c.value("expirationDate").toDouble(0);
        cookie.expires = c.value("session").toBool(false) || expires <= 0 ? 0 : static_cast<std::int64_t>(std::floor(expires));
        if (cookie.name.empty() || cookie.domain.empty()) continue;
        cookies.push_back(std::move(cookie));
    }
    return cookies;
}

}

MainWindow::MainWindow(orbit::core::DownloadManager& manager, bool portable, QObject* parent)
    : QObject(parent), manager_(manager), portable_(portable) {
    QSettings settings;
    darkTheme_ = settings.value("ui/darkTheme", true).toBool();
    maxConnectionsPerDownload_ = settings.value("downloads/maxConnections", 8).toInt();

    const QString savedDir = settings.value("downloads/directory").toString();
    if (!savedDir.isEmpty()) {
        manager_.setDefaultDownloadDir(savedDir.toStdString());
    }
    manager_.setMaxConcurrentDownloads(settings.value("downloads/maxConcurrent", 3).toInt());
    fileConflictAction_ = settings.value("downloads/fileConflict", fileConflictAction_).toString();
    if (fileConflictAction_ != QLatin1String("rename") && fileConflictAction_ != QLatin1String("overwrite")) {
        fileConflictAction_ = QStringLiteral("ask");
    }
    manager_.setDefaultFileConflictAction(orbit::core::fileConflictActionFromString(fileConflictAction_.toStdString()));
    httpSpeedLimitKBps_ = settings.value("downloads/httpSpeedLimitKBps", 0).toInt();

    proxyType_ = settings.value("network/proxyType", "none").toString();
    proxyHost_ = settings.value("network/proxyHost").toString();
    proxyPort_ = settings.value("network/proxyPort", 0).toInt();
    proxyUsername_ = settings.value("network/proxyUsername").toString();
    proxyPassword_ = unprotectSecret(settings.value("network/proxyPassword").toString());
    applyProxy();

    torrentDownloadLimitKBps_ = settings.value("torrent/downloadLimitKBps", 0).toInt();
    torrentUploadLimitKBps_ = settings.value("torrent/uploadLimitKBps", 0).toInt();
    seedRatioLimit_ = settings.value("torrent/seedRatioLimit", 0.0).toDouble();
    seedTimeLimitMinutes_ = settings.value("torrent/seedTimeLimitMinutes", 0).toInt();
    applyTorrentSettings();

    mediaCookiesBrowser_ = settings.value("media/cookiesFromBrowser").toString();
    manager_.mediaDownloader().setCookiesFromBrowser(mediaCookiesBrowser_);

    remoteServer_ = std::make_unique<orbit::core::RemoteServer>(manager_, this);
    connect(remoteServer_.get(), &orbit::core::RemoteServer::errorOccurred, this, [this](const QString& msg) {
        remoteStatusMessage_ = "Couldn't start: " + msg;
        emit remoteStatusChanged();
    });

    remoteAccessEnabled_ = settings.value("remote/enabled", false).toBool();
    remotePort_ = settings.value("remote/port", 8765).toInt();
    setRemoteTunnelHost(settings.value("remote/tunnelHost").toString());
    const QString remoteUser = settings.value("remote/username").toString();
    const QString remoteHash = settings.value("remote/passwordHash").toString();
    const QString remoteSalt = settings.value("remote/passwordSalt").toString();
    const int remoteIterations = settings.value("remote/iterations", 0).toInt();
    if (!remoteUser.isEmpty() && !remoteHash.isEmpty() && !remoteSalt.isEmpty()) {
        remoteServer_->loadStoredCredentials(remoteUser, remoteHash, remoteSalt, remoteIterations);
    }
    applyRemoteServerState();

    scheduler_ = std::make_unique<orbit::core::Scheduler>(manager_, this);
    connect(scheduler_.get(), &orbit::core::Scheduler::speedRuleChanged, this, [this](bool active, int) {
        if (active != speedRuleActive_) {
            speedRuleActive_ = active;
            ORBIT_LOG_INFO("schedule", active ? "Scheduled speed limit is now in effect" : "Scheduled speed limit ended");
            emit speedRuleActiveChanged();
        }
        applySpeedLimits();
    });
    scheduleStartTime_ = settings.value("schedule/start", scheduleStartTime_).toString();
    scheduleEndTime_ = settings.value("schedule/end", scheduleEndTime_).toString();
    scheduleDays_ = settings.value("schedule/days", scheduleDays_).toInt();
    scheduleEnabled_ = settings.value("schedule/enabled", false).toBool();
    speedRuleEnabled_ = settings.value("schedule/speedRuleEnabled", false).toBool();
    speedRuleStart_ = settings.value("schedule/speedRuleStart", speedRuleStart_).toString();
    speedRuleEnd_ = settings.value("schedule/speedRuleEnd", speedRuleEnd_).toString();
    speedRuleDays_ = settings.value("schedule/speedRuleDays", speedRuleDays_).toInt();
    speedRuleLimitKBps_ = settings.value("schedule/speedRuleLimitKBps", speedRuleLimitKBps_).toInt();
    applySchedule();
    scheduler_->setEnabled(scheduleEnabled_);
    applySpeedLimits();

    browserIntegrationEnabled_ = settings.value("browser/integration", !portable_).toBool();
    if (browserIntegrationEnabled_) repairBrowserIntegration();
    else browserIntegrationStatus_ = QStringLiteral("Turned off");
}

MainWindow::~MainWindow() = default;

void MainWindow::setDarkTheme(bool dark) {
    if (darkTheme_ == dark) return;
    darkTheme_ = dark;
    QSettings().setValue("ui/darkTheme", dark);
    emit darkThemeChanged();
}

QString MainWindow::downloadDirectory() const {
    return QString::fromStdString(manager_.defaultDownloadDir().string());
}

void MainWindow::setDownloadDirectory(const QString& dir) {
    if (dir.isEmpty() || dir == downloadDirectory()) return;
    manager_.setDefaultDownloadDir(dir.toStdString());
    QSettings().setValue("downloads/directory", dir);
    emit downloadDirectoryChanged();
}

void MainWindow::setMaxConnectionsPerDownload(int n) {
    n = std::clamp(n, 1, 32);
    if (n == maxConnectionsPerDownload_) return;
    maxConnectionsPerDownload_ = n;
    QSettings().setValue("downloads/maxConnections", n);
    emit maxConnectionsPerDownloadChanged();
}

int MainWindow::maxConcurrentDownloads() const { return manager_.maxConcurrentDownloads(); }

void MainWindow::setMaxConcurrentDownloads(int n) {
    manager_.setMaxConcurrentDownloads(n);
    QSettings().setValue("downloads/maxConcurrent", n);
    emit maxConcurrentDownloadsChanged();
}

void MainWindow::setHttpSpeedLimitKBps(int kbps) {
    kbps = std::max(0, kbps);
    if (kbps == httpSpeedLimitKBps_) return;
    httpSpeedLimitKBps_ = kbps;
    QSettings().setValue("downloads/httpSpeedLimitKBps", kbps);
    applySpeedLimits();
    emit httpSpeedLimitKBpsChanged();
}

void MainWindow::applySpeedLimits() {
    auto capped = [this](int limitKBps) {
        if (!speedRuleActive_) return limitKBps;
        return limitKBps > 0 ? std::min(limitKBps, speedRuleLimitKBps_) : speedRuleLimitKBps_;
    };
    const int http = capped(httpSpeedLimitKBps_);
    const int torrentDown = capped(torrentDownloadLimitKBps_);
    manager_.setGlobalHttpSpeedLimit(http > 0 ? static_cast<std::uint64_t>(http) * 1024 : 0);
    manager_.torrentEngine().setDownloadRateLimit(torrentDown > 0 ? torrentDown * 1024 : 0);
    manager_.torrentEngine().setUploadRateLimit(torrentUploadLimitKBps_ > 0 ? torrentUploadLimitKBps_ * 1024 : 0);
}

QString MainWindow::pickDownloadDirectory() {
    const QString dir = QFileDialog::getExistingDirectory(
        nullptr, tr("Choose Download Directory"), downloadDirectory());
    if (!dir.isEmpty()) setDownloadDirectory(dir);
    return dir;
}

QString MainWindow::pickFolder(const QString& title, const QString& startDir) {
    return QFileDialog::getExistingDirectory(nullptr, title, startDir.isEmpty() ? downloadDirectory() : startDir);
}

QString MainWindow::pickBackupFileToSave() {
    const QString suggested = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                              QStringLiteral("/Orbit backup ") + QDate::currentDate().toString(Qt::ISODate) +
                              QStringLiteral(".json");
    return QFileDialog::getSaveFileName(nullptr, tr("Export Orbit Backup"), suggested, tr("Orbit backup (*.json)"));
}

QString MainWindow::pickBackupFileToOpen() {
    return QFileDialog::getOpenFileName(nullptr, tr("Import Orbit Backup"),
                                        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                        tr("Orbit backup (*.json);;All Files (*)"));
}

QString MainWindow::dataFolderPath() const {
    return QString::fromStdWString(orbit::utils::appDataDir().wstring());
}

bool MainWindow::openDataFolder() const {
    return QDesktopServices::openUrl(QUrl::fromLocalFile(dataFolderPath()));
}

bool MainWindow::revealFile(const QString& path) const {
    const QFileInfo info(path);
    if (!info.exists()) {
        return !info.absolutePath().isEmpty() && QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
    }
#if defined(_WIN32)
    QProcess explorer;
    explorer.setProgram(QStringLiteral("explorer.exe"));
    explorer.setNativeArguments(QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(info.absoluteFilePath())));
    if (explorer.startDetached()) return true;
#elif defined(__linux__)
    return linux_integration::revealInFileManager(info.absoluteFilePath());
#endif
    return QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
}

QString MainWindow::pickTorrentFile() {
    return QFileDialog::getOpenFileName(nullptr, tr("Open Torrent File"), QString(),
                                         tr("Torrent Files (*.torrent);;All Files (*)"));
}

bool MainWindow::isMagnetOrUrl(const QString& text) const {
    return orbit::utils::isMagnetUri(text.toStdString()) || isHttpUrl(text);
}

orbit::core::DownloadId MainWindow::addLink(const QString& link, orbit::core::DownloadRequest request) {
    request.source = link.toStdString();
    if (request.destinationDir.empty()) request.destinationDir = manager_.defaultDownloadDir().string();
    request.maxConnections = maxConnectionsPerDownload_;
    if (orbit::utils::isMagnetUri(request.source)) return manager_.addTorrent(std::move(request));
    if (orbit::utils::isMediaUrl(request.source)) return manager_.addMediaDownload(std::move(request));
    return manager_.addHttpDownload(std::move(request));
}

void MainWindow::setFileConflictAction(const QString& action) {
    const QString normalized = action == QLatin1String("rename") || action == QLatin1String("overwrite") ? action : QStringLiteral("ask");
    if (normalized == fileConflictAction_) return;
    fileConflictAction_ = normalized;
    manager_.setDefaultFileConflictAction(orbit::core::fileConflictActionFromString(normalized.toStdString()));
    QSettings().setValue("downloads/fileConflict", normalized);
    emit fileConflictActionChanged();
}

void MainWindow::beginLinkCapture(qlonglong id) {
    capturedUrl_.clear();
    capturedContext_ = {};
    if (linkCaptureTarget_ == id) return;
    linkCaptureTarget_ = id;
    emit linkCaptureTargetChanged();
}

void MainWindow::endLinkCapture() {
    capturedUrl_.clear();
    capturedContext_ = {};
    if (linkCaptureTarget_ == 0) return;
    linkCaptureTarget_ = 0;
    emit linkCaptureTargetChanged();
}

bool MainWindow::offerToLinkCapture(const QString& url, const orbit::core::DownloadRequest& context) {
    if (linkCaptureTarget_ == 0 || !isHttpUrl(url) || orbit::utils::isMediaUrl(url.toStdString())) return false;
    const auto snap = manager_.snapshot(static_cast<orbit::core::DownloadId>(linkCaptureTarget_));
    if (!snap || snap->type != orbit::core::DownloadType::Http) {
        endLinkCapture();
        return false;
    }
    capturedUrl_ = url;
    capturedContext_ = context;
    ORBIT_LOG_INFO("refresh", "Captured a new link for task " + std::to_string(linkCaptureTarget_) + " with " +
                                  std::to_string(context.cookies.size()) + " cookies");
    emit linkCaptured(linkCaptureTarget_, url, !context.cookies.empty());
    return true;
}

orbit::core::DownloadRequest MainWindow::captureContextFor(const QString& url) const {
    orbit::core::DownloadRequest context;
    if (!capturedUrl_.isEmpty() && capturedUrl_ == url) context = capturedContext_;
    context.source = url.toStdString();
    return context;
}

int MainWindow::probeLink(qlonglong id, const QString& rawUrl) {
    const int token = nextProbeToken_++;
    const QString url = rawUrl.trimmed();
    if (!isHttpUrl(url)) {
        QMetaObject::invokeMethod(this, [this, token] {
            emit linkProbed(token, QVariantMap{{"ok", false}, {"error", QStringLiteral("Enter an http:// or https:// link")}});
        }, Qt::QueuedConnection);
        return token;
    }
    QPointer<MainWindow> self(this);
    manager_.probeLink(static_cast<orbit::core::DownloadId>(id), captureContextFor(url),
                       [self, token](const orbit::core::RemoteProbe& probe) {
                           QVariantMap result{{"ok", probe.ok},
                                              {"error", QString::fromStdString(probe.error)},
                                              {"size", static_cast<double>(probe.size)},
                                              {"sizeText", probe.size > 0 ? QString::fromStdString(orbit::utils::formatBytes(probe.size))
                                                                          : QStringLiteral("Unknown")},
                                              {"supportsRanges", probe.supportsRanges},
                                              {"fileName", QString::fromStdString(probe.fileName)}};
                           QMetaObject::invokeMethod(qApp, [self, token, result] {
                               if (self) emit self->linkProbed(token, result);
                           }, Qt::QueuedConnection);
                       });
    return token;
}

QString MainWindow::replaceLink(qlonglong id, const QString& rawUrl, bool startOver) {
    const QString url = rawUrl.trimmed();
    const std::string error = manager_.replaceLink(static_cast<orbit::core::DownloadId>(id), captureContextFor(url), startOver);
    if (error.empty()) endLinkCapture();
    return QString::fromStdString(error);
}

QString MainWindow::addFromExternalSource(const QString& source) {
    const QString trimmed = source.trimmed();
    if (trimmed.isEmpty()) return QString();

    const bool isMagnet = orbit::utils::isMagnetUri(trimmed.toStdString());
    const bool isTorrentFile = !isMagnet && !isHttpUrl(trimmed) && trimmed.endsWith(QStringLiteral(".torrent"), Qt::CaseInsensitive);

    // Sources reaching this point can come from untrusted places (the orbit:// protocol
    // handler, drag-and-drop, command-line args), so only ever forward a magnet URI, a
    // local .torrent path, or an http(s) URL — never something like file:// or ftp://.
    if (!isMagnet && !isTorrentFile && !isMagnetOrUrl(trimmed)) {
        return QString();
    }

    if (isTorrentFile) {
        // A .torrent's file list should be reviewed before anything starts downloading,
        // so route it to the Add Download dialog (which can parse a local .torrent and
        // show its file list) instead of blind-adding every file in it.
        emit externalTorrentNeedsReview(trimmed);
        return trimmed;
    }

    if (offerToLinkCapture(trimmed, {})) return trimmed;
    addLink(trimmed, {});
    emit externalDownloadAdded(trimmed);
    return trimmed;
}

int MainWindow::addLinks(const QStringList& links) {
    int added = 0;
    for (const QString& raw : links) {
        const QString link = raw.trimmed();
        if (!isMagnetOrUrl(link)) continue;
        addLink(link, {});
        ++added;
    }
    return added;
}

QJsonObject MainWindow::handleApiRequest(const QJsonObject& request) {
    const QString type = request.value("type").toString();
    if (type == QLatin1String("ping")) {
        return QJsonObject{{"ok", true},
                           {"app", QStringLiteral("Orbit Download Manager")},
                           {"version", QCoreApplication::applicationVersion()},
                           {"api", 1}};
    }
    if (type != QLatin1String("add")) return QJsonObject{{"ok", false}, {"error", "Unknown request"}};

    const QString url = request.value("url").toString().trimmed();
    if (!isMagnetOrUrl(url)) return QJsonObject{{"ok", false}, {"error", "Only http(s) and magnet links can be added"}};

    orbit::core::DownloadRequest download;
    const QString referrer = request.value("referrer").toString().trimmed();
    if (isHttpUrl(referrer)) download.referrer = referrer.left(4096).toStdString();
    download.userAgent = request.value("userAgent").toString().trimmed().left(512).toStdString();
    download.cookies = cookiesFromJson(request.value("cookies").toArray());

    QString fileName = request.value("filename").toString().trimmed();
    fileName = fileName.mid(std::max(fileName.lastIndexOf(QLatin1Char('/')), fileName.lastIndexOf(QLatin1Char('\\'))) + 1);
    if (!fileName.isEmpty()) {
        download.suggestedName = orbit::utils::sanitizeFileName(fileName.left(240).toStdString());
    }

    if (offerToLinkCapture(url, download)) {
        return QJsonObject{{"ok", true}, {"id", QString::number(linkCaptureTarget_)}, {"kind", "http"}, {"refreshed", true}};
    }

    const size_t cookieCount = download.cookies.size();
    const auto id = addLink(url, std::move(download));
    const auto snap = manager_.snapshot(id);
    const QString kind = !snap ? QStringLiteral("http")
                         : snap->type == orbit::core::DownloadType::Torrent ? QStringLiteral("torrent")
                         : snap->type == orbit::core::DownloadType::Media   ? QStringLiteral("media")
                                                                            : QStringLiteral("http");
    ORBIT_LOG_INFO("api", "Added " + kind.toStdString() + " download from the browser with " +
                              std::to_string(cookieCount) + " cookies");
    emit externalDownloadAdded(fileName.isEmpty() ? url : fileName);
    return QJsonObject{{"ok", true}, {"id", QString::number(static_cast<qulonglong>(id))}, {"kind", kind}};
}

void MainWindow::setRemoteAccessEnabled(bool enabled) {
    if (enabled == remoteAccessEnabled_) return;
    remoteAccessEnabled_ = enabled;
    QSettings().setValue("remote/enabled", enabled);
    emit remoteAccessEnabledChanged();
    applyRemoteServerState();
}

void MainWindow::setRemotePort(int port) {
    port = std::clamp(port, 1024, 65535);
    if (port == remotePort_) return;
    remotePort_ = port;
    QSettings().setValue("remote/port", port);
    emit remotePortChanged();
    if (remoteAccessEnabled_) applyRemoteServerState();
}

void MainWindow::setRemoteTunnelHost(const QString& hosts) {
    // Accepts "orbit.example.com", "https://orbit.example.com/" or several, comma separated.
    QStringList names;
    for (const QString& part : hosts.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts)) {
        const QString host = orbit::core::RemoteServer::normalizeHost(part);
        if (!host.isEmpty() && !names.contains(host)) names.append(host);
    }
    remoteServer_->setAllowedHosts(names);
    const QString joined = names.join(QStringLiteral(", "));
    if (joined == remoteTunnelHost_) return;
    remoteTunnelHost_ = joined;
    QSettings().setValue("remote/tunnelHost", joined);
    emit remoteTunnelHostChanged();
}

QString MainWindow::remoteUsername() const { return remoteServer_->username(); }

bool MainWindow::remoteCredentialsConfigured() const { return remoteServer_->hasCredentials(); }

bool MainWindow::remoteRunning() const { return remoteServer_->isRunning(); }

QString MainWindow::remoteLocalUrl() const { return QStringLiteral("http://127.0.0.1:%1").arg(remotePort_); }

bool MainWindow::setRemoteCredentials(const QString& username, const QString& password) {
    const QString trimmedUser = username.trimmed();
    if (trimmedUser.isEmpty() || password.size() < 8) return false;

    remoteServer_->setCredentials(trimmedUser, password);
    QSettings settings;
    settings.setValue("remote/username", trimmedUser);
    settings.setValue("remote/passwordHash", remoteServer_->passwordHashBase64());
    settings.setValue("remote/passwordSalt", remoteServer_->passwordSaltBase64());
    settings.setValue("remote/iterations", remoteServer_->passwordIterations());

    emit remoteCredentialsChanged();
    applyRemoteServerState();
    return true;
}

void MainWindow::applySchedule() {
    scheduler_->setWindow(scheduleStartTime_, scheduleEndTime_, scheduleDays_);
    scheduler_->setSpeedRule(speedRuleEnabled_, speedRuleStart_, speedRuleEnd_, speedRuleDays_, speedRuleLimitKBps_);
}

void MainWindow::setScheduleEnabled(bool enabled) {
    if (enabled == scheduleEnabled_) return;
    scheduleEnabled_ = enabled;
    scheduler_->setEnabled(enabled);
    QSettings().setValue("schedule/enabled", enabled);
    emit scheduleChanged();
}

void MainWindow::setScheduleStartTime(const QString& time) {
    if (time == scheduleStartTime_) return;
    scheduleStartTime_ = time;
    QSettings().setValue("schedule/start", time);
    applySchedule();
    emit scheduleChanged();
}

void MainWindow::setScheduleEndTime(const QString& time) {
    if (time == scheduleEndTime_) return;
    scheduleEndTime_ = time;
    QSettings().setValue("schedule/end", time);
    applySchedule();
    emit scheduleChanged();
}

void MainWindow::setScheduleDays(int days) {
    days &= 0x7f;
    if (days == scheduleDays_) return;
    scheduleDays_ = days;
    QSettings().setValue("schedule/days", days);
    applySchedule();
    emit scheduleChanged();
}

void MainWindow::setSpeedRuleEnabled(bool enabled) {
    if (enabled == speedRuleEnabled_) return;
    speedRuleEnabled_ = enabled;
    QSettings().setValue("schedule/speedRuleEnabled", enabled);
    applySchedule();
    emit scheduleChanged();
}

void MainWindow::setSpeedRuleStartTime(const QString& time) {
    if (time == speedRuleStart_) return;
    speedRuleStart_ = time;
    QSettings().setValue("schedule/speedRuleStart", time);
    applySchedule();
    emit scheduleChanged();
}

void MainWindow::setSpeedRuleEndTime(const QString& time) {
    if (time == speedRuleEnd_) return;
    speedRuleEnd_ = time;
    QSettings().setValue("schedule/speedRuleEnd", time);
    applySchedule();
    emit scheduleChanged();
}

void MainWindow::setSpeedRuleDays(int days) {
    days &= 0x7f;
    if (days == speedRuleDays_) return;
    speedRuleDays_ = days;
    QSettings().setValue("schedule/speedRuleDays", days);
    applySchedule();
    emit scheduleChanged();
}

void MainWindow::setSpeedRuleLimitKBps(int kbps) {
    kbps = std::max(1, kbps);
    if (kbps == speedRuleLimitKBps_) return;
    speedRuleLimitKBps_ = kbps;
    QSettings().setValue("schedule/speedRuleLimitKBps", kbps);
    applySchedule();
    emit scheduleChanged();
}

QString MainWindow::setProxy(const QString& type, const QString& host, int port, const QString& username,
                             const QString& password, bool clearPassword) {
    const QString normalizedType = (type == QLatin1String("http") || type == QLatin1String("socks5")) ? type : QStringLiteral("none");
    const QString trimmedHost = host.trimmed();
    if (normalizedType != QLatin1String("none")) {
        if (trimmedHost.isEmpty()) return QStringLiteral("Enter the proxy's host name or IP address");
        if (trimmedHost.contains(QLatin1Char('/')) || trimmedHost.contains(QLatin1Char(' '))) {
            return QStringLiteral("Enter just the host name, without http:// or a path");
        }
        if (port < 1 || port > 65535) return QStringLiteral("Enter a port between 1 and 65535");
    }

    proxyType_ = normalizedType;
    proxyHost_ = trimmedHost;
    proxyPort_ = port;
    proxyUsername_ = username.trimmed();
    if (clearPassword || proxyUsername_.isEmpty()) proxyPassword_.clear();
    else if (!password.isEmpty()) proxyPassword_ = password;

    QSettings settings;
    settings.setValue("network/proxyType", proxyType_);
    settings.setValue("network/proxyHost", proxyHost_);
    settings.setValue("network/proxyPort", proxyPort_);
    settings.setValue("network/proxyUsername", proxyUsername_);
    settings.setValue("network/proxyPassword", protectSecret(proxyPassword_));
    applyProxy();
    emit proxyChanged();
    return {};
}

void MainWindow::applyProxy() {
    orbit::core::ProxySettings proxy;
    proxy.type = proxyType_ == QLatin1String("socks5") ? orbit::core::ProxySettings::Type::Socks5
                 : proxyType_ == QLatin1String("http") ? orbit::core::ProxySettings::Type::Http
                                                       : orbit::core::ProxySettings::Type::None;
    proxy.host = proxyHost_.toStdString();
    proxy.port = proxyPort_;
    proxy.username = proxyUsername_.toStdString();
    proxy.password = proxyPassword_.toStdString();
    manager_.setProxy(proxy);

    // Orbit's own requests (update checks) follow the same proxy.
    if (proxy.enabled()) {
        QNetworkProxy appProxy(proxy.type == orbit::core::ProxySettings::Type::Socks5 ? QNetworkProxy::Socks5Proxy
                                                                                    : QNetworkProxy::HttpProxy,
                               proxyHost_, static_cast<quint16>(proxyPort_), proxyUsername_, proxyPassword_);
        QNetworkProxy::setApplicationProxy(appProxy);
        ORBIT_LOG_INFO("network", "Using " + proxyType_.toStdString() + " proxy " + proxyHost_.toStdString() + ":" +
                                      std::to_string(proxyPort_));
    } else {
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    }
}

void MainWindow::applyTorrentSettings() {
    manager_.torrentEngine().setSeedingLimits(seedRatioLimit_, seedTimeLimitMinutes_);
}

void MainWindow::setTorrentDownloadLimitKBps(int kbps) {
    kbps = std::max(0, kbps);
    if (kbps == torrentDownloadLimitKBps_) return;
    torrentDownloadLimitKBps_ = kbps;
    QSettings().setValue("torrent/downloadLimitKBps", kbps);
    applySpeedLimits();
    emit torrentSettingsChanged();
}

void MainWindow::setTorrentUploadLimitKBps(int kbps) {
    kbps = std::max(0, kbps);
    if (kbps == torrentUploadLimitKBps_) return;
    torrentUploadLimitKBps_ = kbps;
    QSettings().setValue("torrent/uploadLimitKBps", kbps);
    applySpeedLimits();
    emit torrentSettingsChanged();
}

void MainWindow::setSeedRatioLimit(double ratio) {
    ratio = std::max(0.0, std::round(ratio * 100.0) / 100.0);
    if (std::abs(ratio - seedRatioLimit_) < 0.001) return;
    seedRatioLimit_ = ratio;
    QSettings().setValue("torrent/seedRatioLimit", ratio);
    applyTorrentSettings();
    emit torrentSettingsChanged();
}

void MainWindow::setSeedTimeLimitMinutes(int minutes) {
    minutes = std::max(0, minutes);
    if (minutes == seedTimeLimitMinutes_) return;
    seedTimeLimitMinutes_ = minutes;
    QSettings().setValue("torrent/seedTimeLimitMinutes", minutes);
    applyTorrentSettings();
    emit torrentSettingsChanged();
}

void MainWindow::setMediaCookiesBrowser(const QString& browser) {
    static const QStringList kAllowed = {"", "firefox", "chrome", "edge", "brave", "opera", "vivaldi", "chromium"};
    const QString normalized = kAllowed.contains(browser) ? browser : QString();
    if (normalized == mediaCookiesBrowser_) return;
    mediaCookiesBrowser_ = normalized;
    QSettings().setValue("media/cookiesFromBrowser", normalized);
    manager_.mediaDownloader().setCookiesFromBrowser(normalized);
    emit mediaSettingsChanged();
}

void MainWindow::setBrowserIntegrationEnabled(bool enabled) {
    if (enabled == browserIntegrationEnabled_) return;
    browserIntegrationEnabled_ = enabled;
    QSettings().setValue("browser/integration", enabled);
    if (enabled) {
        repairBrowserIntegration();
    } else {
        browser_integration::unregisterNativeMessagingHost();
        browserIntegrationStatus_ = QStringLiteral("Turned off");
        emit browserIntegrationChanged();
    }
}

bool MainWindow::nativeHostRegistered() const { return browser_integration::isNativeMessagingHostRegistered(); }

QString MainWindow::chromeExtensionId() const { return QString::fromLatin1(browser_integration::kChromeExtensionId); }

void MainWindow::repairBrowserIntegration() {
#if defined(_WIN32)
    const std::string exePath = QCoreApplication::applicationFilePath().toStdString();
    orbit::core::protocol::registerWindowsProtocolHandler(exePath);
    orbit::core::protocol::registerWindowsTorrentFileAssociation(exePath);
#elif defined(__linux__)
    linux_integration::registerDesktopEntry(QCoreApplication::applicationFilePath());
#endif
    const QString error = browser_integration::registerNativeMessagingHost();
    browserIntegrationStatus_ = error.isEmpty() ? QStringLiteral("Ready for Chrome, Edge, Brave and Firefox") : error;
    if (!error.isEmpty()) ORBIT_LOG_WARN("browser", "Native messaging setup failed: " + error.toStdString());
    emit browserIntegrationChanged();
}

QString MainWindow::exportBackup(const QString& path, bool includeDownloads) {
    if (path.isEmpty()) return QStringLiteral("Choose where to save the backup");
    QSettings settings;
    QJsonObject values;
    for (const QString& key : settings.allKeys()) {
        if (isExcludedFromBackup(key)) continue;
        values.insert(key, QJsonValue::fromVariant(settings.value(key)));
    }

    QJsonObject root;
    root["format"] = QStringLiteral("orbit-backup");
    root["formatVersion"] = 1;
    root["appVersion"] = QCoreApplication::applicationVersion();
    root["created"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root["settings"] = values;
    if (includeDownloads) {
        QJsonArray downloads;
        for (const auto& record : manager_.exportRecords()) {
            QJsonObject task;
            for (const auto& [key, value] : record) {
                if (key != "id") task.insert(QString::fromStdString(key), QString::fromStdString(value));
            }
            downloads.append(task);
        }
        root["downloads"] = downloads;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return QStringLiteral("Couldn't write to ") + path;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) return QStringLiteral("Couldn't save the backup: ") + file.errorString();
    ORBIT_LOG_INFO("backup", "Exported settings" + std::string(includeDownloads ? " and downloads" : "") + " to " + path.toStdString());
    return {};
}

QVariantMap MainWindow::importBackup(const QString& path, bool includeDownloads) {
    QVariantMap result{{"ok", false}, {"settings", 0}, {"downloads", 0}};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result["error"] = QStringLiteral("Couldn't open ") + path;
        return result;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value("format").toString() != QLatin1String("orbit-backup")) {
        result["error"] = QStringLiteral("That file isn't an Orbit backup");
        return result;
    }

    QSettings settings;
    int restored = 0;
    const QJsonObject values = root.value("settings").toObject();
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (isExcludedFromBackup(it.key())) continue;
        settings.setValue(it.key(), it.value().toVariant());
        ++restored;
    }
    settings.sync();

    int downloads = 0;
    if (includeDownloads) {
        std::vector<orbit::core::DownloadManager::TaskRecord> records;
        for (const QJsonValue& value : root.value("downloads").toArray()) {
            const QJsonObject task = value.toObject();
            orbit::core::DownloadManager::TaskRecord record;
            for (auto it = task.begin(); it != task.end(); ++it) {
                if (it.key() == QLatin1String("cookie") || it.key() == QLatin1String("id")) continue;
                record.emplace_back(it.key().toStdString(), it.value().toString().toStdString());
            }
            records.push_back(std::move(record));
        }
        downloads = manager_.importRecords(records);
    }

    ORBIT_LOG_INFO("backup", "Imported " + std::to_string(restored) + " settings and " + std::to_string(downloads) +
                                 " downloads from " + path.toStdString());
    result["ok"] = true;
    result["settings"] = restored;
    result["downloads"] = downloads;
    return result;
}

void MainWindow::restartApp() {
    QStringList args{QStringLiteral("--wait-for-exit=") + QString::number(QCoreApplication::applicationPid())};
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), args)) {
        ORBIT_LOG_ERROR("main", "Couldn't start a new Orbit instance for the restart");
        return;
    }
    emit quitRequested();
}

void MainWindow::applyRemoteServerState() {
    if (!remoteAccessEnabled_) {
        remoteServer_->stop();
        remoteStatusMessage_ = QStringLiteral("Remote access is off");
        emit remoteStatusChanged();
        return;
    }
    if (!remoteServer_->hasCredentials()) {
        remoteServer_->stop();
        remoteStatusMessage_ = QStringLiteral("Set a username and password below to turn on remote access");
        emit remoteStatusChanged();
        return;
    }
    if (remoteServer_->start(static_cast<quint16>(remotePort_))) {
        remoteStatusMessage_ = QStringLiteral("Running at ") + remoteLocalUrl();
    } else {
        remoteStatusMessage_ = QStringLiteral("Couldn't start: ") + remoteServer_->lastError();
    }
    emit remoteStatusChanged();
}
