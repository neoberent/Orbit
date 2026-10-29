#include "utils/Hashing.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>

namespace orbit::utils {

namespace {

const EVP_MD* digestFor(HashAlgorithm algorithm) {
    switch (algorithm) {
        case HashAlgorithm::Md5: return EVP_md5();
        case HashAlgorithm::Sha1: return EVP_sha1();
        case HashAlgorithm::Sha256: return EVP_sha256();
        case HashAlgorithm::Sha512: return EVP_sha512();
    }
    return EVP_sha256();
}

std::string toHex(const unsigned char* data, unsigned int size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (unsigned int i = 0; i < size; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0f]);
    }
    return out;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

}

const char* hashAlgorithmName(HashAlgorithm algorithm) noexcept {
    switch (algorithm) {
        case HashAlgorithm::Md5: return "md5";
        case HashAlgorithm::Sha1: return "sha1";
        case HashAlgorithm::Sha256: return "sha256";
        case HashAlgorithm::Sha512: return "sha512";
    }
    return "sha256";
}

std::string ChecksumSpec::toString() const { return std::string(hashAlgorithmName(algorithm)) + ":" + hex; }

std::optional<ChecksumSpec> parseChecksum(const std::string& text) {
    std::string value;
    for (char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) value.push_back(c);
    }
    value = lower(value);
    if (value.empty()) return std::nullopt;

    std::optional<HashAlgorithm> named;
    static const std::pair<const char*, HashAlgorithm> kPrefixes[] = {
        {"sha-256", HashAlgorithm::Sha256}, {"sha256", HashAlgorithm::Sha256},
        {"sha-512", HashAlgorithm::Sha512}, {"sha512", HashAlgorithm::Sha512},
        {"sha-1", HashAlgorithm::Sha1},     {"sha1", HashAlgorithm::Sha1},
        {"md5", HashAlgorithm::Md5},
    };
    for (const auto& [prefix, algorithm] : kPrefixes) {
        const std::string p(prefix);
        if (value.rfind(p, 0) == 0) {
            named = algorithm;
            value = value.substr(p.size());
            if (!value.empty() && (value.front() == ':' || value.front() == '=')) value.erase(value.begin());
            break;
        }
    }

    if (value.empty() || !std::all_of(value.begin(), value.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })) {
        return std::nullopt;
    }

    HashAlgorithm inferred;
    switch (value.size()) {
        case 32: inferred = HashAlgorithm::Md5; break;
        case 40: inferred = HashAlgorithm::Sha1; break;
        case 64: inferred = HashAlgorithm::Sha256; break;
        case 128: inferred = HashAlgorithm::Sha512; break;
        default: return std::nullopt;
    }
    if (named && *named != inferred) return std::nullopt;
    return ChecksumSpec{inferred, value};
}

std::optional<std::vector<std::string>> hashFile(const std::filesystem::path& path,
                                                 const std::vector<HashAlgorithm>& algorithms,
                                                 const HashProgress& progress) {
    std::ifstream in(path, std::ios::binary);
    if (!in || algorithms.empty()) return std::nullopt;

    std::error_code ec;
    const std::uint64_t total = std::filesystem::file_size(path, ec);

    using ContextPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    std::vector<ContextPtr> contexts;
    for (HashAlgorithm algorithm : algorithms) {
        ContextPtr ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
        if (!ctx || EVP_DigestInit_ex(ctx.get(), digestFor(algorithm), nullptr) != 1) return std::nullopt;
        contexts.push_back(std::move(ctx));
    }

    std::vector<char> buffer(1 << 20);
    std::uint64_t done = 0;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = in.gcount();
        if (got <= 0) break;
        for (auto& ctx : contexts) EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<size_t>(got));
        done += static_cast<std::uint64_t>(got);
        if (progress && !progress(done, total)) return std::nullopt;
    }
    if (in.bad()) return std::nullopt;

    std::vector<std::string> digests;
    for (auto& ctx : contexts) {
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int size = 0;
        if (EVP_DigestFinal_ex(ctx.get(), md, &size) != 1) return std::nullopt;
        digests.push_back(toHex(md, size));
    }
    return digests;
}

}
