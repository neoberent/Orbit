#include "LinuxIntegration.hpp"

#include "utils/FileUtils.hpp"
#include "utils/Logger.hpp"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QVariantMap>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace linux_integration {

namespace {

const QString kLogindService = QStringLiteral("org.freedesktop.login1");
const QString kLogindPath = QStringLiteral("/org/freedesktop/login1");
const QString kLogindManager = QStringLiteral("org.freedesktop.login1.Manager");

const QString kPortalService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPortalPath = QStringLiteral("/org/freedesktop/portal/desktop");

constexpr int kKeyBytes = 32;
constexpr int kIvBytes = 12;
constexpr int kTagBytes = 16;
const QByteArray kSecretPrefix = QByteArrayLiteral("v1:");

QDBusMessage logindCall(const QString& method) {
    return QDBusMessage::createMethodCall(kLogindService, kLogindPath, kLogindManager, method);
}

QString keyFilePath() {
    return QString::fromStdString((orbit::utils::appDataDir() / "secret.key").string());
}

// Reads the per-user key, creating it (mode 0600) on first use.
QByteArray secretKey(bool create) {
    const QString path = keyFilePath();
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray key = file.read(kKeyBytes + 1);
        if (key.size() == kKeyBytes) return key;
        ORBIT_LOG_WARN("secrets", "Ignoring a damaged key file: " + path.toStdString());
        return {};
    }
    if (!create) return {};

    QByteArray key(kKeyBytes, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(key.data()), kKeyBytes) != 1) return {};
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QByteArray native = QFile::encodeName(path);
    const int fd = ::open(native.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        // Another call won the race; use its key.
        return errno == EEXIST ? secretKey(false) : QByteArray();
    }
    const bool ok = ::write(fd, key.constData(), kKeyBytes) == kKeyBytes;
    ::close(fd);
    if (!ok) {
        QFile::remove(path);
        return {};
    }
    return key;
}

QString desktopExecQuote(const QString& path) {
    QString escaped;
    for (const QChar c : path) {
        if (c == QLatin1Char('"') || c == QLatin1Char('`') || c == QLatin1Char('$') || c == QLatin1Char('\\')) {
            escaped += QLatin1Char('\\');
        }
        escaped += c;
    }
    // A literal % has to be doubled so it isn't read as a field code.
    escaped.replace(QLatin1Char('%'), QStringLiteral("%%"));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

bool writeIfChanged(const QString& path, const QByteArray& content, QFileDevice::Permissions permissions = {}) {
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == content) return false;
    existing.close();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(content);
    if (permissions) file.setPermissions(permissions);
    return file.commit();
}

bool isDefaultOrbitHandler(const QString& mimeAppsList) {
    QFile file(mimeAppsList);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    bool inDefaults = false;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            inDefaults = line == QLatin1String("[Default Applications]");
        } else if (inDefaults && line.startsWith(QLatin1String("x-scheme-handler/orbit="))) {
            return line.section(QLatin1Char('='), 1).section(QLatin1Char(';'), 0, 0).trimmed() ==
                   QLatin1String("orbit.desktop");
        }
    }
    return false;
}

}

SleepInhibitor::~SleepInhibitor() { release(); }

