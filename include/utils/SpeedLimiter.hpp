#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace orbit::utils {

// A shared, thread-safe rate limiter. Every caller that wants to be throttled
// together (e.g. every HTTP download's write callback, for one process-wide cap)
// attaches to the same instance and calls consume() with the bytes it just
// transferred; consume() blocks the calling thread just long enough to keep the
// combined rate at or below the configured cap.
//
// It paces rather than counting per fixed window: each consume() reserves the next
// slice of time on one shared schedule (bytes / limit), so any number of concurrent
// connections add up to the cap instead of each getting a fresh window's budget.
//
// curl's own CURLOPT_MAX_RECV_SPEED_LARGE throttles a single easy handle and restarts
// with every request - it can't express "the sum across many connections must stay
// under X", which is exactly the shape both the global and per-download limits need.
class SpeedLimiter {
public:
    // 0 (the default) means unlimited - consume() then returns immediately.
    void setLimitBytesPerSec(std::uint64_t bytesPerSec);
    [[nodiscard]] std::uint64_t limitBytesPerSec() const;

    // Waits (in short slices) until the bytes fit the schedule. Returns early when any
    // of the given flags becomes true, so a pause or cancel is never held up.
    void consume(std::uint64_t bytes, const std::atomic<bool>* abortA = nullptr,
                 const std::atomic<bool>* abortB = nullptr);

private:
    mutable std::mutex mutex_;
    std::uint64_t limitBytesPerSec_{0};
    std::chrono::steady_clock::time_point nextFree_{std::chrono::steady_clock::now()};
};

}
