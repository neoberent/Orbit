#include "FileChecks.hpp"

#include "SignatureCheck.hpp"
#include "core/DownloadManager.hpp"
#include "core/HttpDownloader.hpp"
#include "utils/Hashing.hpp"
#include "utils/Logger.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>

#include <filesystem>
#include <optional>

using orbit::core::DownloadState;
using orbit::core::DownloadType;
using signature_check::State;

namespace {

// Sidecars a publisher puts beside a release file, checked in this order.
const char* const kSidecars[] = {".asc", ".sig", ".minisig"};
constexpr std::size_t kMaxSignatureBytes = 64 * 1024;
constexpr int kScanTimeoutMs = 30 * 60 * 1000;
constexpr int kGpgTimeoutMs = 60 * 1000;
// Bigger files than this aren't worth a full ClamAV run (clamscan would skip them anyway).
constexpr qint64 kMaxScanBytes = qint64(1) << 30;
// Files looked at in the download's folder for a duplicate.
constexpr int kMaxFolderEntries = 5000;

// Runs `work` on the main thread later, if `guard` is still alive by then. Safe to call
// from any thread: the pointer is only read on the main thread.
template <typename Work>
void onMainThread(QPointer<FileChecks> guard, Work work) {
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [guard, work = std::move(work)]() mutable {
            if (guard) work();
        },
        Qt::QueuedConnection);
}

// Runs a program and calls `done` exactly once: with its exit code and output, or with
// started = false when it couldn't run or was killed (timeout).
void runProcess(QObject* parent, const QString& program, const QStringList& args, int timeoutMs,
                std::function<void(bool started, int code, const QByteArray& output)> done) {
    auto* process = new QProcess(parent);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process->setProcessEnvironment(env);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->setProgram(program);
    process->setArguments(args);
    auto finish = std::make_shared<std::function<void(bool, int, const QByteArray&)>>(std::move(done));
    auto* timer = new QTimer(process);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, process, [process] { process->kill(); });
    QObject::connect(process, &QProcess::finished, parent, [process, finish](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        if (auto callback = std::exchange(*finish, nullptr)) {
            callback(status == QProcess::NormalExit, code, process->readAllStandardOutput() + process->readAllStandardError());
        }
    });
    QObject::connect(process, &QProcess::errorOccurred, parent, [process, finish](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;  // the others end in finished()
        process->deleteLater();
        if (auto callback = std::exchange(*finish, nullptr)) callback(false, -1, {});
    });
    timer->start(timeoutMs);
    process->start();
}

bool sameFile(const QString& a, const QString& b) {
    std::error_code ec;
    return std::filesystem::equivalent(std::filesystem::path(a.toStdString()), std::filesystem::path(b.toStdString()), ec);
}

}

struct FileChecks::Job {
    qlonglong id{0};
    QString path;
    QString name;
    QString source;
    DownloadType type{DownloadType::Http};
    bool isFile{false};
    qint64 size{0};
    bool proceed{true};
    std::function<void(bool)> done;
    std::unique_ptr<QTemporaryFile> signatureFile;
};

FileChecks::FileChecks(orbit::core::DownloadManager& manager, QObject* parent) : QObject(parent), manager_(manager) {
    pool_.setMaxThreadCount(1);  // one file hashed at a time; disks don't like more
}

FileChecks::~FileChecks() {
    stopping_->store(true);
    pool_.clear();
    pool_.waitForDone();
}

bool FileChecks::clamavAvailable() {
    return !QStandardPaths::findExecutable(QStringLiteral("clamscan")).isEmpty() ||
           !QStandardPaths::findExecutable(QStringLiteral("clamdscan")).isEmpty();
}

QStringList FileChecks::threatsIn(const QByteArray& output) {
    QStringList threats;
    static const QRegularExpression found(QStringLiteral("^.*: (.+) FOUND$"));
    for (const QByteArray& line : output.split('\n')) {
        const auto match = found.match(QString::fromUtf8(line).trimmed());
        if (match.hasMatch() && !threats.contains(match.captured(1))) threats << match.captured(1);
    }
    return threats;
}

void FileChecks::setResult(qlonglong id, const QVariantMap& values) {
    QVariantMap& result = results_[id];
    for (auto it = values.cbegin(); it != values.cend(); ++it) result.insert(it.key(), it.value());
    ++revision_;
    emit changed();
}

