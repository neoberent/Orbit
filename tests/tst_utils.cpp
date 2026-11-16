#include "TestHttpServer.hpp"
#include "core/InterfaceDns.hpp"
#include "core/InterfaceRelay.hpp"
#include "core/UpdateFeed.hpp"
#include "core/TorrentEngine.hpp"
#include "core/MediaDownloader.hpp"
#include "core/ProtocolHandler.hpp"
#include "utils/FileUtils.hpp"
#include "utils/Hashing.hpp"
#include "utils/SpeedLimiter.hpp"
#include "utils/StringUtils.hpp"

#include <QDir>
#include <QVersionNumber>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QProcess>
#include <QHostAddress>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <openssl/evp.h>

#include <chrono>
#include <clocale>
#include <fstream>
#include <thread>
#include <atomic>
#include <mutex>

#if defined(__linux__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace orbit::utils;

namespace {

void writeFile(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Sends `request` to the relay and returns everything it answers until it hangs up. For a
// CONNECT, `thenSend` goes through the tunnel once it's established.
QByteArray throughRelay(int port, const QByteArray& request, const QByteArray& thenSend = {}) {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(port));
    if (!socket.waitForConnected(3000)) return {};
    socket.write(request);
    QByteArray answer;
    if (!thenSend.isEmpty()) {
        while (!answer.contains("\r\n\r\n") && socket.waitForReadyRead(5000)) answer += socket.readAll();
        if (!answer.startsWith("HTTP/1.1 200")) return answer;
        socket.write(thenSend);
    }
    while (socket.state() == QAbstractSocket::ConnectedState && socket.waitForReadyRead(5000)) answer += socket.readAll();
    answer += socket.readAll();
    return answer;
}

#if defined(__linux__)
// A stand-in for the user's own proxy in the relay tests. It speaks SOCKS5 (with user
// name and password) or HTTP (CONNECT and absolute-form requests), writes down what the
// relay asked it for, then connects to 127.0.0.1 at the requested port and passes bytes
// through. With refuse set, it turns every request down.
class FakeUpstream {
public:
    FakeUpstream(bool socks, bool refuse = false) : socks_(socks), refuse_(refuse) {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof(address);
        ::bind(listenFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(listenFd_, 8);
        ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&address), &length);
        port = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeUpstream() {
        stop_ = true;
        thread_.join();
        ::close(listenFd_);
    }
    std::string seen() {
        std::scoped_lock lock(mutex_);
        return seen_;
    }

    int port{0};

private:
    void note(const std::string& text) {
        std::scoped_lock lock(mutex_);
        seen_ += text + "\n";
    }
    static bool readExact(int fd, char* data, size_t size) {
        while (size > 0) {
            const ssize_t got = ::recv(fd, data, size, 0);
            if (got <= 0) return false;
            data += got;
            size -= static_cast<size_t>(got);
        }
        return true;
    }
    static int connectLocal(int targetPort) {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<uint16_t>(targetPort));
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(fd);
            return -1;
        }
        return fd;
    }
    static void pass(int a, int b, const std::string& first = {}) {
        if (!first.empty()) ::send(b, first.data(), first.size(), MSG_NOSIGNAL);
        char buffer[16384];
        bool open[2] = {true, true};
        int fds[2] = {a, b};
        while (open[0] || open[1]) {
            pollfd wait[2] = {{a, static_cast<short>(open[0] ? POLLIN : 0), 0}, {b, static_cast<short>(open[1] ? POLLIN : 0), 0}};
            if (::poll(wait, 2, 5000) <= 0) return;
            for (int i = 0; i < 2; ++i) {
                if (!open[i] || !(wait[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                const ssize_t got = ::recv(fds[i], buffer, sizeof(buffer), 0);
                if (got <= 0) {
                    open[i] = false;
                    ::shutdown(fds[1 - i], SHUT_WR);
                } else {
                    ::send(fds[1 - i], buffer, static_cast<size_t>(got), MSG_NOSIGNAL);
                }
            }
        }
    }
    void serveSocks(int client) {
        unsigned char head[2];
        if (!readExact(client, reinterpret_cast<char*>(head), 2)) return;
        std::string methods(head[1], '\0');
        if (!readExact(client, methods.data(), methods.size())) return;
        ::send(client, "\x05\x02", 2, MSG_NOSIGNAL);  // user name and password
        unsigned char version = 0, length = 0;
        if (!readExact(client, reinterpret_cast<char*>(&version), 1) || !readExact(client, reinterpret_cast<char*>(&length), 1)) return;
        std::string user(length, '\0');
        if (!readExact(client, user.data(), user.size()) || !readExact(client, reinterpret_cast<char*>(&length), 1)) return;
        std::string password(length, '\0');
        if (!readExact(client, password.data(), password.size())) return;
        note("login " + user + ":" + password);
        if (refuse_) {
            ::send(client, "\x01\x01", 2, MSG_NOSIGNAL);
            return;
        }
        ::send(client, "\x01\x00", 2, MSG_NOSIGNAL);
        unsigned char request[4];
        if (!readExact(client, reinterpret_cast<char*>(request), 4) || request[3] != 3) {
            note("not a name");
            return;
        }
        if (!readExact(client, reinterpret_cast<char*>(&length), 1)) return;
        std::string name(length, '\0');
        unsigned char portBytes[2];
        if (!readExact(client, name.data(), name.size()) || !readExact(client, reinterpret_cast<char*>(portBytes), 2)) return;
        const int targetPort = portBytes[0] << 8 | portBytes[1];
        note("connect " + name + ":" + std::to_string(targetPort));
        const int server = connectLocal(targetPort);
        if (server < 0) return;
        ::send(client, "\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00", 10, MSG_NOSIGNAL);
        pass(client, server);
        ::close(server);
    }
    void serveHttp(int client) {
        std::string head;
        char c;
        while (head.find("\r\n\r\n") == std::string::npos && ::recv(client, &c, 1, 0) == 1) head += c;
        note(head);
        if (refuse_) {
            const std::string no = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
            ::send(client, no.data(), no.size(), MSG_NOSIGNAL);
            return;
        }
        const std::string line = head.substr(0, head.find("\r\n"));
        const auto targetStart = line.find(' ') + 1;
        std::string target = line.substr(targetStart, line.find(' ', targetStart) - targetStart);
        const bool tunnel = line.rfind("CONNECT ", 0) == 0;
        if (!tunnel) target = target.substr(7, target.find('/', 7) - 7);  // http://host:port/path
        const int server = connectLocal(std::atoi(target.substr(target.rfind(':') + 1).c_str()));
        if (server < 0) return;
        if (tunnel) {
            ::send(client, "HTTP/1.1 200 OK\r\n\r\n", 19, MSG_NOSIGNAL);
            pass(client, server);
        } else {
            // On to the server in origin form: "GET /path HTTP/1.1".
            std::string forwarded = head;
            forwarded.erase(targetStart, line.find('/', targetStart + 7) - targetStart);
            pass(client, server, forwarded);
        }
        ::close(server);
    }
    void serve() {
        while (!stop_) {
            pollfd wait{listenFd_, POLLIN, 0};
            if (::poll(&wait, 1, 100) <= 0) continue;
            const int client = ::accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) continue;
            timeval timeout{5, 0};
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            if (socks_) serveSocks(client);
            else serveHttp(client);
            ::close(client);
        }
    }

    bool socks_;
    bool refuse_;
    int listenFd_{-1};
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::string seen_;
};
#endif

QByteArray basicAuth(const orbit::core::ProxySettings& proxy) {
    return "Proxy-Authorization: Basic " +
           QByteArray::fromStdString(proxy.username + ":" + proxy.password).toBase64() + "\r\n";
}

}

