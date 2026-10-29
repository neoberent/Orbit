#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace orbit::utils {

enum class HashAlgorithm { Md5, Sha1, Sha256, Sha512 };

[[nodiscard]] const char* hashAlgorithmName(HashAlgorithm algorithm) noexcept;

struct ChecksumSpec {
    HashAlgorithm algorithm{HashAlgorithm::Sha256};
    std::string hex;  // lowercase

    [[nodiscard]] std::string toString() const;  // "sha256:<hex>"
};

// Accepts "sha256:<hex>", "SHA-256 <hex>", or a bare hex digest (the algorithm is then
// inferred from its length). Returns nullopt for anything that isn't a valid digest.
[[nodiscard]] std::optional<ChecksumSpec> parseChecksum(const std::string& text);

// Called with (bytesHashed, totalBytes); return false to abort.
using HashProgress = std::function<bool(std::uint64_t, std::uint64_t)>;

// Hashes the file once, producing one lowercase hex digest per requested algorithm (in
// the same order). Returns nullopt if the file can't be read or progress aborted.
[[nodiscard]] std::optional<std::vector<std::string>> hashFile(const std::filesystem::path& path,
                                                               const std::vector<HashAlgorithm>& algorithms,
                                                               const HashProgress& progress = {});

}
