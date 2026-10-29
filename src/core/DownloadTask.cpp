#include "core/DownloadTask.hpp"

#include <chrono>

namespace orbit::core {

DownloadTask::DownloadTask(DownloadId id, DownloadType type, DownloadRequest request)
    : id_(id), type_(type), request_(std::move(request)) {
    snapshot_.id = id_;
    snapshot_.type = type_;
    snapshot_.state = DownloadState::Queued;
    snapshot_.source = request_.source;
    snapshot_.name = request_.suggestedName;
    snapshot_.addedAt = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
}

DownloadRequest DownloadTask::request() const {
    std::scoped_lock lock(mutex_);
    return request_;
}

void DownloadTask::updateRequest(const std::function<void(DownloadRequest&)>& change) {
    std::scoped_lock lock(mutex_);
    change(request_);
}

void DownloadTask::updateStats(const DownloadStats& stats) {
    std::scoped_lock lock(mutex_);
    snapshot_.stats = stats;
}

void DownloadTask::setState(DownloadState state) {
    std::scoped_lock lock(mutex_);
    snapshot_.state = state;
    if (state != DownloadState::Error) snapshot_.errorMessage.clear();
    if (state != DownloadState::Paused) snapshot_.fileConflictPath.clear();
}

void DownloadTask::setError(const std::string& message) {
    std::scoped_lock lock(mutex_);
    snapshot_.state = DownloadState::Error;
    snapshot_.errorMessage = message;
    snapshot_.fileConflictPath.clear();
}

void DownloadTask::setFileConflictPath(const std::string& path) {
    std::scoped_lock lock(mutex_);
    snapshot_.fileConflictPath = path;
}

void DownloadTask::setSource(const std::string& source) {
    std::scoped_lock lock(mutex_);
    snapshot_.source = source;
}

void DownloadTask::setName(const std::string& name) {
    std::scoped_lock lock(mutex_);
    snapshot_.name = name;
}

void DownloadTask::setDestinationPath(const std::filesystem::path& path) {
    std::scoped_lock lock(mutex_);
    snapshot_.destinationPath = path.string();
}

void DownloadTask::setAddedAt(std::int64_t unixSeconds) {
    std::scoped_lock lock(mutex_);
    snapshot_.addedAt = unixSeconds;
}

void DownloadTask::setChecksumState(ChecksumState state, const std::string& detail) {
    std::scoped_lock lock(mutex_);
    snapshot_.checksumState = state;
    snapshot_.checksumDetail = detail;
}

TaskSnapshot DownloadTask::snapshot() const {
    std::scoped_lock lock(mutex_);
    TaskSnapshot snap = snapshot_;
    snap.options.maxConnections = request_.maxConnections;
    snap.options.speedLimitBytesPerSec = request_.speedLimitBytesPerSec;
    snap.options.uploadLimitBytesPerSec = request_.uploadLimitBytesPerSec;
    snap.options.sequential = request_.sequential;
    snap.options.expectedChecksum = request_.expectedChecksum;
    snap.options.scheduledStartAt = request_.scheduledStartAt;
    snap.options.hasBrowserSession = !request_.cookies.empty();
    snap.options.destinationDir = request_.destinationDir;
    return snap;
}

}