bool SleepInhibitor::acquire(const QString& reason) {
    if (held()) return true;
    QDBusConnection session = QDBusConnection::sessionBus();
    if (session.isConnected()) {
        // The desktop's inhibit portal reaches whatever power manager the session runs
        // (PowerDevil on KDE, gnome-session on GNOME). Flag 4 = suspend.
        QDBusMessage call = QDBusMessage::createMethodCall(kPortalService, kPortalPath,
                                                           QStringLiteral("org.freedesktop.portal.Inhibit"),
                                                           QStringLiteral("Inhibit"));
        call << QString() << 4u << QVariantMap{{QStringLiteral("reason"), reason}};
        const QDBusReply<QDBusObjectPath> reply = session.call(call, QDBus::Block, 3000);
        if (reply.isValid()) {
            portalHandle_ = reply.value().path();
            return true;
        }
        ORBIT_LOG_WARN("power", "Inhibit portal unavailable: " + reply.error().message().toStdString());
    }

    // Without a portal, logind's idle lock stops the system's own idle suspend.
    QDBusMessage call = logindCall(QStringLiteral("Inhibit"));
    call << QStringLiteral("idle") << QStringLiteral("Orbit") << reason << QStringLiteral("block");
    const QDBusReply<QDBusUnixFileDescriptor> reply = QDBusConnection::systemBus().call(call, QDBus::Block, 3000);
    if (reply.isValid() && reply.value().isValid()) {
        logindFd_ = ::dup(reply.value().fileDescriptor());
        if (logindFd_ >= 0) return true;
    }
    ORBIT_LOG_ERROR("power", "Couldn't prevent sleep: " + reply.error().message().toStdString());
    return false;
}

void SleepInhibitor::release() {
    if (!portalHandle_.isEmpty()) {
        QDBusMessage close = QDBusMessage::createMethodCall(kPortalService, portalHandle_,
                                                            QStringLiteral("org.freedesktop.portal.Request"),
                                                            QStringLiteral("Close"));
        QDBusConnection::sessionBus().call(close, QDBus::NoBlock);
        portalHandle_.clear();
    }
    if (logindFd_ >= 0) {
        ::close(logindFd_);
        logindFd_ = -1;
    }
}

bool canHibernate() {
    const QDBusReply<QString> reply =
        QDBusConnection::systemBus().call(logindCall(QStringLiteral("CanHibernate")), QDBus::Block, 3000);
    return reply.isValid() && (reply.value() == QLatin1String("yes") || reply.value() == QLatin1String("challenge"));
}

void requestPowerAction(const QString& action, QObject* context, std::function<void(const QString&)> done) {
    QString method;
    if (action == QLatin1String("sleep")) method = QStringLiteral("Suspend");
    else if (action == QLatin1String("hibernate")) method = QStringLiteral("Hibernate");
    else if (action == QLatin1String("shutdown")) method = QStringLiteral("PowerOff");
    else {
        done(QStringLiteral("Unknown action"));
        return;
    }

    // interactive = true lets polkit ask for a password where the system requires one.
    QDBusMessage call = logindCall(method);
    call << true;
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call, 120000), context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context, [watcher, done = std::move(done)] {
        done(watcher->isError() ? watcher->error().message() : QString());
        watcher->deleteLater();
    });
}

bool revealInFileManager(const QString& filePath) {
    const QFileInfo info(filePath);
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("/org/freedesktop/FileManager1"),
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("ShowItems"));
    call << QStringList{QUrl::fromLocalFile(info.absoluteFilePath()).toString()} << QString();
    // Starting the file manager can take a while; fall back to the folder only if it fails.
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 15000));
    const QString folder = info.absolutePath();
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher, [watcher, folder] {
        if (watcher->isError()) {
            ORBIT_LOG_WARN("ui", "File manager couldn't select the file: " + watcher->error().message().toStdString());
            QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
        }
        watcher->deleteLater();
    });
    return true;
}

