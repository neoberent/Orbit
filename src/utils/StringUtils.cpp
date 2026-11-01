#include "utils/StringUtils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace orbit::utils {

std::string trim(std::string_view input) {
    size_t begin = 0;
    size_t end = input.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(input[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1]))) --end;
    return std::string(input.substr(begin, end - begin));
}

std::vector<std::string> split(std::string_view input, char delimiter) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start <= input.size()) {
        const size_t pos = input.find(delimiter, start);
        if (pos == std::string_view::npos) {
            result.emplace_back(input.substr(start));
            break;
        }
        result.emplace_back(input.substr(start, pos - start));
        start = pos + 1;
    }
    return result;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

bool startsWith(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string formatBytes(std::uint64_t bytes) {
    static constexpr std::array<const char*, 6> units{"B", "KB", "MB", "GB", "TB", "PB"};
    double value = static_cast<double>(bytes);
    size_t unitIndex = 0;
    while (value >= 1024.0 && unitIndex + 1 < units.size()) {
        value /= 1024.0;
        ++unitIndex;
    }
    char buf[64];
    if (unitIndex == 0) {
        std::snprintf(buf, sizeof(buf), "%llu %s", static_cast<unsigned long long>(bytes), units[unitIndex]);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f %s", value, units[unitIndex]);
    }
    return std::string(buf);
}

std::string formatRate(double bytesPerSecond) {
    return formatBytes(static_cast<std::uint64_t>(std::max(0.0, bytesPerSecond))) + "/s";
}

std::string formatDuration(std::chrono::seconds seconds) {
    auto total = seconds.count();
    if (total < 0) total = 0;
    const auto hours = total / 3600;
    const auto minutes = (total % 3600) / 60;
    const auto secs = total % 60;

    char buf[32];
    if (hours > 0) {
        std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", static_cast<long long>(hours),
                       static_cast<long long>(minutes), static_cast<long long>(secs));
    } else {
        std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(minutes),
                       static_cast<long long>(secs));
    }
    return std::string(buf);
}

std::string urlDecode(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hexVal(input[i + 1]);
            const int lo = hexVal(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (input[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(input[i]);
        }
    }
    return out;
}

std::string fileNameFromUrl(std::string_view url) {
    std::string_view withoutQuery = url;
    if (const auto q = withoutQuery.find_first_of("?#"); q != std::string_view::npos) {
        withoutQuery = withoutQuery.substr(0, q);
    }
    const auto slash = withoutQuery.find_last_of('/');
    std::string_view name = slash == std::string_view::npos ? withoutQuery : withoutQuery.substr(slash + 1);
    std::string decoded = urlDecode(name);
    return trim(decoded);
}

bool isMagnetUri(std::string_view text) noexcept {
    return startsWith(text, "magnet:?");
}

bool isMediaUrl(std::string_view url) noexcept {
    if (!startsWith(url, "http://") && !startsWith(url, "https://")) return false;

    // Isolate the host component: after "scheme://", drop any "user:pass@" prefix,
    // then stop at the first '/', '?', '#', or ':' (port).
    std::string_view rest = url.substr(url.find("://") + 3);
    // Only an '@' inside the authority starts the host; "https://a.com/@b.com" is on a.com.
    rest = rest.substr(0, rest.find_first_of("/?#"));
    if (const auto at = rest.rfind('@'); at != std::string_view::npos) rest = rest.substr(at + 1);
    const auto hostEnd = rest.find(':');
    std::string_view host = hostEnd == std::string_view::npos ? rest : rest.substr(0, hostEnd);

    std::string lowerHost;
    lowerHost.reserve(host.size());
    for (char c : host) lowerHost.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    static constexpr std::array<std::string_view, 17> kKnownMediaHosts{
        "youtube.com",   "youtu.be",     "m.youtube.com", "music.youtube.com",
        "tiktok.com",    "vm.tiktok.com","twitch.tv",     "clips.twitch.tv",
        "vimeo.com",     "dailymotion.com", "facebook.com", "fb.watch",
        "instagram.com", "twitter.com",  "x.com",          "soundcloud.com",
        "streamable.com",
    };
    // Match the host exactly, or as a subdomain of a known host (e.g. "www.youtube.com"),
    // never as a substring anywhere in the URL (which would also match netflix.com, etc.
    // against "x.com").
    for (std::string_view known : kKnownMediaHosts) {
        if (lowerHost == known) return true;
        if (lowerHost.size() > known.size() + 1 && lowerHost[lowerHost.size() - known.size() - 1] == '.' &&
            lowerHost.compare(lowerHost.size() - known.size(), known.size(), known) == 0) {
            return true;
        }
    }
    return false;
}

}
