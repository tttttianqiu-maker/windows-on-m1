#!/bin/sh
# j613 input: where the built-in keyboard/trackpad got to in the last run.
# Reads hv.log (or the file given) and prints one verdict per stage, from the
# m1n1 MTP bring-up to Windows draining reports (m1n1 ae2af33+ telemetry,
# "dchid: avhd ..." lines). Read-only.
L=${1:-hv.log}
[ -f "$L" ] || { echo "no $L (run something first)"; exit 1; }

has() { grep -a -q -E "$1" "$L"; }
first() { grep -a -E "$1" "$L" | head -n "${2:-1}"; }
last() { grep -a -E "$1" "$L" | tail -n "${2:-1}"; }
ok() { printf '  [ok]   %s\n' "$*"; }
bad() { printf '  [FAIL] %s\n' "$*"; }
info() { printf '  [..]   %s\n' "$*"; }
FAILED=0
fail() { bad "$*"; FAILED=1; }

echo "== $L ($(wc -l < "$L" | tr -d ' ') lines)"
if has "SError|L2C_ERR_ADR|Unhandled exception"; then
    fail "SError in this run:"
    first "SError|L2C_ERR_ADR|FAR|Unhandled exception" 6 | sed 's/^/         /'
    has "trackpad firmware staged" &&
        echo "         trackpad firmware was on: try 'mv firmware/tpmtfw-j613.bin firmware/tpmtfw-j613.bin.off' + 'j613 again'"
fi

echo "-- m1n1 side"
if has "dchid: MTP bring-up disabled"; then
    fail "MTP bring-up disabled (j613 mtp on)"
elif has "dchid: MTP not running"; then
    fail "MTP coprocessor not running (no keyboard/trackpad possible)"
fi
has "dchid: AVHD device at" && ok "AVHD page mapped" || { has "cannot map the AVHD" && fail "AVHD page not mapped"; }
has "oversized report-size items fixed" || info "no descriptor dump (m1n1 older than ae2af33?)"
if has "bring-up done .*keyboard ready"; then ok "keyboard up in m1n1"; else fail "keyboard not ready in m1n1: $(last 'dchid: bring-up' | sed 's/^.*dchid: //')"; fi
L1=$(last "dchid: keyboard layout code" | sed 's/^.*dchid: //'); [ -n "$L1" ] && info "$L1"
N=$(grep -a -c "dchid: keyboard report #" "$L"); [ "$N" -gt 0 ] && info "$N keyboard reports logged by m1n1 (keys reached the hypervisor)"
if has "trackpad firmware staged"; then
    if has "trackpad [0-9]+ x [0-9]+"; then ok "trackpad up: $(last 'dchid: trackpad [0-9]+ x' | sed 's/^.*dchid: //')"
    else fail "trackpad did not come up:"; first "trackpad (not ready|start failed|not a valid|dimensions unavailable)|cannot DMA-map|power method" 4 | sed 's/^/         /'; fi
else
    info "trackpad firmware off (firmware/tpmtfw-j613.bin absent)"
fi
first "dchid: trackpad report .* (trimmed|unusable|dropped)" 2 | sed 's/^.*dchid: /  [..]   /'

if has "battery: Windows battery ON"; then
    echo "-- battery (experimental, j613 battery on)"
    if has "battery: #[0-9]+ [0-9]+% (AC|battery) "; then
        ok "m1n1 reads the SMC: $(last 'battery: #' | sed 's/^.*battery: //')"
    elif has "battery: #"; then
        fail "SMC answered but no battery: $(last 'battery: #' | sed 's/^.*battery: //')"
    else
        fail "no battery reading from the SMC: $(last 'battery: (SMC unavailable|read kept back)' | sed 's/^.*battery: //')"
    fi
    K=$(grep -a -c "battery: read kept back" "$L")
    [ "$K" -gt 0 ] && info "$K SMC reads kept back (Windows kept the last good values)"
    if has "battery: Windows read the battery flags"; then ok "Windows found the battery (BAT0 _STA)"
    else fail "Windows never looked at the battery (Mu FD older than build 44, or Windows did not boot)"; fi
    if has "battery: Windows read _BST"; then ok "Windows reads the charge level (_BST)"
    else info "Windows never read the charge level (_BST) yet"; fi
    R=$(last "battery: Windows reads after" | sed 's/^.*battery: //'); [ -n "$R" ] && info "$R"
else
    B1=$(last "battery: Windows battery" | sed 's/^.*battery: //'); [ -n "$B1" ] && info "battery: $B1"
fi

echo "-- Windows driver (AppleMtpInput)"
if ! has "avhd guest read MAGIC"; then
    fail "the Windows driver never touched the device (not installed, not loading, or Windows did not get this far)"
    echo "         -> Windows-side problem: one macOS boot with scripts/j613-windows-diag.sh (see docs/J613-TONIGHT.md)"
else
    ok "driver started (PrepareHardware read MAGIC)"
    has "avhd guest read STATUS" && ok "driver polls STATUS" || fail "no STATUS read: D0 entry / transport start failed"
    has "read keyboard DESC_LEN" && ok "keyboard descriptor loaded by the driver" || fail "driver never loaded the keyboard descriptor (STATUS keyboard bit not set?)"
    has "avhd guest enabled GSIV" && ok "interrupt GSIV 65 enabled" || info "GSIV 65 never enabled (driver falls back to 100/500 ms polling)"
    has "avhd guest IRQ ack" && ok "transport running (IRQ ack)" || info "no IRQ ack yet"
    if has "popped its first report"; then ok "driver drains reports (input reaches Windows)"
    else info "no report drained yet (no key pressed while Windows ran, or the driver stalls)"; fi
    if has "trackpad firmware staged"; then
        has "read DIMS" && ok "driver loading the trackpad (read DIMS)" || info "driver did not read trackpad DIMS yet"
    fi
    S=$(last "dchid: avhd stats" | sed 's/^.*dchid: avhd stats //'); [ -n "$S" ] && info "last stats: $S"
fi

echo "--"
if [ "$FAILED" = 0 ]; then
    echo "m1n1 + driver path OK. If keys or the cursor still do nothing in Windows, it is the"
    echo "Windows HID side: tell Claude, and paste this output."
else
    echo "First [FAIL] above is where it broke. Paste this output to Claude."
fi
