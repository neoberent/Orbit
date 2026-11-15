#include "core/HttpDownloader.hpp"
#include "core/InterfaceDns.hpp"

#include "utils/FileUtils.hpp"
#include "utils/Logger.hpp"
#include "utils/StringUtils.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <limits>
#include <mutex>
#include <system_error>

#if defined(_WIN32)
#include <share.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace orbit::core {

namespace {

constexpr std::uint64_t kUnknownSize = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kMinSegmentSize = 1024ull * 1024ull;
// How far ahead of a connection's write position a rebalancing split must land; twice
// this is the smallest remaining range worth splitting. Small, so that near the end idle
// connections still take over part of the slowest one's work instead of sitting idle
// while it finishes alone (each connection stops exactly at its moved end, so a short
// lead is safe).
constexpr std::uint64_t kRebalanceChunkBytes = 2ull * 1024ull * 1024ull;
// A split is abandoned unless the donor is still at least this far from it once the new
// end is in place; one write from curl is at most 16 KB, so it can never cross it.
constexpr std::uint64_t kSplitSafetyBytes = 1ull * 1024ull * 1024ull;

// Transient failures (throttling, 5xx, dropped connections) are retried per segment with
// exponential backoff before the whole download is marked as failed.
constexpr int kMaxSegmentRetries = 6;

bool isTransientFailure(CURLcode res, long httpCode) {
    if (res == CURLE_HTTP_RETURNED_ERROR) {
        return httpCode == 408 || httpCode == 429 || httpCode == 500 || httpCode == 502 ||
               httpCode == 503 || httpCode == 504;
    }
    switch (res) {
        case CURLE_COULDNT_CONNECT:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_PARTIAL_FILE:
        case CURLE_GOT_NOTHING:
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_HTTP2:
        case CURLE_HTTP2_STREAM:
            return true;
        default:
            return false;
    }
}

struct WriteContext {
    FILE* file{nullptr};
    std::atomic<std::uint64_t>* counter{nullptr};
    std::atomic<bool>* paused{nullptr};
    std::atomic<bool>* cancelled{nullptr};
    orbit::utils::SpeedLimiter* globalLimiter{nullptr};
    orbit::utils::SpeedLimiter* jobLimiter{nullptr};
    // Stall detection: when data last arrived (or a speed cap last let this connection go
    // on), and how long without data counts as a dead connection.
    std::chrono::steady_clock::duration stallTimeout{};
    std::chrono::steady_clock::time_point lastData{};
    bool stalled{false};

    // Set for a range request: the answer must be a 206 of at most rangeBytesLeft bytes.
    // Anything else is written nowhere and flags the job through rangeIgnored.
    CURL* curl{nullptr};
    std::uint64_t rangeBytesLeft{0};
    bool statusChecked{false};
    std::atomic<bool>* rangeIgnored{nullptr};

    // For a segment's request: where the next byte goes, and the segment's end, which
    // rebalancing may pull in while the request runs. Data past it is left for the helper
    // segment that took over, and the request ends there (reachedEnd).
    std::atomic<std::uint64_t>* segmentEnd{nullptr};
    std::uint64_t position{0};
    bool reachedEnd{false};

    // Called before each request.
    void arm() {
        lastData = std::chrono::steady_clock::now();
        stalled = false;
        statusChecked = false;
        reachedEnd = false;
    }
};

size_t curlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<WriteContext*>(userdata);
    const size_t total = size * nmemb;
    if (total == 0) return 0;
    if (ctx->curl) {
        if (!ctx->statusChecked) {
            ctx->statusChecked = true;
            long httpCode = 0;
            curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &httpCode);
            if (httpCode != 206) {
                ctx->rangeIgnored->store(true);
                return 0;
            }
        }
        if (total > ctx->rangeBytesLeft) {
            ctx->rangeIgnored->store(true);
            return 0;
        }
        ctx->rangeBytesLeft -= total;
    }
    size_t accepted = total;
    if (ctx->segmentEnd) {
        const std::uint64_t end = ctx->segmentEnd->load();
        if (ctx->position > end) {
            ctx->reachedEnd = true;
            return 0;
        }
        const std::uint64_t allowed = end - ctx->position + 1;
        if (accepted > allowed) {
            accepted = static_cast<size_t>(allowed);
            ctx->reachedEnd = true;
        }
    }
    const size_t written = std::fwrite(ptr, 1, accepted, ctx->file);
    ctx->position += written;
    ctx->counter->fetch_add(written, std::memory_order_relaxed);
    if (ctx->reachedEnd) return 0;  // Ends the request; the rest belongs to another segment.
    if (ctx->jobLimiter) ctx->jobLimiter->consume(written, ctx->paused, ctx->cancelled);
    if (ctx->globalLimiter) ctx->globalLimiter->consume(written, ctx->paused, ctx->cancelled);
    // Stamped after the speed caps let go: time spent held back by a cap is never a stall.
    ctx->lastData = std::chrono::steady_clock::now();
    return written;
}

// curl calls this at least once a second, even while no data is arriving.
int curlProgressCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<WriteContext*>(clientp);
    if (ctx->paused->load(std::memory_order_relaxed) || ctx->cancelled->load(std::memory_order_relaxed)) {
        return 1;
    }
    // Another segment found the server ignoring ranges: this one's data is suspect too.
    if (ctx->rangeIgnored && ctx->rangeIgnored->load(std::memory_order_relaxed)) return 1;
    // A server that keeps the connection open but stops sending (or never starts) would
    // otherwise hold this segment forever.
    if (ctx->stallTimeout.count() > 0 && std::chrono::steady_clock::now() - ctx->lastData > ctx->stallTimeout) {
        ctx->stalled = true;
        return 1;
    }
    return 0;
}

// A request this downloader aborted because it stalled is retried like a dropped
// connection; any other abort is a pause or cancel.
// Set when curl was about to ask the system resolver while downloads are locked to an
// interface; the lookup is refused so the name never leaves over the normal connection.
struct ResolverGuard {
    bool blocked{false};
};

int refuseSystemResolver(void*, void*, void* clientp) {
    static_cast<ResolverGuard*>(clientp)->blocked = true;
    return 1;
}

CURLcode stallAsTimeout(CURLcode res, const WriteContext& ctx) {
    return res == CURLE_ABORTED_BY_CALLBACK && ctx.stalled ? CURLE_OPERATION_TIMEDOUT : res;
}

// Lets a probe give up as soon as any of its flags is set: the downloader shutting down,
// or the job it probes for being paused or cancelled. Unused flags are null.
struct AbortFlags {
    const std::atomic<bool>* flags[3]{};
};

int abortOnFlagCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    for (const std::atomic<bool>* flag : static_cast<const AbortFlags*>(clientp)->flags) {
        if (flag && flag->load(std::memory_order_relaxed)) return 1;
    }
    return 0;
}

struct ProbeContext {
    bool supportsRanges{false};
    std::string contentDispositionFilename;
    std::uint64_t contentRangeTotal{0};
    // A digest of the whole file from Repr-Digest / Digest / x-goog-hash, as
    // "sha512:<hex>", "sha256:<hex>" or "md5:<hex>" (the strongest one offered).
    std::string checksum;
    int checksumStrength{0};
};

std::string base64ToHex(std::string_view text) {
    static const std::string kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string bytes;
    unsigned buffer = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=') break;
        if (c == '-') c = '+';
        if (c == '_') c = '/';
        const auto value = kAlphabet.find(c);
        if (value == std::string::npos) return {};
        buffer = (buffer << 6) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    static const char* const kHex = "0123456789abcdef";
    std::string hex;
    for (unsigned char b : bytes) {
        hex.push_back(kHex[b >> 4]);
        hex.push_back(kHex[b & 0x0F]);
    }
    return hex;
}

