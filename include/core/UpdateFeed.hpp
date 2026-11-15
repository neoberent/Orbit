#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVersionNumber>

namespace orbit::core {

// What the update feed (/updates.json) offers one platform.
//
// The top level describes the Windows release (Orbit 3.1.5 and older read only that, on
// both platforms). "linux" carries the Linux build; from 3.1.6 on it may name its own
// "version", "notes" and "released", so Linux can move ahead of Windows. "linuxNews"
// tells Windows users what a Linux-only release brought, without anything to install.
struct UpdateOffer {
    QVersionNumber version;  // null = nothing to install on this platform
    QString url;
    QString sha256;
    qint64 size{0};
    QStringList notes;
    QString released;  // ISO date
    QString page;
    // Base64 Ed25519 signature over updateSignedMessage(), made with Orbit's release key.
    QString signature;

    // Windows only: a Linux release to tell about (null when there's none).
    QVersionNumber newsVersion;
    QStringList newsNotes;
    QString newsReleased;
};

[[nodiscard]] UpdateOffer readUpdateFeed(const QJsonObject& feed, bool forLinux);
[[nodiscard]] QStringList parseReleaseNotes(const QJsonValue& value);

// What the release key signs for one platform's offer: everything that decides what gets
// installed (platform, version, download link, checksum), one per line:
//   "orbit-update-v1\n<windows|linux>\n<version>\n<url>\n<sha256, lower-case hex>\n"
// The website's build (vite.config.js) produces the same text.
[[nodiscard]] QByteArray updateSignedMessage(const UpdateOffer& offer, bool forLinux);
// Whether the offer carries a valid signature by this Ed25519 public key (32 raw bytes).
// Without it, a changed updates.json (a hacked website or hosting account) could point
// every Orbit at another installer, since the checksum comes from the same file.
[[nodiscard]] bool verifyUpdateSignature(const UpdateOffer& offer, bool forLinux, const QByteArray& publicKey);

}
