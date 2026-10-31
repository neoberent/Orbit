#pragma once

#include <QByteArray>
#include <QString>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

class QThread;

namespace orbit::tests {

// A small HTTP/1.1 file server on 127.0.0.1 for exercising the downloaders against a
// server whose behaviour the test controls. It runs on its own thread, so a test can
// block (waiting on a download) without starving it. Every response closes the
// connection, which keeps the server simple and is valid for any HTTP client.
class TestHttpServer {
public:
    struct Behaviour {
        QByteArray payload;
        bool ranges{true};
        // 0 = send everything at once; otherwise this many bytes every 10 ms per connection.
        int bytesPerTick{0};
        // Delays the response headers (HEAD and GET) by this long.
        int headerDelayMs{0};
        // Accepts connections but never answers them.
        bool hang{false};
        // Answers every request with this status and no body (0 = serve normally).
        int status{0};
        // Sent as Content-Disposition when set.
        QString fileName;
    };

    TestHttpServer();
    ~TestHttpServer();

    TestHttpServer(const TestHttpServer&) = delete;
    TestHttpServer& operator=(const TestHttpServer&) = delete;

    void setBehaviour(const Behaviour& behaviour);
    [[nodiscard]] Behaviour behaviour() const;

    [[nodiscard]] std::string url(const std::string& path = "/file.bin") const;

    [[nodiscard]] int getRequests() const noexcept { return getRequests_.load(); }
    [[nodiscard]] int rangedGetRequests() const noexcept { return rangedGetRequests_.load(); }
    [[nodiscard]] int peakConcurrentGets() const noexcept { return peakConcurrentGets_.load(); }
    [[nodiscard]] int openConnections() const noexcept { return openConnections_.load(); }

private:
    friend class TestHttpServerWorker;

    mutable std::mutex behaviourMutex_;
    Behaviour behaviour_;
    quint16 port_{0};

    std::atomic<int> getRequests_{0};
    std::atomic<int> rangedGetRequests_{0};
    std::atomic<int> concurrentGets_{0};
    std::atomic<int> peakConcurrentGets_{0};
    std::atomic<int> openConnections_{0};

    QThread* thread_{nullptr};
};

}
