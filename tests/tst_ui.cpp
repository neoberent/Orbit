// The app-side services that talk to other programs: the aria2-compatible RPC interface
// and the torrent feed watcher.
#include "Aria2RpcServer.hpp"
#include "FeedWatcher.hpp"
#include "TestHttpServer.hpp"
#include "core/DownloadManager.hpp"
#include "utils/FileUtils.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <fstream>

namespace fs = std::filesystem;
using namespace orbit::core;
using orbit::tests::TestHttpServer;

namespace {

const QString kSecret = QStringLiteral("0123456789abcdef0123");

quint16 freePort() {
    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost, 0);
    return probe.serverPort();
}

QJsonObject call(Aria2RpcServer& rpc, const QString& method, QJsonArray params = {}, bool withToken = true) {
    if (withToken) params.prepend(QStringLiteral("token:") + kSecret);
    return rpc.handleCall(QJsonObject{{"jsonrpc", "2.0"}, {"id", "t"}, {"method", method}, {"params", params}});
}

QString errorOf(const QJsonObject& reply) { return reply.value("error").toObject().value("message").toString(); }

template <typename Pred>
bool waitFor(Pred pred, int ms = 20000) {
    QElapsedTimer timer;
    timer.start();
    while (!pred() && timer.elapsed() < ms) QTest::qWait(20);
    return pred();
}

}

class TestUi : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> appData_;
    std::unique_ptr<QTemporaryDir> downloads_;
    std::unique_ptr<DownloadManager> manager_;
    std::unique_ptr<TestHttpServer> http_;
    QStringList lastLinks_;

    std::unique_ptr<Aria2RpcServer> makeRpc() {
        return std::make_unique<Aria2RpcServer>(*manager_, [this](const QStringList& links, const QString&, QString*) -> qulonglong {
            lastLinks_ = links;
            DownloadRequest request;
            request.source = links.value(0).toStdString();
            request.suggestedName = "via-aria2.bin";
            return manager_->addHttpDownload(request);
        });
    }

    // One raw HTTP exchange with the aria2 server (it runs on this thread's event loop).
    QByteArray post(quint16 port, const QByteArray& host, const QByteArray& body) {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, port);
        if (!waitFor([&] { return socket.state() == QAbstractSocket::ConnectedState; }, 3000)) return {};
        socket.write("POST /jsonrpc HTTP/1.1\r\nHost: " + host + "\r\nContent-Type: application/json\r\nContent-Length: " +
                     QByteArray::number(body.size()) + "\r\n\r\n" + body);
        QByteArray raw;
        waitFor([&] {
            raw += socket.readAll();
            return socket.state() == QAbstractSocket::UnconnectedState;
        }, 5000);
        return raw + socket.readAll();
    }

