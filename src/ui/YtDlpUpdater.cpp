#include "YtDlpUpdater.hpp"

#include "core/MediaDownloader.hpp"
#include "utils/FileUtils.hpp"
#include "utils/Logger.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QVersionNumber>

namespace {

constexpr auto kReleaseApi = "https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest";
constexpr int kFirstCheckDelayMs = 20000;
constexpr int kPeriodicCheckMs = 6 * 60 * 60 * 1000;
constexpr qint64 kCheckIntervalSecs = 24 * 60 * 60;
constexpr qint64 kFailureRecheckSecs = 60 * 60;
constexpr int kRequestTimeoutMs = 20000;
constexpr int kDownloadStallTimeoutMs = 30000;
constexpr qint64 kMaxJsonBytes = 1024 * 1024;
constexpr qint64 kMaxBinaryBytes = 200LL * 1024 * 1024;
constexpr int kVersionTimeoutMs = 30000;

QString assetName() {
#if defined(_WIN32)
    return QStringLiteral("yt-dlp.exe");
#elif defined(__APPLE__)
    return QStringLiteral("yt-dlp_macos");
#else
    return QStringLiteral("yt-dlp_linux");
#endif
}

QString exeName() {
#if defined(_WIN32)
    return QStringLiteral("yt-dlp.exe");
#else
    return QStringLiteral("yt-dlp");
#endif
}

// yt-dlp versions are dates, optionally with a build time: 2026.08.19 or 2026.08.19.123456.
QVersionNumber parseVersion(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral("^\\s*v?(\\d+(?:\\.\\d+)+)\\s*$"));
    const auto match = pattern.match(text);
    return match.hasMatch() ? QVersionNumber::fromString(match.captured(1)) : QVersionNumber();
}

QString describeError(QNetworkReply* reply) {
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (http == 403 || http == 429) return QStringLiteral("GitHub is limiting requests right now; Orbit will try again later");
    if (http >= 400) return QStringLiteral("GitHub responded with HTTP %1").arg(http);
    switch (reply->error()) {
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
        return QStringLiteral("You appear to be offline, or GitHub can't be reached");
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
        return QStringLiteral("GitHub took too long to respond");
    case QNetworkReply::SslHandshakeFailedError:
        return QStringLiteral("A secure connection to GitHub couldn't be established");
    default:
        return QStringLiteral("Couldn't reach GitHub (%1)").arg(reply->errorString());
    }
}

QNetworkRequest makeRequest(const QUrl& url, int timeoutMs) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Orbit/%1 (yt-dlp updater)").arg(QCoreApplication::applicationVersion()));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(timeoutMs);
    return request;
}

}

YtDlpUpdater::YtDlpUpdater(QObject* parent) : QObject(parent), network_(new QNetworkAccessManager(this)) {
    const QByteArray overrideFeed = qgetenv("ORBIT_YTDLP_FEED");
    if (!overrideFeed.isEmpty()) {
        feedUrl_ = QUrl::fromUserInput(QString::fromLocal8Bit(overrideFeed));
        feedOverridden_ = true;
        ORBIT_LOG_INFO("yt-dlp", "Using release feed override: " + feedUrl_.toString().toStdString());
    } else {
        feedUrl_ = QUrl(QString::fromLatin1(kReleaseApi));
    }

    QSettings settings;
    autoUpdate_ = settings.value("media/autoUpdateYtDlp", true).toBool();
    lastChecked_ = settings.value("media/ytDlpLastChecked").toDateTime();

    periodicTimer_.setInterval(kPeriodicCheckMs);
    connect(&periodicTimer_, &QTimer::timeout, this, [this] {
        if (autoUpdate_ && isDue(kCheckIntervalSecs)) check(false);
    });
}

YtDlpUpdater::~YtDlpUpdater() { abortTransfers(); }

QString YtDlpUpdater::toolsDir() const {
    return QString::fromStdWString((orbit::utils::appDataDir() / "tools").wstring());
}

QString YtDlpUpdater::managedPath() const { return toolsDir() + QLatin1Char('/') + exeName(); }

// Staged under a name that still ends in .exe, so it can be test-run before it's swapped in.
QString YtDlpUpdater::stagingPath() const {
#if defined(_WIN32)
    return toolsDir() + QStringLiteral("/yt-dlp.new.exe");
#else
    return toolsDir() + QStringLiteral("/yt-dlp.new");
#endif
}

void YtDlpUpdater::setAutoUpdate(bool enabled) {
    if (enabled == autoUpdate_) return;
    autoUpdate_ = enabled;
    QSettings().setValue("media/autoUpdateYtDlp", enabled);
    emit autoUpdateChanged();
    if (enabled && isDue(kCheckIntervalSecs) && state_ != QLatin1String("detecting")) check(false);
}

