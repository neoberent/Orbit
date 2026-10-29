#include "DownloadListModel.hpp"

#include "core/MediaDownloader.hpp"
#include "core/TorrentEngine.hpp"
#include "utils/Hashing.hpp"
#include "utils/StringUtils.hpp"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMetaObject>
#include <QPointer>
#include <QUrl>

#include <algorithm>

using orbit::core::DownloadId;
using orbit::core::DownloadRequest;
using orbit::core::DownloadState;
using orbit::core::DownloadType;
using orbit::core::TaskSnapshot;

namespace {

QString stateLabel(DownloadState state) {
    switch (state) {
        case DownloadState::Queued: return QStringLiteral("Queued");
        case DownloadState::Connecting: return QStringLiteral("Connecting");
        case DownloadState::Downloading: return QStringLiteral("Downloading");
        case DownloadState::Paused: return QStringLiteral("Paused");
        case DownloadState::Seeding: return QStringLiteral("Seeding");
        case DownloadState::Completed: return QStringLiteral("Completed");
        case DownloadState::Error: return QStringLiteral("Error");
        case DownloadState::Cancelled: return QStringLiteral("Cancelled");
    }
    return QStringLiteral("Unknown");
}

QString typeLabel(DownloadType type) {
    switch (type) {
        case DownloadType::Http: return QStringLiteral("HTTP");
        case DownloadType::Torrent: return QStringLiteral("Torrent");
        case DownloadType::Media: return QStringLiteral("Media");
    }
    return QStringLiteral("HTTP");
}

QString finishedDateText(const std::string& path) {
    const QFileInfo info(QString::fromStdString(path));
    if (path.empty() || !info.exists()) return {};
    const QDateTime modified = info.lastModified();
    const QDate today = QDate::currentDate();
    const QLocale locale;
    const QString time = locale.toString(modified.time(), QLocale::ShortFormat);
    if (modified.date() == today) return QStringLiteral("Today, ") + time;
    if (modified.date() == today.addDays(-1)) return QStringLiteral("Yesterday, ") + time;
    if (modified.date().year() == today.year()) return locale.toString(modified.date(), QStringLiteral("MMM d"));
    return locale.toString(modified.date(), QStringLiteral("MMM d, yyyy"));
}

QString addedDateText(std::int64_t unixSeconds) {
    if (unixSeconds <= 0) return {};
    const QDateTime added = QDateTime::fromSecsSinceEpoch(unixSeconds);
    const QDate today = QDate::currentDate();
    const QLocale locale;
    const QString time = locale.toString(added.time(), QLocale::ShortFormat);
    if (added.date() == today) return QStringLiteral("Today, ") + time;
    if (added.date() == today.addDays(-1)) return QStringLiteral("Yesterday, ") + time;
    if (added.date().year() == today.year()) return locale.toString(added.date(), QStringLiteral("MMM d")) + QStringLiteral(", ") + time;
    return locale.toString(added.date(), QStringLiteral("MMM d, yyyy"));
}

bool isFinishedState(DownloadState s) {
    return s == DownloadState::Completed || s == DownloadState::Error || s == DownloadState::Cancelled;
}

QString checksumStateLabel(orbit::core::ChecksumState state) {
    switch (state) {
        case orbit::core::ChecksumState::None: return QString();
        case orbit::core::ChecksumState::Pending: return QStringLiteral("pending");
        case orbit::core::ChecksumState::Verifying: return QStringLiteral("verifying");
        case orbit::core::ChecksumState::Match: return QStringLiteral("match");
        case orbit::core::ChecksumState::Mismatch: return QStringLiteral("mismatch");
        case orbit::core::ChecksumState::Failed: return QStringLiteral("failed");
    }
    return QString();
}

QString scheduledText(std::int64_t unixSeconds) {
    if (unixSeconds <= 0) return {};
    const QDateTime when = QDateTime::fromSecsSinceEpoch(unixSeconds);
    const QDate today = QDate::currentDate();
    const QLocale locale;
    const QString time = locale.toString(when.time(), QLocale::ShortFormat);
    if (when.date() == today) return QStringLiteral("Today, ") + time;
    if (when.date() == today.addDays(1)) return QStringLiteral("Tomorrow, ") + time;
    return locale.toString(when.date(), QStringLiteral("ddd, MMM d")) + QStringLiteral(", ") + time;
}

std::uint64_t kbpsToBytes(int kbps) { return kbps > 0 ? static_cast<std::uint64_t>(kbps) * 1024 : 0; }
int bytesToKbps(std::uint64_t bytes) { return static_cast<int>(bytes / 1024); }

}

