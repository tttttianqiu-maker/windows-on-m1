#!/bin/bash
# Run on the MacBook Air in macOS, install stick plugged in.
# 1. puts the newest j613-install.cmd on WINSRC
# 1b. puts the CI-built AppleMtpInput driver (J613 keyboard/trackpad) in
#     WINSRC/j613-drivers; WinPE injects it and turns on test signing
# 2. renames Windows' boot files on WINESP (FAT32) so the firmware boots the stick's
#    WinPE once; WinPE writes the settings and renames them back.
set -euo pipefail
die() { printf '\033[1;31mSTOP: %s\033[0m\n' "$*" >&2; exit 1; }
say() { printf '\033[1;36m==> %s\033[0m\n' "$*"; }
[ -d /Volumes/WINSRC ] || die "WINSRC not mounted"
[ -f /Volumes/WINBOOT/sources/boot.wim ] || die "plug in the install stick"
# result of the previous WinPE run, if any
for L in /Volumes/WINSRC/j613-postcfg.log /Volumes/WINSRC/j613-install.log; do
    if [ -f "$L" ]; then
        say "previous WinPE run ($L):"
        tr -d '\r' < "$L" | grep -E "STEP|WARNING|STOPPED|DONE|failed|testsigning|Published Name|AppleMtp|successfully|chkdsk" | tail -25
        break
    fi
done
curl -fsSL https://raw.githubusercontent.com/tttttianqiu-maker/windows-on-m1/ee6abcfd0b89836e2f98b29affcc8e2b9afc336b/tools/j613/autoinstall/j613-install.cmd -o /Volumes/WINSRC/j613-install.cmd
curl -fsSL https://raw.githubusercontent.com/tttttianqiu-maker/windows-on-m1/ee6abcfd0b89836e2f98b29affcc8e2b9afc336b/tools/j613/autoinstall/j613-jobs.cmd -o /Volumes/WINSRC/j613-jobs.cmd
mkdir -p /Volumes/WINSRC/j613-jobs/drivers
grep -q "GroupPolicy" /Volumes/WINSRC/j613-install.cmd && grep -q pnputil /Volumes/WINSRC/j613-jobs.cmd || die "download failed"
grep -q "STEP P2" /Volumes/WINSRC/j613-install.cmd || die "install script without driver step"
# CI run 42415908537 (root f913402), artifact AppleMtpInput-J613-ARM64-Debug
TMPD=$(mktemp -d)
curl -fsSL -o "$TMPD/drv.zip" "https://nightly.link/tttttianqiu-maker/windows-on-m1/actions/runs/42415908537/AppleMtpInput-J613-ARM64-Debug.zip"
unzip -q -o "$TMPD/drv.zip" -d "$TMPD/x"
SYS=$(find "$TMPD/x" -name AppleMtpInput.sys | head -1)
[ -n "$SYS" ] || die "driver download failed"
echo "83bc606c5b6de3d832c00c1ecdaa58d856c1c04346a28a58c2dbbe1c7fbf7666  $SYS" | shasum -a 256 -c - >/dev/null || die "driver checksum mismatch"
rm -rf /Volumes/WINSRC/j613-drivers; mkdir -p /Volumes/WINSRC/j613-drivers
cp "$(dirname "$SYS")"/AppleMtpInput.inf "$(dirname "$SYS")"/AppleMtpInput.sys "$(dirname "$SYS")"/*.cat /Volumes/WINSRC/j613-drivers/
DIAG=$(find "$TMPD/x" -name AppleInputDiag.exe | head -1); [ -n "$DIAG" ] && cp "$DIAG" /Volumes/WINSRC/
ls -la /Volumes/WINSRC/j613-drivers; rm -rf "$TMPD"
say "driver staged (AppleMtpInput, test-signed)"
ESPDEV=$(diskutil list disk0 | awk '/ WINESP /{print $NF}')
[ -n "$ESPDEV" ] || die "no WINESP partition found"
sudo diskutil mount "$ESPDEV" >/dev/null
MNT=$(diskutil info "$ESPDEV" | awk -F': *' '/Mount Point/{print $2}')
[ -f "$MNT/EFI/Microsoft/Boot/BCD" ] || die "$MNT is not the Windows ESP"
for f in EFI/Microsoft/Boot/bootmgfw.efi EFI/BOOT/BOOTAA64.EFI; do
    [ -f "$MNT/$f" ] && sudo mv "$MNT/$f" "$MNT/$f.j613off" && say "hid $f"
done
ls "$MNT/EFI/Microsoft/Boot/" | grep -i bootmgfw; ls "$MNT/EFI/BOOT/"
sudo diskutil unmount "$ESPDEV" >/dev/null
say "ready: shut down, stick in, boot Windows 11 (watcher). WinPE writes the settings, installs the driver, turns on test signing, turns off automatic repair, restores the boot files and powers off."
