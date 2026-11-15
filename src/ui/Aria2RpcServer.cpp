#include "Aria2RpcServer.hpp"

#include "core/DownloadManager.hpp"
#include "utils/FileUtils.hpp"
#include "utils/Logger.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

using orbit::core::DownloadState;
using orbit::core::DownloadType;

namespace {

constexpr qsizetype kMaxRequestBytes = 8 * 1024 * 1024;  // a base64 .torrent can be large
constexpr int kMaxFailedTokens = 10;
constexpr int kLockoutSeconds = 60;

// aria2 identifies downloads by a 16-digit hex "GID".
QString gidOf(qulonglong id) { return QStringLiteral("%1").arg(id, 16, 16, QLatin1Char('0')); }
qulonglong idOfGid(const QString& gid) {
    bool ok = false;
    const qulonglong id = gid.toULongLong(&ok, 16);
    return ok ? id : 0;
}

QString aria2Status(DownloadState state) {
    switch (state) {
        case DownloadState::Downloading:
        case DownloadState::Connecting:
        case DownloadState::Seeding: return QStringLiteral("active");
        case DownloadState::Queued: return QStringLiteral("waiting");
        case DownloadState::Paused: return QStringLiteral("paused");
        case DownloadState::Completed: return QStringLiteral("complete");
        case DownloadState::Error: return QStringLiteral("error");
        case DownloadState::Cancelled: return QStringLiteral("removed");
    }
    return QStringLiteral("waiting");
}

// aria2 sends every number as a string.
QString num(double value) { return QString::number(static_cast<qulonglong>(value > 0 ? value : 0)); }

QJsonObject rpcError(const QJsonValue& id, int code, const QString& message) {
    return QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

bool isLoopbackHost(const QByteArray& host, quint16 port) {
    // "127.0.0.1:6800", "localhost:6800", "[::1]:6800" (or without the port).
    QByteArray h = host.trimmed().toLower();
    const QByteArray suffix = ":" + QByteArray::number(port);
    if (h.endsWith(suffix)) h.chop(suffix.size());
    return h == "127.0.0.1" || h == "localhost" || h == "[::1]";
}

}

Aria2RpcServer::Aria2RpcServer(orbit::core::DownloadManager& manager, AddLink addLink, QObject* parent)
    : QObject(parent), manager_(manager), addLink_(std::move(addLink)) {}

Aria2RpcServer::~Aria2RpcServer() { stop(); }

QString Aria2RpcServer::start(quint16 port, const QString& secret) {
    stop();
    if (secret.size() < 16) return QStringLiteral("The secret token is too short");
    secret_ = secret;
    port_ = port;
    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection, this, &Aria2RpcServer::onNewConnection);
    if (!server_->listen(QHostAddress::LocalHost, port)) {
        const QString error = server_->errorString();
        delete server_;
        server_ = nullptr;
        return error;
    }
    ORBIT_LOG_INFO("aria2", "aria2-compatible interface on 127.0.0.1:" + std::to_string(port));
    return {};
}

void Aria2RpcServer::stop() {
    if (!server_) return;
    server_->close();
    server_->deleteLater();
    server_ = nullptr;
}

bool Aria2RpcServer::isRunning() const { return server_ && server_->isListening(); }

void Aria2RpcServer::onNewConnection() {
    while (QTcpSocket* socket = server_->nextPendingConnection()) {
        buffers_.insert(socket, {});
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            buffers_.remove(socket);
            socket->deleteLater();
        });
        QPointer<QTcpSocket> guard(socket);
        QTimer::singleShot(30000, this, [guard] {
            if (guard) guard->abort();
        });
    }
}

