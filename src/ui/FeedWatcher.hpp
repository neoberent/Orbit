#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

namespace orbit::core {
class DownloadManager;
}

// Torrent RSS/Atom feeds: checked every 30 minutes, and each new item whose title matches
// the feed's filter is downloaded (a magnet link directly, a .torrent link fetched first).
// Items already in the feed when it's added count as seen, so adding a feed never starts
// a flood of old downloads.
class FeedWatcher final : public QObject {
    Q_OBJECT
    // [{url, name, filter, directory, enabled, lastChecked, lastError, matched}]
    Q_PROPERTY(QVariantList feeds READ feeds NOTIFY feedsChanged)

public:
    struct Item {
        QString id;       // guid, or the link
        QString title;
        QString torrent;  // magnet: or http(s) link to a .torrent file
    };

    explicit FeedWatcher(orbit::core::DownloadManager& manager, QObject* parent = nullptr);

    [[nodiscard]] QVariantList feeds() const;

    // Returns an error message, or an empty string.
    Q_INVOKABLE QString addFeed(const QString& url, const QString& filter, const QString& directory);
    Q_INVOKABLE void removeFeed(const QString& url);
    Q_INVOKABLE void setFeedEnabled(const QString& url, bool enabled);
    Q_INVOKABLE void checkNow();

    // RSS 2.0 or Atom; exposed for the tests.
    static QList<Item> parse(const QByteArray& xml, QString* title = nullptr);
    // A filter is words that must all appear (case-insensitive), or /a regex/.
    static bool matches(const QString& filter, const QString& title);

signals:
    void feedsChanged();
    // A matching item was added as a download.
    void itemAdded(const QString& feed, const QString& title);

private:
    struct Feed {
        QString url;
        QString name;
        QString filter;
        QString directory;
        bool enabled{true};
        bool primed{false};  // the items present when it was added are marked seen
        qint64 lastChecked{0};
        QString lastError;
        int matched{0};
        QStringList seen;
    };

    void load();
    void save() const;
    void check(const QString& url);
    void handleFeed(const QString& url, bool ok, const QByteArray& body, const QString& error);
    void startItem(const Feed& feed, const Item& item);
    Feed* find(const QString& url);

    orbit::core::DownloadManager& manager_;
    QList<Feed> feeds_;
    QTimer timer_;
};
