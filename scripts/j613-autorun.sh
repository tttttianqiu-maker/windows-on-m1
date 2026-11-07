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
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
mkdir -p runs

notify() {
    osascript -e "display notification \"$1\" with title \"J613\"" >/dev/null 2>&1
    say "$1" >/dev/null 2>&1 &
}

newest_build() { ls -t "$HOME"/Downloads/j613-*.tar.gz 2>/dev/null | head -n 1; }

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
        notify "J613 run finished"
    fi
    sleep 5
done
