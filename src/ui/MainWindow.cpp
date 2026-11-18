#include "MainWindow.hpp"

#include "BrowserIntegration.hpp"
#include "FeedWatcher.hpp"
#include "Aria2RpcServer.hpp"
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
#include <QDrag>
#include <QIcon>
#include <QMimeData>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QMetaProperty>
#include <QJsonDocument>
#include <QLocale>
#include <QNetworkInformation>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QPointer>
#include <QProcess>
#include <QSaveFile>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#include <dpapi.h>
#elif defined(__linux__)
#include "LinuxIntegration.hpp"
#include "NetworkIdentity.hpp"
#endif

namespace {

// Interface names are short and use none of the separators curl and libtorrent parse
// (':' starts a port, ',' another interface); a name from an edited or restored settings
// file goes through the same check as one picked in Settings.
bool isValidInterfaceName(const QString& name) {
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_.-]{0,15}$"));
    return valid.match(name).hasMatch();
}


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
        // A command to run must never arrive from someone else's backup file, and the
        // aria2 token stays on this PC.
        "after/command", "aria2/secret",
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

    networkInterface_ = settings.value("network/interface").toString().trimmed();
    if (!isValidInterfaceName(networkInterface_)) {
        ORBIT_LOG_WARN("network", "Ignoring an invalid network interface name in the settings");
        networkInterface_.clear();
    }
    if (!networkInterface_.isEmpty()) manager_.setNetworkInterface(networkInterface_.toStdString());

    torrentDownloadLimitKBps_ = settings.value("torrent/downloadLimitKBps", 0).toInt();
    torrentUploadLimitKBps_ = settings.value("torrent/uploadLimitKBps", 0).toInt();
    seedRatioLimit_ = settings.value("torrent/seedRatioLimit", 0.0).toDouble();
    seedTimeLimitMinutes_ = settings.value("torrent/seedTimeLimitMinutes", 0).toInt();
    seedAfterDownload_ = settings.value("torrent/seedAfterDownload", false).toBool();
    noCowOnBtrfs_ = settings.value("torrent/noCowOnBtrfs", true).toBool();
    torrentListenPort_ = std::clamp(settings.value("torrent/listenPort", 51413).toInt(), 1024, 65535);
    addPublicTrackers_ = settings.value("torrent/addPublicTrackers", true).toBool();
    manager_.torrentEngine().setListenPort(torrentListenPort_);
    manager_.torrentEngine().setAddPublicTrackers(addPublicTrackers_);
    autoRetryCount_ = std::clamp(settings.value("downloads/autoRetryCount", 3).toInt(), 0, 20);
    autoRetryDelaySeconds_ = std::clamp(settings.value("downloads/autoRetryDelaySeconds", 30).toInt(), 5, 3600);
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

    {
        // Folders per type, defaulting to the desktop's own Videos, Music, ... folders.
        const QString downloads = downloadDirectory();
        const QVariantMap defaults{
            {"video", QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)},
            {"audio", QStandardPaths::writableLocation(QStandardPaths::MusicLocation)},
            {"image", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)},
            {"document", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)},
            {"archive", downloads + QStringLiteral("/Archives")},
            {"program", downloads + QStringLiteral("/Programs")},
        };
        const QVariantMap saved = settings.value("downloads/typeFolders").toMap();
        for (auto it = defaults.cbegin(); it != defaults.cend(); ++it) {
            const QString folder = saved.value(it.key()).toString();
            typeFolders_.insert(it.key(), folder.isEmpty() ? it.value() : folder);
        }
        sortByType_ = settings.value("downloads/sortByType", false).toBool();
        applyTypeFolders();
        unpackArchives_ = settings.value("after/unpackArchives", false).toBool();
        afterCommand_ = settings.value("after/command").toString();
    }

    aria2_ = new Aria2RpcServer(manager_, [this](const QStringList& links, const QString& directory, QString* error) -> qulonglong {
        const QString link = links.value(0);
        orbit::core::DownloadRequest request;
        for (const QString& mirror : links.mid(1)) {
            if (isHttpUrl(mirror)) request.mirrors.push_back(mirror.toStdString());
        }
        if (!directory.isEmpty()) {
            if (!QDir::isAbsolutePath(directory) || !QDir().mkpath(directory)) {
                *error = QStringLiteral("The folder must be an absolute path");
                return 0;
            }
            request.destinationDir = QDir::cleanPath(directory).toStdString();
        }
        if (link.endsWith(QStringLiteral(".torrent"), Qt::CaseInsensitive) && QDir::isAbsolutePath(link) && QFileInfo(link).isFile()) {
            request.source = link.toStdString();
            if (request.destinationDir.empty()) request.destinationDir = manager_.defaultDownloadDir().string();
            request.maxConnections = maxConnectionsPerDownload_;
            return static_cast<qulonglong>(manager_.addTorrent(std::move(request)));
        }
        if (!isMagnetOrUrl(link)) {
            *error = QStringLiteral("Only http(s) and magnet links can be added");
            return 0;
        }
        ORBIT_LOG_INFO("aria2", "Added a download through the aria2 interface");
        return static_cast<qulonglong>(addLink(link, std::move(request)));
    }, this);
    aria2Enabled_ = settings.value("aria2/enabled", false).toBool();
    aria2Port_ = std::clamp(settings.value("aria2/port", 6800).toInt(), 1024, 65535);
    aria2Secret_ = unprotectSecret(settings.value("aria2/secret").toString());
    applyAria2();

    networkLimits_ = settings.value("network/limits").toMap();
#if defined(__linux__)
    networkIdentity_ = new NetworkIdentity(this);
    connect(networkIdentity_, &NetworkIdentity::changed, this, [this] {
        applySpeedLimits();
        emit networkLimitsChanged();
    });
#endif

    pauseOnMetered_ = settings.value("network/pauseOnMetered", true).toBool();
    setupMeteredDetection();

    browserIntegrationEnabled_ = settings.value("browser/integration", !portable_).toBool();
    if (browserIntegrationEnabled_) repairBrowserIntegration();
    else browserIntegrationStatus_ = QStringLiteral("Turned off");
}

MainWindow::~MainWindow() = default;

void MainWindow::setHeadless(bool headless) {
    headless_ = headless;
    if (headless && fileConflictAction_ == QLatin1String("ask")) {
        manager_.setDefaultFileConflictAction(orbit::core::FileConflictAction::Rename);
    }
}

