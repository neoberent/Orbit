#!/bin/sh
# Removes Orbit installed by install.sh. Your downloads, settings and download list stay
# (~/.config/Orbit and ~/.local/share/orbit), as on Windows; --purge removes them too.
set -eu

dest=$(cd "$(dirname "$0")" && pwd)
data=${XDG_DATA_HOME:-$HOME/.local/share}
config=${XDG_CONFIG_HOME:-$HOME/.config}

if pgrep -x -u "$(id -u)" orbit >/dev/null 2>&1; then
    pkill -TERM -x -u "$(id -u)" orbit || true
    i=0
    while pgrep -x -u "$(id -u)" orbit >/dev/null 2>&1 && [ $i -lt 100 ]; do sleep 0.2; i=$((i + 1)); done
fi

for dir in .config/google-chrome .config/google-chrome-beta .config/google-chrome-unstable .config/chromium \
           .config/BraveSoftware/Brave-Browser .config/microsoft-edge .config/microsoft-edge-beta .config/vivaldi \
           .config/opera .config/thorium; do
    rm -f "$HOME/$dir/NativeMessagingHosts/com.orbit.downloadmanager.json"
done
for dir in .mozilla .config/mozilla .librewolf .waterfox .floorp .zen; do
    rm -f "$HOME/$dir/native-messaging-hosts/com.orbit.downloadmanager.json"
done

rm -f "$data/applications/orbit.desktop" "$data/icons/hicolor/256x256/apps/orbit.png"
if command -v update-desktop-database >/dev/null 2>&1; then update-desktop-database "$data/applications" >/dev/null 2>&1 || true; fi
if [ "$(readlink "$HOME/.local/bin/orbit" 2>/dev/null)" = "$dest/orbit" ]; then rm -f "$HOME/.local/bin/orbit"; fi

if [ "${1:-}" = "--purge" ]; then
    rm -rf "$config/Orbit" "$data/orbit"
    echo "Removed Orbit's settings and download list."
fi

if [ -f "$dest/orbit" ] && [ -f "$dest/orbit-native-host" ]; then
    rm -f "$dest/orbit" "$dest/orbit-native-host" "$dest/License.txt" "$dest/PrivacyPolicy.txt" "$dest/Inter-LICENSE.txt" "$dest/uninstall.sh"
    rmdir "$dest" 2>/dev/null || true
fi
echo "Orbit was uninstalled."
