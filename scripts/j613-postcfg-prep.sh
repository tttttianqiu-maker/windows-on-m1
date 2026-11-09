#!/bin/bash
# Run on the MacBook Air in macOS, install stick plugged in.
# 1. puts the pinned j613-install.cmd, j613-jobs.cmd and oobe-unattend.xml on WINSRC
# 1b. puts the CI-built AppleMtpInput driver (J613 keyboard/trackpad) in
#     WINSRC/j613-drivers; WinPE injects it and turns on test signing
#     (skipped with --no-driver)
# 2. renames Windows' boot files on WINESP (FAT32) so the firmware boots the stick's
#    WinPE once; WinPE writes the settings and renames them back.
set -euo pipefail
# --reinstall: one-shot flag; WinPE wipes ONLY the "Windows" partition and
#              reinstalls Windows 11 Pro, then applies everything below
# --fix-setup: one-shot flag; WinPE sets Setup\Status\ChildCompletion
#              setup.exe=3 so a crashed first-boot Setup can continue
# --no-driver: do not stage the keyboard/trackpad driver (no test signing);
#              keeps a reinstall to a single variable
REINSTALL=0; FIXSETUP=0; DRIVER=1
for a in "$@"; do
    case "$a" in
        --reinstall) REINSTALL=1 ;;
        --fix-setup) FIXSETUP=1 ;;
        --no-driver) DRIVER=0 ;;
        *) echo "unknown option $a" >&2; exit 2 ;;
    esac
done
die() { printf '\033[1;31mSTOP: %s\033[0m\n' "$*" >&2; exit 1; }
say() { printf '\033[1;36m==> %s\033[0m\n' "$*"; }
[ -d /Volumes/WINSRC ] || die "WINSRC not mounted"
[ -f /Volumes/WINBOOT/sources/boot.wim ] || die "plug in the install stick"
# result of the previous WinPE run, if any
for L in /Volumes/WINSRC/j613-postcfg.log /Volumes/WINSRC/j613-install.log; do
    if [ -f "$L" ]; then
        say "previous WinPE run ($L):"
        tr -d '\r' < "$L" | grep -E "STEP|WARNING|STOPPED|DONE|failed|refusing|missing|testsigning|Published Name|AppleMtp|successfully|chkdsk" | tail -25
        break
    fi
done
# WinPE scripts and the first-boot answer file, all from one reviewed commit
PIN=73a0cab12633fcbc5d378113f817287a33d6e99b
RAW=https://raw.githubusercontent.com/tttttianqiu-maker/windows-on-m1/$PIN/tools/j613/autoinstall
for f in j613-install.cmd j613-jobs.cmd oobe-unattend.xml; do
    curl -fsSL "$RAW/$f" -o "/Volumes/WINSRC/$f" || die "download of $f failed"
done
mkdir -p /Volumes/WINSRC/j613-jobs/drivers
grep -q "GroupPolicy" /Volumes/WINSRC/j613-install.cmd && grep -q pnputil /Volumes/WINSRC/j613-jobs.cmd || die "download failed"
grep -q "STEP P2" /Volumes/WINSRC/j613-install.cmd && grep -q "STEP P1b" /Volumes/WINSRC/j613-install.cmd || die "install script too old"
grep -q "oobe-unattend.xml missing" /Volumes/WINSRC/j613-install.cmd || die "install script too old (no answer-file check)"
grep -q "<LocalAccount " /Volumes/WINSRC/oobe-unattend.xml || die "oobe-unattend.xml download failed"
say "WinPE scripts and first-boot answer file staged (commit ${PIN:0:7})"
rm -rf /Volumes/WINSRC/j613-drivers
if [ "$DRIVER" = 1 ]; then
    # CI run 42415908537 (root f913402), artifact AppleMtpInput-J613-ARM64-Debug
    TMPD=$(mktemp -d)
    curl -fsSL -o "$TMPD/drv.zip" "https://nightly.link/tttttianqiu-maker/windows-on-m1/actions/runs/42415908537/AppleMtpInput-J613-ARM64-Debug.zip"
    unzip -q -o "$TMPD/drv.zip" -d "$TMPD/x"
    SYS=$(find "$TMPD/x" -name AppleMtpInput.sys | head -1)
    [ -n "$SYS" ] || die "driver download failed"
    echo "83bc606c5b6de3d832c00c1ecdaa58d856c1c04346a28a58c2dbbe1c7fbf7666  $SYS" | shasum -a 256 -c - >/dev/null || die "driver checksum mismatch"
    mkdir -p /Volumes/WINSRC/j613-drivers
    cp "$(dirname "$SYS")"/AppleMtpInput.inf "$(dirname "$SYS")"/AppleMtpInput.sys "$(dirname "$SYS")"/*.cat /Volumes/WINSRC/j613-drivers/
    DIAG=$(find "$TMPD/x" -name AppleInputDiag.exe | head -1); [ -n "$DIAG" ] && cp "$DIAG" /Volumes/WINSRC/
    ls -la /Volumes/WINSRC/j613-drivers; rm -rf "$TMPD"
    say "driver staged (AppleMtpInput, test-signed)"
else
    say "no driver this time (--no-driver): no driver injection, no test signing"
fi
rm -f /Volumes/WINSRC/j613-reinstall.flag /Volumes/WINSRC/j613-fixsetup.flag
if [ "$FIXSETUP" = 1 ]; then
    echo "owner-requested setup resume $(date)" > /Volumes/WINSRC/j613-fixsetup.flag
    say "SETUP FIX armed: WinPE sets ChildCompletion setup.exe=3 (~3 min)"
fi
if [ "$REINSTALL" = 1 ]; then
    echo "owner-requested reinstall $(date)" > /Volumes/WINSRC/j613-reinstall.flag
    say "REINSTALL armed: the next WinPE run wipes the Windows partition and reinstalls (~20 min)"
fi
ESPDEV=$(diskutil list disk0 | awk '/ WINESP /{print $NF}')
[ -n "$ESPDEV" ] || die "no WINESP partition found"
sudo diskutil mount "$ESPDEV" >/dev/null
MNT=$(diskutil info "$ESPDEV" | awk -F': *' '/Mount Point/{print $2}')
[ -f "$MNT/EFI/Microsoft/Boot/BCD" ] || die "$MNT is not the Windows ESP"
for f in EFI/Microsoft/Boot/bootmgfw.efi EFI/BOOT/BOOTAA64.EFI; do
    [ -f "$MNT/$f" ] && sudo mv -f "$MNT/$f" "$MNT/$f.j613off" && say "hid $f"
done
ls "$MNT/EFI/Microsoft/Boot/" | grep -i bootmgfw; ls "$MNT/EFI/BOOT/"
sudo diskutil unmount "$ESPDEV" >/dev/null
[ "$REINSTALL" = 1 ] && say "reinstall: leave it alone ~20-25 min, it powers off by itself"
say "ready: shut down, stick in, boot Windows 11 (watcher). WinPE writes the settings, turns off automatic repair, restores the boot files and powers off."
