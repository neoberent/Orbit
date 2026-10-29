#pragma once

#include "core/IDownloader.hpp"
#include <libtorrent/fwd.hpp>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>

namespace orbit::core {

struct TorrentEngineSettings {
    std::filesystem::path defaultSavePath;
    int downloadRateLimitBytesPerSec{0};
    int uploadRateLimitBytesPerSec{0};
    int listenPort{51413};
    bool enableDht{true};
};

class TorrentEngine final : public IDownloader {
public:
    explicit TorrentEngine(ProgressReporter reporter, TorrentEngineSettings settings = {});
    ~TorrentEngine() override;

    TorrentEngine(const TorrentEngine&) = delete;
    TorrentEngine& operator=(const TorrentEngine&) = delete;

    void start(DownloadId id, const DownloadRequest& request) override;
    void pause(DownloadId id) override;
    void resume(DownloadId id) override;
    void cancel(DownloadId id) override;
    void forget(DownloadId id) override;
    // Like forget(), but has libtorrent delete the torrent's downloaded files too.
    // Returns false when there was no live torrent for this id.
    bool forgetAndDeleteFiles(DownloadId id);
    [[nodiscard]] DownloadStats stats(DownloadId id) const override;

    [[nodiscard]] bool hasEntry(DownloadId id) const;

    void setSequentialDownload(DownloadId id, bool sequential);
    void setDownloadRateLimit(int bytesPerSec);
    void setUploadRateLimit(int bytesPerSec);
    void setTorrentRateLimits(DownloadId id, std::uint64_t downloadBytesPerSec, std::uint64_t uploadBytesPerSec);
    void setProxy(const ProxySettings& proxy);

    // Stops seeding once a torrent has uploaded ratio x its size, or has seeded for
    // minutes. 0 disables either limit; both 0 means seed indefinitely.
    void setSeedingLimits(double ratio, int minutes);

    [[nodiscard]] std::vector<TorrentFileStatus> files(DownloadId id) const;
    // priority: 0 = skip, 1 = low, 4 = normal, 7 = high.
    bool setFilePriority(DownloadId id, int fileIndex, int priority);
    [[nodiscard]] std::vector<TorrentPeerInfo> peers(DownloadId id) const;
    [[nodiscard]] std::vector<TorrentTrackerInfo> trackers(DownloadId id) const;
    // Downsamples the have-bitfield to at most `buckets` cells, each 0..255 = the share
    // of pieces in that cell we have. Empty when there's no metadata yet.
    [[nodiscard]] std::vector<std::uint8_t> pieceMap(DownloadId id, int buckets) const;
    [[nodiscard]] int pieceCount(DownloadId id) const;
    bool forceRecheck(DownloadId id);
    bool forceReannounce(DownloadId id);

    // Renames the torrent's top-level file or folder. Returns an error message, or an
    // empty string when the rename was queued.
    std::string rename(DownloadId id, const std::string& newName);
    // Moves the downloaded data to another folder; the result arrives via onNotice.
    std::string moveStorage(DownloadId id, const std::string& directory);

    static bool parseTorrentFile(const std::string& path, std::string& outName,
                                  std::vector<TorrentFileEntry>& outFiles, std::string& outError);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    void alertLoop(std::stop_token stopToken);
    [[nodiscard]] DownloadId idForHandle(const libtorrent::torrent_handle& handle) const;

    // Requests fresh resume data for every active, metadata-having torrent and blocks
    // (bounded) until each has been written to disk or the deadline passes. Used on
    // shutdown, once the alert thread has already stopped, so it drains alerts itself.
    void saveAllResumeDataBlocking();

    // Serializes one torrent's resume data to <id>.fastresume. Used both by the
    // blocking shutdown save and by the periodic checkpoint handled in alertLoop().
    void writeResumeData(const libtorrent::torrent_handle& handle, const libtorrent::add_torrent_params& params);

    ProgressReporter reporter_;
    std::jthread alertThread_;
};

}
