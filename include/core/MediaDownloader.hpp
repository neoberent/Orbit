#pragma once

#include "core/IDownloader.hpp"

#include <QObject>
#include <QProcess>
#include <QString>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace orbit::core {

// Downloads media (YouTube, TikTok, Twitch, and similar sites) by shelling out to
// yt-dlp, which in turn calls ffmpeg to merge separate video/audio streams (or to
// extract/transcode audio). Unlike HttpDownloader/TorrentEngine, progress comes from
// parsing yt-dlp's stdout rather than tracking byte counts directly, since the actual
// transfer happens inside the external process.
struct MediaEntry {
    QString url;
    QString title;
    QString uploader;
    double durationSeconds{0};
};

struct MediaInfo {
    bool ok{false};
    QString error;
    QString title;
    QString uploader;
    double durationSeconds{0};
    bool isPlaylist{false};
    std::vector<MediaEntry> entries;
};

class MediaDownloader final : public QObject, public IDownloader {
    Q_OBJECT

public:
    explicit MediaDownloader(ProgressReporter reporter, QObject* parent = nullptr);
    ~MediaDownloader() override;

    MediaDownloader(const MediaDownloader&) = delete;
    MediaDownloader& operator=(const MediaDownloader&) = delete;

    void start(DownloadId id, const DownloadRequest& request) override;
    void pause(DownloadId id) override;
    void resume(DownloadId id) override;
    void cancel(DownloadId id) override;
    void forget(DownloadId id) override;
    [[nodiscard]] DownloadStats stats(DownloadId id) const override;

    [[nodiscard]] bool hasJob(DownloadId id) const;

    // Whether yt-dlp and ffmpeg were found (bundled next to the executable, or on PATH).
    // The Add Download dialog / Media section can use this to warn the user up front.
    [[nodiscard]] static bool isEngineAvailable();
    [[nodiscard]] static QString resolveYtDlpPath();
    // Makes every new yt-dlp run use this executable (a newer, self-updated copy) instead
    // of the bundled one. An empty path, or one that no longer exists, falls back again.
    static void setYtDlpPathOverride(const QString& path);
    [[nodiscard]] static QString bundledYtDlpPath();
    [[nodiscard]] static QString resolveFfmpegPath();

    void setProxy(const ProxySettings& proxy);
    // "" (off), or a browser yt-dlp can read cookies from: "firefox", "chrome", "edge", "brave".
    void setCookiesFromBrowser(const QString& browser);

    // Asks yt-dlp what a URL points to (one video, or a playlist/channel and its items)
    // without downloading anything. `done` runs on this object's thread.
    void fetchInfo(const QString& url, const std::vector<HttpCookie>& cookies, std::function<void(const MediaInfo&)> done);

private:
    struct Job {
        DownloadId id{0};
        DownloadRequest request;
        std::unique_ptr<QProcess> process;
        QString lastKnownDestination;
        QString stderrTail;
        QString cookieFile;
        bool subtitleStage{false};
        bool pausedByUser{false};
        bool cancelRequested{false};
        bool startedDownloadPhase{false};
        DownloadStats lastStats;

        // yt-dlp reports progress separately per stream (video, then audio, then the
        // ffmpeg merge), each restarting at 0 with its own total size. These track the
        // running byte total across streams seen so far, so the reported progress climbs
        // instead of resetting to ~0% at each stream boundary.
        std::uint64_t cumulativeCompletedBytes{0};
        std::uint64_t currentStageTotalBytes{0};
    };

    void launchProcess(DownloadId id);
    void onReadyReadStdout(DownloadId id);
    void onReadyReadStderr(DownloadId id);
    void onProcessFinished(DownloadId id, int exitCode, QProcess::ExitStatus exitStatus);
    void parseProgressLine(Job& job, const QString& line);
    void removePartialFiles(const Job& job) const;
    [[nodiscard]] QStringList commonArguments(DownloadId id, const DownloadRequest& request) const;
    [[nodiscard]] QString writeCookieFile(const QString& key, const std::vector<HttpCookie>& cookies) const;
    static void removeCookieFile(const QString& path);

    ProgressReporter reporter_;
    mutable std::mutex jobsMutex_;
    std::unordered_map<DownloadId, std::unique_ptr<Job>> jobs_;

    mutable std::mutex optionsMutex_;
    ProxySettings proxy_;
    QString cookiesFromBrowser_;
};

}
