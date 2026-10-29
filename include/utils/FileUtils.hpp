#pragma once

#include <filesystem>
#include <string>
#include <cstdint>
#include <functional>
#include <optional>

namespace orbit::utils {

bool ensureDirectoryExists(const std::filesystem::path& dir);

bool preallocateFile(const std::filesystem::path& path, std::uint64_t sizeBytes);

std::filesystem::path uniquePath(const std::filesystem::path& desired);

std::optional<std::uint64_t> fileSize(const std::filesystem::path& path);

std::string sanitizeFileName(const std::string& name);

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
