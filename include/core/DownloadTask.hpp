#pragma once

#include "core/Types.hpp"
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

namespace orbit::core {

class DownloadTask {
public:
    DownloadTask(DownloadId id, DownloadType type, DownloadRequest request);

    [[nodiscard]] DownloadId id() const noexcept { return id_; }
    [[nodiscard]] DownloadType type() const noexcept { return type_; }
    [[nodiscard]] DownloadRequest request() const;
    void updateRequest(const std::function<void(DownloadRequest&)>& change);

    void updateStats(const DownloadStats& stats);
    void setState(DownloadState state);
    void setError(const std::string& message);
    void setName(const std::string& name);
    void setSource(const std::string& source);
    void setDestinationPath(const std::filesystem::path& path);
    void setAddedAt(std::int64_t unixSeconds);
    void setChecksumState(ChecksumState state, const std::string& detail = {});
    // Cleared again by any state change other than Paused.
    void setFileConflictPath(const std::string& path);

    [[nodiscard]] TaskSnapshot snapshot() const;

private:
    const DownloadId id_;
    const DownloadType type_;

    mutable std::mutex mutex_;
    DownloadRequest request_;
    TaskSnapshot snapshot_;
};

}
