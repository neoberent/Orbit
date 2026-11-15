#include "core/InterfaceDns.hpp"

#include "core/HttpDownloader.hpp"

#include "utils/Logger.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace orbit::core {

namespace {

// Resolvers reached by IP address, so finding them needs no lookup of its own. Their
// certificates are issued for these addresses, so TLS is verified as usual.
const char* const kResolvers[] = {"https://9.9.9.9/dns-query", "https://1.1.1.1/dns-query"};

constexpr int kTypeA = 1;
constexpr int kTypeAAAA = 28;
constexpr int kMinTtl = 60;
constexpr int kMaxTtl = 600;
constexpr size_t kMaxAnswerBytes = 64 * 1024;

struct CacheEntry {
    std::vector<std::string> addresses;
    std::chrono::steady_clock::time_point expires;
};

std::mutex& cacheMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, CacheEntry>& cache() {
    static std::unordered_map<std::string, CacheEntry> entries;
    return entries;
}

size_t collect(char* data, size_t size, size_t count, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const size_t bytes = size * count;
    if (out->size() + bytes > kMaxAnswerBytes) return 0;
    out->append(data, bytes);
    return bytes;
}

// One DNS-over-HTTPS exchange (RFC 8484, POST) bound to the interface.
bool exchange(const char* resolver, const std::string& query, const std::string& interfaceName, std::string& answer) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    const std::string binding = "if!" + interfaceName;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/dns-message");
    headers = curl_slist_append(headers, "Accept: application/dns-message");
    curl_easy_setopt(curl, CURLOPT_URL, resolver);
    HttpDownloader::applyTrustStore(curl);
    curl_easy_setopt(curl, CURLOPT_INTERFACE, binding.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, query.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(query.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &answer);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Orbit");
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result == CURLE_OK && status == 200;
}

// Moves past a (possibly compressed) name; false if it runs off the message.
bool skipName(const std::string& message, size_t& pos) {
    for (int guard = 0; guard < 128; ++guard) {
        if (pos >= message.size()) return false;
        const auto length = static_cast<unsigned char>(message[pos]);
        if (length == 0) {
            ++pos;
            return true;
        }
        if ((length & 0xC0) == 0xC0) {
            pos += 2;
            return pos <= message.size();
        }
        pos += 1 + length;
    }
    return false;
}

unsigned read16(const std::string& message, size_t pos) {
    return (static_cast<unsigned char>(message[pos]) << 8) | static_cast<unsigned char>(message[pos + 1]);
}

}

std::string InterfaceDns::buildQuery(const std::string& host, int type) {
    std::string query;
    // ID 0 (RFC 8484 recommends it, for caching), recursion desired, one question.
    query.append("\x00\x00\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00", 12);
    size_t start = 0;
    while (start <= host.size()) {
        size_t dot = host.find('.', start);
        if (dot == std::string::npos) dot = host.size();
        const size_t length = dot - start;
        if (length > 63) return {};
        if (length > 0) {
            query.push_back(static_cast<char>(length));
            query.append(host, start, length);
        }
        start = dot + 1;
    }
    query.push_back('\0');
    query.push_back(static_cast<char>((type >> 8) & 0xFF));
    query.push_back(static_cast<char>(type & 0xFF));
    query.append("\x00\x01", 2);  // class IN
    return query;
}

