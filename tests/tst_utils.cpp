#include "core/MediaDownloader.hpp"
#include "core/ProtocolHandler.hpp"
#include "utils/FileUtils.hpp"
#include "utils/Hashing.hpp"
#include "utils/SpeedLimiter.hpp"
#include "utils/StringUtils.hpp"

#include <QDir>
#include <QFile>
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
