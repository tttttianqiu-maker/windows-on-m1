#!/bin/bash
# One-command J613 assisted run on the macOS debug host.
#
#   scripts/j613-run.sh [build.tar.gz] [--minutes N]
#
# 1. installs the newest ~/Downloads/j613-*.tar.gz (or the given one) after
#    checking its SHA256SUMS,
# 2. stops any previous runner,
# 3. waits for the Air's two USB serial devices (cold-boot the Air into the
#    "Windows 11" entry while it waits),
# 4. starts the virtual display and the assisted run in the background,
# 5. watches hv.log until the run exits, stalls, loops on faults, or N minutes
#    pass (default 10),
# 6. writes runs/<time>/summary.txt + screen.png and copies the summary to the
#    clipboard. The guest keeps running; the next invocation stops it.
#
# Touches only this Mac. Nothing here writes to the Air's disks.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
MINUTES=10
TARBALL=""
while [ $# -gt 0 ]; do
    case "$1" in
        --minutes) MINUTES=$2; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) TARBALL=$1; shift ;;
    esac
done

say() { printf '\033[1;36m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m!! %s\033[0m\n' "$*"; }
count() { local c; c=$(grep -a -c -E "$1" hv.log 2>/dev/null); echo "${c:-0}"; }
PY="$ROOT/proxyenv/bin/python"
[ -x "$PY" ] || PY=python3

# --- 1. build -----------------------------------------------------------------
if [ -z "$TARBALL" ]; then
    TARBALL=$(ls -t "$HOME"/Downloads/j613-*.tar.gz 2>/dev/null | head -n 1)
fi
if [ -n "$TARBALL" ] && [ -f "$TARBALL" ]; then
    SUM=$(shasum -a 256 "$TARBALL" | cut -c1-64)
    if [ "$(cat runs/.installed-tarball 2>/dev/null)" != "$SUM" ]; then
        say "installing $(basename "$TARBALL")"
        TMP=$(mktemp -d)
        tar -xzf "$TARBALL" -C "$TMP" || { warn "cannot unpack $TARBALL"; exit 1; }
        (cd "$TMP/debug-uart" && shasum -a 256 -c SHA256SUMS >/dev/null) ||
            { warn "checksum mismatch in $TARBALL"; exit 1; }
        mkdir -p dist/j613 runs
        rm -rf dist/j613/debug-uart && mv "$TMP/debug-uart" dist/j613/ && rmdir "$TMP"
        echo "$SUM" > runs/.installed-tarball
    else
        say "build $(basename "$TARBALL") already installed"
    fi
fi
[ -f dist/j613/debug-uart/m1n1.macho ] || { warn "no build in dist/j613/debug-uart"; exit 1; }
git pull -q --ff-only 2>/dev/null && git submodule update -q 2>/dev/null || warn "git pull skipped"
BUILD=$("$PY" -c 'import json;m=json.load(open("dist/j613/debug-uart/MANIFEST.json"));print("m1n1",m["m1n1_windows_commit"][:7],"mu",m["mu_commit"][:7],"root",m.get("root_commit","")[:7])' 2>/dev/null)
say "build: $BUILD"

# --- 2. stop old runs -----------------------------------------------------------
pkill -9 -f run_uefi.py 2>/dev/null; pkill -9 -f uart-reader.py 2>/dev/null
pkill -9 -f "run-assisted.sh" 2>/dev/null
sleep 1

# --- 3. wait for the Air ------------------------------------------------------
say "waiting for the Air: cold-boot it now (hold power 10 s, unplug all, cable in the"
say "usb-drd0 port, stick in the other, hold power -> Windows 11 -> 'Running proxy...')"
DEVS=""
for _ in $(seq 1 300); do
    DEVS=$(ls ${J613_TEST_DEVGLOB:-/dev/cu.usbmodem*} 2>/dev/null | sort)
    [ "$(echo "$DEVS" | grep -c usbmodem)" -ge 2 ] && break
    sleep 1
done
[ "$(echo "$DEVS" | grep -c usbmodem)" -ge 2 ] || { warn "Air not found after 5 minutes"; exit 1; }
PROXY=$(echo "$DEVS" | sed -n 1p); VUART=$(echo "$DEVS" | sed -n 2p)
say "Air found: proxy=$PROXY vuart=$VUART"
sleep 2
# The Air may still be running (or hung in) the previous guest: its USB serial
# devices exist but no proxy answers. Then wait for a cold boot.
if [ -z "${J613_TEST_DEVGLOB:-}" ] && ! "$PY" tools/proxy-ping.py "$PROXY" >/dev/null 2>&1; then
    warn "the Air is still running the previous build: cold-boot it now"
    osascript -e 'display notification "Cold boot the MacBook Air into Windows 11 now" with title "J613"' >/dev/null 2>&1
    command -v say >/dev/null && /usr/bin/say "Cold boot the MacBook Air into Windows 11 now" >/dev/null 2>&1 &
    OK=""
    for _ in $(seq 1 100); do
        sleep 3
        DEVS=$(ls /dev/cu.usbmodem* 2>/dev/null | sort)
        [ "$(echo "$DEVS" | grep -c usbmodem)" -ge 2 ] || continue
        PROXY=$(echo "$DEVS" | sed -n 1p); VUART=$(echo "$DEVS" | sed -n 2p)
        if "$PY" tools/proxy-ping.py "$PROXY" >/dev/null 2>&1; then OK=1; break; fi
    done
    [ -n "$OK" ] || { warn "no m1n1 proxy answered within 5 minutes"; exit 1; }
    say "proxy answering: proxy=$PROXY vuart=$VUART"
    sleep 1
