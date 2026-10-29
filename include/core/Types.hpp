#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <chrono>
#include <optional>
#include <vector>

namespace orbit::core {

using DownloadId = std::uint64_t;

enum class DownloadType {
    Http,
    Torrent,
    Media
};

enum class DownloadState {
    Queued,
    Connecting,
    Downloading,
    Paused,
    Seeding,
    Completed,
    Error,
    Cancelled
};

[[nodiscard]] const char* toString(DownloadState state) noexcept;

[[nodiscard]] const char* toString(DownloadType type) noexcept;

struct DownloadStats {
    std::uint64_t bytesDownloaded{0};
    std::uint64_t bytesUploaded{0};
    std::uint64_t totalBytes{0};
    double downloadRateBytesPerSec{0.0};
    double uploadRateBytesPerSec{0.0};
    std::optional<std::chrono::seconds> eta{};
    int activeConnections{0};
    int numPeers{0};
    int numSeeds{0};

    [[nodiscard]] double progressRatio() const noexcept {
        if (totalBytes == 0) return 0.0;
        return std::clamp(static_cast<double>(bytesDownloaded) / static_cast<double>(totalBytes), 0.0, 1.0);
    }
};

// A browser cookie handed over with a download (e.g. by the browser extension), kept
// in the same shape as a Netscape cookies.txt line so curl and yt-dlp can scope it to
// its own domain instead of sending it to every host a redirect chain visits.
struct HttpCookie {
    std::string domain;
    std::string path{"/"};
    std::string name;
    std::string value;
    bool secure{false};
    bool includeSubdomains{false};
    std::int64_t expires{0};
};

struct ProxySettings {
    enum class Type { None, Http, Socks5 };
    Type type{Type::None};
    std::string host;
    int port{0};
    std::string username;
    std::string password;

    [[nodiscard]] bool enabled() const noexcept { return type != Type::None && !host.empty() && port > 0; }
    // "http://host:port" or "socks5h://host:port" (socks5h resolves names through the proxy).
    [[nodiscard]] std::string url(bool withCredentials = false) const;
};

enum class ChecksumState { None, Pending, Verifying, Match, Mismatch, Failed };

// What a direct download does when its file name is already taken in the destination
// folder. Default follows the app-wide setting (DownloadManager::setDefaultFileConflictAction).
enum class FileConflictAction { Default, Ask, Rename, Overwrite };

[[nodiscard]] const char* toString(FileConflictAction action) noexcept;
[[nodiscard]] FileConflictAction fileConflictActionFromString(const std::string& text) noexcept;

struct DownloadRequest {
    std::string source;
    std::string destinationDir;
    std::string suggestedName{};
    int maxConnections{8};

    std::vector<int> torrentExcludedFileIndices{};

    std::string existingFilePath{};

    // Media (yt-dlp) options. Ignored for Http/Torrent requests.
    bool mediaAudioOnly{false};
    std::string mediaQuality{"best"};      // "best", "1080", "720", "480"
    std::string mediaAudioFormat{"auto"};  // "auto", "m4a", "opus", "mp3"
    bool mediaSubtitles{false};
    std::string mediaSubtitleLanguages{"en"};
    bool mediaEmbedSubtitles{true};

    // Request context captured from the browser, so downloads behind a sign-in work.
    std::string userAgent{};
    std::string referrer{};
    std::vector<HttpCookie> cookies{};

    // Per-download settings. 0 = unlimited.
    std::uint64_t speedLimitBytesPerSec{0};
    std::uint64_t uploadLimitBytesPerSec{0};
    bool sequential{false};

    // "sha256:<hex>" (or md5/sha1/sha512); verified once the download completes.
    std::string expectedChecksum{};

    // Unix seconds. A task with a future start time waits (paused) until then.
    std::int64_t scheduledStartAt{0};

    FileConflictAction fileConflict{FileConflictAction::Default};
};

// What the server says about a link, without downloading it.
struct RemoteProbe {
    bool ok{false};
    std::uint64_t size{0};
    bool supportsRanges{false};
    std::string fileName;
    std::string error;
};

struct TorrentFileEntry {
    std::string path;
    std::uint64_t size{0};
};

struct TorrentFileStatus {
    int index{0};
    bool padding{false};
    std::string path;
    std::uint64_t size{0};
    std::uint64_t downloaded{0};
    int priority{4};  // libtorrent scale: 0 = skip, 1 = low, 4 = normal, 7 = high
};

struct TorrentPeerInfo {
    std::string address;
    std::string client;
    std::string flags;
    double downloadRate{0};
    double uploadRate{0};
    double progress{0};
    bool seed{false};
};

struct TorrentTrackerInfo {
    std::string url;
    int tier{0};
    std::string status;
    std::string message;
    int seeds{-1};
    int peers{-1};
};

// Extra per-task data the UI shows next to the live stats.
struct TaskOptions {
    int maxConnections{8};
    std::uint64_t speedLimitBytesPerSec{0};
    std::uint64_t uploadLimitBytesPerSec{0};
    bool sequential{false};
    std::string expectedChecksum;
    std::int64_t scheduledStartAt{0};
    bool hasBrowserSession{false};
    std::string destinationDir;
};

struct TaskSnapshot {
    DownloadId id{0};
    DownloadType type{DownloadType::Http};
    DownloadState state{DownloadState::Queued};
    std::string name;
    std::string destinationPath;
    std::string source;
    std::string errorMessage;
    std::int64_t addedAt{0};
    DownloadStats stats;
    TaskOptions options;
    ChecksumState checksumState{ChecksumState::None};
    std::string checksumDetail;
    // Set while a download waits for the user to decide about a file that already exists.
    std::string fileConflictPath;
};

}
