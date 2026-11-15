#!/bin/sh
# Builds Orbit-<version>-x86_64.AppImage from a Release build, with linuxdeploy and its Qt
# plugin bundling Qt, libtorrent and the rest.
#
#   installer/appimage/build-appimage.sh
#
# Needs linuxdeploy-x86_64.AppImage and linuxdeploy-plugin-qt-x86_64.AppImage (from
# github.com/linuxdeploy) on PATH or in installer/appimage/tools. This script downloads
# nothing itself.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
version=$(sed -n 's/^#define AppVersion "\([^"]*\)".*/\1/p' "$root/installer/Orbit.iss" | head -n 1)
build=$root/build-appimage
tools=$root/installer/appimage/tools
PATH=$tools:$PATH
for tool in linuxdeploy-x86_64.AppImage linuxdeploy-plugin-qt-x86_64.AppImage; do
    command -v "$tool" >/dev/null 2>&1 || { echo "error: $tool not found (see the top of this script)" >&2; exit 1; }
done

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$build"
rm -rf "$build/AppDir"
DESTDIR="$build/AppDir" cmake --install "$build"
install -Dm644 "$root/src/ui/assets/logo.png" "$build/AppDir/usr/share/icons/hicolor/256x256/apps/orbit.png"
cp "$root/installer/flatpak/io.github.neoberent.Orbit.desktop" "$build/orbit.desktop"

cd "$build"
export QML_SOURCES_PATHS="$root/src/ui/qml"
# Native Wayland needs Qt's Wayland platform plugin next to the xcb one.
export EXTRA_QT_MODULES="svg"
export EXTRA_PLATFORM_PLUGINS="libqwayland.so"
export VERSION="$version"
linuxdeploy-x86_64.AppImage --appdir AppDir --desktop-file orbit.desktop --icon-file AppDir/usr/share/icons/hicolor/256x256/apps/orbit.png \
    --plugin qt --output appimage
out=$(ls -t Orbit*.AppImage | head -n 1)
mkdir -p "$root/installer/Output"
mv "$out" "$root/installer/Output/Orbit-$version-x86_64.AppImage"
echo "Built installer/Output/Orbit-$version-x86_64.AppImage"
sha256sum "$root/installer/Output/Orbit-$version-x86_64.AppImage"
