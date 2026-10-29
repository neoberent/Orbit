#include "utils/SpeedLimiter.hpp"

#include <algorithm>
#include <thread>

namespace orbit::utils {

namespace {
// How far the schedule may lag behind "now" - i.e. the largest burst allowed after an
// idle moment. Small enough that the measured rate stays close to the cap.
constexpr std::chrono::milliseconds kMaxBurst{150};
constexpr std::chrono::milliseconds kSleepSlice{50};
}

void SpeedLimiter::setLimitBytesPerSec(std::uint64_t bytesPerSec) {
    std::scoped_lock lock(mutex_);
    limitBytesPerSec_ = bytesPerSec;
    nextFree_ = std::chrono::steady_clock::now();
}

std::uint64_t SpeedLimiter::limitBytesPerSec() const {
    std::scoped_lock lock(mutex_);
    return limitBytesPerSec_;
}

void SpeedLimiter::consume(std::uint64_t bytes, const std::atomic<bool>* abortA, const std::atomic<bool>* abortB) {
    if (bytes == 0) return;

    std::chrono::steady_clock::time_point until;
    {
        std::scoped_lock lock(mutex_);
        if (limitBytesPerSec_ == 0) return;
        const auto now = std::chrono::steady_clock::now();
        nextFree_ = std::max(nextFree_, now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(kMaxBurst));
        const auto cost = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(static_cast<double>(bytes) / static_cast<double>(limitBytesPerSec_)));
        nextFree_ += cost;
        until = nextFree_;
    }

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= until) return;
        if ((abortA && abortA->load(std::memory_order_relaxed)) || (abortB && abortB->load(std::memory_order_relaxed))) return;
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(until - now, kSleepSlice));
    }
}

}