DownloadListModel::DownloadListModel(orbit::core::DownloadManager& manager, QObject* parent)
    : QAbstractListModel(parent), manager_(manager) {
    items_ = manager_.snapshot();

    updateSubToken_ = manager_.subscribeUpdates([this](const TaskSnapshot& snap) {
        QMetaObject::invokeMethod(this, [this, snap]() { applySnapshot(snap); }, Qt::QueuedConnection);
    });
    removeSubToken_ = manager_.subscribeRemovals([this](DownloadId id) {
        QMetaObject::invokeMethod(this, [this, id]() { applyRemoval(id); }, Qt::QueuedConnection);
    });
    noticeSubToken_ = manager_.subscribeNotices([this](DownloadId id, bool ok, const std::string& message) {
        const QString text = QString::fromStdString(message);
        QMetaObject::invokeMethod(this, [this, id, ok, text]() {
            const int row = rowOf(static_cast<qlonglong>(id));
            const QString name = row >= 0 ? data(index(row), NameRole).toString() : QString();
            emit notice(static_cast<qlonglong>(id), name, ok, text);
        }, Qt::QueuedConnection);
    });
}

DownloadListModel::~DownloadListModel() {
    manager_.unsubscribe(updateSubToken_);
    manager_.unsubscribe(removeSubToken_);
    manager_.unsubscribe(noticeSubToken_);
}

int DownloadListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(items_.size());
}

