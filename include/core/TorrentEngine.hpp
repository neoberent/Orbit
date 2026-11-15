#pragma once

#include "core/IDownloader.hpp"
#include <libtorrent/fwd.hpp>
#include <filesystem>
#include <string>
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
    // Locked to this interface from the first moment (see setNetworkInterface).
    std::string networkInterface;
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
    // Stops a finished torrent from seeding; its files stay. It then shows as Completed.
    void stopSeeding(DownloadId id);
    [[nodiscard]] DownloadStats stats(DownloadId id) const override;

    [[nodiscard]] bool hasEntry(DownloadId id) const;

    void setSequentialDownload(DownloadId id, bool sequential);
    void setDownloadRateLimit(int bytesPerSec);
    void setUploadRateLimit(int bytesPerSec);
    void setTorrentRateLimits(DownloadId id, std::uint64_t downloadBytesPerSec, std::uint64_t uploadBytesPerSec);
    void setProxy(const ProxySettings& proxy);
    // The relay (DownloadManager's InterfaceRelay) that HTTP(S) trackers go through while
    // the interface is locked and the user has no proxy of their own.
    void setRelayProxy(const ProxySettings& relay);
    // Listens and connects only through this interface (empty = all of them).
    void setNetworkInterface(const std::string& interfaceName);
    void resolveBootstrapThrough(const std::string& interfaceName);

    // Stops seeding once a torrent has uploaded ratio x its size, or has seeded for
    // minutes. 0 disables either limit; both 0 means seed indefinitely.
    void setSeedingLimits(double ratio, int minutes);
    // Off: a torrent stops as soon as it has finished downloading. On: it keeps sharing
    // until the seeding limits above (if any) are reached.
    void setSeedAfterDownload(bool seed);
    // On btrfs, marks a torrent's files (or its folder) "no copy-on-write" before
    // anything is written, so the scattered piece writes don't fragment them badly.
    // Torrents check every piece's hash themselves, so btrfs's checksums aren't needed.
    void setNoCowOnBtrfs(bool enabled);
    // The port peers reach this PC on (TCP and UDP). A VPN that forwards one port needs
    // exactly that one here.
    void setListenPort(int port);
    // Whether non-private torrents get Orbit's list of public trackers added (more peers;
    // off for those who'd rather announce only where the torrent says).
    void setAddPublicTrackers(bool add);
    // Whether path is on btrfs, and the marking itself (exposed for the tests).
    static bool isOnBtrfs(const std::string& path);
    static bool markNoCow(const std::string& path);

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
    // The user's proxy if there is one, else the relay while locked, else none.
    void applyProxy();
    // While locked, libtorrent would look UDP trackers' names up with the system resolver:
    // they're swapped for their addresses looked up through the interface (the UDP
    // tracker protocol carries no name, so nothing is lost), and web seeds (connections
    // the relay doesn't carry) are set aside. unlockTrackers puts the originals back.
    void lockTrackers(DownloadId id, const libtorrent::torrent_handle& handle);
    void unlockTrackers(DownloadId id);

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
