#include "core/DownloadManager.hpp"
#include "core/RemoteServer.hpp"
#include "utils/FileUtils.hpp"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

namespace fs = std::filesystem;
using namespace orbit::core;

namespace {

struct Reply {
    int status{0};
    QByteArray headers;
    QByteArray body;
    [[nodiscard]] QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

quint16 freePort() {
    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost, 0);
    return probe.serverPort();
}

}

class TestRemoteServer : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> appData_;
    std::unique_ptr<DownloadManager> manager_;
    std::unique_ptr<RemoteServer> server_;

    // A raw HTTP/1.1 exchange; the server answers and closes, and its event loop runs on
    // this thread, so the reply is awaited with QTest's event-processing waits.
    Reply send(const QByteArray& method, const QByteArray& path, const QByteArray& body = {},
               const QByteArray& cookie = {}, const QByteArray& host = "127.0.0.1", const QByteArray& extraHeaders = {}) {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, server_->port());
        QElapsedTimer timer;
        timer.start();
        while (socket.state() != QAbstractSocket::ConnectedState && timer.elapsed() < 3000) QTest::qWait(5);
        if (socket.state() != QAbstractSocket::ConnectedState) return {};
        QByteArray request = method + " " + path + " HTTP/1.1\r\n";
        if (!host.isEmpty()) request += "Host: " + host + "\r\n";
        request += "Connection: close\r\n";
        if (!cookie.isEmpty()) request += "Cookie: " + cookie + "\r\n";
        request += extraHeaders;
        if (!body.isEmpty()) request += "Content-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
        request += "\r\n" + body;
        socket.write(request);

        QByteArray raw;
        timer.restart();
        while (timer.elapsed() < 5000) {
            QTest::qWait(5);
            raw += socket.readAll();
            if (socket.state() == QAbstractSocket::UnconnectedState || hasFullBody(raw)) break;
        }
        Reply reply;
        const qsizetype split = raw.indexOf("\r\n\r\n");
        reply.headers = raw.left(split);
        reply.body = raw.mid(split + 4);
        reply.status = reply.headers.split(' ').value(1).toInt();
        return reply;
    }

    static bool hasFullBody(const QByteArray& raw) {
        const qsizetype split = raw.indexOf("\r\n\r\n");
        if (split < 0) return false;
        for (const QByteArray& line : raw.left(split).split('\n')) {
            if (line.toLower().startsWith("content-length:")) {
                return raw.size() - split - 4 >= line.mid(15).trimmed().toLongLong();
            }
        }
        return false;
    }

    static QByteArray sessionCookie(const Reply& reply) {
        for (const QByteArray& line : reply.headers.split('\n')) {
            if (!line.toLower().startsWith("set-cookie:")) continue;
            return line.mid(11).trimmed().split(';').value(0);
        }
        return {};
    }

