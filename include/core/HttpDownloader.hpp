#pragma once

#include "core/IDownloader.hpp"
#include "utils/SpeedLimiter.hpp"
#include <atomic>
#include <map>
#include <deque>
#include <chrono>
#include <condition_variable>
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
    // Every connection goes out through this interface only (curl's "if!" binding), or
    // fails while it's down. Empty = any interface.
    void setNetworkInterface(const std::string& interfaceName);
    // Folder per file type ("video", "audio", "image", "document", "archive", "program"),
    // used for downloads marked sortByType. A type without one stays in destinationDir.
    void setTypeFolders(std::map<std::string, std::string> folders);
    // The type a file name belongs to, as used for the folders above ("" for other files).
    [[nodiscard]] static std::string fileTypeOf(const std::string& fileName);

    // A connection that delivers no data for this long is treated as dropped and
    // retried, instead of waiting on a silent server forever. Default 60 s.
    void setStallTimeout(std::chrono::seconds timeout);
    // How often a running download writes a crash-safe resume checkpoint. Default 15 s.
    void setCheckpointInterval(std::chrono::milliseconds interval);

    // Asks the server about a link (size, range support, file name) without downloading
    // it, using the request's cookies, referrer and user agent. Blocks for up to ~50 s.
    [[nodiscard]] RemoteProbe probe(const DownloadRequest& request) const;

    // Fetches a small document (a feed, a .torrent file) through the same proxy, interface
    // lock and DNS as downloads. Fails past maxBytes. Blocks; run it in the background.
    struct Fetched {
        bool ok{false};
        long status{0};
        std::string body;
        std::string error;
    };
    [[nodiscard]] Fetched fetchSmall(const DownloadRequest& request, std::size_t maxBytes) const;
    // Release files (disk images, archives, packages, installers) are the ones projects
    // publish checksums and signatures beside, as <link>.sha256, <link>.asc and so on.
    [[nodiscard]] static bool isReleaseFileName(const std::string& fileName);

    // Points a partial download's .orbitresume sidecar at a new link, so a resume picks
    // up the bytes already on disk instead of starting over. False if there's no sidecar.
    static bool rewriteResumeSource(const std::filesystem::path& metaPath, const std::string& newSource);

    // Serializes a cookie into the Netscape cookies.txt line format curl and yt-dlp read.
    [[nodiscard]] static std::string netscapeCookieLine(const HttpCookie& cookie);
    // Points a curl handle at the system's certificates when curl's built-in path isn't
    // where this system keeps them (a bundled curl, as in the AppImage).
    static void applyTrustStore(void* curl);

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
        // The link this connection fetches from (the source or a mirror); empty = source.
        std::string url;
    };

    // What tells one version of a file on a server from another besides its size: a
    // strong ETag and Last-Modified. Kept in the resume data, so a file replaced by one of
    // the same size isn't finished with the old one's parts.
    struct FileIdentity {
        std::string etag;
        std::string lastModified;
        // Last-Modified decides when both sides have it (it's the same across a CDN's
        // servers, where ETags often aren't); otherwise the ETag; with neither, no change.
        [[nodiscard]] bool changedFrom(const FileIdentity& before) const {
            if (!lastModified.empty() && !before.lastModified.empty()) return lastModified != before.lastModified;
            if (!etag.empty() && !before.etag.empty()) return etag != before.etag;
            return false;
        }
    };

    struct Job {
        DownloadId id{0};
        DownloadRequest request;
        FileIdentity identity;
        std::filesystem::path filePath;
        std::filesystem::path metaPath;
        std::string resolvedFileName;

        std::atomic<bool> paused{false};
        std::atomic<bool> cancelStart{false};
        std::atomic<bool> running{false};
        std::atomic<bool> segmentError{false};
        // A range request came back as something other than that range (a whole-file 200
        // or an oversized 206). The parts so far can't be trusted, so the job starts over
        // in one piece, and rangesUnreliable keeps it from trying ranges again.
        std::atomic<bool> rangeIgnored{false};
        bool rangesUnreliable{false};
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
        // Requests on the wire right now. A segment takes a slot (below desiredConnections)
        // before each request, so once a server has said how many connections it allows,
        // the other segments wait their turn instead of being turned away and backing off.
        std::atomic<int> requestsInFlight{0};
        // 0 = unlimited.
        orbit::utils::SpeedLimiter limiter;

        std::vector<std::unique_ptr<Segment>> segments;
        // The source and the mirrors that checked out (same size, byte ranges), in the
        // order connections are spread over them. Filled before the segments start.
        std::vector<std::string> sources;
        bool sourcesChecked{false};

        mutable std::mutex statsMutex;
        DownloadStats lastStats;
        std::string lastErrorMessage;
        std::uint64_t bytesAtLastSample{0};
        std::chrono::steady_clock::time_point lastSampleTime;
        // (time, bytes) over the last couple of seconds: the speed shown is measured across
        // this window, so it doesn't jump with every 400 ms sample.
        std::deque<std::pair<std::chrono::steady_clock::time_point, std::uint64_t>> rateWindow;
        std::chrono::steady_clock::time_point startTime;
    };

    // Picks the file for a fresh download whose name is `desired`, applying the conflict
    // policy. Returns an empty path when the user has to be asked first.
    [[nodiscard]] std::filesystem::path resolveTargetPath(const Job& job, const std::filesystem::path& desired) const;

    // Runs runSupervisor(id) on a detached thread that the destructor still waits for.
    // The caller must already have set the job's `running` flag, so neither a second
    // resume() nor forget()/start() can touch the job before the supervisor picks it up.
    void launchSupervisor(DownloadId id);
    void runSupervisor(DownloadId id);
    void downloadSegment(Job& job, Segment& segment, int segmentIndex);
    // Called from the supervisor's poll loop only, so it's the sole writer of
    // job.segments (worker threads only ever touch their own Segment&) and the sole
    // writer of a donor segment's endOffset.
    void rebalanceSegments(Job& job);

    // `job`, when given, lets the probe give up as soon as that job is paused or cancelled
    // (it always gives up on shutdown).
    bool probeRemote(const DownloadRequest& request, std::uint64_t& outSize,
                     bool& outSupportsRanges, std::string& outFileName,
                     std::string& outError, const Job* job = nullptr, std::string* outChecksum = nullptr,
                     FileIdentity* outIdentity = nullptr) const;
    // A checksum published next to the file (name.sha256 / .sha512 / .sha256sum), looked
    // for when the link looks like a release download. Empty when there's none.
    [[nodiscard]] std::string fetchPublishedChecksum(const DownloadRequest& request, const std::string& fileName) const;
    void applyRequestOptions(void* curl, const DownloadRequest& request) const;

    // curl_easy_perform, except that with downloads locked to an interface every host is
    // looked up through that interface (InterfaceDns) and handed to curl, and curl may
    // never use the system resolver. A redirect to a new host is looked up and retried.
    // Returns the CURLcode (as int: this header doesn't include curl).
    [[nodiscard]] int perform(void* curl, const std::string& url) const;

    // whileRunning: a checkpoint taken during the download (so a crash or power cut can
    // resume). Running segments are recorded a little behind where they are, since their
    // last writes may not have reached the disk yet; the file's data is synced first.
    void saveResumeMetadata(Job& job, bool whileRunning = false) const;
    bool loadResumeMetadata(Job& job) const;
    void sampleAndReportProgress(Job& job);
    void finalizeJob(DownloadId id, bool success, const std::string& error = {});

    ProgressReporter reporter_;
    mutable std::mutex jobsMutex_;
    std::unordered_map<DownloadId, std::unique_ptr<Job>> jobs_;
    orbit::utils::SpeedLimiter globalLimiter_;
    mutable std::mutex proxyMutex_;
    ProxySettings proxy_;
    std::string networkInterface_;
    std::map<std::string, std::string> typeFolders_;

    // Supervisor threads are detached (a state-change listener may erase their Job from
    // inside the supervisor's own callback, so nobody can own and join them), but every
    // one is counted here, and the destructor waits for the count to reach zero before
    // the jobs they point at are freed.
    std::mutex supervisorsMutex_;
    std::condition_variable supervisorsDone_;
    int liveSupervisors_{0};
    // Set by the destructor: aborts in-flight probes and refuses new supervisors.
    std::atomic<bool> shuttingDown_{false};
    std::atomic<long> stallTimeoutSeconds_{60};
    std::atomic<long> checkpointIntervalMs_{15000};

    // Connections a host has shown it allows (it answered more with 429/503), with when
    // that was learned. New downloads from it start at that many for a while instead of
    // being turned away again.
    [[nodiscard]] int connectionsFor(const std::string& source, int wanted) const;
    void rememberHostLimit(const std::string& source, int connections);
    mutable std::mutex hostLimitsMutex_;
    std::unordered_map<std::string, std::pair<int, std::chrono::steady_clock::time_point>> hostLimits_;
};

}