// "sha-256=:BASE64:, sha-512=:...:" (RFC 9530) or "SHA-256=BASE64" (RFC 3230), or Google's
// "crc32c=..., md5=BASE64". Keeps the strongest digest it can read.
void readDigestHeader(std::string_view value, ProbeContext& ctx) {
    size_t start = 0;
    while (start < value.size()) {
        size_t end = value.find(',', start);
        if (end == std::string_view::npos) end = value.size();
        std::string item(utils::trim(value.substr(start, end - start)));
        start = end + 1;
        const auto eq = item.find('=');
        if (eq == std::string::npos) continue;
        std::string algorithm = item.substr(0, eq);
        std::transform(algorithm.begin(), algorithm.end(), algorithm.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string encoded = item.substr(eq + 1);
        encoded.erase(std::remove(encoded.begin(), encoded.end(), ':'), encoded.end());
        int strength = 0;
        std::string name;
        if (algorithm == "sha-512") { strength = 3; name = "sha512"; }
        else if (algorithm == "sha-256") { strength = 2; name = "sha256"; }
        else if (algorithm == "md5") { strength = 1; name = "md5"; }
        if (strength <= ctx.checksumStrength) continue;
        const std::string hex = base64ToHex(encoded);
        const size_t expected = strength == 3 ? 128 : strength == 2 ? 64 : 32;
        if (hex.size() != expected) continue;
        ctx.checksum = name + ":" + hex;
        ctx.checksumStrength = strength;
    }
}

std::string extractContentDispositionFilename(std::string_view line) {
    const auto starPos = line.find("filename*=");
    if (starPos != std::string_view::npos) {
        std::string_view rest = line.substr(starPos + std::string_view("filename*=").size());
        const auto sepPos = rest.find("''");
        if (sepPos != std::string_view::npos) {
            std::string_view encoded = rest.substr(sepPos + 2);
            const auto end = encoded.find_first_of(";\r\n");
            if (end != std::string_view::npos) encoded = encoded.substr(0, end);
            return utils::urlDecode(utils::trim(encoded));
        }
    }

    const auto pos = line.find("filename=");
    if (pos != std::string_view::npos) {
        std::string_view rest = line.substr(pos + std::string_view("filename=").size());
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
        if (!rest.empty() && rest.front() == '"') {
            rest.remove_prefix(1);
            const auto endQuote = rest.find('"');
            return std::string(endQuote != std::string_view::npos ? rest.substr(0, endQuote) : rest);
        }
        const auto end = rest.find_first_of(";\r\n");
        return std::string(utils::trim(end != std::string_view::npos ? rest.substr(0, end) : rest));
    }
    return {};
}

size_t probeHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* ctx = static_cast<ProbeContext*>(userdata);
    const size_t total = size * nitems;
    std::string_view line(buffer, total);
    if (utils::startsWith(line, "Accept-Ranges") || utils::startsWith(line, "accept-ranges")) {
        std::string lower;
        lower.reserve(line.size());
        for (char c : line) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (lower.find("bytes") != std::string::npos) {
            ctx->supportsRanges = true;
        }
    }
    if (utils::startsWith(line, "Content-Range") || utils::startsWith(line, "content-range")) {
        // "Content-Range: bytes 0-0/10737418240" - the part after '/' is the full size.
        const auto slash = line.rfind('/');
        if (slash != std::string_view::npos) {
            const std::string totalText(utils::trim(line.substr(slash + 1)));
            if (!totalText.empty() && totalText != "*") {
                ctx->contentRangeTotal = std::strtoull(totalText.c_str(), nullptr, 10);
            }
        }
    }
    if (utils::startsWith(line, "Content-Disposition") || utils::startsWith(line, "content-disposition")) {
        std::string name = extractContentDispositionFilename(line);
        if (!name.empty()) ctx->contentDispositionFilename = std::move(name);
    }
    {
        std::string lower(line.substr(0, std::min<size_t>(line.size(), 16)));
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // A redirect's headers describe something else: only the last response counts.
        if (utils::startsWith(lower, "http/")) {
            ctx->checksum.clear();
            ctx->checksumStrength = 0;
        }
        // Content-Digest is left out on purpose: it covers just this response's bytes,
        // which for a range answer is only part of the file.
        for (const char* header : {"repr-digest:", "digest:", "x-goog-hash:"}) {
            if (utils::startsWith(lower, header)) readDigestHeader(line.substr(std::strlen(header)), *ctx);
        }
    }
    return total;
}

size_t discardWriteCallback(char*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

std::string netscapeLine(const HttpCookie& cookie) {
    auto clean = [](const std::string& text) { return text.find_first_of("\t\r\n") == std::string::npos; };
    if (cookie.name.empty() || cookie.domain.empty() || !clean(cookie.name) || !clean(cookie.value) ||
        !clean(cookie.domain) || !clean(cookie.path)) {
        return {};
    }
    std::string domain = cookie.domain;
    if (cookie.includeSubdomains && domain.front() != '.') domain.insert(domain.begin(), '.');
    if (!cookie.includeSubdomains && domain.front() == '.') domain.erase(domain.begin());
    return domain + "\t" + (cookie.includeSubdomains ? "TRUE" : "FALSE") + "\t" +
           (cookie.path.empty() ? std::string("/") : cookie.path) + "\t" + (cookie.secure ? "TRUE" : "FALSE") +
           "\t" + std::to_string(std::max<std::int64_t>(0, cookie.expires)) + "\t" + cookie.name + "\t" +
           cookie.value;
}

void applyCommonSslOptions(CURL* curl) {
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
    // Requests are only ever meant to hit remote HTTP(S) servers. Without this, a
    // caller that fails to validate its input (e.g. a URL arriving via the orbit://
    // protocol handler) could hand curl a file://, ftp://, or other local/unexpected
    // scheme, and CURLOPT_FOLLOWLOCATION could likewise be redirected into one server-side.
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
}

}

