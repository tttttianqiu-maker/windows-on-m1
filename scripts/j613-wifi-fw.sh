#!/bin/bash
# Run ON THE AIR, IN macOS, once: packs this Mac's Wi-Fi firmware for m1n1's Wi-Fi driver.
# The files are Apple's (part of macOS) and cannot ship with this repo, so they come
# from your own Mac, like Asahi Linux's installer does. Read-only on the Mac.
#   curl -fsSL https://raw.githubusercontent.com/tttttianqiu-maker/windows-on-m1/j613-bringup/scripts/j613-wifi-fw.sh -o /tmp/wifi-fw.sh && bash /tmp/wifi-fw.sh
# Then get the .tar.gz from your Desktop onto the Hackintosh (AirDrop, USB stick, iCloud
# Drive, ...) into ~/Downloads and run `j613 wififw` there.
set -euo pipefail
SRC=/usr/share/firmware/wifi
[ "$(uname -m)" = arm64 ] || { echo "STOP: run this on the Air (Apple Silicon) in macOS"; exit 1; }
[ -d "$SRC" ] || { echo "STOP: $SRC missing"; exit 1; }
WORK=$(mktemp -d)
OUT="$HOME/Desktop/j613-wifi-fw-$(date +%Y%m%d-%H%M%S).tar.gz"
{
    echo "== model / macOS"; sysctl -n hw.model; sw_vers
    echo "== Wi-Fi card"; system_profiler SPAirPortDataType 2>/dev/null | sed -n 1,30p
    echo "== AppleBCMWLAN ioreg (identifiers only)"
    ioreg -l -w0 -r -c AppleBCMWLANBusInterfacePCIe 2>/dev/null |
        grep -E '"(module-instance|ModuleInfo|Chip|chip|Firmware|firmware|OTP|antenna|vendor-id|device-id|revision-id|subsystem)' | head -60 || true
    ioreg -l -w0 -r -c AppleBCMWLANCore 2>/dev/null |
        grep -E '"(FirmwareVersion|Firmware|ModuleInfo|Chip|RomVersion|CountryCode|OTP)' | head -40 || true
    echo "== firmware folders"; ls -la "$SRC" | head -80
} > "$WORK/j613-wifi-info.txt" 2>&1
( cd "$SRC" && ls -d C-4388* >/dev/null 2>&1 ) || echo "note: no C-4388* folder, packing every C-43* folder" >> "$WORK/j613-wifi-info.txt"
cd "$SRC"
DIRS=$(ls -d C-4388* 2>/dev/null || ls -d C-43*)
tar -czf "$OUT" -C "$SRC" $DIRS -C "$WORK" j613-wifi-info.txt
rm -rf "$WORK"
echo "packed $(du -h "$OUT" | cut -f1) -> $OUT"
echo "now: copy it to the Hackintosh's ~/Downloads and run 'j613 wififw' there"
