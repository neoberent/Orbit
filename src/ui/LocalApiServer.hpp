#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

class QLocalServer;
class QLocalSocket;

// A request/response channel for other programs of the same user, used by the browser
// extension's native messaging host. Each connection carries one JSON object per line
// and gets one JSON object per line back. The pipe only accepts the current user.
class LocalApiServer final : public QObject {
    Q_OBJECT

public:
    using Handler = std::function<QJsonObject(const QJsonObject& request)>;

    static constexpr const char* kPipeName = "OrbitDownloadManager.Api";

    explicit LocalApiServer(Handler handler, QObject* parent = nullptr);
    ~LocalApiServer() override;

    bool listen();
    [[nodiscard]] bool isListening() const;

private:
    void onNewConnection();
    void onReadyRead(QLocalSocket* socket);

    Handler handler_;
    QLocalServer* server_{nullptr};
    QHash<QLocalSocket*, QByteArray> buffers_;
};
