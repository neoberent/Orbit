#include "UpdateManager.hpp"

#include "utils/FileUtils.hpp"
#include "utils/Logger.hpp"

#include <QCoreApplication>
#include <QDate>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QVersionNumber>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr auto kDefaultFeedUrl = "https://orbitdm.netlify.app/updates.json";
constexpr int kFirstCheckDelayMs = 5000;
constexpr int kPeriodicCheckMs = 6 * 60 * 60 * 1000;
constexpr int kFeedTimeoutMs = 15000;
constexpr int kDownloadStallTimeoutMs = 30000;
constexpr qint64 kMaxFeedBytes = 256 * 1024;

QString describeNetworkError(QNetworkReply* reply) {
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (http >= 400) return QStringLiteral("The update server responded with HTTP %1").arg(http);
    switch (reply->error()) {
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
        return QStringLiteral("You appear to be offline, or the update server can't be reached");
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
        return QStringLiteral("The update server took too long to respond");
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
        return QStringLiteral("The update server isn't reachable right now");
    case QNetworkReply::SslHandshakeFailedError:
        return QStringLiteral("A secure connection to the update server couldn't be established");
    default:
        return QStringLiteral("Couldn't reach the update server (%1)").arg(reply->errorString());
    }
}

QVersionNumber parseVersion(const QString& text) {
    QString trimmed = text.trimmed();
    if (trimmed.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) trimmed.remove(0, 1);
    qsizetype suffixIndex = -1;
    const QVersionNumber version = QVersionNumber::fromString(trimmed, &suffixIndex);
    if (version.isNull() || suffixIndex != trimmed.size()) return {};
    return version;
}

QStringList parseNotes(const QJsonValue& value) {
    QStringList lines;
    if (value.isArray()) {
        for (const QJsonValue& item : value.toArray()) lines << item.toString();
    } else if (value.isString()) {
        lines = value.toString().split(QLatin1Char('\n'));
    }
    static const QRegularExpression bullet(QStringLiteral("^\\s*(?:[-*\\x{2022}]\\s+)?"));
    QStringList cleaned;
    for (QString line : lines) {
        line.remove(bullet);
        line = line.trimmed();
        if (!line.isEmpty()) cleaned << line;
    }
    return cleaned;
}

QString userAgent(const QString& version) {
#if defined(_WIN32)
    return QStringLiteral("Orbit/%1 (Windows)").arg(version);
#else
    return QStringLiteral("Orbit/%1 (Linux)").arg(version);
#endif
}

// Where installers are saved. On Linux not in /tmp: another user could create the folder
// there first and swap the verified archive before it is unpacked.
QString updateDir() {
#if defined(_WIN32)
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/OrbitUpdate");
#else
    return QString::fromStdString((orbit::utils::appDataDir() / "updates").string());
#endif
}

QByteArray hashFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return hash.result().toHex();
}

}

UpdateManager::UpdateManager(QObject* parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)) {
    const QByteArray overrideFeed = qgetenv("ORBIT_UPDATE_FEED");
    if (!overrideFeed.isEmpty()) {
        feedUrl_ = QUrl::fromUserInput(QString::fromLocal8Bit(overrideFeed));
        feedOverridden_ = true;
        ORBIT_LOG_INFO("update", "Using update feed override: " + feedUrl_.toString().toStdString());
    } else {
        feedUrl_ = QUrl(QString::fromLatin1(kDefaultFeedUrl));
    }

    QSettings settings;
    autoCheck_ = settings.value("updates/autoCheck", true).toBool();
    lastChecked_ = settings.value("updates/lastChecked").toDateTime();

    periodicTimer_.setInterval(kPeriodicCheckMs);
    connect(&periodicTimer_, &QTimer::timeout, this, [this] {
        if (autoCheck_) check(false);
    });

    rateTimer_.setInterval(500);
    connect(&rateTimer_, &QTimer::timeout, this, [this] {
        const qint64 elapsed = rateClock_.restart();
        if (elapsed <= 0) return;
        const double instant = static_cast<double>(bytesReceived_ - rateBytesMark_) * 1000.0 / elapsed;
        rateBytesMark_ = bytesReceived_;
        downloadRate_ = downloadRate_ <= 0.0 ? instant : downloadRate_ * 0.6 + instant * 0.4;
        emit progressChanged();
    });
}

