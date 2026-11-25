#include "SignatureCheck.hpp"

#include <QFile>
#include <QList>
#include <QRegularExpression>

#include <openssl/evp.h>

#include <cstring>
#include <memory>
#include <optional>

namespace signature_check {

namespace {

// GnuPG writes user IDs in status lines with %XX escapes.
QString unescapeStatus(const QByteArray& text) {
    return QString::fromUtf8(QByteArray::fromPercentEncoding(text));
}

QString hexId(const QByteArray& raw) {
    // minisign stores the key ID little-endian; `minisign` prints it the other way round.
    QByteArray reversed(raw.size(), '\0');
    for (qsizetype i = 0; i < raw.size(); ++i) reversed[i] = raw[raw.size() - 1 - i];
    return QString::fromLatin1(reversed.toHex().toUpper());
}

bool ed25519Verify(const QByteArray& publicKey, const QByteArray& signature, const unsigned char* message, std::size_t length) {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(publicKey.constData()),
                                    static_cast<std::size_t>(publicKey.size())),
        &EVP_PKEY_free);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    return key && ctx && EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) == 1 &&
           EVP_DigestVerify(ctx.get(), reinterpret_cast<const unsigned char*>(signature.constData()),
                            static_cast<std::size_t>(signature.size()), message, length) == 1;
}

struct PublicKey {
    QByteArray keyId;  // 8 bytes
    QByteArray key;    // 32 bytes
};

std::optional<PublicKey> decodePublicKey(const QString& text) {
    const QByteArray raw = QByteArray::fromBase64(text.trimmed().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (raw.size() != 42 || raw.left(2) != "Ed") return std::nullopt;
    return PublicKey{raw.mid(2, 8), raw.mid(10, 32)};
}

}

const char* toString(State state) noexcept {
    switch (state) {
        case State::None: return "none";
        case State::Good: return "good";
        case State::Bad: return "bad";
        case State::UnknownKey: return "unknown-key";
        case State::Revoked: return "revoked";
        case State::Expired: return "expired";
        case State::NotASignature: return "not-a-signature";
        case State::Failed: return "failed";
    }
    return "none";
}

Outcome parseGpgStatus(const QByteArray& status) {
    Outcome good;
    Outcome worst;
    bool sawGood = false;
    bool sawBad = false;
    bool sawRevoked = false;
    bool sawExpired = false;
    bool untrusted = false;
    QString missingKey;
    bool noData = false;

    for (const QByteArray& rawLine : status.split('\n')) {
        const QByteArray line = rawLine.trimmed();
        if (!line.startsWith("[GNUPG:] ")) continue;
        const QByteArray rest = line.mid(9);
        const qsizetype space = rest.indexOf(' ');
        const QByteArray keyword = space < 0 ? rest : rest.left(space);
        const QByteArray args = space < 0 ? QByteArray() : rest.mid(space + 1);
        const qsizetype argSpace = args.indexOf(' ');
        const QByteArray first = argSpace < 0 ? args : args.left(argSpace);
        const QByteArray remainder = argSpace < 0 ? QByteArray() : args.mid(argSpace + 1);

        if (keyword == "GOODSIG") {
            sawGood = true;
            good.keyId = QString::fromLatin1(first);
            good.signer = unescapeStatus(remainder);
        } else if (keyword == "EXPKEYSIG" || keyword == "EXPSIG") {
            sawExpired = true;
            worst.keyId = QString::fromLatin1(first);
            worst.signer = unescapeStatus(remainder);
        } else if (keyword == "REVKEYSIG") {
            sawRevoked = true;
            worst.keyId = QString::fromLatin1(first);
            worst.signer = unescapeStatus(remainder);
        } else if (keyword == "BADSIG") {
            sawBad = true;
            worst.keyId = QString::fromLatin1(first);
            worst.signer = unescapeStatus(remainder);
        } else if (keyword == "NO_PUBKEY") {
            missingKey = QString::fromLatin1(first);
        } else if (keyword == "ERRSIG") {
            // ERRSIG <keyid> <pkalgo> <hashalgo> <class> <time> <rc> [<fpr>]; rc 9 = no key
            const QList<QByteArray> parts = args.split(' ');
            if (parts.size() >= 6 && parts[5] == "9" && missingKey.isEmpty()) missingKey = QString::fromLatin1(parts[0]);
        } else if (keyword == "NODATA") {
            noData = true;
        } else if (keyword == "TRUST_NEVER") {
            untrusted = true;
        }
    }

    // A file with several signatures is as good as its worst one.
    if (sawBad) return Outcome{State::Bad, worst.signer, worst.keyId, {}};
    if (sawRevoked) return Outcome{State::Revoked, worst.signer, worst.keyId, {}};
    if (sawGood && untrusted) return Outcome{State::UnknownKey, good.signer, good.keyId, QStringLiteral("You marked this key as never trusted")};
    if (sawGood) return Outcome{State::Good, good.signer, good.keyId, {}};
    if (sawExpired) return Outcome{State::Expired, worst.signer, worst.keyId, {}};
    if (!missingKey.isEmpty()) return Outcome{State::UnknownKey, {}, missingKey, {}};
    if (noData) return Outcome{State::NotASignature, {}, {}, {}};
    return Outcome{State::Failed, {}, {}, QStringLiteral("GnuPG gave no verdict")};
}

