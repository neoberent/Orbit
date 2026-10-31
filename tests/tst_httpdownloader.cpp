#include "TestHttpServer.hpp"
#include "core/HttpDownloader.hpp"

#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <thread>

namespace fs = std::filesystem;
using namespace orbit::core;
using orbit::tests::TestHttpServer;
using namespace std::chrono_literals;

namespace {

QByteArray randomPayload(qsizetype size) {
    QByteArray data(size, Qt::Uninitialized);
    QRandomGenerator gen(static_cast<quint32>(size));
    for (qsizetype i = 0; i < size; ++i) data[i] = static_cast<char>(gen.bounded(256));
    return data;
}

QByteArray readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return QByteArray::fromStdString(bytes);
}

// Collects what a downloader reports, for tests to wait on.
class Recorder {
public:
    ProgressReporter reporter() {
        ProgressReporter r;
        r.onStateChanged = [this](DownloadId id, DownloadState state) {
            std::scoped_lock lock(mutex_);
            states_[id] = state;
            history_[id].push_back(state);
            changed_.notify_all();
        };
        r.onProgress = [this](DownloadId id, const DownloadStats& stats) {
            std::scoped_lock lock(mutex_);
            bytes_[id] = stats.bytesDownloaded;
            changed_.notify_all();
        };
        r.onError = [this](DownloadId id, const std::string& message) {
            std::scoped_lock lock(mutex_);
            errors_[id] = message;
        };
        r.onNameResolved = [this](DownloadId id, const std::string&, const std::string& path) {
            std::scoped_lock lock(mutex_);
            paths_[id] = path;
        };
        return r;
    }

    bool waitForState(DownloadId id, DownloadState state, std::chrono::milliseconds timeout = 20s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return states_.contains(id) && states_[id] == state; });
    }

    bool waitForBytes(DownloadId id, std::uint64_t atLeast, std::chrono::milliseconds timeout = 20s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return bytes_[id] >= atLeast; });
    }

    DownloadState state(DownloadId id) {
        std::scoped_lock lock(mutex_);
        return states_[id];
    }
    int count(DownloadId id, DownloadState state) {
        std::scoped_lock lock(mutex_);
        return static_cast<int>(std::count(history_[id].begin(), history_[id].end(), state));
    }
    std::string error(DownloadId id) {
        std::scoped_lock lock(mutex_);
        return errors_[id];
    }
    fs::path path(DownloadId id) {
        std::scoped_lock lock(mutex_);
        return paths_[id];
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<DownloadId, DownloadState> states_;
    std::map<DownloadId, std::vector<DownloadState>> history_;
    std::map<DownloadId, std::uint64_t> bytes_;
    std::map<DownloadId, std::string> errors_;
    std::map<DownloadId, fs::path> paths_;
};

}

class TestHttpDownloader : public QObject {
    Q_OBJECT

private:
    DownloadRequest request(const QTemporaryDir& dir, const std::string& name = "file.bin", int connections = 8) {
        DownloadRequest r;
        r.source = server_->url();
        r.destinationDir = dir.path().toStdString();
        r.suggestedName = name;
        r.maxConnections = connections;
        return r;
    }

    std::unique_ptr<TestHttpServer> server_;

private slots:
    void init() { server_ = std::make_unique<TestHttpServer>(); }
    void cleanup() { server_.reset(); }

    void segmentedDownloadIsByteExact() {
        const QByteArray payload = randomPayload(6 * 1024 * 1024 + 123);
        server_->setBehaviour({.payload = payload, .bytesPerTick = 64 * 1024});
        QTemporaryDir dir;
        Recorder rec;
        HttpDownloader dl(rec.reporter());
        dl.start(1, request(dir));
        QVERIFY(rec.waitForState(1, DownloadState::Completed));
        const fs::path file = rec.path(1);
        QCOMPARE(readFile(file), payload);
        QVERIFY(!fs::exists(fs::path(file).concat(".orbitresume")));
        QVERIFY2(server_->peakConcurrentGets() > 1, "the download should have used several connections");
        QVERIFY(server_->rangedGetRequests() > 1);
    }

    void serverWithoutRangesDownloadsInOnePiece() {
        const QByteArray payload = randomPayload(3 * 1024 * 1024);
        server_->setBehaviour({.payload = payload, .ranges = false});
        QTemporaryDir dir;
        Recorder rec;
        HttpDownloader dl(rec.reporter());
        dl.start(1, request(dir));
        QVERIFY(rec.waitForState(1, DownloadState::Completed));
        QCOMPARE(readFile(rec.path(1)), payload);
        QCOMPARE(server_->rangedGetRequests(), 0);
    }

