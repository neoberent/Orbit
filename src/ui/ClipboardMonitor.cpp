#include "ClipboardMonitor.hpp"

#include "utils/Logger.hpp"
#include "utils/StringUtils.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QSettings>
#include <QUrl>

#if defined(__linux__)
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QProcess>
#include <QStandardPaths>
#endif

namespace {

constexpr int kMaxLinks = 100;
const char* kDefaultExtensions =
    "zip rar 7z tar gz bz2 xz iso img exe msi msix dmg pkg deb rpm apk mp4 mkv avi mov webm mp3 flac wav m4a pdf epub";

QStringList parseExtensions(const QString& text) {
    QStringList result;
    for (QString ext : text.split(QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts)) {
        ext = ext.trimmed().toLower();
        while (ext.startsWith(QLatin1Char('.')) || ext.startsWith(QLatin1Char('*'))) ext.remove(0, 1);
        if (!ext.isEmpty() && !result.contains(ext)) result << ext;
    }
    return result;
}

}

ClipboardMonitor::ClipboardMonitor(QObject* parent) : QObject(parent) {
    QSettings settings;
    enabled_ = settings.value("clipboard/enabled", true).toBool();
    catchMedia_ = settings.value("clipboard/catchMedia", false).toBool();
    extensions_ = parseExtensions(settings.value("clipboard/extensions", QString::fromLatin1(kDefaultExtensions)).toString());

    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        lastText_ = clipboard->text();
        connect(clipboard, &QClipboard::dataChanged, this, &ClipboardMonitor::onClipboardChanged);
    }
#if defined(__linux__)
    watchDesktopClipboard();
#endif
}

void ClipboardMonitor::setEnabled(bool enabled) {
    if (enabled == enabled_) return;
    enabled_ = enabled;
    QSettings().setValue("clipboard/enabled", enabled);
    emit settingsChanged();
}

void ClipboardMonitor::setExtensions(const QString& text) {
    const QStringList parsed = parseExtensions(text);
    if (parsed == extensions_) return;
    extensions_ = parsed;
    QSettings().setValue("clipboard/extensions", extensions_.join(QLatin1Char(' ')));
    emit settingsChanged();
}

void ClipboardMonitor::setCatchMedia(bool enabled) {
    if (enabled == catchMedia_) return;
    catchMedia_ = enabled;
    QSettings().setValue("clipboard/catchMedia", enabled);
    emit settingsChanged();
}

QStringList ClipboardMonitor::extractLinks(const QString& text) const {
    QStringList links;
    static const QRegularExpression kSeparators(QStringLiteral("[\\s<>\"']+"));
    for (QString token : text.split(kSeparators, Qt::SkipEmptyParts)) {
        while (!token.isEmpty() && QStringLiteral("),.;]").contains(token.back())) token.chop(1);
        if (token.startsWith(QLatin1String("magnet:?"), Qt::CaseInsensitive)) {
            if (!links.contains(token)) links << token;
        } else if (token.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) ||
                   token.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
            const QUrl url(token, QUrl::StrictMode);
            if (!url.isValid() || url.host().isEmpty()) continue;
            const QString file = url.fileName().toLower();
            const qsizetype dot = file.lastIndexOf(QLatin1Char('.'));
            const QString ext = dot >= 0 ? file.mid(dot + 1) : QString();
            const bool isFile = !ext.isEmpty() && extensions_.contains(ext);
            const bool isMedia = catchMedia_ && orbit::utils::isMediaUrl(token.toStdString());
            if ((isFile || isMedia) && !links.contains(token)) links << token;
        }
        if (links.size() >= kMaxLinks) break;
    }
    return links;
}

void ClipboardMonitor::onClipboardChanged() {
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard) return;
    handleText(clipboard->text());
}

void ClipboardMonitor::handleText(const QString& text) {
    if (text == lastText_) return;
    lastText_ = text;

    // Copying inside Orbit itself (e.g. "Copy Source Link") is not an offer to download.
    if (!enabled_ || QGuiApplication::applicationState() == Qt::ApplicationActive) return;
    if (text.size() > 200000) return;

    QStringList fresh;
    for (const QString& link : extractLinks(text)) {
        if (offered_.contains(link)) continue;
        offered_.insert(link);
        fresh << link;
    }
    if (fresh.isEmpty()) return;
    ORBIT_LOG_INFO("clipboard", "Offering " + std::to_string(fresh.size()) + " copied link(s)");
    emit linksDetected(fresh);
}

#if defined(__linux__)
// Wayland only tells the focused app about clipboard changes (and KWin passes them on to
// XWayland only while an X window has focus), so in the background Orbit would never see
// a copied link the way it does on Windows. Plasma's Klipper announces every change over
// D-Bus; on other Wayland desktops wl-paste --watch reports them through the
// data-control protocol. Qt's own notification still covers X11 sessions.
void ClipboardMonitor::watchDesktopClipboard() {
    const bool plasma = qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains(QLatin1String("KDE"), Qt::CaseInsensitive);
    if (plasma && QDBusConnection::sessionBus().connect(QStringLiteral("org.kde.klipper"), QStringLiteral("/klipper"),
                                                        QStringLiteral("org.kde.klipper.klipper"),
                                                        QStringLiteral("clipboardHistoryUpdated"), this,
                                                        SLOT(onKlipperChanged()))) {
        ORBIT_LOG_INFO("clipboard", "Watching the clipboard through Klipper");
        return;
    }
    if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) return;
    const QString wlPaste = QStandardPaths::findExecutable(QStringLiteral("wl-paste"));
    if (wlPaste.isEmpty()) {
        ORBIT_LOG_INFO("clipboard", "Install wl-clipboard to offer links copied while Orbit is in the background");
        return;
    }
    // Each change runs the command with the new text on stdin; a record separator marks
    // where one copy ends.
    wlPaste_ = new QProcess(this);
    wlPaste_->setProgram(wlPaste);
    wlPaste_->setArguments({QStringLiteral("--type"), QStringLiteral("text"), QStringLiteral("--watch"),
                            QStringLiteral("sh"), QStringLiteral("-c"), QStringLiteral("cat; printf '\\036'")});
    connect(wlPaste_, &QProcess::readyReadStandardOutput, this, [this] {
        wlPasteBuffer_ += wlPaste_->readAllStandardOutput();
        qsizetype end;
        while ((end = wlPasteBuffer_.indexOf('\x1e')) >= 0) {
            const QString text = QString::fromUtf8(wlPasteBuffer_.left(end));
            wlPasteBuffer_.remove(0, end + 1);
            handleText(text.endsWith(QLatin1Char('\n')) ? text.chopped(1) : text);
        }
        if (wlPasteBuffer_.size() > 400000) wlPasteBuffer_.clear();
    });
    wlPaste_->start(QIODevice::ReadOnly);
    ORBIT_LOG_INFO("clipboard", "Watching the clipboard through wl-paste");
}

void ClipboardMonitor::onKlipperChanged() {
    const QDBusMessage call =
        QDBusMessage::createMethodCall(QStringLiteral("org.kde.klipper"), QStringLiteral("/klipper"),
                                       QStringLiteral("org.kde.klipper.klipper"), QStringLiteral("getClipboardContents"));
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        const QDBusPendingReply<QString> reply = *watcher;
        if (!reply.isError()) handleText(reply.value());
        watcher->deleteLater();
    });
}
#endif
