#include "utils/FileUtils.hpp"
#include "utils/Logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <shlobj.h>
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace fs = std::filesystem;

namespace orbit::utils {

bool ensureDirectoryExists(const fs::path& dir) {
    std::error_code ec;
    if (fs::exists(dir, ec)) return fs::is_directory(dir, ec);
    return fs::create_directories(dir, ec);
}

bool preallocateFile(const fs::path& path, std::uint64_t sizeBytes) {
    std::error_code ec;
    ensureDirectoryExists(path.parent_path());

#if defined(_WIN32)
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        ORBIT_LOG_ERROR("FileUtils", "CreateFileW failed for \"" + path.string() +
                                          "\" (GetLastError=" + std::to_string(::GetLastError()) + ")");
        return false;
    }

    LARGE_INTEGER size{};
    size.QuadPart = static_cast<LONGLONG>(sizeBytes);
    bool ok = ::SetFilePointerEx(handle, size, nullptr, FILE_BEGIN) && ::SetEndOfFile(handle);
    if (!ok) {
        ORBIT_LOG_ERROR("FileUtils", "Failed to size \"" + path.string() + "\" to " +
                                          std::to_string(sizeBytes) +
                                          " bytes (GetLastError=" + std::to_string(::GetLastError()) + ")");
    }
    ::CloseHandle(handle);
    return ok;
#else
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    if (sizeBytes > 0) {
        file.seekp(static_cast<std::streamoff>(sizeBytes - 1));
        char zero = 0;
        file.write(&zero, 1);
    }
    return static_cast<bool>(file);
#endif
}

fs::path uniquePath(const fs::path& desired) {
    if (!fs::exists(desired)) return desired;

    const fs::path stem = desired.stem();
    const fs::path ext = desired.extension();
    const fs::path parent = desired.parent_path();

    for (int i = 1; i < 10000; ++i) {
        fs::path candidate = parent / (stem.string() + " (" + std::to_string(i) + ")" + ext.string());
        if (!fs::exists(candidate)) return candidate;
    }
    return desired;
}

std::optional<std::uint64_t> fileSize(const fs::path& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return std::nullopt;
    return static_cast<std::uint64_t>(size);
}

std::string sanitizeFileName(const std::string& name) {
    static constexpr std::string_view forbidden = "\\/:*?\"<>|";
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        if (forbidden.find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 0x20) {
            result.push_back('_');
        } else {
            result.push_back(c);
        }
    }
    while (!result.empty() && (result.back() == ' ' || result.back() == '.')) {
        result.pop_back();
    }
    if (result.empty()) result = "download";
#if defined(_WIN32)
    // CON, NUL, COM1 and the like name devices, with or without an extension.
    std::string stem = result.substr(0, result.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    for (char& c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const bool numbered = stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) &&
                          stem[3] >= '0' && stem[3] <= '9';
    if (numbered || stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") result.insert(result.begin(), '_');
#endif
    return result;
}

namespace {
fs::path& appDataOverride() {
    static fs::path value;
    return value;
}
}

void setAppDataDirOverride(const fs::path& dir) { appDataOverride() = dir; }

std::string moveFile(const fs::path& from, const fs::path& to, const std::function<bool()>& shouldStop) {
    std::error_code ec;
    if (!fs::exists(from, ec)) return "The file no longer exists";
    ensureDirectoryExists(to.parent_path());
    fs::rename(from, to, ec);
    if (!ec) return {};

    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!in || !out) return "Could not open the file for moving";
    std::vector<char> buffer(4 << 20);
    while (in) {
        if (shouldStop && shouldStop()) {
            out.close();
            fs::remove(to, ec);
            return "Move was interrupted";
        }
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = in.gcount();
        if (got > 0) out.write(buffer.data(), got);
        if (!out) {
            out.close();
            fs::remove(to, ec);
            return "Not enough space or no permission at the destination";
        }
    }
    in.close();
    out.close();
    fs::remove(from, ec);
    if (ec) return "Copied, but the original could not be removed: " + ec.message();
    return {};
}

fs::path appDataDir() {
    if (!appDataOverride().empty()) return appDataOverride();
#if defined(_WIN32)
    PWSTR path = nullptr;
    fs::path result;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path))) {
        result = fs::path(path) / "Orbit";
        ::CoTaskMemFree(path);
    } else {
        result = fs::temp_directory_path() / "Orbit";
    }
    return result;
#else
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) / ".local/share" : fs::temp_directory_path();
    return base / "orbit";
#endif
}

fs::path defaultDownloadsDir() {
#if defined(_WIN32)
    PWSTR path = nullptr;
    fs::path result;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path))) {
        result = fs::path(path);
        ::CoTaskMemFree(path);
    } else {
        result = fs::path(std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : ".") / "Downloads";
    }
    return result;
#else
    const char* home = std::getenv("HOME");
    return home ? fs::path(home) / "Downloads" : fs::current_path();
#endif
}

}
