#!/bin/sh
# Build a J613 dev package WITHOUT Docker (Intel Hackintosh):
#   - m1n1 is compiled natively (Homebrew llvm + lld + rustup),
#   - Mu is NOT rebuilt: the J613_EFI.fd of the currently installed build
#     (dist/j613/debug-uart) is reused. Only valid while mu/ is unchanged.
# usage: scripts/j613-build-m1n1.sh <name>   -> ~/Downloads/j613-<name>.tar.gz
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
die() { printf 'STOP: %s\n' "$*" >&2; exit 1; }
NAME=${1:-local-$(date +%H%M)}
DIST=dist/j613/debug-uart
[ -f "$DIST/J613_EFI.fd" ] && [ -f "$DIST/MANIFEST.json" ] || die "no installed build in $DIST (run one build via the watcher first)"

# Mu must be exactly the commit the reused firmware was built from.
FD_MU=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["mu_commit"])' "$DIST/MANIFEST.json")
MU_NOW=$(git -C mu rev-parse HEAD)
[ "$FD_MU" = "$MU_NOW" ] || die "mu/ is at $MU_NOW but the installed J613_EFI.fd is from $FD_MU: Mu needs a real (Docker/Linux) build"
[ -z "$(git -C mu status --porcelain)" ] || die "mu/ has local changes: Mu needs a real (Docker/Linux) build"

# Native toolchain (m1n1_windows/README.md, macOS section).
if [ "$(uname -s)" = Darwin ]; then
    for p in llvm lld; do
        PFX=$(brew --prefix "$p" 2>/dev/null) || die "brew install llvm lld rustup"
        PATH="$PFX/bin:$PATH"
    done
    export PATH
fi
command -v clang >/dev/null || die "clang missing"

FD_DIR=mu/Build/MacBookAirM3J613-AARCH64/DEBUG_CLANGPDB/FV
mkdir -p "$FD_DIR"
cp "$DIST/J613_EFI.fd" "$FD_DIR/MACBOOKAIRM3J613_EFI.fd"

STANDALONE_BUILD_CONTAINER=never STANDALONE_SKIP_MU=1 BOARD=j613 \
    sh scripts/build-development.sh --display virtual --debug uart

(cd "$DIST" && shasum -a 256 -c SHA256SUMS >/dev/null) || die "SHA256SUMS check failed"
OUT="$HOME/Downloads/j613-$NAME.tar.gz"
tar -czf "$OUT" -C dist/j613 debug-uart
python3 -c 'import json,sys;m=json.load(open(sys.argv[1]));print("built m1n1",m["m1n1_windows_commit"][:7],"mu",m["mu_commit"][:7],"root",m["root_commit"][:7],"dirty",m["source_dirty"])' "$DIST/MANIFEST.json"
shasum -a 256 "$OUT"
echo "-> $OUT (the watcher picks it up)"