void Aria2RpcServer::reply(QTcpSocket* socket, int status, const QByteArray& body, const QByteArray& contentType, bool allowCors) {
    QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + (status == 200 ? " OK" : status == 204 ? " No Content" : " Error") + "\r\n";
    head += "Content-Type: " + contentType + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
    // Browser add-ons call from their own origin; the token is what protects the calls.
    if (allowCors) {
        head += "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: POST, OPTIONS\r\n"
                "Access-Control-Allow-Headers: Content-Type\r\nAccess-Control-Max-Age: 600\r\n";
    }
    head += "Connection: close\r\n\r\n";
    socket->write(head + body);
    socket->disconnectFromHost();
}

void Aria2RpcServer::onReadyRead(QTcpSocket* socket) {
    QByteArray& buffer = buffers_[socket];
    buffer += socket->readAll();
    if (buffer.size() > kMaxRequestBytes) {
        reply(socket, 413, {}, "text/plain", false);
        return;
    }
    const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) return;
    const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
    const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
    const QByteArray method = requestLine.value(0);
    QByteArray path = requestLine.value(1);
    if (const qsizetype q = path.indexOf('?'); q >= 0) path.truncate(q);
    qsizetype contentLength = 0;
    QByteArray host;
    for (const QByteArray& raw : lines.mid(1)) {
        const QByteArray line = raw.trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon < 0) continue;
        const QByteArray name = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (name == "content-length") contentLength = value.toLongLong();
        else if (name == "host") host = value;
    }
    if (contentLength < 0 || contentLength > kMaxRequestBytes) {
        reply(socket, 413, {}, "text/plain", false);
        return;
    }
    if (buffer.size() < headerEnd + 4 + contentLength) return;  // wait for the body
    const QByteArray body = buffer.mid(headerEnd + 4, contentLength);

    if (!isLoopbackHost(host, port_)) {
        reply(socket, 403, "Forbidden", "text/plain", false);
        return;
    }
    if (path != "/jsonrpc") {
        reply(socket, 404, "Not found", "text/plain", false);
        return;
    }
    if (method == "OPTIONS") {
        reply(socket, 204, {}, "text/plain", true);
        return;
    }
    if (method != "POST") {
        reply(socket, 405, "Use POST", "text/plain", true);
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(body);
    QByteArray out;
    if (doc.isArray()) {  // a batch
        QJsonArray results;
        for (const QJsonValue& call : doc.array()) results.append(handleCall(call.toObject()));
        out = QJsonDocument(results).toJson(QJsonDocument::Compact);
    } else if (doc.isObject()) {
        out = QJsonDocument(handleCall(doc.object())).toJson(QJsonDocument::Compact);
    } else {
        out = QJsonDocument(rpcError(QJsonValue::Null, -32700, QStringLiteral("Parse error"))).toJson(QJsonDocument::Compact);
    }
    reply(socket, 200, out, "application/json", true);
}

