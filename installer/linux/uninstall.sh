#!/bin/sh
# Removes Orbit installed by install.sh. Your downloads, settings and download list stay
# (~/.config/Orbit and ~/.local/share/orbit), as on Windows; --purge removes them too.
set -eu

dest=$(cd "$(dirname "$0")" && pwd)
data=${XDG_DATA_HOME:-$HOME/.local/share}
config=${XDG_CONFIG_HOME:-$HOME/.config}

# Only processes running this install's program, not anything else called "orbit". A
# replaced or removed binary shows as "<path> (deleted)".
installed_pids() {
    for proc in /proc/[0-9]*; do
        case $(readlink "$proc/exe" 2>/dev/null) in
            "$1/orbit" | "$1/orbit (deleted)") echo "${proc#/proc/}" ;;
        esac
    done
}
stop_installed() {
    pids=$(installed_pids "$1")
    [ -n "$pids" ] || return 0
    echo "Closing the running Orbit..."
    # shellcheck disable=SC2086
    kill -TERM $pids 2>/dev/null || true
    i=0
    while [ -n "$(installed_pids "$1")" ] && [ $i -lt 100 ]; do sleep 0.2; i=$((i + 1)); done
}
stop_installed "$dest"

for dir in .config/google-chrome .config/google-chrome-beta .config/google-chrome-unstable .config/chromium \
           .config/BraveSoftware/Brave-Browser .config/microsoft-edge .config/microsoft-edge-beta .config/vivaldi \
           .config/opera .config/thorium; do
    rm -f "$HOME/$dir/NativeMessagingHosts/com.orbit.downloadmanager.json"
done
for dir in .mozilla .config/mozilla .librewolf .waterfox .floorp .zen; do
    rm -f "$HOME/$dir/native-messaging-hosts/com.orbit.downloadmanager.json"
done

if command -v systemctl >/dev/null 2>&1 && [ -f "$config/systemd/user/orbit.service" ]; then
    systemctl --user disable --now orbit.service >/dev/null 2>&1 || true
fi
rm -f "$config/systemd/user/orbit.service" "$data/bash-completion/completions/orbit" "$data/zsh/site-functions/_orbit" \
      "$config/fish/completions/orbit.fish" "$data/kio/servicemenus/orbit-torrent.desktop" \
      "$data/kio/servicemenus/orbit-links.desktop" "$data/nautilus/scripts/Download with Orbit"
rm -f "$data/applications/orbit.desktop" "$data/icons/hicolor/256x256/apps/orbit.png"
if command -v update-desktop-database >/dev/null 2>&1; then update-desktop-database "$data/applications" >/dev/null 2>&1 || true; fi
if [ "$(readlink "$HOME/.local/bin/orbit" 2>/dev/null)" = "$dest/orbit" ]; then rm -f "$HOME/.local/bin/orbit"; fi

if [ "${1:-}" = "--purge" ]; then
    # Older versions kept their data in ~/.local/share/orbit even with XDG_DATA_HOME set.
    rm -rf "$config/Orbit" "$data/orbit" "$HOME/.local/share/orbit"
    echo "Removed Orbit's settings and download list."
fi

# Everything a release archive puts here (an in-app update unpacks the whole archive, so
# install.sh, orbit.png and share/ are here too), plus what an interrupted update leaves.
# Only Orbit's own names: anything else in the folder stays, and then so does the folder.
if [ -f "$dest/orbit" ] && [ -f "$dest/orbit-native-host" ]; then
    rm -f "$dest/orbit" "$dest/orbit-native-host" "$dest/License.txt" "$dest/PrivacyPolicy.txt" "$dest/Inter-LICENSE.txt" \
          "$dest/install.sh" "$dest/orbit.png" "$dest"/*.orbit-new "$dest/uninstall.sh"
    rm -rf "$dest/share/completions" "$dest"/.orbit-update.* "$dest"/.orbit-backup.*
    rm -f "$dest/share/orbit.service" "$dest/share/orbit-torrent.desktop" "$dest/share/orbit-links.desktop" \
          "$dest/share/nautilus-download-with-orbit"
    rmdir "$dest/share" 2>/dev/null || true
    rmdir "$dest" 2>/dev/null || true
fi
echo "Orbit was uninstalled."
