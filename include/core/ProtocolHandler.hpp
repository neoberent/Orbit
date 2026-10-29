#pragma once

#include <optional>
#include <string>

namespace orbit::core::protocol {

inline constexpr const char* kScheme = "orbit://";

[[nodiscard]] bool isOrbitUri(const std::string& uri) noexcept;

// Parses "orbit://add?url=<percent-encoded-url>" and returns the decoded target URL.
[[nodiscard]] std::optional<std::string> extractDownloadUrl(const std::string& uri);

#if defined(_WIN32)
// Registers the orbit:// scheme for the current user (HKCU, no admin rights required)
// so the OS routes orbit:// links to this executable. Safe to call on every launch.
void registerWindowsProtocolHandler(const std::string& executablePath);

// Registers Orbit as a candidate handler for .torrent files (HKCU, no admin rights
// required) so it's selectable as the default app for opening them. Safe to call on
// every launch.
void registerWindowsTorrentFileAssociation(const std::string& executablePath);
#endif

}