bool looksLikeMinisign(const QByteArray& signature) {
    return signature.startsWith("untrusted comment:") && signature.count('\n') >= 3;
}

QStringList parseMinisignKeys(const QString& text) {
    QStringList keys;
    const QStringList words = text.split(QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts);
    for (const QString& word : words) {
        if (word.size() == 56 && word.startsWith(QLatin1String("RW")) && decodePublicKey(word) && !keys.contains(word)) keys << word;
    }
    return keys;
}

Outcome verifyMinisign(const QString& filePath, const QByteArray& signature, const QStringList& publicKeys,
                       const std::function<bool()>& cancelled) {
    Outcome out;
    const QList<QByteArray> lines = signature.split('\n');
    if (lines.size() < 4 || !lines[0].startsWith("untrusted comment:") || !lines[2].startsWith("trusted comment: ")) {
        out.state = State::NotASignature;
        return out;
    }
    const QByteArray sigStruct = QByteArray::fromBase64(lines[1].trimmed(), QByteArray::AbortOnBase64DecodingErrors);
    const QByteArray globalSig = QByteArray::fromBase64(lines[3].trimmed(), QByteArray::AbortOnBase64DecodingErrors);
    if (sigStruct.size() != 74 || globalSig.size() != 64) {
        out.state = State::NotASignature;
        return out;
    }
    const QByteArray algorithm = sigStruct.left(2);
    const QByteArray keyId = sigStruct.mid(2, 8);
    const QByteArray sig = sigStruct.mid(10, 64);
    QByteArray trustedComment = lines[2].mid(static_cast<qsizetype>(std::strlen("trusted comment: ")));
    if (trustedComment.endsWith('\r')) trustedComment.chop(1);
    out.keyId = hexId(keyId);
    if (algorithm != "Ed" && algorithm != "ED") {
        out.state = State::NotASignature;
        return out;
    }

    std::optional<PublicKey> key;
    for (const QString& text : publicKeys) {
        auto candidate = decodePublicKey(text);
        if (candidate && candidate->keyId == keyId) key = candidate;
    }
    if (!key) {
        out.state = State::UnknownKey;
        return out;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        out.state = State::Failed;
        out.detail = QStringLiteral("Couldn't read the file");
        return out;
    }
    QByteArray message;
    if (algorithm == "ED") {
        // Current minisign signs the file's BLAKE2b-512 hash.
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
        if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_blake2b512(), nullptr) != 1) {
            out.state = State::Failed;
            out.detail = QStringLiteral("BLAKE2b isn't available");
            return out;
        }
        QByteArray buffer(1 << 20, Qt::Uninitialized);
        for (;;) {
            if (cancelled && cancelled()) {
                out.state = State::Failed;
                out.detail = QStringLiteral("Cancelled");
                return out;
            }
            const qint64 n = file.read(buffer.data(), buffer.size());
            if (n < 0) {
                out.state = State::Failed;
                out.detail = QStringLiteral("Couldn't read the file");
                return out;
            }
            if (n == 0) break;
            EVP_DigestUpdate(ctx.get(), buffer.constData(), static_cast<std::size_t>(n));
        }
        unsigned char digest[64];
        unsigned int length = 0;
        EVP_DigestFinal_ex(ctx.get(), digest, &length);
        message = QByteArray(reinterpret_cast<const char*>(digest), static_cast<qsizetype>(length));
    } else {
        // Old minisign signed the whole file; that needs it in memory.
        if (file.size() > (qint64(1) << 30)) {
            out.state = State::Failed;
            out.detail = QStringLiteral("Too big for an old-style minisign signature");
            return out;
        }
        message = file.readAll();
    }

    if (!ed25519Verify(key->key, sig, reinterpret_cast<const unsigned char*>(message.constData()), static_cast<std::size_t>(message.size()))) {
        out.state = State::Bad;
        return out;
    }
    // The trusted comment is signed together with the signature, so it can be shown.
    const QByteArray global = sig + trustedComment;
    if (!ed25519Verify(key->key, globalSig, reinterpret_cast<const unsigned char*>(global.constData()), static_cast<std::size_t>(global.size()))) {
        out.state = State::Bad;
        out.detail = QStringLiteral("The trusted comment was changed");
        return out;
    }
    out.state = State::Good;
    out.signer = QString::fromUtf8(trustedComment).left(200);
    return out;
}

}
