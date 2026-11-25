// The app-side services that talk to other programs: the aria2-compatible RPC interface
// and the torrent feed watcher.
#include "Aria2RpcServer.hpp"
#include "FeedWatcher.hpp"
#include "FileChecks.hpp"
#include "Metalink.hpp"
#include "SignatureCheck.hpp"
#include "TestHttpServer.hpp"
#include "core/DownloadManager.hpp"
#include "utils/FileUtils.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
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

// minisign fixtures made by an independent implementation of the format (Python's
// cryptography + hashlib.blake2b), from the key with seed bytes 0..31 and key ID
// 1122334455667788, over kMinisignData.
const QString kMinisignKey = QStringLiteral("RWQRIjNEVWZ3iAOhB7/zzhC+HXDdGOdLwJln5NYwm6UNXx3chmQSVTG4");
QByteArray minisignData() { return QByteArray("Orbit minisign fixture\n").repeated(1000); }
const QByteArray kMinisigHashed =
    "untrusted comment: signature from minisign secret key\n"
    "RUQRIjNEVWZ3iDfDQuDyHgVH2T8EBP/MLgkEcBMH9mIhMWIXYux8b5983zgOGvQ9++Jmrsm9bN3hKO7YzrbBJKatOg8Gm3t+mww=\n"
    "trusted comment: timestamp:1700000000\tfile:fixture.iso\thashed\n"
    "sSJ/TPSwMUvxOjG1Je4razsZpuuAidLKvWOgnG8KJ/2GgmcF62tdHPwr9VbmLDfw14hc5a1XKi1fDt44eqtDBQ==\n";
const QByteArray kMinisigLegacy =
    "untrusted comment: signature from minisign secret key\n"
    "RWQRIjNEVWZ3iIlLcJ3TP7FaI17UqRtDye0AX9NsM02ReL8/zBC+zqlFWAgxYmqRkDdceNgfCsrqzDsxja7/ILcGiYSZ0/IbZQU=\n"
    "trusted comment: legacy\n"
    "7B3z/7Sfbt+qy3Zbc1zRtLoatPqS9neG6WO2nZvo+y5dqh5DuM8/srpMa6OgH0ejTsfPmp9FP9+vs0sdG7huBg==\n";

