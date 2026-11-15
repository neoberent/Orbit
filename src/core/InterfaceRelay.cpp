#include "core/InterfaceRelay.hpp"

#include "core/InterfaceDns.hpp"
#include "utils/Logger.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

namespace orbit::core {

#if defined(__linux__)

namespace {

constexpr int kMaxConnections = 64;
constexpr size_t kMaxHeadBytes = 16 * 1024;
constexpr int kHeadTimeoutMs = 15000;
constexpr int kConnectTimeoutMs = 15000;
constexpr int kPollSliceMs = 500;

std::string randomHex(int bytes) {
    std::vector<unsigned char> raw(static_cast<size_t>(bytes));
    if (RAND_bytes(raw.data(), bytes) != 1) return {};
    static const char* const kHex = "0123456789abcdef";
    std::string out;
    for (unsigned char c : raw) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0F]);
    }
    return out;
}

std::string base64(const std::string& text) {
    std::string out(4 * ((text.size() + 2) / 3) + 1, '\0');
    const int length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                       reinterpret_cast<const unsigned char*>(text.data()), static_cast<int>(text.size()));
    out.resize(length > 0 ? static_cast<size_t>(length) : 0);
    return out;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r");
    if (begin == std::string::npos) return {};
    return text.substr(begin, text.find_last_not_of(" \t\r") - begin + 1);
}

bool sendAll(int fd, const char* data, size_t size) {
    while (size > 0) {
        const ssize_t sent = ::send(fd, data, size, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

void replyStatus(int fd, const std::string& status, const std::string& extraHeaders = {}) {
    const std::string text = "HTTP/1.1 " + status + "\r\n" + extraHeaders + "Content-Length: 0\r\nConnection: close\r\n\r\n";
    sendAll(fd, text.data(), text.size());
}

// "host:port" or "[v6]:port"; the port defaults when absent. Only plain host names and
// IP addresses are accepted.
bool splitHostPort(const std::string& authority, int defaultPort, std::string& host, int& port) {
    port = defaultPort;
    if (authority.empty()) return false;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) return false;
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return false;
            port = std::atoi(authority.c_str() + close + 2);
        }
    } else {
        const auto colon = authority.rfind(':');
        host = colon == std::string::npos ? authority : authority.substr(0, colon);
        if (colon != std::string::npos) port = std::atoi(authority.c_str() + colon + 1);
    }
    host = lower(host);
    while (!host.empty() && host.back() == '.') host.pop_back();
    const bool clean = std::all_of(host.begin(), host.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == ':' || c == '_';
    });
    return clean && !host.empty() && port > 0 && port < 65536;
}

// Connects to host:port through the interface: the name is looked up through it, and the
// socket is bound to it, so a connection that can't go that way fails instead.
int connectThrough(const std::string& host, int port, const std::string& interfaceName) {
    for (const std::string& address : InterfaceDns::resolve(host, interfaceName)) {
        sockaddr_storage storage{};
        socklen_t length = 0;
        int family = AF_INET;
        if (auto* v4 = reinterpret_cast<sockaddr_in*>(&storage); ::inet_pton(AF_INET, address.c_str(), &v4->sin_addr) == 1) {
            v4->sin_family = AF_INET;
            v4->sin_port = htons(static_cast<uint16_t>(port));
            length = sizeof(sockaddr_in);
        } else if (auto* v6 = reinterpret_cast<sockaddr_in6*>(&storage); ::inet_pton(AF_INET6, address.c_str(), &v6->sin6_addr) == 1) {
            v6->sin6_family = AF_INET6;
            v6->sin6_port = htons(static_cast<uint16_t>(port));
            length = sizeof(sockaddr_in6);
            family = AF_INET6;
        } else {
            continue;
        }
        const int fd = ::socket(family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) continue;
        if (::setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interfaceName.c_str(),
                         static_cast<socklen_t>(interfaceName.size())) != 0) {
            // Never fall back to an unbound connection.
            ORBIT_LOG_WARN("relay", "Couldn't bind a connection to " + interfaceName);
            ::close(fd);
            return -1;
        }
        bool connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&storage), length) == 0;
        if (!connected && errno == EINPROGRESS) {
            pollfd wait{fd, POLLOUT, 0};
            int error = 0;
            socklen_t errorLength = sizeof(error);
            connected = ::poll(&wait, 1, kConnectTimeoutMs) == 1 &&
                        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorLength) == 0 && error == 0;
        }
        if (connected) {
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);
            return fd;
        }
        ::close(fd);
    }
    return -1;
}

}

struct InterfaceRelay::State {
    std::mutex mutex;
    std::string interfaceName;
    int listenFd{-1};
    int port{0};
    std::string username;
    std::string password;
    std::string expectedAuth;  // "basic <base64>", lower-case scheme
    std::atomic<bool> stopping{false};
    std::thread acceptThread;

    std::mutex connectionsMutex;
    std::condition_variable connectionsDone;
    std::set<int> clients;
    int live{0};

