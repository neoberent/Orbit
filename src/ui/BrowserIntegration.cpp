#include "BrowserIntegration.hpp"

#include "utils/FileUtils.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

namespace browser_integration {

namespace {

#if defined(_WIN32)
struct RegistryTarget {
    const char* key;
    bool firefox;
};

constexpr RegistryTarget kTargets[] = {
    {"HKEY_CURRENT_USER\\Software\\Google\\Chrome\\NativeMessagingHosts\\", false},
    {"HKEY_CURRENT_USER\\Software\\Microsoft\\Edge\\NativeMessagingHosts\\", false},
    {"HKEY_CURRENT_USER\\Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts\\", false},
    {"HKEY_CURRENT_USER\\Software\\Chromium\\NativeMessagingHosts\\", false},
    {"HKEY_CURRENT_USER\\Software\\Mozilla\\NativeMessagingHosts\\", true},
};
#else
// Browsers read per-user host manifests from fixed folders. Only browsers that have
// created their profile folder get one; Orbit re-checks on every launch, so a browser
// installed later is picked up the next time Orbit starts.
struct ManifestTarget {
    const char* browserDir;    // under $HOME; must exist
    const char* manifestDir;   // relative to browserDir
    bool firefox;
};

constexpr ManifestTarget kTargets[] = {
    {".config/google-chrome", "NativeMessagingHosts", false},
    {".config/google-chrome-beta", "NativeMessagingHosts", false},
    {".config/google-chrome-unstable", "NativeMessagingHosts", false},
    {".config/chromium", "NativeMessagingHosts", false},
    {".config/BraveSoftware/Brave-Browser", "NativeMessagingHosts", false},
    {".config/microsoft-edge", "NativeMessagingHosts", false},
    {".config/microsoft-edge-beta", "NativeMessagingHosts", false},
    {".config/vivaldi", "NativeMessagingHosts", false},
    {".config/opera", "NativeMessagingHosts", false},
    {".config/thorium", "NativeMessagingHosts", false},
    {".mozilla", "native-messaging-hosts", true},
    {".config/mozilla", "native-messaging-hosts", true},
    {".librewolf", "native-messaging-hosts", true},
    {".waterfox", "native-messaging-hosts", true},
    {".floorp", "native-messaging-hosts", true},
    {".zen", "native-messaging-hosts", true},
};

QString targetManifestPath(const ManifestTarget& target) {
    return QDir::homePath() + QLatin1Char('/') + QString::fromLatin1(target.browserDir) + QLatin1Char('/') +
           QString::fromLatin1(target.manifestDir) + QLatin1Char('/') + QString::fromLatin1(kNativeHostName) +
           QStringLiteral(".json");
}

bool browserPresent(const ManifestTarget& target) {
    return QFileInfo(QDir::homePath() + QLatin1Char('/') + QString::fromLatin1(target.browserDir)).isDir();
}

bool writeManifest(const QString& path, const QByteArray& content) {
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == content) return true;
    existing.close();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(content) == content.size();
}
#endif

#if defined(_WIN32)
QString manifestDir() {
    return QString::fromStdWString((orbit::utils::appDataDir() / "native-messaging").wstring());
}

QString manifestPath(bool firefox) {
    return manifestDir() + (firefox ? QStringLiteral("/com.orbit.downloadmanager.firefox.json")
                                    : QStringLiteral("/com.orbit.downloadmanager.json"));
}
#endif

QByteArray manifestJson(bool firefox) {
    QJsonObject manifest;
    manifest["name"] = QString::fromLatin1(kNativeHostName);
    manifest["description"] = QStringLiteral("Orbit Download Manager");
    manifest["path"] = QDir::toNativeSeparators(nativeHostExecutable());
    manifest["type"] = QStringLiteral("stdio");
    if (firefox) {
        manifest["allowed_extensions"] = QJsonArray{QString::fromLatin1(kFirefoxExtensionId)};
    } else {
        manifest["allowed_origins"] =
            QJsonArray{QStringLiteral("chrome-extension://") + QString::fromLatin1(kChromeExtensionId) + QStringLiteral("/")};
    }
    return QJsonDocument(manifest).toJson(QJsonDocument::Indented);
}

}

QString nativeHostExecutable() {
#if defined(_WIN32)
    return QCoreApplication::applicationDirPath() + QStringLiteral("/orbit-native-host.exe");
#else
    return QCoreApplication::applicationDirPath() + QStringLiteral("/orbit-native-host");
#endif
}

QString registerNativeMessagingHost() {
#if defined(_WIN32)
    if (!QFileInfo::exists(nativeHostExecutable())) return QStringLiteral("orbit-native-host.exe is missing next to Orbit");
    if (!QDir().mkpath(manifestDir())) return QStringLiteral("Couldn't create ") + manifestDir();

    for (bool firefox : {false, true}) {
        QFile file(manifestPath(firefox));
        const QByteArray content = manifestJson(firefox);
        if (file.exists() && file.open(QIODevice::ReadOnly) && file.readAll() == content) {
            file.close();
            continue;
        }
        file.close();
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QStringLiteral("Couldn't write ") + file.fileName();
        file.write(content);
    }

    for (const RegistryTarget& target : kTargets) {
        QSettings key(QString::fromLatin1(target.key) + QString::fromLatin1(kNativeHostName), QSettings::NativeFormat);
        key.setValue(QStringLiteral("."), QDir::toNativeSeparators(manifestPath(target.firefox)));
    }
    return {};
#else
    if (!QFileInfo(nativeHostExecutable()).isExecutable()) return QStringLiteral("orbit-native-host is missing next to Orbit");
    QString failed;
    int written = 0;
    for (const ManifestTarget& target : kTargets) {
        if (!browserPresent(target)) continue;
        const QString path = targetManifestPath(target);
        if (writeManifest(path, manifestJson(target.firefox))) ++written;
        else failed = QStringLiteral("Couldn't write ") + path;
    }
    if (!failed.isEmpty()) return failed;
    if (written == 0) return QStringLiteral("No supported browser found. Start your browser once, then reopen Orbit");
    return {};
#endif
}

void unregisterNativeMessagingHost() {
#if defined(_WIN32)
    for (const RegistryTarget& target : kTargets) {
        QSettings parent(QString::fromLatin1(target.key).chopped(1), QSettings::NativeFormat);
        parent.remove(QString::fromLatin1(kNativeHostName));
    }
    QFile::remove(manifestPath(false));
    QFile::remove(manifestPath(true));
#else
    for (const ManifestTarget& target : kTargets) QFile::remove(targetManifestPath(target));
#endif
}

bool isNativeMessagingHostRegistered() {
#if defined(_WIN32)
    QSettings key(QString::fromLatin1(kTargets[0].key) + QString::fromLatin1(kNativeHostName), QSettings::NativeFormat);
    const QString registered = key.value(QStringLiteral(".")).toString();
    if (registered.isEmpty() || !QFileInfo::exists(registered)) return false;
    QFile file(registered);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QJsonObject manifest = QJsonDocument::fromJson(file.readAll()).object();
    const QString exe = manifest.value("path").toString();
    return !exe.isEmpty() && QFileInfo(exe) == QFileInfo(nativeHostExecutable());
#else
    for (const ManifestTarget& target : kTargets) {
        QFile file(targetManifestPath(target));
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QString exe = QJsonDocument::fromJson(file.readAll()).object().value("path").toString();
        if (!exe.isEmpty() && QFileInfo(exe) == QFileInfo(nativeHostExecutable())) return true;
    }
    return false;
#endif
}

}
