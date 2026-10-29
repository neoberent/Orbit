#include "core/DownloadManager.hpp"

#include "core/HttpDownloader.hpp"
#include "core/MediaDownloader.hpp"
#include "core/TorrentEngine.hpp"
#include "utils/FileUtils.hpp"
#include "utils/Hashing.hpp"
#include "utils/Logger.hpp"
#include "utils/StringUtils.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace orbit::core {

namespace {

constexpr const char* kTaskFileExtension = ".task";

bool parseDownloadStateName(const std::string& s, DownloadState& out) {
    static const std::unordered_map<std::string, DownloadState> table = {
        {"Queued", DownloadState::Queued},         {"Connecting", DownloadState::Connecting},
        {"Downloading", DownloadState::Downloading}, {"Paused", DownloadState::Paused},
        {"Seeding", DownloadState::Seeding},         {"Completed", DownloadState::Completed},
        {"Error", DownloadState::Error},             {"Cancelled", DownloadState::Cancelled},
    };
    const auto it = table.find(s);
    if (it == table.end()) return false;
    out = it->second;
    return true;
}

const char* checksumStateName(ChecksumState state) {
    switch (state) {
        case ChecksumState::None: return "None";
        case ChecksumState::Pending: return "Pending";
        case ChecksumState::Verifying: return "Verifying";
        case ChecksumState::Match: return "Match";
        case ChecksumState::Mismatch: return "Mismatch";
        case ChecksumState::Failed: return "Failed";
    }
    return "None";
}

ChecksumState parseChecksumState(const std::string& s) {
    if (s == "Pending") return ChecksumState::Pending;
    if (s == "Verifying") return ChecksumState::Verifying;
    if (s == "Match") return ChecksumState::Match;
    if (s == "Mismatch") return ChecksumState::Mismatch;
    if (s == "Failed") return ChecksumState::Failed;
    return ChecksumState::None;
}

std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool isActiveState(DownloadState s) {
    return s == DownloadState::Downloading || s == DownloadState::Connecting || s == DownloadState::Seeding;
}

std::optional<HttpCookie> cookieFromLine(const std::string& line) {
    const auto parts = utils::split(line, '\t');
    if (parts.size() != 7) return std::nullopt;
    HttpCookie cookie;
    cookie.domain = parts[0];
    cookie.includeSubdomains = parts[1] == "TRUE";
    cookie.path = parts[2];
    cookie.secure = parts[3] == "TRUE";
    cookie.expires = std::strtoll(parts[4].c_str(), nullptr, 10);
    cookie.name = parts[5];
    cookie.value = parts[6];
    if (cookie.name.empty() || cookie.domain.empty()) return std::nullopt;
    return cookie;
}

std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

// Moves a file or a whole folder, falling back to copy + delete across drives.
std::string movePath(const std::filesystem::path& from, const std::filesystem::path& to,
                     const std::function<bool()>& shouldStop) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::is_regular_file(from, ec)) return utils::moveFile(from, to, shouldStop);
    if (!fs::is_directory(from, ec)) return "The downloaded files no longer exist";
    utils::ensureDirectoryExists(to.parent_path());
    fs::rename(from, to, ec);
    if (!ec) return {};
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::error_code cleanup;
        fs::remove_all(to, cleanup);
        return "Couldn't copy the files: " + ec.message();
    }
    fs::remove_all(from, ec);
    return {};
}

}

struct DownloadManager::Loaded {
    DownloadId id{0};
    DownloadType type{DownloadType::Http};
    DownloadState state{DownloadState::Queued};
    std::string name, destinationPath, errorMessage, checksumDetail, fileConflictPath;
    DownloadRequest request;
    ChecksumState checksumState{ChecksumState::None};
    std::int64_t addedAt{0};
    std::uint64_t bytesDownloaded{0};
    std::uint64_t totalBytes{0};
};

DownloadManager::DownloadManager(std::filesystem::path stateDir) : stateDir_(std::move(stateDir)) {
    utils::ensureDirectoryExists(stateDir_);
    defaultDownloadDir_ = utils::defaultDownloadsDir();
    utils::ensureDirectoryExists(defaultDownloadDir_);

    // Cookie files are only needed while yt-dlp runs; drop any left behind by a crash.
    {
        std::error_code ec;
        std::filesystem::remove_all(utils::appDataDir() / "cookies", ec);
    }

    ProgressReporter reporter;
    reporter.onProgress = [this](DownloadId id, const DownloadStats& stats) { onProgress(id, stats); };
    reporter.onStateChanged = [this](DownloadId id, DownloadState state) { onStateChanged(id, state); };
    reporter.onError = [this](DownloadId id, const std::string& msg) { onError(id, msg); };
    reporter.onNameResolved = [this](DownloadId id, const std::string& name, const std::string& path) {
        onNameResolved(id, name, path);
    };
    reporter.onNotice = [this](DownloadId id, bool ok, const std::string& message) { notifyNotice(id, ok, message); };
    reporter.onFileConflict = [this](DownloadId id, const std::string& path) { onFileConflict(id, path); };

    httpDownloader_ = std::make_unique<HttpDownloader>(reporter);
    mediaDownloader_ = std::make_unique<MediaDownloader>(reporter);

    TorrentEngineSettings torrentSettings;
    torrentSettings.defaultSavePath = defaultDownloadDir_;
    torrentEngine_ = std::make_unique<TorrentEngine>(reporter, torrentSettings);

    loadPersistedTasksAndResume();
}

DownloadManager::~DownloadManager() {
    {
        std::scoped_lock lock(workersMutex_);
        for (Worker& worker : workers_) worker.thread.request_stop();
        workers_.clear();
    }

    std::vector<DownloadId> activeHttp;
    {
        std::scoped_lock lock(mutex_);
        for (auto& [id, task] : tasks_) {
            if (task->type() != DownloadType::Http) continue;
            const auto state = task->snapshot().state;
            if (state == DownloadState::Downloading || state == DownloadState::Connecting) {
                activeHttp.push_back(id);
            }
        }
    }
    for (DownloadId id : activeHttp) httpDownloader_->pause(id);

    for (DownloadId id : activeHttp) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto snap = snapshot(id);
            if (!snap || snap->state != DownloadState::Downloading) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (task && task->snapshot().state == DownloadState::Paused) {
            persistTask(*task, DownloadState::Downloading);
        }
    }
}

