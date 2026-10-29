#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <sstream>

namespace orbit::utils {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

// Logger is called concurrently from the UI thread and background download/torrent
// threads, so its mutable state must be safe to touch from any of them.
class Logger {
public:
    static Logger& instance();

    void setMinLevel(LogLevel level) noexcept { minLevel_.store(level, std::memory_order_relaxed); }
    void setLogFile(const std::string& path);

    void log(LogLevel level, std::string_view tag, std::string_view message);

private:
    Logger() = default;
    std::atomic<LogLevel> minLevel_{LogLevel::Info};
    std::string logFilePath_{};  // guarded by g_ioMutex in Logger.cpp
};

std::string formatCurrentTimestamp();

}

#define ORBIT_LOG_TRACE(tag, msg) ::orbit::utils::Logger::instance().log(::orbit::utils::LogLevel::Trace, tag, msg)
#define ORBIT_LOG_DEBUG(tag, msg) ::orbit::utils::Logger::instance().log(::orbit::utils::LogLevel::Debug, tag, msg)
#define ORBIT_LOG_INFO(tag, msg)  ::orbit::utils::Logger::instance().log(::orbit::utils::LogLevel::Info, tag, msg)
#define ORBIT_LOG_WARN(tag, msg)  ::orbit::utils::Logger::instance().log(::orbit::utils::LogLevel::Warn, tag, msg)
#define ORBIT_LOG_ERROR(tag, msg) ::orbit::utils::Logger::instance().log(::orbit::utils::LogLevel::Error, tag, msg)
