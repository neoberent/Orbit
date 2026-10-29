#include "core/HttpDownloader.hpp"

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
#include <limits>
#include <mutex>
#include <system_error>

#if defined(_WIN32)
#include <share.h>
#endif

namespace orbit::core {

namespace {

constexpr std::uint64_t kUnknownSize = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kMinSegmentSize = 1024ull * 1024ull;
// Bound on a single curl request within a range-capable segment. Requesting in
// bounded chunks (instead of one open-ended request per segment) is what lets a
// donor segment's tail be handed to a fresh helper segment between chunks, without
// ever touching a request that's already in flight.
constexpr std::uint64_t kRebalanceChunkBytes = 8ull * 1024ull * 1024ull;

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
};

size_t curlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<WriteContext*>(userdata);
    const size_t total = size * nmemb;
    if (total == 0) return 0;
    const size_t written = std::fwrite(ptr, 1, total, ctx->file);
    ctx->counter->fetch_add(written, std::memory_order_relaxed);
    if (ctx->jobLimiter) ctx->jobLimiter->consume(written, ctx->paused, ctx->cancelled);
    if (ctx->globalLimiter) ctx->globalLimiter->consume(written, ctx->paused, ctx->cancelled);
    return written;
}

int curlProgressCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<WriteContext*>(clientp);
    if (ctx->paused->load(std::memory_order_relaxed) || ctx->cancelled->load(std::memory_order_relaxed)) {
        return 1;
    }
    return 0;
}

