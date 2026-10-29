#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <utility>

class QTcpServer;
class QTcpSocket;
class QTimer;
class QJsonObject;

namespace orbit::core {

class DownloadManager;
struct ParsedHttpRequest;

class RemoteServer final : public QObject {
    Q_OBJECT

public:
    explicit RemoteServer(DownloadManager& manager, QObject* parent = nullptr);
    ~RemoteServer() override;

    RemoteServer(const RemoteServer&) = delete;
    RemoteServer& operator=(const RemoteServer&) = delete;

    bool start(quint16 port);
    void stop();
    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] quint16 port() const noexcept { return port_; }
    [[nodiscard]] QString lastError() const { return lastError_; }

    void setCredentials(const QString& username, const QString& plainPassword);

    void loadStoredCredentials(const QString& username, const QString& passwordHashBase64,
                                const QString& passwordSaltBase64, int iterations);

    [[nodiscard]] bool hasCredentials() const;
    [[nodiscard]] QString username() const { return username_; }
    [[nodiscard]] QString passwordHashBase64() const;
    [[nodiscard]] QString passwordSaltBase64() const;
    [[nodiscard]] int passwordIterations() const { return iterations_; }

signals:
    void errorOccurred(const QString& message);

private:
    struct ConnectionState {
        QByteArray buffer;
        qint64 openedAtMs{0};
    };
    struct LoginAttempts {
        int failures{0};
        QDateTime lockedUntil;
    };

    void onNewConnection();
    void onSocketReadyRead(QTcpSocket* socket);
    void cleanupConnection(QTcpSocket* socket);
    void sweepStaleConnections();

    void handleRequest(QTcpSocket* socket, const ParsedHttpRequest& req);
    void handleSession(QTcpSocket* socket, const ParsedHttpRequest& req);
    void handleLogin(QTcpSocket* socket, const ParsedHttpRequest& req);
    void handleLogout(QTcpSocket* socket, const ParsedHttpRequest& req);
    void handleDownloadsList(QTcpSocket* socket);
    void handleDownloadAction(QTcpSocket* socket, quint64 id, const QString& action);
    void handleAddDownload(QTcpSocket* socket, const ParsedHttpRequest& req);
    void handleTorrentParse(QTcpSocket* socket, const ParsedHttpRequest& req);
    void handleAddTorrentFromUpload(QTcpSocket* socket, const ParsedHttpRequest& req);
    void serveAsset(QTcpSocket* socket, const QString& resourcePath, const QByteArray& contentType);

    [[nodiscard]] QString uploadsDirPath() const;
    void pruneTorrentUploads();
    void purgeStaleUploadFiles() const;

    [[nodiscard]] bool isAuthenticated(const ParsedHttpRequest& req);
    [[nodiscard]] bool verifyCredentials(const QString& user, const QString& password) const;
    QString createSession();
    void pruneSessions();
    [[nodiscard]] bool checkRateLimit(const QString& clientKey) const;
    void recordFailedLogin(const QString& clientKey);
    void recordSuccessfulLogin(const QString& clientKey);

    [[nodiscard]] QByteArray snapshotsPayload() const;

    void writeResponse(QTcpSocket* socket, int status, const QByteArray& contentType, const QByteArray& body,
                        const QList<std::pair<QByteArray, QByteArray>>& extraHeaders = {});
    void writeJson(QTcpSocket* socket, int status, const QJsonObject& obj,
                    const QList<std::pair<QByteArray, QByteArray>>& extraHeaders = {});
    void writeJsonError(QTcpSocket* socket, int status, const QString& message);

    DownloadManager& manager_;
    QTcpServer* server_{nullptr};
    quint16 port_{8765};
    QString lastError_;

    QString username_;
    QByteArray passwordHash_;
    QByteArray passwordSalt_;
    int iterations_{200000};

    QHash<QString, QDateTime> sessions_;
    QHash<QString, LoginAttempts> loginAttempts_;

    struct PendingTorrentUpload {
        QString filePath;
        QString name;
        QDateTime expiresAt;
    };
    QHash<QString, PendingTorrentUpload> pendingTorrentUploads_;

    QHash<QTcpSocket*, ConnectionState> connections_;

    QTimer* sweepTimer_{nullptr};
    QTimer* uploadPurgeTimer_{nullptr};
};

}