QVariant DownloadListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(items_.size())) {
        return {};
    }
    const TaskSnapshot& snap = items_[static_cast<size_t>(index.row())];

    switch (role) {
        case IdRole: return QVariant::fromValue<qlonglong>(static_cast<qlonglong>(snap.id));
        case NameRole: return QString::fromStdString(snap.name.empty() ? snap.source : snap.name);
        case TypeRole: return static_cast<int>(snap.type);
        case TypeLabelRole: return typeLabel(snap.type);
        case StateRole: return static_cast<int>(snap.state);
        case StateLabelRole: return stateLabel(snap.state);
        case ProgressRole: return snap.stats.progressRatio();
        case DownloadedTextRole: return QString::fromStdString(orbit::utils::formatBytes(snap.stats.bytesDownloaded));
        case TotalTextRole:
            return snap.stats.totalBytes > 0 ? QString::fromStdString(orbit::utils::formatBytes(snap.stats.totalBytes))
                                              : QStringLiteral("Unknown");
        case SpeedTextRole:
            return (snap.state == DownloadState::Downloading || snap.state == DownloadState::Connecting)
                       ? QString::fromStdString(orbit::utils::formatRate(snap.stats.downloadRateBytesPerSec))
                       : QStringLiteral("--");
        case UploadSpeedTextRole:
            return snap.stats.uploadRateBytesPerSec > 0 &&
                           (snap.state == DownloadState::Seeding || snap.state == DownloadState::Downloading)
                       ? QString::fromStdString(orbit::utils::formatRate(snap.stats.uploadRateBytesPerSec))
                       : QStringLiteral("--");
        case EtaTextRole:
            return snap.stats.eta.has_value() ? QString::fromStdString(orbit::utils::formatDuration(*snap.stats.eta))
                                               : QStringLiteral("--");
        case PeersRole: return snap.stats.numPeers;
        case SeedsRole: return snap.stats.numSeeds;
        case ErrorRole: return QString::fromStdString(snap.errorMessage);
        case DestinationRole: return QDir::toNativeSeparators(QString::fromStdString(snap.destinationPath));
        case IsActiveRole:
            return snap.state == DownloadState::Downloading || snap.state == DownloadState::Connecting ||
                   snap.state == DownloadState::Seeding;
        case IsPausedRole: return snap.state == DownloadState::Paused;
        case IsTorrentRole: return snap.type == DownloadType::Torrent;
        case IsMediaRole: return snap.type == DownloadType::Media;
        case DateTextRole:
            return (snap.state == DownloadState::Completed || snap.state == DownloadState::Seeding)
                       ? finishedDateText(snap.destinationPath)
                       : QString();
        case SourceRole: return QString::fromStdString(snap.source);
        case DownloadedBytesRole: return static_cast<double>(snap.stats.bytesDownloaded);
        case TotalBytesRole: return static_cast<double>(snap.stats.totalBytes);
        case SpeedBytesRole:
            return (snap.state == DownloadState::Downloading || snap.state == DownloadState::Connecting)
                       ? snap.stats.downloadRateBytesPerSec
                       : (snap.state == DownloadState::Seeding ? snap.stats.uploadRateBytesPerSec : 0.0);
        case EtaSecondsRole:
            return snap.state == DownloadState::Downloading && snap.stats.eta.has_value()
                       ? static_cast<double>(snap.stats.eta->count())
                       : -1.0;
        case AddedAtRole: return static_cast<double>(snap.addedAt);
        case AddedTextRole: return addedDateText(snap.addedAt);
        case ConnectionsRole: return snap.stats.activeConnections;
        case MaxConnectionsRole: return snap.options.maxConnections;
        case SpeedLimitKBpsRole: return bytesToKbps(snap.options.speedLimitBytesPerSec);
        case UploadLimitKBpsRole: return bytesToKbps(snap.options.uploadLimitBytesPerSec);
        case SequentialRole: return snap.options.sequential;
        case ExpectedChecksumRole: return QString::fromStdString(snap.options.expectedChecksum);
        case ChecksumStateRole: return checksumStateLabel(snap.checksumState);
        case ChecksumDetailRole: return QString::fromStdString(snap.checksumDetail);
        case ScheduledStartRole: return static_cast<double>(snap.options.scheduledStartAt);
        case ScheduledTextRole:
            return snap.state == DownloadState::Paused ? scheduledText(snap.options.scheduledStartAt) : QString();
        case HasBrowserSessionRole: return snap.options.hasBrowserSession;
        case DestinationDirRole: return QDir::toNativeSeparators(QString::fromStdString(snap.options.destinationDir));
        case FileConflictPathRole: return QDir::toNativeSeparators(QString::fromStdString(snap.fileConflictPath));
        case CanRefreshLinkRole:
            return snap.type == DownloadType::Http && snap.fileConflictPath.empty() &&
                   (snap.state == DownloadState::Paused || snap.state == DownloadState::Error ||
                    snap.state == DownloadState::Queued || snap.state == DownloadState::Cancelled);
        default: return {};
    }
}

QHash<int, QByteArray> DownloadListModel::roleNames() const {
    return {
        {IdRole, "taskId"},
        {NameRole, "name"},
        {TypeRole, "type"},
        {TypeLabelRole, "typeLabel"},
        {StateRole, "state"},
        {StateLabelRole, "stateLabel"},
        {ProgressRole, "progress"},
        {DownloadedTextRole, "downloadedText"},
        {TotalTextRole, "totalText"},
        {SpeedTextRole, "speedText"},
        {UploadSpeedTextRole, "uploadSpeedText"},
        {EtaTextRole, "etaText"},
        {PeersRole, "peers"},
        {SeedsRole, "seeds"},
        {ErrorRole, "errorMessage"},
        {DestinationRole, "destination"},
        {IsActiveRole, "isActive"},
        {IsPausedRole, "isPaused"},
        {IsTorrentRole, "isTorrent"},
        {IsMediaRole, "isMedia"},
        {DateTextRole, "dateText"},
        {SourceRole, "source"},
        {DownloadedBytesRole, "downloadedBytes"},
        {TotalBytesRole, "totalBytes"},
        {SpeedBytesRole, "speedBytes"},
        {EtaSecondsRole, "etaSeconds"},
        {AddedAtRole, "addedAt"},
        {AddedTextRole, "addedText"},
        {ConnectionsRole, "connections"},
        {MaxConnectionsRole, "maxConnections"},
        {SpeedLimitKBpsRole, "speedLimitKBps"},
        {UploadLimitKBpsRole, "uploadLimitKBps"},
        {SequentialRole, "sequential"},
        {ExpectedChecksumRole, "expectedChecksum"},
        {ChecksumStateRole, "checksumState"},
        {ChecksumDetailRole, "checksumDetail"},
        {ScheduledStartRole, "scheduledStart"},
        {ScheduledTextRole, "scheduledText"},
        {HasBrowserSessionRole, "hasBrowserSession"},
        {DestinationDirRole, "destinationDir"},
        {FileConflictPathRole, "fileConflictPath"},
        {CanRefreshLinkRole, "canRefreshLink"},
    };
}