void FileChecks::check(qlonglong id, std::function<void(bool proceed)> done) {
    const auto snap = manager_.snapshot(static_cast<orbit::core::DownloadId>(id));
    if (!snap || snap->state != DownloadState::Completed || snap->destinationPath.empty()) {
        if (done) done(true);
        return;
    }
    auto job = std::make_shared<Job>();
    job->id = id;
    job->path = QString::fromStdString(snap->destinationPath);
    job->name = QString::fromStdString(snap->name);
    job->source = QString::fromStdString(snap->source);
    job->type = snap->type;
    const QFileInfo info(job->path);
    if (!info.exists()) {
        if (done) done(true);
        return;
    }
    job->isFile = info.isFile();
    job->size = info.size();
    if (job->name.isEmpty()) job->name = info.fileName();
    job->done = std::move(done);
    results_.remove(id);
    scan(job);
}

void FileChecks::scan(const std::shared_ptr<Job>& job) {
    if (!settings_.scan) {
        signature(job, 0);
        return;
    }
    const QString clamdscan = QStandardPaths::findExecutable(QStringLiteral("clamdscan"));
    const QString clamscan = QStandardPaths::findExecutable(QStringLiteral("clamscan"));
    if (clamdscan.isEmpty() && clamscan.isEmpty()) {
        setResult(job->id, {{QStringLiteral("scan"), QStringLiteral("unavailable")}});
        if (!warnedNoClamav_) {
            warnedNoClamav_ = true;
            emit notice(QStringLiteral("ClamAV isn't installed"),
                        QStringLiteral("Install clamav to scan downloads, or turn the scan off in Settings › Downloads."), false);
        }
        signature(job, 0);
        return;
    }
    if (job->isFile && job->size > kMaxScanBytes) {
        setResult(job->id, {{QStringLiteral("scan"), QStringLiteral("skipped")},
                            {QStringLiteral("scanDetail"), QStringLiteral("Over 1 GB, too big for ClamAV")}});
        signature(job, 0);
        return;
    }

    setResult(job->id, {{QStringLiteral("scan"), QStringLiteral("scanning")}});
    // The daemon is much faster (its signatures are already loaded); clamscan loads them
    // for every run. clamdscan exits with 2 when no daemon is listening.
    auto run = std::make_shared<std::function<void(bool)>>();
    *run = [this, job, clamdscan, clamscan, weakRun = std::weak_ptr<std::function<void(bool)>>(run)](bool daemon) {
        QStringList args{QStringLiteral("--no-summary"), QStringLiteral("--infected")};
        if (daemon) {
            args << QStringLiteral("--fdpass");
        } else {
            args << QStringLiteral("--recursive") << QStringLiteral("--max-filesize=1024M") << QStringLiteral("--max-scansize=1024M");
        }
        args << QStringLiteral("--") << job->path;
        auto again = weakRun.lock();
        runProcess(this, daemon ? clamdscan : clamscan, args, kScanTimeoutMs,
                   [this, job, daemon, clamscan, again](bool started, int code, const QByteArray& output) {
            if (started && code == 1) {
                const QStringList threats = threatsIn(output);
                const QString threat = threats.isEmpty() ? QStringLiteral("a threat") : threats.join(QStringLiteral(", "));
                job->proceed = false;
                setResult(job->id, {{QStringLiteral("scan"), QStringLiteral("infected")}, {QStringLiteral("threat"), threat}});
                ORBIT_LOG_WARN("checks", "ClamAV found " + threat.toStdString() + " in download " + std::to_string(job->id));
                emit notice(QStringLiteral("Threat found"),
                            job->name + QStringLiteral(": ") + threat +
                                QStringLiteral(". The file was left where it is; Orbit didn't open or unpack it."),
                            false);
            } else if (started && code == 0) {
                setResult(job->id, {{QStringLiteral("scan"), QStringLiteral("clean")}});
            } else if (daemon && !clamscan.isEmpty() && again) {
                (*again)(false);  // no daemon running: scan the slow way
                return;
            } else {
                QString detail = QString::fromUtf8(output).trimmed().section(QLatin1Char('\n'), -1).left(200);
                if (detail.isEmpty()) detail = started ? QStringLiteral("ClamAV exited with %1").arg(code) : QStringLiteral("ClamAV didn't run or took too long");
                setResult(job->id, {{QStringLiteral("scan"), QStringLiteral("failed")}, {QStringLiteral("scanDetail"), detail}});
                ORBIT_LOG_WARN("checks", "ClamAV couldn't scan download " + std::to_string(job->id) + ": " + detail.toStdString());
            }
            signature(job, 0);
        });
    };
    // The running scan's callback holds `run` (strongly) while the process runs, so the
    // retry with clamscan can still reach it; nothing holds it once both are done.
    (*run)(!clamdscan.isEmpty());
}