class TestUtils : public QObject {
    Q_OBJECT

private slots:
    // QCoreApplication adopts the system locale, and formatBytes() follows its decimal
    // separator; the expectations below are written for the C locale.
    void initTestCase() { std::setlocale(LC_NUMERIC, "C"); }

    void dnsQueryAndAnswer() {
        using orbit::core::InterfaceDns;
        const std::string query = InterfaceDns::buildQuery("dl.example.org", 1);
        // Header (12) + 2dl 7example 3org 0 + type + class.
        QCOMPARE(query.size(), size_t(12 + 16 + 4));
        QCOMPARE(query.substr(12, 3), std::string("\x02" "dl"));

        // An answer: the question echoed, a CNAME, an A and an AAAA record, the last two
        // with compressed names.
        std::string answer("\x00\x00\x81\x80\x00\x01\x00\x03\x00\x00\x00\x00", 12);
        answer += query.substr(12);
        auto record = [&](const std::string& type, const std::string& ttl, const std::string& data) {
            answer += std::string("\xC0\x0C", 2) + type + std::string("\x00\x01", 2) + ttl;
            answer.push_back(static_cast<char>(data.size() >> 8));
            answer.push_back(static_cast<char>(data.size() & 0xFF));
            answer += data;
        };
        record(std::string("\x00\x05", 2), std::string("\x00\x00\x00\x3C", 4), std::string("\x03" "cdn\xC0\x0F", 6));
        record(std::string("\x00\x01", 2), std::string("\x00\x00\x01\x2C", 4), std::string("\xC0\x00\x02\x0A", 4));
        record(std::string("\x00\x1C", 2), std::string("\x00\x00\x0E\x10", 4),
               std::string("\x20\x01\x0d\xb8\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01", 16));
        int ttl = 0;
        const auto addresses = InterfaceDns::parseAnswer(answer, ttl);
        QCOMPARE(addresses.size(), size_t(2));
        QCOMPARE(addresses[0], std::string("192.0.2.10"));
        QCOMPARE(addresses[1], std::string("2001:db8:0:0:0:0:0:1"));
        QCOMPARE(ttl, 300);  // the shortest address record's

        // A failed lookup (NXDOMAIN) and a truncated answer give nothing.
        std::string nx = answer;
        nx[3] = '\x83';
        QVERIFY(InterfaceDns::parseAnswer(nx, ttl).empty());
        QVERIFY(InterfaceDns::parseAnswer(answer.substr(0, 20), ttl).empty());
    }