fi

# --- 4. start -------------------------------------------------------------------
RUN="runs/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN"
rm -f hv.log guest-uart.log fb.raw fb-info.json
if ! curl -s -o /dev/null -m 2 http://127.0.0.1:8766/; then
    nohup scripts/display-assisted.sh >"$RUN/display.log" 2>&1 &
    sleep 1
fi
say "starting run (viewer: http://127.0.0.1:8766/), log folder $RUN"
BOARD=j613 nohup scripts/run-assisted.sh --proxy "$PROXY" --vuart "$VUART" \
    --display virtual --debug uart --chainload --foreground >"$RUN/console.log" 2>&1 &
RUNNER=$!

# --- 5. watch -------------------------------------------------------------------
STATUS="time limit reached (still running)"
LAST=0; STILL=0; T=0
while [ $T -lt $((MINUTES * 60)) ]; do
    sleep "${J613_TEST_TICK:-10}"; T=$((T + 10))
    if grep -q "came back as m1n1\|chainload failed" "$RUN/console.log" 2>/dev/null; then
        STATUS="chainload failed: the new m1n1 reset the Air (photo of the Air screen helps)"; break
    fi
    if ! kill -0 $RUNNER 2>/dev/null; then STATUS="runner exited (Air crashed, rebooted or disconnected)"; break; fi
    N=$(count "")
    F=$(count "UNHANDLED|Unmapped IPA")
    if [ "$F" -gt 50 ]; then
        STATUS="guest stuck in a fault loop ($F unhandled accesses); runner stopped"
        pkill -9 -f run_uefi.py; break
    fi
    if [ "$N" -eq "$LAST" ]; then STILL=$((STILL + 10)); else STILL=0; LAST=$N; fi
    if [ $STILL -ge 180 ] && [ "$N" -gt 0 ]; then
        STATUS="hv.log idle for 3 minutes at $N lines (hang, or waiting for input; run left alive)"; break
    fi
    printf '\r   %3ds  hv.log %7s lines  faults %s   ' "$T" "$N" "$F"
done
echo

# --- 6. summarise ---------------------------------------------------------------
"$PY" tools/fb2png.py fb.raw fb-info.json "$RUN/screen.png" 2>/dev/null && SHOT="$RUN/screen.png" || SHOT="(no framebuffer yet)"
head -c 2000000 hv.log > "$RUN/hv-head.log" 2>/dev/null
tail -n 3000 hv.log > "$RUN/hv-tail.log" 2>/dev/null
cp guest-uart.log "$RUN/" 2>/dev/null
{
    echo "J613 run $(basename "$RUN")  |  $BUILD"
    echo "status: $STATUS"
    echo "hv.log lines: $(count "")   screen: $SHOT"
    echo "--- milestones"
    grep -a -E "chainload: |came back as|CPU: |counter redirect|usbdiag 8|J613:|ExitBootServices|XhcClearBiosOwnership|turning on CPU|secondary [0-9]|Entering guest secondary" \
        "$RUN/console.log" hv.log 2>/dev/null | sed 's/^[^:]*://' | head -40
    echo "--- faults (first 12, total $(count "UNHANDLED|Unmapped IPA|Exception|panicked|BugCheck"))"
    grep -a -E "UNHANDLED|Unmapped IPA|Exception|panicked|BugCheck|ESR|FAR|elr=" hv.log 2>/dev/null | head -12
    echo "--- last 40 lines of hv.log"
    tail -n 40 hv.log 2>/dev/null | cut -c1-220
    echo "--- last 10 lines of runner console"
    tail -n 10 "$RUN/console.log" | cut -c1-220
} > "$RUN/summary.txt"
cat "$RUN/summary.txt"
pbcopy < "$RUN/summary.txt" 2>/dev/null && say "summary copied to the clipboard: paste it into the chat"
say "files: $RUN (summary.txt, screen.png, hv-tail.log)"
[ -z "${J613_NO_OPEN:-}" ] && [ -f "$RUN/screen.png" ] && open "$RUN/screen.png" 2>/dev/null
exit 0
