#include "core/MediaDownloader.hpp"

#include "core/HttpDownloader.hpp"
#include "utils/FileUtils.hpp"
#include "utils/Logger.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>

#include <filesystem>
#include <optional>

namespace orbit::core {

namespace {

qint64 unitMultiplier(const QString& unit) {
    if (unit.compare(QStringLiteral("KiB"), Qt::CaseInsensitive) == 0) return 1024LL;
    if (unit.compare(QStringLiteral("MiB"), Qt::CaseInsensitive) == 0) return 1024LL * 1024;
    if (unit.compare(QStringLiteral("GiB"), Qt::CaseInsensitive) == 0) return 1024LL * 1024 * 1024;
    if (unit.compare(QStringLiteral("TiB"), Qt::CaseInsensitive) == 0) return 1024LL * 1024 * 1024 * 1024;
    return 1;
}

std::optional<std::chrono::seconds> parseEta(const QString& text) {
    if (text.isEmpty() || text.compare(QStringLiteral("Unknown"), Qt::CaseInsensitive) == 0 || text == "NA") {
        return std::nullopt;
    }
    const QStringList parts = text.split(':');
    long long total = 0;
    for (const QString& part : parts) {
        bool ok = false;
        const long long value = part.toLongLong(&ok);
        if (!ok) return std::nullopt;
        total = total * 60 + value;
    }
    return std::chrono::seconds(total);
}

std::mutex& overrideMutex() {
    static std::mutex mutex;
    return mutex;
}

QString& ytDlpOverride() {
    static QString path;
    return path;
}

QString platformExeName(const char* baseName) {
#if defined(_WIN32)
    return QString::fromLatin1(baseName) + QStringLiteral(".exe");
#else
    return QString::fromLatin1(baseName);
#endif
}

// We're called from a slot connected to the QProcess's own finished() signal, so the
// process object must not be destroyed synchronously here (its signal emission is
// still unwinding the call stack) — schedule the actual delete for later instead.
void deferDeleteProcess(std::unique_ptr<QProcess>& process) {
    if (process) process.release()->deleteLater();
}

bool isSubtitleFile(const QString& path) {
    static const QStringList kSuffixes = {"vtt", "srt", "ass", "ssa", "lrc", "ttml", "srv1", "srv2", "srv3", "json3"};
    return kSuffixes.contains(QFileInfo(path).suffix().toLower());
}

}

MediaDownloader::MediaDownloader(ProgressReporter reporter, QObject* parent)
    : QObject(parent), reporter_(std::move(reporter)) {}

MediaDownloader::~MediaDownloader() {
    std::vector<QProcess*> running;
    {
        std::scoped_lock lock(jobsMutex_);
        for (auto& [id, job] : jobs_) {
            if (job->process && job->process->state() != QProcess::NotRunning) {
                // Treat a still-running job as paused (not errored/cancelled) so it's
                // just picked back up on the next launch instead of showing as failed.
                job->pausedByUser = true;
                job->process->kill();
                running.push_back(job->process.get());
            }
        }
    }
    for (QProcess* proc : running) {
        proc->waitForFinished(2000);
    }
}

void MediaDownloader::setYtDlpPathOverride(const QString& path) {
    std::scoped_lock lock(overrideMutex());
    ytDlpOverride() = path;
}

QString MediaDownloader::bundledYtDlpPath() {
    const QString exeName = platformExeName("yt-dlp");
    const QString bundled = QCoreApplication::applicationDirPath() + QLatin1Char('/') + exeName;
    if (QFileInfo::exists(bundled)) return bundled;
    return QStandardPaths::findExecutable(exeName);
}

QString MediaDownloader::resolveYtDlpPath() {
    {
        std::scoped_lock lock(overrideMutex());
        if (!ytDlpOverride().isEmpty() && QFileInfo::exists(ytDlpOverride())) return ytDlpOverride();
    }
    return bundledYtDlpPath();
}

QString MediaDownloader::resolveFfmpegPath() {
    const QString exeName = platformExeName("ffmpeg");

    const QString bundledDir = QCoreApplication::applicationDirPath();
    if (QFileInfo::exists(bundledDir + QLatin1Char('/') + exeName)) return bundledDir;

    const QString onPath = QStandardPaths::findExecutable(exeName);
    if (!onPath.isEmpty()) return QFileInfo(onPath).absolutePath();
    return {};
}

bool MediaDownloader::isEngineAvailable() { return !resolveYtDlpPath().isEmpty(); }

void MediaDownloader::setProxy(const ProxySettings& proxy) {
    std::scoped_lock lock(optionsMutex_);
    proxy_ = proxy;
}

void MediaDownloader::setCookiesFromBrowser(const QString& browser) {
    std::scoped_lock lock(optionsMutex_);
    cookiesFromBrowser_ = browser.trimmed().toLower();
}

QString MediaDownloader::writeCookieFile(const QString& key, const std::vector<HttpCookie>& cookies) const {
    if (cookies.empty()) return {};
    const QString dir = QString::fromStdWString((utils::appDataDir() / "cookies").wstring());
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/") + key + QStringLiteral(".txt");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return {};
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write("# Netscape HTTP Cookie File\n");
    for (const HttpCookie& cookie : cookies) {
        const std::string line = HttpDownloader::netscapeCookieLine(cookie);
        if (!line.empty()) file.write(QByteArray::fromStdString(line) + "\n");
    }
    return path;
}

void MediaDownloader::removeCookieFile(const QString& path) {
    if (!path.isEmpty()) QFile::remove(path);
}

QStringList MediaDownloader::commonArguments(DownloadId, const DownloadRequest& request) const {
    // Without --encoding, yt-dlp prints file names in the console code page and silently
    // drops characters it can't represent, so the reported path wouldn't match the file.
    QStringList args{QStringLiteral("--encoding"), QStringLiteral("utf-8")};
    ProxySettings proxy;
    QString browser;
    {
        std::scoped_lock lock(optionsMutex_);
        proxy = proxy_;
        browser = cookiesFromBrowser_;
    }
    if (proxy.enabled()) args << QStringLiteral("--proxy") << QString::fromStdString(proxy.url(true));
    // Cookies captured from the browser session win over reading a browser's cookie store.
    if (request.cookies.empty() && !browser.isEmpty()) args << QStringLiteral("--cookies-from-browser") << browser;
    if (!request.userAgent.empty()) args << QStringLiteral("--user-agent") << QString::fromStdString(request.userAgent);
    if (!request.referrer.empty()) args << QStringLiteral("--referer") << QString::fromStdString(request.referrer);
    return args;
}

void MediaDownloader::fetchInfo(const QString& url, const std::vector<HttpCookie>& cookies,
                                std::function<void(const MediaInfo&)> done) {
    const QString ytDlpPath = resolveYtDlpPath();
    if (ytDlpPath.isEmpty()) {
        MediaInfo info;
        info.error = QStringLiteral("yt-dlp was not found next to Orbit or on PATH");
        done(info);
        return;
    }

    DownloadRequest request;
    request.cookies = cookies;
    static std::atomic<int> probeCounter{0};
    const QString cookieFile = writeCookieFile(QStringLiteral("probe-") + QString::number(++probeCounter), cookies);

    QStringList args;
    args << QStringLiteral("-J") << QStringLiteral("--flat-playlist") << QStringLiteral("--no-warnings")
         << QStringLiteral("--skip-download") << QStringLiteral("--no-color");
    args << commonArguments(0, request);
    if (!cookieFile.isEmpty()) args << QStringLiteral("--cookies") << cookieFile;
    args << url;

    auto* proc = new QProcess(this);
    proc->setProgram(ytDlpPath);
    proc->setArguments(args);
    auto* timeout = new QTimer(proc);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, proc, [proc] { proc->kill(); });