void registerDesktopEntry(const QString& executablePath) {
    const QString dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString appsDir = dataHome + QStringLiteral("/applications");
    const QString iconPath = dataHome + QStringLiteral("/icons/hicolor/256x256/apps/orbit.png");

    if (!QFileInfo::exists(iconPath)) {
        QDir().mkpath(QFileInfo(iconPath).absolutePath());
        const QImage logo(QStringLiteral(":/images/logo.png"));
        if (!logo.isNull()) logo.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(iconPath);
    }

    const QString entry = QStringLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Orbit\n"
        "GenericName=Download Manager\n"
        "Comment=Multi-connection HTTP and BitTorrent downloads\n"
        "Exec=%1 %u\n"
        "Icon=orbit\n"
        "Terminal=false\n"
        "Categories=Network;FileTransfer;\n"
        "MimeType=x-scheme-handler/orbit;application/x-bittorrent;\n"
        "StartupNotify=true\n"
        "StartupWMClass=orbit\n").arg(desktopExecQuote(executablePath));

    const bool changed = writeIfChanged(appsDir + QStringLiteral("/orbit.desktop"), entry.toUtf8());
    if (changed) {
        ORBIT_LOG_INFO("protocol", "Registered orbit.desktop for " + executablePath.toStdString());
        const QString updater = QStandardPaths::findExecutable(QStringLiteral("update-desktop-database"));
        if (!updater.isEmpty()) QProcess::startDetached(updater, {appsDir});
    }

    const QString configHome = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (!isDefaultOrbitHandler(configHome + QStringLiteral("/mimeapps.list"))) {
        const QString xdgMime = QStandardPaths::findExecutable(QStringLiteral("xdg-mime"));
        if (xdgMime.isEmpty() ||
            !QProcess::startDetached(xdgMime, {QStringLiteral("default"), QStringLiteral("orbit.desktop"),
                                               QStringLiteral("x-scheme-handler/orbit")})) {
            ORBIT_LOG_WARN("protocol", "Couldn't make Orbit the orbit:// handler (xdg-mime missing)");
        }
    }
}

QString protectSecret(const QString& plain) {
    if (plain.isEmpty()) return {};
    const QByteArray key = secretKey(true);
    if (key.isEmpty()) return {};

    const QByteArray input = plain.toUtf8();
    QByteArray iv(kIvBytes, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(iv.data()), kIvBytes) != 1) return {};
    QByteArray cipher(input.size(), Qt::Uninitialized);
    QByteArray tag(kTagBytes, Qt::Uninitialized);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int length = 0;
    int finalLength = 0;
    const bool ok =
        ctx && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvBytes, nullptr) == 1 &&
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, reinterpret_cast<const unsigned char*>(key.constData()),
                           reinterpret_cast<const unsigned char*>(iv.constData())) == 1 &&
        EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(cipher.data()), &length,
                          reinterpret_cast<const unsigned char*>(input.constData()), static_cast<int>(input.size())) == 1 &&
        EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(cipher.data()) + length, &finalLength) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagBytes, tag.data()) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return {};
    cipher.resize(length + finalLength);
    return QString::fromLatin1(kSecretPrefix + (iv + cipher + tag).toBase64());
}

QString unprotectSecret(const QString& stored) {
    if (stored.isEmpty()) return {};
    const QByteArray raw = stored.toLatin1();
    if (!raw.startsWith(kSecretPrefix)) {
        // Saved by an earlier Linux build, which only base64-encoded it.
        return QString::fromUtf8(QByteArray::fromBase64(raw));
    }
    const QByteArray blob = QByteArray::fromBase64(raw.mid(kSecretPrefix.size()));
    if (blob.size() < kIvBytes + kTagBytes) return {};
    const QByteArray key = secretKey(false);
    if (key.isEmpty()) return {};

    const QByteArray iv = blob.left(kIvBytes);
    const QByteArray tag = blob.right(kTagBytes);
    const QByteArray cipher = blob.mid(kIvBytes, blob.size() - kIvBytes - kTagBytes);
    QByteArray plain(cipher.size(), Qt::Uninitialized);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int length = 0;
    int finalLength = 0;
    const bool ok =
        ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvBytes, nullptr) == 1 &&
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, reinterpret_cast<const unsigned char*>(key.constData()),
                           reinterpret_cast<const unsigned char*>(iv.constData())) == 1 &&
        EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(plain.data()), &length,
                          reinterpret_cast<const unsigned char*>(cipher.constData()), static_cast<int>(cipher.size())) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagBytes, const_cast<char*>(tag.constData())) == 1 &&
        EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(plain.data()) + length, &finalLength) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return {};
    plain.resize(length + finalLength);
    const QString result = QString::fromUtf8(plain);
    plain.fill('\0');
    return result;
}

}