void MainWindow::setNetworkInterface(const QString& name) {
    const QString trimmed = name.trimmed();
    if (!isValidInterfaceName(trimmed) || trimmed == networkInterface_) return;
    networkInterface_ = trimmed;
    QSettings().setValue("network/interface", trimmed);
    manager_.setNetworkInterface(trimmed.toStdString());
    ORBIT_LOG_INFO("network", trimmed.isEmpty() ? std::string("Downloads use any network interface")
                                                : "Downloads bound to network interface " + trimmed.toStdString());
    emit networkInterfaceChanged();
}

QVariantList MainWindow::networkInterfaces() const {
    static const QRegularExpression hidden(QStringLiteral("^(lo|docker|veth|br-|virbr|vnet|podman|cni|flannel|kube)"));
    QVariantList result;
    bool savedListed = networkInterface_.isEmpty();
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        if (iface.flags().testFlag(QNetworkInterface::IsLoopBack) || hidden.match(iface.name()).hasMatch()) continue;
        const bool up = iface.flags().testFlag(QNetworkInterface::IsUp) && iface.flags().testFlag(QNetworkInterface::IsRunning);
        QString address;
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                address = entry.ip().toString();
                break;
            }
        }
        const bool vpn = iface.type() == QNetworkInterface::Virtual || iface.name().startsWith(QLatin1String("wg")) ||
                         iface.name().startsWith(QLatin1String("tun")) || iface.name().startsWith(QLatin1String("tap")) ||
                         iface.name().startsWith(QLatin1String("proton")) || iface.name().startsWith(QLatin1String("nordlynx"));
        result.append(QVariantMap{{"name", iface.name()}, {"address", address}, {"up", up}, {"vpn", vpn}});
        if (iface.name() == networkInterface_) savedListed = true;
    }
    // A VPN that's down right now has no interface at all; keep the saved choice visible.
    if (!savedListed) result.append(QVariantMap{{"name", networkInterface_}, {"address", QString()}, {"up", false}, {"vpn", true}});
    return result;
}

void MainWindow::setupMeteredDetection() {
    if (!QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Metered)) {
        ORBIT_LOG_INFO("network", "No metered-connection detection on this system");
        return;
    }
    QNetworkInformation* info = QNetworkInformation::instance();
    meteredDetectionAvailable_ = true;
    metered_ = info->isMetered();
    ORBIT_LOG_INFO("network", "Metered-connection detection through " + info->backendName().toStdString() +
                                  (metered_ ? " (metered now)" : ""));
    connect(info, &QNetworkInformation::isMeteredChanged, this, [this](bool metered) {
        if (metered == metered_) return;
        metered_ = metered;
        ORBIT_LOG_INFO("network", metered ? "The connection is metered now" : "The connection isn't metered any more");
        if (!metered) meteredOverridden_ = false;
        emit meteredChanged();
        applyMeteredState();
    });
    // Downloads restored at startup start right after this; give them a moment to.
    QTimer::singleShot(1500, this, [this] { applyMeteredState(); });

    if (info->supports(QNetworkInformation::Feature::Reachability)) {
        connect(info, &QNetworkInformation::reachabilityChanged, this, [this](QNetworkInformation::Reachability reachability) {
            if (reachability != QNetworkInformation::Reachability::Online) return;
            // Give the new connection (or a VPN coming up with it) a moment to settle.
            QTimer::singleShot(3000, this, [this] { retryAfterReconnect(); });
        });
    }
}

namespace {

// Failures a working connection can fix: dropped or refused connections, timeouts, name
// lookups, a bound interface that was down. Not a 4xx answer, a bad checksum or a full
// disk, which would only fail the same way again.
bool failedForNetworkReasons(const std::string& error) {
    // "Slow down" and "took too long" are 4xx answers that a later try usually gets past.
    if (error.find("HTTP 429") != std::string::npos || error.find("HTTP 408") != std::string::npos) return true;
    static const char* const kPermanent[] = {"HTTP 4", "free space", "checksum", "Could not create destination",
                                             "yt-dlp was not found", "Unsupported URL", "Video unavailable",
                                             "Private video", "Sign in"};
    for (const char* marker : kPermanent) {
        if (error.find(marker) != std::string::npos) return false;
    }
    return !error.empty();
}

}

void MainWindow::retryAfterReconnect() {
    int retried = 0;
    for (const auto& snap : manager_.snapshot()) {
        if (snap.state != orbit::core::DownloadState::Error || snap.type == orbit::core::DownloadType::Torrent) continue;
        if (!failedForNetworkReasons(snap.errorMessage)) continue;
        // At most a few automatic tries per download, so a link that's simply dead
        // doesn't restart on every Wi-Fi hiccup.
        int& tries = autoRetries_[snap.id];
        if (tries >= 3) continue;
        ++tries;
        manager_.resume(snap.id);
        ++retried;
    }
    if (retried > 0) ORBIT_LOG_INFO("network", "Back online; retrying " + std::to_string(retried) + " failed downloads");
}

void MainWindow::retryIfWorthIt(qlonglong rawId) {
    const auto id = static_cast<orbit::core::DownloadId>(rawId);
    const auto snap = manager_.snapshot(id);
    if (!snap) return;
    if (snap->state == orbit::core::DownloadState::Completed) {
        autoRetries_.erase(id);
        return;
    }
    // Torrents keep looking for peers by themselves; checksum mismatches, 4xx answers and
    // full disks would only fail the same way again.
    if (snap->state != orbit::core::DownloadState::Error || snap->type == orbit::core::DownloadType::Torrent ||
        snap->checksumState == orbit::core::ChecksumState::Mismatch || !failedForNetworkReasons(snap->errorMessage)) {
        return;
    }
    int& tries = autoRetries_[id];
    if (tries >= autoRetryCount_) return;
    ++tries;
    // A little longer each time, so a server that's down gets some room.
    const int delay = autoRetryDelaySeconds_ * tries;
    ORBIT_LOG_INFO("retry", "Download " + std::to_string(id) + " failed (" + snap->errorMessage + "); trying again in " +
                                std::to_string(delay) + " s (" + std::to_string(tries) + " of " + std::to_string(autoRetryCount_) + ")");
    QTimer::singleShot(delay * 1000, this, [this, id] {
        // Only if it's still failed: the user may have retried, removed or cancelled it.
        const auto now = manager_.snapshot(id);
        if (now && now->state == orbit::core::DownloadState::Error) manager_.resume(id);
    });
}

