#pragma once

#include <QObject>
#include <QString>

#include <functional>

// Linux counterparts of the Windows-only pieces Orbit relies on: power requests and
// shutdown (systemd-logind and the desktop's inhibit portal over D-Bus), revealing a file
// in the file manager, the orbit:// and .torrent registration (a .desktop entry), and
// DPAPI-style protection for stored secrets.
namespace linux_integration {

// Blocks automatic (idle) suspend while held, like a Windows SystemRequired power
// request: the user can still put the PC to sleep on purpose.
class SleepInhibitor {
public:
    SleepInhibitor() = default;
    ~SleepInhibitor();
    SleepInhibitor(const SleepInhibitor&) = delete;
    SleepInhibitor& operator=(const SleepInhibitor&) = delete;

    bool acquire(const QString& reason);
    void release();
    [[nodiscard]] bool held() const noexcept { return portalHandle_.size() > 0 || logindFd_ >= 0; }

private:
    QString portalHandle_;
    int logindFd_{-1};
};

[[nodiscard]] bool canHibernate();
// "sleep", "hibernate" or "shutdown" through logind, without blocking the UI (polkit may
// ask for a password). done() gets an error message, or an empty string once logind
// accepted the request.
void requestPowerAction(const QString& action, QObject* context, std::function<void(const QString&)> done);

// Sets or clears _KDE_NET_WM_BLUR_BEHIND_REGION on an X11 window (the whole window).
void setBlurBehind(quintptr windowId, bool enabled);

// Opens the file manager with the file selected (org.freedesktop.FileManager1).
bool revealInFileManager(const QString& filePath);

// Records where a finished download came from, as browsers on Linux do: the link in
// user.xdg.origin.url and the page it was on in user.xdg.referrer.url. Dolphin and
// Files show these. Sign-in details in a link are left out, and only http(s) and ftp
// links are recorded. False when nothing could be written (e.g. no user xattrs on that
// file system).
bool tagDownloadOrigin(const QString& filePath, const QString& origin, const QString& referrer);

// The link as tagDownloadOrigin stores it: empty unless http(s) or ftp, without a
// username or password.
QString originForTag(const QString& url);

// Writes ~/.local/share/applications/orbit.desktop for this executable, listing
// orbit:// links and .torrent files, and makes Orbit the orbit:// handler. Like the
// Windows registration, an existing default app for .torrent files is left alone.
void registerDesktopEntry(const QString& executablePath);

// Puts the extras install.sh installs (shell completions, the systemd user service for
// headless mode, Dolphin and Files menus) in place from the share/ folder beside the
// program. An in-app update only replaces the program folder, so this is how updated
// installs get new and changed extras. A file the user edited is never overwritten.
void syncExtras(const QString& appDir);

// Encrypts a secret with a per-user key kept in Orbit's data folder (readable only by
// this user), so a copied settings file doesn't reveal it.
QString protectSecret(const QString& plain);
QString unprotectSecret(const QString& stored);

}
