#pragma once

#include "core/IDownloader.hpp"
#include "utils/SpeedLimiter.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace orbit::core {

class HttpDownloader final : public IDownloader {
public:
    explicit HttpDownloader(ProgressReporter reporter);
    ~HttpDownloader() override;

    HttpDownloader(const HttpDownloader&) = delete;
    HttpDownloader& operator=(const HttpDownloader&) = delete;

    void start(DownloadId id, const DownloadRequest& request) override;
    void pause(DownloadId id) override;
    void resume(DownloadId id) override;
    void cancel(DownloadId id) override;
    void forget(DownloadId id) override;
    [[nodiscard]] DownloadStats stats(DownloadId id) const override;

    [[nodiscard]] bool hasJob(DownloadId id) const;

    // 0 = unlimited. Applies to the combined throughput of every active HTTP
    // download, split further by each one's own per-download cap (if any).
    void setGlobalSpeedLimit(std::uint64_t bytesPerSec);

    // 0 = unlimited. Caps this one download; shared evenly across its active
    // segments and recomputed as rebalancing adds/removes them.
    void setSpeedLimit(DownloadId id, std::uint64_t bytesPerSec);

    // Changes how many segments a running (or paused) job may use from now on.
    void setMaxConnections(DownloadId id, int connections);

    // Points a job that isn't currently running at a renamed/moved file (and its
    // .orbitresume sidecar), so a later resume continues writing in the new place.
    void relocate(DownloadId id, const std::filesystem::path& newFilePath);

    void setProxy(const ProxySettings& proxy);

    // Asks the server about a link (size, range support, file name) without downloading
    // it, using the request's cookies, referrer and user agent. Blocks for up to ~50 s.
    [[nodiscard]] RemoteProbe probe(const DownloadRequest& request) const;

    // Points a partial download's .orbitresume sidecar at a new link, so a resume picks
    // up the bytes already on disk instead of starting over. False if there's no sidecar.
    static bool rewriteResumeSource(const std::filesystem::path& metaPath, const std::string& newSource);

    // Serializes a cookie into the Netscape cookies.txt line format curl and yt-dlp read.
    [[nodiscard]] static std::string netscapeCookieLine(const HttpCookie& cookie);

private:
    struct Segment {
        std::uint64_t startOffset{0};
        // Written by the supervisor thread when it shrinks this segment to hand its
        // tail to a new helper segment (see rebalanceSegments); read by this segment's
        // own worker thread between chunks. Everything else here is only ever touched
        // by the one worker thread that owns this Segment.
        std::atomic<std::uint64_t> endOffset{0};
        std::atomic<std::uint64_t> bytesDownloaded{0};
        std::atomic<bool> finished{false};
        std::atomic<bool> success{false};
        std::thread thread;
    };

    struct Job {
        DownloadId id{0};
        DownloadRequest request;
        std::filesystem::path filePath;
        std::filesystem::path metaPath;
        std::string resolvedFileName;

        std::atomic<bool> paused{false};
        std::atomic<bool> cancelStart{false};
        std::atomic<bool> running{false};
        std::atomic<bool> segmentError{false};
        // Set once the file is complete. A finished job no longer claims its path, so a
        // later download with the same name can replace it or ask about it.
        std::atomic<bool> completed{false};

        std::uint64_t totalBytes{0};
        bool supportsRanges{false};
        // The connection count this job was started (or resumed) with. Rebalancing
        // never grows the number of concurrently active segments past this.
        // Written by segment threads when a server throttles us (HTTP 429/503), read by
        // the supervisor's rebalancing, hence atomic.
        std::atomic<int> desiredConnections{1};
        // Count of not-yet-finished segments, maintained by whichever thread starts
        // or finishes one. Lets a segment's own worker thread learn how many peers
        // it's currently sharing the per-download speed cap with, without touching
        // the segments vector itself (only the supervisor thread does that).
        std::atomic<int> activeSegments{0};
        // 0 = unlimited.
        orbit::utils::SpeedLimiter limiter;

        std::vector<std::unique_ptr<Segment>> segments;

        mutable std::mutex statsMutex;
        DownloadStats lastStats;
        std::string lastErrorMessage;
        std::uint64_t bytesAtLastSample{0};
        std::chrono::steady_clock::time_point lastSampleTime;
        std::chrono::steady_clock::time_point startTime;
    };

    // Picks the file for a fresh download whose name is `desired`, applying the conflict
    // policy. Returns an empty path when the user has to be asked first.
    [[nodiscard]] std::filesystem::path resolveTargetPath(const Job& job, const std::filesystem::path& desired) const;

    void runSupervisor(DownloadId id);
    void downloadSegment(Job& job, Segment& segment, int segmentIndex);
    // Called from the supervisor's poll loop only, so it's the sole writer of
    // job.segments (worker threads only ever touch their own Segment&) and the sole
    // writer of a donor segment's endOffset.
    void rebalanceSegments(Job& job);

    bool probeRemote(const DownloadRequest& request, std::uint64_t& outSize,
                     bool& outSupportsRanges, std::string& outFileName,
                     std::string& outError) const;
    void applyRequestOptions(void* curl, const DownloadRequest& request) const;

    void saveResumeMetadata(Job& job) const;
    bool loadResumeMetadata(Job& job) const;
    void sampleAndReportProgress(Job& job);
    void finalizeJob(DownloadId id, bool success, const std::string& error = {});

    ProgressReporter reporter_;
    mutable std::mutex jobsMutex_;
    std::unordered_map<DownloadId, std::unique_ptr<Job>> jobs_;
    orbit::utils::SpeedLimiter globalLimiter_;
    mutable std::mutex proxyMutex_;
    ProxySettings proxy_;
};

}
