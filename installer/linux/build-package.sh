#!/bin/sh
# Builds the Release app and the Linux release archive (OrbitSetup-<version>-linux.tar.gz),
# the counterpart of build-installer.ps1. Prints its size and SHA-256 for the website's
# release.js ("linux" entry), which Orbit's in-app updater on Linux installs from.
#
#   installer/linux/build-package.sh [--skip-build]
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
build=$root/build-linux
version=$(sed -n 's/^#define AppVersion "\([^"]*\)".*/\1/p' "$root/installer/Orbit.iss" | head -n 1)
[ -n "$version" ] || { echo "error: AppVersion was not found in Orbit.iss" >&2; exit 1; }

if [ "${1:-}" != "--skip-build" ]; then
    echo "Building Orbit $version..."
    cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -Wno-dev >/dev/null
    cmake --build "$build" --parallel
fi

name=orbit-$version-linux-$(uname -m)
stage=$build/package/$name
rm -rf "$build/package"
mkdir -p "$stage"
install -m 755 "$build/orbit" "$build/orbit-native-host" "$stage/"
strip --strip-unneeded "$stage/orbit" "$stage/orbit-native-host"
install -m 755 "$root/installer/linux/install.sh" "$root/installer/linux/uninstall.sh" "$stage/"
install -m 644 "$root/installer/License.txt" "$root/installer/PrivacyPolicy.txt" "$stage/"
install -m 644 "$root/src/ui/assets/logo.png" "$stage/orbit.png"
install -m 644 "$root/src/ui/assets/fonts/Inter-LICENSE.txt" "$stage/"

out=$root/installer/Output
mkdir -p "$out"
archive=$out/OrbitSetup-$version-linux.tar.gz
tar -C "$build/package" --owner=0 --group=0 -czf "$archive" "$name"

size=$(stat -c %s "$archive")
sha=$(sha256sum "$archive" | cut -d' ' -f1 | tr 'a-f' 'A-F')
echo
echo "Built $archive"
echo "  size:   $size bytes ($(awk "BEGIN {printf \"%.1f MB\", $size / 1048576}"))"
echo "  sha256: $sha"
echo "To install: tar -xzf $(basename "$archive") && ./$name/install.sh"