DownloadId DownloadManager::addTask(DownloadType type, DownloadRequest request, const std::string& initialName) {
    if (request.destinationDir.empty()) request.destinationDir = defaultDownloadDir_.string();
    if (!request.expectedChecksum.empty()) {
        const auto spec = utils::parseChecksum(request.expectedChecksum);
        request.expectedChecksum = spec ? spec->toString() : std::string();
    }

    DownloadId id;
    std::vector<DownloadId> toStart;
    {
        std::scoped_lock lock(mutex_);
        id = nextId_++;
        const bool scheduled = request.scheduledStartAt > nowSeconds();
        const bool hasChecksum = !request.expectedChecksum.empty();
        auto task = std::make_unique<DownloadTask>(id, type, std::move(request));
        task->setName(initialName);
        if (scheduled) task->setState(DownloadState::Paused);
        if (hasChecksum) task->setChecksumState(ChecksumState::Pending);
        DownloadTask* taskPtr = task.get();
        tasks_.emplace(id, std::move(task));
        insertionOrder_.push_back(id);
        persistTask(*taskPtr);
        toStart = pumpQueueLocked();
    }
    startTasks(toStart);
    notifyUpdate(*snapshot(id));
    return id;
}

DownloadId DownloadManager::addHttpDownload(DownloadRequest request) {
    const std::string name = !request.suggestedName.empty() ? request.suggestedName : utils::fileNameFromUrl(request.source);
    return addTask(DownloadType::Http, std::move(request), name);
}

DownloadId DownloadManager::addTorrent(DownloadRequest request) {
    const std::string name = !request.suggestedName.empty() ? request.suggestedName
                             : (utils::isMagnetUri(request.source) ? std::string("Fetching torrent metadata...")
                                                                     : request.source);
    return addTask(DownloadType::Torrent, std::move(request), name);
}

DownloadId DownloadManager::addMediaDownload(DownloadRequest request) {
    const std::string name = !request.suggestedName.empty() ? request.suggestedName : request.source;
    return addTask(DownloadType::Media, std::move(request), name);
}

std::vector<DownloadId> DownloadManager::pumpQueueLocked() {
    int activeCount = 0;
    for (auto& [id, task] : tasks_) {
        const auto state = task->snapshot().state;
        if (state == DownloadState::Downloading || state == DownloadState::Connecting) ++activeCount;
    }

    std::vector<DownloadId> toStart;
    for (DownloadId id : insertionOrder_) {
        if (activeCount >= maxConcurrent_) break;
        auto it = tasks_.find(id);
        if (it == tasks_.end()) continue;
        DownloadTask& task = *it->second;
        if (task.snapshot().state != DownloadState::Queued) continue;

        task.setState(DownloadState::Connecting);
        persistTask(task);
        toStart.push_back(id);
        ++activeCount;
    }
    return toStart;
}

void DownloadManager::startTasks(const std::vector<DownloadId>& ids) {
    for (DownloadId id : ids) {
        DownloadType type;
        DownloadRequest request;
        {
            std::scoped_lock lock(mutex_);
            DownloadTask* task = findTaskLocked(id);
            if (!task) continue;
            type = task->type();
            request = task->request();
            if (request.fileConflict == FileConflictAction::Default) request.fileConflict = defaultConflict_;
        }
        if (type == DownloadType::Http) {
            httpDownloader_->start(id, request);
        } else if (type == DownloadType::Media) {
            mediaDownloader_->start(id, request);
        } else {
            torrentEngine_->start(id, request);
        }
    }
}

DownloadTask* DownloadManager::findTaskLocked(DownloadId id) {
    auto it = tasks_.find(id);
    return it == tasks_.end() ? nullptr : it->second.get();
}

bool DownloadManager::engineHasJobLocked(DownloadType type, DownloadId id) const {
    if (type == DownloadType::Http) return httpDownloader_->hasJob(id);
    if (type == DownloadType::Media) return mediaDownloader_->hasJob(id);
    return torrentEngine_->hasEntry(id);
}

DownloadManager::TaskRecord DownloadManager::recordFor(const DownloadTask& task, std::optional<DownloadState> stateOverride,
                                                       bool includeCookies) const {
    const TaskSnapshot snap = task.snapshot();
    const DownloadRequest req = task.request();
    TaskRecord r;
    auto add = [&r](const char* key, std::string value) { r.emplace_back(key, std::move(value)); };
    const char* typeName = snap.type == DownloadType::Torrent ? "Torrent" : snap.type == DownloadType::Media ? "Media" : "Http";

    add("id", std::to_string(snap.id));
    add("type", typeName);
    add("state", toString(stateOverride.value_or(snap.state)));
    add("name", snap.name);
    add("source", req.source);
    add("destinationDir", req.destinationDir);
    add("destinationPath", snap.destinationPath);
    add("suggestedName", req.suggestedName);
    add("maxConnections", std::to_string(req.maxConnections));
    add("errorMessage", snap.errorMessage);
    add("addedAt", std::to_string(snap.addedAt));
    add("bytesDownloaded", std::to_string(snap.stats.bytesDownloaded));
    add("totalBytes", std::to_string(snap.stats.totalBytes));
    add("mediaAudioOnly", req.mediaAudioOnly ? "1" : "0");
    add("mediaQuality", req.mediaQuality);
    add("mediaAudioFormat", req.mediaAudioFormat);
    add("mediaSubtitles", req.mediaSubtitles ? "1" : "0");
    add("mediaSubtitleLanguages", req.mediaSubtitleLanguages);
    add("mediaEmbedSubtitles", req.mediaEmbedSubtitles ? "1" : "0");

    std::string excluded;
    for (size_t i = 0; i < req.torrentExcludedFileIndices.size(); ++i) {
        if (i > 0) excluded += ",";
        excluded += std::to_string(req.torrentExcludedFileIndices[i]);
    }
    add("excludedFiles", excluded);

    add("speedLimit", std::to_string(req.speedLimitBytesPerSec));
    add("uploadLimit", std::to_string(req.uploadLimitBytesPerSec));
    add("sequential", req.sequential ? "1" : "0");
    add("expectedChecksum", req.expectedChecksum);
    add("checksumState", checksumStateName(snap.checksumState));
    add("checksumDetail", snap.checksumDetail);
    add("scheduledStartAt", std::to_string(req.scheduledStartAt));
    add("fileConflict", toString(req.fileConflict));
    add("fileConflictPath", snap.fileConflictPath);
    add("userAgent", req.userAgent);
    add("referrer", req.referrer);
    if (includeCookies) {
        for (const HttpCookie& cookie : req.cookies) {
            const std::string line = HttpDownloader::netscapeCookieLine(cookie);
            if (!line.empty()) add("cookie", line);
        }
    }
    return r;
}

