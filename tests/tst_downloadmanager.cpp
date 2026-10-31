#include "TestHttpServer.hpp"
#include "core/DownloadManager.hpp"
#include "utils/FileUtils.hpp"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using namespace orbit::core;
using orbit::tests::TestHttpServer;
using namespace std::chrono_literals;

namespace {

QByteArray randomPayload(qsizetype size) {
    QByteArray data(size, Qt::Uninitialized);
    QRandomGenerator gen(static_cast<quint32>(size) * 7u + 1u);
    for (qsizetype i = 0; i < size; ++i) data[i] = static_cast<char>(gen.bounded(256));
    return data;
}

QByteArray readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return QByteArray::fromStdString(bytes);
}

template <typename Pred>
bool waitUntil(Pred pred, std::chrono::milliseconds timeout = 30s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred();
}

DownloadState stateOf(const DownloadManager& m, DownloadId id) {
    const auto snap = m.snapshot(id);
    return snap ? snap->state : DownloadState::Cancelled;
}

}

class TestDownloadManager : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<TestHttpServer> server_;
    std::unique_ptr<QTemporaryDir> appData_;
    std::unique_ptr<QTemporaryDir> downloads_;

    std::unique_ptr<DownloadManager> makeManager() {
        auto m = std::make_unique<DownloadManager>(fs::path(appData_->path().toStdString()) / "tasks");
        m->setDefaultDownloadDir(downloads_->path().toStdString());
        return m;
    }

    DownloadRequest request(const std::string& name) const {
        DownloadRequest r;
        r.source = server_->url("/" + name);
        r.suggestedName = name;
        return r;
    }

    fs::path downloadPath(const std::string& name) const { return fs::path(downloads_->path().toStdString()) / name; }

