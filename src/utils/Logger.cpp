#include "utils/Logger.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace orbit::utils {

namespace {
std::mutex g_ioMutex;

const char* levelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}
}

std::string formatCurrentTimestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tmBuf{};
#if defined(_WIN32)
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::setLogFile(const std::string& path) {
    std::scoped_lock lock(g_ioMutex);
    logFilePath_ = path;
}

void Logger::log(LogLevel level, std::string_view tag, std::string_view message) {
    if (level < minLevel_.load(std::memory_order_relaxed)) return;

    std::ostringstream line;
    line << '[' << formatCurrentTimestamp() << "] [" << levelName(level) << "] [" << tag << "] "
         << message;

    std::scoped_lock lock(g_ioMutex);
    if (level >= LogLevel::Warn) {
        std::fprintf(stderr, "%s\n", line.str().c_str());
        std::fflush(stderr);
    } else {
        std::fprintf(stdout, "%s\n", line.str().c_str());
        std::fflush(stdout);
    }
    if (!logFilePath_.empty()) {
        std::ofstream file(logFilePath_, std::ios::app);
        if (file) {
            file << line.str() << '\n';
        }
    }
}

}