DownloadManager::Loaded DownloadManager::parseRecord(const TaskRecord& record) {
    Loaded rec;
    for (const auto& [key, value] : record) {
        DownloadRequest& q = rec.request;
        if (key == "id") rec.id = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "type") {
            rec.type = (value == "Torrent") ? DownloadType::Torrent : (value == "Media") ? DownloadType::Media : DownloadType::Http;
        }
        else if (key == "state") { DownloadState s{}; if (parseDownloadStateName(value, s)) rec.state = s; }
        else if (key == "name") rec.name = value;
        else if (key == "source") q.source = value;
        else if (key == "destinationDir") q.destinationDir = value;
        else if (key == "destinationPath") rec.destinationPath = value;
        else if (key == "suggestedName") q.suggestedName = value;
        else if (key == "maxConnections") q.maxConnections = std::max(1, std::atoi(value.c_str()));
        else if (key == "errorMessage") rec.errorMessage = value;
        else if (key == "excludedFiles") {
            for (const auto& part : utils::split(value, ',')) {
                if (!part.empty()) q.torrentExcludedFileIndices.push_back(std::atoi(part.c_str()));
            }
        }
        else if (key == "mediaAudioOnly") q.mediaAudioOnly = (value == "1");
        else if (key == "mediaQuality") q.mediaQuality = value;
        else if (key == "mediaAudioFormat") q.mediaAudioFormat = value;
        else if (key == "mediaSubtitles") q.mediaSubtitles = (value == "1");
        else if (key == "mediaSubtitleLanguages") q.mediaSubtitleLanguages = value;
        else if (key == "mediaEmbedSubtitles") q.mediaEmbedSubtitles = (value == "1");
        else if (key == "addedAt") rec.addedAt = std::strtoll(value.c_str(), nullptr, 10);
        else if (key == "bytesDownloaded") rec.bytesDownloaded = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "totalBytes") rec.totalBytes = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "speedLimit") q.speedLimitBytesPerSec = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "uploadLimit") q.uploadLimitBytesPerSec = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "sequential") q.sequential = (value == "1");
        else if (key == "expectedChecksum") q.expectedChecksum = value;
        else if (key == "checksumState") rec.checksumState = parseChecksumState(value);
        else if (key == "checksumDetail") rec.checksumDetail = value;
        else if (key == "scheduledStartAt") q.scheduledStartAt = std::strtoll(value.c_str(), nullptr, 10);
        else if (key == "fileConflict") q.fileConflict = fileConflictActionFromString(value);
        else if (key == "fileConflictPath") rec.fileConflictPath = value;
        else if (key == "userAgent") q.userAgent = value;
        else if (key == "referrer") q.referrer = value;
        else if (key == "cookie") {
            if (auto cookie = cookieFromLine(value)) q.cookies.push_back(std::move(*cookie));
        }
    }
    return rec;
}

std::unique_ptr<DownloadTask> DownloadManager::taskFromLoaded(const Loaded& rec, DownloadId id) const {
    DownloadRequest request = rec.request;
    const bool keepsExistingFile = rec.state != DownloadState::Completed && rec.state != DownloadState::Cancelled;
    if (rec.type == DownloadType::Http && keepsExistingFile && !rec.destinationPath.empty()) {
        request.existingFilePath = rec.destinationPath;
    }

    auto task = std::make_unique<DownloadTask>(id, rec.type, std::move(request));
    if (!rec.name.empty()) task->setName(rec.name);
    if (!rec.destinationPath.empty()) task->setDestinationPath(rec.destinationPath);
    if (rec.addedAt > 0) task->setAddedAt(rec.addedAt);
    task->setChecksumState(rec.checksumState, rec.checksumDetail);

    // Older task files predate the byte counters; for a finished single-file
    // download the file on disk is the best available answer.
    DownloadStats restoredStats;
    restoredStats.bytesDownloaded = rec.bytesDownloaded;
    restoredStats.totalBytes = rec.totalBytes;
    if (rec.state == DownloadState::Completed && restoredStats.totalBytes == 0 && !rec.destinationPath.empty()) {
        std::error_code sizeEc;
        if (std::filesystem::is_regular_file(rec.destinationPath, sizeEc)) {
            const auto size = std::filesystem::file_size(rec.destinationPath, sizeEc);
            if (!sizeEc) restoredStats.bytesDownloaded = restoredStats.totalBytes = size;
        }
    }
    task->updateStats(restoredStats);
    return task;
}

void DownloadManager::persistTask(const DownloadTask& task, std::optional<DownloadState> stateOverride) const {
    const auto finalPath = stateDir_ / (std::to_string(task.id()) + kTaskFileExtension);
    auto tempPath = finalPath;
    tempPath += ".tmp";
    {
        std::ofstream out(tempPath, std::ios::trunc);
        if (!out) return;
        for (auto [key, value] : recordFor(task, stateOverride, true)) {
            std::replace(value.begin(), value.end(), '\n', ' ');
            std::replace(value.begin(), value.end(), '\r', ' ');
            out << key << "=" << value << "\n";
        }
        if (!out) return;
    }
    // Written to a temporary file and swapped in, so a crash mid-write can't leave a
    // truncated task file behind.
    std::error_code ec;
    std::filesystem::rename(tempPath, finalPath, ec);
    if (ec) std::filesystem::remove(tempPath, ec);
}

void DownloadManager::removePersistedTask(DownloadId id) const {
    std::error_code ec;
    std::filesystem::remove(stateDir_ / (std::to_string(id) + kTaskFileExtension), ec);
}

