#pragma once

#include <QObject>
#include <QString>

#include <memory>

class QLocalServer;
class QLockFile;

namespace orbit::core {

// Cross-process single-instance coordination using a local named-pipe/socket server.
// The first process to call start() becomes the primary instance and listens for
// payloads (e.g. orbit:// URLs) forwarded by later launches; later launches get
// start() == false, having already sent payloadIfSecondary to the primary over the very
// same connection used to detect it (so each launch produces exactly one payloadReceived
// on the primary — no separate probe-then-send round trip that could double-fire it).
class SingleInstanceGuard final : public QObject {
    Q_OBJECT

public:
    explicit SingleInstanceGuard(QString key, QObject* parent = nullptr);
    ~SingleInstanceGuard() override;

    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    [[nodiscard]] bool start(const QString& payloadIfSecondary = {});

signals:
    void payloadReceived(const QString& payload);

private:
    void onNewConnection();
    bool sendPayloadToPrimary(const QString& payload) const;

    QString key_;
    QString serverName_;
    QLocalServer* server_{nullptr};
#if defined(_WIN32)
    // Opaque HANDLE (avoids pulling <Windows.h> into this header) for the named mutex
    // used to atomically decide which launch becomes the primary instance.
    void* primaryMutex_{nullptr};
#else
    // The same atomic election elsewhere: a lock file in the user's runtime directory,
    // held for the primary's lifetime and reclaimed automatically if it crashed.
    std::unique_ptr<QLockFile> primaryLock_;
#endif
};

}