    std::string currentInterface() {
        std::scoped_lock lock(mutex);
        return interfaceName;
    }
};

namespace {

// Copies bytes both ways until both sides are done (or the relay stops).
void pump(int client, int server, const std::atomic<bool>& stopping) {
    int fds[2] = {client, server};
    bool open[2] = {true, true};
    std::vector<char> buffer(64 * 1024);
    while ((open[0] || open[1]) && !stopping.load()) {
        pollfd wait[2] = {{fds[0], static_cast<short>(open[0] ? POLLIN : 0), 0},
                          {fds[1], static_cast<short>(open[1] ? POLLIN : 0), 0}};
        const int ready = ::poll(wait, 2, kPollSliceMs);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return;
        for (int i = 0; i < 2; ++i) {
            if (!open[i] || !(wait[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const ssize_t got = ::recv(fds[i], buffer.data(), buffer.size(), 0);
            if (got > 0) {
                if (!sendAll(fds[1 - i], buffer.data(), static_cast<size_t>(got))) return;
            } else if (got == 0) {
                open[i] = false;
                ::shutdown(fds[1 - i], SHUT_WR);
            } else if (errno != EINTR && errno != EAGAIN) {
                return;
            }
        }
    }
}

}

namespace {

void handleClient(int client, const std::shared_ptr<InterfaceRelay::State>& state) {
    // The request head: "CONNECT host:443 HTTP/1.1" or "GET http://host/path HTTP/1.1".
    std::string head;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kHeadTimeoutMs);
    size_t headEnd = std::string::npos;
    char chunk[4096];
    while ((headEnd = head.find("\r\n\r\n")) == std::string::npos) {
        if (head.size() > kMaxHeadBytes || state->stopping.load()) return;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return;
        pollfd wait{client, POLLIN, 0};
        if (::poll(&wait, 1, static_cast<int>(std::min<long long>(left, kPollSliceMs))) <= 0) continue;
        const ssize_t got = ::recv(client, chunk, sizeof(chunk), 0);
        if (got <= 0) return;
        head.append(chunk, static_cast<size_t>(got));
    }
    const std::string leftover = head.substr(headEnd + 4);
    head.resize(headEnd);

    std::vector<std::string> lines;
    for (size_t start = 0; start <= head.size();) {
        size_t end = head.find("\r\n", start);
        if (end == std::string::npos) end = head.size();
        lines.push_back(head.substr(start, end - start));
        start = end + 2;
    }
    const std::string& requestLine = lines.front();
    const auto firstSpace = requestLine.find(' ');
    const auto secondSpace = requestLine.find(' ', firstSpace + 1);
    if (firstSpace == std::string::npos || secondSpace == std::string::npos) {
        replyStatus(client, "400 Bad Request");
        return;
    }
    const std::string method = requestLine.substr(0, firstSpace);
    const std::string target = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    const std::string version = requestLine.substr(secondSpace + 1);

    std::string auth;
    bool hasHost = false;
    std::vector<std::string> forwarded;
    for (size_t i = 1; i < lines.size(); ++i) {
        const auto colon = lines[i].find(':');
        if (colon == std::string::npos) continue;
        const std::string name = lower(trim(lines[i].substr(0, colon)));
        if (name == "proxy-authorization") {
            auth = trim(lines[i].substr(colon + 1));
            continue;
        }
        if (name == "proxy-connection" || name == "connection" || name == "keep-alive") continue;
        if (name == "host") hasHost = true;
        forwarded.push_back(lines[i]);
    }
    // Only the programs Orbit started know the credentials.
    const auto space = auth.find(' ');
    if (space == std::string::npos || lower(auth.substr(0, space)) + " " + trim(auth.substr(space + 1)) != state->expectedAuth) {
        replyStatus(client, "407 Proxy Authentication Required", "Proxy-Authenticate: Basic realm=\"Orbit\"\r\n");
        return;
    }

    const std::string interfaceName = state->currentInterface();
    if (interfaceName.empty()) {
        replyStatus(client, "503 Service Unavailable");
        return;
    }

    const bool tunnel = method == "CONNECT";
    std::string host;
    int port = 0;
    std::string authority = target;
    std::string path = "/";
    if (!tunnel) {
        // Only plain http comes as an absolute URL; https always uses CONNECT.
        if (lower(target).rfind("http://", 0) != 0) {
            replyStatus(client, "400 Bad Request");
            return;
        }
        authority = target.substr(7);
        if (const auto slash = authority.find_first_of("/?"); slash != std::string::npos) {
            path = authority.substr(slash);
            if (path.front() == '?') path.insert(path.begin(), '/');
            authority.resize(slash);
        }
        if (const auto at = authority.rfind('@'); at != std::string::npos) authority = authority.substr(at + 1);
    }
    if (!splitHostPort(authority, tunnel ? 443 : 80, host, port)) {
        replyStatus(client, "400 Bad Request");
        return;
    }

    const int server = connectThrough(host, port, interfaceName);
    if (server < 0) {
        ORBIT_LOG_WARN("relay", "Couldn't reach " + host + " through " + interfaceName);
        replyStatus(client, "502 Bad Gateway");
        return;
    }

    bool ok = true;
    if (tunnel) {
        static const std::string kEstablished = "HTTP/1.1 200 Connection established\r\n\r\n";
        ok = sendAll(client, kEstablished.data(), kEstablished.size());
    } else {
        std::string request = method + " " + path + " " + version + "\r\n";
        if (!hasHost) request += "Host: " + authority + "\r\n";
        for (const std::string& line : forwarded) request += line + "\r\n";
        // One request per connection, so the next one (maybe for another host) comes
        // through the relay again.
        request += "Connection: close\r\n\r\n";
        ok = sendAll(server, request.data(), request.size());
    }
    if (ok && !leftover.empty()) ok = sendAll(server, leftover.data(), leftover.size());
    if (ok) pump(client, server, state->stopping);
    ::close(server);
}

bool startListening(InterfaceRelay::State& state) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    socklen_t length = sizeof(address);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || ::listen(fd, 32) != 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::close(fd);
        return false;
    }
    state.listenFd = fd;
    state.port = ntohs(address.sin_port);
    state.username = randomHex(8);
    state.password = randomHex(16);
    if (state.username.empty() || state.password.empty()) {
        ::close(fd);
        state.listenFd = -1;
        return false;
    }
    state.expectedAuth = "basic " + base64(state.username + ":" + state.password);
    return true;
}

void acceptLoop(const std::shared_ptr<InterfaceRelay::State>& state) {
    while (!state->stopping.load()) {
        pollfd wait{state->listenFd, POLLIN, 0};
        if (::poll(&wait, 1, kPollSliceMs) <= 0) continue;
        const int client = ::accept4(state->listenFd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;
        {
            std::scoped_lock lock(state->connectionsMutex);
            if (state->live >= kMaxConnections) {
                replyStatus(client, "503 Service Unavailable");
                ::close(client);
                continue;
            }
            ++state->live;
            state->clients.insert(client);
        }
        try {
            std::thread([state, client] {
                handleClient(client, state);
                std::scoped_lock lock(state->connectionsMutex);
                state->clients.erase(client);
                ::close(client);
                --state->live;
                state->connectionsDone.notify_all();
            }).detach();
        } catch (...) {
            std::scoped_lock lock(state->connectionsMutex);
            state->clients.erase(client);
            ::close(client);
            --state->live;
        }
    }
}

void stopRelay(const std::shared_ptr<InterfaceRelay::State>& state) {
    state->stopping = true;
    if (state->acceptThread.joinable()) state->acceptThread.join();
    if (state->listenFd >= 0) ::close(state->listenFd);
    state->listenFd = -1;
    std::unique_lock lock(state->connectionsMutex);
    for (const int client : state->clients) ::shutdown(client, SHUT_RDWR);
    // A connection still waiting on a name lookup ends on its own; it holds its own
    // reference to the state, so it's safe to stop waiting for it.
    state->connectionsDone.wait_for(lock, std::chrono::seconds(3), [&] { return state->live == 0; });
}

}

InterfaceRelay::InterfaceRelay() = default;

InterfaceRelay::~InterfaceRelay() {
    if (state_) stopRelay(state_);
}

bool InterfaceRelay::setInterface(const std::string& interfaceName) {
    if (interfaceName.empty()) {
        if (state_) {
            stopRelay(state_);
            state_.reset();
            ORBIT_LOG_INFO("relay", "Stopped");
        }
        return true;
    }
    if (!state_) {
        auto state = std::make_shared<State>();
        if (!startListening(*state)) {
            ORBIT_LOG_ERROR("relay", "Couldn't start the relay for the locked interface");
            return false;
        }
        state->acceptThread = std::thread([state] { acceptLoop(state); });
        state_ = std::move(state);
        ORBIT_LOG_INFO("relay", "Relaying video and tracker connections on 127.0.0.1:" + std::to_string(state_->port));
    }
    std::scoped_lock lock(state_->mutex);
    state_->interfaceName = interfaceName;
    return true;
}

ProxySettings InterfaceRelay::proxy() const {
    ProxySettings proxy;
    if (!state_) return proxy;
    proxy.type = ProxySettings::Type::Http;
    proxy.host = "127.0.0.1";
    proxy.port = state_->port;
    proxy.username = state_->username;
    proxy.password = state_->password;
    return proxy;
}

#else

struct InterfaceRelay::State {};
InterfaceRelay::InterfaceRelay() = default;
InterfaceRelay::~InterfaceRelay() = default;
bool InterfaceRelay::setInterface(const std::string& interfaceName) { return interfaceName.empty(); }
ProxySettings InterfaceRelay::proxy() const { return {}; }

#endif

}
