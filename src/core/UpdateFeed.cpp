#include "core/UpdateFeed.hpp"

#include <QJsonArray>
#include <QRegularExpression>

#include <openssl/evp.h>

namespace orbit::core {

namespace {
QVersionNumber versionOf(const QJsonValue& value) {
    QString text = value.toString().trimmed();
    if (text.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) text.remove(0, 1);
    return QVersionNumber::fromString(text);
}
}

QStringList parseReleaseNotes(const QJsonValue& value) {
    QStringList lines;
    if (value.isArray()) {
        for (const QJsonValue& item : value.toArray()) lines << item.toString();
    } else if (value.isString()) {
        lines = value.toString().split(QLatin1Char('\n'));
    }
    static const QRegularExpression bullet(QStringLiteral("^\\s*(?:[-*\\x{2022}]\\s+)?"));
    QStringList cleaned;
    for (QString line : lines) {
        line.remove(bullet);
        line = line.trimmed();
        if (!line.isEmpty()) cleaned << line.left(500);
        if (cleaned.size() >= 40) break;
    }
    return cleaned;
}

UpdateOffer readUpdateFeed(const QJsonObject& feed, bool forLinux) {
    UpdateOffer offer;
    offer.page = feed.value("page").toString();
    if (forLinux) {
        const QJsonObject linux = feed.value("linux").toObject();
        if (linux.isEmpty()) return offer;  // no Linux build: nothing to offer
        // A Linux entry with its own version is ahead of (or apart from) Windows; one
        // without follows the top-level release, as in feeds before 3.1.6.
        const bool own = linux.contains("version");
        offer.version = versionOf(own ? linux.value("version") : feed.value("version"));
        offer.url = linux.value("url").toString();
        offer.sha256 = linux.value("sha256").toString();
        offer.size = linux.value("size").toInteger(0);
        offer.notes = parseReleaseNotes(own && linux.contains("notes") ? linux.value("notes") : feed.value("notes"));
        offer.released = (own && linux.contains("released") ? linux.value("released") : feed.value("released")).toString();
        offer.signature = linux.value("signature").toString();
        return offer;
    }
    offer.signature = feed.value("signature").toString();
    offer.version = versionOf(feed.value("version"));
    offer.url = feed.value("url").toString();
    offer.sha256 = feed.value("sha256").toString();
    offer.size = feed.value("size").toInteger(0);
    offer.notes = parseReleaseNotes(feed.value("notes"));
    offer.released = feed.value("released").toString();
    const QJsonObject news = feed.value("linuxNews").toObject();
    offer.newsVersion = versionOf(news.value("version"));
    offer.newsNotes = parseReleaseNotes(news.value("notes"));
    offer.newsReleased = news.value("released").toString();
    return offer;
}

}

namespace orbit::core {

QByteArray updateSignedMessage(const UpdateOffer& offer, bool forLinux) {
    return "orbit-update-v1\n" + QByteArray(forLinux ? "linux" : "windows") + "\n" + offer.version.toString().toUtf8() + "\n" +
           offer.url.toUtf8() + "\n" + offer.sha256.trimmed().toLower().toUtf8() + "\n";
}

bool verifyUpdateSignature(const UpdateOffer& offer, bool forLinux, const QByteArray& publicKey) {
    const QByteArray signature = QByteArray::fromBase64(offer.signature.trimmed().toLatin1());
    if (publicKey.size() != 32 || signature.size() != 64 || offer.version.isNull()) return false;
    EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                reinterpret_cast<const unsigned char*>(publicKey.constData()), 32);
    if (!key) return false;
    const QByteArray message = updateSignedMessage(offer, forLinux);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    const bool ok = ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
                    EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char*>(signature.constData()), 64,
                                     reinterpret_cast<const unsigned char*>(message.constData()),
                                     static_cast<size_t>(message.size())) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

}
