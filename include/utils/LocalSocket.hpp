#pragma once

#include <QString>

#if !defined(_WIN32)
#include <QDir>
#include <QStandardPaths>
#endif

namespace orbit::utils {

// The address Orbit's local servers listen on. Windows named pipes are per-session
// already; elsewhere a bare name would become a socket in the shared /tmp, where another
// user could squat it, so it goes in the user's private runtime directory instead
// ($XDG_RUNTIME_DIR, mode 0700). orbit-native-host resolves the same path.
inline QString localServerName(const QString& name) {
#if defined(_WIN32)
    return name;
#else
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) return name;
    QDir().mkpath(dir);
    return dir + QLatin1Char('/') + name;
#endif
}

}