UpdateManager::~UpdateManager() {
    if (downloadReply_) {
        downloadReply_->disconnect(this);
        downloadReply_->abort();
    }
    if (partFile_) {
        partFile_->close();
        partFile_->remove();
    }
}

QString UpdateManager::currentVersion() const { return QCoreApplication::applicationVersion(); }

void UpdateManager::setAutoCheck(bool enabled) {
    if (enabled == autoCheck_) return;
    autoCheck_ = enabled;
    QSettings().setValue("updates/autoCheck", enabled);
    emit autoCheckChanged();
}

void UpdateManager::startAutomaticChecks() {
    QTimer::singleShot(kFirstCheckDelayMs, this, [this] {
        removeStaleInstallers();
        if (autoCheck_) check(false);
    });
    periodicTimer_.start();
}

void UpdateManager::checkNow() { check(true); }

void UpdateManager::setState(const QString& state, const QString& error) {
    if (state == state_ && error == errorMessage_) return;
    state_ = state;
    errorMessage_ = error;
    emit stateChanged();
}

void UpdateManager::check(bool userInitiated) {
    if (state_ == QLatin1String("checking") || state_ == QLatin1String("downloading")) return;

    ORBIT_LOG_INFO("update", std::string(userInitiated ? "Manual" : "Automatic") + " update check: " +
                                 feedUrl_.toString().toStdString());
    setState(QStringLiteral("checking"));

    QNetworkRequest request(feedUrl_);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent(currentVersion()));
    request.setRawHeader("Cache-Control", "no-cache");
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(kFeedTimeoutMs);

    feedReply_ = network_->get(request);
    connect(feedReply_, &QNetworkReply::finished, this, [this, reply = feedReply_.data(), userInitiated] {
        handleFeedReply(reply, userInitiated);
        reply->deleteLater();
    });
}