private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName(QStringLiteral("OrbitTest"));
        QCoreApplication::setApplicationName(QStringLiteral("tst_ui"));
        appData_ = std::make_unique<QTemporaryDir>();
        downloads_ = std::make_unique<QTemporaryDir>();
        orbit::utils::setAppDataDirOverride(appData_->path().toStdString());
        manager_ = std::make_unique<DownloadManager>(fs::path(appData_->path().toStdString()) / "tasks");
        manager_->setDefaultDownloadDir(downloads_->path().toStdString());
        http_ = std::make_unique<TestHttpServer>();
    }

    void cleanupTestCase() {
        manager_.reset();
        http_.reset();
    }

    // --- aria2 ---------------------------------------------------------------------------

    void aria2NeedsTheTokenAndLocksOutGuessing() {
        auto rpc = makeRpc();
        QVERIFY(rpc->start(freePort(), kSecret).isEmpty());
        // Listing the methods needs no token, as in aria2.
        QVERIFY(call(*rpc, "system.listMethods", {}, false).value("result").toArray().contains("aria2.addUri"));
        QCOMPARE(errorOf(call(*rpc, "aria2.getGlobalStat", {}, false)), QStringLiteral("Unauthorized"));
        QVERIFY(call(*rpc, "aria2.getGlobalStat").contains("result"));
        for (int i = 0; i < 10; ++i) call(*rpc, "aria2.getGlobalStat", {QStringLiteral("token:wrong")}, false);
        // Locked for a minute now, even for the right token.
        QVERIFY(errorOf(call(*rpc, "aria2.getGlobalStat")).contains("Too many wrong tokens"));
        // multicall checks each inner call's own token (a fresh server: no lockout).
        auto fresh = makeRpc();
        QVERIFY(fresh->start(freePort(), kSecret).isEmpty());
        const QJsonArray inner{QJsonObject{{"methodName", "aria2.getVersion"}, {"params", QJsonArray{QStringLiteral("token:") + kSecret}}},
                               QJsonObject{{"methodName", "aria2.getVersion"}, {"params", QJsonArray{}}}};
        const QJsonArray results = call(*fresh, "system.multicall", {inner}, false).value("result").toArray();
        QCOMPARE(results.size(), 2);
        QVERIFY(results.at(0).isArray());
        QCOMPARE(results.at(1).toObject().value("message").toString(), QStringLiteral("Unauthorized"));
    }

    void aria2RefusesShortSecretsAndForeignHosts() {
        auto rpc = makeRpc();
        QVERIFY(!rpc->start(freePort(), QStringLiteral("short")).isEmpty());
        const quint16 port = freePort();
        QVERIFY(rpc->start(port, kSecret).isEmpty());
        const QByteArray body = QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "aria2.getVersion"},
                                                          {"params", QJsonArray{QStringLiteral("token:") + kSecret}}})
                                    .toJson(QJsonDocument::Compact);
        // A web page reaching 127.0.0.1 through its own domain (DNS rebinding) is refused.
        QVERIFY(post(port, "attacker.example:" + QByteArray::number(port), body).startsWith("HTTP/1.1 403"));
        const QByteArray ok = post(port, "127.0.0.1:" + QByteArray::number(port), body);
        QVERIFY2(ok.startsWith("HTTP/1.1 200"), ok.constData());
        QVERIFY(ok.contains("\"version\""));
    }

    void aria2AddsDownloadsAndReportsThem() {
        const QByteArray payload(300 * 1024, 'z');
        http_->setBehaviour({.payload = payload});
        auto rpc = makeRpc();
        QVERIFY(rpc->start(freePort(), kSecret).isEmpty());
        const QString gid = call(*rpc, "aria2.addUri", {QJsonArray{QString::fromStdString(http_->url()), "https://mirror.example/f.bin"}})
                                .value("result").toString();
        QCOMPARE(gid.size(), 16);
        // Every URI arrives: the first is the download, the rest become its mirrors.
        QCOMPARE(lastLinks_, (QStringList{QString::fromStdString(http_->url()), "https://mirror.example/f.bin"}));
        QVERIFY(waitFor([&] {
            return call(*rpc, "aria2.tellStatus", {gid, QJsonArray{"status"}}).value("result").toObject().value("status") ==
                   QLatin1String("complete");
        }));
        const QJsonObject status = call(*rpc, "aria2.tellStatus", {gid}).value("result").toObject();
        QCOMPARE(status.value("totalLength").toString(), QString::number(payload.size()));
        QVERIFY(call(*rpc, "aria2.tellStopped", {0, 100}).value("result").toArray().size() >= 1);
        QVERIFY(errorOf(call(*rpc, "aria2.tellStatus", {QStringLiteral("00000000000000ff")})).contains("not found"));

        // Removing a finished download through aria2 never touches its file.
        const fs::path file = fs::path(downloads_->path().toStdString()) / "via-aria2.bin";
        QVERIFY(fs::exists(file));
        call(*rpc, "aria2.remove", {gid});
        QTest::qWait(300);
        QVERIFY(fs::exists(file));
        call(*rpc, "aria2.removeDownloadResult", {gid});
        QVERIFY(waitFor([&] { return !manager_->snapshot(QString(gid).toULongLong(nullptr, 16)).has_value(); }));
        QVERIFY(fs::exists(file));
    }

    // --- feeds ---------------------------------------------------------------------------

    void feedsReadRssAndAtom() {
        // (Plain strings with single-quoted attributes: raw strings trip up moc.)
        const QByteArray rss =
            "<?xml version='1.0'?><rss><channel><title>Linux ISOs</title>"
            "<item><title>Distro 1.0</title><guid>a1</guid>"
            "<enclosure url='https://x.example/d1.torrent' type='application/x-bittorrent'/></item>"
            "<item><title>Distro 2.0</title><link>magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567</link></item>"
            "<item><title>Just a news post</title><link>https://x.example/news</link></item>"
            "</channel></rss>";
        QString title;
        const auto items = FeedWatcher::parse(rss, &title);
        QCOMPARE(title, QStringLiteral("Linux ISOs"));
        QCOMPARE(items.size(), 2);  // the news post has nothing to download
        QCOMPARE(items[0].torrent, QStringLiteral("https://x.example/d1.torrent"));
        QCOMPARE(items[0].id, QStringLiteral("a1"));
        QVERIFY(items[1].torrent.startsWith("magnet:?"));

        const QByteArray atom = "<feed xmlns='http://www.w3.org/2005/Atom'><title>Atom</title>"
                                "<entry><title>One</title><id>e1</id><link href='https://y.example/one.torrent'/></entry></feed>";
        const auto entries = FeedWatcher::parse(atom);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries[0].torrent, QStringLiteral("https://y.example/one.torrent"));
        // Not a feed at all: nothing, and no crash.
        QVERIFY(FeedWatcher::parse("<html><body>hi").isEmpty());
    }

    void feedFiltersMatchLikeTheyRead() {
        QVERIFY(FeedWatcher::matches({}, "Anything"));
        QVERIFY(FeedWatcher::matches("show 1080p", "The Show S01E02 1080p WEB"));
        QVERIFY(!FeedWatcher::matches("show 1080p", "The Show S01E02 720p"));
        QVERIFY(!FeedWatcher::matches("show -cam", "The Show CAM"));
        QVERIFY(FeedWatcher::matches("/S01E0[1-3]/", "show s01e02"));
        QVERIFY(!FeedWatcher::matches("/([/", "anything"));  // a broken regex matches nothing
    }

    // Items already in a feed when it's added are only remembered; later ones that match
    // the filter download by themselves.
    void feedsDownloadOnlyNewMatchingItems() {
        QSettings().remove("feeds");
        FeedWatcher feeds(*manager_);
        QVERIFY(!feeds.addFeed("ftp://x.example/feed", {}, {}).isEmpty());
        QVERIFY(!feeds.addFeed(QString::fromStdString(http_->url()), "/([/", {}).isEmpty());

        auto rssWith = [](const QByteArray& items) {
            return "<rss><channel><title>T</title>" + items + "</channel></rss>";
        };
        const QByteArray old = "<item><title>Old wanted</title><link>magnet:?xt=urn:btih:1111111111111111111111111111111111111111</link></item>";
        http_->setBehaviour({.payload = rssWith(old)});
        const auto before = manager_->snapshot().size();
        QVERIFY(feeds.addFeed(QString::fromStdString(http_->url()), "wanted", {}).isEmpty());
        QVERIFY(waitFor([&] { return feeds.feeds().value(0).toMap().value("lastChecked").toLongLong() > 0; }));
        QTest::qWait(300);
        QCOMPARE(manager_->snapshot().size(), before);  // present at the start: not downloaded

        http_->setBehaviour({.payload = rssWith(old +
            "<item><title>New wanted</title><link>magnet:?xt=urn:btih:2222222222222222222222222222222222222222</link></item>"
            "<item><title>New other</title><link>magnet:?xt=urn:btih:3333333333333333333333333333333333333333</link></item>")});
        feeds.checkNow();
        QVERIFY(waitFor([&] { return feeds.feeds().value(0).toMap().value("matched").toInt() == 1; }));
        QCOMPARE(manager_->snapshot().size(), before + 1);
        const auto added = manager_->snapshot().back();
        QCOMPARE(added.type, DownloadType::Torrent);
        QVERIFY(added.source.find("2222222222") != std::string::npos);
        manager_->cancel(added.id);
        feeds.removeFeed(QString::fromStdString(http_->url()));
        QVERIFY(feeds.feeds().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestUi)
#include "tst_ui.moc"
