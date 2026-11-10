#!/bin/bash
# Run ON THE AIR, IN macOS, once: stages the Windows driver for the built-in Wi-Fi.
# m1n1 (build 52+, `j613 wifi "<network>"`) gives Windows a virtio-net adapter
# (PCI\VEN_1AF4&DEV_1041) that carries the Air's own Wi-Fi; Windows needs Red Hat's
# NetKVM driver for it (virtio-win, NetKVM\w11\ARM64 - the same one the Asahi docs use for
# Windows 11 VMs). This script
#   1. downloads the virtio-win ISO (stable; --latest for the newest) unless one is given,
#   2. copies NetKVM\w11\ARM64 to WINSRC:\j613-netkvm\,
#   3. stages the one-shot job j613-netkvm.cmd in the j613-jobs mailbox: at the next
#      Windows boot it installs the driver (trusting the catalog signer only if Windows
#      says it is a valid Red Hat signature) and sets up a boot-time network snapshot.
#   curl -fsSL https://raw.githubusercontent.com/tttttianqiu-maker/windows-on-m1/j613-bringup/scripts/j613-netkvm.sh -o /tmp/netkvm.sh && bash /tmp/netkvm.sh
# Results after a Windows boot: bash /tmp/netkvm.sh --read
set -euo pipefail
LATEST=0; READ_ONLY=0; ISO=
for a in "$@"; do
    case "$a" in
        --latest) LATEST=1 ;;
        --read) READ_ONLY=1 ;;
        *.iso) ISO=$a ;;
        *) echo "usage: $0 [--latest] [--read] [virtio-win.iso]" >&2; exit 2 ;;
    esac
done
die() { printf '\033[1;31mSTOP: %s\033[0m\n' "$*" >&2; exit 1; }
say() { printf '\033[1;36m==> %s\033[0m\n' "$*"; }
W=/Volumes/WINSRC
J=$W/j613-jobs
[ -d "$W" ] || die "WINSRC not mounted (it is the exFAT partition on the Air's SSD)"

for f in "$J/netkvm.txt" "$J/net-snapshot.txt"; do
    [ -f "$f" ] || continue
    printf '\n\033[1;36m================ %s (%s)\033[0m\n' "$(basename "$f")" "$(date -r "$f" '+%Y-%m-%d %H:%M')"
    tr -d '\r' < "$f" | head -n 300
done
[ "$READ_ONLY" = 1 ] && exit 0

# 1. the ISO
if [ -z "$ISO" ]; then
    CH=stable-virtio; [ "$LATEST" = 1 ] && CH=latest-virtio
    ISO="$HOME/Downloads/virtio-win-$CH.iso"
    if [ ! -s "$ISO" ]; then
        say "downloading virtio-win ($CH, ~700 MB) to $ISO"
        curl -fL --progress-bar -o "$ISO.part" \
            "https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/$CH/virtio-win.iso" ||
            die "download failed"
        mv "$ISO.part" "$ISO"
    fi
fi
[ -s "$ISO" ] || die "no ISO at $ISO"

# 2. NetKVM for Windows 11 ARM64
MNT=$(mktemp -d)
hdiutil attach -readonly -nobrowse -mountpoint "$MNT" "$ISO" >/dev/null || die "cannot mount $ISO"
trap 'hdiutil detach "$MNT" >/dev/null 2>&1 || true' EXIT
SRC=$MNT/NetKVM/w11/ARM64
[ -f "$SRC/netkvm.inf" ] || die "this ISO has no NetKVM/w11/ARM64 (try: bash $0 --latest)"
VER=$(grep -i -m1 '^DriverVer' "$SRC/netkvm.inf" | tr -d '\r' || true)
rm -rf "$W/j613-netkvm"
mkdir -p "$W/j613-netkvm"
cp "$SRC"/* "$W/j613-netkvm/"
ls "$W"/j613-netkvm/*.cat >/dev/null 2>&1 || die "no catalog (.cat) next to netkvm.inf"
say "NetKVM ARM64 staged on WINSRC ($VER)"

# 3. the one-shot job and its helper, from one reviewed commit (CRLF for cmd.exe)
PIN=ed417c5f8cb7fdc9c1ebf7c5c0783ba658c68da6
RAW=https://raw.githubusercontent.com/tttttianqiu-maker/windows-on-m1/$PIN/tools/j613/autoinstall
mkdir -p "$J/lib"
fetch() { # $1 file, $2 destination
    curl -fsSL "$RAW/$1" -o "$2.tmp" || die "download of $1 failed"
    perl -pi -e 's/\r?\n/\r\n/' "$2.tmp"
    mv "$2.tmp" "$2"
}
fetch j613-net-snapshot.cmd "$J/lib/j613-net-snapshot.cmd"
grep -q "J613 net snapshot" "$J/lib/j613-net-snapshot.cmd" || die "snapshot script download failed"
fetch j613-netkvm.cmd "$J/j613-netkvm.cmd"
grep -q "J613 NetKVM" "$J/j613-netkvm.cmd" || die "job download failed"
rm -f "$J/netkvm.txt"
say "staged: the next Windows boot installs NetKVM once (j613-jobs/netkvm.txt has the result)"
say "now: shut down, then on the Hackintosh 'j613 wifi \"<network>\"' (once) and 'j613 again'"
