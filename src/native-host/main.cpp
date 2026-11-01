// Native messaging host for the Orbit browser extension.
//
// The browser starts this program and talks to it over stdin/stdout: every message is a
// 4-byte length (native byte order) followed by that many bytes of UTF-8 JSON. Each
// request is forwarded as one line of JSON to the running Orbit over its local API pipe
// (a Unix socket in the user's runtime directory on Linux), and Orbit's one-line reply is
// passed back. If Orbit isn't running, it is started and given a moment to open the pipe.

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#else
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <thread>

namespace {

constexpr std::uint32_t kMaxMessageBytes = 8u * 1024u * 1024u;
#if defined(_WIN32)
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\OrbitDownloadManager.Api";
constexpr DWORD kLaunchTimeoutMs = 25000;
constexpr DWORD kReplyTimeoutMs = 20000;
#else
constexpr char kSocketName[] = "OrbitDownloadManager.Api";
constexpr int kLaunchTimeoutMs = 25000;
constexpr int kReplyTimeoutMs = 20000;
#endif

bool readExact(void* buffer, size_t size) {
    auto* out = static_cast<char*>(buffer);
    size_t done = 0;
    while (done < size) {
        const size_t got = std::fread(out + done, 1, size - done, stdin);
        if (got == 0) return false;
        done += got;
    }
    return true;
}

void writeMessage(const std::string& json) {
    const auto size = static_cast<std::uint32_t>(json.size());
    std::fwrite(&size, sizeof(size), 1, stdout);
    std::fwrite(json.data(), 1, json.size(), stdout);
    std::fflush(stdout);
}

std::string escapeJson(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string errorReply(const std::string& message) {
    return "{\"ok\":false,\"error\":\"" + escapeJson(message) + "\"}";
}

#if defined(_WIN32)
std::wstring orbitExecutablePath() {
    wchar_t path[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) return {};
    std::wstring self(path, length);
    const size_t slash = self.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring() : self.substr(0, slash + 1)) + L"orbit.exe";
}

HANDLE openPipe(DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (true) {
        // SECURITY_IDENTIFICATION: whoever holds the pipe may learn who connected, but can't
        // act as this user, even if another account created a pipe of the same name first.
        HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        const DWORD error = GetLastError();
        if (GetTickCount64() >= deadline) return INVALID_HANDLE_VALUE;
        if (error == ERROR_PIPE_BUSY) WaitNamedPipeW(kPipeName, 500);
        else Sleep(250);
    }
}

bool launchOrbit() {
    const std::wstring exe = orbitExecutablePath();
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring commandLine = L"\"" + exe + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    // Detached from the browser's job and console so Orbit keeps running after this
    // helper exits.
    const DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB;
    BOOL started = CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr,
                                  &startup, &process);
    if (!started) {
        started = CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                                 DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process);
    }
    if (!started) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

std::string forward(const std::string& request) {
    HANDLE pipe = openPipe(300);
    if (pipe == INVALID_HANDLE_VALUE) {
        if (!launchOrbit()) return errorReply("Orbit isn't installed next to its browser helper");
        pipe = openPipe(kLaunchTimeoutMs);
        if (pipe == INVALID_HANDLE_VALUE) return errorReply("Orbit didn't start in time");
    }

    std::string line = request;
    for (char& c : line) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    line.push_back('\n');

    DWORD written = 0;
    if (!WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) || written != line.size()) {
        CloseHandle(pipe);
        return errorReply("Couldn't send the request to Orbit");
    }

    std::string reply;
    char buffer[4096];
    const ULONGLONG deadline = GetTickCount64() + kReplyTimeoutMs;
    while (reply.find('\n') == std::string::npos && GetTickCount64() < deadline) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available == 0) {
            Sleep(20);
            continue;
        }
        DWORD got = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr) || got == 0) break;
        reply.append(buffer, got);
    }
    CloseHandle(pipe);

    const size_t newline = reply.find('\n');
    if (newline == std::string::npos) return errorReply("Orbit didn't answer");
    reply.resize(newline);
    return reply;
}

#else

std::string selfDirectory() {
    char path[PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0) return {};
    std::string self(path, static_cast<size_t>(length));
    const size_t slash = self.find_last_of('/');
    return slash == std::string::npos ? std::string() : self.substr(0, slash + 1);
}

// Must match QStandardPaths::RuntimeLocation, which Orbit's LocalApiServer listens in.
std::string socketPath() {
    std::string dir;
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime) {
        dir = runtime;
    } else {
        const char* tmp = std::getenv("TMPDIR");
        const passwd* user = getpwuid(getuid());
        dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/runtime-" + (user ? user->pw_name : std::to_string(getuid()));
    }
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    return dir + "/" + kSocketName;
}

int connectOnce(const std::string& path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) return -1;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
#if defined(__linux__)
        // The requests carry browser cookies; only hand them to an Orbit of this same user.
        ucred peer{};
        socklen_t length = sizeof(peer);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) == 0 && peer.uid == getuid()) return fd;
#else
        return fd;
#endif
    }
    close(fd);
    return -1;
}

int openSocket(int timeoutMs) {
    const std::string path = socketPath();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        const int fd = connectOnce(path);
        if (fd >= 0) return fd;
        if (std::chrono::steady_clock::now() >= deadline) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
}

bool launchOrbit() {
    const std::string exe = selfDirectory() + "orbit";
    if (access(exe.c_str(), X_OK) != 0) return false;
    // Double fork into a new session so Orbit isn't a child of the browser (which kills
    // the host's process group when the port closes) and doesn't hold the browser's pipes.
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        setsid();
        if (fork() != 0) _exit(0);
        const int devNull = open("/dev/null", O_RDWR);
        if (devNull >= 0) {
            dup2(devNull, STDIN_FILENO);
            dup2(devNull, STDOUT_FILENO);
            dup2(devNull, STDERR_FILENO);
            if (devNull > STDERR_FILENO) close(devNull);
        }
        execl(exe.c_str(), exe.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool writeAll(int fd, const std::string& data) {
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t sent = send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        done += static_cast<size_t>(sent);
    }
    return true;
}

std::string forward(const std::string& request) {
    int fd = openSocket(300);
    if (fd < 0) {
        if (!launchOrbit()) return errorReply("Orbit isn't installed next to its browser helper");
        fd = openSocket(kLaunchTimeoutMs);
        if (fd < 0) return errorReply("Orbit didn't start in time");
    }

    std::string line = request;
    for (char& c : line) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    line.push_back('\n');

    if (!writeAll(fd, line)) {
        close(fd);
        return errorReply("Couldn't send the request to Orbit");
    }

    std::string reply;
    char buffer[4096];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kReplyTimeoutMs);
    while (reply.find('\n') == std::string::npos) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) break;
        pollfd waitFor{fd, POLLIN, 0};
        const int ready = poll(&waitFor, 1, static_cast<int>(left));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        const ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        reply.append(buffer, static_cast<size_t>(got));
    }
    close(fd);

    const size_t newline = reply.find('\n');
    if (newline == std::string::npos) return errorReply("Orbit didn't answer");
    reply.resize(newline);
    return reply;
}

#endif

}

int main() {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    while (true) {
        std::uint32_t size = 0;
        if (!readExact(&size, sizeof(size))) break;
        if (size == 0 || size > kMaxMessageBytes) {
            writeMessage(errorReply("Message too large"));
            break;
        }
        std::string request(size, '\0');
        if (!readExact(request.data(), size)) break;
        writeMessage(forward(request));
    }
    return 0;
}