qlonglong DownloadListModel::addDownload(const QVariantMap& spec) {
    const QString kind = spec.value("kind").toString();
    const QString source = spec.value("source").toString().trimmed();
    if (source.isEmpty()) return -1;

    DownloadRequest request;
    request.source = source.toStdString();
    request.destinationDir = spec.value("directory").toString().trimmed().toStdString();
    request.maxConnections = std::clamp(spec.value("connections", 8).toInt(), 1, 32);
    request.speedLimitBytesPerSec = kbpsToBytes(spec.value("speedLimitKBps", 0).toInt());
    request.scheduledStartAt = static_cast<std::int64_t>(spec.value("startAt", 0).toDouble());
    if (const QString checksum = spec.value("checksum").toString().trimmed(); !checksum.isEmpty()) {
        const auto parsed = orbit::utils::parseChecksum(checksum.toStdString());
        if (parsed) request.expectedChecksum = parsed->toString();
    }

    if (kind == QLatin1String("torrent")) {
        for (const QVariant& v : spec.value("excludedFiles").toList()) request.torrentExcludedFileIndices.push_back(v.toInt());
        request.expectedChecksum.clear();
        return static_cast<qlonglong>(manager_.addTorrent(std::move(request)));
    }
    if (kind == QLatin1String("media")) {
        request.mediaAudioOnly = spec.value("audioOnly").toBool();
        request.mediaQuality = spec.value("quality", "best").toString().toStdString();
        request.mediaAudioFormat = spec.value("audioFormat", "auto").toString().toStdString();
        request.mediaSubtitles = spec.value("subtitles").toBool();
        request.mediaSubtitleLanguages = spec.value("subtitleLanguages", "en").toString().trimmed().toStdString();
        request.mediaEmbedSubtitles = spec.value("embedSubtitles", true).toBool();
        if (const QString title = spec.value("title").toString().trimmed(); !title.isEmpty()) {
            request.suggestedName = title.toStdString();
        }
        return static_cast<qlonglong>(manager_.addMediaDownload(std::move(request)));
    }
    return static_cast<qlonglong>(manager_.addHttpDownload(std::move(request)));
}

bool DownloadListModel::isMediaSource(const QString& text) const {
    return orbit::utils::isMediaUrl(text.toStdString());
}

bool DownloadListModel::isValidChecksum(const QString& text) const {
    return orbit::utils::parseChecksum(text.toStdString()).has_value();
}

bool DownloadListModel::isMediaEngineAvailable() const { return orbit::core::MediaDownloader::isEngineAvailable(); }
bool DownloadListModel::isFfmpegAvailable() const { return !orbit::core::MediaDownloader::resolveFfmpegPath().isEmpty(); }

QVariantMap DownloadListModel::parseTorrentFile(const QString& path) const {
    std::string name;
    std::vector<orbit::core::TorrentFileEntry> files;
    std::string error;
    const bool ok = orbit::core::TorrentEngine::parseTorrentFile(path.toStdString(), name, files, error);

    QVariantMap result;
    result["ok"] = ok;
    result["error"] = QString::fromStdString(error);
    result["name"] = QString::fromStdString(name);

    QVariantList fileList;
    fileList.reserve(static_cast<qsizetype>(files.size()));
    for (int i = 0; i < static_cast<int>(files.size()); ++i) {
        QVariantMap entry;
        entry["index"] = i;
        entry["path"] = QString::fromStdString(files[static_cast<size_t>(i)].path);
        entry["size"] = static_cast<double>(files[static_cast<size_t>(i)].size);
        fileList.push_back(entry);
    }
    result["files"] = fileList;
    return result;
}