    connect(proc, &QProcess::finished, this, [proc, done, cookieFile](int exitCode, QProcess::ExitStatus status) {
        removeCookieFile(cookieFile);
        MediaInfo info;
        const QByteArray out = proc->readAllStandardOutput();
        const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
        proc->deleteLater();

        const QJsonDocument doc = QJsonDocument::fromJson(out);
        if (status != QProcess::NormalExit || exitCode != 0 || !doc.isObject()) {
            const QStringList lines = err.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            info.error = lines.isEmpty() ? QStringLiteral("yt-dlp couldn't read this link") : lines.last().trimmed();
            done(info);
            return;
        }
        const QJsonObject root = doc.object();
        info.ok = true;
        info.title = root.value("title").toString();
        info.uploader = root.value("uploader").toString(root.value("channel").toString());
        info.durationSeconds = root.value("duration").toDouble();
        const QString type = root.value("_type").toString();
        if (type == QLatin1String("playlist") || root.contains("entries")) {
            info.isPlaylist = true;
            for (const QJsonValue& value : root.value("entries").toArray()) {
                const QJsonObject e = value.toObject();
                MediaEntry entry;
                entry.url = e.value("url").toString();
                if (!entry.url.startsWith(QLatin1String("http"))) entry.url = e.value("webpage_url").toString();
                if (!entry.url.startsWith(QLatin1String("http"))) continue;
                entry.title = e.value("title").toString();
                entry.uploader = e.value("uploader").toString(e.value("channel").toString());
                entry.durationSeconds = e.value("duration").toDouble();
                info.entries.push_back(std::move(entry));
            }
        }
        done(info);
    });
    proc->start();
    timeout->start(90000);
}

