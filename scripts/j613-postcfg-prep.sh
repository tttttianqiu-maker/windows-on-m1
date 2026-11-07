#!/bin/bash
# Run on the MacBook Air in macOS, install stick plugged in.
# 1. puts the newest j613-install.cmd on WINSRC
# 2. renames Windows' boot files on WINESP (FAT32) so the firmware boots the stick's
#    WinPE once; WinPE writes the settings and renames them back.
set -euo pipefail
die() { printf '\033[1;31mSTOP: %s\033[0m\n' "$*" >&2; exit 1; }
say() { printf '\033[1;36m==> %s\033[0m\n' "$*"; }
[ -d /Volumes/WINSRC ] || die "WINSRC not mounted"
[ -f /Volumes/WINBOOT/sources/boot.wim ] || die "plug in the install stick"
curl -fsSL https://raw.githubusercontent.com/tttttianqiu-maker/windows-on-m1/576465406dbbb9ab12908694edd2bd721b154b2e/tools/j613/autoinstall/j613-install.cmd -o /Volumes/WINSRC/j613-install.cmd
grep -q ":postcfg" /Volumes/WINSRC/j613-install.cmd || die "download failed"
ESPDEV=$(diskutil list disk0 | awk '/ WINESP /{print $NF}')
[ -n "$ESPDEV" ] || die "no WINESP partition found"
diskutil mount "$ESPDEV" >/dev/null
MNT=$(diskutil info "$ESPDEV" | awk -F': *' '/Mount Point/{print $2}')
[ -f "$MNT/EFI/Microsoft/Boot/BCD" ] || die "$MNT is not the Windows ESP"
for f in EFI/Microsoft/Boot/bootmgfw.efi EFI/BOOT/BOOTAA64.EFI; do
    [ -f "$MNT/$f" ] && mv "$MNT/$f" "$MNT/$f.j613off" && say "hid $f"
done
ls "$MNT/EFI/Microsoft/Boot/" | grep -i bootmgfw; ls "$MNT/EFI/BOOT/"
diskutil unmount "$ESPDEV" >/dev/null
say "ready: shut down, stick in, boot Windows 11 (watcher). WinPE writes the settings, restores the boot files and powers off."
