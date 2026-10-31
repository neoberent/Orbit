#include "core/RemoteServer.hpp"

#include "core/DownloadManager.hpp"
#include "core/TorrentEngine.hpp"
#include "core/Types.hpp"
#include "utils/Logger.hpp"
#include "utils/StringUtils.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaObject>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>

using orbit::core::DownloadId;
using orbit::core::DownloadState;
using orbit::core::DownloadType;
using orbit::core::TaskSnapshot;

namespace orbit::core {

namespace {

constexpr qint64 kMaxRequestBytes = 8 * 1024 * 1024;
constexpr int kSessionTtlSeconds = 24 * 60 * 60;
constexpr int kStaleConnectionMs = 15000;
constexpr int kMaxLoginFailures = 5;
constexpr int kLoginLockoutSeconds = 60;
constexpr int kTorrentUploadTtlSeconds = 30 * 60;
constexpr qint64 kUploadFileMaxAgeMs = qint64(24) * 60 * 60 * 1000;

QByteArray randomBytes(int n) {
    QByteArray b(n, 0);
    RAND_bytes(reinterpret_cast<unsigned char*>(b.data()), n);
    return b;
}

QByteArray pbkdf2(const QByteArray& password, const QByteArray& salt, int iterations, int keyLen = 32) {
    QByteArray out(keyLen, 0);
    PKCS5_PBKDF2_HMAC(password.constData(), password.size(),
                       reinterpret_cast<const unsigned char*>(salt.constData()), salt.size(), iterations,
                       EVP_sha256(), keyLen, reinterpret_cast<unsigned char*>(out.data()));
    return out;
}

bool constantTimeEqual(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size() || a.isEmpty()) return false;
    return CRYPTO_memcmp(a.constData(), b.constData(), static_cast<size_t>(a.size())) == 0;
}

QByteArray httpStatusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 410: return "Gone";
        case 413: return "Payload Too Large";
        case 421: return "Misdirected Request";
        case 429: return "Too Many Requests";
        case 503: return "Service Unavailable";
        default: return "Internal Server Error";
    }
}

QString extractCookieValue(const QString& cookieHeader, const QString& name) {
    const QStringList parts = cookieHeader.split(u';');
    for (const QString& part : parts) {
        const QString trimmed = part.trimmed();
        const int eq = trimmed.indexOf(u'=');
        if (eq < 0) continue;
        if (trimmed.left(eq) == name) return trimmed.mid(eq + 1);
    }
    return {};
}

QJsonValue snapshotToJson(const TaskSnapshot& snap) {
    QJsonObject o;
    o["id"] = QString::number(static_cast<qulonglong>(snap.id));
    o["name"] = QString::fromStdString(snap.name.empty() ? snap.source : snap.name);
    switch (snap.type) {
        case DownloadType::Torrent: o["type"] = "torrent"; break;
        case DownloadType::Media:   o["type"] = "media"; break;
        case DownloadType::Http:    o["type"] = "http"; break;
    }
    o["state"] = toString(snap.state);
    o["progress"] = snap.stats.progressRatio();
    o["bytesDownloaded"] = static_cast<double>(snap.stats.bytesDownloaded);
    o["totalBytes"] = static_cast<double>(snap.stats.totalBytes);
    o["downloadRate"] = snap.stats.downloadRateBytesPerSec;
    o["uploadRate"] = snap.stats.uploadRateBytesPerSec;
    o["eta"] = snap.stats.eta.has_value() ? QJsonValue(static_cast<double>(snap.stats.eta->count())) : QJsonValue();
    o["peers"] = snap.stats.numPeers;
    o["seeds"] = snap.stats.numSeeds;
    o["error"] = QString::fromStdString(snap.errorMessage);
    o["isActive"] = snap.state == DownloadState::Downloading || snap.state == DownloadState::Connecting ||
                     snap.state == DownloadState::Seeding;
    o["isPaused"] = snap.state == DownloadState::Paused;
    return o;
}

}

enum class ParseStatus { Incomplete, Ok, Malformed, TooLarge };