QString writeFile(const QString& path, const QByteArray& data) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    file.write(data);
    return path;
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

    // Downloads a path from the test server and waits until it has finished.
    DownloadId downloadAndWait(const std::string& path, const std::string& name) {
        DownloadRequest request;
        request.source = http_->url(path);
        request.suggestedName = name;
        const DownloadId id = manager_->addHttpDownload(request);
        if (!waitFor([&] {
                const auto snap = manager_->snapshot(id);
                return snap && snap->state == DownloadState::Completed;
            })) {
            return 0;
        }
        return id;
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

    // --- Metalink ------------------------------------------------------------------------

    void metalink4PicksTheBestLinksAndHash() {
        const QByteArray xml =
            "<?xml version='1.0' encoding='UTF-8'?>"
            "<metalink xmlns='urn:ietf:params:xml:ns:metalink'>"
            " <file name='../../.bashrc'>"
            "  <size>14471447</size>"
            "  <hash type='md5'>0123456789abcdef0123456789abcdef</hash>"
            "  <hash type='sha-256'>F0AD929CD259957E160EA442EB80986B5F01DD54D6B9D6F2F8A7E3B5A2E1D4C3</hash>"
            "  <hash type='sha-256'>not-hex</hash>"
            "  <url priority='2'>http://mirror-b.example/x.iso</url>"
            "  <url priority='1' location='de'>https://mirror-a.example/x.iso</url>"
            "  <url>ftp://old.example/x.iso</url>"
            "  <metaurl mediatype='torrent'>https://example.com/x.torrent</metaurl>"
            "  <url priority='3'>https://mirror-a.example/x.iso</url>"
            " </file>"
            " <file name='no-links.bin'><url>ftp://only.example/f</url></file>"
            " <file name='second.img'><url>https://c.example/second.img</url></file>"
            "</metalink>";
        const metalink::Result result = metalink::parse(xml);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.files.size(), 2);  // the one without http(s) links is left out
        const metalink::File& first = result.files.at(0);
        QCOMPARE(first.name, QStringLiteral(".bashrc"));  // never a path out of the folder
        QCOMPARE(first.size, qint64(14471447));
        QCOMPARE(first.checksum, QStringLiteral("sha256:f0ad929cd259957e160ea442eb80986b5f01dd54d6b9d6f2f8a7e3b5a2e1d4c3"));
        QCOMPARE(first.urls, (QStringList{"https://mirror-a.example/x.iso", "http://mirror-b.example/x.iso"}));
        QCOMPARE(result.files.at(1).checksum, QString());

        QVERIFY(metalink::looksLikeMetalink("https://example.com/ubuntu.meta4?x=1"));
        QVERIFY(metalink::looksLikeMetalink("/home/u/f.METALINK"));
        QVERIFY(!metalink::looksLikeMetalink("https://example.com/f.meta4.iso"));
    }

    void metalink3AndThingsThatArentMetalinks() {
        const QByteArray v3 =
            "<?xml version='1.0'?>"
            "<metalink version='3.0' xmlns='http://www.metalinker.org/'><files>"
            " <file name='dir/app.tar.gz'><size>10</size>"
            "  <verification>"
            "   <hash type='sha1'>0123456789abcdef0123456789abcdef01234567</hash>"
            "   <pieces length='262144' type='sha1'><hash piece='0'>89abcdef0123456789abcdef0123456789abcdef</hash></pieces>"
            "  </verification>"
            "  <resources>"
            "   <url type='http' preference='10'>http://slow.example/app.tar.gz</url>"
            "   <url type='http' preference='100'>https://fast.example/app.tar.gz</url>"
            "  </resources>"
            " </file>"
            "</files></metalink>";
        const metalink::Result result = metalink::parse(v3);
        QCOMPARE(result.files.size(), 1);
        QCOMPARE(result.files.at(0).name, QStringLiteral("app.tar.gz"));
        QCOMPARE(result.files.at(0).checksum, QStringLiteral("sha1:0123456789abcdef0123456789abcdef01234567"));
        QCOMPARE(result.files.at(0).urls.first(), QStringLiteral("https://fast.example/app.tar.gz"));

        QVERIFY(!metalink::parse("<html><body>404</body></html>").error.isEmpty());
        QVERIFY(!metalink::parse("not xml at all").error.isEmpty());
        QVERIFY(!metalink::parse("<metalink xmlns='urn:ietf:params:xml:ns:metalink'></metalink>").error.isEmpty());
        // Entity expansion bombs are refused by the reader.
        QByteArray bomb = "<?xml version='1.0'?><!DOCTYPE m [<!ENTITY a 'aaaaaaaaaa'>";
        for (int i = 1; i < 9; ++i) {
            bomb += "<!ENTITY " + QByteArray(1, char('a' + i)) + " '" + QByteArray("&") + QByteArray(1, char('a' + i - 1)) + ";";
            bomb += QByteArray("&") + QByteArray(1, char('a' + i - 1)) + ";" + "&" + QByteArray(1, char('a' + i - 1)) + ";'>";
        }
        bomb += "]><metalink><file name='&i;'><url>https://x.example/f</url></file></metalink>";
        const metalink::Result bombed = metalink::parse(bomb);
        QVERIFY(bombed.files.isEmpty() || bombed.files.at(0).name.size() <= 200);
    }

    void aria2AddsMetalinks() {
        auto rpc = makeRpc();
        QByteArray received;
        rpc->setAddMetalink([&](const QByteArray& xml, const QString&, QString*) {
            received = xml;
            return QList<qulonglong>{7, 9};
        });
        QVERIFY(rpc->start(freePort(), kSecret).isEmpty());
        const QByteArray doc = "<metalink xmlns='urn:ietf:params:xml:ns:metalink'/>";
        const QJsonArray gids = call(*rpc, "aria2.addMetalink", {QString::fromLatin1(doc.toBase64())}).value("result").toArray();
        QCOMPARE(received, doc);
        QCOMPARE(gids, (QJsonArray{"0000000000000007", "0000000000000009"}));
        QVERIFY(call(*rpc, "system.listMethods", {}, false).value("result").toArray().contains("aria2.addMetalink"));
    }

    // --- signatures ----------------------------------------------------------------------

    void gpgStatusLinesGiveTheRightVerdict() {
        using signature_check::State;
        auto parse = [](const char* text) { return signature_check::parseGpgStatus(QByteArray(text)); };
        auto good = parse("[GNUPG:] NEWSIG\n[GNUPG:] KEY_CONSIDERED ABCD 0\n"
                          "[GNUPG:] GOODSIG 1234ABCD5678EF00 Jane Doe %3Cjane@example.org%3E\n"
                          "[GNUPG:] VALIDSIG FPR 2024-01-01\n[GNUPG:] TRUST_UNDEFINED 0 pgp\n");
        QCOMPARE(good.state, State::Good);
        QCOMPARE(good.signer, QStringLiteral("Jane Doe <jane@example.org>"));
        QCOMPARE(good.keyId, QStringLiteral("1234ABCD5678EF00"));
        QCOMPARE(parse("[GNUPG:] BADSIG 1234 Jane\n").state, State::Bad);
        // A bad one among good ones makes the whole file bad.
        QCOMPARE(parse("[GNUPG:] GOODSIG 1 A\n[GNUPG:] BADSIG 2 B\n").state, State::Bad);
        auto missing = parse("[GNUPG:] ERRSIG 99AA88BB77CC66DD 22 10 00 1700000000 9 -\n[GNUPG:] NO_PUBKEY 99AA88BB77CC66DD\n");
        QCOMPARE(missing.state, State::UnknownKey);
        QCOMPARE(missing.keyId, QStringLiteral("99AA88BB77CC66DD"));
        QCOMPARE(parse("[GNUPG:] REVKEYSIG 1 A\n").state, State::Revoked);
        QCOMPARE(parse("[GNUPG:] EXPKEYSIG 1 A\n").state, State::Expired);
        QCOMPARE(parse("[GNUPG:] GOODSIG 1 A\n[GNUPG:] TRUST_NEVER 0\n").state, State::UnknownKey);
        QCOMPARE(parse("[GNUPG:] NODATA 1\n[GNUPG:] NODATA 2\n").state, State::NotASignature);
        QCOMPARE(parse("gpg: something odd\n").state, State::Failed);
    }

    void minisignMatchesAnIndependentSigner() {
        using signature_check::State;
        QTemporaryDir dir;
        const QString file = writeFile(dir.filePath("fixture.iso"), minisignData());
        QVERIFY(signature_check::looksLikeMinisign(kMinisigHashed));
        QVERIFY(!signature_check::looksLikeMinisign("-----BEGIN PGP SIGNATURE-----\n"));

        auto hashed = signature_check::verifyMinisign(file, kMinisigHashed, {kMinisignKey});
        QCOMPARE(hashed.state, State::Good);
        QCOMPARE(hashed.signer, QStringLiteral("timestamp:1700000000\tfile:fixture.iso\thashed"));
        QCOMPARE(hashed.keyId, QStringLiteral("8877665544332211"));
        QCOMPARE(signature_check::verifyMinisign(file, kMinisigLegacy, {kMinisignKey}).state, State::Good);

        // No key for it: known to be signed, not by whom.
        QCOMPARE(signature_check::verifyMinisign(file, kMinisigHashed, {}).state, State::UnknownKey);
        // A changed trusted comment or a changed file is caught.
        QByteArray comment = kMinisigHashed;
        comment.replace("hashed", "hashes");
        QCOMPARE(signature_check::verifyMinisign(file, comment, {kMinisignKey}).state, State::Bad);
        QByteArray changed = minisignData();
        changed[100] = 'X';
        writeFile(file, changed);
        QCOMPARE(signature_check::verifyMinisign(file, kMinisigHashed, {kMinisignKey}).state, State::Bad);
        QCOMPARE(signature_check::verifyMinisign(file, kMinisigLegacy, {kMinisignKey}).state, State::Bad);
        QCOMPARE(signature_check::verifyMinisign(file, "untrusted comment: x\nnope\n", {kMinisignKey}).state, State::NotASignature);

        QCOMPARE(signature_check::parseMinisignKeys("  junk " + kMinisignKey + ", RWshort " + kMinisignKey),
                 QStringList{kMinisignKey});
    }

    void clamavOutputNamesTheThreats() {
        const QByteArray output = "/home/u/Downloads/eicar.com: Win.Test.EICAR_HDB-1 FOUND\n"
                                  "/home/u/Downloads/b.zip: Eicar-Signature FOUND\n"
                                  "/home/u/Downloads/c: OK\n";
        QCOMPARE(FileChecks::threatsIn(output), (QStringList{"Win.Test.EICAR_HDB-1", "Eicar-Signature"}));
        QVERIFY(FileChecks::threatsIn("/x: OK\n").isEmpty());
    }

    // --- checks after a download ---------------------------------------------------------

    void checksVerifyAMinisignedRelease() {
        TestHttpServer::Behaviour b;
        b.payload = minisignData();
        b.payloadPath = "/fixture.iso";
        b.files.insert("/fixture.iso.minisig", kMinisigHashed);
        http_->setBehaviour(b);
        const DownloadId id = downloadAndWait("/fixture.iso", "fixture.iso");
        QVERIFY(id != 0);

        FileChecks checks(*manager_);
        checks.setSettings({.scan = false, .signatures = true, .duplicates = false, .minisignKeys = {kMinisignKey}});
        std::optional<bool> proceed;
        checks.check(static_cast<qlonglong>(id), [&](bool ok) { proceed = ok; });
        QVERIFY(waitFor([&] { return proceed.has_value(); }));
        QVERIFY(*proceed);
        QCOMPARE(checks.resultFor(id).value("signature").toString(), QStringLiteral("good"));
        QCOMPARE(checks.resultFor(id).value("signatureKind").toString(), QStringLiteral("minisign"));

        // A signature that doesn't match stops the after-download actions and says so.
        // (A changed signature byte; the key ID in front of it stays the same.)
        QByteArray tampered = kMinisigHashed;
        tampered[100] = tampered[100] == 'A' ? 'B' : 'A';
        b.files.insert("/fixture.iso.minisig", tampered);
        http_->setBehaviour(b);
        QSignalSpy notices(&checks, &FileChecks::notice);
        proceed.reset();
        checks.check(static_cast<qlonglong>(id), [&](bool ok) { proceed = ok; });
        QVERIFY(waitFor([&] { return proceed.has_value(); }));
        QVERIFY(!*proceed);
        QCOMPARE(checks.resultFor(id).value("signature").toString(), QStringLiteral("bad"));
        QCOMPARE(notices.size(), 1);
        QCOMPARE(notices.at(0).at(0).toString(), QStringLiteral("Bad signature"));

        manager_->remove(id, true);
    }

    void checksVerifyOpenPgpWithTheUsersKeyring() {
        const QString gpg = QStandardPaths::findExecutable("gpg");
        if (gpg.isEmpty()) QSKIP("gpg isn't installed");
        QTemporaryDir home(QDir::tempPath() + "/og-XXXXXX");  // short: gpg-agent's socket path
        QTemporaryDir stranger(QDir::tempPath() + "/og-XXXXXX");
        auto runGpg = [&](const QString& gnupgHome, const QStringList& args, const QByteArray& input = {}) {
            QProcess p;
            auto env = QProcessEnvironment::systemEnvironment();
            env.insert("GNUPGHOME", gnupgHome);
            p.setProcessEnvironment(env);
            p.start(gpg, QStringList{"--batch", "--no-tty", "--pinentry-mode", "loopback", "--passphrase", ""} + args);
            p.write(input);
            p.closeWriteChannel();
            p.waitForFinished(60000);
            return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0 ? std::optional<QByteArray>(p.readAllStandardOutput())
                                                                               : std::nullopt;
        };
        QVERIFY(runGpg(home.path(), {"--quick-gen-key", "Orbit Test <orbit-test@example.invalid>", "ed25519", "sign", "never"}));
        const QByteArray payload = QByteArray("signed release\n").repeated(5000);
        const QByteArray asc = runGpg(home.path(), {"--armor", "--detach-sign", "--output", "-"}, payload).value_or(QByteArray());
        QVERIFY(asc.startsWith("-----BEGIN PGP SIGNATURE-----"));

        TestHttpServer::Behaviour b;
        b.payload = payload;
        b.payloadPath = "/release.tar.gz";
        b.files.insert("/release.tar.gz.asc", asc);
        http_->setBehaviour(b);
        const DownloadId id = downloadAndWait("/release.tar.gz", "release.tar.gz");
        QVERIFY(id != 0);

        const QByteArray oldHome = qgetenv("GNUPGHOME");
        FileChecks checks(*manager_);
        checks.setSettings({.scan = false, .signatures = true, .duplicates = false, .minisignKeys = {}});
        auto run = [&](const QString& gnupgHome) {
            qputenv("GNUPGHOME", gnupgHome.toLocal8Bit());
            std::optional<bool> proceed;
            checks.check(static_cast<qlonglong>(id), [&](bool ok) { proceed = ok; });
            waitFor([&] { return proceed.has_value(); }, 60000);
            return proceed;
        };
        const auto withKey = run(home.path());
        QVERIFY(withKey && *withKey);
        QCOMPARE(checks.resultFor(id).value("signature").toString(), QStringLiteral("good"));
        QCOMPARE(checks.resultFor(id).value("signer").toString(), QStringLiteral("Orbit Test <orbit-test@example.invalid>"));
        // Someone without the key learns it's signed, by which key, and nothing more.
        const auto withoutKey = run(stranger.path());
        QVERIFY(withoutKey && *withoutKey);
        QCOMPARE(checks.resultFor(id).value("signature").toString(), QStringLiteral("unknown-key"));
        QVERIFY(!checks.resultFor(id).value("keyId").toString().isEmpty());
        if (oldHome.isNull()) qunsetenv("GNUPGHOME");
        else qputenv("GNUPGHOME", oldHome);
        for (const QString& h : {home.path(), stranger.path()}) {
            QProcess::execute("gpgconf", {"--homedir", h, "--kill", "all"});
        }
        manager_->remove(id, true);
    }

    void checksSpotDuplicates() {
        const QByteArray payload = QByteArray("duplicate me ").repeated(40000);
        TestHttpServer::Behaviour b;
        b.payload = payload;
        http_->setBehaviour(b);
        const DownloadId first = downloadAndWait("/dup.bin", "dup.bin");
        QVERIFY(first != 0);
        FileChecks checks(*manager_);
        checks.setSettings({.scan = false, .signatures = true, .duplicates = true, .minisignKeys = {}});
        QSignalSpy found(&checks, &FileChecks::duplicateFound);
        checks.check(static_cast<qlonglong>(first), {});
        QTest::qWait(500);
        QCOMPARE(found.size(), 0);  // the only copy

        // A half-finished download of the same size beside it isn't a copy yet.
        const QString partial = writeFile(QString::fromStdString(manager_->defaultDownloadDir().string()) + "/partial.bin", payload);
        writeFile(partial + ".orbitresume", "{}");
        const DownloadId second = downloadAndWait("/dup.bin", "dup.bin");  // lands as "dup (1).bin"
        QVERIFY(second != 0);
        bool proceeded = false;
        checks.check(static_cast<qlonglong>(second), [&](bool ok) { proceeded = ok; });
        QVERIFY(waitFor([&] { return found.size() == 1; }));
        QVERIFY(proceeded);  // a duplicate doesn't stop anything
        QCOMPARE(found.at(0).at(0).toLongLong(), static_cast<qlonglong>(second));
        const QString original = found.at(0).at(2).toString();
        QCOMPARE(original, QString::fromStdString(manager_->snapshot(first)->destinationPath));
        QCOMPARE(checks.resultFor(second).value("duplicateOf").toString(), original);

        // A file the user already had in the folder counts too.
        manager_->remove(first, false);
        QFile::remove(partial);
        QFile::remove(partial + ".orbitresume");
        found.clear();
        checks.check(static_cast<qlonglong>(second), {});
        QVERIFY(waitFor([&] { return found.size() == 1; }));
        QCOMPARE(found.at(0).at(2).toString(), original);
        QFile::remove(original);
        manager_->remove(second, true);
    }

    // Slow (clamscan loads its database each run), so only when asked for.
    void checksScanWithClamav() {
        if (qEnvironmentVariableIsEmpty("ORBIT_TEST_CLAMAV")) QSKIP("set ORBIT_TEST_CLAMAV=1 to run the ClamAV scan");
        if (!FileChecks::clamavAvailable()) QSKIP("ClamAV isn't installed");
        // The EICAR test file: harmless, and every scanner reports it.
        QByteArray eicar = "X5O!P%@AP[4\\PZX54(P^)7CC)7}$EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*";
        TestHttpServer::Behaviour b;
        b.payload = eicar;
        http_->setBehaviour(b);
        const DownloadId id = downloadAndWait("/eicar.com", "eicar.com");
        QVERIFY(id != 0);
        FileChecks checks(*manager_);
        checks.setSettings({.scan = true, .signatures = false, .duplicates = false, .minisignKeys = {}});
        QSignalSpy notices(&checks, &FileChecks::notice);
        std::optional<bool> proceed;
        checks.check(static_cast<qlonglong>(id), [&](bool ok) { proceed = ok; });
        QVERIFY(waitFor([&] { return proceed.has_value(); }, 240000));
        QVERIFY(!*proceed);
        QCOMPARE(checks.resultFor(id).value("scan").toString(), QStringLiteral("infected"));
        QVERIFY(checks.resultFor(id).value("threat").toString().contains("Eicar", Qt::CaseInsensitive));
        QCOMPARE(notices.size(), 1);
        manager_->remove(id, true);
    }
};

QTEST_GUILESS_MAIN(TestUi)
#include "tst_ui.moc"
