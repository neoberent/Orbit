#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <chrono>

namespace orbit::utils {

[[nodiscard]] std::string trim(std::string_view input);

[[nodiscard]] std::vector<std::string> split(std::string_view input, char delimiter);

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept;

[[nodiscard]] bool startsWith(std::string_view text, std::string_view prefix) noexcept;

[[nodiscard]] std::string formatBytes(std::uint64_t bytes);

[[nodiscard]] std::string formatRate(double bytesPerSecond);

[[nodiscard]] std::string formatDuration(std::chrono::seconds seconds);

[[nodiscard]] std::string fileNameFromUrl(std::string_view url);

[[nodiscard]] bool isMagnetUri(std::string_view text) noexcept;

// Heuristic check for links that should be routed to the yt-dlp media engine
// (YouTube, TikTok, Twitch, and other common video/audio hosting sites) rather
// than the plain HTTP downloader.
[[nodiscard]] bool isMediaUrl(std::string_view url) noexcept;

[[nodiscard]] std::string urlDecode(std::string_view input);

}