void UpdateManager::handleFeedReply(QNetworkReply* reply, bool userInitiated) {
    if (reply->error() != QNetworkReply::NoError) {
        const QString message = describeNetworkError(reply);
        ORBIT_LOG_WARN("update", "Update check failed: " + message.toStdString());
        setState(QStringLiteral("error"), message);
        return;
    }

    const QByteArray body = reply->read(kMaxFeedBytes + 1);
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    const QJsonObject feed = doc.object();

    const QVersionNumber latest = parseVersion(feed.value("version").toString());
#if defined(_WIN32)
    const QJsonObject package = feed;
#else
    // The top-level link is the Windows installer; Linux builds come from "linux", and a
    // release without one isn't offered (there would be nothing to install).
    const QJsonObject package = feed.value("linux").toObject();
    if (doc.isObject() && !latest.isNull() && package.isEmpty()) {
        ORBIT_LOG_INFO("update", "Version " + latest.toString().toStdString() + " has no Linux build yet");
        lastChecked_ = QDateTime::currentDateTime();
        QSettings().setValue("updates/lastChecked", lastChecked_);
        emit lastCheckedChanged();
        latestVersion_.clear();
        releaseNotes_.clear();
        releaseDate_.clear();
        releasePage_.clear();
        emit releaseChanged();
        setState(QStringLiteral("upToDate"));
        return;
    }
#endif
    const QUrl url(package.value("url").toString());
    const QByteArray sha256 = package.value("sha256").toString().trimmed().toLower().toLatin1();
    static const QRegularExpression hexPattern(QStringLiteral("^[0-9a-f]{64}$"));

    QString problem;
    if (body.size() > kMaxFeedBytes || parseError.error != QJsonParseError::NoError || !doc.isObject())
        problem = QStringLiteral("The update information couldn't be read");
    else if (latest.isNull())
        problem = QStringLiteral("The update information has no valid version");
    else if (!url.isValid() || !isAllowedDownloadUrl(url))
        problem = QStringLiteral("The update information has no secure download link");
    else if (!hexPattern.match(QString::fromLatin1(sha256)).hasMatch())
        problem = QStringLiteral("The update information has no valid SHA-256 checksum");

    if (!problem.isEmpty()) {
        ORBIT_LOG_WARN("update", "Rejected update feed: " + problem.toStdString());
        setState(QStringLiteral("error"), problem);
        return;
    }

    lastChecked_ = QDateTime::currentDateTime();
    QSettings().setValue("updates/lastChecked", lastChecked_);
    emit lastCheckedChanged();

    const QVersionNumber current = parseVersion(currentVersion());
    if (latest <= current) {
        ORBIT_LOG_INFO("update", "Up to date (latest " + latest.toString().toStdString() + ")");
        latestVersion_.clear();
        releaseNotes_.clear();
        releaseDate_.clear();
        releasePage_.clear();
        emit releaseChanged();
        setState(QStringLiteral("upToDate"));
        return;
    }

    latestVersion_ = latest.toString();
    installerUrl_ = url;
    expectedSha256_ = sha256;
    downloadSize_ = package.value("size").toInteger(0);
    releaseNotes_ = parseNotes(feed.value("notes"));
    releasePage_ = feed.value("page").toString();
    const QDate released = QDate::fromString(feed.value("released").toString(), Qt::ISODate);
    releaseDate_ = released.isValid() ? QLocale::c().toString(released, QStringLiteral("MMMM d, yyyy")) : QString();
    emit releaseChanged();

    ORBIT_LOG_INFO("update", "Update available: " + latestVersion_.toStdString());
    setState(verifyCachedInstaller() ? QStringLiteral("ready") : QStringLiteral("available"));

    const QString skipped = QSettings().value("updates/skippedVersion").toString();
    if (userInitiated) {
        emit updateFound(true);
    } else if (latestVersion_ != skipped && latestVersion_ != announcedVersion_) {
        announcedVersion_ = latestVersion_;
        emit updateFound(false);
    }
}

bool UpdateManager::isAllowedDownloadUrl(const QUrl& url) const {
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("https")) return !url.host().isEmpty();
    return feedOverridden_ && (scheme == QLatin1String("file") || scheme == QLatin1String("http"));
}

QString UpdateManager::installerPath() const {
    const QString dir = updateDir();
#if defined(_WIN32)
    return dir + QStringLiteral("/OrbitSetup-%1.exe").arg(latestVersion_);
#else
    return dir + QStringLiteral("/OrbitSetup-%1-linux.tar.gz").arg(latestVersion_);
#endif
}

bool UpdateManager::verifyCachedInstaller() {
    const QString path = installerPath();
    if (!QFileInfo::exists(path)) return false;
    if (hashFile(path) == expectedSha256_) return true;
    QFile::remove(path);
    return false;
}

void UpdateManager::removeStaleInstallers() const {
    QDir dir(updateDir());
    if (!dir.exists()) return;
    const QVersionNumber current = parseVersion(currentVersion());
    static const QRegularExpression namePattern(QStringLiteral("^OrbitSetup-(.+?)(?:\\.exe|-linux\\.tar\\.gz)(?:\\.part)?$"));
    for (const QString& name : dir.entryList(QDir::Files)) {
        const auto match = namePattern.match(name);
        if (!match.hasMatch()) continue;
        const QVersionNumber version = parseVersion(match.captured(1));
        if (version.isNull() || version <= current || name.endsWith(QLatin1String(".part"))) {
            dir.remove(name);
        }
    }
}

