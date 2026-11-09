#!/usr/bin/env bash
# Builds .deb packages of the layer from the latest commit of this checkout.
#
#   packaging/debian/build-deb.sh            # 64-bit (amd64) package
#   packaging/debian/build-deb.sh --i386     # 64-bit and 32-bit (i386) packages
#
# The .deb files end up in packaging/debian/out/. Like the Arch PKGBUILD, only
# committed changes are built, and nothing is downloaded.
#
# Needs: sudo apt install build-essential debhelper cmake libvulkan-dev git
# --i386 also needs: sudo apt install crossbuild-essential-i386
set -euo pipefail

want_i386=0
for arg in "$@"; do
  case "$arg" in
    --i386) want_i386=1 ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(git -C "$here" rev-parse --show-toplevel)"
out="$here/out"
version="0~r$(git -C "$repo" rev-list --count HEAD).$(git -C "$repo" rev-parse --short HEAD)"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
src="$work/vram-fallback-layer-$version"
mkdir -p "$src"
git -C "$repo" archive HEAD | tar -x -C "$src"
# Packaging files come from the commit too, so out/ always matches a commit.
cp -r "$src/packaging/debian/debian" "$src/debian"

{
  echo "vram-fallback-layer ($version) unstable; urgency=medium"
  echo
  echo "  * Built from commit $(git -C "$repo" rev-parse HEAD)."
  echo
  echo " -- $(sed -n 's/^Maintainer: //p' "$src/debian/control")  $(date -R)"
} > "$src/debian/changelog"

build() {
  (cd "$src" && dpkg-buildpackage -b -us -uc "$@")
  (cd "$src" && dh_clean)
}

build
if [ "$want_i386" = 1 ]; then
  # Cross build. Only the (architecture-independent) Vulkan headers from the
  # amd64 libvulkan-dev are used, so skip the check for libvulkan-dev:i386.
  build -a i386 -d
fi

mkdir -p "$out"
mv "$work"/*.deb "$out"/
rm -f "$work"/*.buildinfo "$work"/*.changes
echo
echo "Built:"
ls -1 "$out"/*"$version"*.deb
echo
echo "Install with:  sudo apt install $(ls "$out"/*"$version"*.deb | tr '\n' ' ')"
