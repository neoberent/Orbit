#pragma once

#include "core/Types.hpp"
#include "core/DownloadTask.hpp"
#include <atomic>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace orbit::core {

class HttpDownloader;
class TorrentEngine;
class MediaDownloader;

class DownloadManager {
public:
    using UpdateCallback = std::function<void(const TaskSnapshot&)>;
    using RemovedCallback = std::function<void(DownloadId)>;
    // Outcome of an operation that finishes in the background (move, checksum, ...).
    using NoticeCallback = std::function<void(DownloadId, bool ok, const std::string& message)>;
    // One task as ordered key/value pairs: the same shape as a .task file.
    using TaskRecord = std::vector<std::pair<std::string, std::string>>;

    explicit DownloadManager(std::filesystem::path stateDir);
    ~DownloadManager();

    DownloadManager(const DownloadManager&) = delete;
    DownloadManager& operator=(const DownloadManager&) = delete;

    DownloadId addHttpDownload(DownloadRequest request);

    DownloadId addTorrent(DownloadRequest request);

    DownloadId addMediaDownload(DownloadRequest request);

    void pause(DownloadId id);
    void resume(DownloadId id);
    void cancel(DownloadId id);

    // Removes the task from the list. With deleteFiles, its downloaded file (or a
    // torrent's content) is deleted from disk as well.
    void remove(DownloadId id, bool deleteFiles = false);

    [[nodiscard]] std::vector<TaskSnapshot> snapshot() const;
    [[nodiscard]] std::optional<TaskSnapshot> snapshot(DownloadId id) const;

    void setMaxConcurrentDownloads(int n);
    [[nodiscard]] int maxConcurrentDownloads() const noexcept;

    // 0 = unlimited. Applies to the combined throughput of every active HTTP
    // download. Torrent throttling is a separate mechanism
    // (see torrentEngine().setDownloadRateLimit) and is untouched by this.
    void setGlobalHttpSpeedLimit(std::uint64_t bytesPerSec);

    // Per-download settings. They're saved with the task, so they survive a retry or a
    // restart. 0 = unlimited. A media download picks up a new limit the next time it starts.
    void setSpeedLimit(DownloadId id, std::uint64_t bytesPerSec);
    void setUploadLimit(DownloadId id, std::uint64_t bytesPerSec);
    void setMaxConnections(DownloadId id, int connections);
    void setSequential(DownloadId id, bool sequential);
    // A future time keeps the task paused until then; 0 clears the schedule.
    void setScheduledStart(DownloadId id, std::int64_t unixSeconds);

    // Returns an error message, or an empty string. An empty checksum clears it; setting
    // one on a finished download verifies it right away.
    std::string setExpectedChecksum(DownloadId id, const std::string& checksum);

    // Both return an error message, or an empty string once the change is done or queued
    // (a move across drives or of a torrent finishes in the background and reports
    // through subscribeNotices).
    std::string rename(DownloadId id, const std::string& newName);
    std::string move(DownloadId id, const std::string& directory);

    void setDefaultDownloadDir(std::filesystem::path dir);
    [[nodiscard]] const std::filesystem::path& defaultDownloadDir() const noexcept;

    void setProxy(const ProxySettings& proxy);

    // What a direct download does when its file name is already taken, unless the task
    // carries its own choice. Applies to downloads that haven't picked a file yet.
    void setDefaultFileConflictAction(FileConflictAction action);
    [[nodiscard]] FileConflictAction defaultFileConflictAction() const;

    // Answers a download waiting on an existing file (snapshot.fileConflictPath): Rename
    // keeps both, Overwrite replaces the existing file. The download then continues.
    // Returns an error message, or an empty string.
    std::string resolveFileConflict(DownloadId id, FileConflictAction action);