void FileChecks::signature(const std::shared_ptr<Job>& job, int sidecar) {
    const bool eligible = settings_.signatures && job->isFile && job->type == DownloadType::Http &&
                          (job->source.startsWith(QLatin1String("https://"), Qt::CaseInsensitive) ||
                           job->source.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)) &&
                          !job->source.contains(QLatin1Char('?')) && !job->source.contains(QLatin1Char('#')) &&
                          orbit::core::HttpDownloader::isReleaseFileName(QUrl(job->source).fileName().toStdString());
    if (!eligible || sidecar >= static_cast<int>(std::size(kSidecars))) {
        proceed(job);
        return;
    }
    const std::string url = job->source.toStdString() + kSidecars[sidecar];
    QPointer<FileChecks> guard(this);
    manager_.fetchSmall(url, kMaxSignatureBytes, [guard, job, sidecar](bool ok, const std::string& body, const std::string&) {
        onMainThread(guard, [guard, job, sidecar, ok, signature = QByteArray::fromStdString(body)] {
            FileChecks* self = guard.data();
            if (!ok || signature.isEmpty()) {
                self->signature(job, sidecar + 1);
                return;
            }
            if (signature_check::looksLikeMinisign(signature)) {
                self->verifyMinisign(job, signature, sidecar);
            } else if (QByteArray(kSidecars[sidecar]) == ".minisig") {
                self->signature(job, sidecar + 1);
            } else {
                self->verifyOpenPgp(job, signature, sidecar);
            }
        });
    });
}

void FileChecks::verifyOpenPgp(const std::shared_ptr<Job>& job, const QByteArray& signature, int sidecar) {
    const QString gpg = QStandardPaths::findExecutable(QStringLiteral("gpg"));
    if (gpg.isEmpty()) {
        finishSignature(job, {{QStringLiteral("signature"), QStringLiteral("failed")},
                              {QStringLiteral("signatureKind"), QStringLiteral("openpgp")},
                              {QStringLiteral("signatureDetail"), QStringLiteral("GnuPG isn't installed")}});
        return;
    }
    job->signatureFile = std::make_unique<QTemporaryFile>(QDir::tempPath() + QStringLiteral("/orbit-signature-XXXXXX"));
    if (!job->signatureFile->open() || job->signatureFile->write(signature) != signature.size() || !job->signatureFile->flush()) {
        finishSignature(job, {{QStringLiteral("signature"), QStringLiteral("failed")},
                              {QStringLiteral("signatureKind"), QStringLiteral("openpgp")},
                              {QStringLiteral("signatureDetail"), QStringLiteral("Couldn't save the signature")}});
        return;
    }
    // Only the user's own keyring: never ask a keyserver (that would also be a request
    // outside a locked network interface).
    const QStringList args{QStringLiteral("--batch"), QStringLiteral("--no-tty"), QStringLiteral("--status-fd"), QStringLiteral("1"),
                           QStringLiteral("--no-auto-key-retrieve"), QStringLiteral("--verify"), job->signatureFile->fileName(),
                           job->path};
    runProcess(this, gpg, args, kGpgTimeoutMs, [this, job, sidecar](bool, int, const QByteArray& output) {
        job->signatureFile.reset();
        const auto outcome = signature_check::parseGpgStatus(output);
        if (outcome.state == State::NotASignature) {
            this->signature(job, sidecar + 1);
            return;
        }
        QVariantMap result{{QStringLiteral("signature"), QString::fromLatin1(signature_check::toString(outcome.state))},
                           {QStringLiteral("signatureKind"), QStringLiteral("openpgp")},
                           {QStringLiteral("signer"), outcome.signer},
                           {QStringLiteral("keyId"), outcome.keyId}};
        if (!outcome.detail.isEmpty()) result.insert(QStringLiteral("signatureDetail"), outcome.detail);
        finishSignature(job, result);
    });
}

void FileChecks::verifyMinisign(const std::shared_ptr<Job>& job, const QByteArray& signature, int sidecar) {
    const QStringList keys = settings_.minisignKeys;
    QPointer<FileChecks> guard(this);
    auto stopping = stopping_;
    setResult(job->id, {{QStringLiteral("signature"), QStringLiteral("checking")}});
    // Reading and hashing the file takes a while for a big one: off the main thread.
    pool_.start([guard, job, signature, sidecar, keys, stopping] {
        const auto outcome = signature_check::verifyMinisign(job->path, signature, keys, [stopping] { return stopping->load(); });
        if (stopping->load()) return;
        onMainThread(guard, [guard, job, sidecar, outcome] {
            if (outcome.state == State::NotASignature) {
                guard->signature(job, sidecar + 1);
                return;
            }
            QVariantMap result{{QStringLiteral("signature"), QString::fromLatin1(signature_check::toString(outcome.state))},
                               {QStringLiteral("signatureKind"), QStringLiteral("minisign")},
                               {QStringLiteral("signer"), outcome.signer},
                               {QStringLiteral("keyId"), outcome.keyId}};
            if (!outcome.detail.isEmpty()) result.insert(QStringLiteral("signatureDetail"), outcome.detail);
            guard->finishSignature(job, result);
        });
    });
}

