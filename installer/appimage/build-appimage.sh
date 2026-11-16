#!/bin/sh
# Builds Orbit-<version>-x86_64.AppImage from a Release build, with linuxdeploy and its Qt
# plugin bundling Qt, libtorrent, curl and the rest. CI (.github/workflows/appimage.yml)
# runs it on an old base system, so the result runs on most distributions; built on a
# rolling one it only runs where that glibc is.
#
#   installer/appimage/build-appimage.sh
#
# Needs linuxdeploy-x86_64.AppImage and linuxdeploy-plugin-qt-x86_64.AppImage (from
# github.com/linuxdeploy) on PATH or in installer/appimage/tools; this script downloads
# nothing itself. QMAKE picks the Qt to bundle, CMAKE_PREFIX_PATH where Qt and the other
# libraries are found.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
version=$(sed -n 's/^#define AppVersion "\([^"]*\)".*/\1/p' "$root/installer/Orbit.iss" | head -n 1)
build=$root/build-appimage
tools=$root/installer/appimage/tools
PATH=$tools:$PATH
for tool in linuxdeploy-x86_64.AppImage linuxdeploy-plugin-qt-x86_64.AppImage; do
    command -v "$tool" >/dev/null 2>&1 || { echo "error: $tool not found (see the top of this script)" >&2; exit 1; }
done
# AppImages normally mount themselves with FUSE, which containers and CI runners lack.
export APPIMAGE_EXTRACT_AND_RUN=1
: "${QMAKE:=$(command -v qmake6 || command -v qmake)}"
export QMAKE

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -Wno-dev
cmake --build "$build"
rm -rf "$build/AppDir"
DESTDIR="$build/AppDir" cmake --install "$build"
install -Dm644 "$root/src/ui/assets/logo.png" "$build/AppDir/usr/share/icons/hicolor/256x256/apps/orbit.png"
cp "$root/installer/flatpak/io.github.neoberent.Orbit.desktop" "$build/orbit.desktop"

# Native Wayland needs Qt's Wayland platform plugins next to the xcb one; their names
# differ between Qt versions, so take whichever this Qt has.
plugins=$("$QMAKE" -query QT_INSTALL_PLUGINS)
wayland=
for p in libqwayland-generic.so libqwayland-egl.so libqwayland.so; do
    if [ -f "$plugins/platforms/$p" ]; then wayland="${wayland:+$wayland;}$p"; fi
done
[ -n "$wayland" ] || echo "warning: this Qt has no Wayland platform plugin; the AppImage will use XWayland" >&2
# --headless runs on Qt's offscreen platform (no display at all), which nothing links to either.
[ -f "$plugins/platforms/libqoffscreen.so" ] || { echo "error: this Qt has no offscreen platform plugin (needed by --headless)" >&2; exit 1; }

cd "$build"
export QML_SOURCES_PATHS="$root/src/ui/qml"
export EXTRA_QT_MODULES="svg"
export EXTRA_PLATFORM_PLUGINS="libqoffscreen.so${wayland:+;$wayland}"
export VERSION="$version"
rm -f Orbit*.AppImage
linuxdeploy-x86_64.AppImage --appdir AppDir --desktop-file orbit.desktop \
    --icon-file AppDir/usr/share/icons/hicolor/256x256/apps/orbit.png --plugin qt
# A Wayland window also needs the shell (xdg-shell) and decoration plugins, which nothing
# links to directly; the second pass bundles their libraries and fixes their paths.
for dir in wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client; do
    if [ -d "$plugins/$dir" ]; then mkdir -p AppDir/usr/plugins && cp -r "$plugins/$dir" AppDir/usr/plugins/; fi
done
linuxdeploy-x86_64.AppImage --appdir AppDir --output appimage
out=$(ls -t Orbit*.AppImage | head -n 1)
mkdir -p "$root/installer/Output"
mv "$out" "$root/installer/Output/Orbit-$version-x86_64.AppImage"
echo "Built installer/Output/Orbit-$version-x86_64.AppImage"
sha256sum "$root/installer/Output/Orbit-$version-x86_64.AppImage"