void MediaDownloader::start(DownloadId id, const DownloadRequest& request) {
    {
        std::scoped_lock lock(jobsMutex_);
        if (auto it = jobs_.find(id); it != jobs_.end()) {
            // A failed job is being retried: start a fresh process with the (possibly
            // updated) request, unless one is somehow still running.
            Job& existing = *it->second;
            if (existing.process && existing.process->state() != QProcess::NotRunning) return;
            existing.request = request;
            existing.pausedByUser = false;
            existing.cancelRequested = false;
            existing.startedDownloadPhase = false;
            existing.subtitleStage = false;
            existing.cumulativeCompletedBytes = 0;
            existing.currentStageTotalBytes = 0;
            existing.stderrTail.clear();
        } else {
            auto job = std::make_unique<Job>();
            job->id = id;
            job->request = request;
            jobs_.emplace(id, std::move(job));
        }
    }
    reporter_.reportStateChanged(id, DownloadState::Connecting);
    launchProcess(id);
}

void MediaDownloader::launchProcess(DownloadId id) {
    Job* jobPtr = nullptr;
    {
        std::scoped_lock lock(jobsMutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return;
        jobPtr = it->second.get();
    }
    Job& job = *jobPtr;

    const QString ytDlpPath = resolveYtDlpPath();
    if (ytDlpPath.isEmpty()) {
        reporter_.reportError(id, "yt-dlp was not found. Place yt-dlp.exe next to Orbit, or install it and "
                                   "make sure it's on PATH.");
        reporter_.reportStateChanged(id, DownloadState::Error);
        return;
    }

    utils::ensureDirectoryExists(job.request.destinationDir);

    const QString destDir = QString::fromStdString(job.request.destinationDir);
    // "%(title)B" truncates to a byte budget so absurdly long titles don't blow past
    // filesystem filename limits; yt-dlp still sanitizes illegal characters itself.
    const QString outputTemplate = destDir + QStringLiteral("/%(title).200B [%(id)s].%(ext)s");

    QStringList args;
    args << QStringLiteral("--newline") << QStringLiteral("--no-color") << QStringLiteral("--no-playlist")
         << QStringLiteral("--no-mtime") << QStringLiteral("-o") << outputTemplate;
    args << commonArguments(id, job.request);
    removeCookieFile(job.cookieFile);
    job.cookieFile = writeCookieFile(QString::number(id), job.request.cookies);
    if (!job.cookieFile.isEmpty()) args << QStringLiteral("--cookies") << job.cookieFile;
    if (job.request.speedLimitBytesPerSec > 0) {
        args << QStringLiteral("--limit-rate") << QString::number(job.request.speedLimitBytesPerSec);
    }

    const QString ffmpegDir = resolveFfmpegPath();
    if (!ffmpegDir.isEmpty()) {
        args << QStringLiteral("--ffmpeg-location") << ffmpegDir;
    }

    if (job.request.mediaAudioOnly) {
        // Grab the best available audio-only stream and extract it. "--audio-format best"
        // keeps the source codec (no re-encode) whenever the container allows it.
        QString format = QString::fromStdString(job.request.mediaAudioFormat);
        if (format.isEmpty() || format == QStringLiteral("auto")) format = QStringLiteral("best");

        args << QStringLiteral("-f") << QStringLiteral("bestaudio/best") << QStringLiteral("-x")
             << QStringLiteral("--audio-format") << format << QStringLiteral("--audio-quality")
             << QStringLiteral("0");
    } else {
        QString heightFilter;
        const std::string& quality = job.request.mediaQuality;
        if (quality == "1080") heightFilter = QStringLiteral("[height<=1080]");
        else if (quality == "720") heightFilter = QStringLiteral("[height<=720]");
        else if (quality == "480") heightFilter = QStringLiteral("[height<=480]");

        // Best available separate video+audio streams, merged; falls back to a single
        // pre-muxed format if the site doesn't offer separate streams.
        args << QStringLiteral("-f") << (QStringLiteral("bv*") + heightFilter + QStringLiteral("+ba/b") + heightFilter)
             << QStringLiteral("--merge-output-format") << QStringLiteral("mp4");

        if (job.request.mediaSubtitles) {
            const QString languages = QString::fromStdString(job.request.mediaSubtitleLanguages).trimmed();
            // Subtitles are best-effort: a missing or rate-limited track shouldn't fail the video
            // (--ignore-errors turns it into a warning; a failed video still exits non-zero).
            args << QStringLiteral("--ignore-errors") << QStringLiteral("--write-subs") << QStringLiteral("--write-auto-subs")
                 << QStringLiteral("--sub-langs") << (languages.isEmpty() ? QStringLiteral("en.*,en") : languages);
            if (job.request.mediaEmbedSubtitles) {
                args << QStringLiteral("--embed-subs") << QStringLiteral("--compat-options") << QStringLiteral("no-keep-subs");
            } else {
                args << QStringLiteral("--convert-subs") << QStringLiteral("srt");
            }
        }
    }

    args << QString::fromStdString(job.request.source);

    job.process = std::make_unique<QProcess>();
    QProcess* proc = job.process.get();
    proc->setProgram(ytDlpPath);
    proc->setArguments(args);

    QObject::connect(proc, &QProcess::readyReadStandardOutput, this, [this, id] { onReadyReadStdout(id); });
    QObject::connect(proc, &QProcess::readyReadStandardError, this, [this, id] { onReadyReadStderr(id); });
    QObject::connect(proc, &QProcess::finished, this, [this, id](int exitCode, QProcess::ExitStatus exitStatus) {
        onProcessFinished(id, exitCode, exitStatus);
    });

    proc->start();
}

void MediaDownloader::onReadyReadStdout(DownloadId id) {
    Job* jobPtr = nullptr;
    {
        std::scoped_lock lock(jobsMutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return;
        jobPtr = it->second.get();
    }
    Job& job = *jobPtr;
    if (!job.process) return;

    const QByteArray data = job.process->readAllStandardOutput();
    const QStringList lines = QString::fromUtf8(data).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        parseProgressLine(job, line.trimmed());
    }
}

void MediaDownloader::onReadyReadStderr(DownloadId id) {
    Job* jobPtr = nullptr;
    {
        std::scoped_lock lock(jobsMutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return;
        jobPtr = it->second.get();
    }
    Job& job = *jobPtr;
    if (!job.process) return;

    job.stderrTail += QString::fromUtf8(job.process->readAllStandardError());
    constexpr qsizetype kMaxTail = 4000;
    if (job.stderrTail.size() > kMaxTail) {
        job.stderrTail = job.stderrTail.right(kMaxTail);
    }
}

void MediaDownloader::parseProgressLine(Job& job, const QString& line) {
    static const QRegularExpression kPercentRe(QStringLiteral(R"(\[download\]\s+([\d.]+)%)"));
    static const QRegularExpression kSizeRe(QStringLiteral(R"(of\s+~?\s*([\d.]+)\s*(B|KiB|MiB|GiB|TiB))"));
    static const QRegularExpression kSpeedRe(QStringLiteral(R"(at\s+([\d.]+)\s*(B|KiB|MiB|GiB|TiB)/s)"));
    static const QRegularExpression kEtaRe(QStringLiteral(R"(ETA\s+([\d:]+|Unknown))"));
    static const QRegularExpression kMergeRe(QStringLiteral(R"re(Merging formats into\s+"([^"]+)")re"));
    static const QRegularExpression kDestRe(QStringLiteral(R"(Destination:\s*(.+)$)"));

    if (const auto m = kMergeRe.match(line); m.hasMatch()) {
        job.lastKnownDestination = m.captured(1);
        reporter_.reportNameResolved(job.id, QFileInfo(job.lastKnownDestination).fileName().toStdString(),
                                      job.lastKnownDestination.toStdString());
    } else if (const auto sub = kDestRe.match(line); sub.hasMatch() && isSubtitleFile(sub.captured(1).trimmed())) {
        job.subtitleStage = true;
    } else if (const auto d = kDestRe.match(line); d.hasMatch()) {
        job.subtitleStage = false;
        job.lastKnownDestination = d.captured(1).trimmed();
        reporter_.reportNameResolved(job.id, QFileInfo(job.lastKnownDestination).fileName().toStdString(),
                                      job.lastKnownDestination.toStdString());
    }

    const auto percentMatch = kPercentRe.match(line);
    if (!percentMatch.hasMatch()) return;
    if (job.subtitleStage) return;

    if (!job.startedDownloadPhase) {
        job.startedDownloadPhase = true;
        reporter_.reportStateChanged(job.id, DownloadState::Downloading);
    }

    // yt-dlp reports progress separately per stream (video, then audio, then the
    // ffmpeg merge), each restarting at 0% with its own total size. Fold each
    // completed stream's size into a running baseline so the reported progress climbs
    // across the whole download instead of resetting to ~0% at each stream boundary.
    DownloadStats stats = job.lastStats;
    const double percent = percentMatch.captured(1).toDouble();

    if (const auto sizeMatch = kSizeRe.match(line); sizeMatch.hasMatch()) {
        const double value = sizeMatch.captured(1).toDouble();
        const auto stageTotal =
            static_cast<std::uint64_t>(value * static_cast<double>(unitMultiplier(sizeMatch.captured(2))));
        if (job.currentStageTotalBytes != 0 && stageTotal != job.currentStageTotalBytes) {
            job.cumulativeCompletedBytes += job.currentStageTotalBytes;
        }
        job.currentStageTotalBytes = stageTotal;
        stats.totalBytes = job.cumulativeCompletedBytes + job.currentStageTotalBytes;
    }
    if (job.currentStageTotalBytes > 0) {
        const auto stageDownloaded =
            static_cast<std::uint64_t>(static_cast<double>(job.currentStageTotalBytes) * (percent / 100.0));
        stats.bytesDownloaded = job.cumulativeCompletedBytes + stageDownloaded;
    }

    if (const auto speedMatch = kSpeedRe.match(line); speedMatch.hasMatch()) {
        const double value = speedMatch.captured(1).toDouble();
        stats.downloadRateBytesPerSec = value * static_cast<double>(unitMultiplier(speedMatch.captured(2)));
    } else {
        stats.downloadRateBytesPerSec = 0.0;
    }

    if (const auto etaMatch = kEtaRe.match(line); etaMatch.hasMatch()) {
        stats.eta = parseEta(etaMatch.captured(1));
    } else {
        stats.eta.reset();
    }
    stats.activeConnections = 1;

    job.lastStats = stats;
    reporter_.reportProgress(job.id, stats);
}

void MediaDownloader::onProcessFinished(DownloadId id, int exitCode, QProcess::ExitStatus exitStatus) {
    Job* jobPtr = nullptr;
    {
        std::scoped_lock lock(jobsMutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return;
        jobPtr = it->second.get();
    }
    Job& job = *jobPtr;
    removeCookieFile(job.cookieFile);
    job.cookieFile.clear();

    if (job.cancelRequested) {
        removePartialFiles(job);
        reporter_.reportStateChanged(id, DownloadState::Cancelled);
        deferDeleteProcess(job.process);
        // Unlike pause, a cancelled job is done for good — drop it so a later
        // DownloadManager-driven restart (e.g. the user hits Resume on a cancelled
        // entry) sees no stale job and is able to start a fresh process.
        std::scoped_lock lock(jobsMutex_);
        jobs_.erase(id);
        return;
    }

    if (job.pausedByUser) {
        reporter_.reportStateChanged(id, DownloadState::Paused);
        deferDeleteProcess(job.process);
        return;
    }

    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        DownloadStats finalStats = job.lastStats;
        if (finalStats.totalBytes > 0) finalStats.bytesDownloaded = finalStats.totalBytes;
        // Post-processing (merging streams, converting to MP3, ...) changes the size, so
        // report the size of the file that actually ended up on disk.
        if (!job.lastKnownDestination.isEmpty()) {
            std::error_code sizeEc;
            const std::filesystem::path finalPath = job.lastKnownDestination.toStdWString();
            const auto finalSize = std::filesystem::file_size(finalPath, sizeEc);
            if (!sizeEc && finalSize > 0) finalStats.bytesDownloaded = finalStats.totalBytes = finalSize;
        }
        finalStats.downloadRateBytesPerSec = 0.0;
        finalStats.eta.reset();
        job.lastStats = finalStats;
        reporter_.reportProgress(id, finalStats);
        reporter_.reportStateChanged(id, DownloadState::Completed);
    } else {
        QString error = job.stderrTail.trimmed();
        const QStringList lines = error.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (!lines.isEmpty()) error = lines.last();
        if (error.isEmpty()) error = QStringLiteral("yt-dlp exited with code %1").arg(exitCode);
        ORBIT_LOG_ERROR("MediaDownloader", "Task " + std::to_string(id) + " failed: " + error.toStdString());
        reporter_.reportError(id, error.toStdString());
        reporter_.reportStateChanged(id, DownloadState::Error);
    }
    deferDeleteProcess(job.process);
}

void MediaDownloader::pause(DownloadId id) {
    QProcess* proc = nullptr;
    {
        std::scoped_lock lock(jobsMutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return;
        it->second->pausedByUser = true;
        proc = it->second->process.get();
    }
    // yt-dlp is a console process with no window to close gracefully, and it can
    // resume a partially-written file on its own (it checks the .part file), so a
    // hard kill here is simpler and just as safe as trying to signal it politely.
    if (proc && proc->state() != QProcess::NotRunning) {
        proc->kill();
    }
}

void MediaDownloader::resume(DownloadId id) {
    bool needsLaunch = false;
    {
        std::scoped_lock lock(jobsMutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return;
        Job& job = *it->second;
        if (job.process && job.process->state() != QProcess::NotRunning) return;
        job.pausedByUser = false;
        job.cancelRequested = false;
        job.startedDownloadPhase = false;
        job.subtitleStage = false;
        // The relaunched yt-dlp process reports its own per-stage progress from scratch,
        // so the running totals used to fold multi-stream progress into one percentage
        // must restart from zero too, or they keep accumulating on top of the stale
        // pre-pause values and the reported byte counts inflate/jump on resume.
        job.cumulativeCompletedBytes = 0;
        job.currentStageTotalBytes = 0;
        needsLaunch = true;
    }
    if (needsLaunch) {
        reporter_.reportStateChanged(id, DownloadState::Connecting);
        launchProcess(id);
    }
}

void MediaDownloader::cancel(DownloadId id) {
    QProcess* proc = nullptr;
    bool wasRunning = false;
    {
        std::scoped_lock lock(jobsMutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return;
        it->second->cancelRequested = true;
        proc = it->second->process.get();
        wasRunning = proc && proc->state() != QProcess::NotRunning;
    }
    if (wasRunning) {
        proc->kill();
        return;
    }

    Job* jobPtr = nullptr;
    {
        std::scoped_lock lock(jobsMutex_);
        auto it = jobs_.find(id);
        if (it != jobs_.end()) jobPtr = it->second.get();
    }
    if (jobPtr) removePartialFiles(*jobPtr);
    reporter_.reportStateChanged(id, DownloadState::Cancelled);

    std::scoped_lock lock(jobsMutex_);
    jobs_.erase(id);
}

void MediaDownloader::forget(DownloadId id) {
    std::scoped_lock lock(jobsMutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return;
    if (it->second->process && it->second->process->state() != QProcess::NotRunning) return;
    jobs_.erase(it);
}

DownloadStats MediaDownloader::stats(DownloadId id) const {
    std::scoped_lock lock(jobsMutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return {};
    return it->second->lastStats;
}

bool MediaDownloader::hasJob(DownloadId id) const {
    std::scoped_lock lock(jobsMutex_);
    return jobs_.contains(id);
}

void MediaDownloader::removePartialFiles(const Job& job) const {
    if (job.lastKnownDestination.isEmpty()) return;
    const std::filesystem::path path = job.lastKnownDestination.toStdString();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    for (const char* suffix : {".part", ".ytdl"}) {
        std::filesystem::path withSuffix = path;
        withSuffix += suffix;
        std::filesystem::remove(withSuffix, ec);
    }
}

}