void YtDlpUpdater::setState(const QString& state, const QString& error) {
    if (state == state_ && error == errorMessage_) return;
    state_ = state;
    errorMessage_ = error;
    emit stateChanged();
}

bool YtDlpUpdater::isDue(qint64 maxAgeSecs) const {
    return !lastChecked_.isValid() || lastChecked_.secsTo(QDateTime::currentDateTime()) >= maxAgeSecs;
}

bool YtDlpUpdater::isAllowedUrl(const QUrl& url) const {
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("https")) {
        const QString host = url.host().toLower();
        return host == QLatin1String("github.com") || host == QLatin1String("api.github.com") ||
               host.endsWith(QLatin1String(".githubusercontent.com"));
    }
    return feedOverridden_ && (scheme == QLatin1String("http") || scheme == QLatin1String("file"));
}

void YtDlpUpdater::cleanupLeftovers() const {
    QDir dir(toolsDir());
    if (!dir.exists()) return;
    for (const QString& name : dir.entryList(QDir::Files)) {
        if (name.endsWith(QLatin1String(".old")) || name.startsWith(QLatin1String("yt-dlp.new"))) dir.remove(name);
    }
}

void YtDlpUpdater::start() {
    cleanupLeftovers();
    detectVersions();
    QTimer::singleShot(kFirstCheckDelayMs, this, [this] {
        if (autoUpdate_ && isDue(kCheckIntervalSecs)) check(false);
    });
    periodicTimer_.start();
}

void YtDlpUpdater::queryVersion(const QString& exe, std::function<void(const QString&)> done) {
    if (exe.isEmpty() || !QFileInfo::exists(exe)) {
        done(QString());
        return;
    }
    auto* process = new QProcess(this);
    process->setProgram(exe);
    process->setArguments({QStringLiteral("--version")});
    process->setProcessChannelMode(QProcess::MergedChannels);
    auto finished = std::make_shared<bool>(false);
    auto finish = [process, done, finished](const QString& version) {
        if (*finished) return;
        *finished = true;
        process->deleteLater();
        done(version);
    };
    connect(process, &QProcess::finished, this, [process, finish](int exitCode, QProcess::ExitStatus status) {
        const QString output = QString::fromUtf8(process->readAll()).trimmed();
        const QString last = output.split(QLatin1Char('\n')).last().trimmed();
        finish(status == QProcess::NormalExit && exitCode == 0 && !parseVersion(last).isNull() ? last : QString());
    });
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish(QString());
    });
    QTimer::singleShot(kVersionTimeoutMs, process, [process, finish] {
        process->kill();
        finish(QString());
    });
    process->start();
}

void YtDlpUpdater::detectVersions() {
    setState(QStringLiteral("detecting"));
    const QString bundled = orbit::core::MediaDownloader::bundledYtDlpPath();
    queryVersion(bundled, [this](const QString& bundledVersion) {
        queryVersion(managedPath(), [this, bundledVersion](const QString& managedVersion) {
            chooseCopy(bundledVersion, managedVersion);
            if (state_ == QLatin1String("detecting")) setState(QStringLiteral("idle"));
        });
    });
}

void YtDlpUpdater::chooseCopy(const QString& bundledVersion, const QString& managedVersion) {
    const QVersionNumber bundled = parseVersion(bundledVersion);
    const QVersionNumber managed = parseVersion(managedVersion);
    usingManaged_ = !managed.isNull() && (bundled.isNull() || managed > bundled);
    currentVersion_ = usingManaged_ ? managedVersion : bundledVersion;
    orbit::core::MediaDownloader::setYtDlpPathOverride(usingManaged_ ? managedPath() : QString());

    if (!managedVersion.isEmpty() && !usingManaged_) {
        // Orbit itself was updated with a newer yt-dlp than the downloaded copy.
        QFile::remove(managedPath());
    }
    ORBIT_LOG_INFO("yt-dlp", "Using " + std::string(usingManaged_ ? "updated" : "bundled") + " yt-dlp " +
                                 (currentVersion_.isEmpty() ? std::string("(not found)") : currentVersion_.toStdString()));
    emit versionChanged();
}

void YtDlpUpdater::checkNow() { check(true); }

void YtDlpUpdater::noteMediaFailure() {
    if (!autoUpdate_ || !isDue(kFailureRecheckSecs)) return;
    ORBIT_LOG_INFO("yt-dlp", "A media download failed; checking whether yt-dlp is out of date");
    check(false);
}

