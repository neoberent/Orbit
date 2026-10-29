#include "LocalApiServer.hpp"

#include "utils/LocalSocket.hpp"
#include "utils/Logger.hpp"

#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QTimer>

namespace {
constexpr qint64 kMaxRequestBytes = 4 * 1024 * 1024;
}

LocalApiServer::LocalApiServer(Handler handler, QObject* parent) : QObject(parent), handler_(std::move(handler)) {}

LocalApiServer::~LocalApiServer() {
    if (server_) server_->close();
}

bool LocalApiServer::listen() {
    if (server_ && server_->isListening()) return true;
    const QString name = orbit::utils::localServerName(QString::fromLatin1(kPipeName));
    QLocalServer::removeServer(name);
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    connect(server_, &QLocalServer::newConnection, this, &LocalApiServer::onNewConnection);
    if (!server_->listen(name)) {
        ORBIT_LOG_ERROR("api", "Couldn't open the local API pipe: " + server_->errorString().toStdString());
        return false;
    }
    ORBIT_LOG_INFO("api", "Local API listening");
    return true;
}

bool LocalApiServer::isListening() const { return server_ && server_->isListening(); }

void LocalApiServer::onNewConnection() {
    while (QLocalSocket* socket = server_->nextPendingConnection()) {
        buffers_.insert(socket, QByteArray());
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            buffers_.remove(socket);
            socket->deleteLater();
        });
        QPointer<QLocalSocket> guard(socket);
        QTimer::singleShot(30000, this, [guard] {
            if (guard) guard->disconnectFromServer();
        });
    }
}

void LocalApiServer::onReadyRead(QLocalSocket* socket) {
    auto it = buffers_.find(socket);
    if (it == buffers_.end()) return;
    QByteArray& buffer = it.value();
    buffer += socket->readAll();
    if (buffer.size() > kMaxRequestBytes) {
        socket->abort();
        return;
    }

    qsizetype newline;
    while ((newline = buffer.indexOf('\n')) >= 0) {
        const QByteArray line = buffer.left(newline).trimmed();
        buffer.remove(0, newline + 1);
        if (line.isEmpty()) continue;

        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
        QJsonObject response;
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            response = QJsonObject{{"ok", false}, {"error", "Malformed request"}};
        } else {
            response = handler_(doc.object());
        }
        socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
        socket->flush();
    }
}
