#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QVariantMap>

#include <atomic>
#include <functional>
#include <memory>

namespace orbit::core {
class DownloadManager;
}

// What Orbit checks on a finished download before the after-download actions run:
//  - a virus scan with ClamAV (clamdscan, or clamscan when the daemon isn't running);
//  - the publisher's signature beside a release file (<link>.asc / .sig with GnuPG,
//    <link>.minisig against the user's minisign keys);
//  - whether the same file is already there (another download or a file in the folder).
// A threat or a bad signature stops the after-download actions; everything else only
// informs. Results live for the session and show in the details pane and `orbit list`.
class FileChecks final : public QObject {
    Q_OBJECT

public:
    struct Settings {
        bool scan{false};
        bool signatures{true};
        bool duplicates{true};
        QStringList minisignKeys;
    };

    explicit FileChecks(orbit::core::DownloadManager& manager, QObject* parent = nullptr);
    ~FileChecks() override;

    void setSettings(const Settings& settings) { settings_ = settings; }

    // Runs the checks for a download that just finished. `done(proceed)` runs once the
    // scan and the signature check are over; proceed is false when either found a problem.
    // The duplicate check carries on after that.
    void check(qlonglong id, std::function<void(bool proceed)> done);

    // "scan": clean|infected|failed|unavailable, "threat"; "signature": see
    // signature_check::toString, "signer", "keyId", "signatureKind" (openpgp|minisign);
    // "duplicateOf": the path of the same file. Empty when nothing was checked.
    [[nodiscard]] QVariantMap resultFor(qlonglong id) const { return results_.value(id); }
    [[nodiscard]] int revision() const noexcept { return revision_; }

    [[nodiscard]] static bool clamavAvailable();

    // Reads clamscan / clamdscan output ("<path>: <name> FOUND") for the threat names.
    [[nodiscard]] static QStringList threatsIn(const QByteArray& output);

signals:
    void changed();
    void notice(const QString& title, const QString& message, bool ok);
    void duplicateFound(qlonglong id, const QString& name, const QString& originalPath);

private:
    struct Job;

    void scan(const std::shared_ptr<Job>& job);
    void signature(const std::shared_ptr<Job>& job, int sidecar);
    void verifyOpenPgp(const std::shared_ptr<Job>& job, const QByteArray& signature, int sidecar);
    void verifyMinisign(const std::shared_ptr<Job>& job, const QByteArray& signature, int sidecar);
    void finishSignature(const std::shared_ptr<Job>& job, const QVariantMap& result);
    void proceed(const std::shared_ptr<Job>& job);
    void duplicates(const std::shared_ptr<Job>& job);
    void setResult(qlonglong id, const QVariantMap& values);

    orbit::core::DownloadManager& manager_;
    Settings settings_;
    QHash<qlonglong, QVariantMap> results_;
    int revision_{0};
    QThreadPool pool_;
    std::shared_ptr<std::atomic<bool>> stopping_{std::make_shared<std::atomic<bool>>(false)};
    bool warnedNoClamav_{false};
};