void DownloadManager::loadPersistedTasksAndResume() {
    std::vector<DownloadId> toStart;
    std::vector<DownloadId> toVerify;
    {
        std::scoped_lock lock(mutex_);

        std::error_code ec;
        if (!std::filesystem::exists(stateDir_, ec)) return;

        std::vector<Loaded> records;
        for (const auto& entry : std::filesystem::directory_iterator(stateDir_, ec)) {
            if (ec) break;
            if (!entry.is_regular_file() || entry.path().extension() != kTaskFileExtension) continue;

            std::ifstream in(entry.path());
            if (!in) continue;

            TaskRecord record;
            std::string line;
            while (std::getline(in, line)) {
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                record.emplace_back(line.substr(0, eq), line.substr(eq + 1));
            }
            Loaded rec = parseRecord(record);
            if (rec.id == 0 || rec.request.source.empty()) continue;
            records.push_back(std::move(rec));
        }

        std::sort(records.begin(), records.end(), [](const Loaded& a, const Loaded& b) { return a.id < b.id; });

        for (const Loaded& rec : records) {
            nextId_ = std::max(nextId_, rec.id + 1);
            auto task = taskFromLoaded(rec, rec.id);

            if (rec.state == DownloadState::Error) {
                task->setError(rec.errorMessage);
            } else if (rec.state == DownloadState::Completed || rec.state == DownloadState::Cancelled ||
                       rec.state == DownloadState::Paused) {
                task->setState(rec.state);
                if (rec.state == DownloadState::Paused) task->setFileConflictPath(rec.fileConflictPath);
            } else {
                task->setState(DownloadState::Queued);
            }

            if (rec.state == DownloadState::Completed && rec.type != DownloadType::Torrent &&
                !rec.request.expectedChecksum.empty() &&
                (rec.checksumState == ChecksumState::None || rec.checksumState == ChecksumState::Pending ||
                 rec.checksumState == ChecksumState::Verifying)) {
                toVerify.push_back(rec.id);
            }

            ORBIT_LOG_INFO("DownloadManager", "Restored task " + std::to_string(rec.id) + " (" + rec.name + ") from previous session");

            tasks_.emplace(rec.id, std::move(task));
            insertionOrder_.push_back(rec.id);
        }

        toStart = pumpQueueLocked();
    }
    startTasks(toStart);
    for (DownloadId id : toVerify) startChecksumVerification(id);
}

void DownloadManager::pause(DownloadId id) {
    DownloadType type;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task) return;
        type = task->type();
    }
    if (type == DownloadType::Http) httpDownloader_->pause(id);
    else if (type == DownloadType::Media) mediaDownloader_->pause(id);
    else torrentEngine_->pause(id);
}

void DownloadManager::resume(DownloadId id) {
    DownloadType type = DownloadType::Http;
    bool needsFreshStart = false;
    std::vector<DownloadId> toStart;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task || busy_.contains(id)) return;
        type = task->type();
        // Starting a download by hand (or by its schedule) supersedes any start time.
        if (task->request().scheduledStartAt != 0) {
            task->updateRequest([](DownloadRequest& r) { r.scheduledStartAt = 0; });
            persistTask(*task);
        }
        const bool engineHasLiveJob = engineHasJobLocked(type, id);
        const auto state = task->snapshot().state;
        if (state == DownloadState::Error || state == DownloadState::Cancelled ||
            (state == DownloadState::Paused && !engineHasLiveJob)) {
            task->setState(DownloadState::Queued);
            if (state == DownloadState::Error && task->snapshot().checksumState == ChecksumState::Mismatch) {
                task->setChecksumState(ChecksumState::Pending);
            }
            persistTask(*task);
            needsFreshStart = true;
            toStart = pumpQueueLocked();
        }
    }
    if (needsFreshStart) {
        startTasks(toStart);
        if (auto snap = snapshot(id)) notifyUpdate(*snap);
    } else {
        if (type == DownloadType::Http) httpDownloader_->resume(id);
        else if (type == DownloadType::Media) mediaDownloader_->resume(id);
        else torrentEngine_->resume(id);
    }
}

void DownloadManager::cancel(DownloadId id) {
    DownloadType type;
    std::optional<TaskSnapshot> directlyCancelled;
    std::vector<DownloadId> toStart;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task || busy_.contains(id)) return;
        type = task->type();

        // A task restored from a previous session (or still waiting in the queue) has no
        // engine-side job yet, so the engines' cancel() would silently do nothing. Cancel
        // it here instead, cleaning up any partial HTTP download the same way the engine would.
        const bool engineHasLiveJob = engineHasJobLocked(type, id);
        const auto snap = task->snapshot();
        if (!engineHasLiveJob && snap.state != DownloadState::Completed && snap.state != DownloadState::Cancelled) {
            // A download waiting on a file conflict never wrote anything, and the file at
            // its path belongs to someone else, so there's nothing of ours to delete.
            if (type == DownloadType::Http && !snap.destinationPath.empty() && snap.fileConflictPath.empty()) {
                std::error_code ec;
                std::filesystem::remove(snap.destinationPath, ec);
                std::filesystem::remove(snap.destinationPath + ".orbitresume", ec);
            }
            task->setState(DownloadState::Cancelled);
            persistTask(*task);
            directlyCancelled = task->snapshot();
            toStart = pumpQueueLocked();
        }
    }
    if (directlyCancelled) {
        startTasks(toStart);
        notifyUpdate(*directlyCancelled);
        return;
    }
    if (type == DownloadType::Http) httpDownloader_->cancel(id);
    else if (type == DownloadType::Media) mediaDownloader_->cancel(id);
    else torrentEngine_->cancel(id);
}

void DownloadManager::remove(DownloadId id, bool deleteFiles) {
    std::vector<DownloadId> toStart;
    DownloadType type;
    std::string destinationPath;
    {
        std::scoped_lock lock(mutex_);
        auto it = tasks_.find(id);
        if (it == tasks_.end() || busy_.contains(id)) return;
        const auto state = it->second->snapshot().state;
        if (isActiveState(state)) return;
        type = it->second->type();
        // Cancelling already deleted a direct or media download's file; a file at that path
        // now belongs to something else.
        if (state != DownloadState::Cancelled || type == DownloadType::Torrent) {
            destinationPath = it->second->snapshot().destinationPath;
        }
        tasks_.erase(it);
        insertionOrder_.erase(std::remove(insertionOrder_.begin(), insertionOrder_.end(), id),
                               insertionOrder_.end());
        removePersistedTask(id);
        toStart = pumpQueueLocked();
    }
    // Release the engine-side job/torrent-handle now that DownloadManager no longer tracks
    // this task, so removed entries don't linger for the lifetime of the process.
    bool filesHandled = false;
    if (type == DownloadType::Http) httpDownloader_->forget(id);
    else if (type == DownloadType::Media) mediaDownloader_->forget(id);
    else if (deleteFiles) filesHandled = torrentEngine_->forgetAndDeleteFiles(id);
    else torrentEngine_->forget(id);

    if (deleteFiles && !filesHandled && !destinationPath.empty()) {
        deleteDownloadedFiles(type, destinationPath);
    }
    startTasks(toStart);
    notifyRemoved(id);
}

void DownloadManager::deleteDownloadedFiles(DownloadType type, const std::string& pathText) const {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path path(pathText);
    if (fs::is_regular_file(path, ec)) {
        fs::remove(path, ec);
        for (const char* suffix : {".orbitresume", ".part", ".ytdl"}) fs::remove(fs::path(pathText + suffix), ec);
        return;
    }
    // Only a torrent's own content folder is ever deleted recursively - never the
    // download root itself or anything sitting directly under a drive root.
    if (type != DownloadType::Torrent || !fs::is_directory(path, ec)) return;
    const fs::path target = fs::weakly_canonical(path, ec);
    const fs::path root = fs::weakly_canonical(defaultDownloadDir_, ec);
    if (target.empty() || target == root || !target.has_parent_path() || target.parent_path() == target.root_path()) return;
    fs::remove_all(target, ec);
}

