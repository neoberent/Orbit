#!/bin/sh
# Installs Orbit for the current user, the Linux counterpart of the Windows installer:
# no root needed, program files in ~/.local/lib/orbit (where in-app updates go), an
# `orbit` command in ~/.local/bin and a menu entry. Orbit registers orbit:// links,
# .torrent files and the browser extension's native connection itself when it starts.
#
#   ./install.sh [--yes]        run from the unpacked release (or installer/linux in a build)
#
# ORBIT_INSTALL_DIR overrides the install folder.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
dest=${ORBIT_INSTALL_DIR:-$HOME/.local/lib/orbit}
bindir=$HOME/.local/bin
apps=${XDG_DATA_HOME:-$HOME/.local/share}/applications
icons=${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps

# A release archive has everything beside this script; a source checkout uses the build.
src=$here
if [ ! -x "$src/orbit" ]; then
    src=$(cd "$here/../.." && pwd)/build-linux
    docs=$(cd "$here/.." && pwd)
    logo=$(cd "$here/../.." && pwd)/src/ui/assets/logo.png
else
    docs=$src
    logo=$src/orbit.png
fi
for f in orbit orbit-native-host; do
    if [ ! -x "$src/$f" ]; then
        echo "error: $src/$f is missing. Build Orbit first: cmake -B build-linux -G Ninja && cmake --build build-linux" >&2
        exit 1
    fi
done

if [ "${1:-}" != "--yes" ]; then
    echo "Orbit Download Manager will be installed to $dest"
    echo "Orbit is free software under the GNU GPL v3 ($docs/License.txt)."
    echo "Privacy policy: $docs/PrivacyPolicy.txt"
    printf "Continue? [y/N] "
    read -r answer
    case $answer in y|Y|yes|Yes) ;; *) echo "Cancelled."; exit 1 ;; esac
fi

missing=$(ldd "$src/orbit" 2>/dev/null | awk '/not found/ {print $1}')
if [ -n "$missing" ]; then
    echo "error: these libraries Orbit needs are not installed:" >&2
    echo "$missing" | sed 's/^/  /' >&2
    echo "On Arch/CachyOS: sudo pacman -S --needed qt6-base qt6-declarative qt6-svg libtorrent-rasterbar curl openssl" >&2
    echo "On Debian/Ubuntu: sudo apt install qt6-base-dev qt6-declarative-dev qml6-module-qtquick-controls qml6-module-qtquick-layouts libtorrent-rasterbar2.0 libcurl4" >&2
    exit 1
fi

# A running Orbit would keep the old binary; ask it to quit and wait for it.
if pgrep -x -u "$(id -u)" orbit >/dev/null 2>&1; then
    echo "Closing the running Orbit..."
    pkill -TERM -x -u "$(id -u)" orbit || true
    i=0
    while pgrep -x -u "$(id -u)" orbit >/dev/null 2>&1 && [ $i -lt 100 ]; do sleep 0.2; i=$((i + 1)); done
fi

mkdir -p "$dest"
dest=$(cd "$dest" && pwd)
for f in orbit orbit-native-host; do
    install -m 755 "$src/$f" "$dest/$f.new"
    mv -f "$dest/$f.new" "$dest/$f"
done
for f in License.txt PrivacyPolicy.txt Inter-LICENSE.txt; do
    if [ -f "$docs/$f" ]; then install -m 644 "$docs/$f" "$dest/$f"; fi
done
install -m 755 "$here/uninstall.sh" "$dest/uninstall.sh"

mkdir -p "$bindir" "$apps" "$icons"
ln -sf "$dest/orbit" "$bindir/orbit"
if [ -f "$logo" ]; then install -m 644 "$logo" "$icons/orbit.png"; fi

# Same entry Orbit writes on start-up, so the menu has it before the first launch.
exec_path=$(printf '%s' "$dest/orbit" | sed 's/[\\"`$]/\\&/g; s/%/%%/g')
cat > "$apps/orbit.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Orbit
GenericName=Download Manager
Comment=Multi-connection HTTP and BitTorrent downloads
Exec="$exec_path" %u
Icon=orbit
Terminal=false
Categories=Network;FileTransfer;
MimeType=x-scheme-handler/orbit;application/x-bittorrent;
StartupNotify=true
StartupWMClass=orbit
DESKTOP
if command -v update-desktop-database >/dev/null 2>&1; then update-desktop-database "$apps" >/dev/null 2>&1 || true; fi
if command -v xdg-mime >/dev/null 2>&1; then xdg-mime default orbit.desktop x-scheme-handler/orbit 2>/dev/null || true; fi

if ! command -v yt-dlp >/dev/null 2>&1 && [ ! -x "$dest/yt-dlp" ]; then
    echo "note: install yt-dlp and ffmpeg for YouTube/TikTok/Twitch downloads (Arch: sudo pacman -S yt-dlp ffmpeg)"
fi

echo "Orbit is installed. Start it from the application menu or run: orbit"
case ":$PATH:" in *":$bindir:"*) ;; *) echo "($bindir is not on your PATH; the menu entry works regardless)";; esac