void UpdateManager::downloadUpdate() {
    if (!updateAvailable() || state_ == QLatin1String("downloading") || state_ == QLatin1String("checking")) return;
    if (verifyCachedInstaller()) {
        setState(QStringLiteral("ready"));
        return;
    }

    const QString finalPath = installerPath();
    QDir().mkpath(QFileInfo(finalPath).absolutePath());
#if !defined(_WIN32)
    QFile::setPermissions(QFileInfo(finalPath).absolutePath(),
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
    partFile_ = std::make_unique<QFile>(finalPath + QStringLiteral(".part"));
    if (!partFile_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString reason = partFile_->errorString();
        partFile_.reset();
        setState(QStringLiteral("error"), QStringLiteral("Couldn't save the update: ") + reason);
        return;
    }
    hasher_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    bytesReceived_ = 0;
    rateBytesMark_ = 0;
    downloadRate_ = 0.0;
    emit progressChanged();

    ORBIT_LOG_INFO("update", "Downloading installer: " + installerUrl_.toString().toStdString());

    QNetworkRequest request(installerUrl_);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent(currentVersion()));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kDownloadStallTimeoutMs);

    downloadReply_ = network_->get(request);
    connect(downloadReply_, &QNetworkReply::readyRead, this, [this] {
        if (!downloadReply_ || !partFile_) return;
        const QByteArray chunk = downloadReply_->readAll();
        if (partFile_->write(chunk) != chunk.size()) {
            failDownload(QStringLiteral("Couldn't write the update to disk: ") + partFile_->errorString());
            return;
        }
        hasher_->addData(chunk);
        bytesReceived_ += chunk.size();
    });
    connect(downloadReply_, &QNetworkReply::downloadProgress, this, [this](qint64, qint64 total) {
        if (total > 0 && downloadSize_ != total) {
            downloadSize_ = total;
            emit releaseChanged();
        }
    });
    connect(downloadReply_, &QNetworkReply::finished, this, &UpdateManager::handleDownloadFinished);

    rateClock_.start();
    rateTimer_.start();
    setState(QStringLiteral("downloading"));
}

void UpdateManager::handleDownloadFinished() {
    if (!downloadReply_ || !partFile_) return;
    QNetworkReply* reply = downloadReply_;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        failDownload(describeNetworkError(reply));
        return;
    }
    const QByteArray rest = reply->readAll();
    if (!rest.isEmpty()) {
        partFile_->write(rest);
        hasher_->addData(rest);
        bytesReceived_ += rest.size();
    }
    partFile_->close();

    if (hasher_->result().toHex() != expectedSha256_) {
        failDownload(QStringLiteral("The downloaded installer failed its integrity check and was discarded"));
        return;
    }

    const QString finalPath = installerPath();
    QFile::remove(finalPath);
    if (!partFile_->rename(finalPath)) {
        failDownload(QStringLiteral("Couldn't save the update: ") + partFile_->errorString());
        return;
    }

    ORBIT_LOG_INFO("update", "Installer downloaded and verified: " + finalPath.toStdString());
    partFile_.reset();
    hasher_.reset();
    rateTimer_.stop();
    downloadRate_ = 0.0;
    emit progressChanged();
    setState(QStringLiteral("ready"));
}

void UpdateManager::failDownload(const QString& message) {
    ORBIT_LOG_WARN("update", "Update download failed: " + message.toStdString());
    resetDownload();
    setState(QStringLiteral("error"), message);
}

void UpdateManager::resetDownload() {
    rateTimer_.stop();
    if (downloadReply_) {
        downloadReply_->disconnect(this);
        downloadReply_->abort();
        downloadReply_->deleteLater();
        downloadReply_ = nullptr;
    }
    if (partFile_) {
        partFile_->close();
        partFile_->remove();
        partFile_.reset();
    }
    hasher_.reset();
    bytesReceived_ = 0;
    downloadRate_ = 0.0;
    emit progressChanged();
}

void UpdateManager::cancelDownload() {
    if (state_ != QLatin1String("downloading")) return;
    ORBIT_LOG_INFO("update", "Update download cancelled");
    resetDownload();
    setState(QStringLiteral("available"));
}