HttpDownloader::HttpDownloader(ProgressReporter reporter) : reporter_(std::move(reporter)) {
    static std::once_flag initFlag;
    std::call_once(initFlag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

HttpDownloader::~HttpDownloader() {
    shuttingDown_ = true;
    {
        std::scoped_lock lock(jobsMutex_);
        for (auto& [id, job] : jobs_) job->paused = true;
    }
    // Every blocking step a supervisor takes (the probe, segment transfers, retry
    // back-off) gives up within a moment of `paused`/`shuttingDown_`, so this wait is
    // short; ending it early would free jobs that live threads are still using.
    std::unique_lock lock(supervisorsMutex_);
    supervisorsDone_.wait(lock, [this] { return liveSupervisors_ == 0; });
}

void HttpDownloader::launchSupervisor(DownloadId id) {
    {
        std::scoped_lock lock(supervisorsMutex_);
        ++liveSupervisors_;
    }
    try {
        std::thread([this, id] {
            runSupervisor(id);
            // Notified under the lock: once it's released this thread touches nothing of
            // `this`, so the destructor may go ahead and free it.
            std::scoped_lock lock(supervisorsMutex_);
            --liveSupervisors_;
            supervisorsDone_.notify_all();
        }).detach();
    } catch (...) {
        std::scoped_lock lock(supervisorsMutex_);
        --liveSupervisors_;
        throw;
    }
}

void HttpDownloader::start(DownloadId id, const DownloadRequest& request) {
    {
        std::scoped_lock lock(jobsMutex_);
        std::filesystem::path partialFile;
        if (auto it = jobs_.find(id); it != jobs_.end()) {
            if (it->second->running.load()) return;
            // A job that already ended (failed or cancelled) is being retried. Start it
            // over, but pick up the partial file and its resume metadata when a failure
            // left them behind (a cancel deletes both).
            if (!it->second->cancelStart.load()) partialFile = it->second->filePath;
            jobs_.erase(it);
        }
        auto job = std::make_unique<Job>();
        job->id = id;
        job->request = request;
        job->limiter.setLimitBytesPerSec(request.speedLimitBytesPerSec);
        if (!partialFile.empty() && std::filesystem::exists(partialFile)) {
            job->request.existingFilePath = partialFile.string();
        }
        if (shuttingDown_.load()) return;
        job->running = true;
        jobs_.emplace(id, std::move(job));
    }
    launchSupervisor(id);
}

void HttpDownloader::pause(DownloadId id) {
    std::scoped_lock lock(jobsMutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return;
    it->second->paused = true;
}

void HttpDownloader::resume(DownloadId id) {
    {
        std::scoped_lock lock(jobsMutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end() || shuttingDown_.load()) return;
        Job& job = *it->second;
        // Claimed under the lock, so two quick resumes can't start two supervisors.
        if (job.running.exchange(true)) return;
        job.paused = false;
        job.cancelStart = false;
    }
    launchSupervisor(id);
}

void HttpDownloader::cancel(DownloadId id) {
    Job* jobPtr = nullptr;
    {
        std::scoped_lock lock(jobsMutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return;
        jobPtr = it->second.get();
    }
    jobPtr->cancelStart = true;
    jobPtr->paused = false;

    if (!jobPtr->running.load()) {
        std::error_code ec;
        std::filesystem::remove(jobPtr->filePath, ec);
        std::filesystem::remove(jobPtr->metaPath, ec);
        reporter_.reportStateChanged(id, DownloadState::Cancelled);
    }
}

void HttpDownloader::forget(DownloadId id) {
    std::scoped_lock lock(jobsMutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end() || it->second->running.load()) return;
    jobs_.erase(it);
}

DownloadStats HttpDownloader::stats(DownloadId id) const {
    std::scoped_lock lock(jobsMutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return {};
    std::scoped_lock statsLock(it->second->statsMutex);
    return it->second->lastStats;
}

bool HttpDownloader::hasJob(DownloadId id) const {
    std::scoped_lock lock(jobsMutex_);
    return jobs_.contains(id);
}

void HttpDownloader::setGlobalSpeedLimit(std::uint64_t bytesPerSec) {
    globalLimiter_.setLimitBytesPerSec(bytesPerSec);
}

void HttpDownloader::setSpeedLimit(DownloadId id, std::uint64_t bytesPerSec) {
    std::scoped_lock lock(jobsMutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return;
    it->second->request.speedLimitBytesPerSec = bytesPerSec;
    it->second->limiter.setLimitBytesPerSec(bytesPerSec);
}

void HttpDownloader::setMaxConnections(DownloadId id, int connections) {
    connections = std::clamp(connections, 1, 32);
    std::scoped_lock lock(jobsMutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return;
    Job& job = *it->second;
    job.request.maxConnections = connections;
    if (job.supportsRanges) job.desiredConnections = std::clamp(connections, 1, 16);
}

void HttpDownloader::relocate(DownloadId id, const std::filesystem::path& newFilePath) {
    std::scoped_lock lock(jobsMutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end() || it->second->running.load()) return;
    Job& job = *it->second;
    job.filePath = newFilePath;
    job.metaPath = newFilePath;
    job.metaPath += ".orbitresume";
    job.resolvedFileName = newFilePath.filename().string();
    job.request.existingFilePath = newFilePath.string();
    job.request.destinationDir = newFilePath.parent_path().string();
}

void HttpDownloader::setProxy(const ProxySettings& proxy) {
    std::scoped_lock lock(proxyMutex_);
    proxy_ = proxy;
}

void HttpDownloader::setNetworkInterface(const std::string& interfaceName) {
    std::scoped_lock lock(proxyMutex_);
    networkInterface_ = interfaceName;
}

namespace {
// "host:port" of a URL (lower-case), or empty.
std::string hostKey(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    std::string rest = url.substr(scheme + 3);
    rest = rest.substr(0, rest.find_first_of("/?#"));
    if (const auto at = rest.rfind('@'); at != std::string::npos) rest = rest.substr(at + 1);
    std::transform(rest.begin(), rest.end(), rest.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return rest;
}
// How long a learned limit is trusted before Orbit tries the full count again.
constexpr auto kHostLimitMemory = std::chrono::minutes(30);
}

int HttpDownloader::connectionsFor(const std::string& source, int wanted) const {
    wanted = std::clamp(wanted, 1, 16);
    const std::string host = hostKey(source);
    std::scoped_lock lock(hostLimitsMutex_);
    const auto it = hostLimits_.find(host);
    if (host.empty() || it == hostLimits_.end()) return wanted;
    if (std::chrono::steady_clock::now() - it->second.second > kHostLimitMemory) return wanted;
    return std::min(wanted, it->second.first);
}

void HttpDownloader::rememberHostLimit(const std::string& source, int connections) {
    const std::string host = hostKey(source);
    if (host.empty()) return;
    std::scoped_lock lock(hostLimitsMutex_);
    auto& entry = hostLimits_[host];
    const bool fresh = entry.first == 0 || std::chrono::steady_clock::now() - entry.second > kHostLimitMemory;
    entry.first = fresh ? connections : std::min(entry.first, connections);
    entry.second = std::chrono::steady_clock::now();
}

HttpDownloader::Fetched HttpDownloader::fetchSmall(const DownloadRequest& request, std::size_t maxBytes) const {
    Fetched out;
    CURL* curl = curl_easy_init();
    if (!curl) {
        out.error = "Failed to initialize HTTP client";
        return out;
    }
    struct Sink {
        std::string* body;
        std::size_t max;
    } sink{&out.body, maxBytes};
    applyRequestOptions(curl, request);
    curl_easy_setopt(curl, CURLOPT_URL, request.source.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* data, size_t size, size_t count, void* userdata) -> size_t {
        auto* s = static_cast<Sink*>(userdata);
        if (s->body->size() + size * count > s->max) return 0;
        s->body->append(data, size * count);
        return size * count;
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    const auto result = static_cast<CURLcode>(perform(curl, request.source));
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status);
    curl_easy_cleanup(curl);
    out.ok = result == CURLE_OK && out.status >= 200 && out.status < 300;
    if (!out.ok) {
        out.error = result == CURLE_WRITE_ERROR ? "Too large" : result != CURLE_OK ? curl_easy_strerror(result)
                                                                                  : "HTTP " + std::to_string(out.status);
    }
    return out;
}

std::string HttpDownloader::fetchPublishedChecksum(const DownloadRequest& request, const std::string& fileName) const {
    const std::string& url = request.source;
    // Signed or generated links (anything with a query) don't have files beside them.
    if (url.find_first_of("?#") != std::string::npos || fileName.empty()) return {};
    static const char* const kReleaseTypes[] = {".iso", ".img", ".zip", ".7z", ".tar", ".gz", ".xz", ".zst", ".bz2",
                                                ".appimage", ".deb", ".rpm", ".exe", ".msi", ".dmg", ".pkg", ".apk"};
    std::string lowerName = fileName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool release = std::any_of(std::begin(kReleaseTypes), std::end(kReleaseTypes), [&](const char* ext) {
        return lowerName.size() > std::strlen(ext) && lowerName.compare(lowerName.size() - std::strlen(ext), std::string::npos, ext) == 0;
    });
    if (!release) return {};

    for (const char* suffix : {".sha256", ".sha512", ".sha256sum"}) {
        CURL* curl = curl_easy_init();
        if (!curl) return {};
        std::string body;
        const std::string sidecar = url + suffix;
        applyRequestOptions(curl, request);
        curl_easy_setopt(curl, CURLOPT_URL, sidecar.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* data, size_t size, size_t count, void* userdata) -> size_t {
            auto* out = static_cast<std::string*>(userdata);
            if (out->size() + size * count > 16 * 1024) return 0;  // a checksum file is tiny
            out->append(data, size * count);
            return size * count;
        });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        const auto result = static_cast<CURLcode>(perform(curl, sidecar));
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
        if (result != CURLE_OK || status != 200) continue;

        // "<hex>", "<hex>  name" or "<hex> *name" per line; a line naming another file
        // doesn't count.
        std::istringstream lines(body);
        std::string line;
        while (std::getline(lines, line)) {
            std::istringstream words(line);
            std::string hex;
            std::string name;
            words >> hex >> name;
            if (!name.empty() && name.front() == '*') name.erase(0, 1);
            if (!name.empty()) name = name.substr(name.find_last_of('/') + 1);
            if (!name.empty() && name != fileName) continue;
            if ((hex.size() == 64 || hex.size() == 128) &&
                std::all_of(hex.begin(), hex.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) {
                ORBIT_LOG_INFO("HttpDownloader", "Found a published checksum at " + sidecar);
                return (hex.size() == 64 ? "sha256:" : "sha512:") + hex;
            }
        }
    }
    return {};
}

int HttpDownloader::perform(void* handle, const std::string& url) const {
    CURL* curl = static_cast<CURL*>(handle);
    std::string interfaceName;
    std::string proxyUrl;
    {
        std::scoped_lock lock(proxyMutex_);
        interfaceName = networkInterface_;
        if (proxy_.enabled()) proxyUrl = proxy_.url();
    }
    if (interfaceName.empty()) return curl_easy_perform(curl);

    ResolverGuard guard;
    curl_easy_setopt(curl, CURLOPT_RESOLVER_START_FUNCTION, refuseSystemResolver);
    curl_easy_setopt(curl, CURLOPT_RESOLVER_START_DATA, &guard);

    // host -> ports curl may connect to it on.
    std::vector<std::pair<std::string, int>> targets{{InterfaceDns::hostOf(url), InterfaceDns::portOf(url)}};
    if (!proxyUrl.empty()) targets.emplace_back(InterfaceDns::hostOf(proxyUrl), InterfaceDns::portOf(proxyUrl));

    CURLcode result = CURLE_COULDNT_RESOLVE_HOST;
    for (int attempt = 0; attempt < 6; ++attempt) {
        curl_slist* entries = nullptr;
        bool resolved = true;
        for (const auto& [host, port] : targets) {
            // IP addresses go in too: curl runs them past its resolver as well, which
            // is refused here, so it has to find them already "looked up".
            if (host.empty()) continue;
            const std::vector<std::string> addresses = InterfaceDns::resolve(host, interfaceName);
            if (addresses.empty()) {
                resolved = false;
                break;
            }
            std::string list;
            for (const std::string& address : addresses) {
                if (!list.empty()) list += ',';
                list += address.find(':') != std::string::npos ? "[" + address + "]" : address;
            }
            // Redirects keep the host but may switch between http and https.
            const std::string name = host.find(':') != std::string::npos ? "[" + host + "]" : host;
            for (const int p : {port, 80, 443}) {
                entries = curl_slist_append(entries, (name + ":" + std::to_string(p) + ":" + list).c_str());
            }
        }
        if (!resolved) {
            curl_slist_free_all(entries);
            result = CURLE_COULDNT_RESOLVE_HOST;
            break;
        }
        curl_easy_setopt(curl, CURLOPT_RESOLVE, entries);
        guard.blocked = false;
        result = curl_easy_perform(curl);
        curl_easy_setopt(curl, CURLOPT_RESOLVE, nullptr);
        curl_slist_free_all(entries);
        if (!guard.blocked) break;

        // A redirect led to a host not looked up yet: look it up and go again.
        char* effective = nullptr;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
        const std::string next = effective ? effective : "";
        const std::string nextHost = InterfaceDns::hostOf(next);
        const bool known = std::any_of(targets.begin(), targets.end(), [&](const auto& t) { return t.first == nextHost; });
        if (nextHost.empty() || known) {
            result = CURLE_COULDNT_RESOLVE_HOST;
            break;
        }
        targets.emplace_back(nextHost, InterfaceDns::portOf(next));
        result = CURLE_COULDNT_RESOLVE_HOST;
    }
    curl_easy_setopt(curl, CURLOPT_RESOLVER_START_FUNCTION, nullptr);
    curl_easy_setopt(curl, CURLOPT_RESOLVER_START_DATA, nullptr);
    return result;
}

void HttpDownloader::setTypeFolders(std::map<std::string, std::string> folders) {
    std::scoped_lock lock(proxyMutex_);
    typeFolders_ = std::move(folders);
}

std::string HttpDownloader::fileTypeOf(const std::string& fileName) {
    static const std::map<std::string, std::string> kTypes = {
        {"mp4", "video"}, {"mkv", "video"}, {"webm", "video"}, {"avi", "video"}, {"mov", "video"}, {"wmv", "video"},
        {"flv", "video"}, {"m4v", "video"}, {"ts", "video"},
        {"mp3", "audio"}, {"m4a", "audio"}, {"flac", "audio"}, {"wav", "audio"}, {"ogg", "audio"}, {"opus", "audio"},
        {"aac", "audio"}, {"wma", "audio"},
        {"png", "image"}, {"jpg", "image"}, {"jpeg", "image"}, {"gif", "image"}, {"webp", "image"}, {"svg", "image"},
        {"bmp", "image"}, {"tiff", "image"}, {"heic", "image"}, {"avif", "image"},
        {"pdf", "document"}, {"doc", "document"}, {"docx", "document"}, {"txt", "document"}, {"md", "document"},
        {"epub", "document"}, {"xls", "document"}, {"xlsx", "document"}, {"ppt", "document"}, {"pptx", "document"},
        {"csv", "document"}, {"odt", "document"}, {"ods", "document"}, {"rtf", "document"},
        {"zip", "archive"}, {"rar", "archive"}, {"7z", "archive"}, {"tar", "archive"}, {"gz", "archive"}, {"tgz", "archive"},
        {"bz2", "archive"}, {"xz", "archive"}, {"zst", "archive"},
        {"exe", "program"}, {"msi", "program"}, {"deb", "program"}, {"rpm", "program"}, {"appimage", "program"},
        {"apk", "program"}, {"dmg", "program"}, {"pkg", "program"}, {"iso", "program"}, {"flatpakref", "program"},
    };
    const auto dot = fileName.rfind('.');
    if (dot == std::string::npos) return {};
    std::string ext = fileName.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto it = kTypes.find(ext);
    return it == kTypes.end() ? std::string() : it->second;
}

void HttpDownloader::setCheckpointInterval(std::chrono::milliseconds interval) {
    checkpointIntervalMs_ = std::max<long>(100, static_cast<long>(interval.count()));
}

void HttpDownloader::setStallTimeout(std::chrono::seconds timeout) {
    stallTimeoutSeconds_ = std::max<long>(1, static_cast<long>(timeout.count()));
}

std::string HttpDownloader::netscapeCookieLine(const HttpCookie& cookie) { return netscapeLine(cookie); }

void HttpDownloader::applyRequestOptions(void* handle, const DownloadRequest& request) const {
    CURL* curl = static_cast<CURL*>(handle);
    applyCommonSslOptions(curl);
    // Without the browser's own, Orbit names itself with a link, as sites like Wikimedia
    // require of programs (a bare name gets 429 Too Many Requests there).
    static const std::string kUserAgent = std::string("Orbit/") + ORBIT_VERSION + " (+https://github.com/neoberent/Orbit)";
    curl_easy_setopt(curl, CURLOPT_USERAGENT, request.userAgent.empty() ? kUserAgent.c_str() : request.userAgent.c_str());
    if (!request.referrer.empty()) curl_easy_setopt(curl, CURLOPT_REFERER, request.referrer.c_str());
    // Handing cookies to curl's cookie engine (rather than a raw Cookie header) keeps
    // each one scoped to its own domain, so a redirect to a CDN can't leak them.
    for (const HttpCookie& cookie : request.cookies) {
        const std::string line = netscapeLine(cookie);
        if (!line.empty()) curl_easy_setopt(curl, CURLOPT_COOKIELIST, line.c_str());
    }

    ProxySettings proxy;
    std::string networkInterface;
    {
        std::scoped_lock lock(proxyMutex_);
        proxy = proxy_;
        networkInterface = networkInterface_;
    }
    if (!networkInterface.empty()) {
        // "if!" binds to the interface by name and fails instead of falling back to
        // another route while it's down: the kill switch behaviour VPN users expect.
        const std::string binding = "if!" + networkInterface;
        curl_easy_setopt(curl, CURLOPT_INTERFACE, binding.c_str());
    }
    if (proxy.enabled()) {
        const std::string url = proxy.url();
        curl_easy_setopt(curl, CURLOPT_PROXY, url.c_str());
        if (!proxy.username.empty()) {
            curl_easy_setopt(curl, CURLOPT_PROXYUSERNAME, proxy.username.c_str());
            curl_easy_setopt(curl, CURLOPT_PROXYPASSWORD, proxy.password.c_str());
        }
    }
}

namespace {

bool samePath(const std::filesystem::path& a, const std::filesystem::path& b) {
#if defined(_WIN32)
    return _wcsicmp(a.lexically_normal().c_str(), b.lexically_normal().c_str()) == 0;
#else
    return a.lexically_normal() == b.lexically_normal();
#endif
}

}

std::filesystem::path HttpDownloader::resolveTargetPath(const Job& job, const std::filesystem::path& desired) const {
    namespace fs = std::filesystem;
    auto claimedByAnotherJob = [&](const fs::path& path) {
        for (const auto& [otherId, other] : jobs_) {
            if (otherId == job.id || other->completed.load() || other->cancelStart.load() || other->filePath.empty()) continue;
            if (samePath(other->filePath, path)) return true;
        }
        return false;
    };
    auto sidecarOf = [](fs::path path) {
        path += ".orbitresume";
        return path;
    };
    auto taken = [&](const fs::path& path) {
        std::error_code ec;
        return fs::exists(path, ec) || fs::exists(sidecarOf(path), ec) || claimedByAnotherJob(path);
    };

    if (!taken(desired)) return desired;

    // Another download's partial file (or a folder) is never replaced and never worth
    // asking about: this download simply gets a numbered name.
    std::error_code ec;
    const bool replaceable = fs::is_regular_file(desired, ec) && !fs::exists(sidecarOf(desired), ec) &&
                             !claimedByAnotherJob(desired);
    if (replaceable && job.request.fileConflict == FileConflictAction::Overwrite) return desired;
    if (replaceable && job.request.fileConflict == FileConflictAction::Ask) return {};

    const fs::path stem = desired.stem();
    const fs::path ext = desired.extension();
    for (int i = 1; i < 10000; ++i) {
        fs::path candidate = desired.parent_path() / (stem.string() + " (" + std::to_string(i) + ")" + ext.string());
        if (!taken(candidate)) return candidate;
    }
    return utils::uniquePath(desired);
}

RemoteProbe HttpDownloader::probe(const DownloadRequest& request) const {
    RemoteProbe result;
    result.ok = probeRemote(request, result.size, result.supportsRanges, result.fileName, result.error);
    if (result.ok && result.size > 0 && !result.supportsRanges) {
        // Some servers only reveal range support when actually asked for a range.
        CURL* curl = curl_easy_init();
        if (curl) {
            applyRequestOptions(curl, request);
            curl_easy_setopt(curl, CURLOPT_URL, request.source.c_str());
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
            curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardWriteCallback);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            AbortFlags abort{{&shuttingDown_}};
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abortOnFlagCallback);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &abort);
            long httpCode = 0;
            if (static_cast<CURLcode>(perform(curl, request.source)) == CURLE_OK) {
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
                result.supportsRanges = httpCode == 206;
            }
            curl_easy_cleanup(curl);
        }
    }
    return result;
}

bool HttpDownloader::rewriteResumeSource(const std::filesystem::path& metaPath, const std::string& newSource) {
    std::ifstream in(metaPath);
    if (!in) return false;
    std::vector<std::string> lines;
    std::string line;
    bool replaced = false;
    while (std::getline(in, line)) {
        if (line.rfind("source=", 0) == 0) {
            line = "source=" + newSource;
            replaced = true;
        }
        lines.push_back(line);
    }
    in.close();
    if (!replaced) lines.insert(lines.begin(), "source=" + newSource);

    auto tempPath = metaPath;
    tempPath += ".tmp";
    {
        std::ofstream out(tempPath, std::ios::trunc);
        if (!out) return false;
        for (const std::string& l : lines) out << l << "\n";
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tempPath, metaPath, ec);
    if (ec) {
        std::filesystem::remove(tempPath, ec);
        return false;
    }
    return true;
}

bool HttpDownloader::probeRemote(const DownloadRequest& request, std::uint64_t& outSize,
                                 bool& outSupportsRanges, std::string& outFileName,
                                 std::string& outError, const Job* job, std::string* outChecksum) const {
    const std::string& url = request.source;
    CURL* curl = curl_easy_init();
    if (!curl) {
        outError = "Failed to initialize HTTP client";
        return false;
    }

    ProbeContext ctx;
    outSize = 0;
    outSupportsRanges = false;

    applyRequestOptions(curl, request);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, probeHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    AbortFlags abort{{&shuttingDown_, job ? &job->paused : nullptr, job ? &job->cancelStart : nullptr}};
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abortOnFlagCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &abort);

    CURLcode res = static_cast<CURLcode>(perform(curl, url));
    long httpCode = 0;
    curl_off_t contentLength = -1;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &contentLength);

    bool ok = (res == CURLE_OK) && httpCode >= 200 && httpCode < 400 && contentLength > 0;
    if (ok) {
        outSize = static_cast<std::uint64_t>(contentLength);
    }
    outSupportsRanges = ctx.supportsRanges;

    if (!ok) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardWriteCallback);
        res = static_cast<CURLcode>(perform(curl, url));
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        if (res == CURLE_OK && (httpCode == 206 || httpCode == 200)) {
            // Servers that reject HEAD still report the full size here: in Content-Range
            // for a 206, or as the plain Content-Length when the range was ignored (200).
            if (httpCode == 206) {
                outSupportsRanges = true;
                outSize = ctx.contentRangeTotal;
            } else {
                curl_off_t fullLength = -1;
                curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &fullLength);
                if (fullLength > 0) outSize = static_cast<std::uint64_t>(fullLength);
            }
            ok = true;
        }
    }

    outFileName = !ctx.contentDispositionFilename.empty() ? ctx.contentDispositionFilename
                                                            : utils::fileNameFromUrl(url);

    if (!ok) {
        outError = (res != CURLE_OK) ? curl_easy_strerror(res) : ("HTTP " + std::to_string(httpCode));
    }
    if (ok && outChecksum) *outChecksum = ctx.checksum;

    curl_easy_cleanup(curl);
    return ok;
}

