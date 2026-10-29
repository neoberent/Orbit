#pragma once

#include "core/DownloadManager.hpp"
#include "core/Types.hpp"

#include <QAbstractListModel>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <vector>

class DownloadListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(QVariantMap summary READ summary NOTIFY revisionChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        TypeRole,
        TypeLabelRole,
        StateRole,
        StateLabelRole,
        ProgressRole,
        DownloadedTextRole,
        TotalTextRole,
        SpeedTextRole,
        UploadSpeedTextRole,
        EtaTextRole,
        PeersRole,
        SeedsRole,
        ErrorRole,
        DestinationRole,
        IsActiveRole,
        IsPausedRole,
        IsTorrentRole,
        IsMediaRole,
        DateTextRole,
        SourceRole,
        DownloadedBytesRole,
        TotalBytesRole,
        SpeedBytesRole,
        EtaSecondsRole,
        AddedAtRole,
        AddedTextRole,
        ConnectionsRole,
        MaxConnectionsRole,
        SpeedLimitKBpsRole,
        UploadLimitKBpsRole,
        SequentialRole,
        ExpectedChecksumRole,
        ChecksumStateRole,
        ChecksumDetailRole,
        ScheduledStartRole,
        ScheduledTextRole,
        HasBrowserSessionRole,
        DestinationDirRole,
        FileConflictPathRole,
        CanRefreshLinkRole,
    };
    Q_ENUM(Roles)

    explicit DownloadListModel(orbit::core::DownloadManager& manager, QObject* parent = nullptr);
    ~DownloadListModel() override;

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] int revision() const noexcept { return revision_; }
    [[nodiscard]] QVariantMap summary() const;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    // spec: {kind: "http"|"torrent"|"media", source, directory, connections, excludedFiles,
    // audioOnly, quality, audioFormat, subtitles, subtitleLanguages, embedSubtitles,
    // checksum, startAt (Unix seconds, 0 = now), speedLimitKBps}. Returns the new id, or
    // -1 when the spec is invalid.
    Q_INVOKABLE qlonglong addDownload(const QVariantMap& spec);
    Q_INVOKABLE bool isMediaSource(const QString& text) const;
    Q_INVOKABLE bool isValidChecksum(const QString& text) const;
    Q_INVOKABLE bool isMediaEngineAvailable() const;
    Q_INVOKABLE bool isFfmpegAvailable() const;

    Q_INVOKABLE QVariantMap parseTorrentFile(const QString& path) const;
    Q_INVOKABLE void pauseItem(qlonglong id);
    Q_INVOKABLE void resumeItem(qlonglong id);
    Q_INVOKABLE void cancelItem(qlonglong id);
    Q_INVOKABLE void removeItem(qlonglong id);
    Q_INVOKABLE void pauseAll();
    Q_INVOKABLE void resumeAll();
    Q_INVOKABLE void clearFinished();
    Q_INVOKABLE void setSequentialDownload(qlonglong id, bool sequential);
    Q_INVOKABLE void setSpeedLimit(qlonglong id, int kbps);
    // options: {speedLimitKBps, uploadLimitKBps, connections, sequential}; missing keys are left alone.
    Q_INVOKABLE void setDownloadOptions(qlonglong id, const QVariantMap& options);
    Q_INVOKABLE QString setExpectedChecksum(qlonglong id, const QString& checksum);
    Q_INVOKABLE void setScheduledStart(qlonglong id, double unixSeconds);
    Q_INVOKABLE QString renameItem(qlonglong id, const QString& newName);
    Q_INVOKABLE QString moveItem(qlonglong id, const QString& directory);

    // For the "file already exists" prompt: {path, folder, name, existingSize, existingSizeText,
    // existingModifiedText, incomingSizeText, keepBothName}. Empty when the download isn't waiting.
    Q_INVOKABLE QVariantMap fileConflictInfo(qlonglong id) const;
    // action: "replace" or "keepBoth". Returns an error message, or an empty string.
    Q_INVOKABLE QString resolveFileConflict(qlonglong id, const QString& action);
    // Downloads currently waiting on a file conflict, oldest first.
    Q_INVOKABLE QVariantList pendingFileConflicts() const;

    Q_INVOKABLE QVariantList torrentFiles(qlonglong id) const;
    Q_INVOKABLE bool setTorrentFilePriority(qlonglong id, int fileIndex, int priority);
    Q_INVOKABLE QVariantList torrentPeers(qlonglong id) const;
    Q_INVOKABLE QVariantList torrentTrackers(qlonglong id) const;
    Q_INVOKABLE QVariantList torrentPieceMap(qlonglong id, int buckets) const;
    Q_INVOKABLE int torrentPieceCount(qlonglong id) const;
    Q_INVOKABLE bool forceRecheck(qlonglong id);
    Q_INVOKABLE bool forceReannounce(qlonglong id);

    // Asks yt-dlp what a media link contains; the answer arrives via mediaInfoReady(token, info).
    Q_INVOKABLE int fetchMediaInfo(const QString& url);
    Q_INVOKABLE bool openContainingFolder(qlonglong id);
    Q_INVOKABLE bool openFile(qlonglong id);
    Q_INVOKABLE int removeWithFiles(const QVariantList& ids);
    Q_INVOKABLE void setMaxConcurrentDownloads(int n);

    Q_INVOKABLE int countByCategory(const QString& category) const;
    Q_INVOKABLE QVariantMap get(qlonglong id) const;
    Q_INVOKABLE QVariantMap rates() const;

signals:
    void countChanged();
    void revisionChanged();
    void transferFinished(qlonglong id, const QString& name, bool success, const QString& detail);
    void notice(qlonglong id, const QString& name, bool ok, const QString& message);
    void mediaInfoReady(int token, const QVariantMap& info);
    void fileConflict(qlonglong id, const QString& name);

private:
    void applySnapshot(const orbit::core::TaskSnapshot& snapshot);
    void applyRemoval(orbit::core::DownloadId id);
    [[nodiscard]] int rowOf(qlonglong id) const;

    orbit::core::DownloadManager& manager_;
    std::vector<orbit::core::TaskSnapshot> items_;
    int updateSubToken_{-1};
    int removeSubToken_{-1};
    int noticeSubToken_{-1};
    int nextMediaInfoToken_{1};
    int revision_{0};
};