QJsonObject Aria2RpcServer::handleCall(const QJsonObject& call) {
    const QJsonValue id = call.value("id");
    const QString method = call.value("method").toString();
    QJsonArray params = call.value("params").toArray();

    // system.listMethods and system.listNotifications need no token in aria2 either.
    if (method != QLatin1String("system.listMethods") && method != QLatin1String("system.listNotifications")) {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (now < lockedUntil_) return rpcError(id, 1, QStringLiteral("Too many wrong tokens; try again in a minute"));
        if (method == QLatin1String("system.multicall")) {
            // Each inner call carries its own token.
        } else if (params.isEmpty() || params.first().toString() != QStringLiteral("token:") + secret_) {
            if (++failedTokens_ >= kMaxFailedTokens) {
                lockedUntil_ = now + kLockoutSeconds;
                failedTokens_ = 0;
                ORBIT_LOG_WARN("aria2", "Too many calls with a wrong token; refusing calls for a minute");
            }
            return rpcError(id, 1, QStringLiteral("Unauthorized"));
        } else {
            failedTokens_ = 0;
            params.removeFirst();
        }
    }

    QString error;
    int code = 1;
    const QJsonValue result = dispatch(method, params, &error, &code);
    if (!error.isEmpty()) return rpcError(id, code, error);
    return QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

QJsonValue Aria2RpcServer::dispatch(const QString& method, QJsonArray params, QString* error, int* code) {
    auto param = [&](int index) { return index < params.size() ? params.at(index) : QJsonValue(); };
    auto option = [&](int index, const char* key) {
        return params.size() > index ? params.at(index).toObject().value(QLatin1String(key)).toString() : QString();
    };
    auto requireId = [&](qulonglong* out) {
        *out = idOfGid(param(0).toString());
        if (*out == 0 || !manager_.snapshot(static_cast<orbit::core::DownloadId>(*out))) {
            *error = QStringLiteral("GID %1 is not found").arg(param(0).toString());
            return false;
        }
        return true;
    };

    if (method == QLatin1String("aria2.addUri")) {
        const QJsonArray uris = param(0).toArray();
        if (uris.isEmpty()) { *error = QStringLiteral("No URI to download"); return {}; }
        // aria2 takes several URIs for one file: the first is the download, the rest mirrors.
        QStringList links;
        for (const QJsonValue& uri : uris) {
            const QString link = uri.toString().trimmed();
            if (!link.isEmpty() && links.size() < 9) links << link;
        }
        if (links.isEmpty()) { *error = QStringLiteral("No URI to download"); return {}; }
        QString addError;
        const qulonglong newId = addLink_(links, option(1, "dir"), &addError);
        if (newId == 0) { *error = addError.isEmpty() ? QStringLiteral("Couldn't add it") : addError; return {}; }
        return gidOf(newId);
    }
    if (method == QLatin1String("aria2.addTorrent")) {
        const QByteArray torrent = QByteArray::fromBase64(param(0).toString().toLatin1());
        if (torrent.isEmpty() || torrent.front() != 'd') { *error = QStringLiteral("Not a .torrent file"); return {}; }
        const QString dir = QString::fromStdWString((orbit::utils::appDataDir() / "aria2").wstring());
        QDir().mkpath(dir);
        const QString path = dir + QLatin1Char('/') + QString::fromLatin1(QCryptographicHash::hash(torrent, QCryptographicHash::Sha1).toHex()) +
                             QStringLiteral(".torrent");
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) { *error = QStringLiteral("Couldn't store the .torrent"); return {}; }
        file.write(torrent);
        file.close();
        QString addError;
        const qulonglong newId = addLink_({path}, option(2, "dir"), &addError);
        if (newId == 0) { *error = addError; return {}; }
        return gidOf(newId);
    }
    if (method == QLatin1String("aria2.tellStatus")) {
        qulonglong id = 0;
        if (!requireId(&id)) return {};
        return statusOf(id, param(1).toArray());
    }
    if (method == QLatin1String("aria2.tellActive")) return statusList(QStringLiteral("active"), 0, 1000, param(0).toArray());
    if (method == QLatin1String("aria2.tellWaiting")) {
        return statusList(QStringLiteral("waiting"), param(0).toInt(), param(1).toInt(1000), param(2).toArray());
    }
    if (method == QLatin1String("aria2.tellStopped")) {
        return statusList(QStringLiteral("stopped"), param(0).toInt(), param(1).toInt(1000), param(2).toArray());
    }
    if (method == QLatin1String("aria2.pause") || method == QLatin1String("aria2.forcePause")) {
        qulonglong id = 0;
        if (!requireId(&id)) return {};
        manager_.pause(static_cast<orbit::core::DownloadId>(id));
        return gidOf(id);
    }
    if (method == QLatin1String("aria2.unpause")) {
        qulonglong id = 0;
        if (!requireId(&id)) return {};
        manager_.resume(static_cast<orbit::core::DownloadId>(id));
        return gidOf(id);
    }
    if (method == QLatin1String("aria2.remove") || method == QLatin1String("aria2.forceRemove")) {
        qulonglong id = 0;
        if (!requireId(&id)) return {};
        manager_.cancel(static_cast<orbit::core::DownloadId>(id));
        return gidOf(id);
    }
    if (method == QLatin1String("aria2.removeDownloadResult")) {
        qulonglong id = 0;
        if (!requireId(&id)) return {};
        const auto snap = manager_.snapshot(static_cast<orbit::core::DownloadId>(id));
        if (snap && (snap->state == DownloadState::Completed || snap->state == DownloadState::Error || snap->state == DownloadState::Cancelled)) {
            manager_.remove(snap->id, false);  // files are never deleted from here
        }
        return QStringLiteral("OK");
    }
    if (method == QLatin1String("aria2.purgeDownloadResult")) {
        for (const auto& snap : manager_.snapshot()) {
            if (snap.state == DownloadState::Completed || snap.state == DownloadState::Error || snap.state == DownloadState::Cancelled) {
                manager_.remove(snap.id, false);
            }
        }
        return QStringLiteral("OK");
    }
    if (method == QLatin1String("aria2.pauseAll") || method == QLatin1String("aria2.forcePauseAll")) {
        for (const auto& snap : manager_.snapshot()) {
            if (snap.state == DownloadState::Downloading || snap.state == DownloadState::Connecting ||
                snap.state == DownloadState::Queued || snap.state == DownloadState::Seeding) manager_.pause(snap.id);
        }
        return QStringLiteral("OK");
    }
    if (method == QLatin1String("aria2.unpauseAll")) {
        for (const auto& snap : manager_.snapshot()) {
            if (snap.state == DownloadState::Paused) manager_.resume(snap.id);
        }
        return QStringLiteral("OK");
    }
    if (method == QLatin1String("aria2.getGlobalStat")) {
        double down = 0, up = 0;
        int active = 0, waiting = 0, stopped = 0;
        for (const auto& snap : manager_.snapshot()) {
            const QString s = aria2Status(snap.state);
            if (s == QLatin1String("active")) {
                ++active;
                down += snap.stats.downloadRateBytesPerSec;
                up += snap.stats.uploadRateBytesPerSec;
            } else if (s == QLatin1String("waiting") || s == QLatin1String("paused")) {
                ++waiting;
            } else {
                ++stopped;
            }
        }
        return QJsonObject{{"downloadSpeed", num(down)}, {"uploadSpeed", num(up)}, {"numActive", QString::number(active)},
                           {"numWaiting", QString::number(waiting)}, {"numStopped", QString::number(stopped)},
                           {"numStoppedTotal", QString::number(stopped)}};
    }
    if (method == QLatin1String("aria2.getVersion")) {
        return QJsonObject{{"version", QStringLiteral("1.37.0")}, {"enabledFeatures", QJsonArray{"BitTorrent", "Metalink", "HTTPS"}},
                           {"orbit", QStringLiteral(ORBIT_VERSION)}};
    }
    if (method == QLatin1String("aria2.getSessionInfo")) return QJsonObject{{"sessionId", QStringLiteral("orbit")}};
    if (method == QLatin1String("aria2.getGlobalOption") || method == QLatin1String("aria2.getOption")) {
        return QJsonObject{{"dir", QString::fromStdString(manager_.defaultDownloadDir().string())},
                           {"max-concurrent-downloads", QString::number(manager_.maxConcurrentDownloads())}};
    }
    if (method == QLatin1String("aria2.changeOption") || method == QLatin1String("aria2.changeGlobalOption") ||
        method == QLatin1String("aria2.saveSession")) {
        return QStringLiteral("OK");  // Orbit's own settings stay in charge.
    }
    if (method == QLatin1String("system.listMethods")) {
        return QJsonArray{"aria2.addUri", "aria2.addTorrent", "aria2.tellStatus", "aria2.tellActive", "aria2.tellWaiting",
                          "aria2.tellStopped", "aria2.pause", "aria2.forcePause", "aria2.unpause", "aria2.remove",
                          "aria2.forceRemove", "aria2.removeDownloadResult", "aria2.purgeDownloadResult", "aria2.pauseAll",
                          "aria2.forcePauseAll", "aria2.unpauseAll", "aria2.getGlobalStat", "aria2.getVersion",
                          "aria2.getSessionInfo", "aria2.getGlobalOption", "aria2.getOption", "aria2.changeOption",
                          "aria2.changeGlobalOption", "aria2.saveSession", "system.multicall", "system.listMethods",
                          "system.listNotifications"};
    }
    if (method == QLatin1String("system.listNotifications")) return QJsonArray{};
    if (method == QLatin1String("system.multicall")) {
        QJsonArray results;
        for (const QJsonValue& inner : param(0).toArray()) {
            const QJsonObject call{{"id", QJsonValue::Null}, {"method", inner.toObject().value("methodName")},
                                   {"params", inner.toObject().value("params")}};
            const QJsonObject r = handleCall(call);
            // aria2 wraps each successful result in a one-element array.
            results.append(r.contains("error") ? QJsonValue(r.value("error")) : QJsonValue(QJsonArray{r.value("result")}));
        }
        return results;
    }
    *code = -32601;
    *error = QStringLiteral("Method not found: ") + method;
    return {};
}