void YtDlpUpdater::check(bool userInitiated) {
    if (state_ == QLatin1String("detecting") || state_ == QLatin1String("checking") ||
        state_ == QLatin1String("downloading") || state_ == QLatin1String("installing")) {
        return;
    }
    userInitiated_ = userInitiated;
    ORBIT_LOG_INFO("yt-dlp", std::string(userInitiated ? "Manual" : "Automatic") + " update check");
    setState(QStringLiteral("checking"));

    QNetworkRequest request = makeRequest(feedUrl_, kRequestTimeoutMs);
    request.setRawHeader("Accept", "application/vnd.github+json");
    reply_ = network_->get(request);
    connect(reply_, &QNetworkReply::finished, this, [this, reply = reply_.data(), userInitiated] {
        reply->deleteLater();
        if (reply != reply_) return;
        reply_ = nullptr;
        handleRelease(reply, userInitiated);
    });
}

void YtDlpUpdater::handleRelease(QNetworkReply* reply, bool userInitiated) {
    Q_UNUSED(userInitiated);
    if (reply->error() != QNetworkReply::NoError) {
        fail(describeError(reply));
        return;
    }
    const QByteArray body = reply->read(kMaxJsonBytes + 1);
    QJsonParseError parseError{};
    const QJsonObject release = QJsonDocument::fromJson(body, &parseError).object();
    const QString tag = release.value("tag_name").toString();
    const QVersionNumber latest = parseVersion(tag);
    if (body.size() > kMaxJsonBytes || parseError.error != QJsonParseError::NoError || latest.isNull()) {
        fail(QStringLiteral("GitHub's release information couldn't be read"));
        return;
    }

    latestVersion_ = tag;
    emit versionChanged();

    const QVersionNumber current = parseVersion(currentVersion_);
    if (!current.isNull() && latest <= current) {
        ORBIT_LOG_INFO("yt-dlp", "Up to date (" + currentVersion_.toStdString() + ")");
        markChecked();
        setState(QStringLiteral("upToDate"));
        return;
    }

    QUrl binaryUrl;
    QUrl sumsUrl;
    for (const QJsonValue& value : release.value("assets").toArray()) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value("name").toString();
        const QUrl url(asset.value("browser_download_url").toString());
        if (name == assetName()) binaryUrl = url;
        else if (name == QLatin1String("SHA2-256SUMS")) sumsUrl = url;
    }
    if (!binaryUrl.isValid() || !sumsUrl.isValid() || !isAllowedUrl(binaryUrl) || !isAllowedUrl(sumsUrl)) {
        fail(QStringLiteral("The yt-dlp release is missing its download or checksum file"));
        return;
    }

    ORBIT_LOG_INFO("yt-dlp", "Updating " + (currentVersion_.isEmpty() ? std::string("(none)") : currentVersion_.toStdString()) +
                                 " -> " + tag.toStdString());
    fetchChecksums(sumsUrl, binaryUrl);
}

void YtDlpUpdater::fetchChecksums(const QUrl& sumsUrl, const QUrl& binaryUrl) {
    reply_ = network_->get(makeRequest(sumsUrl, kRequestTimeoutMs));
    connect(reply_, &QNetworkReply::finished, this, [this, reply = reply_.data(), binaryUrl] {
        reply->deleteLater();
        if (reply != reply_) return;
        reply_ = nullptr;
        if (reply->error() != QNetworkReply::NoError) {
            fail(describeError(reply));
            return;
        }
        const QString sums = QString::fromUtf8(reply->read(kMaxJsonBytes));
        static const QRegularExpression line(QStringLiteral("^([0-9a-fA-F]{64})\\s+\\*?(\\S+)\\s*$"),
                                             QRegularExpression::MultilineOption);
        QByteArray expected;
        for (auto it = line.globalMatch(sums); it.hasNext();) {
            const auto match = it.next();
            if (match.captured(2) == assetName()) expected = match.captured(1).toLower().toLatin1();
        }
        if (expected.isEmpty()) {
            fail(QStringLiteral("The yt-dlp release has no checksum for %1").arg(assetName()));
            return;
        }
        downloadBinary(binaryUrl, expected);
    });
}