void DownloadListModel::pauseItem(qlonglong id) { manager_.pause(static_cast<DownloadId>(id)); }
void DownloadListModel::resumeItem(qlonglong id) { manager_.resume(static_cast<DownloadId>(id)); }
void DownloadListModel::cancelItem(qlonglong id) { manager_.cancel(static_cast<DownloadId>(id)); }
void DownloadListModel::removeItem(qlonglong id) { manager_.remove(static_cast<DownloadId>(id)); }

void DownloadListModel::pauseAll() {
    for (const TaskSnapshot& s : items_) {
        if (s.state == DownloadState::Downloading || s.state == DownloadState::Connecting ||
            s.state == DownloadState::Seeding) {
            manager_.pause(s.id);
        }
    }
}

void DownloadListModel::resumeAll() {
    for (const TaskSnapshot& s : items_) {
        if (s.state == DownloadState::Paused) manager_.resume(s.id);
    }
}

void DownloadListModel::clearFinished() {
    std::vector<DownloadId> ids;
    for (const TaskSnapshot& s : items_) {
        if (s.state == DownloadState::Completed || s.state == DownloadState::Cancelled) ids.push_back(s.id);
    }
    for (DownloadId id : ids) manager_.remove(id);
}

QVariantMap DownloadListModel::summary() const {
    double downRate = 0;
    double upRate = 0;
    int active = 0, queued = 0, paused = 0, completed = 0, errors = 0, seeding = 0, finished = 0;
    for (const TaskSnapshot& s : items_) {
        switch (s.state) {
            case DownloadState::Downloading:
            case DownloadState::Connecting: ++active; downRate += s.stats.downloadRateBytesPerSec; break;
            case DownloadState::Seeding: ++seeding; break;
            case DownloadState::Queued: ++queued; break;
            case DownloadState::Paused: ++paused; break;
            case DownloadState::Completed: ++completed; ++finished; break;
            case DownloadState::Cancelled: ++finished; break;
            case DownloadState::Error: ++errors; break;
        }
        if (s.state == DownloadState::Seeding || s.state == DownloadState::Downloading) upRate += s.stats.uploadRateBytesPerSec;
    }
    QVariantMap result;
    result["downloadRateText"] = QString::fromStdString(orbit::utils::formatRate(downRate));
    result["uploadRateText"] = QString::fromStdString(orbit::utils::formatRate(upRate));
    result["downloadRate"] = downRate;
    result["uploadRate"] = upRate;
    result["downloading"] = downRate > 0;
    result["uploading"] = upRate > 0;
    result["active"] = active;
    result["queued"] = queued;
    result["paused"] = paused;
    result["completed"] = completed;
    result["errors"] = errors;
    result["seeding"] = seeding;
    result["finished"] = finished;
    return result;
}

void DownloadListModel::setSequentialDownload(qlonglong id, bool sequential) {
    manager_.setSequential(static_cast<DownloadId>(id), sequential);
}

void DownloadListModel::setSpeedLimit(qlonglong id, int kbps) {
    manager_.setSpeedLimit(static_cast<DownloadId>(id), kbpsToBytes(std::max(0, kbps)));
}

void DownloadListModel::setDownloadOptions(qlonglong id, const QVariantMap& options) {
    const auto did = static_cast<DownloadId>(id);
    if (options.contains("speedLimitKBps")) manager_.setSpeedLimit(did, kbpsToBytes(std::max(0, options.value("speedLimitKBps").toInt())));
    if (options.contains("uploadLimitKBps")) manager_.setUploadLimit(did, kbpsToBytes(std::max(0, options.value("uploadLimitKBps").toInt())));
    if (options.contains("connections")) manager_.setMaxConnections(did, options.value("connections").toInt());
    if (options.contains("sequential")) manager_.setSequential(did, options.value("sequential").toBool());
}

