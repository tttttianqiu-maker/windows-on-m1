#!/bin/bash
# Leave this running in a Terminal on the macOS debug host:
#
#   ~/src/windows-on-m1/scripts/j613-autorun.sh
#
# It starts scripts/j613-run.sh by itself whenever
#   - a new ~/Downloads/j613-*.tar.gz build appears, or
#   - the file runs/.go appears (a request for another run of the same build).
# When the Air has to be cold-booted it shows a notification and says so out
# loud. Results land in runs/<time>/ (summary.txt, screen.png) and runs/.last
# names the newest run folder. Stop it with Ctrl-C.
#
# Follow mode (default on): when a run ends because the guest rebooted the Air
# (Windows setup reboots several times), the next run starts by itself as soon as
# the Air is back at "Running proxy..." (up to J613_FOLLOW_MAX times in a row).
# J613_FOLLOW=0 turns it off.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
mkdir -p runs

notify() {
    osascript -e "display notification \"$1\" with title \"J613\"" >/dev/null 2>&1
    say "$1" >/dev/null 2>&1 &
}

newest_build() { ls -t "$HOME"/Downloads/j613-*.tar.gz 2>/dev/null | head -n 1; }

FOLLOW=${J613_FOLLOW:-1}; FOLLOW_MAX=${J613_FOLLOW_MAX:-8}; FOLLOWED=0
echo "J613 autorun: watching ~/Downloads/j613-*.tar.gz and runs/.go (Ctrl-C to stop)"
while true; do
    REASON=""
    B=$(newest_build)
    if [ -n "$B" ]; then
        SUM=$(shasum -a 256 "$B" | cut -c1-64)
        [ "$SUM" != "$(cat runs/.installed-tarball 2>/dev/null)" ] && REASON="new build $(basename "$B")"
    fi
    [ -z "$REASON" ] && [ -e runs/.go ] && REASON="run requested"
    if [ -n "$REASON" ]; then
        rm -f runs/.go
        echo; echo "=== $(date '+%H:%M:%S') $REASON"
        if [ "$(ls /dev/cu.usbmodem* 2>/dev/null | grep -c usbmodem)" -lt 2 ]; then
            notify "Cold boot the MacBook Air into Windows 11 now"
        fi
        J613_NO_OPEN=1 scripts/j613-run.sh ${B:+"$B"}
        ls -td runs/2*/ 2>/dev/null | head -n 1 | tr -d '\n' > runs/.last
        LASTRUN=$(cat runs/.last)
        # The watch window may end while the guest keeps running (idle/time limit):
        # keep waiting for the runner, then look at the live log for a guest reset.
        if [ "$FOLLOW" = 1 ] && pgrep -f run_uefi.py >/dev/null; then
            echo "=== guest still running; waiting for it to reboot or stop (Ctrl-C to quit)"
            while pgrep -f run_uefi.py >/dev/null; do
                sleep 5
                [ -e runs/.go ] && break
                B2=$(newest_build)
                [ -n "$B2" ] && [ "$(shasum -a 256 "$B2" | cut -c1-64)" != "$(cat runs/.installed-tarball 2>/dev/null)" ] && break
            done
            tail -n 3000 hv.log > "$LASTRUN/hv-tail.log" 2>/dev/null
        fi
        if [ "$FOLLOW" = 1 ] && ! pgrep -f run_uefi.py >/dev/null &&
           grep -q "PSCI RESET" "$LASTRUN/hv-tail.log" 2>/dev/null &&
           [ "$FOLLOWED" -lt "$FOLLOW_MAX" ]; then
            FOLLOWED=$((FOLLOWED + 1))
            echo "=== guest rebooted the Air: following (#$FOLLOWED), waiting for Running proxy..."
            notify "Windows rebooted the Air. If it does not come back to Running proxy by itself, boot it into Windows 11"
            touch runs/.go
        else
            FOLLOWED=0
            notify "J613 run finished"
        fi
    fi
    sleep 5
done
