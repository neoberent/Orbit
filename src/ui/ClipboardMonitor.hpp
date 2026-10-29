#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class QProcess;

// Watches the clipboard for download links (magnet links, and http(s) links to files
// with a watched extension) copied in other apps, and reports them so Orbit can offer
// to download them.
class ClipboardMonitor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY settingsChanged)
    Q_PROPERTY(QString extensions READ extensions WRITE setExtensions NOTIFY settingsChanged)
    Q_PROPERTY(bool catchMedia READ catchMedia WRITE setCatchMedia NOTIFY settingsChanged)

public:
    explicit ClipboardMonitor(QObject* parent = nullptr);

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    void setEnabled(bool enabled);
    [[nodiscard]] QString extensions() const { return extensions_.join(QLatin1Char(' ')); }
    void setExtensions(const QString& text);
    [[nodiscard]] bool catchMedia() const noexcept { return catchMedia_; }
    void setCatchMedia(bool enabled);

    Q_INVOKABLE QStringList extractLinks(const QString& text) const;

signals:
    void settingsChanged();
    void linksDetected(const QStringList& links);

private:
    void onClipboardChanged();
    void handleText(const QString& text);
#if defined(__linux__)
    void watchDesktopClipboard();
    Q_SLOT void onKlipperChanged();
    QProcess* wlPaste_{nullptr};
    QByteArray wlPasteBuffer_;
#endif

    bool enabled_{true};
    bool catchMedia_{false};
    QStringList extensions_;
    QString lastText_;
    QSet<QString> offered_;
};