struct ParsedHttpRequest {
    QString method;
    QString path;
    QHash<QString, QString> headers;
    QByteArray body;
};

namespace {

ParseStatus tryParseRequest(const QByteArray& buf, ParsedHttpRequest& out) {
    if (buf.size() > kMaxRequestBytes) return ParseStatus::TooLarge;

    const int headerEnd = buf.indexOf("\r\n\r\n");
    if (headerEnd < 0) return ParseStatus::Incomplete;

    const QByteArray headerBlock = buf.left(headerEnd);
    const QList<QByteArray> lines = headerBlock.split('\n');
    if (lines.isEmpty()) return ParseStatus::Malformed;

    const QByteArray requestLine = lines[0].trimmed();
    const QList<QByteArray> parts = requestLine.split(' ');
    if (parts.size() < 2) return ParseStatus::Malformed;

    out.method = QString::fromLatin1(parts[0]).toUpper();
    const QByteArray rawTarget = parts[1];
    const int qpos = rawTarget.indexOf('?');
    const QByteArray rawPath = qpos >= 0 ? rawTarget.left(qpos) : rawTarget;
    out.path = QUrl::fromPercentEncoding(rawPath);

    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        if (line.isEmpty()) continue;
        const int colon = line.indexOf(':');
        if (colon < 0) continue;
        const QString key = QString::fromLatin1(line.left(colon)).trimmed().toLower();
        const QString value = QString::fromLatin1(line.mid(colon + 1)).trimmed();
        out.headers.insert(key, value);
    }

    qint64 contentLength = 0;
    if (out.headers.contains(QStringLiteral("content-length"))) {
        bool ok = false;
        contentLength = out.headers.value(QStringLiteral("content-length")).toLongLong(&ok);
        if (!ok || contentLength < 0) return ParseStatus::Malformed;
    }
    if (contentLength > kMaxRequestBytes) return ParseStatus::TooLarge;

    const qint64 bodyStart = headerEnd + 4;
    if (buf.size() - bodyStart < contentLength) return ParseStatus::Incomplete;

    out.body = buf.mid(static_cast<int>(bodyStart), static_cast<int>(contentLength));
    return ParseStatus::Ok;
}

}

RemoteServer::RemoteServer(DownloadManager& manager, QObject* parent) : QObject(parent), manager_(manager) {
    sweepTimer_ = new QTimer(this);
    sweepTimer_->setInterval(5000);
    connect(sweepTimer_, &QTimer::timeout, this, &RemoteServer::sweepStaleConnections);

    uploadPurgeTimer_ = new QTimer(this);
    uploadPurgeTimer_->setInterval(60 * 60 * 1000);
    connect(uploadPurgeTimer_, &QTimer::timeout, this, &RemoteServer::purgeStaleUploadFiles);
}

RemoteServer::~RemoteServer() { stop(); }

bool RemoteServer::start(quint16 port) {
    stop();

    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection, this, &RemoteServer::onNewConnection);

    if (!server_->listen(QHostAddress::LocalHost, port)) {
        lastError_ = server_->errorString();
        server_->deleteLater();
        server_ = nullptr;
        ORBIT_LOG_ERROR("RemoteServer", "Failed to listen on 127.0.0.1:" + std::to_string(port) + ": " +
                                             lastError_.toStdString());
        emit errorOccurred(lastError_);
        return false;
    }

    port_ = port;
    lastError_.clear();
    sweepTimer_->start();
    uploadPurgeTimer_->start();
    purgeStaleUploadFiles();
    ORBIT_LOG_INFO("RemoteServer", "Listening on 127.0.0.1:" + std::to_string(port));
    return true;
}

void RemoteServer::stop() {
    sweepTimer_->stop();
    uploadPurgeTimer_->stop();

    const auto sockets = connections_.keys();
    for (QTcpSocket* socket : sockets) {
        socket->disconnect(this);
        socket->close();
        socket->deleteLater();
    }
    connections_.clear();

    if (server_) {
        server_->close();
        server_->deleteLater();
        server_ = nullptr;
        ORBIT_LOG_INFO("RemoteServer", "Stopped");
    }
}