void HttpDownloader::runSupervisor(DownloadId id) {
    Job* jobPtr = nullptr;
    {
        std::scoped_lock lock(jobsMutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return;
        jobPtr = it->second.get();
    }
    Job& job = *jobPtr;
    job.running = true;

    // A paused job resumes from the segments it still holds in memory, which skips the
    // size check a restart makes against the sidecar below. Make it here too: if the file
    // on the server changed size, finishing the old segments would splice two files.
    if (!job.segments.empty() && job.totalBytes > 0) {
        reporter_.reportStateChanged(id, DownloadState::Connecting);
        std::uint64_t remoteSize = 0;
        bool supportsRanges = false;
        std::string fileName;
        std::string probeError;
        if (probeRemote(job.request, remoteSize, supportsRanges, fileName, probeError, &job) && remoteSize > 0 &&
            remoteSize != job.totalBytes) {
            ORBIT_LOG_WARN("HttpDownloader", "Size on the server changed (" + std::to_string(job.totalBytes) + " -> " +
                                                 std::to_string(remoteSize) + "); restarting " + job.filePath.string());
            job.segments.clear();
            std::error_code ec;
            std::filesystem::remove(job.metaPath, ec);
            job.request.existingFilePath = job.filePath.string();
        }
    }

    if (job.segments.empty()) {
        reporter_.reportStateChanged(id, DownloadState::Connecting);

        std::uint64_t remoteSize = 0;
        bool supportsRanges = false;
        std::string fileName;
        std::string probeError;
        std::string serverChecksum;
        const bool probeOk = probeRemote(job.request, remoteSize, supportsRanges, fileName, probeError, &job, &serverChecksum);
        if (probeOk && job.request.expectedChecksum.empty()) {
            if (serverChecksum.empty()) serverChecksum = fetchPublishedChecksum(job.request, fileName);
            if (!serverChecksum.empty()) reporter_.reportChecksumFound(id, serverChecksum);
        }

        if (!probeOk && !job.paused.load() && !job.cancelStart.load()) {
            ORBIT_LOG_WARN("HttpDownloader", "Probe failed for " + job.request.source + ": " + probeError);
        }

        // Paused, cancelled or shut down while the server was being asked: stop before any
        // file is created, so a later resume simply asks again.
        if (job.cancelStart.load()) {
            job.running = false;
            reporter_.reportStateChanged(id, DownloadState::Cancelled);
            return;
        }
        if (job.paused.load()) {
            job.running = false;
            reporter_.reportStateChanged(id, DownloadState::Paused);
            return;
        }

        utils::ensureDirectoryExists(job.request.destinationDir);

        std::filesystem::path conflictPath;
        {
            // Chosen under the jobs lock, so two downloads starting at the same moment with
            // the same name can't both claim it before either file exists on disk.
            std::scoped_lock lock(jobsMutex_);
            if (!job.request.existingFilePath.empty()) {
                job.filePath = job.request.existingFilePath;
            } else {
                std::string finalName = !job.request.suggestedName.empty() ? job.request.suggestedName
                                         : (fileName.empty() ? std::string("download") : fileName);
                finalName = utils::sanitizeFileName(finalName);
                if (job.request.sortByType) {
                    std::string folder;
                    {
                        std::scoped_lock folderLock(proxyMutex_);
                        const auto it = typeFolders_.find(fileTypeOf(finalName));
                        if (it != typeFolders_.end()) folder = it->second;
                    }
                    if (!folder.empty() && utils::ensureDirectoryExists(folder)) job.request.destinationDir = folder;
                }
                const std::filesystem::path desired = std::filesystem::path(job.request.destinationDir) / finalName;
                job.filePath = resolveTargetPath(job, desired);
                if (job.filePath.empty()) conflictPath = desired;
            }
        }
        if (!conflictPath.empty()) {
            ORBIT_LOG_INFO("HttpDownloader", "Waiting for a decision: " + conflictPath.string() + " already exists");
            job.running = false;
            // Lets the prompt show the incoming file's size next to the existing one.
            DownloadStats incoming;
            incoming.totalBytes = probeOk ? remoteSize : 0;
            reporter_.reportProgress(id, incoming);
            reporter_.reportFileConflict(id, conflictPath.string());
            return;
        }
        job.metaPath = job.filePath;
        job.metaPath += ".orbitresume";
        job.resolvedFileName = job.filePath.filename().string();
        reporter_.reportNameResolved(id, job.resolvedFileName, job.filePath.string());

        bool restoredFromMetadata = false;
        if (std::filesystem::exists(job.metaPath)) {
            restoredFromMetadata = loadResumeMetadata(job);
        }
        // The saved segments only fit the file they were downloaded from. If the server now
        // reports a different size (the file was replaced, or a refreshed link points at
        // something else), splicing the two would produce a corrupt file, so start over.
        if (restoredFromMetadata && probeOk && remoteSize > 0 && remoteSize != job.totalBytes) {
            ORBIT_LOG_WARN("HttpDownloader", "Size on the server changed (" + std::to_string(job.totalBytes) + " -> " +
                                                 std::to_string(remoteSize) + "); restarting " + job.filePath.string());
            job.segments.clear();
            std::error_code metaEc;
            std::filesystem::remove(job.metaPath, metaEc);
            restoredFromMetadata = false;
        }

        if (!restoredFromMetadata) {
            job.totalBytes = remoteSize;
            job.supportsRanges = probeOk && supportsRanges && remoteSize > 0 && !job.rangesUnreliable;

            // Better to say so now than to fail at 90% with a full disk.
            if (job.totalBytes > 0) {
                std::error_code spaceEc;
                const auto space = std::filesystem::space(job.filePath.parent_path(), spaceEc);
                constexpr std::uint64_t kHeadroom = 64ull * 1024 * 1024;
                if (!spaceEc && space.available < job.totalBytes + kHeadroom) {
                    job.running = false;
                    reporter_.reportError(id, "Not enough free space in " + job.filePath.parent_path().string() + ": needs " +
                                                  utils::formatBytes(job.totalBytes) + ", " +
                                                  utils::formatBytes(space.available) + " free");
                    reporter_.reportStateChanged(id, DownloadState::Error);
                    return;
                }
            }

            if (!utils::preallocateFile(job.filePath, job.totalBytes)) {
                job.running = false;
                reporter_.reportError(id, "Could not create destination file: " + job.filePath.string());
                reporter_.reportStateChanged(id, DownloadState::Error);
                return;
            }

            if (job.supportsRanges) {
                job.desiredConnections = connectionsFor(job.request.source, job.request.maxConnections);
                int segCount = job.desiredConnections;
                if (job.totalBytes / static_cast<std::uint64_t>(segCount) < kMinSegmentSize) {
                    segCount = static_cast<int>(std::max<std::uint64_t>(1, job.totalBytes / kMinSegmentSize));
                }
                segCount = std::clamp(segCount, 1, job.desiredConnections.load());

                const std::uint64_t chunk = job.totalBytes / static_cast<std::uint64_t>(segCount);
                std::uint64_t offset = 0;
                for (int i = 0; i < segCount; ++i) {
                    auto seg = std::make_unique<Segment>();
                    seg->startOffset = offset;
                    seg->endOffset = (i == segCount - 1) ? job.totalBytes - 1 : (offset + chunk - 1);
                    offset = seg->endOffset.load() + 1;
                    job.segments.push_back(std::move(seg));
                }
            } else {
                job.desiredConnections = 1;
                auto seg = std::make_unique<Segment>();
                seg->startOffset = 0;
                seg->endOffset = job.totalBytes > 0 ? job.totalBytes - 1 : kUnknownSize;
                job.segments.push_back(std::move(seg));
            }
        } else {
            // Resume path: segments/desiredConnections aren't in the .orbitresume
            // metadata, so re-derive the same target connection count the original
            // request asked for (rebalancing then behaves the same whether a download
            // is fresh or resumed).
            job.desiredConnections = job.supportsRanges ? connectionsFor(job.request.source, job.request.maxConnections) : 1;
        }

        job.startTime = std::chrono::steady_clock::now();
        job.lastSampleTime = job.startTime;
    }
    {
        // A resume measures its speed afresh, not across the time it was paused.
        std::scoped_lock lock(job.statsMutex);
        job.rateWindow.clear();
    }

    reporter_.reportStateChanged(id, DownloadState::Downloading);

    for (int i = 0; i < static_cast<int>(job.segments.size()); ++i) {
        Segment& seg = *job.segments[i];
        const std::uint64_t segSize =
            (seg.endOffset == kUnknownSize) ? kUnknownSize : (seg.endOffset - seg.startOffset + 1);
        if (segSize != kUnknownSize && seg.bytesDownloaded.load() >= segSize) {
            seg.success = true;
            seg.finished = true;
            continue;
        }
        seg.finished = false;
    }

    // Mirrors serve connections only if they offer the very same file: same size, ranges.
    if (!job.sourcesChecked) {
        job.sourcesChecked = true;
        job.sources = {job.request.source};
        if (job.supportsRanges && job.totalBytes > 0) {
            for (const std::string& mirror : job.request.mirrors) {
                if (job.sources.size() >= 5 || job.paused.load() || job.cancelStart.load()) break;
                if (mirror.empty() || mirror == job.request.source) continue;
                DownloadRequest probeRequest = job.request;
                probeRequest.source = mirror;
                std::uint64_t mirrorSize = 0;
                bool mirrorRanges = false;
                std::string ignoredName;
                std::string mirrorError;
                if (probeRemote(probeRequest, mirrorSize, mirrorRanges, ignoredName, mirrorError, &job) &&
                    mirrorSize == job.totalBytes && mirrorRanges) {
                    job.sources.push_back(mirror);
                } else {
                    ORBIT_LOG_WARN("HttpDownloader", "Not using mirror " + mirror + ": " +
                                                         (mirrorError.empty() ? "a different file or no byte ranges" : mirrorError));
                }
            }
            if (job.sources.size() > 1) {
                ORBIT_LOG_INFO("HttpDownloader", "Download " + std::to_string(id) + " uses " + std::to_string(job.sources.size()) + " sources");
            }
        }
    }

    // A resumed download can carry more unfinished segments than the configured
    // connection count (rebalancing helpers are saved too), so only run up to
    // desiredConnections at once and start the rest as earlier ones complete.
    auto startPendingSegments = [&] {
        if (job.paused.load() || job.cancelStart.load() || job.rangeIgnored.load()) return;
        int running = 0;
        for (const auto& s : job.segments) {
            if (s->thread.joinable() && !s->finished.load()) ++running;
        }
        for (int i = 0; i < static_cast<int>(job.segments.size()) && running < std::max(1, job.desiredConnections.load()); ++i) {
            Segment& seg = *job.segments[i];
            if (seg.finished.load() || seg.thread.joinable()) continue;
            if (seg.url.empty() && !job.sources.empty()) seg.url = job.sources[static_cast<size_t>(i) % job.sources.size()];
            seg.thread = std::thread(&HttpDownloader::downloadSegment, this, std::ref(job), std::ref(seg), i);
            ++running;
        }
    };
    startPendingSegments();

    auto lastCheckpoint = std::chrono::steady_clock::now();
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        sampleAndReportProgress(job);
        startPendingSegments();
        rebalanceSegments(job);
        // A regular checkpoint, so a crash or power cut costs seconds, not the download.
        if (job.supportsRanges && job.totalBytes > 0 &&
            std::chrono::steady_clock::now() - lastCheckpoint > std::chrono::milliseconds(checkpointIntervalMs_.load())) {
            saveResumeMetadata(job, true);
            lastCheckpoint = std::chrono::steady_clock::now();
        }

        const bool allFinished = std::all_of(job.segments.begin(), job.segments.end(),
                                              [](const auto& s) { return s->finished.load(); });
        if (allFinished || job.paused.load() || job.cancelStart.load() || job.rangeIgnored.load()) break;
    }

    for (auto& seg : job.segments) {
        if (seg->thread.joinable()) seg->thread.join();
    }

    if (job.cancelStart.load()) {
        std::error_code ec;
        std::filesystem::remove(job.filePath, ec);
        std::filesystem::remove(job.metaPath, ec);
        // running must be cleared before the notification fires: a listener reacting
        // synchronously to the state change (e.g. calling DownloadManager::remove(), which
        // calls forget()) must see this job as no longer running, or forget() will silently
        // no-op and leak it.
        job.running = false;
        reporter_.reportStateChanged(id, DownloadState::Cancelled);
        return;
    }

    if (job.paused.load()) {
        saveResumeMetadata(job);
        sampleAndReportProgress(job);
        job.running = false;
        reporter_.reportStateChanged(id, DownloadState::Paused);
        return;
    }

    if (job.rangeIgnored.load()) {
        // The server advertised ranges but didn't honour one. Nothing it sent was written,
        // but the file can't be finished in parts, so fetch it again over one connection.
        ORBIT_LOG_WARN("HttpDownloader", "The server ignored a byte range; downloading in one piece: " +
                                             job.request.source);
        job.rangeIgnored = false;
        job.rangesUnreliable = true;
        job.segmentError = false;
        {
            std::scoped_lock lock(job.statsMutex);
            job.lastErrorMessage.clear();
        }
        job.segments.clear();
        std::error_code ec;
        std::filesystem::remove(job.metaPath, ec);
        job.request.existingFilePath = job.filePath.string();
        runSupervisor(id);  // rangesUnreliable makes this a single-piece run, so it can't recurse again
        return;
    }

    const bool allSucceeded = std::all_of(job.segments.begin(), job.segments.end(),
                                           [](const auto& s) { return s->success.load(); });

    if (!allSucceeded || job.segmentError.load()) {
        std::string message;
        {
            std::scoped_lock lock(job.statsMutex);
            message = job.lastErrorMessage;
        }
        std::uint64_t receivedSoFar = 0;
        for (const auto& seg : job.segments) receivedSoFar += seg->bytesDownloaded.load();
        if (receivedSoFar == 0) {
            // Nothing arrived: there's nothing to resume, so don't leave an empty (or
            // space-reserving) file behind that a retry would have to name around.
            std::error_code removeEc;
            std::filesystem::remove(job.filePath, removeEc);
            std::filesystem::remove(job.metaPath, removeEc);
        } else {
            saveResumeMetadata(job);
        }
        job.running = false;
        reporter_.reportError(id, message.empty() ? "Download failed" : message);
        reporter_.reportStateChanged(id, DownloadState::Error);
        return;
    }

    std::uint64_t received = 0;
    for (const auto& seg : job.segments) received += seg->bytesDownloaded.load();
    if (job.totalBytes > 0 && received < job.totalBytes) {
        saveResumeMetadata(job);
        job.running = false;
        reporter_.reportError(id, "Connection closed early (" + std::to_string(received) + " of " +
                                      std::to_string(job.totalBytes) + " bytes received)");
        reporter_.reportStateChanged(id, DownloadState::Error);
        return;
    }

    sampleAndReportProgress(job);
    std::error_code ec;
    std::filesystem::remove(job.metaPath, ec);
    job.completed = true;
    job.running = false;
    reporter_.reportStateChanged(id, DownloadState::Completed);
}