std::vector<std::string> InterfaceDns::parseAnswer(const std::string& message, int& ttlSeconds) {
    std::vector<std::string> addresses;
    ttlSeconds = kMaxTtl;
    if (message.size() < 12) return addresses;
    if ((read16(message, 2) & 0x000F) != 0) return addresses;  // RCODE: an error answer
    const unsigned questions = read16(message, 4);
    const unsigned answers = read16(message, 6);
    size_t pos = 12;
    for (unsigned i = 0; i < questions; ++i) {
        if (!skipName(message, pos) || pos + 4 > message.size()) return {};
        pos += 4;
    }
    for (unsigned i = 0; i < answers; ++i) {
        if (!skipName(message, pos) || pos + 10 > message.size()) break;
        const unsigned type = read16(message, pos);
        const std::uint32_t ttl = (static_cast<std::uint32_t>(read16(message, pos + 4)) << 16) | read16(message, pos + 6);
        const unsigned length = read16(message, pos + 8);
        pos += 10;
        if (pos + length > message.size()) break;
        char text[64];
        if (type == kTypeA && length == 4) {
            std::snprintf(text, sizeof(text), "%u.%u.%u.%u", static_cast<unsigned char>(message[pos]),
                          static_cast<unsigned char>(message[pos + 1]), static_cast<unsigned char>(message[pos + 2]),
                          static_cast<unsigned char>(message[pos + 3]));
            addresses.emplace_back(text);
            ttlSeconds = std::min<int>(ttlSeconds, static_cast<int>(std::min<std::uint32_t>(ttl, kMaxTtl)));
        } else if (type == kTypeAAAA && length == 16) {
            std::string ip;
            for (int group = 0; group < 8; ++group) {
                std::snprintf(text, sizeof(text), group == 0 ? "%x" : ":%x", read16(message, pos + group * 2));
                ip += text;
            }
            addresses.push_back(ip);
            ttlSeconds = std::min<int>(ttlSeconds, static_cast<int>(std::min<std::uint32_t>(ttl, kMaxTtl)));
        }
        pos += length;
    }
    ttlSeconds = std::clamp(ttlSeconds, kMinTtl, kMaxTtl);
    return addresses;
}

std::vector<std::string> InterfaceDns::resolve(const std::string& host, const std::string& interfaceName) {
    if (host.empty()) return {};
    if (isIpLiteral(host)) return {host};
    const std::string key = interfaceName + "|" + host;
    {
        std::scoped_lock lock(cacheMutex());
        const auto it = cache().find(key);
        if (it != cache().end() && std::chrono::steady_clock::now() < it->second.expires) return it->second.addresses;
    }

    std::vector<std::string> addresses;
    int ttl = kMaxTtl;
    for (const char* resolver : kResolvers) {
        for (const int type : {kTypeA, kTypeAAAA}) {
            const std::string query = buildQuery(host, type);
            std::string answer;
            if (query.empty() || !exchange(resolver, query, interfaceName, answer)) continue;
            int recordTtl = kMaxTtl;
            for (std::string& address : parseAnswer(answer, recordTtl)) addresses.push_back(std::move(address));
            ttl = std::min(ttl, recordTtl);
        }
        if (!addresses.empty()) break;
    }
    if (addresses.empty()) {
        ORBIT_LOG_WARN("InterfaceDns", "Couldn't look up " + host + " through " + interfaceName);
        return {};
    }
    std::scoped_lock lock(cacheMutex());
    cache()[key] = {addresses, std::chrono::steady_clock::now() + std::chrono::seconds(ttl)};
    return addresses;
}

std::string InterfaceDns::hostOf(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    std::string authority = url.substr(scheme + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (const auto at = authority.rfind('@'); at != std::string::npos) authority = authority.substr(at + 1);
    std::string host;
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) return {};
        host = authority.substr(1, close - 1);
    } else {
        host = authority.substr(0, authority.find(':'));
    }
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!host.empty() && host.back() == '.') host.pop_back();
    return host;
}

int InterfaceDns::portOf(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return 0;
    std::string authority = url.substr(scheme + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (const auto at = authority.rfind('@'); at != std::string::npos) authority = authority.substr(at + 1);
    const auto close = authority.rfind(']');
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos && (close == std::string::npos || colon > close)) {
        const int port = std::atoi(authority.c_str() + colon + 1);
        if (port > 0 && port < 65536) return port;
    }
    std::string lowerScheme = url.substr(0, scheme);
    std::transform(lowerScheme.begin(), lowerScheme.end(), lowerScheme.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowerScheme == "https" ? 443 : 80;
}

bool InterfaceDns::isIpLiteral(const std::string& host) {
    if (host.find(':') != std::string::npos) return true;  // IPv6
    int dots = 0;
    for (char c : host) {
        if (c == '.') ++dots;
        else if (c < '0' || c > '9') return false;
    }
    return dots == 3;
}

void InterfaceDns::clearCache() {
    std::scoped_lock lock(cacheMutex());
    cache().clear();
}

}
