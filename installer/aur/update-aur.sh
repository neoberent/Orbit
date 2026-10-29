#!/bin/sh
# Brings the AUR packages up to the release in the website's release.js (version, release
# tags and the extension version), refreshes their checksums from the GitHub downloads and
# regenerates .SRCINFO. Run it after the release files are on GitHub.
#
#   installer/aur/update-aur.sh [--push]
#
# --push also commits both packages to their AUR git repositories and pushes them. That
# needs an AUR account with your SSH key (https://aur.archlinux.org/account).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
release=$(cd "$here/../../.." && pwd)/Orbitwebsite/src/config/release.js
[ -f "$release" ] || { echo "error: $release was not found" >&2; exit 1; }

js_value() {
    sed -n "s/^const $1 = '\([^']*\)'.*/\1/p" "$release" | head -n 1
}
app_version=$(js_value version)
app_tag=$(js_value installerTag)
ext_version=$(js_value extensionVersion)
ext_tag=$(js_value extensionTag)
for v in app_version app_tag ext_version ext_tag; do
    eval "[ -n \"\$$v\" ]" || { echo "error: couldn't read $v from release.js" >&2; exit 1; }
done

# A new version starts again at pkgrel 1; the same version keeps its pkgrel (bump it by
# hand when only the packaging changed).
set_version() {
    pkgbuild=$1/PKGBUILD
    old=$(sed -n 's/^pkgver=//p' "$pkgbuild")
    sed -i "s/^pkgver=.*/pkgver=$2/; s/^_tag=.*/_tag=$3/" "$pkgbuild"
    if [ "$old" != "$2" ]; then sed -i 's/^pkgrel=.*/pkgrel=1/' "$pkgbuild"; fi
    (cd "$1" && updpkgsums && makepkg --printsrcinfo > .SRCINFO)
    echo "$(basename "$1") $2-$(sed -n 's/^pkgrel=//p' "$pkgbuild")"
}

command -v updpkgsums >/dev/null 2>&1 || { echo "error: updpkgsums is missing (sudo pacman -S pacman-contrib)" >&2; exit 1; }
set_version "$here/orbit-download-manager-bin" "$app_version" "$app_tag"
set_version "$here/orbit-download-manager-extension" "$ext_version" "$ext_tag"
# updpkgsums leaves the downloads beside the PKGBUILD.
rm -f "$here"/orbit-download-manager-bin/*.tar.gz "$here"/orbit-download-manager-extension/*.zip

[ "${1:-}" = "--push" ] || exit 0

checkout=$here/.aur
mkdir -p "$checkout"
for pkg in orbit-download-manager-bin orbit-download-manager-extension; do
    repo=$checkout/$pkg
    if [ ! -d "$repo/.git" ]; then
        git clone "ssh://aur@aur.archlinux.org/$pkg.git" "$repo"
    else
        git -C "$repo" pull --ff-only
    fi
    find "$here/$pkg" -maxdepth 1 -type f -exec cp {} "$repo/" \;
    git -C "$repo" add -A
    if git -C "$repo" diff --cached --quiet; then
        echo "$pkg: nothing changed"
        continue
    fi
    ver=$(sed -n 's/^\tpkgver = //p' "$repo/.SRCINFO")-$(sed -n 's/^\tpkgrel = //p' "$repo/.SRCINFO")
    git -C "$repo" commit -m "Update to $ver"
    git -C "$repo" push origin HEAD:master
done