    void httpErrorFailsTheDownload() {
        server_->setBehaviour({.payload = randomPayload(1000), .status = 404});
        QTemporaryDir dir;
        Recorder rec;
        HttpDownloader dl(rec.reporter());
        dl.start(1, request(dir));
        QVERIFY(rec.waitForState(1, DownloadState::Error));
        QCOMPARE(rec.count(1, DownloadState::Completed), 0);
    }

    void pauseAndResumeKeepsTheFileIntact() {
        const QByteArray payload = randomPayload(4 * 1024 * 1024);
        server_->setBehaviour({.payload = payload, .bytesPerTick = 16 * 1024});
        QTemporaryDir dir;
        Recorder rec;
        HttpDownloader dl(rec.reporter());
        dl.start(1, request(dir));
        QVERIFY(rec.waitForBytes(1, 512 * 1024));
        dl.pause(1);
        QVERIFY(rec.waitForState(1, DownloadState::Paused));
        const fs::path file = rec.path(1);
        QVERIFY(fs::exists(fs::path(file).concat(".orbitresume")));

        server_->setBehaviour({.payload = payload});
        const int getsBefore = server_->getRequests();
        dl.resume(1);
        QVERIFY(rec.waitForState(1, DownloadState::Completed));
        QCOMPARE(readFile(file), payload);
        QVERIFY(server_->getRequests() > getsBefore);
    }

    void rapidResumesStartOneSupervisor() {
        const QByteArray payload = randomPayload(2 * 1024 * 1024);
        server_->setBehaviour({.payload = payload, .bytesPerTick = 16 * 1024});
        QTemporaryDir dir;
        Recorder rec;
        HttpDownloader dl(rec.reporter());
        dl.start(1, request(dir));
        QVERIFY(rec.waitForBytes(1, 256 * 1024));
        dl.pause(1);
        QVERIFY(rec.waitForState(1, DownloadState::Paused));

        server_->setBehaviour({.payload = payload});
        for (int i = 0; i < 5; ++i) dl.resume(1);
        QVERIFY(rec.waitForState(1, DownloadState::Completed));
        std::this_thread::sleep_for(300ms);
        QCOMPARE(rec.count(1, DownloadState::Completed), 1);
        QCOMPARE(readFile(rec.path(1)), payload);
    }

    void pauseWhileConnectingCreatesNoFile() {
        const QByteArray payload = randomPayload(256 * 1024);
        server_->setBehaviour({.payload = payload, .headerDelayMs = 400});
        QTemporaryDir dir;
        Recorder rec;
        HttpDownloader dl(rec.reporter());
        dl.start(1, request(dir, "late.bin"));
        QVERIFY(rec.waitForState(1, DownloadState::Connecting));
        dl.pause(1);
        QVERIFY(rec.waitForState(1, DownloadState::Paused));
        QVERIFY(!fs::exists(fs::path(dir.path().toStdString()) / "late.bin"));

        server_->setBehaviour({.payload = payload});
        dl.resume(1);
        QVERIFY(rec.waitForState(1, DownloadState::Completed));
        QCOMPARE(readFile(rec.path(1)), payload);
    }

    void cancelRemovesPartialFiles() {
        server_->setBehaviour({.payload = randomPayload(4 * 1024 * 1024), .bytesPerTick = 8 * 1024});
        QTemporaryDir dir;
        Recorder rec;
        HttpDownloader dl(rec.reporter());
        dl.start(1, request(dir));
        QVERIFY(rec.waitForBytes(1, 64 * 1024));
        const fs::path file = rec.path(1);
        dl.cancel(1);
        QVERIFY(rec.waitForState(1, DownloadState::Cancelled));
        QVERIFY(!fs::exists(file));
        QVERIFY(!fs::exists(fs::path(file).concat(".orbitresume")));
    }

    void changedRemoteSizeStartsOver_data() {
        QTest::addColumn<int>("newSize");
        QTest::newRow("shrank") << 2 * 1024 * 1024 + 7;
        QTest::newRow("grew") << 5 * 1024 * 1024 + 3;
    }

