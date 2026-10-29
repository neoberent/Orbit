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

// Opens the file manager with the file selected (org.freedesktop.FileManager1).
bool revealInFileManager(const QString& filePath);

// Writes ~/.local/share/applications/orbit.desktop for this executable, listing
// orbit:// links and .torrent files, and makes Orbit the orbit:// handler. Like the
// Windows registration, an existing default app for .torrent files is left alone.
void registerDesktopEntry(const QString& executablePath);

// Encrypts a secret with a per-user key kept in Orbit's data folder (readable only by
// this user), so a copied settings file doesn't reveal it.
QString protectSecret(const QString& plain);
QString unprotectSecret(const QString& stored);

}
