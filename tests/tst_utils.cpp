#include "core/InterfaceDns.hpp"
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
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <clocale>
#include <fstream>
#include <thread>

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