QString DownloadListModel::setExpectedChecksum(qlonglong id, const QString& checksum) {
    return QString::fromStdString(manager_.setExpectedChecksum(static_cast<DownloadId>(id), checksum.toStdString()));
}

void DownloadListModel::setScheduledStart(qlonglong id, double unixSeconds) {
    manager_.setScheduledStart(static_cast<DownloadId>(id), static_cast<std::int64_t>(unixSeconds));
}

QString DownloadListModel::renameItem(qlonglong id, const QString& newName) {
    return QString::fromStdString(manager_.rename(static_cast<DownloadId>(id), newName.toStdString()));
}

QString DownloadListModel::moveItem(qlonglong id, const QString& directory) {
    return QString::fromStdString(manager_.move(static_cast<DownloadId>(id), directory.toStdString()));
}

QVariantList DownloadListModel::torrentFiles(qlonglong id) const {
    QVariantList result;
    for (const auto& file : manager_.torrentEngine().files(static_cast<DownloadId>(id))) {
        if (file.padding) continue;
        result.push_back(QVariantMap{
            {"index", file.index},
            {"path", QString::fromStdString(file.path)},
            {"size", static_cast<double>(file.size)},
            {"downloaded", static_cast<double>(file.downloaded)},
            {"progress", file.size > 0 ? static_cast<double>(file.downloaded) / static_cast<double>(file.size) : 1.0},
            {"priority", file.priority},
        });
    }
    return result;
}

bool DownloadListModel::setTorrentFilePriority(qlonglong id, int fileIndex, int priority) {
    return manager_.torrentEngine().setFilePriority(static_cast<DownloadId>(id), fileIndex, priority);
}

QVariantList DownloadListModel::torrentPeers(qlonglong id) const {
    QVariantList result;
    for (const auto& peer : manager_.torrentEngine().peers(static_cast<DownloadId>(id))) {
        result.push_back(QVariantMap{
            {"address", QString::fromStdString(peer.address)},
            {"client", QString::fromStdString(peer.client)},
            {"flags", QString::fromStdString(peer.flags)},
            {"downloadRate", peer.downloadRate},
            {"uploadRate", peer.uploadRate},
            {"progress", peer.progress},
            {"seed", peer.seed},
        });
    }
    return result;
}

QVariantList DownloadListModel::torrentTrackers(qlonglong id) const {
    QVariantList result;
    for (const auto& tracker : manager_.torrentEngine().trackers(static_cast<DownloadId>(id))) {
        result.push_back(QVariantMap{
            {"url", QString::fromStdString(tracker.url)},
            {"tier", tracker.tier},
            {"status", QString::fromStdString(tracker.status)},
            {"message", QString::fromStdString(tracker.message)},
            {"seeds", tracker.seeds},
            {"peers", tracker.peers},
        });
    }
    return result;
}

QVariantList DownloadListModel::torrentPieceMap(qlonglong id, int buckets) const {
    QVariantList result;
    for (std::uint8_t cell : manager_.torrentEngine().pieceMap(static_cast<DownloadId>(id), buckets)) result.push_back(cell);
    return result;
}

int DownloadListModel::torrentPieceCount(qlonglong id) const {
    return manager_.torrentEngine().pieceCount(static_cast<DownloadId>(id));
}

bool DownloadListModel::forceRecheck(qlonglong id) { return manager_.torrentEngine().forceRecheck(static_cast<DownloadId>(id)); }

bool DownloadListModel::forceReannounce(qlonglong id) {
    return manager_.torrentEngine().forceReannounce(static_cast<DownloadId>(id));
}