private slots:
    void initTestCase() {
        Q_INIT_RESOURCE(webassets);
        appData_ = std::make_unique<QTemporaryDir>();
        orbit::utils::setAppDataDirOverride(appData_->path().toStdString());
        manager_ = std::make_unique<DownloadManager>(fs::path(appData_->path().toStdString()) / "tasks");
        manager_->setDefaultDownloadDir(appData_->path().toStdString());
    }

    void init() {
        server_ = std::make_unique<RemoteServer>(*manager_);
        server_->setCredentials(QStringLiteral("admin"), QStringLiteral("correct horse"));
        QVERIFY2(server_->start(freePort()), qPrintable(server_->lastError()));
    }

    void cleanup() { server_.reset(); }

    void cleanupTestCase() { manager_.reset(); }

    void servesTheBundledWebUi() {
        const Reply index = send("GET", "/");
        QCOMPARE(index.status, 200);
        QVERIFY(index.body.contains("<html") || index.body.contains("<!doctype") || index.body.contains("<!DOCTYPE"));
        QCOMPARE(send("GET", "/app.js").status, 200);
        QCOMPARE(send("GET", "/style.css").status, 200);
    }

    void apiNeedsASession() {
        const Reply session = send("GET", "/api/session");
        QCOMPARE(session.status, 200);
        QCOMPARE(session.json().value("authenticated").toBool(), false);
        QCOMPARE(session.json().value("configured").toBool(), true);
        QCOMPARE(send("GET", "/api/downloads").status, 401);
        QCOMPARE(send("POST", "/api/downloads/1/pause").status, 401);
    }

    void loginGivesAWorkingSession() {
        const Reply login = send("POST", "/api/login", R"({"username":"admin","password":"correct horse"})");
        QCOMPARE(login.status, 200);
        const QByteArray cookie = sessionCookie(login);
        QVERIFY(cookie.startsWith("orbit_session="));
        QVERIFY(login.headers.toLower().contains("httponly"));

        const Reply list = send("GET", "/api/downloads", {}, cookie);
        QCOMPARE(list.status, 200);
        QVERIFY(list.json().value("downloads").isArray());

        QCOMPARE(send("POST", "/api/logout", "{}", cookie).status, 200);
        QCOMPARE(send("GET", "/api/downloads", {}, cookie).status, 401);
    }

    void repeatedBadLoginsAreThrottled() {
        for (int i = 0; i < 5; ++i) {
            QCOMPARE(send("POST", "/api/login", R"({"username":"admin","password":"wrong"})").status, 401);
        }
        QCOMPARE(send("POST", "/api/login", R"({"username":"admin","password":"wrong"})").status, 429);
        // Even the right password waits out the lockout.
        QCOMPARE(send("POST", "/api/login", R"({"username":"admin","password":"correct horse"})").status, 429);
    }

    void rejectsMalformedAndUnknownRequests() {
        QCOMPARE(send("GET", "/no/such/page").status, 401);
        const Reply login = send("POST", "/api/login", R"({"username":"admin","password":"correct horse"})");
        QCOMPARE(send("GET", "/no/such/page", {}, sessionCookie(login)).status, 404);
    }

    // DNS rebinding: a web page that points its own domain at 127.0.0.1 reaches the
    // dashboard with its own name in Host. Only this PC's names, Cloudflare quick tunnels
    // and configured tunnel names are answered.
    void answersOnlyKnownHosts_data() {
        QTest::addColumn<QByteArray>("host");
        QTest::addColumn<int>("status");
        const QByteArray port = ":" + QByteArray::number(freePort());
        QTest::newRow("127.0.0.1") << QByteArray("127.0.0.1") + port << 200;
        QTest::newRow("localhost") << QByteArray("LocalHost") + port << 200;
        QTest::newRow("ipv6 loopback") << QByteArray("[::1]") + port << 200;
        QTest::newRow("quick tunnel") << QByteArray("calm-river-1234.trycloudflare.com") << 200;
        QTest::newRow("named tunnel") << QByteArray("orbit.example.com") << 200;
        QTest::newRow("named tunnel, trailing dot") << QByteArray("orbit.example.com.") << 200;
        QTest::newRow("no host header") << QByteArray() << 200;
        QTest::newRow("rebinding domain") << QByteArray("evil.example.net") + port << 421;
        QTest::newRow("lookalike suffix") << QByteArray("trycloudflare.com.evil.net") << 421;
        QTest::newRow("lookalike tunnel") << QByteArray("xorbit.example.com") << 421;
        QTest::newRow("loopback as name") << QByteArray("127.0.0.1.nip.io") << 421;
    }

    void answersOnlyKnownHosts() {
        QFETCH(QByteArray, host);
        QFETCH(int, status);
        server_->setAllowedHosts({QStringLiteral("https://Orbit.Example.com/")});
        QCOMPARE(send("GET", "/", {}, {}, host).status, status);
        const Reply login = send("POST", "/api/login", R"({"username":"admin","password":"correct horse"})", {}, host);
        QCOMPARE(login.status, status);
    }

    // A page on another site can't send a POST that carries its own Origin.
    void refusesCrossSitePosts_data() {
        QTest::addColumn<QByteArray>("origin");
        QTest::addColumn<int>("status");
        QTest::newRow("same origin") << QByteArray("http://127.0.0.1:8765") << 200;
        QTest::newRow("quick tunnel") << QByteArray("https://calm-river-1234.trycloudflare.com") << 200;
        QTest::newRow("no origin") << QByteArray() << 200;
        QTest::newRow("other site") << QByteArray("https://evil.example.net") << 403;
        QTest::newRow("opaque origin") << QByteArray("null") << 403;
    }

    void refusesCrossSitePosts() {
        QFETCH(QByteArray, origin);
        QFETCH(int, status);
        const QByteArray header = origin.isEmpty() ? QByteArray() : "Origin: " + origin + "\r\n";
        const Reply login = send("POST", "/api/login", R"({"username":"admin","password":"correct horse"})", {},
                                 "127.0.0.1", header);
        QCOMPARE(login.status, status);
    }

    // Before signing in, nobody can make Orbit buffer a large body.
    void limitsBodiesBeforeSignIn() {
        const QByteArray big(200 * 1024, 'x');
        QCOMPARE(send("POST", "/api/torrents/parse", big).status, 413);
        QCOMPARE(send("POST", "/api/login", big).status, 413);
        // Signed in, the same upload is read (and rejected only as a bad torrent).
        const Reply login = send("POST", "/api/login", R"({"username":"admin","password":"correct horse"})");
        QCOMPARE(send("POST", "/api/torrents/parse", big, sessionCookie(login)).status, 400);
    }

    void sendsBrowserSecurityHeaders() {
        const QByteArray headers = send("GET", "/").headers.toLower();
        QVERIFY(headers.contains("content-security-policy:"));
        QVERIFY(headers.contains("frame-ancestors 'none'"));
        QVERIFY(headers.contains("x-frame-options: deny"));
        QVERIFY(headers.contains("referrer-policy: no-referrer"));
    }

    void normalizesHostNames() {
        QCOMPARE(RemoteServer::normalizeHost(QStringLiteral(" HTTPS://user@Orbit.Example.com:8443/path ")),
                 QStringLiteral("orbit.example.com"));
        QCOMPARE(RemoteServer::normalizeHost(QStringLiteral("[::1]:8765")), QStringLiteral("::1"));
        QCOMPARE(RemoteServer::normalizeHost(QStringLiteral("localhost")), QStringLiteral("localhost"));
        QCOMPARE(RemoteServer::normalizeHost(QString()), QString());
    }
};

QTEST_GUILESS_MAIN(TestRemoteServer)
#include "tst_remoteserver.moc"
