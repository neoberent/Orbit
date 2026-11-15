#include "FeedWatcher.hpp"

#include "core/DownloadManager.hpp"
#include "utils/FileUtils.hpp"
#include "utils/Logger.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QUrl>
#include <QXmlStreamReader>

namespace {

constexpr int kCheckIntervalMs = 30 * 60 * 1000;
constexpr int kMaxSeen = 1000;
constexpr int kMaxFeeds = 50;
constexpr std::size_t kMaxFeedBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxTorrentBytes = 16 * 1024 * 1024;

bool isHttp(const QString& text) {
    return text.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) ||
           text.startsWith(QLatin1String("https://"), Qt::CaseInsensitive);
}

bool isTorrentLink(const QString& text) {
    if (text.startsWith(QLatin1String("magnet:?"), Qt::CaseInsensitive)) return true;
    if (!isHttp(text)) return false;
    return QUrl(text).path().endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive);
}

}

FeedWatcher::FeedWatcher(orbit::core::DownloadManager& manager, QObject* parent) : QObject(parent), manager_(manager) {
    load();
    timer_.setInterval(kCheckIntervalMs);
    connect(&timer_, &QTimer::timeout, this, &FeedWatcher::checkNow);
    timer_.start();
    // A first look shortly after start, once the network is up.
    QTimer::singleShot(20000, this, &FeedWatcher::checkNow);
}

QVariantList FeedWatcher::feeds() const {
    QVariantList list;
    for (const Feed& f : feeds_) {
        list.append(QVariantMap{{"url", f.url}, {"name", f.name.isEmpty() ? QUrl(f.url).host() : f.name},
                                {"filter", f.filter}, {"directory", f.directory}, {"enabled", f.enabled},
                                {"lastChecked", f.lastChecked}, {"lastError", f.lastError}, {"matched", f.matched}});
    }
    return list;
}

QList<FeedWatcher::Item> FeedWatcher::parse(const QByteArray& xml, QString* title) {
    QList<Item> items;
    QXmlStreamReader reader(xml);
    Item current;
    bool inItem = false;
    int depth = 0;
    while (!reader.atEnd() && !reader.hasError()) {
        reader.readNext();
        if (reader.isStartElement()) {
            ++depth;
            const QString name = reader.name().toString().toLower();
            if (name == QLatin1String("item") || name == QLatin1String("entry")) {
                inItem = true;
                current = {};
                continue;
            }
            if (name == QLatin1String("enclosure") || (name == QLatin1String("link") && reader.attributes().hasAttribute(QStringLiteral("href")))) {
                const QString link = reader.attributes().value(name == QLatin1String("enclosure") ? QStringLiteral("url") : QStringLiteral("href")).toString().trimmed();
                if (inItem && isTorrentLink(link) && (current.torrent.isEmpty() || link.startsWith(QLatin1String("magnet:")))) current.torrent = link;
                if (inItem && name == QLatin1String("link") && current.id.isEmpty()) current.id = link;
                continue;
            }
            if (name == QLatin1String("title") || name == QLatin1String("link") || name == QLatin1String("guid") ||
                name == QLatin1String("id") || name.contains(QLatin1String("magnet"))) {
                const QString text = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                --depth;
                if (!inItem) {
                    if (name == QLatin1String("title") && title && title->isEmpty()) *title = text.left(120);
                    continue;
                }
                if (name == QLatin1String("title")) current.title = text.left(500);
                else if (name == QLatin1String("guid") || name == QLatin1String("id")) current.id = text.left(1000);
                if ((name == QLatin1String("link") || name.contains(QLatin1String("magnet"))) && isTorrentLink(text) &&
                    (current.torrent.isEmpty() || text.startsWith(QLatin1String("magnet:")))) {
                    current.torrent = text;
                }
                if (name == QLatin1String("link") && current.id.isEmpty()) current.id = text.left(1000);
            }
        } else if (reader.isEndElement()) {
            --depth;
            const QString name = reader.name().toString().toLower();
            if (inItem && (name == QLatin1String("item") || name == QLatin1String("entry"))) {
                inItem = false;
                if (current.id.isEmpty()) current.id = current.torrent;
                if (!current.torrent.isEmpty() && !current.id.isEmpty()) items.append(current);
                if (items.size() >= 500) break;
            }
        }
    }
    return items;
}