void MainWindow::applyMeteredState() {
    using orbit::core::DownloadState;
    if (metered_ && pauseOnMetered_ && !meteredOverridden_) {
        int paused = 0;
        for (const auto& snap : manager_.snapshot()) {
            if (snap.state != DownloadState::Downloading && snap.state != DownloadState::Connecting &&
                snap.state != DownloadState::Queued && snap.state != DownloadState::Seeding) continue;
            if (std::find(meteredPausedIds_.begin(), meteredPausedIds_.end(), snap.id) != meteredPausedIds_.end()) continue;
            manager_.pause(snap.id);
            meteredPausedIds_.push_back(snap.id);
            ++paused;
        }
        if (paused > 0) {
            ORBIT_LOG_INFO("network", "Paused " + std::to_string(paused) + " downloads on a metered connection");
            emit meteredPaused(paused);
            watchMeteredPauses();
        }
        return;
    }
    // Not metered (or the user said to go ahead): resume what this paused, and nothing else.
    for (const auto id : std::exchange(meteredPausedIds_, {})) {
        const auto snap = manager_.snapshot(id);
        if (snap && snap->state == DownloadState::Paused) manager_.resume(id);
    }
}

// A download the user starts again by hand is theirs from then on: if they pause it later,
// the end of the metered connection mustn't resume it. Checked every couple of seconds
// while any metered pauses are outstanding.
void MainWindow::watchMeteredPauses() {
    if (!meteredWatch_) {
        meteredWatch_ = new QTimer(this);
        meteredWatch_->setInterval(2000);
        connect(meteredWatch_, &QTimer::timeout, this, [this] {
            std::erase_if(meteredPausedIds_, [this](orbit::core::DownloadId id) {
                const auto snap = manager_.snapshot(id);
                return !snap || snap->state != orbit::core::DownloadState::Paused;
            });
            if (meteredPausedIds_.empty()) meteredWatch_->stop();
        });
    }
    meteredWatch_->start();
}

void MainWindow::setPauseOnMetered(bool enabled) {
    if (enabled == pauseOnMetered_) return;
    pauseOnMetered_ = enabled;
    QSettings().setValue("network/pauseOnMetered", enabled);
    emit meteredChanged();
    applyMeteredState();
}

void MainWindow::overrideMeteredPause() {
    meteredOverridden_ = true;
    applyMeteredState();
}

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

void MainWindow::applyTypeFolders() {
    std::map<std::string, std::string> folders;
    for (auto it = typeFolders_.cbegin(); it != typeFolders_.cend(); ++it) {
        const QString folder = it.value().toString();
        if (QDir::isAbsolutePath(folder)) folders[it.key().toStdString()] = QDir::cleanPath(folder).toStdString();
    }
    manager_.setTypeFolders(sortByType_, std::move(folders));
}

void MainWindow::setSortByType(bool enabled) {
    if (enabled == sortByType_) return;
    sortByType_ = enabled;
    QSettings().setValue("downloads/sortByType", enabled);
    applyTypeFolders();
    emit afterDownloadChanged();
}

void MainWindow::setTypeFolder(const QString& type, const QString& folder) {
    if (!typeFolders_.contains(type) || !QDir::isAbsolutePath(folder)) return;
    typeFolders_.insert(type, QDir::cleanPath(folder));
    QSettings().setValue("downloads/typeFolders", typeFolders_);
    applyTypeFolders();
    emit afterDownloadChanged();
}

void MainWindow::setUnpackArchives(bool enabled) {
    if (enabled == unpackArchives_) return;
    unpackArchives_ = enabled;
    QSettings().setValue("after/unpackArchives", enabled);
    emit afterDownloadChanged();
}

void MainWindow::setAfterCommand(const QString& command) {
    const QString trimmed = command.trimmed().left(1000);
    if (trimmed == afterCommand_) return;
    afterCommand_ = trimmed;
    QSettings().setValue("after/command", trimmed);
    emit afterDownloadChanged();
}

void MainWindow::runAfterDownload(qlonglong id) {
    const auto snap = manager_.snapshot(static_cast<orbit::core::DownloadId>(id));
    if (!snap || snap->state != orbit::core::DownloadState::Completed || snap->destinationPath.empty()) return;
    // A web page chose this file (orbit:// link): it's never unpacked or handed to the
    // user's command, which may trust what it's given.
    if (snap->options.fromWebLink) {
        if (unpackArchives_ || !afterCommand_.isEmpty()) {
            ORBIT_LOG_INFO("after", "Skipped the after-download actions for download " + std::to_string(id) +
                                        " (added by a link on a web page)");
        }
        return;
    }
    // A file that failed its checksum is left alone; one still being checked waits.
    if (snap->checksumState == orbit::core::ChecksumState::Mismatch) return;
    if (snap->checksumState == orbit::core::ChecksumState::Pending ||
        snap->checksumState == orbit::core::ChecksumState::Verifying) {
        QTimer::singleShot(1500, this, [this, id] { runAfterDownload(id); });
        return;
    }
    const QString path = QString::fromStdString(snap->destinationPath);
    const QFileInfo info(path);
    if (!info.exists()) return;

    if (unpackArchives_ && info.isFile() && snap->type != orbit::core::DownloadType::Torrent) {
        static const QStringList kArchives{QStringLiteral(".zip"), QStringLiteral(".7z"), QStringLiteral(".rar"),
                                           QStringLiteral(".tar"), QStringLiteral(".tgz"), QStringLiteral(".tar.gz"),
                                           QStringLiteral(".tar.xz"), QStringLiteral(".tar.zst"), QStringLiteral(".tar.bz2")};
        const QString lower = info.fileName().toLower();
        QString stem;
        for (const QString& ext : kArchives) {
            if (lower.endsWith(ext)) stem = info.fileName().left(info.fileName().size() - ext.size());
        }
        const QString bsdtar = QStandardPaths::findExecutable(QStringLiteral("bsdtar"));
        if (!stem.isEmpty() && !bsdtar.isEmpty()) {
            // Into a new folder beside the archive, never over an existing one. bsdtar
            // refuses absolute paths and ".." in archives, so nothing lands outside it.
            QString target = info.absolutePath() + QLatin1Char('/') + stem;
            for (int n = 1; QFileInfo::exists(target); ++n) {
                target = info.absolutePath() + QLatin1Char('/') + stem + QStringLiteral(" (%1)").arg(n);
            }
            QDir().mkpath(target);
            auto* process = new QProcess(this);
            process->setProgram(bsdtar);
            process->setArguments({QStringLiteral("-x"), QStringLiteral("-f"), path, QStringLiteral("-C"), target});
            connect(process, &QProcess::finished, this, [this, process, target, name = info.fileName()](int code, QProcess::ExitStatus status) {
                process->deleteLater();
                const bool ok = status == QProcess::NormalExit && code == 0;
                ORBIT_LOG_INFO("after", (ok ? "Unpacked " : "Couldn't unpack ") + name.toStdString());
                if (!ok) QDir(target).removeRecursively();
                emit afterDownloadNotice(ok ? QStringLiteral("Unpacked") : QStringLiteral("Couldn't unpack"),
                                         ok ? name + QStringLiteral(" → ") + QFileInfo(target).fileName() : name, ok);
            });
            process->start();
        }
    }

    if (!afterCommand_.isEmpty()) {
        // Split like a shell would, but run directly (no shell), with the placeholders
        // filled in per argument, so a file name can never inject anything.
        QStringList args = QProcess::splitCommand(afterCommand_);
        if (args.isEmpty()) return;
        for (QString& arg : args) {
            arg.replace(QStringLiteral("%f"), info.absoluteFilePath())
                .replace(QStringLiteral("%d"), info.absolutePath())
                .replace(QStringLiteral("%n"), info.fileName());
        }
        const QString program = args.takeFirst();
        if (!QProcess::startDetached(program, args, info.absolutePath())) {
            ORBIT_LOG_WARN("after", "Couldn't run the after-download command: " + program.toStdString());
            emit afterDownloadNotice(QStringLiteral("Couldn't run the after-download command"), program, false);
        } else {
            ORBIT_LOG_INFO("after", "Ran the after-download command for download " + std::to_string(id));
        }
    }
}

