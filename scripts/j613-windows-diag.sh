#!/bin/bash
# Run on the MacBook Air in macOS when Windows-side input fails, i.e. `j613 input`
# on the Hackintosh says "the Windows driver never touched the device", or the
# m1n1 + driver path is OK and Windows still ignores keys/the trackpad.
# One macOS visit does both halves:
# 1. prints what Windows recorded on WINSRC last time: diag-input.txt (this
#    job's previous result), input-snapshot.txt (boot task, 2 min after every
#    boot), diag-appl0613.txt (older one-shot job) and the mailbox log
# 2. removes pending one-shot jobs that shut Windows down (diag-appl0613.cmd)
# 3. stages j613-input-diag.cmd for the next Windows boot: state before ->
#    repair (trust the package's test certificate, pnputil /add-driver /install,
#    rescan) -> state after; it also installs the J613InputSnapshot boot task.
#    It never shuts Windows down. --no-repair: diagnose only.
#    --read: only step 1.
# Then shut down, `j613 again` on the Hackintosh, boot the Air into proxy mode as
# usual. The job runs before the sign-in screen, so a successful repair already
# gives a working keyboard in that boot. Run this script again on the next macOS
# visit to see the results (or paste them to Claude).
set -euo pipefail
READ_ONLY=0; REPAIR=1
for a in "$@"; do
    case "$a" in
        --read) READ_ONLY=1 ;;
        --no-repair) REPAIR=0 ;;
        *) echo "unknown option $a" >&2; exit 2 ;;
    esac
done
die() { printf '\033[1;31mSTOP: %s\033[0m\n' "$*" >&2; exit 1; }
say() { printf '\033[1;36m==> %s\033[0m\n' "$*"; }
W=/Volumes/WINSRC
J=$W/j613-jobs
[ -d "$W" ] || die "WINSRC not mounted"

shown=0
for f in "$J/diag-input.txt" "$J/input-snapshot.txt" "$J/diag-appl0613.txt"; do
    [ -f "$f" ] || continue
    shown=1
    printf '\n\033[1;36m================ %s (%s)\033[0m\n' "$(basename "$f")" "$(date -r "$f" '+%Y-%m-%d %H:%M')"
    tr -d '\r' < "$f" | head -n 600
done
if [ -f "$J/jobs.log" ]; then
    printf '\n\033[1;36m================ jobs.log (last 30 lines)\033[0m\n'
    tr -d '\r' < "$J/jobs.log" | tail -n 30
    shown=1
fi
[ "$shown" = 1 ] || say "no Windows-side results on WINSRC yet"
[ "$READ_ONLY" = 1 ] && exit 0

# The job files, from one reviewed commit (CRLF for cmd.exe)
PIN=9a4ff8b8100c5099bd4b4a370d12d4b688cbd943
RAW=https://raw.githubusercontent.com/tttttianqiu-maker/windows-on-m1/$PIN/tools/j613/autoinstall
mkdir -p "$J/lib"
fetch() { # $1 file, $2 destination
    curl -fsSL "$RAW/$1" -o "$2.tmp" || die "download of $1 failed"
    perl -pi -e 's/\r?\n/\r\n/' "$2.tmp"
    mv "$2.tmp" "$2"
}
fetch j613-input-state.cmd "$J/lib/j613-input-state.cmd"
fetch j613-input-snapshot.cmd "$J/lib/j613-input-snapshot.cmd"
grep -q "J613 input snapshot" "$J/lib/j613-input-snapshot.cmd" || die "snapshot script download failed"
if [ -f "$J/diag-appl0613.cmd" ]; then
    rm -f "$J/diag-appl0613.cmd"
    say "removed the pending diag-appl0613.cmd (it would shut Windows down a minute after boot)"
fi
if [ "$REPAIR" = 1 ]; then rm -f "$J/no-repair.flag"; else echo "owner: diagnose only" > "$J/no-repair.flag"; fi

# Driver package for the repair: the same CI artifact and checksum the prep stages
if [ "$REPAIR" = 1 ] && { [ ! -f "$W/j613-drivers/AppleMtpInput.inf" ] || ! ls "$W"/j613-drivers/*.cat >/dev/null 2>&1; } ||
   [ ! -f "$W/AppleInputDiag.exe" ]; then
    TMPD=$(mktemp -d)
    curl -fsSL -o "$TMPD/drv.zip" "https://nightly.link/tttttianqiu-maker/windows-on-m1/actions/runs/42415908537/AppleMtpInput-J613-ARM64-Debug.zip" ||
        die "driver download failed"
    unzip -q -o "$TMPD/drv.zip" -d "$TMPD/x"
    PKG=$(dirname "$(find "$TMPD/x" -path '*AppleMtpInput/AppleMtpInput.sys' | head -1)")
    [ -f "$PKG/AppleMtpInput.inf" ] && ls "$PKG"/*.cat >/dev/null 2>&1 || die "driver package incomplete"
    echo "83bc606c5b6de3d832c00c1ecdaa58d856c1c04346a28a58c2dbbe1c7fbf7666  $PKG/AppleMtpInput.sys" | shasum -a 256 -c - >/dev/null ||
        die "driver checksum mismatch"
    mkdir -p "$W/j613-drivers"
    cp "$PKG/AppleMtpInput.inf" "$PKG/AppleMtpInput.sys" "$PKG"/*.cat "$W/j613-drivers/"
    DIAG=$(find "$TMPD/x" -name AppleInputDiag.exe | head -1)
    [ -n "$DIAG" ] && cp "$DIAG" "$W/"
    rm -rf "$TMPD"
    say "driver package + AppleInputDiag.exe staged on WINSRC"
fi

# The one-shot job last: the mailbox runs every *.cmd in j613-jobs at boot
fetch j613-input-diag.cmd "$J/j613-input-diag.cmd"
grep -q "J613 input diag" "$J/j613-input-diag.cmd" || die "job download failed"
rm -f "$J/diag-input.txt"
say "staged: the next Windows boot runs j613-input-diag.cmd once ($([ "$REPAIR" = 1 ] && echo "diagnose + repair" || echo "diagnose only"))"
say "now: shut down, 'j613 again' on the Hackintosh, Air into proxy mode as usual"