bool FeedWatcher::matches(const QString& filter, const QString& title) {
    const QString f = filter.trimmed();
    if (f.isEmpty()) return true;
    if (f.size() > 2 && f.startsWith(QLatin1Char('/')) && f.endsWith(QLatin1Char('/'))) {
        const QRegularExpression re(f.mid(1, f.size() - 2), QRegularExpression::CaseInsensitiveOption);
        return re.isValid() && re.match(title).hasMatch();
    }
    for (const QString& word : f.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (word.startsWith(QLatin1Char('-')) && word.size() > 1) {
            if (title.contains(word.mid(1), Qt::CaseInsensitive)) return false;  // "-cam" excludes
        } else if (!title.contains(word, Qt::CaseInsensitive)) {
            return false;
        }
    }
    return true;
}

QString FeedWatcher::addFeed(const QString& rawUrl, const QString& filter, const QString& directory) {
    const QString url = rawUrl.trimmed();
    if (!isHttp(url)) return QStringLiteral("Enter an http:// or https:// feed address");
    if (find(url)) return QStringLiteral("That feed is already there");
    if (feeds_.size() >= kMaxFeeds) return QStringLiteral("That's the most feeds Orbit keeps");
    const QString f = filter.trimmed();
    if (f.size() > 2 && f.startsWith(QLatin1Char('/')) && f.endsWith(QLatin1Char('/')) &&
        !QRegularExpression(f.mid(1, f.size() - 2)).isValid()) {
        return QStringLiteral("The filter isn't a valid regular expression");
    }
    Feed feed;
    feed.url = url;
    feed.filter = f.left(300);
    feed.directory = QDir::isAbsolutePath(directory) ? QDir::cleanPath(directory) : QString();
    feeds_.append(feed);
    save();
    emit feedsChanged();
    ORBIT_LOG_INFO("feeds", "Added a feed");
    check(url);
    return {};
}

void FeedWatcher::removeFeed(const QString& url) {
    for (int i = 0; i < feeds_.size(); ++i) {
        if (feeds_[i].url != url) continue;
        feeds_.removeAt(i);
        save();
        emit feedsChanged();
        return;
    }
}

void FeedWatcher::setFeedEnabled(const QString& url, bool enabled) {
    if (Feed* f = find(url); f && f->enabled != enabled) {
        f->enabled = enabled;
        save();
        emit feedsChanged();
    }
}

void FeedWatcher::checkNow() {
    for (const Feed& f : feeds_) {
        if (f.enabled) check(f.url);
    }
}

void FeedWatcher::check(const QString& url) {
    QPointer<FeedWatcher> self(this);
    manager_.fetchSmall(url.toStdString(), kMaxFeedBytes, [self, url](bool ok, const std::string& body, const std::string& error) {
        const QByteArray data = QByteArray::fromStdString(body);
        const QString message = QString::fromStdString(error);
        QMetaObject::invokeMethod(qApp, [self, url, ok, data, message] {
            if (self) self->handleFeed(url, ok, data, message);
        }, Qt::QueuedConnection);
    });
}

void FeedWatcher::handleFeed(const QString& url, bool ok, const QByteArray& body, const QString& error) {
    Feed* feed = find(url);
    if (!feed) return;
    feed->lastChecked = QDateTime::currentSecsSinceEpoch();
    if (!ok) {
        feed->lastError = error;
        ORBIT_LOG_WARN("feeds", "Couldn't check a feed: " + error.toStdString());
        save();
        emit feedsChanged();
        return;
    }
    QString title;
    const QList<Item> items = parse(body, &title);
    feed->lastError = items.isEmpty() && title.isEmpty() ? QStringLiteral("Not an RSS or Atom feed") : QString();
    if (!title.isEmpty()) feed->name = title;

    for (const Item& item : items) {
        if (feed->seen.contains(item.id)) continue;
        feed->seen.prepend(item.id);
        if (!feed->primed) continue;  // Present when the feed was added: only remembered.
        if (!matches(feed->filter, item.title)) continue;
        ++feed->matched;
        startItem(*feed, item);
    }
    while (feed->seen.size() > kMaxSeen) feed->seen.removeLast();
    feed->primed = true;
    save();
    emit feedsChanged();
}