void MainWindow::applyAria2() {
    aria2_->stop();
    if (!aria2Enabled_) {
        aria2Status_ = QStringLiteral("Off");
        emit aria2Changed();
        return;
    }
    if (aria2Secret_.size() < 16) {
        newAria2Secret();
        return;
    }
    const QString error = aria2_->start(static_cast<quint16>(aria2Port_), aria2Secret_);
    aria2Status_ = error.isEmpty() ? QStringLiteral("Listening on 127.0.0.1:%1").arg(aria2Port_) : QStringLiteral("Couldn't start: ") + error;
    emit aria2Changed();
}

void MainWindow::setAria2Enabled(bool enabled) {
    if (enabled == aria2Enabled_) return;
    aria2Enabled_ = enabled;
    QSettings().setValue("aria2/enabled", enabled);
    applyAria2();
}

void MainWindow::setAria2Port(int port) {
    port = std::clamp(port, 1024, 65535);
    if (port == aria2Port_) return;
    aria2Port_ = port;
    QSettings().setValue("aria2/port", port);
    applyAria2();
}

QString MainWindow::cloudflaredInstallCommand() const {
#if defined(_WIN32)
    return QStringLiteral("winget install --id Cloudflare.cloudflared");
#else
    // /etc/os-release names the distribution (ID) and the ones it's based on (ID_LIKE).
    QString ids;
    QFile osRelease(QStringLiteral("/etc/os-release"));
    if (osRelease.open(QIODevice::ReadOnly | QIODevice::Text)) {
        for (const QByteArray& line : osRelease.readAll().split('\n')) {
            if (line.startsWith("ID=") || line.startsWith("ID_LIKE=")) {
                ids += QLatin1Char(' ') + QString::fromUtf8(line.mid(line.indexOf('=') + 1)).remove(QLatin1Char('"')).toLower();
            }
        }
    }
    const QStringList words = ids.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    auto is = [&](const char* id) { return words.contains(QLatin1String(id)); };
    if (is("arch")) return QStringLiteral("sudo pacman -S cloudflared");
    if (is("debian") || is("ubuntu")) {
        return QStringLiteral("curl -LO https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-amd64.deb && "
                              "sudo apt install ./cloudflared-linux-amd64.deb");
    }
    if (is("fedora") || is("rhel") || is("centos")) {
        return QStringLiteral("sudo dnf install https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-x86_64.rpm");
    }
    if (is("opensuse") || is("suse")) return QStringLiteral("sudo zypper install cloudflared");
    return QStringLiteral("https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/downloads/");
#endif
}

void MainWindow::newAria2Secret() {
    QByteArray bytes(24, Qt::Uninitialized);
    for (char& b : bytes) b = static_cast<char>(QRandomGenerator::system()->generate() & 0xFF);
    aria2Secret_ = QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    QSettings().setValue("aria2/secret", protectSecret(aria2Secret_));
    applyAria2();
}

QString MainWindow::currentNetwork() const {
#if defined(__linux__)
    return networkIdentity_ ? networkIdentity_->name() : QString();
#else
    return {};
#endif
}

int MainWindow::currentNetworkLimitKBps() const {
    const QString name = currentNetwork();
    return name.isEmpty() ? 0 : std::max(0, networkLimits_.value(name).toInt());
}

void MainWindow::setCurrentNetworkLimitKBps(int kbps) {
    const QString name = currentNetwork();
    if (name.isEmpty()) return;
    kbps = std::max(0, kbps);
    if (kbps == currentNetworkLimitKBps()) return;
    if (kbps > 0) networkLimits_.insert(name, kbps);
    else networkLimits_.remove(name);
    QSettings().setValue("network/limits", networkLimits_);
    ORBIT_LOG_INFO("network", kbps > 0 ? "Download limit on this network: " + std::to_string(kbps) + " KB/s"
                                       : std::string("No download limit on this network"));
    applySpeedLimits();
    emit networkLimitsChanged();
}

