#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// Metalink files (.meta4, RFC 5854, and the older .metalink 3.0) describe downloads by
// their mirrors and checksums. Orbit turns each <file> into one download: the best link
// is the source, the next ones become mirrors, and the strongest hash is checked at the
// end.
namespace metalink {

struct File {
    QString name;       // a bare file name; folders in the Metalink are dropped
    qint64 size{-1};    // -1 when the Metalink doesn't say
    QString checksum;   // "sha512:<hex>", "sha256:<hex>", "sha1:<hex>", "md5:<hex>" or empty
    QStringList urls;   // http(s) only, best first
};

struct Result {
    QList<File> files;
    QString error;  // set when nothing usable was found
};

// Most links a single file keeps (one source plus the mirrors DownloadRequest takes).
inline constexpr int kMaxLinksPerFile = 9;
// More files than this is not a download list anyone reviews; refuse it.
inline constexpr int kMaxFiles = 500;
// Metalink documents are small; anything bigger is not one.
inline constexpr qint64 kMaxBytes = 4 * 1024 * 1024;

[[nodiscard]] Result parse(const QByteArray& xml);

// True for a link or path whose name ends in .meta4 or .metalink.
[[nodiscard]] bool looksLikeMetalink(const QString& source);

}