struct ProbeContext {
    bool supportsRanges{false};
    std::string contentDispositionFilename;
    std::uint64_t contentRangeTotal{0};
};

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
    std::vector<Job*> jobs;
    {
        std::scoped_lock lock(jobsMutex_);
        for (auto& [id, job] : jobs_) {
            job->paused = true;
            jobs.push_back(job.get());
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (Job* job : jobs) {
        while (job->running.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
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
        jobs_.emplace(id, std::move(job));
    }
    std::thread(&HttpDownloader::runSupervisor, this, id).detach();
}

void HttpDownloader::pause(DownloadId id) {
    std::scoped_lock lock(jobsMutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return;
    it->second->paused = true;
}

void HttpDownloader::resume(DownloadId id) {
    Job* jobPtr = nullptr;
    {
        std::scoped_lock lock(jobsMutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return;
        jobPtr = it->second.get();
    }
    if (jobPtr->running.load()) return;
    jobPtr->paused = false;
    jobPtr->cancelStart = false;
    std::thread(&HttpDownloader::runSupervisor, this, id).detach();
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

std::string HttpDownloader::netscapeCookieLine(const HttpCookie& cookie) { return netscapeLine(cookie); }

void HttpDownloader::applyRequestOptions(void* handle, const DownloadRequest& request) const {
    CURL* curl = static_cast<CURL*>(handle);
    applyCommonSslOptions(curl);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, request.userAgent.empty() ? "Orbit/1.0" : request.userAgent.c_str());
    if (!request.referrer.empty()) curl_easy_setopt(curl, CURLOPT_REFERER, request.referrer.c_str());
    // Handing cookies to curl's cookie engine (rather than a raw Cookie header) keeps
    // each one scoped to its own domain, so a redirect to a CDN can't leak them.
    for (const HttpCookie& cookie : request.cookies) {
        const std::string line = netscapeLine(cookie);
        if (!line.empty()) curl_easy_setopt(curl, CURLOPT_COOKIELIST, line.c_str());
    }

    ProxySettings proxy;
    {
        std::scoped_lock lock(proxyMutex_);
        proxy = proxy_;
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
            long httpCode = 0;
            if (curl_easy_perform(curl) == CURLE_OK) {
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
                                 std::string& outError) const {
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

    CURLcode res = curl_easy_perform(curl);
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
        res = curl_easy_perform(curl);
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

    if (job.segments.empty()) {
        reporter_.reportStateChanged(id, DownloadState::Connecting);

        std::uint64_t remoteSize = 0;
        bool supportsRanges = false;
        std::string fileName;
        std::string probeError;
        const bool probeOk = probeRemote(job.request, remoteSize, supportsRanges, fileName, probeError);

        if (!probeOk) {
            ORBIT_LOG_WARN("HttpDownloader", "Probe failed for " + job.request.source + ": " + probeError);
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
            job.supportsRanges = probeOk && supportsRanges && remoteSize > 0;

            if (!utils::preallocateFile(job.filePath, job.totalBytes)) {
                job.running = false;
                reporter_.reportError(id, "Could not create destination file: " + job.filePath.string());
                reporter_.reportStateChanged(id, DownloadState::Error);
                return;
            }

            if (job.supportsRanges) {
                job.desiredConnections = std::clamp(job.request.maxConnections, 1, 16);
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
            job.desiredConnections = job.supportsRanges ? std::clamp(job.request.maxConnections, 1, 16) : 1;
        }

        job.startTime = std::chrono::steady_clock::now();
        job.lastSampleTime = job.startTime;
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

    // A resumed download can carry more unfinished segments than the configured
    // connection count (rebalancing helpers are saved too), so only run up to
    // desiredConnections at once and start the rest as earlier ones complete.
    auto startPendingSegments = [&] {
        if (job.paused.load() || job.cancelStart.load()) return;
        int running = 0;
        for (const auto& s : job.segments) {
            if (s->thread.joinable() && !s->finished.load()) ++running;
        }
        for (int i = 0; i < static_cast<int>(job.segments.size()) && running < std::max(1, job.desiredConnections.load()); ++i) {
            Segment& seg = *job.segments[i];
            if (seg.finished.load() || seg.thread.joinable()) continue;
            seg.thread = std::thread(&HttpDownloader::downloadSegment, this, std::ref(job), std::ref(seg), i);
            ++running;
        }
    };
    startPendingSegments();

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        sampleAndReportProgress(job);
        startPendingSegments();
        rebalanceSegments(job);

        const bool allFinished = std::all_of(job.segments.begin(), job.segments.end(),
                                              [](const auto& s) { return s->finished.load(); });
        if (allFinished || job.paused.load() || job.cancelStart.load()) break;
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

    const bool allSucceeded = std::all_of(job.segments.begin(), job.segments.end(),
                                           [](const auto& s) { return s->success.load(); });

    if (!allSucceeded || job.segmentError.load()) {
        std::string message;
        {
            std::scoped_lock lock(job.statsMutex);
            message = job.lastErrorMessage;
        }
        saveResumeMetadata(job);
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
    curl_easy_setopt(curl, CURLOPT_URL, job.request.source.c_str());
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
        if (httpCode == 429 || httpCode == 503) {
            // The server is throttling parallel connections: shrink this job's target so
            // rebalancing stops adding helpers, instead of hammering it harder.
            int current = job.desiredConnections.load();
            while (current > 1 && !job.desiredConnections.compare_exchange_weak(current, current - 1)) {
            }
        }
        const auto backoff = std::chrono::milliseconds(std::min(30000, 1000 << (retries - 1)));
        const auto until = std::chrono::steady_clock::now() + backoff;
        while (std::chrono::steady_clock::now() < until) {
            if (job.paused.load() || job.cancelStart.load()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return !job.paused.load() && !job.cancelStart.load();
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
        WriteContext ctx{fp, &segment.bytesDownloaded, &job.paused, &job.cancelStart, &globalLimiter_, &job.limiter};
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
        while (true) {
            res = curl_easy_perform(curl);
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
        // Repeatedly request a bounded sub-range instead of the segment's whole span
        // in one request. This is what lets rebalanceSegments shrink endOffset
        // cleanly between chunks - a donor's in-flight request (bounded to
        // kRebalanceChunkBytes) is never touched by a shrink, since the split point
        // is always chosen at least one full chunk ahead of it.
        while (true) {
            const std::uint64_t currentEnd = segment.endOffset.load();
            const std::uint64_t resumeOffset = segment.startOffset + segment.bytesDownloaded.load();
            if (resumeOffset > currentEnd) break;  // shrunk out from under us - nothing left for this segment

            const std::uint64_t chunkEnd = std::min(currentEnd, resumeOffset + kRebalanceChunkBytes - 1);

#if defined(_WIN32)
            _fseeki64(fp, static_cast<long long>(resumeOffset), SEEK_SET);
#else
            std::fseek(fp, static_cast<long>(resumeOffset), SEEK_SET);
#endif
            WriteContext ctx{fp, &segment.bytesDownloaded, &job.paused, &job.cancelStart, &globalLimiter_, &job.limiter};
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

            const std::string rangeHeader = std::to_string(resumeOffset) + "-" + std::to_string(chunkEnd);
            curl_easy_setopt(curl, CURLOPT_RANGE, rangeHeader.c_str());

            res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                if (res == CURLE_ABORTED_BY_CALLBACK) break;
                if (shouldRetry(res)) continue;
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
    // Split past the donor's currently in-flight chunk (bounded to
    // kRebalanceChunkBytes) so the shrink below can never race a write to the same
    // byte range from both the donor and the new helper.
    const std::uint64_t splitPoint = pos + kRebalanceChunkBytes + (donorRemaining - kRebalanceChunkBytes) / 2;
    if (splitPoint >= oldEnd) return;

    donor->endOffset.store(splitPoint - 1);

    auto helper = std::make_unique<Segment>();
    helper->startOffset = splitPoint;
    helper->endOffset = oldEnd;
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
        const double elapsed = std::chrono::duration<double>(now - job.lastSampleTime).count();
        if (elapsed > 0.05) {
            const std::uint64_t delta = downloaded > job.bytesAtLastSample ? downloaded - job.bytesAtLastSample : 0;
            rate = static_cast<double>(delta) / elapsed;
            job.bytesAtLastSample = downloaded;
            job.lastSampleTime = now;
        } else {
            rate = job.lastStats.downloadRateBytesPerSec;
        }
    }

    DownloadStats stats;
    stats.bytesDownloaded = downloaded;
    stats.totalBytes = job.totalBytes;
    stats.downloadRateBytesPerSec = rate;
    stats.activeConnections = std::min(active, job.activeSegments.load());
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

void HttpDownloader::saveResumeMetadata(Job& job) const {
    std::ofstream out(job.metaPath, std::ios::trunc);
    if (!out) return;
    out << "source=" << job.request.source << "\n";
    out << "totalBytes=" << job.totalBytes << "\n";
    out << "supportsRanges=" << (job.supportsRanges ? 1 : 0) << "\n";
    for (const auto& seg : job.segments) {
        out << "seg=" << seg->startOffset << "," << seg->endOffset << "," << seg->bytesDownloaded.load() << "\n";
    }
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