void FileChecks::finishSignature(const std::shared_ptr<Job>& job, const QVariantMap& result) {
    setResult(job->id, result);
    const QString state = result.value(QStringLiteral("signature")).toString();
    const QString signer = result.value(QStringLiteral("signer")).toString();
    ORBIT_LOG_INFO("checks", "Signature of download " + std::to_string(job->id) + ": " + state.toStdString() +
                                 (signer.isEmpty() ? std::string() : " (" + signer.toStdString() + ")"));
    // Only problems are worth a notification; a good signature shows in the details.
    if (state == QLatin1String("bad")) {
        job->proceed = false;
        emit notice(QStringLiteral("Bad signature"),
                    job->name + QStringLiteral(" doesn't match the signature published with it, so it may have been changed. "
                                               "Orbit didn't open or unpack it."),
                    false);
    } else if (state == QLatin1String("revoked")) {
        job->proceed = false;
        emit notice(QStringLiteral("Signed with a revoked key"),
                    job->name + QStringLiteral(" was signed by ") + (signer.isEmpty() ? QStringLiteral("a key") : signer) +
                        QStringLiteral(", which its owner has revoked. Orbit didn't open or unpack it."),
                    false);
    }
    proceed(job);
}

void FileChecks::proceed(const std::shared_ptr<Job>& job) {
    if (auto done = std::exchange(job->done, nullptr)) done(job->proceed);
    if (settings_.duplicates) duplicates(job);
}

void FileChecks::duplicates(const std::shared_ptr<Job>& job) {
    if (!job->isFile || job->size <= 0) return;

    // Other finished downloads and the files beside this one that have the same size.
    QStringList candidates;
    auto consider = [&](const QString& path) {
        if (path.isEmpty() || candidates.contains(path) || path == job->path) return;
        const QFileInfo info(path);
        if (!info.isFile() || info.isSymLink() || info.size() != job->size) return;
        // A download still in progress already has its full size on disk (Orbit reserves
        // the space), so a file with a resume record beside it isn't finished.
        if (QFileInfo::exists(path + QStringLiteral(".orbitresume"))) return;
        if (sameFile(path, job->path)) return;  // a hard link to it isn't a second copy
        candidates << path;
    };
    for (const auto& snap : manager_.snapshot()) {
        if (static_cast<qlonglong>(snap.id) == job->id || snap.state != DownloadState::Completed) continue;
        consider(QString::fromStdString(snap.destinationPath));
    }
    const QFileInfoList siblings = QDir(QFileInfo(job->path).absolutePath())
                                       .entryInfoList(QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDir::Unsorted);
    for (qsizetype i = 0; i < siblings.size() && i < kMaxFolderEntries; ++i) consider(siblings[i].absoluteFilePath());
    if (candidates.isEmpty()) return;

    QPointer<FileChecks> guard(this);
    auto stopping = stopping_;
    pool_.start([guard, job, candidates, stopping] {
        const auto keepGoing = [stopping](std::uint64_t, std::uint64_t) { return !stopping->load(); };
        const auto mine = orbit::utils::hashFile(job->path.toStdString(), {orbit::utils::HashAlgorithm::Sha256}, keepGoing);
        if (!mine || mine->empty() || stopping->load()) return;
        for (const QString& candidate : candidates) {
            const auto theirs = orbit::utils::hashFile(candidate.toStdString(), {orbit::utils::HashAlgorithm::Sha256}, keepGoing);
            if (stopping->load()) return;
            if (!theirs || theirs->empty() || (*theirs)[0] != (*mine)[0]) continue;
            onMainThread(guard, [guard, job, candidate] {
                // Still the same file at the same place? The user may have acted already.
                if (!QFileInfo::exists(job->path) || !QFileInfo::exists(candidate)) return;
                guard->setResult(job->id, {{QStringLiteral("duplicateOf"), candidate}});
                ORBIT_LOG_INFO("checks", "Download " + std::to_string(job->id) + " is the same file as " + candidate.toStdString());
                emit guard->duplicateFound(job->id, job->name, candidate);
            });
            return;
        }
    });
}
