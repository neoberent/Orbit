#include "TestHttpServer.hpp"

#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <future>

namespace orbit::tests {

class TestHttpServerWorker final : public QObject {
public:
    explicit TestHttpServerWorker(TestHttpServer& owner) : owner_(owner) {}

    // Qt deletes children after this destructor has run, and a socket closing then fires
    // `disconnected`, whose handler would reach into this half-destroyed worker (a UBSan
    // report that made tst_httpdownloader fail now and then). Silence and delete them first.
    ~TestHttpServerWorker() override {
        for (QTcpSocket* socket : findChildren<QTcpSocket*>()) {
            socket->disconnect();
            delete socket;
        }
    }

    quint16 listen() {
        server_ = new QTcpServer(this);
        connect(server_, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* socket = server_->nextPendingConnection()) accept(socket);
        });
        server_->listen(QHostAddress::LocalHost, 0);
        return server_->serverPort();
    }

private:
    void accept(QTcpSocket* socket) {
        ++owner_.openConnections_;
        auto buffer = std::make_shared<QByteArray>();
        auto handled = std::make_shared<bool>(false);
        connect(socket, &QTcpSocket::disconnected, socket, [this, socket] {
            --owner_.openConnections_;
            socket->deleteLater();
        });
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer, handled] {
            *buffer += socket->readAll();
            if (*handled || !buffer->contains("\r\n\r\n")) return;
            *handled = true;
            const TestHttpServer::Behaviour b = owner_.behaviour();
            if (b.hang) return;
            const QByteArray head = buffer->left(buffer->indexOf("\r\n\r\n"));
            if (b.headerDelayMs > 0) {
                QTimer::singleShot(b.headerDelayMs, socket, [this, socket, head, b] { respond(socket, head, b); });
            } else {
                respond(socket, head, b);
            }
        });
    }

    void respond(QTcpSocket* socket, const QByteArray& head, const TestHttpServer::Behaviour& b) {
        const QList<QByteArray> lines = head.split('\n');
        const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
        const bool isHead = requestLine.value(0) == "HEAD";

        const QByteArray path = requestLine.value(1);
        if (b.files.contains(path) || (!b.payloadPath.isEmpty() && path != b.payloadPath)) {
            const bool found = b.files.contains(path);
            const QByteArray body = found ? b.files.value(path) : QByteArray();
            socket->write((found ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 Not Found\r\n") + QByteArray("Content-Length: ") +
                          QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + (isHead ? QByteArray() : body));
            socket->disconnectFromHost();
            return;
        }

        if (b.status != 0) {
            socket->write("HTTP/1.1 " + QByteArray::number(b.status) + " Test\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
            return;
        }

        const qint64 total = b.payload.size();
        qint64 first = 0;
        qint64 last = total - 1;
        bool ranged = false;
        if (b.ranges && !(b.ignoreRangeOnGet && !isHead)) {
            static const QRegularExpression rangeRe(QStringLiteral("^range:\\s*bytes=(\\d+)-(\\d*)"),
                                                    QRegularExpression::CaseInsensitiveOption);
            for (const QByteArray& line : lines) {
                const auto m = rangeRe.match(QString::fromLatin1(line.trimmed()));
                if (!m.hasMatch()) continue;
                first = m.captured(1).toLongLong();
                if (!m.captured(2).isEmpty()) last = std::min(last, m.captured(2).toLongLong());
                ranged = true;
            }
        }
        if (ranged && first >= total) {
            socket->write("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" + QByteArray::number(total) +
                          "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
            return;
        }

        if (!isHead && b.maxConcurrentGets > 0 && owner_.concurrentGets_.load() >= b.maxConcurrentGets) {
            ++owner_.rejectedGets_;
            socket->write("HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
            return;
        }

        QByteArray headers = ranged ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
        headers += "Content-Type: application/octet-stream\r\n";
        headers += "Content-Length: " + QByteArray::number(isHead && b.advertisedSize > 0 ? b.advertisedSize : last - first + 1) + "\r\n";
        if (ranged) {
            headers += "Content-Range: bytes " + QByteArray::number(first) + "-" + QByteArray::number(last) + "/" +
                       QByteArray::number(total) + "\r\n";
        }
        if (b.ranges) headers += "Accept-Ranges: bytes\r\n";
        if (!b.extraHeaders.isEmpty()) headers += b.extraHeaders + "\r\n";
        if (!b.fileName.isEmpty()) headers += "Content-Disposition: attachment; filename=\"" + b.fileName.toUtf8() + "\"\r\n";
        headers += "Connection: close\r\n\r\n";
        socket->write(headers);

        if (isHead) {
            socket->disconnectFromHost();
            return;
        }

        const int getNumber = ++owner_.getRequests_;
        if (ranged) ++owner_.rangedGetRequests_;
        const int now = ++owner_.concurrentGets_;
        int peak = owner_.peakConcurrentGets_.load();
        while (now > peak && !owner_.peakConcurrentGets_.compare_exchange_weak(peak, now)) {
        }
        connect(socket, &QTcpSocket::disconnected, socket, [this] { --owner_.concurrentGets_; });

        const QByteArray body = b.payload.mid(first, last - first + 1);
        if (getNumber <= b.stallGets) {
            socket->write(body.left(b.stallAfterBytes));
            return;  // never finishes and never closes
        }
        if (b.bytesPerTick <= 0) {
            socket->write(body);
            socket->disconnectFromHost();
            return;
        }
        auto offset = std::make_shared<qint64>(0);
        auto* timer = new QTimer(socket);
        timer->setInterval(10);
        const int step = getNumber <= b.slowGets ? std::max(1, b.bytesPerTick / 8) : b.bytesPerTick;
        connect(timer, &QTimer::timeout, socket, [socket, timer, body, offset, step] {
            if (socket->state() != QAbstractSocket::ConnectedState) {
                timer->stop();
                return;
            }
            if (socket->bytesToWrite() > step * 4) return;
            socket->write(body.mid(*offset, step));
            *offset += step;
            if (*offset >= body.size()) {
                timer->stop();
                socket->disconnectFromHost();
            }
        });
        timer->start();
    }

    TestHttpServer& owner_;
    QTcpServer* server_{nullptr};
};

TestHttpServer::TestHttpServer() {
    thread_ = new QThread;
    thread_->start();
    auto* worker = new TestHttpServerWorker(*this);
    worker->moveToThread(thread_);
    QObject::connect(thread_, &QThread::finished, worker, &QObject::deleteLater);
    std::promise<quint16> port;
    QMetaObject::invokeMethod(worker, [worker, &port] { port.set_value(worker->listen()); });
    port_ = port.get_future().get();
}

TestHttpServer::~TestHttpServer() {
    thread_->quit();
    thread_->wait();
    delete thread_;
}

void TestHttpServer::setBehaviour(const Behaviour& behaviour) {
    std::scoped_lock lock(behaviourMutex_);
    behaviour_ = behaviour;
}

TestHttpServer::Behaviour TestHttpServer::behaviour() const {
    std::scoped_lock lock(behaviourMutex_);
    return behaviour_;
}

std::string TestHttpServer::url(const std::string& path) const {
    return "http://127.0.0.1:" + std::to_string(port_) + path;
}

}