std::vector<TaskSnapshot> DownloadManager::snapshot() const {
    std::scoped_lock lock(mutex_);
    std::vector<TaskSnapshot> result;
    result.reserve(tasks_.size());
    for (DownloadId id : insertionOrder_) {
        auto it = tasks_.find(id);
        if (it != tasks_.end()) result.push_back(it->second->snapshot());
    }
    return result;
}

std::optional<TaskSnapshot> DownloadManager::snapshot(DownloadId id) const {
    std::scoped_lock lock(mutex_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return std::nullopt;
    return it->second->snapshot();
}

void DownloadManager::setMaxConcurrentDownloads(int n) {
    std::vector<DownloadId> toStart;
    {
        std::scoped_lock lock(mutex_);
        maxConcurrent_ = std::max(1, n);
        toStart = pumpQueueLocked();
    }
    startTasks(toStart);
}

int DownloadManager::maxConcurrentDownloads() const noexcept {
    std::scoped_lock lock(mutex_);
    return maxConcurrent_;
}

void DownloadManager::setGlobalHttpSpeedLimit(std::uint64_t bytesPerSec) {
    httpDownloader_->setGlobalSpeedLimit(bytesPerSec);
}

void DownloadManager::setSpeedLimit(DownloadId id, std::uint64_t bytesPerSec) {
    std::optional<TaskSnapshot> snap;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task) return;
        task->updateRequest([bytesPerSec](DownloadRequest& r) { r.speedLimitBytesPerSec = bytesPerSec; });
        persistTask(*task);
        if (task->type() == DownloadType::Http) {
            httpDownloader_->setSpeedLimit(id, bytesPerSec);
        } else if (task->type() == DownloadType::Torrent) {
            const auto req = task->request();
            torrentEngine_->setTorrentRateLimits(id, req.speedLimitBytesPerSec, req.uploadLimitBytesPerSec);
        }
        snap = task->snapshot();
    }
    notifyUpdate(*snap);
}

void DownloadManager::setUploadLimit(DownloadId id, std::uint64_t bytesPerSec) {
    std::optional<TaskSnapshot> snap;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task || task->type() != DownloadType::Torrent) return;
        task->updateRequest([bytesPerSec](DownloadRequest& r) { r.uploadLimitBytesPerSec = bytesPerSec; });
        persistTask(*task);
        const auto req = task->request();
        torrentEngine_->setTorrentRateLimits(id, req.speedLimitBytesPerSec, req.uploadLimitBytesPerSec);
        snap = task->snapshot();
    }
    notifyUpdate(*snap);
}

void DownloadManager::setMaxConnections(DownloadId id, int connections) {
    connections = std::clamp(connections, 1, 32);
    std::optional<TaskSnapshot> snap;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task || task->type() != DownloadType::Http) return;
        task->updateRequest([connections](DownloadRequest& r) { r.maxConnections = connections; });
        persistTask(*task);
        httpDownloader_->setMaxConnections(id, connections);
        snap = task->snapshot();
    }
    notifyUpdate(*snap);
}

void DownloadManager::setSequential(DownloadId id, bool sequential) {
    std::optional<TaskSnapshot> snap;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task || task->type() != DownloadType::Torrent) return;
        task->updateRequest([sequential](DownloadRequest& r) { r.sequential = sequential; });
        persistTask(*task);
        torrentEngine_->setSequentialDownload(id, sequential);
        snap = task->snapshot();
    }
    notifyUpdate(*snap);
}

void DownloadManager::setScheduledStart(DownloadId id, std::int64_t unixSeconds) {
    std::optional<TaskSnapshot> snap;
    bool pauseEngine = false;
    DownloadType type = DownloadType::Http;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task) return;
        type = task->type();
        task->updateRequest([unixSeconds](DownloadRequest& r) { r.scheduledStartAt = std::max<std::int64_t>(0, unixSeconds); });
        if (unixSeconds > nowSeconds()) {
            const auto state = task->snapshot().state;
            if (state == DownloadState::Queued) task->setState(DownloadState::Paused);
            else if (state == DownloadState::Downloading || state == DownloadState::Connecting) pauseEngine = true;
        }
        persistTask(*task);
        snap = task->snapshot();
    }
    if (pauseEngine) {
        if (type == DownloadType::Http) httpDownloader_->pause(id);
        else if (type == DownloadType::Media) mediaDownloader_->pause(id);
        else torrentEngine_->pause(id);
    }
    notifyUpdate(*snap);
}

std::string DownloadManager::setExpectedChecksum(DownloadId id, const std::string& checksum) {
    std::string normalized;
    if (!utils::trim(checksum).empty()) {
        const auto spec = utils::parseChecksum(checksum);
        if (!spec) return "That doesn't look like an MD5, SHA-1, SHA-256 or SHA-512 checksum";
        normalized = spec->toString();
    }

    bool verifyNow = false;
    std::optional<TaskSnapshot> snap;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task) return "This download no longer exists";
        if (task->type() == DownloadType::Torrent) return "Torrents are already verified piece by piece";
        task->updateRequest([&normalized](DownloadRequest& r) { r.expectedChecksum = normalized; });
        const auto state = task->snapshot().state;
        if (normalized.empty()) task->setChecksumState(ChecksumState::None);
        else if (state == DownloadState::Completed) verifyNow = true;
        else task->setChecksumState(ChecksumState::Pending);
        persistTask(*task);
        snap = task->snapshot();
    }
    notifyUpdate(*snap);
    if (verifyNow) startChecksumVerification(id);
    return {};
}

