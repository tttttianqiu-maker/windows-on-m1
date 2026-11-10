#!/bin/sh
# Read-only Wi-Fi/PCIe hardware map (ADT + pmgr states). Air must sit at
# "Running proxy..." (watcher stopped). Report and full ADT land in runs/.
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT" || exit 1
PY="$ROOT/proxyenv/bin/python"; [ -x "$PY" ] || PY=python3
DEV=${M1N1DEVICE:-$(ls /dev/cu.usbmodem* 2>/dev/null | sort | sed -n 1p)}
[ -n "$DEV" ] || { echo "Air not found: boot it to 'Running proxy...' first"; exit 1; }
pgrep -f j613-autorun.sh >/dev/null && { echo "stop the watcher first (Ctrl-C in its Terminal)"; exit 1; }
mkdir -p runs; OUT=runs/probe-wifi-$(date +%Y%m%d-%H%M%S).txt
echo "probing via $DEV -> $OUT"
M1N1DEVICE=$DEV "$PY" tools/j613/probe-wifi.py 2>&1 | tee "$OUT"
echo
echo "Send Claude: $OUT and the runs/adt-*.bin it saved (attach both files)."