bool RemoteServer::isRunning() const noexcept { return server_ && server_->isListening(); }

void RemoteServer::setCredentials(const QString& username, const QString& plainPassword) {
    username_ = username;
    iterations_ = 200000;
    passwordSalt_ = randomBytes(16);
    passwordHash_ = pbkdf2(plainPassword.toUtf8(), passwordSalt_, iterations_);
    sessions_.clear();
}

void RemoteServer::loadStoredCredentials(const QString& username, const QString& passwordHashBase64,
                                          const QString& passwordSaltBase64, int iterations) {
    username_ = username;
    passwordHash_ = QByteArray::fromBase64(passwordHashBase64.toUtf8());
    passwordSalt_ = QByteArray::fromBase64(passwordSaltBase64.toUtf8());
    iterations_ = iterations > 0 ? iterations : 200000;
}

bool RemoteServer::hasCredentials() const {
    return !username_.isEmpty() && !passwordHash_.isEmpty() && !passwordSalt_.isEmpty();
}

QString RemoteServer::passwordHashBase64() const { return QString::fromLatin1(passwordHash_.toBase64()); }
QString RemoteServer::passwordSaltBase64() const { return QString::fromLatin1(passwordSalt_.toBase64()); }

bool RemoteServer::verifyCredentials(const QString& user, const QString& password) const {
    if (!hasCredentials()) return false;
    // Always run the PBKDF2 hash + comparison, even for a wrong username, so that
    // response latency doesn't reveal whether the username was correct.
    const QByteArray candidate = pbkdf2(password.toUtf8(), passwordSalt_, iterations_, passwordHash_.size());
    const bool passwordMatches = constantTimeEqual(candidate, passwordHash_);
    const bool usernameMatches = constantTimeEqual(user.toUtf8(), username_.toUtf8());
    return usernameMatches && passwordMatches;
}

void RemoteServer::onNewConnection() {
    while (server_ && server_->hasPendingConnections()) {
        QTcpSocket* socket = server_->nextPendingConnection();
        ConnectionState state;
        state.openedAtMs = QDateTime::currentMSecsSinceEpoch();
        connections_.insert(socket, state);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onSocketReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] { cleanupConnection(socket); });
    }
}

void RemoteServer::onSocketReadyRead(QTcpSocket* socket) {
    auto it = connections_.find(socket);
    if (it == connections_.end()) return;

    it->buffer += socket->readAll();

    ParsedHttpRequest req;
    const ParseStatus status = tryParseRequest(it->buffer, req);
    switch (status) {
        case ParseStatus::Incomplete: return;
        case ParseStatus::TooLarge: writeJsonError(socket, 413, "request too large"); return;
        case ParseStatus::Malformed: writeJsonError(socket, 400, "malformed request"); return;
        case ParseStatus::Ok: break;
    }

    handleRequest(socket, req);
}

void RemoteServer::cleanupConnection(QTcpSocket* socket) {
    connections_.remove(socket);
    socket->deleteLater();
}

void RemoteServer::sweepStaleConnections() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = connections_.begin(); it != connections_.end(); ++it) {
        if (now - it->openedAtMs > kStaleConnectionMs) {
            it.key()->disconnectFromHost();
        }
    }
    pruneSessions();
    pruneTorrentUploads();
}

