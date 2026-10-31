# Orbit Download Manager

**Website and downloads: [orbitdm.netlify.app](https://orbitdm.netlify.app/)** · [Releases](https://github.com/neoberent/Orbit-release/releases) · [Browser extension](https://github.com/neoberent/Orbit-Extension)

Orbit is a modern, cross-platform download manager inspired by Free Download Manager,
combining multi-connection (segmented) HTTP/HTTPS downloads with full BitTorrent support
(magnet links and `.torrent` files) behind a polished Qt Quick UI.

## Architecture

```
include/core/     Download engine and service headers
include/utils/    Small logging/string/filesystem helpers used by core and UI
src/core/         Implementation of DownloadManager, HttpDownloader (libcurl),
                   TorrentEngine (libtorrent-rasterbar), DownloadTask
src/utils/        Implementation of the utils headers
src/ui/           Qt Widgets + Qt Quick front end
src/ui/qml/       QML components (main window, dialogs, list delegate, theme)
tests/            Qt Test suites and a local test HTTP server
installer/        Inno Setup script and packaged legal documents for the Windows installer
```

The code is split into two static libraries and the GUI:

- **`orbit_core`** — the download engine (`DownloadManager`, `HttpDownloader` on libcurl,
  `TorrentEngine` on libtorrent-rasterbar, `MediaDownloader`, persistence). It links only
  `Qt6::Core` (`MediaDownloader` drives yt-dlp through `QProcess`) — no GUI or network modules.
- **`orbit_services`** — the app-level services built on the core: the remote web UI
  (`RemoteServer` and its embedded assets), `SingleInstanceGuard`, `ProtocolHandler` and
  `Scheduler`. It adds `Qt6::Network`.
- **`orbit`** — the Qt Widgets + Qt Quick executable. It only talks to the core through
  `DownloadManager`'s public API and its callback-based `ProgressReporter`, which
  `DownloadListModel` adapts into Qt signals for QML data binding.

## Features

- **Multi-connection HTTP(S) downloads**: segmented range-requested downloads (configurable
  connection count, default 8), direct writes to the correct file offset, pause/resume with
  a `.orbitresume` sidecar metadata file, live speed/ETA reporting. HTTP errors fail the
  download with the status code instead of being saved as the file, throttled or dropped
  segments are retried with exponential back-off, and global and per-download speed limits
  are enforced by a shared pacing limiter. Per-download settings (speed limit, connections)
  are saved with the task and survive a retry or restart. A resume whose file changed size
  on the server starts over instead of splicing two different files together.
- **File already exists**: when a download's file name is taken, Orbit asks whether to
  replace the file or keep both (or does either automatically, set in Settings > Downloads).
  Another download's unfinished file is never replaced, and downloads starting at the same
  moment never share a name.
- **Refresh an expired link**: a paused or failed direct download can be given a new link
  (Refresh Link... in its menu or the details pane). Orbit checks the new link first: the
  same file continues where it stopped, a different one starts over. While the dialog is
  open, starting the download again in the browser hands the fresh link, with its sign-in,
  straight to it.
- **Signed-in downloads**: the browser extension hands over the page's cookies, referrer,
  user agent and file name over native messaging. Cookies go through curl's cookie engine,
  so a redirect to another host never receives them, and they're deleted once the file is
  done.
- **Checksums**: an expected MD5/SHA-1/SHA-256/SHA-512 is verified when a download finishes
  (a mismatch marks it corrupt), and any finished file can be hashed on demand.
- **Rename and move**: files, paused partial downloads (with their resume data) and torrents
  (through libtorrent's `rename_file`/`move_storage`) can be renamed or moved at any time.
- **BitTorrent**: a single long-lived `libtorrent::session`, magnet links and `.torrent`
  files, per-file priorities, peers/trackers/pieces views, force recheck and reannounce,
  sequential download, global and per-torrent rate limits, seeding limits by ratio or time,
  a maintained public-tracker fallback list merged into non-private torrents for wider peer
  discovery, and a persisted DHT routing table so peer discovery doesn't cold-start on every
  launch.
- **Proxy**: HTTP or SOCKS5 (with authentication) for direct downloads, torrents, yt-dlp and
  update checks. The password is encrypted with Windows DPAPI (on Linux, AES-256-GCM with a
  per-user key file readable only by you).
- **Clipboard monitoring**: copied magnet links and links to files are offered for download
  from a small prompt by the notification area.
- **Power**: keeps the PC awake while downloading (Windows power requests; on Linux the
  desktop's inhibit portal, or logind), and can exit Orbit, sleep, hibernate or shut down
  once everything has finished (with a 30-second countdown to cancel).
- **Scheduler**: a download window on selected weekdays, a time-based speed limit, and a
  start time per download.
- **Backup and portable mode**: settings and the download list export to JSON (passwords and
  cookies excluded) and import on another PC. A `portable.flag` file next to `orbit.exe` keeps
  all data in a `Data` folder beside it; `ORBIT_DATA_DIR` points Orbit at any other folder.
- **Task persistence across restarts**: the full download list, including in-progress HTTP
  and torrent transfers, is written to disk and automatically resumes on the next launch —
  closing the app mid-download does not lose progress.
- **Media downloads**: YouTube, TikTok, Twitch and similar sites via a bundled yt-dlp, with
  quality selection, audio-only extraction (M4A, Opus, MP3), subtitles (embedded or as .srt),
  picking items from playlists and channels, and sign-in through cookies from the extension
  or read from a browser. yt-dlp keeps itself current: Orbit checks yt-dlp's GitHub releases
  daily (and after a media download fails), verifies the download against the release's
  SHA-256 list and test-runs it before swapping it in, and uses whichever of the updated and
  bundled copies is newer.
- **Remote web dashboard**: an optional, password-protected dashboard bound to 127.0.0.1,
  designed to be reached through a Cloudflare Tunnel.
- **Browser extension support**: the Orbit-Extension talks to `orbit-native-host(.exe)`, a
  small native messaging host that forwards requests to the running Orbit over a per-user
  named pipe (`OrbitDownloadManager.Api`; on Linux a Unix socket in `$XDG_RUNTIME_DIR`) and
  starts Orbit if needed. Older extensions fall back to the `orbit://` protocol handler.
- **Unified DownloadManager**: add HTTP, torrent or media sources, queue with a concurrency
  limit, pause/resume/cancel/remove by id, real-time updates fanned out to any number of
  subscribers.
- **Qt Quick UI**: an information-dense download table with sortable columns, multi-select
  (Ctrl/Shift/Ctrl+A) and bulk actions, a details pane with a live two-minute speed graph, a
  status bar with total throughput, grouped settings, dark and light themes, keyboard
  shortcuts, completion notifications and a tray icon with live status.

## Dependencies (Arch / CachyOS)

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-declarative qt6-svg \
    libtorrent-rasterbar boost curl openssl yt-dlp ffmpeg
```

## Dependencies (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build git pkg-config \
    qt6-base-dev qt6-declarative-dev qt6-quickcontrols2-dev \
    qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts \
    libcurl4-openssl-dev libssl-dev libtorrent-rasterbar-dev
```

If your distribution's `libtorrent-rasterbar-dev` predates libtorrent 2.0 (check with
`pkg-config --modversion libtorrent-rasterbar`), build libtorrent-rasterbar >= 2.0 from
source instead — Orbit's `TorrentEngine` uses the libtorrent 2.x API
(`session_params`, `load_torrent_file`, `torrent_flags`, ...).

## Build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

## Building & testing

The test suites (Qt Test, under `tests/`) are off by default. A Debug build with
AddressSanitizer and UndefinedBehaviorSanitizer is the recommended way to run them:

```bash
cmake -B build-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug -DORBIT_BUILD_TESTS=ON -DORBIT_SANITIZE=ON
cmake --build build-tests --parallel
ctest --test-dir build-tests --output-on-failure
```

The suites cover the utilities, the segmented HTTP downloader (against a local test server
with ranges, throttling and hangs), `DownloadManager` (resume after restart, export/import,
queue limit, checksums, name conflicts) and the remote web UI (assets, login, sessions,
lockout). Each test runs with its own private `HOME`, so your real profile is never touched.

GitHub Actions (`.github/workflows/ci.yml`) builds and runs the same configuration on Arch
Linux for every push to `main` and every pull request.

## Run

```bash
./build/orbit        # Linux/macOS
build\orbit.exe       # Windows
```

Downloads default to your OS "Downloads" folder; change it from the in-app Settings page.
Logs are written to the per-user app-data directory (`~/.local/share/orbit/orbit.log` on
Linux, `%APPDATA%\Orbit\orbit.log` on Windows). Task state and the persisted DHT routing
table live alongside the log in that same directory.

## Linux

Everything the Windows build does works on Linux too; the platform pieces live in
`src/ui/LinuxIntegration.cpp` and the `__linux__` branches next to the Windows code:

| Windows | Linux |
|---|---|
| Win32 layered splash window | override-redirect ARGB X window drawn from its own thread (`SplashScreen.cpp`) |
| Named pipe + named mutex | Unix sockets and a lock file in `$XDG_RUNTIME_DIR` (mode 0700) |
| `orbit-native-host.exe` + registry | `orbit-native-host` + manifests in each browser's `NativeMessagingHosts` folder |
| `orbit://` / `.torrent` in the registry | `~/.local/share/applications/orbit.desktop` + `xdg-mime` |
| Power requests, `SetSuspendState`, `ExitWindowsEx` | inhibit portal / logind `Inhibit`, logind `Suspend`/`Hibernate`/`PowerOff` |
| DPAPI | AES-256-GCM, key in the data folder (`secret.key`, 0600) |
| Explorer `/select` | `org.freedesktop.FileManager1.ShowItems` |
| Clipboard listener | Qt, plus Klipper (Plasma) or `wl-paste --watch` for Wayland in the background |
| Segoe UI | Inter, bundled (`src/ui/assets/fonts`, SIL OFL) |
| Inno Setup installer + in-app update | `installer/linux/build-package.sh` → `OrbitSetup-<v>-linux.tar.gz` with `install.sh`; the in-app updater installs the `linux` entry of `updates.json` |

Under a Wayland session Orbit runs through XWayland, because it positions its own windows
(the splash, the clipboard prompt by the tray, the restored main window), which Wayland
doesn't allow; set `QT_QPA_PLATFORM=wayland` to override. Settings live in
`~/.config/Orbit`, data in `~/.local/share/orbit`, and `install.sh` puts the program in
`~/.local/lib/orbit` (no root needed). `ORBIT_POWER_DRY_RUN=1` logs the when-done power
actions instead of carrying them out, for testing.

```bash
cmake -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
installer/linux/install.sh            # install for this user
installer/linux/build-package.sh      # release archive + size/SHA-256 for the website
```

## Windows build notes (this repository was built on Windows)

On Windows there is no system package manager for these libraries, so this project was
built against [vcpkg](https://github.com/microsoft/vcpkg):

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg.exe install qtbase[widgets,network,openssl,sql-sqlite] qtdeclarative qtsvg qtimageformats curl[openssl] openssl "libtorrent[core]" --triplet x64-windows
```

(The vcpkg port for libtorrent is named `libtorrent`, not `libtorrent-rasterbar`; it exports
a CMake package named `LibtorrentRasterbar` with target `LibtorrentRasterbar::torrent-rasterbar`,
which is what `CMakeLists.txt` looks for.)

Then configure with the vcpkg toolchain file:

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --parallel
```

Building Qt6 and libtorrent (with Boost) from source via vcpkg takes a long time (1-3+
hours) and several GB of disk on the first run; subsequent builds reuse vcpkg's binary
cache.

For best BitTorrent throughput, forward TCP+UDP port 51413 to this PC in your router; Orbit
logs a one-time message on startup if automatic UPnP/NAT-PMP port mapping failed.

## Windows installer

`installer/Orbit.iss` is an [Inno Setup](https://jrsoftware.org/isinfo.php) script that
packages a Release build (`cmake --build build --config Release`, run first) into a
per-user installer that requires no administrator rights: it installs to
`%LOCALAPPDATA%\Orbit`, adds Start Menu / optional desktop shortcuts, and registers an
uninstaller. The wizard shows the license (`installer/License.txt`: GPL-3.0 plus liability terms
under German law, in German and English) and the privacy policy (`installer/PrivacyPolicy.txt`,
GDPR) before installation can proceed.

Build the installer with the Inno Setup Compiler (`ISCC.exe`):

```powershell
ISCC.exe installer\Orbit.iss
```

The resulting `OrbitSetup-<version>.exe` is written to `installer\Output\`.

`installer\build-installer.ps1` does the whole release build in one step: it builds
`build-vcpkg`, signs `orbit.exe` and `orbit-native-host.exe`, compiles the installer (which
Inno Setup then signs together with its uninstaller), and prints the installer's size and
SHA-256 for the website's release feed.

### Code signing

Windows SmartScreen shows "Windows protected your PC" for installers that aren't signed with
a publicly trusted code-signing certificate, and unsigned programs are more likely to be
flagged by antivirus heuristics. `installer\sign.ps1` signs with SHA-256 and an RFC 3161
timestamp and supports three certificate sources, chosen through environment variables:

| Source | Variables |
|---|---|
| Certificate in the Windows store (USB tokens, Certum SimplySign and other cloud HSMs) | `ORBIT_SIGN_THUMBPRINT` |
| `.pfx` file | `ORBIT_SIGN_PFX`, `ORBIT_SIGN_PFX_PASSWORD` |
| Azure Artifact Signing (formerly Trusted Signing) | `ORBIT_SIGN_AZURE_DLIB`, `ORBIT_SIGN_AZURE_METADATA` |

With none set, the build still works and produces an unsigned installer (with a warning);
`-RequireSigning` turns that into an error for release builds.

## Known limitations (v1)

- Non-range-supporting HTTP servers fall back to a single connection, and resuming them restarts
  the file from the beginning
  (a paused/resumed download restarts from byte 0).
- The "file already exists" choice applies to direct downloads; media downloads keep yt-dlp's
  own behaviour of not downloading a video that is already there.
- No CLI/headless front end ships yet, though `orbit_core` has no GUI dependency and can host one.
- macOS packaging is not set up yet. The Linux archive uses the distribution's Qt 6 and
  libtorrent rather than bundling them.
- On GNOME under Wayland, links copied while Orbit is in the background aren't noticed
  (GNOME offers no clipboard-watching protocol); on Plasma and wlroots desktops they are.

## Downloads

Installers for Windows and Linux and the browser extension are on the
[Orbit-release](https://github.com/neoberent/Orbit-release/releases) page and at
[orbitdm.netlify.app](https://orbitdm.netlify.app). The browser extension's source is in
[Orbit-Extension](https://github.com/neoberent/Orbit-Extension).

## License

Copyright (C) 2026 Neo Berent

Orbit is free software: you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version. See [LICENSE](LICENSE). Under section 7 of
the GPL, the installer's `installer/License.txt` adds warranty and liability terms that follow
German law. Third-party components (Qt, libcurl, OpenSSL, libtorrent-rasterbar, yt-dlp, FFmpeg,
Inter) keep their own licenses.
