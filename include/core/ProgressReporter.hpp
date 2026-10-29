#pragma once

#include "core/Types.hpp"
#include <functional>
#include <string>

namespace orbit::core {

using ProgressCallback = std::function<void(DownloadId, const DownloadStats&)>;

using StateChangeCallback = std::function<void(DownloadId, DownloadState)>;

using ErrorCallback = std::function<void(DownloadId, const std::string& message)>;

using NameResolvedCallback =
    std::function<void(DownloadId, const std::string& name, const std::string& destinationPath)>;

// The outcome of a user-requested operation that finishes asynchronously (moving a
// torrent's storage, for example). It does not change the task's state.
using NoticeCallback = std::function<void(DownloadId, bool ok, const std::string& message)>;

// A download stopped before writing anything because its file already exists and the
// user asked to be consulted (FileConflictAction::Ask).
using FileConflictCallback = std::function<void(DownloadId, const std::string& existingPath)>;

struct ProgressReporter {
    ProgressCallback onProgress;
    StateChangeCallback onStateChanged;
    ErrorCallback onError;
    NameResolvedCallback onNameResolved;
    NoticeCallback onNotice;
    FileConflictCallback onFileConflict;

    void reportProgress(DownloadId id, const DownloadStats& stats) const {
        if (onProgress) onProgress(id, stats);
    }
    void reportStateChanged(DownloadId id, DownloadState state) const {
        if (onStateChanged) onStateChanged(id, state);
    }
    void reportError(DownloadId id, const std::string& message) const {
        if (onError) onError(id, message);
    }
    void reportNameResolved(DownloadId id, const std::string& name, const std::string& path) const {
        if (onNameResolved) onNameResolved(id, name, path);
    }
    void reportFileConflict(DownloadId id, const std::string& existingPath) const {
        if (onFileConflict) onFileConflict(id, existingPath);
    }
    void reportNotice(DownloadId id, bool ok, const std::string& message) const {
        if (onNotice) onNotice(id, ok, message);
    }
};

}