QByteArray RemoteServer::snapshotsPayload() const {
    QJsonArray arr;
    for (const TaskSnapshot& snap : manager_.snapshot()) arr.append(snapshotToJson(snap));
    QJsonObject root;
    root["downloads"] = arr;
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QString RemoteServer::normalizeHost(const QString& hostOrUrl) {
    QString h = hostOrUrl.trimmed().toLower();
    const qsizetype scheme = h.indexOf(QStringLiteral("://"));
    if (scheme >= 0) h = h.mid(scheme + 3);
    const qsizetype slash = h.indexOf(u'/');
    if (slash >= 0) h = h.left(slash);
    const qsizetype at = h.lastIndexOf(u'@');
    if (at >= 0) h = h.mid(at + 1);
    if (h.startsWith(u'[')) {
        const qsizetype close = h.indexOf(u']');
        return close > 0 ? h.mid(1, close - 1) : QString();
    }
    const qsizetype colon = h.indexOf(u':');
    if (colon >= 0) h = h.left(colon);
    while (h.endsWith(u'.')) h.chop(1);
    return h;
}

void RemoteServer::setAllowedHosts(const QStringList& hosts) {
    allowedHosts_.clear();
    for (const QString& host : hosts) {
        const QString h = normalizeHost(host);
        if (!h.isEmpty() && !allowedHosts_.contains(h)) allowedHosts_.append(h);
    }
}

bool RemoteServer::isAllowedHost(const QString& hostHeader) const {
    // HTTP/1.1 clients always send Host and browsers can't leave it out, so a request
    // without one can't be a rebinding attack (e.g. an HTTP/1.0 script).
    if (hostHeader.trimmed().isEmpty()) return true;
    const QString h = normalizeHost(hostHeader);
    if (h == QLatin1String("localhost") || h == QLatin1String("127.0.0.1") || h == QLatin1String("::1")) return true;
    // Quick tunnel names are under Cloudflare's own zone, so an attacker can't point one
    // at 127.0.0.1.
    if (h.endsWith(QLatin1String(".trycloudflare.com"))) return true;
    return allowedHosts_.contains(h);
}

void RemoteServer::handleRequest(QTcpSocket* socket, const ParsedHttpRequest& req) {
    const QString host = req.headers.value(QStringLiteral("host"));
    if (!isAllowedHost(host)) {
        ORBIT_LOG_WARN("RemoteServer", "Refused a request for unknown host \"" + host.toStdString() +
                                           "\" (add it under Settings > Remote > Tunnel address if it's yours)");
        writeResponse(socket, 421, "text/plain; charset=utf-8",
                      "Orbit doesn't recognise the address \"" + normalizeHost(host).toUtf8() +
                          "\".\n\nIf this is your own tunnel, add it in Orbit under Settings > Remote > "
                          "Tunnel address.\n");
        return;
    }
    if (req.method == QLatin1String("GET") && (req.path == QLatin1String("/") || req.path == QLatin1String("/index.html"))) {
        return serveAsset(socket, ":/web/index.html", "text/html; charset=utf-8");
    }
    if (req.method == QLatin1String("GET") && req.path == QLatin1String("/app.js")) {
        return serveAsset(socket, ":/web/app.js", "application/javascript; charset=utf-8");
    }
    if (req.method == QLatin1String("GET") && req.path == QLatin1String("/style.css")) {
        return serveAsset(socket, ":/web/style.css", "text/css; charset=utf-8");
    }
    if (req.method == QLatin1String("GET") &&
        (req.path == QLatin1String("/favicon.ico") || req.path == QLatin1String("/favicon.png"))) {
        QFile file(":/images/favicon.png");
        if (file.open(QIODevice::ReadOnly)) {
            writeResponse(socket, 200, "image/png", file.readAll(), {{"Cache-Control", "public, max-age=86400"}});
        } else {
            writeJsonError(socket, 404, "not found");
        }
        return;
    }

    if (req.method == QLatin1String("GET") && req.path == QLatin1String("/api/session")) return handleSession(socket, req);
    if (req.method == QLatin1String("POST") && req.path == QLatin1String("/api/login")) return handleLogin(socket, req);
    if (req.method == QLatin1String("POST") && req.path == QLatin1String("/api/logout")) return handleLogout(socket, req);

    if (!isAuthenticated(req)) {
        writeJsonError(socket, 401, "unauthorized");
        return;
    }

    if (req.method == QLatin1String("GET") && req.path == QLatin1String("/api/downloads")) return handleDownloadsList(socket);
    if (req.method == QLatin1String("POST") && req.path == QLatin1String("/api/downloads")) return handleAddDownload(socket, req);
    if (req.method == QLatin1String("POST") && req.path == QLatin1String("/api/torrents/parse")) return handleTorrentParse(socket, req);
    if (req.method == QLatin1String("POST") && req.path == QLatin1String("/api/downloads/torrent")) return handleAddTorrentFromUpload(socket, req);

    static const QString kPrefix = QStringLiteral("/api/downloads/");
    if (req.method == QLatin1String("POST") && req.path.startsWith(kPrefix)) {
        const QStringList segs = req.path.mid(kPrefix.size()).split(u'/', Qt::SkipEmptyParts);
        if (segs.size() == 2 && (segs[1] == QLatin1String("pause") || segs[1] == QLatin1String("resume") ||
                                  segs[1] == QLatin1String("cancel") || segs[1] == QLatin1String("remove"))) {
            bool ok = false;
            const quint64 id = segs[0].toULongLong(&ok);
            if (ok) return handleDownloadAction(socket, id, segs[1]);
        }
    }

    writeJsonError(socket, 404, "not found");
}

void RemoteServer::handleSession(QTcpSocket* socket, const ParsedHttpRequest& req) {
    QJsonObject obj;
    obj["authenticated"] = isAuthenticated(req);
    obj["configured"] = hasCredentials();
    writeJson(socket, 200, obj);
}

void RemoteServer::handleLogin(QTcpSocket* socket, const ParsedHttpRequest& req) {
    const QString clientKey = socket->peerAddress().toString();
    if (!checkRateLimit(clientKey)) {
        writeJsonError(socket, 429, "too many attempts - try again in a minute");
        return;
    }
    if (!hasCredentials()) {
        writeJsonError(socket, 503, "remote access has not been configured yet");
        return;
    }

    const QJsonObject body = QJsonDocument::fromJson(req.body).object();
    const QString user = body.value("username").toString();
    const QString pass = body.value("password").toString();

    if (user.isEmpty() || pass.isEmpty() || !verifyCredentials(user, pass)) {
        recordFailedLogin(clientKey);
        writeJsonError(socket, 401, "invalid username or password");
        return;
    }

    recordSuccessfulLogin(clientKey);
    const QString token = createSession();
    const QByteArray cookie = "orbit_session=" + token.toLatin1() +
                               "; Path=/; HttpOnly; SameSite=Lax; Max-Age=" + QByteArray::number(kSessionTtlSeconds);
    writeJson(socket, 200, QJsonObject{{"ok", true}}, {{"Set-Cookie", cookie}});
}

void RemoteServer::handleLogout(QTcpSocket* socket, const ParsedHttpRequest& req) {
    const QString token = extractCookieValue(req.headers.value(QStringLiteral("cookie")), QStringLiteral("orbit_session"));
    if (!token.isEmpty()) sessions_.remove(token);
    const QByteArray cookie = "orbit_session=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0";
    writeJson(socket, 200, QJsonObject{{"ok", true}}, {{"Set-Cookie", cookie}});
}

void RemoteServer::handleDownloadsList(QTcpSocket* socket) {
    writeResponse(socket, 200, "application/json", snapshotsPayload());
}

void RemoteServer::handleDownloadAction(QTcpSocket* socket, quint64 id, const QString& action) {
    const auto did = static_cast<DownloadId>(id);
    if (action == QLatin1String("pause")) manager_.pause(did);
    else if (action == QLatin1String("resume")) manager_.resume(did);
    else if (action == QLatin1String("cancel")) manager_.cancel(did);
    else if (action == QLatin1String("remove")) manager_.remove(did);
    writeJson(socket, 200, QJsonObject{{"ok", true}});
}

void RemoteServer::handleAddDownload(QTcpSocket* socket, const ParsedHttpRequest& req) {
    const QJsonObject body = QJsonDocument::fromJson(req.body).object();
    const QString source = body.value("source").toString().trimmed();
    if (source.isEmpty()) {
        writeJsonError(socket, 400, "missing source");
        return;
    }

    const std::string sourceStd = source.toStdString();
    const bool isMagnet = orbit::utils::isMagnetUri(sourceStd);
    const bool isHttp = orbit::utils::startsWith(sourceStd, "http://") || orbit::utils::startsWith(sourceStd, "https://");
    if (!isMagnet && !isHttp) {
        writeJsonError(socket, 400, "source must be a magnet link or http(s) URL - upload .torrent files via /api/torrents/parse");
        return;
    }

    DownloadRequest request;
    request.source = sourceStd;
    request.destinationDir = manager_.defaultDownloadDir().string();
    request.maxConnections = std::clamp(body.value("maxConnections").toInt(8), 1, 32);

    DownloadId id;
    if (isMagnet) {
        id = manager_.addTorrent(request);
    } else if (orbit::utils::isMediaUrl(request.source)) {
        id = manager_.addMediaDownload(request);
    } else {
        id = manager_.addHttpDownload(request);
    }
    writeJson(socket, 200, QJsonObject{{"ok", true}, {"id", QString::number(static_cast<qulonglong>(id))}});
}

QString RemoteServer::uploadsDirPath() const {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/OrbitRemoteUploads");
    QDir().mkpath(dir);
    return dir;
}

void RemoteServer::handleTorrentParse(QTcpSocket* socket, const ParsedHttpRequest& req) {
    if (req.body.isEmpty()) {
        writeJsonError(socket, 400, "empty upload");
        return;
    }

    const QString token = QString::fromLatin1(randomBytes(16).toHex());
    const QString filePath = uploadsDirPath() + QStringLiteral("/") + token + QStringLiteral(".torrent");

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        writeJsonError(socket, 500, "could not store upload");
        return;
    }
    file.write(req.body);
    file.close();

    std::string name;
    std::vector<TorrentFileEntry> files;
    std::string error;
    const bool ok = TorrentEngine::parseTorrentFile(filePath.toStdString(), name, files, error);
    if (!ok) {
        QFile::remove(filePath);
        writeJsonError(socket, 400, QString::fromStdString(error.empty() ? "could not read torrent file" : error));
        return;
    }

    pendingTorrentUploads_.insert(token, PendingTorrentUpload{filePath, QString::fromStdString(name),
                                                                QDateTime::currentDateTime().addSecs(kTorrentUploadTtlSeconds)});

    QJsonArray filesArr;
    for (size_t i = 0; i < files.size(); ++i) {
        QJsonObject f;
        f["index"] = static_cast<int>(i);
        f["path"] = QString::fromStdString(files[i].path);
        f["size"] = static_cast<double>(files[i].size);
        filesArr.append(f);
    }

    QJsonObject obj;
    obj["ok"] = true;
    obj["token"] = token;
    obj["name"] = QString::fromStdString(name);
    obj["files"] = filesArr;
    writeJson(socket, 200, obj);
}