    void updateFeedRules() {
        using orbit::core::readUpdateFeed;
        const QString sha(64, QLatin1Char('a'));
        // The bridge feed: Windows 3.1.6 at the top, Linux 3.2.0 with its own version and
        // notes, and news about it for Windows.
        const QJsonObject bridge{
            {"version", "3.1.6"}, {"url", "https://example.org/OrbitSetup-3.1.6.exe"}, {"sha256", sha},
            {"notes", QJsonArray{"Windows: tells you about Linux releases"}}, {"released", "2026-11-20"},
            {"linux", QJsonObject{{"version", "3.2.0"}, {"url", "https://example.org/OrbitSetup-3.2.0-linux.tar.gz"},
                                  {"sha256", sha}, {"size", 123}, {"notes", QJsonArray{"Native Wayland"}}, {"released", "2026-11-20"}}},
            {"linuxNews", QJsonObject{{"version", "3.2.0"}, {"notes", QJsonArray{"Native Wayland", "- Torrent feeds"}}}},
        };
        const auto win = readUpdateFeed(bridge, false);
        QCOMPARE(win.version.toString(), QStringLiteral("3.1.6"));
        QCOMPARE(win.url, QStringLiteral("https://example.org/OrbitSetup-3.1.6.exe"));
        QCOMPARE(win.newsVersion.toString(), QStringLiteral("3.2.0"));
        QCOMPARE(win.newsNotes, (QStringList{"Native Wayland", "Torrent feeds"}));
        const auto lin = readUpdateFeed(bridge, true);
        QCOMPARE(lin.version.toString(), QStringLiteral("3.2.0"));
        QCOMPARE(lin.url, QStringLiteral("https://example.org/OrbitSetup-3.2.0-linux.tar.gz"));
        QCOMPARE(lin.notes, QStringList{"Native Wayland"});
        QVERIFY(lin.newsVersion.isNull());
        // How Orbit 3.1.5 reads the same feed (top-level version for both platforms): Windows
        // installs 3.1.6, Linux sees 3.1.6 > 3.1.5 and installs the "linux" archive (3.2.0).
        QVERIFY(QVersionNumber::fromString(bridge.value("version").toString()) > QVersionNumber(3, 1, 5));

        // Later: a Linux-only release; Windows is already on 3.1.6 and gets news only.
        QJsonObject later = bridge;
        QJsonObject linux = later.value("linux").toObject();
        linux.insert("version", "3.3.0");
        later.insert("linux", linux);
        later.insert("linuxNews", QJsonObject{{"version", "3.3.0"}, {"notes", QJsonArray{"Something new"}}});
        QCOMPARE(readUpdateFeed(later, false).version.toString(), QStringLiteral("3.1.6"));
        QCOMPARE(readUpdateFeed(later, true).version.toString(), QStringLiteral("3.3.0"));

        // A feed from before: no Linux version of its own, so Linux follows the top level.
        const QJsonObject old{{"version", "3.1.5"}, {"url", "https://example.org/a.exe"}, {"sha256", sha},
                              {"notes", QJsonArray{"fix"}}, {"linux", QJsonObject{{"url", "https://example.org/a.tar.gz"}, {"sha256", sha}}}};
        QCOMPARE(readUpdateFeed(old, true).version.toString(), QStringLiteral("3.1.5"));
        QCOMPARE(readUpdateFeed(old, true).notes, QStringList{"fix"});
        // No Linux build at all: nothing to offer on Linux.
        QJsonObject windowsOnly = old;
        windowsOnly.remove("linux");
        QVERIFY(readUpdateFeed(windowsOnly, true).version.isNull());
    }