void MainWindow::applySpeedLimits() {
    // The lowest of the limits in force: the general one, the scheduled one, and the one
    // for the network Orbit is on.
    const int networkLimit = currentNetworkLimitKBps();
    auto capped = [this, networkLimit](int limitKBps) {
        auto lower = [](int a, int b) { return a > 0 && b > 0 ? std::min(a, b) : std::max(a, b); };
        if (speedRuleActive_) limitKBps = lower(limitKBps, speedRuleLimitKBps_);
        return lower(limitKBps, networkLimit);
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

namespace {
bool isPlayableName(const QString& name) {
    static const QStringList kTypes{QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("webm"), QStringLiteral("avi"),
                                    QStringLiteral("mov"), QStringLiteral("m4v"), QStringLiteral("ts"), QStringLiteral("mp3"),
                                    QStringLiteral("m4a"), QStringLiteral("opus"), QStringLiteral("ogg"), QStringLiteral("flac"),
                                    QStringLiteral("wav"), QStringLiteral("aac")};
    QString lower = name.toLower();
    if (lower.endsWith(QLatin1String(".part"))) lower.chop(5);
    return kTypes.contains(QFileInfo(lower).suffix());
}
}

QString MainWindow::playWhileDownloading(qlonglong id) {
    using orbit::core::DownloadType;
    const auto snap = manager_.snapshot(static_cast<orbit::core::DownloadId>(id));
    if (!snap) return QStringLiteral("This download no longer exists");
    const QString destination = QString::fromStdString(snap->destinationPath);
    QString path;
    if (snap->type == DownloadType::Torrent) {
        // The largest video or audio file in the torrent.
        std::uint64_t largest = 0;
        for (const auto& file : manager_.torrentEngine().files(snap->id)) {
            const QString name = QString::fromStdString(file.path);
            if (file.padding || file.priority == 0 || file.size <= largest || !isPlayableName(name)) continue;
            largest = file.size;
            path = QFileInfo(destination).absolutePath() + QLatin1Char('/') + name;
        }
        if (path.isEmpty()) return QStringLiteral("This torrent has no video or music to play");
        manager_.setSequential(snap->id, true);
    } else {
        // yt-dlp writes to "<name>.part" until it's done.
        for (const QString& candidate : {destination, destination + QStringLiteral(".part")}) {
            if (QFileInfo(candidate).isFile()) {
                path = candidate;
                break;
            }
        }
        if (path.isEmpty()) return QStringLiteral("Nothing has arrived yet; try again in a moment");
        if (!isPlayableName(path) && snap->type != DownloadType::Media) return QStringLiteral("This isn't a video or music file");
    }
    for (const char* player : {"mpv", "vlc", "celluloid"}) {
        const QString program = QStandardPaths::findExecutable(QString::fromLatin1(player));
        if (!program.isEmpty() && QProcess::startDetached(program, {QStringLiteral("--"), path})) {
            ORBIT_LOG_INFO("media", "Playing download " + std::to_string(id) + " in " + player);
            return {};
        }
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path)) ? QString() : QStringLiteral("No video player found");
}

void MainWindow::startFileDrag(const QString& path) {
    if (!QFileInfo(path).isFile()) return;
    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(path)});
    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    const QIcon icon = QIcon::fromTheme(QStringLiteral("text-x-generic"));
    if (!icon.isNull()) drag->setPixmap(icon.pixmap(32, 32));
    draggingOut_ = true;
    emit draggingOutChanged();
    const Qt::DropAction result = drag->exec(Qt::CopyAction, Qt::CopyAction);
    draggingOut_ = false;
    emit draggingOutChanged();
    ORBIT_LOG_INFO("ui", result == Qt::CopyAction ? "Dragged a file out" : "File drag ended without a drop");
    // Not deleted here: the receiving app may still be fetching the data. It's a child
    // of this object, and Qt cleans up a finished drag itself.
}