void DownloadManager::startChecksumVerification(DownloadId id) {
    std::string path;
    std::string expected;
    std::optional<TaskSnapshot> snap;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task) return;
        expected = task->request().expectedChecksum;
        path = task->snapshot().destinationPath;
        if (expected.empty() || path.empty()) return;
        task->setChecksumState(ChecksumState::Verifying);
        snap = task->snapshot();
    }
    notifyUpdate(*snap);

    runInBackground([this, id, path, expected](std::stop_token stop) {
        const auto spec = utils::parseChecksum(expected);
        std::optional<std::vector<std::string>> digests;
        if (spec) {
            digests = utils::hashFile(path, {spec->algorithm}, [&stop](std::uint64_t, std::uint64_t) {
                return !stop.stop_requested();
            });
        }
        if (stop.stop_requested()) return;

        bool ok = false;
        std::string message;
        std::optional<TaskSnapshot> result;
        {
            std::scoped_lock lock(mutex_);
            DownloadTask* task = findTaskLocked(id);
            if (!task) return;
            const std::string algorithm = spec ? upper(utils::hashAlgorithmName(spec->algorithm)) : std::string("checksum");
            if (!spec || !digests || digests->empty()) {
                message = "Couldn't read the file to verify its checksum";
                task->setChecksumState(ChecksumState::Failed, message);
            } else if ((*digests)[0] == spec->hex) {
                ok = true;
                message = algorithm + " checksum verified";
                task->setChecksumState(ChecksumState::Match, message);
                task->updateRequest([](DownloadRequest& r) { r.cookies.clear(); });
            } else {
                message = algorithm + " mismatch: expected " + spec->hex + ", got " + (*digests)[0];
                task->setChecksumState(ChecksumState::Mismatch, message);
                task->setError("Checksum mismatch - the file doesn't match the expected " + algorithm);
            }
            persistTask(*task);
            result = task->snapshot();
        }
        notifyUpdate(*result);
        notifyNotice(id, ok, message);
    });
}

void DownloadManager::runInBackground(std::function<void(std::stop_token)> work) {
    std::scoped_lock lock(workersMutex_);
    workers_.remove_if([](const Worker& w) { return w.done->load(); });
    auto done = std::make_shared<std::atomic<bool>>(false);
    workers_.push_back(Worker{std::jthread([work = std::move(work), done](std::stop_token stop) {
                                  work(stop);
                                  done->store(true);
                              }),
                              done});
}

std::string DownloadManager::rename(DownloadId id, const std::string& newNameRaw) {
    namespace fs = std::filesystem;
    const std::string newName = utils::trim(newNameRaw);
    if (newName.empty()) return "Enter a name";
    if (utils::sanitizeFileName(newName) != newName || newName == "." || newName == "..") {
        return "A name can't contain any of these characters: \\ / : * ? \" < > |";
    }

    std::optional<TaskSnapshot> snap;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task) return "This download no longer exists";
        if (busy_.contains(id)) return "Wait for the current move to finish";
        const TaskSnapshot current = task->snapshot();
        const DownloadType type = task->type();
        if (current.name == newName) return {};

        if (type == DownloadType::Torrent && torrentEngine_->hasEntry(id)) {
            if (std::string error = torrentEngine_->rename(id, newName); !error.empty()) return error;
            task->setName(newName);
            task->updateRequest([&newName](DownloadRequest& r) { r.suggestedName = newName; });
            persistTask(*task);
            snap = task->snapshot();
        } else {
            if (type != DownloadType::Torrent && (current.state == DownloadState::Downloading || current.state == DownloadState::Connecting)) {
                return "Pause the download before renaming it";
            }
            std::error_code ec;
            const fs::path oldPath(current.destinationPath);
            const bool onDisk = !current.destinationPath.empty() && fs::exists(oldPath, ec);
            if (type == DownloadType::Media && current.state != DownloadState::Completed && onDisk) {
                return "A media download can be renamed once it has finished";
            }
            if (onDisk) {
                const fs::path newPath = oldPath.parent_path() / fs::path(newName);
                if (fs::exists(newPath, ec)) return "Something with that name already exists in this folder";
                fs::rename(oldPath, newPath, ec);
                if (ec) return "Couldn't rename: " + ec.message();
                fs::path oldMeta = oldPath;
                oldMeta += ".orbitresume";
                if (fs::exists(oldMeta, ec)) {
                    fs::path newMeta = newPath;
                    newMeta += ".orbitresume";
                    fs::rename(oldMeta, newMeta, ec);
                }
                task->setDestinationPath(newPath);
                const bool incomplete = current.state != DownloadState::Completed;
                task->updateRequest([&](DownloadRequest& r) {
                    r.suggestedName = newName;
                    if (type == DownloadType::Http && incomplete) r.existingFilePath = newPath.string();
                });
                if (type == DownloadType::Http) httpDownloader_->relocate(id, newPath);
            } else {
                task->updateRequest([&newName](DownloadRequest& r) { r.suggestedName = newName; });
            }
            task->setName(newName);
            persistTask(*task);
            snap = task->snapshot();
        }
    }
    notifyUpdate(*snap);
    return {};
}

std::string DownloadManager::move(DownloadId id, const std::string& directory) {
    namespace fs = std::filesystem;
    if (utils::trim(directory).empty()) return "Choose a folder";
    const fs::path targetDir(directory);

    fs::path from;
    fs::path to;
    DownloadType type;
    bool incomplete = false;
    std::optional<TaskSnapshot> snap;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task) return "This download no longer exists";
        if (busy_.contains(id)) return "This download is already being moved";
        const TaskSnapshot current = task->snapshot();
        type = task->type();

        std::error_code ec;
        const fs::path currentPath(current.destinationPath);
        if (!current.destinationPath.empty() && fs::equivalent(currentPath.parent_path(), targetDir, ec)) {
            return "It's already in that folder";
        }

        if (type == DownloadType::Torrent && torrentEngine_->hasEntry(id)) {
            task->updateRequest([&directory](DownloadRequest& r) { r.destinationDir = directory; });
            persistTask(*task);
            return torrentEngine_->moveStorage(id, directory);
        }
        if (type != DownloadType::Torrent && (current.state == DownloadState::Downloading || current.state == DownloadState::Connecting)) {
            return "Pause the download before moving it";
        }

        const bool onDisk = !current.destinationPath.empty() && fs::exists(currentPath, ec);
        if (type == DownloadType::Media && current.state != DownloadState::Completed && onDisk) {
            return "A media download can be moved once it has finished";
        }
        if (!onDisk) {
            // Nothing written yet: just point the download at the new folder.
            task->updateRequest([&directory](DownloadRequest& r) { r.destinationDir = directory; });
            persistTask(*task);
            snap = task->snapshot();
        } else {
            if (!utils::ensureDirectoryExists(targetDir)) return "Couldn't create or open that folder";
            from = currentPath;
            to = utils::uniquePath(targetDir / currentPath.filename());
            incomplete = current.state != DownloadState::Completed;
            busy_.insert(id);
        }
    }
    if (snap) {
        notifyUpdate(*snap);
        return {};
    }

    runInBackground([this, id, from, to, type, incomplete, directory](std::stop_token stop) {
        std::string error = movePath(from, to, [&stop] { return stop.stop_requested(); });
        if (error.empty() && type == DownloadType::Http && incomplete) {
            fs::path oldMeta = from;
            oldMeta += ".orbitresume";
            fs::path newMeta = to;
            newMeta += ".orbitresume";
            std::error_code ec;
            if (fs::exists(oldMeta, ec)) utils::moveFile(oldMeta, newMeta);
        }

        std::optional<TaskSnapshot> result;
        {
            std::scoped_lock lock(mutex_);
            busy_.erase(id);
            DownloadTask* task = findTaskLocked(id);
            if (task && error.empty()) {
                task->setDestinationPath(to);
                task->updateRequest([&](DownloadRequest& r) {
                    r.destinationDir = directory;
                    if (type == DownloadType::Http && incomplete) r.existingFilePath = to.string();
                });
                if (type == DownloadType::Http) httpDownloader_->relocate(id, to);
                persistTask(*task);
                result = task->snapshot();
            }
        }
        if (result) notifyUpdate(*result);
        notifyNotice(id, error.empty(), error.empty() ? "Moved to " + directory : error);
    });
    return {};
}

