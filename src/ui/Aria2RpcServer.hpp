#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <functional>

class QTcpServer;
class QTcpSocket;

namespace orbit::core {
class DownloadManager;
}

// aria2's JSON-RPC interface (POST /jsonrpc), so browser add-ons, scripts and phone apps
// written for aria2 can hand downloads to Orbit and see their progress. It listens on
// 127.0.0.1 only, requires the secret token every call ("token:<secret>" as the first
// parameter, like aria2's --rpc-secret), refuses a Host header that isn't the loopback
// address (so a web page can't reach it through DNS rebinding), and locks out a client
// that keeps sending wrong tokens.
class Aria2RpcServer final : public QObject {
    Q_OBJECT

public:
    // Adds links (http(s), magnet or a local .torrent path) to a folder ("" = default),
    // returning the new download's id, or 0 with an error. The first link is the download;
    // any others are mirrors of the same file (aria2 clients send them together).
    using AddLink = std::function<qulonglong(const QStringList& links, const QString& directory, QString* error)>;

    Aria2RpcServer(orbit::core::DownloadManager& manager, AddLink addLink, QObject* parent = nullptr);
    ~Aria2RpcServer() override;

    // Returns an error message, or an empty string once it listens.
    QString start(quint16 port, const QString& secret);
    void stop();
    [[nodiscard]] bool isRunning() const;

    // One JSON-RPC call (exposed for the tests): {"jsonrpc","id","method","params"}.
    QJsonObject handleCall(const QJsonObject& call);

private:
    void onNewConnection();
    void onReadyRead(QTcpSocket* socket);
    void reply(QTcpSocket* socket, int status, const QByteArray& body, const QByteArray& contentType, bool allowCors);
    QJsonValue dispatch(const QString& method, QJsonArray params, QString* error, int* code);
    QJsonObject statusOf(qulonglong id, const QJsonArray& keys) const;
    QJsonArray statusList(const QString& which, int offset, int num, const QJsonArray& keys) const;

    orbit::core::DownloadManager& manager_;
    AddLink addLink_;
    QTcpServer* server_{nullptr};
    quint16 port_{0};
    QString secret_;
    QHash<QTcpSocket*, QByteArray> buffers_;
    int failedTokens_{0};
    qint64 lockedUntil_{0};
};
