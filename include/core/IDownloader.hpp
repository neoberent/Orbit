#pragma once

#include "core/Types.hpp"
#include "core/ProgressReporter.hpp"

namespace orbit::core {

class IDownloader {
public:
    virtual ~IDownloader() = default;

    virtual void start(DownloadId id, const DownloadRequest& request) = 0;

    virtual void pause(DownloadId id) = 0;

    virtual void resume(DownloadId id) = 0;

    virtual void cancel(DownloadId id) = 0;

    // Releases any engine-side bookkeeping for a task that is no longer tracked by
    // DownloadManager (e.g. after the user removes it from the list). Unlike cancel(),
    // this must not touch files already written to disk. A no-op if the task is still
    // actively running or unknown.
    virtual void forget(DownloadId id) = 0;

    [[nodiscard]] virtual DownloadStats stats(DownloadId id) const = 0;
};

}