void RemoteServer::handleAddTorrentFromUpload(QTcpSocket* socket, const ParsedHttpRequest& req) {
    const QJsonObject body = QJsonDocument::fromJson(req.body).object();
    const QString token = body.value("token").toString();

    const auto it = pendingTorrentUploads_.find(token);
    if (it == pendingTorrentUploads_.end() || QDateTime::currentDateTime() > it->expiresAt) {
        if (it != pendingTorrentUploads_.end()) pendingTorrentUploads_.erase(it);
        writeJsonError(socket, 410, "upload expired - please re-upload the torrent file");
        return;
    }

    DownloadRequest request;
    request.source = it->filePath.toStdString();
    request.suggestedName = it->name.toStdString();
    request.destinationDir = manager_.defaultDownloadDir().string();

    const QJsonArray excludedArr = body.value("excludedFileIndices").toArray();
    request.torrentExcludedFileIndices.reserve(static_cast<size_t>(excludedArr.size()));
    for (const QJsonValue& v : excludedArr) request.torrentExcludedFileIndices.push_back(v.toInt());

    pendingTorrentUploads_.erase(it);

    const DownloadId id = manager_.addTorrent(request);
    writeJson(socket, 200, QJsonObject{{"ok", true}, {"id", QString::number(static_cast<qulonglong>(id))}});
}

