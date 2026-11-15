#include "utils/FileUtils.hpp"

#if !defined(_WIN32)
#include <QStandardPaths>
#include <QString>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif
#include "utils/Logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <io.h>
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
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        ORBIT_LOG_ERROR("FileUtils", "Couldn't create \"" + path.string() + "\": " + std::strerror(errno));
        return false;
    }
    bool ok = true;
    if (sizeBytes > 0) {
        // Reserve the space up front, so another program can't fill the disk while this
        // download is halfway through. fallocate() does it instantly on ext4, btrfs and
        // xfs; where a filesystem can't, the file is just sized (sparse) as before.
        if (::fallocate(fd, 0, 0, static_cast<off_t>(sizeBytes)) != 0) {
            const int err = errno;
            if (err == ENOSPC) {
                ORBIT_LOG_ERROR("FileUtils", "Not enough space to reserve " + std::to_string(sizeBytes) + " bytes for \"" +
                                                 path.string() + "\"");
                ok = false;
            } else {
                ok = ::ftruncate(fd, static_cast<off_t>(sizeBytes)) == 0;
                if (!ok) ORBIT_LOG_ERROR("FileUtils", "Couldn't size \"" + path.string() + "\": " + std::strerror(errno));
            }
        }
    }
    ::close(fd);
    if (!ok) {
        std::error_code removeEc;
        fs::remove(path, removeEc);
    }
    return ok;
#endif
}

std::string linuxPackaging() {
#if defined(__linux__)
    if (std::getenv("FLATPAK_ID") || fs::exists("/.flatpak-info")) return "flatpak";
    if (std::getenv("APPIMAGE")) return "appimage";
#endif
    return {};
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
    if (result.size() > kMaxFileNameBytes) {
        // Keep a short extension; cut the rest at a character boundary, never inside one.
        const auto dot = result.rfind('.');
        std::string ext = dot != std::string::npos && result.size() - dot <= 16 ? result.substr(dot) : std::string();
        std::string stem = result.substr(0, kMaxFileNameBytes - ext.size());
        while (!stem.empty() && (static_cast<unsigned char>(stem.back()) & 0xC0) == 0x80) stem.pop_back();
        if (!stem.empty() && static_cast<unsigned char>(stem.back()) >= 0xC0) stem.pop_back();  // a lead byte left alone
        result = stem + ext;
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

bool writeFileAtomically(const fs::path& path, const std::string& data) {
    fs::path temp = path;
    temp += ".tmp";
    std::error_code ec;
#if defined(_WIN32)
    FILE* file = _wfopen(temp.c_str(), L"wb");
#else
    FILE* file = std::fopen(temp.c_str(), "wb");
#endif
    if (!file) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), file) == data.size() && std::fflush(file) == 0;
#if defined(_WIN32)
    ok = ok && ::FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file))));
#else
    ok = ok && ::fsync(::fileno(file)) == 0;
#endif
    ok = std::fclose(file) == 0 && ok;
    if (!ok) {
        fs::remove(temp, ec);
        return false;
    }
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        return false;
    }
#if !defined(_WIN32)
    // The rename itself lives in the folder; flush it too, or a power cut can undo it.
    if (const int dir = ::open(path.parent_path().empty() ? "." : path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        dir >= 0) {
        ::fsync(dir);
        ::close(dir);
    }
#endif
    return true;
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
    // $XDG_DATA_HOME/orbit (in a Flatpak that's the app's own persistent folder; outside
    // the sandbox ~/.local/share isn't kept). A folder from before XDG_DATA_HOME was
    // honoured stays in use, so nobody's downloads list moves or vanishes.
    static const fs::path resolved = [] {
        const char* home = std::getenv("HOME");
        const fs::path legacy = (home ? fs::path(home) / ".local/share" : fs::temp_directory_path()) / "orbit";
        const char* xdg = std::getenv("XDG_DATA_HOME");
        if (!xdg || !*xdg || !fs::path(xdg).is_absolute()) return legacy;
        const fs::path chosen = fs::path(xdg) / "orbit";
        std::error_code ec;
        if (chosen != legacy && !fs::exists(chosen, ec) && fs::exists(legacy, ec)) return legacy;
        return chosen;
    }();
    return resolved;
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
    // The desktop's own Downloads folder (user-dirs.dirs), which has the user's language
    // ("Téléchargements") or wherever they moved it.
    const QString location = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (!location.isEmpty()) return fs::path(location.toStdString());
    const char* home = std::getenv("HOME");
    return home ? fs::path(home) / "Downloads" : fs::current_path();
#endif
}

}