private slots:
    void initTestCase() {
        appData_ = std::make_unique<QTemporaryDir>();
        orbit::utils::setAppDataDirOverride(appData_->path().toStdString());
    }

    void init() {
        server_ = std::make_unique<TestHttpServer>();
        downloads_ = std::make_unique<QTemporaryDir>();
        fs::remove_all(fs::path(appData_->path().toStdString()) / "tasks");
    }

    void cleanup() {
        server_.reset();
        downloads_.reset();
    }

    void completesAndVerifiesChecksum() {
        const QByteArray payload = randomPayload(2 * 1024 * 1024);
        server_->setBehaviour({.payload = payload});
        auto m = makeManager();
        DownloadRequest r = request("good.bin");
        r.expectedChecksum = "sha256:" + QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex().toStdString();
        const DownloadId id = m->addHttpDownload(r);
        QVERIFY(waitUntil([&] { return m->snapshot(id)->checksumState == ChecksumState::Match; }));
        QCOMPARE(stateOf(*m, id), DownloadState::Completed);
        QCOMPARE(readFile(downloadPath("good.bin")), payload);
    }

    void flagsAChecksumMismatch() {
        server_->setBehaviour({.payload = randomPayload(64 * 1024)});
        auto m = makeManager();
        DownloadRequest r = request("bad.bin");
        r.expectedChecksum = "sha256:" + std::string(64, '0');
        const DownloadId id = m->addHttpDownload(r);
        QVERIFY(waitUntil([&] { return m->snapshot(id)->checksumState == ChecksumState::Mismatch; }));
    }

    void existingFileIsKeptAndTheNewOneRenamed() {
        const QByteArray payload = randomPayload(100 * 1024);
        server_->setBehaviour({.payload = payload});
        {
            std::ofstream out(downloadPath("dup.bin"));
            out << "keep me";
        }
        auto m = makeManager();
        const DownloadId id = m->addHttpDownload(request("dup.bin"));
        QVERIFY(waitUntil([&] { return stateOf(*m, id) == DownloadState::Completed; }));
        QCOMPARE(readFile(downloadPath("dup.bin")), QByteArray("keep me"));
        QCOMPARE(readFile(downloadPath("dup (1).bin")), payload);
    }

    void queueRespectsTheConcurrencyLimit() {
        const QByteArray payload = randomPayload(512 * 1024);
        server_->setBehaviour({.payload = payload, .bytesPerTick = 32 * 1024});
        auto m = makeManager();
        m->setMaxConcurrentDownloads(1);
        std::vector<DownloadId> ids;
        for (int i = 0; i < 3; ++i) ids.push_back(m->addHttpDownload(request("q" + std::to_string(i) + ".bin")));

        int peakActive = 0;
        QVERIFY(waitUntil([&] {
            int active = 0;
            int done = 0;
            for (DownloadId id : ids) {
                const DownloadState s = stateOf(*m, id);
                if (s == DownloadState::Downloading || s == DownloadState::Connecting) ++active;
                if (s == DownloadState::Completed) ++done;
            }
            peakActive = std::max(peakActive, active);
            return done == 3;
        }, 60s));
        QCOMPARE(peakActive, 1);
        for (int i = 0; i < 3; ++i) QCOMPARE(readFile(downloadPath("q" + std::to_string(i) + ".bin")), payload);
    }

    void renameAndRemoveWithFiles() {
        server_->setBehaviour({.payload = randomPayload(10 * 1024)});
        auto m = makeManager();
        const DownloadId id = m->addHttpDownload(request("old.bin"));
        QVERIFY(waitUntil([&] { return stateOf(*m, id) == DownloadState::Completed; }));

        QCOMPARE(m->rename(id, "new.bin"), std::string());
        QVERIFY(!fs::exists(downloadPath("old.bin")));
        QVERIFY(fs::exists(downloadPath("new.bin")));
        QCOMPARE(m->snapshot(id)->name, std::string("new.bin"));

        m->remove(id, true);
        QVERIFY(waitUntil([&] { return !m->snapshot(id).has_value(); }));
        QVERIFY(waitUntil([&] { return !fs::exists(downloadPath("new.bin")); }));
    }

    void pauseResumeAndCancel() {
        const QByteArray payload = randomPayload(2 * 1024 * 1024);
        server_->setBehaviour({.payload = payload, .bytesPerTick = 16 * 1024});
        auto m = makeManager();
        const DownloadId id = m->addHttpDownload(request("pr.bin"));
        QVERIFY(waitUntil([&] { return m->snapshot(id)->stats.bytesDownloaded > 128 * 1024; }));
        m->pause(id);
        QVERIFY(waitUntil([&] { return stateOf(*m, id) == DownloadState::Paused; }));
        server_->setBehaviour({.payload = payload});
        m->resume(id);
        QVERIFY(waitUntil([&] { return stateOf(*m, id) == DownloadState::Completed; }));
        QCOMPARE(readFile(downloadPath("pr.bin")), payload);

        server_->setBehaviour({.payload = payload, .bytesPerTick = 8 * 1024});
        const DownloadId second = m->addHttpDownload(request("cancel.bin"));
        QVERIFY(waitUntil([&] { return m->snapshot(second)->stats.bytesDownloaded > 0; }));
        m->cancel(second);
        QVERIFY(waitUntil([&] { return stateOf(*m, second) == DownloadState::Cancelled; }));
        QVERIFY(waitUntil([&] { return !fs::exists(downloadPath("cancel.bin")); }));
    }

    // Settings > About > Backup: the download list exported from one profile imports into
    // another. Unfinished downloads arrive paused, and importing twice adds nothing.
    void exportAndImportRoundTrip() {
        const QByteArray payload = randomPayload(64 * 1024);
        server_->setBehaviour({.payload = payload});
        std::vector<DownloadManager::TaskRecord> records;
        {
            auto m = makeManager();
            const DownloadId done = m->addHttpDownload(request("done.bin"));
            QVERIFY(waitUntil([&] { return stateOf(*m, done) == DownloadState::Completed; }));
            server_->setBehaviour({.payload = payload, .bytesPerTick = 1024});
            const DownloadId pending = m->addHttpDownload(request("pending.bin"));
            m->pause(pending);
            QVERIFY(waitUntil([&] { return stateOf(*m, pending) == DownloadState::Paused; }));
            records = m->exportRecords();
        }
        QCOMPARE(records.size(), size_t(2));

        fs::remove_all(fs::path(appData_->path().toStdString()) / "tasks");
        auto m = makeManager();
        QCOMPARE(m->snapshot().size(), size_t(0));
        QCOMPARE(m->importRecords(records), 2);
        QCOMPARE(m->importRecords(records), 0);
        int completed = 0;
        int paused = 0;
        for (const TaskSnapshot& snap : m->snapshot()) {
            if (snap.state == DownloadState::Completed) ++completed;
            if (snap.state == DownloadState::Paused) ++paused;
        }
        QCOMPARE(completed, 1);
        QCOMPARE(paused, 1);
    }

    // Closing Orbit mid-download and opening it again picks the download up where it
    // stopped (the manager and HttpDownloader destructors both run here).
    void restartMidDownloadResumes() {
        const QByteArray payload = randomPayload(4 * 1024 * 1024);
        server_->setBehaviour({.payload = payload, .bytesPerTick = 16 * 1024});
        DownloadId id = 0;
        {
            auto m = makeManager();
            id = m->addHttpDownload(request("restart.bin"));
            QVERIFY(waitUntil([&] { return m->snapshot(id)->stats.bytesDownloaded > 256 * 1024; }));
        }
        QVERIFY(fs::exists(downloadPath("restart.bin.orbitresume")));

        server_->setBehaviour({.payload = payload});
        const int getsBefore = server_->getRequests();
        auto m = makeManager();
        QVERIFY(m->snapshot(id).has_value());
        QVERIFY(waitUntil([&] { return stateOf(*m, id) == DownloadState::Completed; }));
        QCOMPARE(readFile(downloadPath("restart.bin")), payload);
        QVERIFY(server_->getRequests() > getsBefore);
        QVERIFY(server_->rangedGetRequests() > 0);
    }
};

QTEST_GUILESS_MAIN(TestDownloadManager)
#include "tst_downloadmanager.moc"