    // Only an offer signed with the release key is installed; changing what it points at,
    // or moving a signature to the other platform, breaks it.
    void updateOffersMustBeSigned() {
        EVP_PKEY* key = nullptr;
        EVP_PKEY_CTX* gen = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        QVERIFY(gen && EVP_PKEY_keygen_init(gen) == 1 && EVP_PKEY_keygen(gen, &key) == 1);
        EVP_PKEY_CTX_free(gen);
        QByteArray publicKey(32, '\0');
        size_t publicLength = 32;
        QVERIFY(EVP_PKEY_get_raw_public_key(key, reinterpret_cast<unsigned char*>(publicKey.data()), &publicLength) == 1);
        auto sign = [key](const QByteArray& message) {
            QByteArray signature(64, '\0');
            size_t length = 64;
            EVP_MD_CTX* ctx = EVP_MD_CTX_new();
            EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key);
            EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(signature.data()), &length,
                           reinterpret_cast<const unsigned char*>(message.constData()), static_cast<size_t>(message.size()));
            EVP_MD_CTX_free(ctx);
            return QString::fromLatin1(signature.toBase64());
        };

        using orbit::core::readUpdateFeed;
        using orbit::core::updateSignedMessage;
        using orbit::core::verifyUpdateSignature;
        QJsonObject feed{{"version", "3.1.7"}, {"url", "https://example.org/OrbitSetup-3.1.7.exe"},
                         {"sha256", QString(64, QLatin1Char('A'))}};
        QJsonObject linux{{"version", "3.2.1"}, {"url", "https://example.org/OrbitSetup-3.2.1-linux.tar.gz"},
                          {"sha256", QString(64, QLatin1Char('b'))}};
        feed["signature"] = sign(updateSignedMessage(readUpdateFeed(feed, false), false));
        linux["signature"] = sign(updateSignedMessage(readUpdateFeed(QJsonObject{{"linux", linux}}, true), true));
        feed["linux"] = linux;

        QVERIFY(verifyUpdateSignature(readUpdateFeed(feed, false), false, publicKey));
        QVERIFY(verifyUpdateSignature(readUpdateFeed(feed, true), true, publicKey));
        // The message is exactly what the website signs.
        QCOMPARE(updateSignedMessage(readUpdateFeed(feed, false), false),
                 QByteArray("orbit-update-v1\nwindows\n3.1.7\nhttps://example.org/OrbitSetup-3.1.7.exe\n" + QByteArray(64, 'a') + "\n"));

        auto tampered = [&](const char* field, const QString& value) {
            QJsonObject copy = feed;
            copy[QLatin1String(field)] = value;
            return verifyUpdateSignature(readUpdateFeed(copy, false), false, publicKey);
        };
        QVERIFY(!tampered("url", QStringLiteral("https://evil.example.net/OrbitSetup-3.1.7.exe")));
        QVERIFY(!tampered("sha256", QString(64, QLatin1Char('c'))));
        QVERIFY(!tampered("version", QStringLiteral("9.9.9")));
        QVERIFY(!tampered("signature", QString()));
        // A Windows signature doesn't vouch for the Linux entry (or the other way round).
        QJsonObject swapped = feed;
        QJsonObject swappedLinux = linux;
        swappedLinux["signature"] = feed["signature"];
        swapped["linux"] = swappedLinux;
        QVERIFY(!verifyUpdateSignature(readUpdateFeed(swapped, true), true, publicKey));
        // Another key's signature counts for nothing.
        QVERIFY(!verifyUpdateSignature(readUpdateFeed(feed, false), false, QByteArray(32, 'k')));
        EVP_PKEY_free(key);
    }

    void noCowOnBtrfs() {
        // Run in the build folder: /tmp is usually tmpfs, where there's nothing to test.
        QTemporaryDir dir(QDir::currentPath() + QStringLiteral("/nocow-XXXXXX"));
        const std::string path = dir.path().toStdString();
        if (!orbit::core::TorrentEngine::isOnBtrfs(path)) QSKIP("not on btrfs");
        const std::string file = path + "/piece-data.bin";
        std::ofstream{file, std::ios::binary};
        QVERIFY(orbit::core::TorrentEngine::markNoCow(file));
        QProcess lsattr;
        lsattr.start(QStringLiteral("lsattr"), {QString::fromStdString(file)});
        QVERIFY(lsattr.waitForFinished(5000));
        const QString attributes = QString::fromLocal8Bit(lsattr.readAllStandardOutput()).section(QLatin1Char(' '), 0, 0);
        QVERIFY2(attributes.contains(QLatin1Char('C')), qPrintable(attributes));
    }

    void dnsUrlParts() {
        using orbit::core::InterfaceDns;
        QCOMPARE(InterfaceDns::hostOf("https://user:pw@Files.Example.org:8443/a?b#c"), std::string("files.example.org"));
        QCOMPARE(InterfaceDns::hostOf("http://[2001:db8::1]:8080/x"), std::string("2001:db8::1"));
        QCOMPARE(InterfaceDns::portOf("https://files.example.org/a"), 443);
        QCOMPARE(InterfaceDns::portOf("http://files.example.org/a"), 80);
        QCOMPARE(InterfaceDns::portOf("http://[2001:db8::1]:8080/x"), 8080);
        QVERIFY(InterfaceDns::isIpLiteral("10.0.0.1"));
        QVERIFY(InterfaceDns::isIpLiteral("2001:db8::1"));
        QVERIFY(!InterfaceDns::isIpLiteral("example.org"));
    }

    void dnsThroughAnUnusableInterfaceFindsNothing() {
        // Bound to loopback, the resolvers can't be reached: the answer must be "nothing",
        // never a fall back to the system resolver.
        using orbit::core::InterfaceDns;
        InterfaceDns::clearCache();
        QVERIFY(InterfaceDns::resolve("example.org", "lo").empty());
        QCOMPARE(InterfaceDns::resolve("127.0.0.1", "lo"), std::vector<std::string>{"127.0.0.1"});
    }

    void trimAndSplit() {
        QCOMPARE(trim("  a b \t\n"), std::string("a b"));
        QCOMPARE(trim(""), std::string());
        const auto parts = split("a,,b", ',');
        QCOMPARE(parts.size(), size_t(3));
        QCOMPARE(parts[2], std::string("b"));
        QVERIFY(iequals("SHA256", "sha256"));
        QVERIFY(!iequals("sha", "sha256"));
        QVERIFY(startsWith("magnet:?xt", "magnet:"));
    }

    void formatting() {
        QCOMPARE(formatBytes(512), std::string("512 B"));
        QCOMPARE(formatBytes(1536), std::string("1.50 KB"));
        QCOMPARE(formatBytes(5ull * 1024 * 1024 * 1024), std::string("5.00 GB"));
        QCOMPARE(formatRate(-5.0), std::string("0 B/s"));
    }

    void urls() {
        QCOMPARE(fileNameFromUrl("https://host/dir/My%20File.zip?token=1#x"), std::string("My File.zip"));
        QCOMPARE(fileNameFromUrl("https://host/"), std::string());
        QCOMPARE(urlDecode("a+b%2Fc%zz"), std::string("a b/c%zz"));
        QVERIFY(isMagnetUri("magnet:?xt=urn:btih:abc"));
        QVERIFY(!isMagnetUri("https://example.com/magnet:?"));
        QVERIFY(isMediaUrl("https://www.youtube.com/watch?v=abc"));
        QVERIFY(!isMediaUrl("https://example.com/file.zip"));
        QVERIFY(isMediaUrl("https://user@www.youtube.com/watch?v=abc"));
        QVERIFY(!isMediaUrl("https://example.com/@youtube.com"));
        QVERIFY(!isMediaUrl("https://example.com/?u=a@youtube.com"));
    }

    void sanitizeFileNames() {
        QCOMPARE(sanitizeFileName("a/b\\c:d*e?f\"g<h>i|j"), std::string("a_b_c_d_e_f_g_h_i_j"));
        QCOMPARE(sanitizeFileName("name. . "), std::string("name"));
        QCOMPARE(sanitizeFileName("..."), std::string("download"));
        QCOMPARE(sanitizeFileName(std::string("tab\there")), std::string("tab_here"));
        // Long names (from a URL or Content-Disposition) are cut so the file can always be
        // created: the extension stays, and no character is cut in half.
        const std::string longAscii = sanitizeFileName(std::string(400, 'a') + ".tar.gz");
        QCOMPARE(longAscii.size(), kMaxFileNameBytes);
        QVERIFY(longAscii.ends_with("a.gz"));
        std::string umlauts;
        for (int i = 0; i < 150; ++i) umlauts += "\xC3\xA4";  // "ä", two bytes each
        const std::string cut = sanitizeFileName(umlauts + ".mkv");
        QVERIFY(cut.size() <= kMaxFileNameBytes);
        QVERIFY(cut.ends_with(".mkv"));
        QVERIFY(QString::fromUtf8(cut.data(), static_cast<qsizetype>(cut.size())).toUtf8().toStdString() == cut);
        QCOMPARE((cut.size() - 4) % 2, std::size_t(0));  // only whole "ä"s
        QCOMPARE(sanitizeFileName(std::string(50, 'b') + ".txt"), std::string(50, 'b') + ".txt");
    }

    void uniquePaths() {
        QTemporaryDir dir;
        const fs::path base = fs::path(dir.path().toStdString()) / "file.tar.gz";
        QCOMPARE(uniquePath(base), base);
        writeFile(base, "x");
        QCOMPARE(uniquePath(base).filename().string(), std::string("file.tar (1).gz"));
        writeFile(uniquePath(base), "y");
        QCOMPARE(uniquePath(base).filename().string(), std::string("file.tar (2).gz"));
    }

    void preallocateAndSize() {
        QTemporaryDir dir;
        const fs::path file = fs::path(dir.path().toStdString()) / "sub" / "big.bin";
        QVERIFY(ensureDirectoryExists(file.parent_path()));
        QVERIFY(preallocateFile(file, 1 << 20));
        QCOMPARE(fileSize(file).value_or(0), std::uint64_t(1 << 20));
        QVERIFY(!fileSize(file.parent_path() / "missing").has_value());
    }

    void moveFiles() {
        QTemporaryDir dir;
        const fs::path root = dir.path().toStdString();
        writeFile(root / "a.txt", "hello");
        fs::create_directories(root / "out");
        QCOMPARE(moveFile(root / "a.txt", root / "out" / "b.txt"), std::string());
        QVERIFY(!fs::exists(root / "a.txt"));
        QCOMPARE(readFile(root / "out" / "b.txt"), std::string("hello"));
        QVERIFY(!moveFile(root / "missing.txt", root / "out" / "c.txt").empty());
    }

    void parseChecksums() {
        const std::string sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        auto spec = parseChecksum("SHA-256 " + std::string("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
        QVERIFY(spec.has_value());
        QCOMPARE(spec->algorithm, HashAlgorithm::Sha256);
        QCOMPARE(spec->toString(), "sha256:" + sha256);

        spec = parseChecksum("900150983cd24fb0d6963f7d28e17f72");
        QVERIFY(spec.has_value());
        QCOMPARE(spec->algorithm, HashAlgorithm::Md5);

        QVERIFY(!parseChecksum("sha256:1234").has_value());
        QVERIFY(!parseChecksum("not a digest").has_value());
        QVERIFY(!parseChecksum("").has_value());
    }

    void hashFiles() {
        QTemporaryDir dir;
        const fs::path file = fs::path(dir.path().toStdString()) / "abc.txt";
        writeFile(file, "abc");
        const auto digests = hashFile(file, {HashAlgorithm::Md5, HashAlgorithm::Sha1, HashAlgorithm::Sha256});
        QVERIFY(digests.has_value());
        QCOMPARE((*digests)[0], std::string("900150983cd24fb0d6963f7d28e17f72"));
        QCOMPARE((*digests)[1], std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
        QCOMPARE((*digests)[2], std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

        QVERIFY(!hashFile(file.parent_path() / "missing", {HashAlgorithm::Sha256}).has_value());
        QVERIFY(!hashFile(file, {HashAlgorithm::Sha256}, [](std::uint64_t, std::uint64_t) { return false; }).has_value());
    }

    void speedLimiterPaces() {
        SpeedLimiter limiter;
        limiter.setLimitBytesPerSec(1'000'000);
        const auto start = std::chrono::steady_clock::now();
        // Two threads sharing one limiter: together they're held to the single cap.
        auto pump = [&limiter] {
            for (int i = 0; i < 25; ++i) limiter.consume(10'000);
        };
        std::thread a(pump);
        std::thread b(pump);
        a.join();
        b.join();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        // 500 kB at 1 MB/s.
        QVERIFY2(seconds > 0.35 && seconds < 1.5, qPrintable(QString::number(seconds)));
    }

    void protocolLinksOnlyCarryWebLinks() {
        using orbit::core::protocol::extractDownloadUrl;
        QCOMPARE(extractDownloadUrl("orbit://add?url=https%3A%2F%2Fexample.com%2Fa.zip").value_or(""),
                 std::string("https://example.com/a.zip"));
        QCOMPARE(extractDownloadUrl("orbit://add/?url=HTTP://example.com/a").value_or(""),
                 std::string("HTTP://example.com/a"));
        QCOMPARE(extractDownloadUrl("orbit://add?url=magnet%3A%3Fxt%3Durn%3Abtih%3Aabc").value_or(""),
                 std::string("magnet:?xt=urn:btih:abc"));
        // A web page must not make Orbit open a file: a network path would send the
        // user's Windows credentials to that server.
        QVERIFY(!extractDownloadUrl("orbit://add?url=%5C%5Cevil.example%5Cs%5Cx.torrent"));
        QVERIFY(!extractDownloadUrl("orbit://add?url=//evil.example/s/x.torrent"));
        QVERIFY(!extractDownloadUrl("orbit://add?url=C%3A%5Cx.torrent"));
        QVERIFY(!extractDownloadUrl("orbit://add?url=file%3A%2F%2F%2Fetc%2Fx.torrent"));
        QVERIFY(!extractDownloadUrl("orbit://add?url=ftp%3A%2F%2Fhost%2Fx"));
    }

#if defined(__linux__)
    // The relay that keeps yt-dlp and HTTP trackers on a locked interface: CONNECT and
    // plain absolute-form requests both arrive, bound to the interface (lo here).
    void relayCarriesTunnelsAndPlainRequests() {
        orbit::tests::TestHttpServer server;
        server.setBehaviour({.payload = "through the relay"});
        const QByteArray target = QUrl(QString::fromStdString(server.url())).authority().toLatin1();
        orbit::core::InterfaceRelay relay;
        QVERIFY(relay.setInterface("lo"));
        const auto proxy = relay.proxy();
        QVERIFY(proxy.enabled());
        QCOMPARE(proxy.host, std::string("127.0.0.1"));

        const QByteArray get = "GET /file.bin HTTP/1.1\r\nHost: " + target + "\r\nConnection: close\r\n\r\n";
        const QByteArray tunneled =
            throughRelay(proxy.port, "CONNECT " + target + " HTTP/1.1\r\nHost: " + target + "\r\n" + basicAuth(proxy) + "\r\n", get);
        QVERIFY2(tunneled.startsWith("HTTP/1.1 200 Connection established"), tunneled.constData());
        QVERIFY(tunneled.endsWith("through the relay"));

        const QByteArray plain = throughRelay(proxy.port, "GET http://" + target + "/file.bin HTTP/1.1\r\nHost: " + target +
                                                              "\r\n" + basicAuth(proxy) + "Proxy-Connection: keep-alive\r\n\r\n");
        QVERIFY2(plain.startsWith("HTTP/1.1 200"), plain.constData());
        QVERIFY(plain.endsWith("through the relay"));
    }

    void relayOnlyServesOrbitsOwnPrograms() {
        orbit::tests::TestHttpServer server;
        const QByteArray target = QUrl(QString::fromStdString(server.url())).authority().toLatin1();
        orbit::core::InterfaceRelay relay;
        QVERIFY(relay.setInterface("lo"));
        const int port = relay.proxy().port;
        QVERIFY(throughRelay(port, "CONNECT " + target + " HTTP/1.1\r\n\r\n").startsWith("HTTP/1.1 407"));
        QVERIFY(throughRelay(port, "CONNECT " + target + " HTTP/1.1\r\nProxy-Authorization: Basic d3Jvbmc6Z3Vlc3M=\r\n\r\n")
                    .startsWith("HTTP/1.1 407"));
        // Turned off with the lock: nothing listens any more.
        relay.setInterface("");
        QVERIFY(!relay.proxy().enabled());
        QVERIFY(throughRelay(port, "CONNECT " + target + " HTTP/1.1\r\n\r\n").isEmpty());
    }

    // A relay bound to an interface that doesn't exist can't reach anything, and says so
    // instead of connecting some other way.
    void relayFailsClosedOnAMissingInterface() {
        orbit::tests::TestHttpServer server;
        const QByteArray target = QUrl(QString::fromStdString(server.url())).authority().toLatin1();
        orbit::core::InterfaceRelay relay;
        QVERIFY(relay.setInterface("orbitnone0"));
        const auto proxy = relay.proxy();
        const QByteArray answer = throughRelay(proxy.port, "CONNECT " + target + " HTTP/1.1\r\n" + basicAuth(proxy) + "\r\n");
        QVERIFY2(answer.startsWith("HTTP/1.1 502"), answer.constData());
        QCOMPARE(server.getRequests(), 0);
    }
    // With the user's own proxy, the relay goes through it: a SOCKS5 proxy gets the
    // credentials and the target's name (so this machine never looks it up).
    void relayChainsThroughASocks5Proxy() {
        orbit::tests::TestHttpServer server;
        server.setBehaviour({.payload = "via socks"});
        const int serverPort = QUrl(QString::fromStdString(server.url())).port();
        const QByteArray target = "localhost:" + QByteArray::number(serverPort);
        FakeUpstream upstream(true);
        orbit::core::InterfaceRelay relay;
        orbit::core::ProxySettings mine;
        mine.type = orbit::core::ProxySettings::Type::Socks5;
        mine.host = "127.0.0.1";
        mine.port = upstream.port;
        mine.username = "me";
        mine.password = "pa:ss";
        relay.setUpstream(mine);  // before the relay starts: kept for it
        QVERIFY(relay.setInterface("lo"));
        const auto proxy = relay.proxy();

        const QByteArray get = "GET /file.bin HTTP/1.1\r\nHost: " + target + "\r\nConnection: close\r\n\r\n";
        const QByteArray tunneled =
            throughRelay(proxy.port, "CONNECT " + target + " HTTP/1.1\r\n" + basicAuth(proxy) + "\r\n", get);
        QVERIFY2(tunneled.startsWith("HTTP/1.1 200 Connection established"), tunneled.constData());
        QVERIFY(tunneled.endsWith("via socks"));
        const QByteArray plain = throughRelay(proxy.port, "GET http://" + target + "/file.bin HTTP/1.1\r\nHost: " + target +
                                                              "\r\n" + basicAuth(proxy) + "\r\n");
        QVERIFY2(plain.endsWith("via socks"), plain.constData());
        const std::string seen = upstream.seen();
        QVERIFY2(seen.find("login me:pa:ss") != std::string::npos, seen.c_str());
        QVERIFY2(seen.find("connect localhost:" + std::to_string(serverPort)) != std::string::npos, seen.c_str());
        QVERIFY2(seen.find("not a name") == std::string::npos, seen.c_str());
    }

    // An HTTP proxy gets a CONNECT for https and the absolute URL for plain http, each
    // with the user's credentials and never the relay's own.
    void relayChainsThroughAnHttpProxy() {
        orbit::tests::TestHttpServer server;
        server.setBehaviour({.payload = "via http proxy"});
        const QByteArray target = "localhost:" + QByteArray::number(QUrl(QString::fromStdString(server.url())).port());
        FakeUpstream upstream(false);
        orbit::core::InterfaceRelay relay;
        QVERIFY(relay.setInterface("lo"));
        orbit::core::ProxySettings mine;
        mine.type = orbit::core::ProxySettings::Type::Http;
        mine.host = "127.0.0.1";
        mine.port = upstream.port;
        mine.username = "me";
        mine.password = "secret";
        relay.setUpstream(mine);  // while running: used from the next connection
        const auto proxy = relay.proxy();

        const QByteArray get = "GET /file.bin HTTP/1.1\r\nHost: " + target + "\r\nConnection: close\r\n\r\n";
        const QByteArray tunneled =
            throughRelay(proxy.port, "CONNECT " + target + " HTTP/1.1\r\n" + basicAuth(proxy) + "\r\n", get);
        QVERIFY2(tunneled.endsWith("via http proxy"), tunneled.constData());
        const QByteArray plain = throughRelay(proxy.port, "GET http://" + target + "/file.bin HTTP/1.1\r\nHost: " + target +
                                                              "\r\n" + basicAuth(proxy) + "\r\n");
        QVERIFY2(plain.endsWith("via http proxy"), plain.constData());

        const std::string seen = upstream.seen();
        const std::string mineAuth = "Proxy-Authorization: Basic " + QByteArray("me:secret").toBase64().toStdString();
        QVERIFY2(seen.find("CONNECT " + target.toStdString() + " HTTP/1.1") != std::string::npos, seen.c_str());
        QVERIFY2(seen.find("GET http://" + target.toStdString() + "/file.bin HTTP/1.1") != std::string::npos, seen.c_str());
        size_t count = 0;
        for (size_t at = seen.find(mineAuth); at != std::string::npos; at = seen.find(mineAuth, at + 1)) ++count;
        QCOMPARE(count, size_t(2));
        QVERIFY2(seen.find(basicAuth(proxy).trimmed().toStdString()) == std::string::npos, seen.c_str());
    }

    // A proxy that turns the relay down means a failed connection, never a direct one.
    void relayFailsWhenTheProxyRefuses() {
        orbit::tests::TestHttpServer server;
        const QByteArray target = "localhost:" + QByteArray::number(QUrl(QString::fromStdString(server.url())).port());
        for (const bool socks : {true, false}) {
            FakeUpstream upstream(socks, true);
            orbit::core::InterfaceRelay relay;
            orbit::core::ProxySettings mine;
            mine.type = socks ? orbit::core::ProxySettings::Type::Socks5 : orbit::core::ProxySettings::Type::Http;
            mine.host = "127.0.0.1";
            mine.port = upstream.port;
            mine.username = "me";
            mine.password = "wrong";
            relay.setUpstream(mine);
            QVERIFY(relay.setInterface("lo"));
            const auto proxy = relay.proxy();
            const QByteArray answer = throughRelay(proxy.port, "CONNECT " + target + " HTTP/1.1\r\n" + basicAuth(proxy) + "\r\n");
            QVERIFY2(answer.startsWith("HTTP/1.1 502"), answer.constData());
        }
        QCOMPARE(server.getRequests(), 0);
    }
#endif

    // What yt-dlp is handed for the user's proxy: locked, only the relay (which reaches the
    // proxy through the interface) and never --proxy or --source-address; unlocked, a
    // password only ever in the environment, never in the command line others can read.
    void ytDlpProxyGoesWhereItShould() {
#if !defined(__linux__)
        QSKIP("the relay and the stand-in script are Linux-only");
#else
        QTemporaryDir fakeDir;
        QVERIFY(fakeDir.isValid());
        const QString record = fakeDir.filePath(QStringLiteral("seen.txt"));
        const QString script = fakeDir.filePath(QStringLiteral("yt-dlp"));
        writeFile(script.toStdString(), "#!/bin/sh\necho \"args: $*\" > '" + record.toStdString() +
                                            "'\necho \"env: $HTTPS_PROXY\" >> '" + record.toStdString() + "'\nexit 1\n");
        QFile::setPermissions(script, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        orbit::core::MediaDownloader::setYtDlpPathOverride(script);

        orbit::core::ProxySettings mine;
        mine.type = orbit::core::ProxySettings::Type::Socks5;
        mine.host = "proxy.example";
        mine.port = 1080;
        mine.username = "me";
        mine.password = "hunter2";
        orbit::core::ProxySettings relay;
        relay.type = orbit::core::ProxySettings::Type::Http;
        relay.host = "127.0.0.1";
        relay.port = 4321;
        relay.username = "relayuser";
        relay.password = "relaypass";

        const auto run = [&](const orbit::core::ProxySettings& proxy, const std::string& interfaceName) {
            QFile::remove(record);
            orbit::core::MediaDownloader media(orbit::core::ProgressReporter{});
            media.setProxy(proxy);
            media.setRelayProxy(relay);
            media.setNetworkInterface(interfaceName);
            bool finished = false;
            media.fetchInfo(QStringLiteral("https://example.com/v"), {}, [&](const orbit::core::MediaInfo&) { finished = true; });
            [&] { QTRY_VERIFY_WITH_TIMEOUT(finished, 10000); }();
            return readFile(record.toStdString());
        };

        const std::string locked = run(mine, "lo");
        QVERIFY2(locked.find("env: http://relayuser:relaypass@127.0.0.1:4321") != std::string::npos, locked.c_str());
        QVERIFY2(locked.find("--proxy") == std::string::npos, locked.c_str());
        QVERIFY2(locked.find("--source-address") == std::string::npos, locked.c_str());
        QVERIFY2(locked.find("hunter2") == std::string::npos, locked.c_str());

        const std::string withPassword = run(mine, "");
        QVERIFY2(withPassword.find("env: socks5h://me:hunter2@proxy.example:1080") != std::string::npos, withPassword.c_str());
        QVERIFY2(withPassword.find("--proxy") == std::string::npos, withPassword.c_str());

        orbit::core::ProxySettings open = mine;
        open.username.clear();
        open.password.clear();
        const std::string withoutPassword = run(open, "");
        QVERIFY2(withoutPassword.find("--proxy socks5h://proxy.example:1080") != std::string::npos, withoutPassword.c_str());

        orbit::core::MediaDownloader::setYtDlpPathOverride({});
#endif
    }

    void ytDlpNeverStartsInTheCurrentFolder() {
#if defined(_WIN32)
        QSKIP("a shell script stands in for yt-dlp");
#else
        // yt-dlp reads a yt-dlp.conf from its working folder, and its --exec runs commands.
        // Orbit may have been started from Downloads, where a web page can drop that file.
        QTemporaryDir fakeDir;
        QTemporaryDir launchDir;
        QVERIFY(fakeDir.isValid() && launchDir.isValid());
        const QString marker = fakeDir.filePath(QStringLiteral("cwd.txt"));
        const QString script = fakeDir.filePath(QStringLiteral("yt-dlp"));
        writeFile(script.toStdString(), "#!/bin/sh\npwd > '" + marker.toStdString() + "'\nexit 1\n");
        QFile::setPermissions(script, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

        const QString previous = QDir::currentPath();
        QVERIFY(QDir::setCurrent(launchDir.path()));
        orbit::core::MediaDownloader::setYtDlpPathOverride(script);
        bool finished = false;
        {
            orbit::core::MediaDownloader media(orbit::core::ProgressReporter{});
            media.fetchInfo(QStringLiteral("https://example.com/v"), {}, [&](const orbit::core::MediaInfo&) { finished = true; });
            QTRY_VERIFY_WITH_TIMEOUT(finished, 10000);
        }
        orbit::core::MediaDownloader::setYtDlpPathOverride({});
        QDir::setCurrent(previous);

        const QString ranIn = QString::fromStdString(readFile(marker.toStdString())).trimmed();
        QVERIFY2(!ranIn.isEmpty(), "the stand-in yt-dlp didn't run");
        QVERIFY2(QDir(ranIn).canonicalPath() != QDir(launchDir.path()).canonicalPath(), qPrintable(ranIn));
#endif
    }

    void speedLimiterUnlimitedAndAbort() {
        SpeedLimiter limiter;
        auto start = std::chrono::steady_clock::now();
        limiter.consume(100'000'000);
        QVERIFY(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(50));

        limiter.setLimitBytesPerSec(1000);
        limiter.consume(1000);
        std::atomic<bool> abort{true};
        start = std::chrono::steady_clock::now();
        limiter.consume(1'000'000, &abort);
        QVERIFY(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));
    }
};

QTEST_GUILESS_MAIN(TestUtils)
#include "tst_utils.moc"