void DownloadManager::setDefaultDownloadDir(std::filesystem::path dir) {
    std::scoped_lock lock(mutex_);
    defaultDownloadDir_ = std::move(dir);
    utils::ensureDirectoryExists(defaultDownloadDir_);
}

const std::filesystem::path& DownloadManager::defaultDownloadDir() const noexcept {
    return defaultDownloadDir_;
}

void DownloadManager::setProxy(const ProxySettings& proxy) {
    httpDownloader_->setProxy(proxy);
    torrentEngine_->setProxy(proxy);
    mediaDownloader_->setProxy(proxy);
}

std::vector<DownloadManager::TaskRecord> DownloadManager::exportRecords() const {
    std::scoped_lock lock(mutex_);
    std::vector<TaskRecord> records;
    for (DownloadId id : insertionOrder_) {
        auto it = tasks_.find(id);
        if (it != tasks_.end()) records.push_back(recordFor(*it->second, std::nullopt, false));
    }
    return records;
}

int DownloadManager::importRecords(const std::vector<TaskRecord>& records) {
    std::vector<TaskSnapshot> added;
    {
        std::scoped_lock lock(mutex_);
        for (const TaskRecord& record : records) {
            Loaded rec = parseRecord(record);
            if (rec.request.source.empty()) continue;
            // The same link can legitimately appear twice (a video and its audio-only
            // copy), so only an entry with the same link and the same name is a duplicate.
            const bool duplicate = std::any_of(tasks_.begin(), tasks_.end(), [&](const auto& entry) {
                return entry.second->type() == rec.type && entry.second->request().source == rec.request.source &&
                       entry.second->snapshot().name == rec.name;
            });
            if (duplicate) continue;

            const DownloadId id = nextId_++;
            if (rec.state != DownloadState::Completed && rec.state != DownloadState::Cancelled &&
                rec.state != DownloadState::Error) {
                rec.state = DownloadState::Paused;
            }
            auto task = taskFromLoaded(rec, id);
            if (rec.state == DownloadState::Error) task->setError(rec.errorMessage);
            else task->setState(rec.state);
            persistTask(*task);
            added.push_back(task->snapshot());
            tasks_.emplace(id, std::move(task));
            insertionOrder_.push_back(id);
        }
    }
    for (const TaskSnapshot& snap : added) notifyUpdate(snap);
    return static_cast<int>(added.size());
}

int DownloadManager::subscribeUpdates(UpdateCallback callback) {
    std::scoped_lock lock(subsMutex_);
    const int token = nextSubToken_++;
    updateSubs_[token] = std::move(callback);
    return token;
}

int DownloadManager::subscribeRemovals(RemovedCallback callback) {
    std::scoped_lock lock(subsMutex_);
    const int token = nextSubToken_++;
    removalSubs_[token] = std::move(callback);
    return token;
}

int DownloadManager::subscribeNotices(NoticeCallback callback) {
    std::scoped_lock lock(subsMutex_);
    const int token = nextSubToken_++;
    noticeSubs_[token] = std::move(callback);
    return token;
}

void DownloadManager::unsubscribe(int token) {
    std::scoped_lock lock(subsMutex_);
    updateSubs_.erase(token);
    removalSubs_.erase(token);
    noticeSubs_.erase(token);
}

void DownloadManager::notifyUpdate(const TaskSnapshot& snap) {
    std::scoped_lock lock(subsMutex_);
    for (auto& [token, cb] : updateSubs_) {
        if (cb) cb(snap);
    }
}

void DownloadManager::notifyRemoved(DownloadId id) {
    std::scoped_lock lock(subsMutex_);
    for (auto& [token, cb] : removalSubs_) {
        if (cb) cb(id);
    }
}

void DownloadManager::notifyNotice(DownloadId id, bool ok, const std::string& message) {
    if (!ok) ORBIT_LOG_WARN("DownloadManager", "Task " + std::to_string(id) + ": " + message);
    std::scoped_lock lock(subsMutex_);
    for (auto& [token, cb] : noticeSubs_) {
        if (cb) cb(id, ok, message);
    }
}

void DownloadManager::onProgress(DownloadId id, const DownloadStats& stats) {
    std::optional<TaskSnapshot> snap;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task) return;
        task->updateStats(stats);
        snap = task->snapshot();
    }
    if (snap) notifyUpdate(*snap);
}

void DownloadManager::onStateChanged(DownloadId id, DownloadState state) {
    std::optional<TaskSnapshot> snap;
    std::vector<DownloadId> toStart;
    bool verify = false;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task) return;
        const DownloadState previous = task->snapshot().state;
        task->setState(state);
        if (state == DownloadState::Completed && previous != DownloadState::Completed && task->type() != DownloadType::Torrent) {
            // Browser cookies are only needed to fetch the file; don't keep them on disk
            // afterwards unless a checksum check might still send the download back.
            if (task->request().expectedChecksum.empty()) {
                task->updateRequest([](DownloadRequest& r) { r.cookies.clear(); });
            } else {
                verify = true;
            }
        }
        persistTask(*task);
        snap = task->snapshot();
        // Any of these transitions frees up a concurrency slot that a queued task may
        // now use. Paused was previously missing here, so pausing an active download
        // would not let the next queued item start until something else happened to
        // re-pump the queue.
        if (state == DownloadState::Completed || state == DownloadState::Error ||
            state == DownloadState::Cancelled || state == DownloadState::Paused) {
            toStart = pumpQueueLocked();
        }
    }
    startTasks(toStart);
    if (snap) notifyUpdate(*snap);
    if (verify) startChecksumVerification(id);
}