void HttpDownloader::downloadSegment(Job& job, Segment& segment, int ) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        job.segmentError = true;
        {
            std::scoped_lock lock(job.statsMutex);
            job.lastErrorMessage = "Failed to initialize HTTP client";
        }
        segment.finished = true;
        return;
    }

    FILE* fp = nullptr;
    int openErr = 0;
    for (int attempt = 0; attempt < 8 && !fp; ++attempt) {
        if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(150));
#if defined(_WIN32)
        fp = _wfsopen(job.filePath.c_str(), L"r+b", _SH_DENYNO);
        openErr = errno;
#else
        fp = std::fopen(job.filePath.c_str(), "r+b");
        openErr = errno;
#endif
    }
    if (!fp) {
        job.segmentError = true;
        {
            std::scoped_lock lock(job.statsMutex);
            job.lastErrorMessage = "Could not open destination file for writing (" + job.filePath.string() +
                                    "): " + std::strerror(openErr);
        }
        curl_easy_cleanup(curl);
        segment.finished = true;
        return;
    }

    applyRequestOptions(curl, job.request);
    std::string url = segment.url.empty() ? job.request.source : segment.url;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    // Without this an HTTP error page (e.g. a 429 "slow down" response) is written into
    // the file as if it were the payload, and the download ends up "Completed".
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlProgressCallback);

    ++job.activeSegments;

    // Both speed caps are enforced in curlWriteCallback: the per-download cap by
    // job.limiter (shared by this job's segments) and the global cap by globalLimiter_
    // (shared by every segment of every job).

    // A segment whose range is unbounded (unknown total size, or the server doesn't
    // support ranges at all) can't be chunked or rebalanced - fetch it in one shot,
    // exactly as before.
    const bool ranged = job.supportsRanges && segment.endOffset.load() != kUnknownSize;

    CURLcode res = CURLE_OK;
    bool sawRealError = false;
    int retries = 0;

    // Returns true when the failed request should be attempted again after a backoff.
    auto shouldRetry = [&](CURLcode failure) {
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        if (!isTransientFailure(failure, httpCode) || retries >= kMaxSegmentRetries) return false;
        ++retries;
        const bool tooMany = httpCode == 429 || httpCode == 503;
        if (tooMany) {
            // The server is limiting parallel connections: lower this job's target. The
            // request gate then keeps the job at what the server accepts, and rebalancing
            // stops adding helpers.
            int current = job.desiredConnections.load();
            while (current > 1 && !job.desiredConnections.compare_exchange_weak(current, current - 1)) {
            }
            rememberHostLimit(job.request.source, std::max(1, current - 1));
        }
        // Turned away for being one connection too many: the gate already makes this
        // segment wait for a free slot, so a short pause is enough. Real failures back off
        // exponentially.
        const auto backoff = tooMany && job.desiredConnections.load() > 1
                                 ? std::chrono::milliseconds(250 * retries)
                                 : std::chrono::milliseconds(std::min(30000, 1000 << (retries - 1)));
        const auto until = std::chrono::steady_clock::now() + backoff;
        while (std::chrono::steady_clock::now() < until) {
            if (job.paused.load() || job.cancelStart.load()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return !job.paused.load() && !job.cancelStart.load();
    };

    // Takes one of the job's request slots, waiting while the server's connection limit
    // is in use. False when the job is paused or cancelled meanwhile.
    auto acquireSlot = [&] {
        while (true) {
            if (job.paused.load() || job.cancelStart.load()) return false;
            int inFlight = job.requestsInFlight.load();
            if (inFlight < std::max(1, job.desiredConnections.load()) &&
                job.requestsInFlight.compare_exchange_weak(inFlight, inFlight + 1)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    };
    auto performGated = [&](WriteContext& ctx) {
        if (!acquireSlot()) return CURLE_ABORTED_BY_CALLBACK;
        ctx.arm();
        const CURLcode result = stallAsTimeout(static_cast<CURLcode>(perform(curl, url)), ctx);
        --job.requestsInFlight;
        return result;
    };

    if (!ranged) {
        // Without range support the server always sends the body from byte 0, so a
        // "resume" must start over - seeking past the saved bytes would write the start
        // of the file at the wrong offset and silently corrupt it.
        segment.bytesDownloaded = 0;
        const std::uint64_t resumeOffset = segment.startOffset;
#if defined(_WIN32)
        _fseeki64(fp, static_cast<long long>(resumeOffset), SEEK_SET);
#else
        std::fseek(fp, static_cast<long>(resumeOffset), SEEK_SET);
#endif
        WriteContext ctx{fp, &segment.bytesDownloaded, &job.paused, &job.cancelStart, &globalLimiter_, &job.limiter,
                         std::chrono::seconds(stallTimeoutSeconds_.load())};
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
        while (true) {
            res = performGated(ctx);
            if (res == CURLE_OK || res == CURLE_ABORTED_BY_CALLBACK) break;
            if (!shouldRetry(res)) break;
            segment.bytesDownloaded = 0;
#if defined(_WIN32)
            _fseeki64(fp, static_cast<long long>(segment.startOffset), SEEK_SET);
#else
            std::fseek(fp, static_cast<long>(segment.startOffset), SEEK_SET);
#endif
        }
        if (res == CURLE_OK) segment.success = true;
        else if (res != CURLE_ABORTED_BY_CALLBACK) sawRealError = true;
    } else {
        // One request for the segment's whole remaining range. Servers that limit how many
        // requests a client sends (speed-test and CDN hosts answer 429) see one request
        // per connection instead of one every few megabytes. When rebalancing pulls the
        // segment's end in, the write callback stops at the new end and the request ends;
        // a dropped connection simply asks again from where it got to.
        while (true) {
            const std::uint64_t currentEnd = segment.endOffset.load();
            const std::uint64_t resumeOffset = segment.startOffset + segment.bytesDownloaded.load();
            if (resumeOffset > currentEnd) break;  // shrunk out from under us - nothing left for this segment

            const std::uint64_t chunkEnd = currentEnd;

#if defined(_WIN32)
            _fseeki64(fp, static_cast<long long>(resumeOffset), SEEK_SET);
#else
            std::fseek(fp, static_cast<long>(resumeOffset), SEEK_SET);
#endif
            WriteContext ctx{fp, &segment.bytesDownloaded, &job.paused, &job.cancelStart, &globalLimiter_, &job.limiter,
                         std::chrono::seconds(stallTimeoutSeconds_.load())};
            ctx.curl = curl;
            ctx.rangeBytesLeft = chunkEnd - resumeOffset + 1;
            ctx.rangeIgnored = &job.rangeIgnored;
            ctx.segmentEnd = &segment.endOffset;
            ctx.position = resumeOffset;
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

            const std::string rangeHeader = std::to_string(resumeOffset) + "-" + std::to_string(chunkEnd);
            curl_easy_setopt(curl, CURLOPT_RANGE, rangeHeader.c_str());

            res = performGated(ctx);
            if (job.rangeIgnored.load()) break;  // the supervisor starts over in one piece
            // Stopped at an end that rebalancing moved: this segment's part is complete
            // (or, if the split was taken back, the loop asks for the rest).
            if (ctx.reachedEnd) res = CURLE_OK;
            if (res != CURLE_OK) {
                if (res == CURLE_ABORTED_BY_CALLBACK) break;
                if (shouldRetry(res)) continue;
                if (url != job.request.source) {
                    // The mirror gave up on this part: the main link finishes it.
                    ORBIT_LOG_WARN("HttpDownloader", "Mirror failed (" + std::string(curl_easy_strerror(res)) + "); using the main link: " + url);
                    url = job.request.source;
                    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                    retries = 0;
                    continue;
                }
                sawRealError = true;
                break;
            }
            retries = 0;
        }
        if (res == CURLE_OK) segment.success = true;
    }

    std::fclose(fp);

    if (sawRealError) {
        job.segmentError = true;
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        std::scoped_lock lock(job.statsMutex);
        if (res == CURLE_HTTP_RETURNED_ERROR && httpCode >= 400) {
            job.lastErrorMessage = "Server responded with HTTP " + std::to_string(httpCode) +
                                   (httpCode == 429 ? " (too many requests, try again later)" : "");
        } else {
            job.lastErrorMessage = curl_easy_strerror(res);
        }
    }

    curl_easy_cleanup(curl);
    --job.activeSegments;
    segment.finished = true;
}

void HttpDownloader::rebalanceSegments(Job& job) {
    if (!job.supportsRanges || job.paused.load() || job.cancelStart.load()) return;

    int activeCount = 0;
    for (auto& seg : job.segments) {
        if (!seg->finished.load()) ++activeCount;
    }
    if (activeCount >= job.desiredConnections) return;

    Segment* donor = nullptr;
    std::uint64_t donorRemaining = 0;
    for (auto& seg : job.segments) {
        if (seg->finished.load()) continue;
        const std::uint64_t end = seg->endOffset.load();
        if (end == kUnknownSize) continue;
        const std::uint64_t pos = seg->startOffset + seg->bytesDownloaded.load();
        if (pos > end) continue;
        const std::uint64_t remaining = end - pos + 1;
        if (remaining > donorRemaining) {
            donorRemaining = remaining;
            donor = seg.get();
        }
    }
    if (!donor || donorRemaining < 2 * kRebalanceChunkBytes) return;

    const std::uint64_t pos = donor->startOffset + donor->bytesDownloaded.load();
    const std::uint64_t oldEnd = donor->endOffset.load();
    // Split well ahead of where the donor is writing, so its request (which stops at the
    // new end) and the new helper never write the same bytes.
    const std::uint64_t splitPoint = pos + kRebalanceChunkBytes + (donorRemaining - kRebalanceChunkBytes) / 2;
    if (splitPoint >= oldEnd) return;

    donor->endOffset.store(splitPoint - 1);
    // The donor kept writing while this was worked out. If it got close to the split,
    // take the split back: nothing past the old position has been handed to anyone yet.
    const std::uint64_t posAfter = donor->startOffset + donor->bytesDownloaded.load();
    if (posAfter + kSplitSafetyBytes > splitPoint) {
        donor->endOffset.store(oldEnd);
        return;
    }

    auto helper = std::make_unique<Segment>();
    helper->startOffset = splitPoint;
    helper->endOffset = oldEnd;
    if (!job.sources.empty()) helper->url = job.sources[job.segments.size() % job.sources.size()];
    Segment* helperPtr = helper.get();
    job.segments.push_back(std::move(helper));
    helperPtr->thread = std::thread(&HttpDownloader::downloadSegment, this, std::ref(job), std::ref(*helperPtr), -1);
}

void HttpDownloader::sampleAndReportProgress(Job& job) {
    std::uint64_t downloaded = 0;
    int active = 0;
    for (auto& seg : job.segments) {
        downloaded += seg->bytesDownloaded.load();
        if (!seg->finished.load()) ++active;
    }

    const auto now = std::chrono::steady_clock::now();
    double rate;
    {
        std::scoped_lock lock(job.statsMutex);
        // Speed over the last ~2 s (or since the start, if sooner).
        constexpr auto kWindow = std::chrono::milliseconds(2000);
        auto& window = job.rateWindow;
        if (!window.empty() && downloaded < window.back().second) window.clear();  // restarted from 0
        window.emplace_back(now, downloaded);
        while (window.size() > 2 && now - window[1].first >= kWindow) window.pop_front();
        const double elapsed = std::chrono::duration<double>(now - window.front().first).count();
        if (elapsed > 0.05) {
            rate = static_cast<double>(downloaded - window.front().second) / elapsed;
        } else {
            rate = job.lastStats.downloadRateBytesPerSec;
        }
        job.bytesAtLastSample = downloaded;
        job.lastSampleTime = now;
    }

    DownloadStats stats;
    stats.bytesDownloaded = downloaded;
    stats.totalBytes = job.totalBytes;
    stats.downloadRateBytesPerSec = rate;
    stats.activeConnections = std::min(active, job.activeSegments.load());
    if (job.segments.size() > 1 && job.totalBytes > 0) {
        // Rebalancing can split segments many times over; past this many the bar would
        // be noise, so it falls back to plain progress.
        constexpr size_t kMaxReportedSegments = 64;
        if (job.segments.size() <= kMaxReportedSegments) {
            stats.segments.reserve(job.segments.size());
            for (auto& seg : job.segments) {
                const std::uint64_t end = seg->endOffset.load();
                const std::uint64_t length = end >= seg->startOffset ? end - seg->startOffset + 1 : 0;
                stats.segments.push_back({seg->startOffset, end, std::min(seg->bytesDownloaded.load(), length)});
            }
        }
    }
    if (rate > 1.0 && job.totalBytes > downloaded) {
        stats.eta = std::chrono::seconds(
            static_cast<std::int64_t>(static_cast<double>(job.totalBytes - downloaded) / rate));
    }

    {
        std::scoped_lock lock(job.statsMutex);
        job.lastStats = stats;
    }
    reporter_.reportProgress(job.id, stats);
}

void HttpDownloader::saveResumeMetadata(Job& job, bool whileRunning) const {
    // Bytes a running segment may still hold in its stdio buffer: well over one buffer.
    constexpr std::uint64_t kUnflushedMargin = 512 * 1024;
    std::string text = "source=" + job.request.source + "\n" + "totalBytes=" + std::to_string(job.totalBytes) + "\n" +
                       "supportsRanges=" + (job.supportsRanges ? "1" : "0") + "\n";
    for (const auto& seg : job.segments) {
        std::uint64_t done = seg->bytesDownloaded.load();
        if (whileRunning && !seg->finished.load()) done = done > kUnflushedMargin ? done - kUnflushedMargin : 0;
        text += "seg=" + std::to_string(seg->startOffset) + "," + std::to_string(seg->endOffset.load()) + "," +
                std::to_string(done) + "\n";
    }
#if !defined(_WIN32)
    if (whileRunning) {
        // The data the checkpoint vouches for has to be on disk before the checkpoint is.
        const int fd = ::open(job.filePath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ::fsync(fd);
            ::close(fd);
        }
    }
#endif
    // Written beside and then renamed over the old one, so a crash mid-write never
    // leaves a half-written file that would lose the download's progress.
    std::filesystem::path temp = job.metaPath;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc | std::ios::binary);
        if (!out) return;
        out << text;
        out.flush();
        if (!out) return;
    }
    std::error_code ec;
    std::filesystem::rename(temp, job.metaPath, ec);
    if (ec) std::filesystem::remove(temp, ec);
}

bool HttpDownloader::loadResumeMetadata(Job& job) const {
    std::ifstream in(job.metaPath);
    if (!in) return false;

    std::string line;
    std::string source;
    std::uint64_t totalBytes = 0;
    bool supportsRanges = false;
    std::vector<std::unique_ptr<Segment>> segments;

    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);

        if (key == "source") {
            source = value;
        } else if (key == "totalBytes") {
            totalBytes = std::strtoull(value.c_str(), nullptr, 10);
        } else if (key == "supportsRanges") {
            supportsRanges = (value == "1");
        } else if (key == "seg") {
            auto parts = utils::split(value, ',');
            if (parts.size() != 3) continue;
            auto seg = std::make_unique<Segment>();
            seg->startOffset = std::strtoull(parts[0].c_str(), nullptr, 10);
            seg->endOffset = std::strtoull(parts[1].c_str(), nullptr, 10);
            seg->bytesDownloaded = std::strtoull(parts[2].c_str(), nullptr, 10);
            segments.push_back(std::move(seg));
        }
    }

    // Metadata written before the size was known can't be resumed reliably; returning
    // false makes the caller probe the server again and start the file over.
    if (source != job.request.source || segments.empty() || totalBytes == 0) return false;

    job.totalBytes = totalBytes;
    job.supportsRanges = supportsRanges;
    job.segments = std::move(segments);
    return true;
}

}
