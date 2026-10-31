// DownloadManager's task persistence: each task is saved as a small key=value ".task"
// file in the state folder, restored (and resumed) on the next launch, and the same
// records back the settings export and import.
#include "core/DownloadManager.hpp"

#include "core/HttpDownloader.hpp"
#include "utils/Hashing.hpp"
#include "utils/Logger.hpp"
#include "utils/StringUtils.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <system_error>
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

}