QJsonObject Aria2RpcServer::statusOf(qulonglong id, const QJsonArray& keys) const {
    const auto snap = manager_.snapshot(static_cast<orbit::core::DownloadId>(id));
    if (!snap) return {};
    const QString path = QString::fromStdString(snap->destinationPath);
    const QJsonObject file{{"index", "1"}, {"path", path}, {"length", num(snap->stats.totalBytes)},
                           {"completedLength", num(snap->stats.bytesDownloaded)}, {"selected", "true"},
                           {"uris", QJsonArray{QJsonObject{{"uri", QString::fromStdString(snap->source)}, {"status", "used"}}}}};
    QJsonObject status{{"gid", gidOf(id)},
                       {"status", aria2Status(snap->state)},
                       {"totalLength", num(snap->stats.totalBytes)},
                       {"completedLength", num(snap->stats.bytesDownloaded)},
                       {"uploadLength", num(snap->stats.bytesUploaded)},
                       {"downloadSpeed", num(snap->stats.downloadRateBytesPerSec)},
                       {"uploadSpeed", num(snap->stats.uploadRateBytesPerSec)},
                       {"connections", QString::number(snap->stats.activeConnections)},
                       {"dir", QFileInfo(path).absolutePath()},
                       {"files", QJsonArray{file}}};
    if (snap->type == DownloadType::Torrent) {
        status.insert("numSeeders", QString::number(snap->stats.numSeeds));
        status.insert("bittorrent", QJsonObject{{"info", QJsonObject{{"name", QString::fromStdString(snap->name)}}}});
    }
    if (snap->state == DownloadState::Error) {
        status.insert("errorCode", "1");
        status.insert("errorMessage", QString::fromStdString(snap->errorMessage));
    }
    if (keys.isEmpty()) return status;
    QJsonObject picked;
    for (const QJsonValue& key : keys) {
        if (status.contains(key.toString())) picked.insert(key.toString(), status.value(key.toString()));
    }
    return picked;
}

QJsonArray Aria2RpcServer::statusList(const QString& which, int offset, int num, const QJsonArray& keys) const {
    QJsonArray list;
    int index = 0;
    for (const auto& snap : manager_.snapshot()) {
        const QString s = aria2Status(snap.state);
        const bool wanted = which == QLatin1String("active") ? s == QLatin1String("active")
                            : which == QLatin1String("waiting") ? (s == QLatin1String("waiting") || s == QLatin1String("paused"))
                                                                : (s == QLatin1String("complete") || s == QLatin1String("error") ||
                                                                   s == QLatin1String("removed"));
        if (!wanted) continue;
        if (index++ < std::max(0, offset)) continue;
        if (list.size() >= std::clamp(num, 0, 1000)) break;
        list.append(statusOf(snap.id, keys));
    }
    return list;
}