int DownloadListModel::fetchMediaInfo(const QString& url) {
    const int token = nextMediaInfoToken_++;
    QPointer<DownloadListModel> self(this);
    manager_.mediaDownloader().fetchInfo(url, {}, [self, token](const orbit::core::MediaInfo& info) {
        if (!self) return;
        QVariantList entries;
        for (const auto& entry : info.entries) {
            entries.push_back(QVariantMap{{"url", entry.url},
                                          {"title", entry.title},
                                          {"uploader", entry.uploader},
                                          {"duration", entry.durationSeconds}});
        }
        emit self->mediaInfoReady(token, QVariantMap{{"ok", info.ok},
                                                     {"error", info.error},
                                                     {"title", info.title},
                                                     {"uploader", info.uploader},
                                                     {"duration", info.durationSeconds},
                                                     {"isPlaylist", info.isPlaylist},
                                                     {"entries", entries}});
    });
    return token;
}

bool DownloadListModel::openContainingFolder(qlonglong id) {
    auto snap = manager_.snapshot(static_cast<DownloadId>(id));
    if (!snap) return false;
    const QString path = QString::fromStdString(snap->destinationPath);
    const QFileInfo info(path);
    const QString dir = info.exists() ? info.absolutePath() : QString::fromStdString(manager_.defaultDownloadDir().string());
    return QDesktopServices::openUrl(QUrl::fromLocalFile(dir.isEmpty() ? QDir::homePath() : dir));
}

bool DownloadListModel::openFile(qlonglong id) {
    auto snap = manager_.snapshot(static_cast<DownloadId>(id));
    if (!snap || snap->destinationPath.empty()) return false;
    const QString path = QString::fromStdString(snap->destinationPath);
    if (!QFileInfo::exists(path)) return false;
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

int DownloadListModel::removeWithFiles(const QVariantList& ids) {
    int removed = 0;
    for (const QVariant& v : ids) {
        const auto id = static_cast<DownloadId>(v.toLongLong());
        auto snap = manager_.snapshot(id);
        if (!snap) continue;
        if (snap->state == DownloadState::Downloading || snap->state == DownloadState::Connecting ||
            snap->state == DownloadState::Seeding) {
            continue;
        }
        manager_.remove(id, true);
        ++removed;
    }
    return removed;
}

QVariantMap DownloadListModel::get(qlonglong id) const {
    QVariantMap result;
    const int row = rowOf(id);
    if (row < 0) return result;
    const QModelIndex idx = index(row);
    const auto names = roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it) {
        result.insert(QString::fromUtf8(it.value()), data(idx, it.key()));
    }
    return result;
}

QVariantMap DownloadListModel::rates() const {
    QVariantMap result;
    for (const TaskSnapshot& s : items_) {
        double rate = 0;
        if (s.state == DownloadState::Downloading || s.state == DownloadState::Connecting) rate = s.stats.downloadRateBytesPerSec;
        else if (s.state == DownloadState::Seeding) rate = s.stats.uploadRateBytesPerSec;
        else continue;
        result.insert(QString::number(static_cast<qlonglong>(s.id)), rate);
    }
    return result;
}

int DownloadListModel::rowOf(qlonglong id) const {
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (static_cast<qlonglong>(items_[static_cast<size_t>(i)].id) == id) return i;
    }
    return -1;
}

void DownloadListModel::setMaxConcurrentDownloads(int n) { manager_.setMaxConcurrentDownloads(n); }

int DownloadListModel::countByCategory(const QString& category) const {
    if (category == "all" || category.isEmpty()) return static_cast<int>(items_.size());

    return static_cast<int>(std::count_if(items_.begin(), items_.end(), [&](const TaskSnapshot& s) {
        if (category == "torrents") return s.type == DownloadType::Torrent;
        if (category == "media") return s.type == DownloadType::Media;
        if (category == "completed") return s.state == DownloadState::Completed;
        if (category == "downloading") {
            return s.state == DownloadState::Queued || s.state == DownloadState::Connecting ||
                   s.state == DownloadState::Downloading || s.state == DownloadState::Paused ||
                   s.state == DownloadState::Seeding;
        }
        return false;
    }));
}

