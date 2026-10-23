#!/bin/bash
# Arch-based distros: rebuild labwc with the Alt+Tab switcher close-button patch and install it.
# Rerun after a labwc repo update replaces the patched package (the pacman hook reminds you).
set -euo pipefail
here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cp "$here/PKGBUILD" "$here/switcher-close-button.patch" "$build/"
cd "$build"

sudo pacman -S --needed --noconfirm base-devel meson scdoc wayland-protocols xorg-xwayland git

# Follow the repo's labwc version; pkgrel x.9 sorts above the repo's own x / x.1 builds
repo_ver=$(LC_ALL=C pacman -Si labwc | awk '/^Version/{print $3; exit}')
ver=${repo_ver%-*}; rel=${repo_ver##*-}; rel=${rel%%.*}
sed -i -e "s/^pkgver=.*/pkgver=$ver/" -e "s/^pkgrel=.*/pkgrel=$rel.9/" PKGBUILD

makepkg -f --noconfirm
sudo pacman -U --noconfirm ./labwc-"$ver"-"$rel".9-*.pkg.tar.zst

sudo install -Dm644 "$here/labwc-patched.hook" /etc/pacman.d/hooks/labwc-patched.hook
echo "Done. Log out and back in (restart labwc) to get the close button on Alt+Tab thumbnails."