void MainWindow::setBlurBehind(QObject* window, bool enabled) const {
#if defined(__linux__)
    if (auto* w = qobject_cast<QWindow*>(window)) linux_integration::setBlurBehind(w->winId(), enabled);
#else
    Q_UNUSED(window) Q_UNUSED(enabled)
#endif
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

int MainWindow::inspectLink(const QString& rawUrl) {
    const int token = nextProbeToken_++;
    const QString url = rawUrl.trimmed();
    const std::string source = url.toStdString();
    QString kind = orbit::utils::isMagnetUri(source) ? QStringLiteral("torrent")
                   : orbit::utils::isMediaUrl(source) ? QStringLiteral("media")
                   : isHttpUrl(url) ? QStringLiteral("http") : QString();
    if (kind != QLatin1String("http")) {
        QVariantMap result{{"ok", !kind.isEmpty()}, {"kind", kind}, {"size", 0.0}, {"sizeText", QString()},
                           {"fileName", kind == QLatin1String("torrent") ? QStringLiteral("Magnet link") : QString()},
                           {"error", kind.isEmpty() ? QStringLiteral("Not a link Orbit can download") : QString()}};
        QMetaObject::invokeMethod(this, [this, token, result] { emit linkInspected(token, result); }, Qt::QueuedConnection);
        return token;
    }
    orbit::core::DownloadRequest request;
    request.source = source;
    QPointer<MainWindow> self(this);
    manager_.inspectLink(request, [self, token](const orbit::core::RemoteProbe& probe) {
        QVariantMap result{{"ok", probe.ok}, {"kind", QStringLiteral("http")},
                           {"error", QString::fromStdString(probe.error)},
                           {"size", static_cast<double>(probe.size)},
                           {"sizeText", probe.size > 0 ? QString::fromStdString(orbit::utils::formatBytes(probe.size)) : QString()},
                           {"fileName", QString::fromStdString(probe.fileName)}};
        QMetaObject::invokeMethod(qApp, [self, token, result] {
            if (self) emit self->linkInspected(token, result);
        }, Qt::QueuedConnection);
    });
    return token;
}

QVariantMap MainWindow::previousDownloadOf(const QString& rawUrl) const {
    // Compared without a trailing slash or fragment, which never change what's downloaded.
    auto normalize = [](QString text) {
        text = text.trimmed();
        if (const int hash = text.indexOf(QLatin1Char('#')); hash >= 0 && !text.startsWith(QLatin1String("magnet:"))) text.truncate(hash);
        while (text.endsWith(QLatin1Char('/'))) text.chop(1);
        return text;
    };
    const QString wanted = normalize(rawUrl);
    if (wanted.isEmpty()) return {{"found", false}};
    for (const auto& snap : manager_.snapshot()) {
        if (normalize(QString::fromStdString(snap.source)) != wanted) continue;
        return {{"found", true},
                {"id", static_cast<qlonglong>(snap.id)},
                {"name", QString::fromStdString(snap.name)},
                {"state", QString::fromLatin1(orbit::core::toString(snap.state))},
                {"when", QLocale().toString(QDateTime::fromSecsSinceEpoch(snap.addedAt), QLocale::ShortFormat)}};
    }
    return {{"found", false}};
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

    if (isTorrentFile && headless_) {
        orbit::core::DownloadRequest request;
        request.source = trimmed.toStdString();
        request.destinationDir = manager_.defaultDownloadDir().string();
        request.maxConnections = maxConnectionsPerDownload_;
        manager_.addTorrent(std::move(request));
        return trimmed;
    }
    if (isTorrentFile) {
        // A .torrent's file list should be reviewed before anything starts downloading,
        // so route it to the Add Download dialog (which can parse a local .torrent and
        // show its file list) instead of blind-adding every file in it.
        emit externalTorrentNeedsReview(trimmed);
        return trimmed;
    }

    if (offerToLinkCapture(trimmed, {})) return trimmed;
    orbit::core::DownloadRequest request;
    request.fromWebLink = true;
    addLink(trimmed, std::move(request));
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

namespace {

bool isTransferring(orbit::core::DownloadState state) {
    using orbit::core::DownloadState;
    return state == DownloadState::Downloading || state == DownloadState::Connecting || state == DownloadState::Seeding;
}

QJsonObject controlTaskJson(const orbit::core::TaskSnapshot& snap) {
    // A finished or paused task keeps the last rate it measured; it isn't moving any more.
    const bool moving = isTransferring(snap.state);
    QJsonObject task{
        {"id", QString::number(static_cast<qulonglong>(snap.id))},
        {"name", QString::fromStdString(snap.name)},
        {"type", QString::fromLatin1(orbit::core::toString(snap.type))},
        {"state", QString::fromLatin1(orbit::core::toString(snap.state))},
        {"progress", snap.stats.progressRatio()},
        {"downloaded", static_cast<double>(snap.stats.bytesDownloaded)},
        {"total", static_cast<double>(snap.stats.totalBytes)},
        {"downloadRate", moving ? snap.stats.downloadRateBytesPerSec : 0.0},
        {"uploadRate", moving ? snap.stats.uploadRateBytesPerSec : 0.0},
        {"path", QString::fromStdString(snap.destinationPath)},
        {"source", QString::fromStdString(snap.source)},
    };
    if (moving && snap.stats.eta) task.insert("eta", static_cast<double>(snap.stats.eta->count()));
    if (!snap.errorMessage.empty()) task.insert("error", QString::fromStdString(snap.errorMessage));
    static const char* const kChecksum[] = {"none", "pending", "verifying", "match", "mismatch", "failed"};
    task.insert("checksum", QString::fromLatin1(kChecksum[static_cast<int>(snap.checksumState)]));
    return task;
}

}

QJsonObject MainWindow::handleControlRequest(const QJsonObject& request) {
    using orbit::core::DownloadId;
    using orbit::core::DownloadState;
    const QString type = request.value("type").toString();
    auto fail = [](const QString& message) { return QJsonObject{{"ok", false}, {"error", message}}; };

    if (type == QLatin1String("ping")) {
        return QJsonObject{{"ok", true}, {"version", QCoreApplication::applicationVersion()}};
    }
    if (type == QLatin1String("feeds") && feedWatcher_) {
        const QString action = request.value("action").toString();
        const QString url = request.value("url").toString().trimmed();
        if (action == QLatin1String("add")) {
            const QString error = feedWatcher_->addFeed(url, request.value("filter").toString(), request.value("directory").toString());
            if (!error.isEmpty()) return fail(error);
        } else if (action == QLatin1String("remove")) {
            feedWatcher_->removeFeed(url);
        } else if (action == QLatin1String("check")) {
            feedWatcher_->checkNow();
        }
        return QJsonObject{{"ok", true}, {"feeds", QJsonArray::fromVariantList(feedWatcher_->feeds())}};
    }
    if (type == QLatin1String("show")) {
        emit showWindowRequested();
        return QJsonObject{{"ok", true}};
    }
    if (type == QLatin1String("config")) return handleConfigRequest(request);
    if (type == QLatin1String("remote")) {
        const QString action = request.value("action").toString();
        if (action == QLatin1String("set-password")) {
            const QString user = request.value("username").toString().trimmed();
            const QString password = request.value("password").toString();
            if (user.isEmpty()) return fail(QStringLiteral("Give a username"));
            if (password.size() < 8) return fail(QStringLiteral("The password needs at least 8 characters"));
            if (!setRemoteCredentials(user, password)) return fail(QStringLiteral("Couldn't save the sign-in"));
            ORBIT_LOG_INFO("control", "Remote access sign-in set from the command line");
        } else if (action == QLatin1String("enable") || action == QLatin1String("disable")) {
            setRemoteAccessEnabled(action == QLatin1String("enable"));
        } else if (action != QLatin1String("status")) {
            return fail(QStringLiteral("Unknown remote action (status, enable, disable, set-password)"));
        }
        return QJsonObject{{"ok", true}, {"enabled", remoteAccessEnabled_}, {"running", remoteRunning()},
                           {"username", remoteUsername()}, {"configured", remoteCredentialsConfigured()},
                           {"url", remoteLocalUrl()}, {"tunnelHosts", remoteTunnelHost_}, {"message", remoteStatusMessage_}};
    }
    if (type == QLatin1String("list") || type == QLatin1String("status")) {
        QJsonArray tasks;
        int active = 0, queued = 0, paused = 0, seeding = 0, done = 0, failed = 0;
        double down = 0, up = 0;
        // Overall progress of what is downloading right now (sizes the server told us),
        // and the fastest of those downloads, for status bars and `orbit status --watch`.
        std::uint64_t activeDone = 0, activeTotal = 0;
        const orbit::core::TaskSnapshot* current = nullptr;
        const auto snapshots = manager_.snapshot();
        for (const auto& snap : snapshots) {
            tasks.append(controlTaskJson(snap));
            switch (snap.state) {
                case DownloadState::Downloading:
                case DownloadState::Connecting:
                    ++active;
                    if (snap.stats.totalBytes > 0) {
                        activeTotal += snap.stats.totalBytes;
                        activeDone += std::min(snap.stats.bytesDownloaded, snap.stats.totalBytes);
                    }
                    if (!current || snap.stats.downloadRateBytesPerSec > current->stats.downloadRateBytesPerSec) current = &snap;
                    break;
                case DownloadState::Queued: ++queued; break;
                case DownloadState::Paused: ++paused; break;
                case DownloadState::Seeding: ++seeding; break;
                case DownloadState::Completed: ++done; break;
                case DownloadState::Error:
                case DownloadState::Cancelled: ++failed; break;
            }
            if (isTransferring(snap.state)) {
                down += snap.stats.downloadRateBytesPerSec;
                up += snap.stats.uploadRateBytesPerSec;
            }
        }
        QJsonObject reply{{"ok", true},
                          {"active", active}, {"queued", queued}, {"paused", paused}, {"seeding", seeding},
                          {"completed", done}, {"failed", failed}, {"downloadRate", down}, {"uploadRate", up},
                          {"metered", metered_ && pauseOnMetered_ && !meteredOverridden_},
                          {"networkInterface", networkInterface_}};
        if (activeTotal > 0) {
            reply.insert("downloaded", static_cast<double>(activeDone));
            reply.insert("total", static_cast<double>(activeTotal));
            reply.insert("progress", static_cast<double>(activeDone) / static_cast<double>(activeTotal));
            if (down > 0) reply.insert("eta", std::ceil(static_cast<double>(activeTotal - activeDone) / down));
        }
        if (current) {
            reply.insert("current", QJsonObject{{"id", QString::number(static_cast<qulonglong>(current->id))},
                                                {"name", QString::fromStdString(current->name)},
                                                {"progress", current->stats.progressRatio()}});
        }
        if (type == QLatin1String("list")) reply.insert("downloads", tasks);
        return reply;
    }
    if (type == QLatin1String("add")) {
        const QString source = request.value("source").toString().trimmed();
        orbit::core::DownloadRequest download;
        const QString directory = request.value("directory").toString().trimmed();
        if (!directory.isEmpty()) {
            if (!QDir::isAbsolutePath(directory)) return fail(QStringLiteral("The folder must be an absolute path"));
            if (!QDir().mkpath(directory)) return fail(QStringLiteral("Couldn't create the folder ") + directory);
            download.destinationDir = QDir::cleanPath(directory).toStdString();
        }
        const QString checksum = request.value("checksum").toString().trimmed();
        if (!checksum.isEmpty()) download.expectedChecksum = checksum.toStdString();
        for (const QJsonValue& value : request.value("mirrors").toArray()) {
            const QString mirror = value.toString().trimmed();
            if (!isHttpUrl(mirror) || download.mirrors.size() >= 8) return fail(QStringLiteral("Mirrors must be http(s) links: ") + mirror);
            download.mirrors.push_back(mirror.toStdString());
        }
        if (!request.value("force").toBool()) {
            const QVariantMap before = previousDownloadOf(source);
            if (before.value("found").toBool()) {
                return fail(QStringLiteral("already downloaded as download %1 (%2, added %3); use --force to download it again")
                                .arg(before.value("id").toLongLong())
                                .arg(before.value("state").toString().toLower(), before.value("when").toString()));
            }
        }

        DownloadId id = 0;
        if (source.endsWith(QStringLiteral(".torrent"), Qt::CaseInsensitive) && QDir::isAbsolutePath(source)) {
            // The command line's own user asked for this file, so it starts right away
            // (a .torrent opened from elsewhere still goes through the review dialog).
            if (!QFileInfo(source).isFile()) return fail(QStringLiteral("No such file: ") + source);
            download.source = source.toStdString();
            if (download.destinationDir.empty()) download.destinationDir = manager_.defaultDownloadDir().string();
            download.maxConnections = maxConnectionsPerDownload_;
            id = manager_.addTorrent(std::move(download));
        } else {
            if (!isMagnetOrUrl(source)) return fail(QStringLiteral("Only http(s) links, magnet links and .torrent files can be added"));
            id = addLink(source, std::move(download));
        }
        if (id == 0) return fail(QStringLiteral("Orbit couldn't add it"));
        ORBIT_LOG_INFO("control", "Added a download from the command line");
        const auto snap = manager_.snapshot(id);
        return QJsonObject{{"ok", true}, {"download", snap ? controlTaskJson(*snap) : QJsonObject{}}};
    }
    if (type == QLatin1String("pause") || type == QLatin1String("resume") || type == QLatin1String("cancel") ||
        type == QLatin1String("remove")) {
        std::vector<DownloadId> ids;
        const bool all = request.value("all").toBool();
        const auto snaps = manager_.snapshot();
        if (all) {
            for (const auto& snap : snaps) ids.push_back(snap.id);
        } else {
            for (const QJsonValue& value : request.value("ids").toArray()) {
                bool ok = false;
                const auto id = static_cast<DownloadId>(value.toString().toULongLong(&ok));
                if (!ok || !manager_.snapshot(id)) return fail(QStringLiteral("No download with id ") + value.toString());
                ids.push_back(id);
            }
        }
        if (ids.empty()) return fail(QStringLiteral("Name the downloads by id, or use --all"));
        int changed = 0;
        for (const DownloadId id : ids) {
            const auto snap = manager_.snapshot(id);
            if (!snap) continue;
            const DownloadState s = snap->state;
            const bool running = s == DownloadState::Downloading || s == DownloadState::Connecting ||
                                 s == DownloadState::Queued || s == DownloadState::Seeding;
            if (type == QLatin1String("pause")) {
                if (!running) continue;
                manager_.pause(id);
            } else if (type == QLatin1String("resume")) {
                if (s != DownloadState::Paused && s != DownloadState::Error && s != DownloadState::Cancelled) continue;
                manager_.resume(id);
            } else if (type == QLatin1String("cancel")) {
                if (s == DownloadState::Completed || s == DownloadState::Cancelled || s == DownloadState::Error) continue;
                manager_.cancel(id);
            } else {
                // Like the window's Remove: active downloads stay, and files are never deleted here.
                if (running && !all) return fail(QStringLiteral("Pause or cancel download ") + QString::number(id) + QStringLiteral(" before removing it"));
                if (running) continue;
                manager_.remove(id, false);
            }
            ++changed;
        }
        return QJsonObject{{"ok", true}, {"changed", changed}};
    }
    return fail(QStringLiteral("Unknown command"));
}

namespace {

// The settings `orbit config` can change: this object's writable properties of a plain
// type, which are exactly what Settings in the window changes (with the same checks).
bool isConfigProperty(const QMetaProperty& property) {
    if (!property.isReadable() || !property.isWritable()) return false;
    const int type = property.metaType().id();
    return type == QMetaType::Bool || type == QMetaType::Int || type == QMetaType::Double || type == QMetaType::QString;
}

QJsonValue configValue(const QVariant& value) {
    switch (value.metaType().id()) {
        case QMetaType::Bool: return value.toBool();
        case QMetaType::Int: return value.toInt();
        case QMetaType::Double: return value.toDouble();
        default: return value.toString();
    }
}

}

QJsonObject MainWindow::handleConfigRequest(const QJsonObject& request) {
    auto fail = [](const QString& message) { return QJsonObject{{"ok", false}, {"error", message}}; };
    const QString action = request.value("action").toString(QStringLiteral("list"));
    const QString name = request.value("name").toString();
    const QMetaObject* meta = metaObject();

    if (action == QLatin1String("list")) {
        QJsonArray settings;
        for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
            const QMetaProperty property = meta->property(i);
            if (!isConfigProperty(property)) continue;
            settings.append(QJsonObject{{"name", QString::fromLatin1(property.name())},
                                        {"type", QString::fromLatin1(property.typeName())},
                                        {"value", configValue(property.read(this))}});
        }
        return QJsonObject{{"ok", true}, {"settings", settings}};
    }

    const int index = meta->indexOfProperty(name.toLatin1().constData());
    if (index < meta->propertyOffset() || !isConfigProperty(meta->property(index))) {
        return fail(QStringLiteral("No setting called \"%1\" (see orbit config list)").arg(name));
    }
    const QMetaProperty property = meta->property(index);
    if (action == QLatin1String("set")) {
        const QString text = request.value("value").toString().trimmed();
        QVariant value;
        bool ok = true;
        switch (property.metaType().id()) {
            case QMetaType::Bool: {
                const QString lower = text.toLower();
                const bool on = lower == "true" || lower == "on" || lower == "yes" || lower == "1";
                ok = on || lower == "false" || lower == "off" || lower == "no" || lower == "0";
                value = on;
                break;
            }
            case QMetaType::Int: value = text.toInt(&ok); break;
            case QMetaType::Double: value = text.toDouble(&ok); break;
            default: value = request.value("value").toString(); break;
        }
        if (!ok) return fail(QStringLiteral("\"%1\" isn't a valid %2").arg(text, QString::fromLatin1(property.typeName())));
        property.write(this, value);
        ORBIT_LOG_INFO("control", "Setting " + name.toStdString() + " changed from the command line");
    } else if (action != QLatin1String("get")) {
        return fail(QStringLiteral("Unknown config action (list, get, set)"));
    }
    // The value as it stands now: a setter may have clamped or refused it.
    return QJsonObject{{"ok", true}, {"name", name}, {"value", configValue(property.read(this))}};
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
    manager_.torrentEngine().setSeedAfterDownload(seedAfterDownload_);
    manager_.torrentEngine().setNoCowOnBtrfs(noCowOnBtrfs_);
    if (!seedAfterDownload_) manager_.completeFinishedPausedTorrents();
}

void MainWindow::setNoCowOnBtrfs(bool enabled) {
    if (enabled == noCowOnBtrfs_) return;
    noCowOnBtrfs_ = enabled;
    QSettings().setValue("torrent/noCowOnBtrfs", enabled);
    applyTorrentSettings();
    emit torrentSettingsChanged();
}

void MainWindow::setTorrentListenPort(int port) {
    port = std::clamp(port, 1024, 65535);
    if (port == torrentListenPort_) return;
    torrentListenPort_ = port;
    QSettings().setValue("torrent/listenPort", port);
    manager_.torrentEngine().setListenPort(port);
    emit torrentSettingsChanged();
}

void MainWindow::setAddPublicTrackers(bool add) {
    if (add == addPublicTrackers_) return;
    addPublicTrackers_ = add;
    QSettings().setValue("torrent/addPublicTrackers", add);
    manager_.torrentEngine().setAddPublicTrackers(add);
    emit torrentSettingsChanged();
}

void MainWindow::setAutoRetryCount(int count) {
    count = std::clamp(count, 0, 20);
    if (count == autoRetryCount_) return;
    autoRetryCount_ = count;
    QSettings().setValue("downloads/autoRetryCount", count);
    emit autoRetryChanged();
}

void MainWindow::setAutoRetryDelaySeconds(int seconds) {
    seconds = std::clamp(seconds, 5, 3600);
    if (seconds == autoRetryDelaySeconds_) return;
    autoRetryDelaySeconds_ = seconds;
    QSettings().setValue("downloads/autoRetryDelaySeconds", seconds);
    emit autoRetryChanged();
}

bool MainWindow::downloadsOnBtrfs() const {
    return orbit::core::TorrentEngine::isOnBtrfs(downloadDirectory().toStdString());
}

void MainWindow::setSeedAfterDownload(bool seed) {
    if (seed == seedAfterDownload_) return;
    seedAfterDownload_ = seed;
    QSettings().setValue("torrent/seedAfterDownload", seed);
    applyTorrentSettings();
    emit torrentSettingsChanged();
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
    const std::string packaging = orbit::utils::linuxPackaging();
    if (packaging == "flatpak") {
        // The Flatpak's own .desktop file is installed with it; browsers can't start a
        // sandboxed helper, so the extension falls back to orbit:// links.
        browserIntegrationStatus_ = QStringLiteral("Handled by the Flatpak; the extension uses orbit:// links");
        emit browserIntegrationChanged();
        return;
    }
    // Extras an in-app update copies but does not install (completions, the headless service,
    // menus): added when missing, refreshed when Orbit wrote them, for a
    // user install in a writable folder. A system package installs its own.
    if (packaging.empty() && QFileInfo(QCoreApplication::applicationDirPath()).isWritable()) {
        linux_integration::syncExtras(QCoreApplication::applicationDirPath());
    }
    // An AppImage runs from a temporary mount: the menu entry points at the AppImage file.
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    linux_integration::registerDesktopEntry(packaging == "appimage" && !appImage.isEmpty() ? appImage
                                                                                           : QCoreApplication::applicationFilePath());
    if (packaging == "appimage") {
        browserIntegrationStatus_ = QStringLiteral("Not available in the AppImage; the extension uses orbit:// links");
        emit browserIntegrationChanged();
        return;
    }
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
        if (it.key() == QLatin1String("feeds/list")) {
            // A feed downloads by itself, so one from a backup file must not choose its own
            // folder (a crafted file could aim it at ~/.config/autostart), and starts
            // unprimed: what it lists now is only remembered, never downloaded.
            QJsonArray feeds;
            for (const QJsonValue& value : QJsonDocument::fromJson(it.value().toString().toUtf8()).array()) {
                QJsonObject feed = value.toObject();
                feed.remove(QStringLiteral("directory"));
                feed.insert(QStringLiteral("primed"), false);
                feeds.append(feed);
            }
            settings.setValue(it.key(), QJsonDocument(feeds).toJson(QJsonDocument::Compact));
            ++restored;
            continue;
        }
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
            // Resuming an unfinished download truncates the file at its path, and cancelling
            // deletes it, so a path from a backup file is only kept for finished downloads
            // (for Open and Show in Folder). The others pick their file afresh when resumed.
            const bool finished = task.value("state").toString() == QLatin1String("Completed");
            for (auto it = task.begin(); it != task.end(); ++it) {
                if (it.key() == QLatin1String("cookie") || it.key() == QLatin1String("id")) continue;
                if (it.key() == QLatin1String("fileConflictPath")) continue;
                if (it.key() == QLatin1String("destinationPath") && !finished) continue;
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
