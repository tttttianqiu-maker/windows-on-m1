#!/bin/bash
# Plan B, step 2 (run on the MacBook Air in macOS, install stick plugged in).
#   scripts/j613-planb-winsrc.sh            -> shows the plan, changes nothing
#   scripts/j613-planb-winsrc.sh --apply    -> creates WINSRC and copies the files
# Creates ONE 10 GB exFAT partition "WINSRC" at the start of the largest free gap on the
# internal disk (the space left for Windows). Existing partitions are not resized,
# moved, formatted or deleted.
set -euo pipefail
say() { printf '\033[1;36m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31mSTOP: %s\033[0m\n' "$*" >&2; exit 1; }
DISK=disk0
[ "$(uname -m)" = arm64 ] || die "run this on the MacBook Air"
[ -f /Volumes/WINDATA/sources/install.wim ] || die "plug in the install stick (need /Volumes/WINDATA/sources/install.wim)"
[ -f /Volumes/WINBOOT/j613/j613-install.cmd ] || die "stick not prepared (run j613-planb-prep.sh on the Hackintosh first)"
if diskutil info WINSRC >/dev/null 2>&1; then say "WINSRC already exists"; else

diskutil list $DISK
# Find the partition right before the largest free gap (GPT sector math via gpt-free list).
PLAN=$(diskutil list -plist $DISK | plutil -convert json -o - - | /usr/bin/python3 -c '
import json,sys,subprocess,plistlib
d=json.load(sys.stdin)
parts=d["AllDisksAndPartitions"][0]["Partitions"]
info=plistlib.loads(subprocess.check_output(["diskutil","info","-plist","disk0"]))
bs=info.get("DeviceBlockSize",4096); total=info["TotalSize"]
spans=[]
for p in parts:
    pi=plistlib.loads(subprocess.check_output(["diskutil","info","-plist",p["DeviceIdentifier"]]))
    off=pi.get("PartitionMapPartitionOffset"); size=pi["TotalSize"]
    spans.append((off,off+size,p["DeviceIdentifier"]))
spans.sort()
best=(0,None,None)
for i,(s,e,dev) in enumerate(spans):
    nxt=spans[i+1][0] if i+1<len(spans) else total-5*1024*1024
    gap=nxt-e
    if gap>best[0]: best=(gap,dev,e)
gap,dev,start=best
print(dev, gap//10**9)
')
PREV=${PLAN% *}; GAPGB=${PLAN#* }
say "largest free gap: ${GAPGB} GB, right after $PREV"
[ "$GAPGB" -ge 120 ] || die "largest free gap is only ${GAPGB} GB; expected the ~200 GB left for Windows"
say "plan: new 10 GB exFAT partition WINSRC right after $PREV; $((GAPGB-10)) GB stay free for Windows"
[ "${1:-}" = --apply ] || { echo "(nothing changed; run again with --apply)"; exit 0; }
diskutil addPartition "$PREV" ExFAT WINSRC 10G
fi
say "copying install.wim (~5 GB) and scripts to WINSRC"
mkdir -p /Volumes/WINSRC/sources
cp /Volumes/WINDATA/sources/install.wim /Volumes/WINSRC/sources/
cp /Volumes/WINBOOT/j613/j613-install.cmd /Volumes/WINBOOT/j613/oobe-unattend.xml /Volumes/WINSRC/
sync; diskutil list $DISK
say "DONE. Shut the Air down. Next: cold boot -> Windows 11 -> Running proxy, start the watcher."
