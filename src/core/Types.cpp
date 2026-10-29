#include "core/Types.hpp"

#include <cctype>
#include <cstdio>

namespace orbit::core {

const char* toString(DownloadState state) noexcept {
    switch (state) {
        case DownloadState::Queued:      return "Queued";
        case DownloadState::Connecting:  return "Connecting";
        case DownloadState::Downloading: return "Downloading";
        case DownloadState::Paused:      return "Paused";
        case DownloadState::Seeding:     return "Seeding";
        case DownloadState::Completed:   return "Completed";
        case DownloadState::Error:       return "Error";
        case DownloadState::Cancelled:   return "Cancelled";
    }
    return "Unknown";
}

const char* toString(FileConflictAction action) noexcept {
    switch (action) {
        case FileConflictAction::Default:   return "default";
        case FileConflictAction::Ask:       return "ask";
        case FileConflictAction::Rename:    return "rename";
        case FileConflictAction::Overwrite: return "overwrite";
    }
    return "default";
}

FileConflictAction fileConflictActionFromString(const std::string& text) noexcept {
    if (text == "ask") return FileConflictAction::Ask;
    if (text == "rename") return FileConflictAction::Rename;
    if (text == "overwrite") return FileConflictAction::Overwrite;
    return FileConflictAction::Default;
}

std::string ProxySettings::url(bool withCredentials) const {
    if (!enabled()) return {};
    std::string scheme = type == Type::Socks5 ? "socks5h://" : "http://";
    std::string auth;
    if (withCredentials && !username.empty()) {
        auto escape = [](const std::string& text) {
            std::string out;
            for (unsigned char c : text) {
                if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
                    out.push_back(static_cast<char>(c));
                } else {
                    char buf[4];
                    std::snprintf(buf, sizeof(buf), "%%%02X", c);
                    out += buf;
                }
            }
            return out;
        };
        auth = escape(username) + ":" + escape(password) + "@";
    }
    const bool ipv6 = host.find(':') != std::string::npos;
    return scheme + auth + (ipv6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

const char* toString(DownloadType type) noexcept {
    switch (type) {
        case DownloadType::Http:    return "HTTP";
        case DownloadType::Torrent: return "Torrent";
        case DownloadType::Media:   return "Media";
    }
    return "Unknown";
}

}