    // Refreshing an expired link. probeLink asks the server about `replacement.source`
    // (with the task's cookies, or the replacement's own when it carries any) and calls
    // `done` from a background thread. replaceLink swaps the link on a direct download that
    // isn't running and queues it again: the bytes already downloaded are kept, unless
    // startOver is set, which deletes the partial file first. Returns an error message,
    // or an empty string.
    void probeLink(DownloadId id, const DownloadRequest& replacement, std::function<void(const RemoteProbe&)> done);
    std::string replaceLink(DownloadId id, const DownloadRequest& replacement, bool startOver);

    [[nodiscard]] std::vector<TaskRecord> exportRecords() const;
    // Adds tasks from a backup, skipping ones already in the list. Unfinished imports
    // arrive paused. Returns how many were added.
    int importRecords(const std::vector<TaskRecord>& records);

    int subscribeUpdates(UpdateCallback callback);
    int subscribeRemovals(RemovedCallback callback);
    int subscribeNotices(NoticeCallback callback);
    void unsubscribe(int token);

    [[nodiscard]] TorrentEngine& torrentEngine() noexcept { return *torrentEngine_; }
    [[nodiscard]] MediaDownloader& mediaDownloader() noexcept { return *mediaDownloader_; }

private:
    struct Loaded;

    void deleteDownloadedFiles(DownloadType type, const std::string& pathText) const;
    void onProgress(DownloadId id, const DownloadStats& stats);
    void onStateChanged(DownloadId id, DownloadState state);
    void onError(DownloadId id, const std::string& message);
    void onNameResolved(DownloadId id, const std::string& name, const std::string& path);
    void onFileConflict(DownloadId id, const std::string& existingPath);

    void notifyUpdate(const TaskSnapshot& snap);
    void notifyRemoved(DownloadId id);
    void notifyNotice(DownloadId id, bool ok, const std::string& message);

    DownloadId addTask(DownloadType type, DownloadRequest request, const std::string& initialName);

    [[nodiscard]] std::vector<DownloadId> pumpQueueLocked();

    void startTasks(const std::vector<DownloadId>& ids);

    [[nodiscard]] DownloadTask* findTaskLocked(DownloadId id);
    [[nodiscard]] bool engineHasJobLocked(DownloadType type, DownloadId id) const;

    [[nodiscard]] TaskRecord recordFor(const DownloadTask& task, std::optional<DownloadState> stateOverride,
                                       bool includeCookies) const;
    [[nodiscard]] static Loaded parseRecord(const TaskRecord& record);
    [[nodiscard]] std::unique_ptr<DownloadTask> taskFromLoaded(const Loaded& rec, DownloadId id) const;
    void persistTask(const DownloadTask& task, std::optional<DownloadState> stateOverride = std::nullopt) const;
    void removePersistedTask(DownloadId id) const;
    void loadPersistedTasksAndResume();

    void startChecksumVerification(DownloadId id);
    void runInBackground(std::function<void(std::stop_token)> work);

    mutable std::mutex mutex_;
    std::unordered_map<DownloadId, std::unique_ptr<DownloadTask>> tasks_;
    std::vector<DownloadId> insertionOrder_;
    std::unordered_set<DownloadId> busy_;
    DownloadId nextId_{1};
    int maxConcurrent_{3};
    FileConflictAction defaultConflict_{FileConflictAction::Rename};
    std::filesystem::path defaultDownloadDir_;
    std::filesystem::path stateDir_;

    std::mutex subsMutex_;
    std::unordered_map<int, UpdateCallback> updateSubs_;
    std::unordered_map<int, RemovedCallback> removalSubs_;
    std::unordered_map<int, NoticeCallback> noticeSubs_;
    int nextSubToken_{1};

    struct Worker {
        std::jthread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex workersMutex_;
    std::list<Worker> workers_;

    // Declared last so C++'s reverse-declaration-order destruction tears these down
    // FIRST. Their background threads (still running while they're being stopped/
    // joined) call back into onProgress/onStateChanged/etc., which touch subsMutex_
    // and the maps above — those must still be alive when that happens.
    std::unique_ptr<HttpDownloader> httpDownloader_;
    std::unique_ptr<TorrentEngine> torrentEngine_;
    std::unique_ptr<MediaDownloader> mediaDownloader_;
};

}
