#!/bin/bash
# Plan B, step 1 (on the macOS debug host, with the WINBOOT/WINDATA stick plugged in):
#   - builds wimlib (once) into ~/src/wimlib-build
#   - patches the stick's WINBOOT/sources/boot.wim (index 2, the Setup image) so that
#     WinPE runs j613-launch.cmd instead of Setup (backup kept as boot.wim.orig)
#   - copies j613-install.cmd + oobe-unattend.xml to WINBOOT/j613/ for step 2 on the Air
# Touches only this Mac and the USB stick. Never touches the Air's disk.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
AI="$ROOT/tools/j613/autoinstall"
say() { printf '\033[1;36m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31mSTOP: %s\033[0m\n' "$*" >&2; exit 1; }

BOOT=/Volumes/WINBOOT; DATA=/Volumes/WINDATA
[ -f "$BOOT/sources/boot.wim" ] || die "plug the install stick into this Mac (need $BOOT/sources/boot.wim)"
[ -f "$DATA/sources/install.wim" ] || [ -f "$BOOT/sources/install.wim" ] || die "no install.wim on WINDATA or WINBOOT"

# 1. wimlib
WL="$HOME/src/wimlib-build"
if [ ! -x "$WL/bin/wimlib-imagex" ]; then
    say "building wimlib 1.14.4 (one time, ~3 min)"
    mkdir -p "$HOME/src" && cd "$HOME/src"
    curl -fL -o wimlib-1.14.4.tar.gz https://wimlib.net/downloads/wimlib-1.14.4.tar.gz
    tar -xzf wimlib-1.14.4.tar.gz && cd wimlib-1.14.4
    ./configure --prefix="$WL" --without-ntfs-3g --without-fuse >/dev/null
    make -j4 >/dev/null && make install >/dev/null
fi
WIM="$WL/bin/wimlib-imagex"
"$WIM" --version | head -1

# 2. editions in install.wim (the installer applies "Windows 11 Pro")
IW="$DATA/sources/install.wim"; [ -f "$IW" ] || IW="$BOOT/sources/install.wim"
say "editions in $(basename "$IW"):"
"$WIM" info "$IW" | grep -E '^(Index|Name)' | paste - - | sed 's/  */ /g'
"$WIM" info "$IW" | grep -q '^Name: *Windows 11 Pro$' || die "no 'Windows 11 Pro' edition in install.wim (send me the list above)"

# 3. patch boot.wim index 2
[ -f "$BOOT/sources/boot.wim.orig" ] || { say "backing up boot.wim -> boot.wim.orig"; cp "$BOOT/sources/boot.wim" "$BOOT/sources/boot.wim.orig"; }
TMP=$(mktemp -d)
cp "$BOOT/sources/boot.wim.orig" "$TMP/boot.wim"
say "patching boot.wim index 2 (winpeshl.ini + j613-launch.cmd)"
"$WIM" update "$TMP/boot.wim" 2 <<CMDS
add '$AI/winpeshl.ini' /Windows/System32/winpeshl.ini
add '$AI/j613-launch.cmd' /j613-launch.cmd
CMDS
"$WIM" dir "$TMP/boot.wim" 2 | grep -iE 'winpeshl.ini|j613-launch.cmd'
cp "$TMP/boot.wim" "$BOOT/sources/boot.wim"
rm -rf "$TMP"

# 4. payload for the WINSRC partition
mkdir -p "$BOOT/j613"
cp "$AI/j613-install.cmd" "$AI/oobe-unattend.xml" "$BOOT/j613/"
sync
say "DONE. Stick is ready. Eject it, plug it into the Air, boot the Air into macOS (step 2)."
