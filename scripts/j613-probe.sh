#!/bin/sh
# Read-only hardware probe for the J613 (MacBook Air 13", M3) Windows bring-up.
#
# Run it on the M3 Air itself, in macOS, as your normal user (no sudo):
#   sh scripts/j613-probe.sh
#
# It only READS: sysctl, ioreg (Apple's device tree as exposed by macOS),
# diskutil list and csrutil status. It writes nothing outside the output
# folder it creates in the current directory. Send back the .tar.gz.
set -eu

OUT="j613-probe-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
echo "writing to $OUT/"

run() {  # run <file> <command...>  (never aborts the probe)
    f="$1"; shift
    { echo "\$ $*"; "$@" 2>&1 || echo "(exit $?)"; } >"$OUT/$f"
}

# Identity: confirm this really is a J613 / T8122.
run identity.txt sh -c 'sw_vers; sysctl hw.model hw.memsize hw.ncpu \
    hw.perflevel0.physicalcpu hw.perflevel1.physicalcpu machdep.cpu.brand_string; \
    ioreg -p IODeviceTree -d 1 -l | grep -E "\"(compatible|model|target-type)\""'

# Boot / security state and disks (needed before any Asahi or Windows install).
run security.txt csrutil status
run disks.txt diskutil list

# The single most important value: where DRAM starts on M3.
run chosen.txt ioreg -p IODeviceTree -n chosen -l

# Every device we touch: name, reg (addresses), interrupts, compatible.
run arm-io.txt ioreg -p IODeviceTree -n arm-io -r -l -w 0

# Full device tree, for anything we did not think of.
run devicetree-full.txt ioreg -p IODeviceTree -l -w 0

# Short summary of the devices the port depends on.
{
    echo "# grep summary (full data in arm-io.txt)"
    for n in uart0 usb-drd0 usb-drd1 atc-phy0 atc-phy1 dart-usb0 dart-usb1 \
             ans sart-ans aic mtp dockchannel-mtp mtp-dockchannel disp0 dcp gfx-asc; do
        echo "== $n"
        grep -n -E "\+-o $n[ @<]" "$OUT/arm-io.txt" | head -3 || true
    done
} >"$OUT/summary.txt" 2>&1

tar -czf "$OUT.tar.gz" "$OUT"
echo
echo "done: $OUT.tar.gz"
echo "dram-base line (the key value):"
grep -E '"dram-base"|"dram-size"' "$OUT/chosen.txt" || echo "  not found, send the tarball anyway"