    // The file on the server was replaced while the download was paused. Resuming must not
    // splice the old bytes onto the new file.
    void changedRemoteSizeStartsOver() {
        QFETCH(int, newSize);
        const QByteArray first = randomPayload(3 * 1024 * 1024);
        server_->setBehaviour({.payload = first, .bytesPerTick = 16 * 1024});
        QTemporaryDir dir;
        Recorder rec;
        HttpDownloader dl(rec.reporter());
        dl.start(1, request(dir));
        QVERIFY(rec.waitForBytes(1, 256 * 1024));
        dl.pause(1);
        QVERIFY(rec.waitForState(1, DownloadState::Paused));

        const QByteArray second = randomPayload(newSize);
        server_->setBehaviour({.payload = second});
        dl.resume(1);
        QVERIFY2(rec.waitForState(1, DownloadState::Completed), rec.error(1).c_str());
        QCOMPARE(readFile(rec.path(1)), second);
    }

    void probeReportsSizeRangesAndName() {
        server_->setBehaviour({.payload = randomPayload(12345), .fileName = QStringLiteral("report final.pdf")});
        Recorder rec;
        HttpDownloader dl(rec.reporter());
        DownloadRequest r;
        r.source = server_->url();
        const RemoteProbe probe = dl.probe(r);
        QVERIFY2(probe.ok, probe.error.c_str());
        QCOMPARE(probe.size, std::uint64_t(12345));
        QVERIFY(probe.supportsRanges);
        QCOMPARE(probe.fileName, std::string("report final.pdf"));
    }

    void rewriteResumeSourceKeepsTheRest() {
        QTemporaryDir dir;
        const fs::path meta = fs::path(dir.path().toStdString()) / "a.bin.orbitresume";
        {
            std::ofstream out(meta);
            out << "source=https://old/link\ntotal=100\n";
        }
        QVERIFY(HttpDownloader::rewriteResumeSource(meta, "https://new/link"));
        const QByteArray text = readFile(meta);
        QVERIFY(text.contains("source=https://new/link\n"));
        QVERIFY(text.contains("total=100\n"));
        QVERIFY(!text.contains("old"));
        QVERIFY(!HttpDownloader::rewriteResumeSource(meta.parent_path() / "missing", "x"));
    }

    // Regression: the destructor used to give up after 3 s and free jobs that detached
    // supervisor threads were still using. It must wait for them, and they must stop
    // quickly when asked.
    void destroyingMidDownloadPausesEverything() {
        const QByteArray payload = randomPayload(8 * 1024 * 1024);
        server_->setBehaviour({.payload = payload, .bytesPerTick = 4 * 1024});
        QTemporaryDir dir;
        Recorder rec;
        auto dl = std::make_unique<HttpDownloader>(rec.reporter());
        for (DownloadId id = 1; id <= 3; ++id) dl->start(id, request(dir, "f" + std::to_string(id) + ".bin"));
        for (DownloadId id = 1; id <= 3; ++id) QVERIFY(rec.waitForBytes(id, 32 * 1024));

        const auto start = std::chrono::steady_clock::now();
        dl.reset();
        QVERIFY(std::chrono::steady_clock::now() - start < 5s);
        for (DownloadId id = 1; id <= 3; ++id) {
            QCOMPARE(rec.state(id), DownloadState::Paused);
            QVERIFY(fs::exists(rec.path(id).concat(".orbitresume")));
        }
    }

    void destroyingDuringAHangingProbeIsQuick() {
        server_->setBehaviour({.payload = randomPayload(1000), .hang = true});
        QTemporaryDir dir;
        Recorder rec;
        auto dl = std::make_unique<HttpDownloader>(rec.reporter());
        dl->start(1, request(dir));
        QVERIFY(rec.waitForState(1, DownloadState::Connecting));
        std::this_thread::sleep_for(200ms);

        const auto start = std::chrono::steady_clock::now();
        dl.reset();
        QVERIFY(std::chrono::steady_clock::now() - start < 3s);
        QCOMPARE(rec.state(1), DownloadState::Paused);
        QVERIFY(!fs::exists(fs::path(dir.path().toStdString()) / "file.bin"));
    }

    void destroyingRightAfterStartIsSafe() {
        server_->setBehaviour({.payload = randomPayload(512 * 1024), .bytesPerTick = 8 * 1024});
        QTemporaryDir dir;
        for (int round = 0; round < 20; ++round) {
            Recorder rec;
            auto dl = std::make_unique<HttpDownloader>(rec.reporter());
            dl->start(1, request(dir, "r" + std::to_string(round) + ".bin"));
            dl->start(2, request(dir, "s" + std::to_string(round) + ".bin"));
            dl.reset();
        }
    }
};

QTEST_GUILESS_MAIN(TestHttpDownloader)
#include "tst_httpdownloader.moc"
