#pragma once

#include <filesystem>
#include <string>
#include <cstdint>
#include <functional>
#include <optional>

namespace orbit::utils {

bool ensureDirectoryExists(const std::filesystem::path& dir);

// How Orbit was packaged on Linux: "flatpak", "appimage", or "" for a normal install. A
// sandboxed or self-mounting copy can't register itself with the desktop, install the
// browser helper or update itself in place; its package manager or a new AppImage does.
[[nodiscard]] std::string linuxPackaging();

bool preallocateFile(const std::filesystem::path& path, std::uint64_t sizeBytes);

std::filesystem::path uniquePath(const std::filesystem::path& desired);

std::optional<std::uint64_t> fileSize(const std::filesystem::path& path);

// Replaces characters no file name may hold and keeps it within kMaxFileNameBytes (cut at a
// UTF-8 character, extension kept), so a long name from a server or URL can always be
// created, with room left for " (12)" and the ".orbitresume" beside it.
inline constexpr std::size_t kMaxFileNameBytes = 200;
std::string sanitizeFileName(const std::string& name);
// Replaces path with data so that after a crash or power cut it holds either the old
// contents or the new ones, never a truncated mix: written beside it, flushed to disk,
// renamed over it, and the folder flushed too. False if any step failed (path untouched).
bool writeFileAtomically(const std::filesystem::path& path, const std::string& data);

std::filesystem::path appDataDir();

// Redirects appDataDir() (logs, task state, torrent resume data) to another folder, as
// portable mode and isolated test profiles do. Must be called before anything reads it.
void setAppDataDirOverride(const std::filesystem::path& dir);

// Moves a file, falling back to copy + delete across volumes. The copy checks
// shouldStop between chunks; on abort or failure the source is left untouched and any
// partial copy removed. Returns an empty string on success, otherwise the reason.
std::string moveFile(const std::filesystem::path& from, const std::filesystem::path& to,
                     const std::function<bool()>& shouldStop = {});

std::filesystem::path defaultDownloadsDir();

}
