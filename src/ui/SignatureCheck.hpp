#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

// Checking a downloaded file against the signature its publisher put beside it
// (<link>.asc / .sig for OpenPGP, <link>.minisig for minisign). OpenPGP goes through the
// user's own GnuPG keyring; minisign is checked here, against keys the user added.
namespace signature_check {

enum class State {
    None,           // nothing checked
    Good,           // signed by a key the user trusts to sign it
    Bad,            // the signature doesn't match the file
    UnknownKey,     // signed, but by a key the user doesn't have
    Revoked,        // good signature, but the key was revoked
    Expired,        // good signature from an expired key
    NotASignature,  // the file beside the download isn't a signature after all
    Failed          // couldn't check (no gpg, unreadable file, ...)
};

struct Outcome {
    State state{State::None};
    QString signer;  // the key's user ID (OpenPGP) or the trusted comment (minisign)
    QString keyId;   // hex
    QString detail;  // for Failed
};

[[nodiscard]] const char* toString(State state) noexcept;

// Reads `gpg --status-fd` output (lines starting "[GNUPG:] ").
[[nodiscard]] Outcome parseGpgStatus(const QByteArray& status);

// A minisign signature starts with "untrusted comment:" and has the signature on its
// second line.
[[nodiscard]] bool looksLikeMinisign(const QByteArray& signature);

// The keys in what the user pasted: minisign public keys are 56 base64 characters
// starting "RW". Anything else is ignored.
[[nodiscard]] QStringList parseMinisignKeys(const QString& text);

// Checks `filePath` against a minisign signature with whichever of `publicKeys` made it
// (by key ID). `cancelled` is polled while the file is read.
[[nodiscard]] Outcome verifyMinisign(const QString& filePath, const QByteArray& signature, const QStringList& publicKeys,
                                     const std::function<bool()>& cancelled = {});

}