void DownloadManager::onNameResolved(DownloadId id, const std::string& name, const std::string& path) {
    std::optional<TaskSnapshot> snap;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task) return;
        if (!name.empty()) task->setName(name);
        if (!path.empty()) task->setDestinationPath(path);
        persistTask(*task);
        snap = task->snapshot();
    }
    if (snap) notifyUpdate(*snap);
}

void DownloadManager::onFileConflict(DownloadId id, const std::string& existingPath) {
    std::optional<TaskSnapshot> snap;
    std::vector<DownloadId> toStart;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task) return;
        task->setState(DownloadState::Paused);
        task->setFileConflictPath(existingPath);
        // Nothing was written yet, so no file on disk belongs to this download (a path left
        // over from an earlier attempt must not be deleted along with it).
        task->setDestinationPath({});
        persistTask(*task);
        snap = task->snapshot();
        // The engine job ended before choosing a file. Dropping it means the next start
        // probes again and applies whatever the user decides.
        httpDownloader_->forget(id);
        toStart = pumpQueueLocked();
    }
    startTasks(toStart);
    if (snap) notifyUpdate(*snap);
}

void DownloadManager::setDefaultFileConflictAction(FileConflictAction action) {
    std::scoped_lock lock(mutex_);
    defaultConflict_ = action == FileConflictAction::Default ? FileConflictAction::Rename : action;
}

FileConflictAction DownloadManager::defaultFileConflictAction() const {
    std::scoped_lock lock(mutex_);
    return defaultConflict_;
}

std::string DownloadManager::resolveFileConflict(DownloadId id, FileConflictAction action) {
    if (action != FileConflictAction::Rename && action != FileConflictAction::Overwrite) return "Choose Replace or Keep Both";
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task) return "This download no longer exists";
        if (task->snapshot().fileConflictPath.empty()) return {};
        task->updateRequest([action](DownloadRequest& r) { r.fileConflict = action; });
        task->setFileConflictPath({});
        persistTask(*task);
    }
    ORBIT_LOG_INFO("DownloadManager", "Task " + std::to_string(id) + ": existing file " +
                                          (action == FileConflictAction::Overwrite ? "will be replaced" : "is kept, new file renamed"));
    resume(id);
    return {};
}

void DownloadManager::probeLink(DownloadId id, const DownloadRequest& replacement, std::function<void(const RemoteProbe&)> done) {
    DownloadRequest request;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task) {
            RemoteProbe missing;
            missing.error = "This download no longer exists";
            done(missing);
            return;
        }
        request = task->request();
    }
    request.source = replacement.source;
    if (!replacement.cookies.empty()) request.cookies = replacement.cookies;
    if (!replacement.userAgent.empty()) request.userAgent = replacement.userAgent;
    if (!replacement.referrer.empty()) request.referrer = replacement.referrer;

    runInBackground([this, request = std::move(request), done = std::move(done)](std::stop_token stop) {
        const RemoteProbe result = httpDownloader_->probe(request);
        if (!stop.stop_requested()) done(result);
    });
}

std::string DownloadManager::replaceLink(DownloadId id, const DownloadRequest& replacement, bool startOver) {
    namespace fs = std::filesystem;
    const std::string url = utils::trim(replacement.source);
    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }(url);
    if (lower.rfind("http://", 0) != 0 && lower.rfind("https://", 0) != 0) return "Enter an http:// or https:// link";

    std::optional<TaskSnapshot> snap;
    std::vector<DownloadId> toStart;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task) return "This download no longer exists";
        if (task->type() != DownloadType::Http) return "Only direct downloads can get a new link";
        if (busy_.contains(id)) return "Wait for the current move to finish";
        const TaskSnapshot current = task->snapshot();
        if (isActiveState(current.state)) return "Pause the download before changing its link";
        if (current.state == DownloadState::Completed) return "This download has already finished";

        // A cancelled download's file was deleted, and whatever sits at that path now isn't ours.
        std::error_code ec;
        const std::string partial = current.destinationPath;
        const bool hasPartial = !partial.empty() && current.fileConflictPath.empty() &&
                                current.state != DownloadState::Cancelled && fs::is_regular_file(partial, ec);
        const fs::path sidecar = fs::path(partial + ".orbitresume");

        if (startOver && hasPartial) {
            fs::remove(partial, ec);
            fs::remove(sidecar, ec);
        } else if (hasPartial) {
            HttpDownloader::rewriteResumeSource(sidecar, url);
        }

        task->updateRequest([&](DownloadRequest& r) {
            r.source = url;
            if (!replacement.cookies.empty()) r.cookies = replacement.cookies;
            if (!replacement.userAgent.empty()) r.userAgent = replacement.userAgent;
            if (!replacement.referrer.empty()) r.referrer = replacement.referrer;
            r.scheduledStartAt = 0;
            if (hasPartial && !startOver) {
                r.existingFilePath = partial;
            } else {
                r.existingFilePath.clear();
                // Keep the name the user knows this download by, even if the new link's
                // server suggests another one.
                if (!current.name.empty()) r.suggestedName = current.name;
            }
        });
        task->setSource(url);
        if (startOver || !hasPartial) {
            DownloadStats cleared;
            cleared.totalBytes = startOver ? 0 : current.stats.totalBytes;
            task->updateStats(cleared);
        }
        if (current.checksumState == ChecksumState::Mismatch) task->setChecksumState(ChecksumState::Pending);
        task->setState(DownloadState::Queued);
        persistTask(*task);
        // Any engine-side job still holds the old link in memory; the fresh start reads the
        // new one (and the rewritten resume data) from the task.
        httpDownloader_->forget(id);
        toStart = pumpQueueLocked();
        snap = task->snapshot();
    }
    ORBIT_LOG_INFO("DownloadManager", "Task " + std::to_string(id) + " has a new link" + (startOver ? " (starting over)" : ""));
    startTasks(toStart);
    if (snap) notifyUpdate(*snap);
    return {};
}

void DownloadManager::onError(DownloadId id, const std::string& message) {
    std::optional<TaskSnapshot> snap;
    {
        std::scoped_lock lock(mutex_);
        DownloadTask* task = findTaskLocked(id);
        if (!task) return;
        task->setError(message);
        persistTask(*task);
        snap = task->snapshot();
    }
    if (snap) {
        ORBIT_LOG_ERROR("DownloadManager", "Task " + std::to_string(id) + " error: " + message);
        notifyUpdate(*snap);
    }
}

}
