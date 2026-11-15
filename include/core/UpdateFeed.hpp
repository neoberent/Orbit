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

    // Windows only: a Linux release to tell about (null when there's none).
    QVersionNumber newsVersion;
    QStringList newsNotes;
    QString newsReleased;
};

[[nodiscard]] UpdateOffer readUpdateFeed(const QJsonObject& feed, bool forLinux);
[[nodiscard]] QStringList parseReleaseNotes(const QJsonValue& value);

}