void DownloadListModel::applySnapshot(const TaskSnapshot& snapshot) {
    auto it = std::find_if(items_.begin(), items_.end(),
                            [&](const TaskSnapshot& s) { return s.id == snapshot.id; });
    if (it == items_.end()) {
        beginInsertRows(QModelIndex(), static_cast<int>(items_.size()), static_cast<int>(items_.size()));
        items_.push_back(snapshot);
        endInsertRows();
        emit countChanged();
        ++revision_;
        emit revisionChanged();
        return;
    }
    const int row = static_cast<int>(std::distance(items_.begin(), it));
    const DownloadState previous = it->state;
    const bool newConflict = it->fileConflictPath.empty() && !snapshot.fileConflictPath.empty();
    *it = snapshot;
    if (previous != snapshot.state && isFinishedState(snapshot.state) && !isFinishedState(previous) &&
        snapshot.state != DownloadState::Cancelled) {
        emit transferFinished(static_cast<qlonglong>(snapshot.id),
                              QString::fromStdString(snapshot.name.empty() ? snapshot.source : snapshot.name),
                              snapshot.state == DownloadState::Completed,
                              QString::fromStdString(snapshot.errorMessage));
    }
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx);
    ++revision_;
    emit revisionChanged();
    if (newConflict) {
        emit fileConflict(static_cast<qlonglong>(snapshot.id),
                          QString::fromStdString(snapshot.name.empty() ? snapshot.source : snapshot.name));
    }
}

QVariantMap DownloadListModel::fileConflictInfo(qlonglong id) const {
    const int row = rowOf(id);
    if (row < 0) return {};
    const TaskSnapshot& snap = items_[static_cast<size_t>(row)];
    if (snap.fileConflictPath.empty()) return {};

    const QString path = QString::fromStdString(snap.fileConflictPath);
    const QFileInfo existing(path);
    QString keepBothName;
    const QString suffix = existing.suffix();
    const QString base = suffix.isEmpty() ? existing.fileName() : existing.completeBaseName();
    for (int i = 1; i < 10000; ++i) {
        const QString candidate = suffix.isEmpty() ? QStringLiteral("%1 (%2)").arg(base).arg(i)
                                                   : QStringLiteral("%1 (%2).%3").arg(base).arg(i).arg(suffix);
        if (!QFileInfo::exists(existing.dir().filePath(candidate))) {
            keepBothName = candidate;
            break;
        }
    }
    const QLocale locale;
    return {
        {"path", QDir::toNativeSeparators(path)},
        {"folder", QDir::toNativeSeparators(existing.absolutePath())},
        {"name", existing.fileName()},
        {"existingSize", static_cast<double>(existing.size())},
        {"existingSizeText", QString::fromStdString(orbit::utils::formatBytes(static_cast<std::uint64_t>(existing.size())))},
        {"existingModifiedText", existing.exists() ? locale.toString(existing.lastModified(), QLocale::ShortFormat) : QString()},
        {"incomingSizeText", snap.stats.totalBytes > 0 ? QString::fromStdString(orbit::utils::formatBytes(snap.stats.totalBytes))
                                                       : QStringLiteral("unknown size")},
        {"keepBothName", keepBothName},
    };
}

QString DownloadListModel::resolveFileConflict(qlonglong id, const QString& action) {
    const auto choice = action == QLatin1String("replace") ? orbit::core::FileConflictAction::Overwrite
                                                           : orbit::core::FileConflictAction::Rename;
    return QString::fromStdString(manager_.resolveFileConflict(static_cast<DownloadId>(id), choice));
}

QVariantList DownloadListModel::pendingFileConflicts() const {
    QVariantList ids;
    for (const TaskSnapshot& snap : items_) {
        if (!snap.fileConflictPath.empty()) ids.push_back(static_cast<qlonglong>(snap.id));
    }
    return ids;
}

void DownloadListModel::applyRemoval(DownloadId id) {
    auto it = std::find_if(items_.begin(), items_.end(), [&](const TaskSnapshot& s) { return s.id == id; });
    if (it == items_.end()) return;
    const int row = static_cast<int>(std::distance(items_.begin(), it));
    beginRemoveRows(QModelIndex(), row, row);
    items_.erase(it);
    endRemoveRows();
    emit countChanged();
    ++revision_;
    emit revisionChanged();
}