void YtDlpUpdater::downloadBinary(const QUrl& url, const QByteArray& expectedSha256) {
    if (!QDir().mkpath(toolsDir())) {
        fail(QStringLiteral("Couldn't create %1").arg(QDir::toNativeSeparators(toolsDir())));
        return;
    }
    stagingFile_ = std::make_unique<QFile>(stagingPath());
    if (!stagingFile_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString reason = stagingFile_->errorString();
        stagingFile_.reset();
        fail(QStringLiteral("Couldn't save the update: ") + reason);
        return;
    }
    hasher_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    expectedSha256_ = expectedSha256;
    progress_ = 0.0;
    emit progressChanged();
    setState(QStringLiteral("downloading"));

    reply_ = network_->get(makeRequest(url, kDownloadStallTimeoutMs));
    connect(reply_, &QNetworkReply::readyRead, this, [this] {
        if (!reply_ || !stagingFile_) return;
        const QByteArray chunk = reply_->readAll();
        if (stagingFile_->size() + chunk.size() > kMaxBinaryBytes) {
            fail(QStringLiteral("The yt-dlp download is larger than expected and was stopped"));
            return;
        }
        if (stagingFile_->write(chunk) != chunk.size()) {
            fail(QStringLiteral("Couldn't write the update to disk: ") + stagingFile_->errorString());
            return;
        }
        hasher_->addData(chunk);
    });
    connect(reply_, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (total <= 0) return;
        progress_ = static_cast<double>(received) / static_cast<double>(total);
        emit progressChanged();
    });
    connect(reply_, &QNetworkReply::finished, this, &YtDlpUpdater::handleDownloadFinished);
}

void YtDlpUpdater::handleDownloadFinished() {
    QNetworkReply* reply = reply_;
    if (!reply || !stagingFile_) return;
    reply->deleteLater();
    reply_ = nullptr;
    if (reply->error() != QNetworkReply::NoError) {
        fail(describeError(reply));
        return;
    }
    const QByteArray rest = reply->readAll();
    if (!rest.isEmpty()) {
        stagingFile_->write(rest);
        hasher_->addData(rest);
    }
    stagingFile_->close();
    stagingFile_.reset();

    if (hasher_->result().toHex() != expectedSha256_) {
        hasher_.reset();
        fail(QStringLiteral("The downloaded yt-dlp failed its integrity check and was discarded"));
        return;
    }
    hasher_.reset();
    installDownloaded();
}

void YtDlpUpdater::installDownloaded() {
    setState(QStringLiteral("installing"));
#if !defined(_WIN32)
    QFile::setPermissions(stagingPath(), QFile::permissions(stagingPath()) | QFile::ExeOwner | QFile::ExeUser);
#endif
    // A binary that doesn't even start is never swapped in.
    queryVersion(stagingPath(), [this](const QString& version) {
        if (version.isEmpty()) {
            QFile::remove(stagingPath());
            fail(QStringLiteral("The downloaded yt-dlp didn't start, so the current one is kept"));
            return;
        }

        // A running yt-dlp keeps its file open, but Windows still lets it be renamed. The
        // old copy moves aside and is deleted on the next start.
        const QString target = managedPath();
        const QString aside = target + QStringLiteral(".") + QString::number(QDateTime::currentMSecsSinceEpoch()) +
                              QStringLiteral(".old");
        if (QFileInfo::exists(target) && !QFile::rename(target, aside)) {
            QFile::remove(stagingPath());
            fail(QStringLiteral("Couldn't replace the current yt-dlp; Orbit will try again later"));
            return;
        }
        if (!QFile::rename(stagingPath(), target)) {
            if (QFileInfo::exists(aside)) QFile::rename(aside, target);
            QFile::remove(stagingPath());
            fail(QStringLiteral("Couldn't install the new yt-dlp; Orbit will try again later"));
            return;
        }
        QFile::remove(aside);

        currentVersion_ = version;
        usingManaged_ = true;
        orbit::core::MediaDownloader::setYtDlpPathOverride(target);
        emit versionChanged();
        ORBIT_LOG_INFO("yt-dlp", "Updated to " + version.toStdString());
        markChecked();
        setState(QStringLiteral("updated"));
        emit updated(version);
    });
}

// Only a check that got all the way through counts; after a failure the periodic timer
// (or the next failed media download) tries again instead of waiting a whole day.
void YtDlpUpdater::markChecked() {
    lastChecked_ = QDateTime::currentDateTime();
    QSettings().setValue("media/ytDlpLastChecked", lastChecked_);
    emit lastCheckedChanged();
}

void YtDlpUpdater::abortTransfers() {
    if (reply_) {
        QNetworkReply* reply = reply_;
        reply_ = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    if (stagingFile_) {
        stagingFile_->close();
        stagingFile_.reset();
    }
    QFile::remove(stagingPath());
    hasher_.reset();
}

void YtDlpUpdater::fail(const QString& message) {
    ORBIT_LOG_WARN("yt-dlp", "Update failed: " + message.toStdString());
    abortTransfers();
    progress_ = 0.0;
    emit progressChanged();
    setState(QStringLiteral("error"), message);
}
