#include "core/ProtocolHandler.hpp"

#include "utils/StringUtils.hpp"

#if defined(_WIN32)
#include <QDir>
#include <QSettings>
#include <QString>
#endif

namespace orbit::core::protocol {

bool isOrbitUri(const std::string& uri) noexcept {
    return orbit::utils::startsWith(uri, kScheme);
}

std::optional<std::string> extractDownloadUrl(const std::string& uri) {
    if (!isOrbitUri(uri)) return std::nullopt;

    const std::string rest = uri.substr(std::string(kScheme).size());
    const auto queryPos = rest.find('?');
    if (queryPos == std::string::npos) return std::nullopt;

    // Browsers commonly normalize a bare authority with no path by inserting a "/"
    // (orbit://add?url=X becomes orbit://add/?url=X) before handing the URI to the OS,
    // so tolerate one trailing slash on the action segment.
    std::string action = rest.substr(0, queryPos);
    if (!action.empty() && action.back() == '/') action.pop_back();
    if (action != "add") return std::nullopt;

    const std::string query = rest.substr(queryPos + 1);
    for (const std::string& pair : orbit::utils::split(query, '&')) {
        const auto eq = pair.find('=');
        if (eq == std::string::npos) continue;
        if (pair.substr(0, eq) != "url") continue;

        std::string decoded = orbit::utils::trim(orbit::utils::urlDecode(pair.substr(eq + 1)));
        if (decoded.empty()) return std::nullopt;
        return decoded;
    }
    return std::nullopt;
}

#if defined(_WIN32)
void registerWindowsProtocolHandler(const std::string& executablePath) {
    // Qt paths use '/' internally; Windows' URL-protocol activation path is pickier about
    // that than a plain CreateProcess call, so write a proper native (backslash) path.
    const QString exe = QDir::toNativeSeparators(QString::fromStdString(executablePath));
    // The trailing %1 is a literal placeholder the OS substitutes with the launched URI —
    // it must not be touched by QString::arg().
    const QString command = QStringLiteral("\"") + exe + QStringLiteral("\" \"%1\"");

    QSettings root(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\orbit"), QSettings::NativeFormat);
    root.setValue(QStringLiteral("."), QStringLiteral("URL:Orbit Protocol"));
    root.setValue(QStringLiteral("URL Protocol"), QStringLiteral(""));

    QSettings icon(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\orbit\\DefaultIcon"), QSettings::NativeFormat);
    icon.setValue(QStringLiteral("."), exe + QStringLiteral(",0"));

    QSettings command_(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\orbit\\shell\\open\\command"),
                        QSettings::NativeFormat);
    command_.setValue(QStringLiteral("."), command);
}

void registerWindowsTorrentFileAssociation(const std::string& executablePath) {
    const QString exe = QDir::toNativeSeparators(QString::fromStdString(executablePath));
    const QString command = QStringLiteral("\"") + exe + QStringLiteral("\" \"%1\"");

    // Point the .torrent extension at our own ProgID rather than overwriting whatever
    // ProgID another torrent client may already own — this makes Orbit a selectable
    // candidate. Windows' UserChoice protection means an existing default still has to
    // be switched explicitly (Settings > Default apps, or "Open with" > "Always use"),
    // which is by design — no app can silently seize a file association out from under
    // another on modern Windows.
    QSettings ext(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\.torrent"), QSettings::NativeFormat);
    ext.setValue(QStringLiteral("."), QStringLiteral("Orbit.Torrent"));

    QSettings progid(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\Orbit.Torrent"), QSettings::NativeFormat);
    progid.setValue(QStringLiteral("."), QStringLiteral("BitTorrent File"));

    QSettings icon(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\Orbit.Torrent\\DefaultIcon"),
                    QSettings::NativeFormat);
    icon.setValue(QStringLiteral("."), exe + QStringLiteral(",0"));

    QSettings command_(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\Orbit.Torrent\\shell\\open\\command"),
                        QSettings::NativeFormat);
    command_.setValue(QStringLiteral("."), command);
}
#endif

}
