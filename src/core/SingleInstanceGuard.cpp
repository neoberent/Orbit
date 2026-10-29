#include "core/SingleInstanceGuard.hpp"

#include "utils/LocalSocket.hpp"
#include "utils/Logger.hpp"

#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>

#include <memory>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace orbit::core {

SingleInstanceGuard::SingleInstanceGuard(QString key, QObject* parent)
    : QObject(parent), key_(std::move(key)), serverName_(orbit::utils::localServerName(key_)) {}

SingleInstanceGuard::~SingleInstanceGuard() {
    if (server_) server_->close();
#if defined(_WIN32)
    if (primaryMutex_) {
        ReleaseMutex(static_cast<HANDLE>(primaryMutex_));
        CloseHandle(static_cast<HANDLE>(primaryMutex_));
    }
#endif
}

bool SingleInstanceGuard::sendPayloadToPrimary(const QString& payload) const {
    QByteArray data = payload.toUtf8();
    data.append('\n');

    // The primary's QLocalServer may not have started listening yet (there's a brief
    // window between it winning the primary election and calling listen()), so retry
    // briefly instead of giving up on the first failed connect.
    for (int attempt = 0; attempt < 10; ++attempt) {
        QLocalSocket socket;
        socket.connectToServer(serverName_);
        if (socket.waitForConnected(200)) {
            socket.write(data);
            socket.flush();
            socket.waitForBytesWritten(300);
            socket.disconnectFromServer();
            return true;
        }
    }
    return false;
}

bool SingleInstanceGuard::start(const QString& payloadIfSecondary) {
#if defined(_WIN32)
    // A named kernel mutex gives an atomic, race-free answer to "am I the first
    // instance?". The previous approach (probe the QLocalServer with a timed connect)
    // could falsely conclude a slow-to-start primary didn't exist, so a second launch
    // would also become primary. CreateMutexW's ERROR_ALREADY_EXISTS is atomic across
    // processes, unlike a connect-with-timeout race.
    const QString mutexName = QStringLiteral("Local\\") + key_ + QStringLiteral("_singleinstance");
    HANDLE mutex = CreateMutexW(nullptr, TRUE, reinterpret_cast<const wchar_t*>(mutexName.utf16()));
    const bool isPrimary = mutex != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
    if (mutex) primaryMutex_ = mutex;

    if (!isPrimary) {
        sendPayloadToPrimary(payloadIfSecondary);
        return false;
    }
#else
    primaryLock_ = std::make_unique<QLockFile>(serverName_ + QStringLiteral(".lock"));
    // A lock left by a crashed primary is detected through its PID and taken over.
    primaryLock_->setStaleLockTime(0);
    if (!primaryLock_->tryLock(0)) {
        if (primaryLock_->error() == QLockFile::LockFailedError) {
            primaryLock_.reset();
            if (!sendPayloadToPrimary(payloadIfSecondary)) {
                ORBIT_LOG_WARN("single-instance", "Another Orbit is running but didn't accept the hand-off");
            }
            return false;
        }
        // The runtime directory isn't usable; carry on as the only instance.
        ORBIT_LOG_WARN("single-instance", "Couldn't create the instance lock; continuing without it");
        primaryLock_.reset();
    }
#endif

    QLocalServer::removeServer(serverName_);
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    connect(server_, &QLocalServer::newConnection, this, &SingleInstanceGuard::onNewConnection);

    if (!server_->listen(serverName_)) {
        ORBIT_LOG_ERROR("single-instance",
                         "Failed to start instance server: " + server_->errorString().toStdString());
    }
    return true;
}

void SingleInstanceGuard::onNewConnection() {
    while (QLocalSocket* socket = server_->nextPendingConnection()) {
        auto buffer = std::make_shared<QByteArray>();

        connect(socket, &QLocalSocket::readyRead, this, [socket, buffer] {
            buffer->append(socket->readAll());
        });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket, buffer] {
            emit payloadReceived(QString::fromUtf8(*buffer).trimmed());
            socket->deleteLater();
        });
    }
}

}