void RemoteServer::pruneTorrentUploads() {
    const QDateTime now = QDateTime::currentDateTime();
    for (auto it = pendingTorrentUploads_.begin(); it != pendingTorrentUploads_.end();) {
        if (now > it->expiresAt) it = pendingTorrentUploads_.erase(it);
        else ++it;
    }
}

void RemoteServer::purgeStaleUploadFiles() const {
    QDir dir(uploadsDirPath());
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (const QFileInfo& info : dir.entryInfoList(QDir::Files)) {
        if (nowMs - info.lastModified().toMSecsSinceEpoch() > kUploadFileMaxAgeMs) {
            QFile::remove(info.absoluteFilePath());
        }
    }
}

void RemoteServer::serveAsset(QTcpSocket* socket, const QString& resourcePath, const QByteArray& contentType) {
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        writeJsonError(socket, 404, "not found");
        return;
    }
    writeResponse(socket, 200, contentType, file.readAll());
}

bool RemoteServer::isAuthenticated(const ParsedHttpRequest& req) {
    const QString token = extractCookieValue(req.headers.value(QStringLiteral("cookie")), QStringLiteral("orbit_session"));
    if (token.isEmpty()) return false;

    auto it = sessions_.find(token);
    if (it == sessions_.end()) return false;
    if (QDateTime::currentDateTime() > it.value()) {
        sessions_.erase(it);
        return false;
    }
    it.value() = QDateTime::currentDateTime().addSecs(kSessionTtlSeconds);
    return true;
}

