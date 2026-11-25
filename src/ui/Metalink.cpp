#include "Metalink.hpp"

#include <QXmlStreamReader>

#include <algorithm>

namespace metalink {

namespace {

struct Link {
    QString url;
    int rank{0};  // lower is better
    int order{0};
};

struct Hash {
    QString algorithm;  // "sha512", "sha256", "sha1", "md5"
    QString hex;
};

// Both versions' names for a hash type, mapped to Orbit's checksum prefixes.
QString algorithmOf(QString type) {
    type = type.trimmed().toLower().remove(QLatin1Char('-'));
    if (type == QLatin1String("sha512") || type == QLatin1String("sha256") || type == QLatin1String("sha1") ||
        type == QLatin1String("md5")) {
        return type;
    }
    return {};
}

int hexLength(const QString& algorithm) {
    if (algorithm == QLatin1String("sha512")) return 128;
    if (algorithm == QLatin1String("sha256")) return 64;
    if (algorithm == QLatin1String("sha1")) return 40;
    return 32;
}

int strength(const QString& algorithm) {
    static const QStringList order{QStringLiteral("md5"), QStringLiteral("sha1"), QStringLiteral("sha256"), QStringLiteral("sha512")};
    return static_cast<int>(order.indexOf(algorithm));
}

// The last part of the name only: a Metalink may say "dir/../../.bashrc", and the files
// go into the folder the user chose, nowhere else.
QString safeName(QString name) {
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    name = name.mid(name.lastIndexOf(QLatin1Char('/')) + 1);
    name.remove(QChar(0));
    for (QChar& c : name) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f) c = QLatin1Char('_');
    }
    name = name.trimmed();
    if (name == QLatin1String(".") || name == QLatin1String("..")) return {};
    return name.left(200);
}

bool isHttp(const QString& url) {
    return url.startsWith(QLatin1String("https://"), Qt::CaseInsensitive) ||
           url.startsWith(QLatin1String("http://"), Qt::CaseInsensitive);
}

}

bool looksLikeMetalink(const QString& source) {
    QString path = source.trimmed();
    // A link's query or fragment isn't part of its name.
    if (path.contains(QLatin1String("://"))) {
        for (const QChar mark : {QLatin1Char('?'), QLatin1Char('#')}) {
            const int cut = path.indexOf(mark);
            if (cut >= 0) path.truncate(cut);
        }
    }
    return path.endsWith(QLatin1String(".meta4"), Qt::CaseInsensitive) ||
           path.endsWith(QLatin1String(".metalink"), Qt::CaseInsensitive);
}

Result parse(const QByteArray& xml) {
    Result result;
    if (xml.size() > kMaxBytes) {
        result.error = QStringLiteral("That's too big to be a Metalink file");
        return result;
    }

    QXmlStreamReader reader(xml);
    bool sawMetalink = false;
    bool inFile = false;
    int piecesDepth = 0;  // Metalink 3 lists per-piece hashes, which aren't the file's
    File file;
    QList<Link> links;
    Hash best;
    int linkOrder = 0;

    auto finishFile = [&] {
        std::stable_sort(links.begin(), links.end(), [](const Link& a, const Link& b) {
            return a.rank != b.rank ? a.rank < b.rank : a.order < b.order;
        });
        for (const Link& link : links) {
            if (!file.urls.contains(link.url)) file.urls << link.url;
            if (file.urls.size() >= kMaxLinksPerFile) break;
        }
        if (!best.hex.isEmpty()) file.checksum = best.algorithm + QLatin1Char(':') + best.hex;
        if (!file.name.isEmpty() && !file.urls.isEmpty()) result.files << file;
        file = File{};
        links.clear();
        best = Hash{};
    };

    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::EndElement) {
            const auto name = reader.name();
            if (name == QLatin1String("pieces")) piecesDepth = std::max(0, piecesDepth - 1);
            if (name == QLatin1String("file") && inFile) {
                inFile = false;
                finishFile();
                if (result.files.size() > kMaxFiles) {
                    result.files.clear();
                    result.error = QStringLiteral("The Metalink lists more than %1 files").arg(kMaxFiles);
                    return result;
                }
            }
            continue;
        }
        if (token != QXmlStreamReader::StartElement) continue;

        const auto name = reader.name();
        if (name == QLatin1String("metalink")) {
            sawMetalink = true;
            continue;
        }
        if (!sawMetalink) break;  // not a Metalink document at all
        if (name == QLatin1String("pieces")) {
            ++piecesDepth;
            continue;
        }
        if (name == QLatin1String("file")) {
            if (inFile) finishFile();  // a nested <file> isn't valid; keep what came before
            inFile = true;
            file.name = safeName(reader.attributes().value(QLatin1String("name")).toString());
            continue;
        }
        if (!inFile) continue;

        if (name == QLatin1String("size")) {
            bool ok = false;
            const qint64 size = reader.readElementText().trimmed().toLongLong(&ok);
            if (ok && size >= 0) file.size = size;
        } else if (name == QLatin1String("hash") && piecesDepth == 0) {
            const QString algorithm = algorithmOf(reader.attributes().value(QLatin1String("type")).toString());
            const QString hex = reader.readElementText().trimmed().toLower();
            const bool valid = !algorithm.isEmpty() && hex.size() == hexLength(algorithm) &&
                               std::all_of(hex.begin(), hex.end(), [](QChar c) { return c.isDigit() || (c >= QLatin1Char('a') && c <= QLatin1Char('f')); });
            if (valid && (best.hex.isEmpty() || strength(algorithm) > strength(best.algorithm))) best = Hash{algorithm, hex};
        } else if (name == QLatin1String("url")) {
            const auto attributes = reader.attributes();
            int rank = 999999;
            bool ok = false;
            if (attributes.hasAttribute(QLatin1String("priority"))) {  // Metalink 4: 1 is the best
                const int priority = attributes.value(QLatin1String("priority")).toInt(&ok);
                if (ok) rank = std::clamp(priority, 1, 999999);
            } else if (attributes.hasAttribute(QLatin1String("preference"))) {  // Metalink 3: 100 is the best
                const int preference = attributes.value(QLatin1String("preference")).toInt(&ok);
                if (ok) rank = 1000 - std::clamp(preference, 0, 100);
            }
            const QString url = reader.readElementText().trimmed();
            if (isHttp(url) && !url.contains(QLatin1Char(' ')) && url.size() < 8192) links << Link{url, rank, linkOrder++};
        }
    }

    if (reader.hasError() && result.files.isEmpty()) {
        result.error = QStringLiteral("Couldn't read the Metalink: ") + reader.errorString();
    } else if (!sawMetalink) {
        result.error = QStringLiteral("That isn't a Metalink file");
    } else if (result.files.isEmpty()) {
        result.error = QStringLiteral("The Metalink has no files with http(s) links");
    }
    return result;
}

}