void UpdateManager::installUpdate() {
    if (state_ != QLatin1String("ready")) return;
    if (!verifyCachedInstaller()) {
        setState(QStringLiteral("available"));
        downloadUpdate();
        return;
    }

#if !defined(_WIN32)
    installLinuxUpdate();
    return;
#endif

    const QString path = QDir::toNativeSeparators(installerPath());
    const QStringList args{QStringLiteral("/SILENT"), QStringLiteral("/SUPPRESSMSGBOXES"),
                           QStringLiteral("/NORESTART"), QStringLiteral("/CLOSEAPPLICATIONS"),
                           QStringLiteral("/ORBITUPDATE=1")};
    QProcess installer;
    installer.setProgram(path);
    installer.setArguments(args);
    installer.setWorkingDirectory(QFileInfo(path).absolutePath());
#if defined(_WIN32)
    // QProcess lets the child inherit every inheritable handle, and sockets are inheritable
    // by default. The installer would then hold copies of libtorrent's sockets, and closing
    // the session waits ~45 s for them, longer than the installer waits for Orbit to exit.
    installer.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* process) {
        process->inheritHandles = false;
        process->startupInfo->dwFlags &= ~STARTF_USESTDHANDLES;
    });
#endif
    if (!installer.startDetached()) {
        ORBIT_LOG_ERROR("update", "Couldn't start installer: " + path.toStdString());
        setState(QStringLiteral("error"), QStringLiteral("Couldn't start the installer"));
        return;
    }

    ORBIT_LOG_INFO("update", "Launched installer for " + latestVersion_.toStdString() + "; shutting down");
    emit installerLaunched();
}

#if !defined(_WIN32)
// The Linux release is a .tar.gz of the install folder. Like the Windows installer's
// /SILENT run, a small detached script waits for Orbit to exit, unpacks the archive over
// the install folder and starts the new version.
void UpdateManager::installLinuxUpdate() {
    const QString installDir = QCoreApplication::applicationDirPath();
    if (::access(QFile::encodeName(installDir).constData(), W_OK) != 0) {
        setState(QStringLiteral("error"),
                 QStringLiteral("Orbit is installed in a system folder; update it with your package manager"));
        return;
    }
    static const char* kScript =
        "pid=$1; archive=$2; dir=$3; exe=$4\n"
        "i=0; while kill -0 \"$pid\" 2>/dev/null && [ $i -lt 600 ]; do sleep 0.1; i=$((i+1)); done\n"
        "tmp=$(mktemp -d \"$dir/.orbit-update.XXXXXX\") || exit 1\n"
        "if tar -xzf \"$archive\" -C \"$tmp\" --strip-components=1 && [ -x \"$tmp/orbit\" ]; then\n"
        "  cp -rf \"$tmp\"/. \"$dir\"/ && rm -f \"$archive\"\n"
        "fi\n"
        "rm -rf \"$tmp\"\n"
        "exec setsid \"$exe\" >/dev/null 2>&1 </dev/null\n";
    const QStringList args{QStringLiteral("-c"), QString::fromLatin1(kScript), QStringLiteral("orbit-update"),
                           QString::number(QCoreApplication::applicationPid()), installerPath(), installDir,
                           QCoreApplication::applicationFilePath()};
    if (!QProcess::startDetached(QStringLiteral("/bin/sh"), args)) {
        ORBIT_LOG_ERROR("update", "Couldn't start the update script");
        setState(QStringLiteral("error"), QStringLiteral("Couldn't start the installer"));
        return;
    }
    ORBIT_LOG_INFO("update", "Installing " + latestVersion_.toStdString() + " into " + installDir.toStdString() +
                                 "; shutting down");
    emit installerLaunched();
}
#endif

void UpdateManager::skipThisVersion() {
    if (!updateAvailable()) return;
    QSettings().setValue("updates/skippedVersion", latestVersion_);
    ORBIT_LOG_INFO("update", "User skipped version " + latestVersion_.toStdString());
}

bool UpdateManager::openReleasePage() const {
    const QUrl url(releasePage_);
    if (!url.isValid() || url.scheme() != QLatin1String("https")) return false;
    return QDesktopServices::openUrl(url);
}
