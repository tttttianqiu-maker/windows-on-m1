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
    if has "battery: Windows connected the battery event interrupt"; then ok "instant updates: Windows connected the battery event interrupt (GED)"
    else info "no battery event interrupt from Windows (GED not started; Windows polls instead)"; fi
    R=$(last "battery: Windows reads after" | sed 's/^.*battery: //'); [ -n "$R" ] && info "$R"
else
    B1=$(last "battery: Windows battery" | sed 's/^.*battery: //'); [ -n "$B1" ] && info "battery: $B1"
fi

K1=$(last "kbdlight: " | sed 's/^.*kbdlight: //'); [ -n "$K1" ] && info "keyboard backlight: $K1"
if has "wlan: (stage 2 probe|probe \\()"; then
    echo "-- built-in Wi-Fi"
    if has "wlan: link (up after|already up)"; then ok "PCIe link to the Wi-Fi chip is up"
    else fail "no PCIe link to the Wi-Fi chip (lines below say where it stopped)"; fi
    has "wlan: probe OK" && ok "Broadcom chip attached (cores, RAM, OTP)"
    if has "wlan: firmware started after"; then ok "Wi-Fi firmware is running"
    elif has "wlan: (firmware did not start|firmware does not fit|firmware set incomplete|bad shared RAM)"; then fail "Wi-Fi firmware did not start"; fi
    if has "wlan: msgbuf up"; then ok "message rings set up (DMA through the pcie0 DART)"
    elif has "wlan: (ring setup FAILED|ring info looks wrong|DMA pool|DART map failed|pcie0 DART init failed)"; then fail "message ring setup failed"; fi
    has "wlan: DART fault" && fail "the chip hit a DART fault (DMA outside the mapped pool)"
    if has "wlan: MAC [0-9a-f]"; then ok "control channel works (first ioctl answered)"
    elif has "wlan: no answer to the first ioctl"; then fail "no answer to the first ioctl: the control rings do not work"; fi
    if has "wlan: interface up: OK"; then ok "Wi-Fi interface up (CLM, calibration, events)"
    elif has "wlan: (interface up: failed|dongle init FAILED)"; then fail "dongle init failed"; fi
    if has "wlan: joining "; then
        if has "wlan: CONNECTED to"; then ok "$(last "wlan: CONNECTED to" | sed 's/^.*wlan: //')"
        elif has "was not found"; then fail "network not found (name, range, 2.4/5 GHz WPA2?)"
        elif has "handshake timeout"; then fail "joined but the WPA2 handshake failed: password wrong?"
        else fail "join failed: $(last "wlan: join failed" | sed 's/^.*wlan: //')"; fi
        has "wlan: link DOWN" && info "link went down later: $(last "wlan: link DOWN" | sed 's/^.*wlan: //')"
        S=$(last "wlan: status: " | sed 's/^.*wlan: //'); [ -n "$S" ] && info "last $S"
    fi
    if has "vnet: virtio-net 00:01.0"; then
        echo "-- Wi-Fi adapter in Windows (virtio-net 00:01.0, NetKVM)"
        has "vnet: BAR0 at" && ok "Windows/UEFI enumerated the adapter" || info "adapter not enumerated yet (Windows did not get to PCI, or no BAR)"
        if has "vnet: guest driver up"; then ok "NetKVM driver running in Windows"
        else fail "no Windows driver on the adapter: scripts/j613-netkvm.sh in macOS on the Air, then boot Windows once"; fi
        has "vnet: device error" && fail "$(last "vnet: device error" | sed 's/^.*vnet: //')"
        V=$(last "vnet: guest (driver up|no driver), " | sed 's/^.*vnet: //')
        if [ -n "$V" ]; then
            info "$V"
            echo "$V" | grep -q -E "tx [1-9]" && ok "Windows sends through the Wi-Fi"
            echo "$V" | grep -q -E "rx [1-9]" && ok "Windows receives from the Wi-Fi"
        fi
    fi
    grep -a "wlan: " "$L" | grep -a -v -E "wlan: (fw: |status: )" | sed 's/^.*wlan: /  [..]   /' | head -60
    N=$(grep -a -c "wlan: fw: " "$L")
    if [ "$N" -gt 0 ]; then
        info "firmware console ($N lines, first 15):"
        grep -a "wlan: fw: " "$L" | sed 's/^.*wlan: fw: /           /' | head -15
    fi
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