void FeedWatcher::startItem(const Feed& feed, const Item& item) {
    const QString feedName = feed.name.isEmpty() ? QUrl(feed.url).host() : feed.name;
    orbit::core::DownloadRequest request;
    request.destinationDir = feed.directory.isEmpty() ? manager_.defaultDownloadDir().string() : feed.directory.toStdString();
    request.maxConnections = 8;
    if (item.torrent.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive)) {
        request.source = item.torrent.toStdString();
        manager_.addTorrent(std::move(request));
        ORBIT_LOG_INFO("feeds", "Added a magnet link from a feed");
        emit itemAdded(feedName, item.title);
        return;
    }
    // A .torrent link: fetch the file (through the same route as downloads), then add it.
    QPointer<FeedWatcher> self(this);
    const QString itemTitle = item.title;
    manager_.fetchSmall(item.torrent.toStdString(), kMaxTorrentBytes,
                        [self, request, feedName, itemTitle](bool ok, const std::string& body, const std::string& error) mutable {
        QMetaObject::invokeMethod(qApp, [self, request, feedName, itemTitle, ok, body, error]() mutable {
            if (!self) return;
            if (!ok || body.empty() || body.front() != 'd') {  // a .torrent is a bencoded dictionary
                ORBIT_LOG_WARN("feeds", "Couldn't fetch a feed's .torrent: " + (ok ? std::string("not a torrent file") : error));
                return;
            }
            const QString dir = QString::fromStdWString((orbit::utils::appDataDir() / "feeds").wstring());
            QDir().mkpath(dir);
            const QString path = dir + QLatin1Char('/') +
                                 QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromStdString(body), QCryptographicHash::Sha1).toHex()) +
                                 QStringLiteral(".torrent");
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
            file.write(body.data(), static_cast<qint64>(body.size()));
            file.close();
            request.source = path.toStdString();
            self->manager_.addTorrent(std::move(request));
            ORBIT_LOG_INFO("feeds", "Added a .torrent from a feed");
            emit self->itemAdded(feedName, itemTitle);
        }, Qt::QueuedConnection);
    });
}

FeedWatcher::Feed* FeedWatcher::find(const QString& url) {
    for (Feed& f : feeds_) {
        if (f.url == url) return &f;
    }
    return nullptr;
}

void FeedWatcher::load() {
    const QJsonArray list = QJsonDocument::fromJson(QSettings().value("feeds/list").toByteArray()).array();
    for (const QJsonValue& value : list) {
        const QJsonObject o = value.toObject();
        Feed f;
        f.url = o.value("url").toString();
        if (!isHttp(f.url) || find(f.url) || feeds_.size() >= kMaxFeeds) continue;
        f.name = o.value("name").toString().left(120);
        f.filter = o.value("filter").toString().left(300);
        f.directory = o.value("directory").toString();
        if (!f.directory.isEmpty() && !QDir::isAbsolutePath(f.directory)) f.directory.clear();
        f.enabled = o.value("enabled").toBool(true);
        f.primed = o.value("primed").toBool(false);
        f.lastChecked = static_cast<qint64>(o.value("lastChecked").toDouble());
        f.matched = o.value("matched").toInt();
        for (const QJsonValue& s : o.value("seen").toArray()) {
            if (f.seen.size() < kMaxSeen) f.seen.append(s.toString());
        }
        feeds_.append(f);
    }
}

void FeedWatcher::save() const {
    QJsonArray list;
    for (const Feed& f : feeds_) {
        list.append(QJsonObject{{"url", f.url}, {"name", f.name}, {"filter", f.filter}, {"directory", f.directory},
                                {"enabled", f.enabled}, {"primed", f.primed}, {"lastChecked", static_cast<double>(f.lastChecked)},
                                {"matched", f.matched}, {"seen", QJsonArray::fromStringList(f.seen)}});
    }
    QSettings().setValue("feeds/list", QJsonDocument(list).toJson(QJsonDocument::Compact));
}