QString RemoteServer::createSession() {
    pruneSessions();
    const QString token = QString::fromLatin1(randomBytes(32).toHex());
    sessions_.insert(token, QDateTime::currentDateTime().addSecs(kSessionTtlSeconds));
    return token;
}

void RemoteServer::pruneSessions() {
    const QDateTime now = QDateTime::currentDateTime();
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (now > it.value()) it = sessions_.erase(it);
        else ++it;
    }
}

bool RemoteServer::checkRateLimit(const QString& clientKey) const {
    const auto it = loginAttempts_.constFind(clientKey);
    if (it == loginAttempts_.constEnd()) return true;
    if (it->lockedUntil.isValid() && QDateTime::currentDateTime() < it->lockedUntil) return false;
    return true;
}

void RemoteServer::recordFailedLogin(const QString& clientKey) {
    LoginAttempts& attempts = loginAttempts_[clientKey];
    ++attempts.failures;
    if (attempts.failures >= kMaxLoginFailures) {
        attempts.lockedUntil = QDateTime::currentDateTime().addSecs(kLoginLockoutSeconds);
        attempts.failures = 0;
    }
}

void RemoteServer::recordSuccessfulLogin(const QString& clientKey) { loginAttempts_.remove(clientKey); }

void RemoteServer::writeResponse(QTcpSocket* socket, int status, const QByteArray& contentType,
                                  const QByteArray& body, const QList<std::pair<QByteArray, QByteArray>>& extraHeaders) {
    bool hasCacheControl = false;
    for (const auto& [key, value] : extraHeaders) {
        if (key.compare("Cache-Control", Qt::CaseInsensitive) == 0) hasCacheControl = true;
    }

    QByteArray resp;
    resp += "HTTP/1.1 " + QByteArray::number(status) + " " + httpStatusText(status) + "\r\n";
    resp += "Content-Type: " + contentType + "\r\n";
    resp += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    resp += "Connection: close\r\n";
    resp += "X-Content-Type-Options: nosniff\r\n";
    if (!hasCacheControl) resp += "Cache-Control: no-store\r\n";
    for (const auto& [key, value] : extraHeaders) resp += key + ": " + value + "\r\n";
    resp += "\r\n";
    resp += body;

    socket->write(resp);
    socket->disconnectFromHost();
}

void RemoteServer::writeJson(QTcpSocket* socket, int status, const QJsonObject& obj,
                              const QList<std::pair<QByteArray, QByteArray>>& extraHeaders) {
    writeResponse(socket, status, "application/json", QJsonDocument(obj).toJson(QJsonDocument::Compact), extraHeaders);
}

void RemoteServer::writeJsonError(QTcpSocket* socket, int status, const QString& message) {
    writeJson(socket, status, QJsonObject{{"error", message}});
}

}
