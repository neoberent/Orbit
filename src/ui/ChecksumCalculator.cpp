#include "ChecksumCalculator.hpp"

#include "utils/Hashing.hpp"

#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>

#include <chrono>

using orbit::utils::HashAlgorithm;

ChecksumCalculator::ChecksumCalculator(QObject* parent) : QObject(parent) {}

ChecksumCalculator::~ChecksumCalculator() { stopWorker(); }

void ChecksumCalculator::stopWorker() {
    ++*generation_;
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

void ChecksumCalculator::start(const QString& path) {
    stopWorker();
    filePath_ = path;
    md5_.clear();
    sha1_.clear();
    sha256_.clear();
    error_.clear();
    progress_ = 0;

    if (!QFileInfo(path).isFile()) {
        running_ = false;
        error_ = QStringLiteral("The file no longer exists");
        emit changed();
        return;
    }
    running_ = true;
    emit changed();

    const int generation = *generation_;
    auto generationRef = generation_;
    QPointer<ChecksumCalculator> self(this);
    const std::filesystem::path file = path.toStdWString();

    worker_ = std::jthread([self, file, generation, generationRef](std::stop_token stop) {
        auto lastReport = std::chrono::steady_clock::now();
        const auto digests = orbit::utils::hashFile(
            file, {HashAlgorithm::Md5, HashAlgorithm::Sha1, HashAlgorithm::Sha256},
            [&](std::uint64_t done, std::uint64_t total) {
                if (stop.stop_requested()) return false;
                const auto now = std::chrono::steady_clock::now();
                if (now - lastReport > std::chrono::milliseconds(100) && total > 0) {
                    lastReport = now;
                    const double fraction = static_cast<double>(done) / static_cast<double>(total);
                    QMetaObject::invokeMethod(self, [self, fraction, generation, generationRef] {
                        if (!self || *generationRef != generation) return;
                        self->progress_ = fraction;
                        emit self->changed();
                    }, Qt::QueuedConnection);
                }
                return true;
            });
        if (stop.stop_requested()) return;
        QMetaObject::invokeMethod(self, [self, digests, generation, generationRef] {
            if (!self || *generationRef != generation) return;
            self->running_ = false;
            self->progress_ = 1;
            if (!digests || digests->size() != 3) {
                self->error_ = QStringLiteral("Couldn't read the file");
            } else {
                self->md5_ = QString::fromStdString((*digests)[0]);
                self->sha1_ = QString::fromStdString((*digests)[1]);
                self->sha256_ = QString::fromStdString((*digests)[2]);
            }
            emit self->changed();
        }, Qt::QueuedConnection);
    });
}

void ChecksumCalculator::cancel() {
    stopWorker();
    if (running_) {
        running_ = false;
        error_ = QStringLiteral("Cancelled");
        emit changed();
    }
}

QString ChecksumCalculator::compare(const QString& expected) const {
    const auto spec = orbit::utils::parseChecksum(expected.toStdString());
    if (!spec) return QStringLiteral("invalid");
    const QString hex = QString::fromStdString(spec->hex);
    switch (spec->algorithm) {
        case HashAlgorithm::Md5: return hex == md5_ ? QStringLiteral("match") : QStringLiteral("mismatch");
        case HashAlgorithm::Sha1: return hex == sha1_ ? QStringLiteral("match") : QStringLiteral("mismatch");
        case HashAlgorithm::Sha256: return hex == sha256_ ? QStringLiteral("match") : QStringLiteral("mismatch");
        case HashAlgorithm::Sha512: return QStringLiteral("unsupported");
    }
    return QStringLiteral("invalid");
}
