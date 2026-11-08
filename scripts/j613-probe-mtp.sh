#!/bin/sh
# Read-only MTP power probe. Air must sit at "Running proxy..." (watcher stopped).
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT" || exit 1
PY="$ROOT/proxyenv/bin/python"; [ -x "$PY" ] || PY=python3
DEV=${M1N1DEVICE:-$(ls /dev/cu.usbmodem* 2>/dev/null | sort | sed -n 1p)}
[ -n "$DEV" ] || { echo "Air not found: boot it to 'Running proxy...' first"; exit 1; }
mkdir -p runs; OUT=runs/probe-mtp-$(date +%Y%m%d-%H%M%S).txt
echo "probing via $DEV -> $OUT"
M1N1DEVICE=$DEV "$PY" tools/j613/probe-mtp-power.py 2>&1 | tee "$OUT"
