#!/bin/bash
# Set up an Intel macOS build host for the J613 (M3 MacBook Air 13") port and
# do the first BOARD=j613 debug-uart build of m1n1 + Project Mu.
#
#   bash mac-host-setup.sh            # setup + clone + build
#   bash mac-host-setup.sh --no-build # setup + clone only
#
# No Homebrew: it no longer supports Intel Macs. The Mac only needs git (Xcode
# Command Line Tools) and Docker Desktop. m1n1 AND Mu both build inside the
# Linux container from Dockerfile.build.
#
# Touches only this Mac: Docker Desktop and ~/src/windows-on-m1.
# It never talks to the Air, its disks, or any boot settings. Safe to re-run.
set -euo pipefail

FORK=https://github.com/tttttianqiu-maker
BRANCH=j613-bringup
SRC="$HOME/src"
REPO="$SRC/windows-on-m1"
IMAGE=windows-on-m1-build:local
BUILD=1
[ "${1:-}" = --no-build ] && BUILD=0

say()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die()  { printf '\n\033[1;31mSTOP: %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = Darwin ] || die "run this on the Mac"
say "host: macOS $(sw_vers -productVersion) $(uname -m)"

# 1. Xcode Command Line Tools (git)
if ! xcode-select -p >/dev/null 2>&1; then
    xcode-select --install || true
    die "a Command Line Tools installer window opened. Finish it, then re-run this script."
fi
say "git: $(git --version)"

# 2. Docker Desktop, installed straight from docker.com (no Homebrew)
if [ "$(sysctl -n kern.hv_support 2>/dev/null || echo 0)" != 1 ]; then
    die "this Mac reports no hardware virtualization (kern.hv_support=0). Docker Desktop
needs it: enable VT-x/VMX in the BIOS and make sure OpenCore isn't hiding it, then re-run."
fi
if [ ! -d /Applications/Docker.app ]; then
    say "downloading Docker Desktop for Intel (about 500 MB)"
    DMG="$HOME/Downloads/Docker-amd64.dmg"
    curl -fL --progress-bar https://desktop.docker.com/mac/main/amd64/Docker.dmg -o "$DMG"
    MNT=$(mktemp -d)
    hdiutil attach -nobrowse -quiet -mountpoint "$MNT" "$DMG"
    cp -R "$MNT/Docker.app" /Applications/ || { hdiutil detach -quiet "$MNT"; \
        die "couldn't copy Docker.app into /Applications. Open $DMG and drag it in yourself, then re-run."; }
    hdiutil detach -quiet "$MNT"
fi
DOCKER=/Applications/Docker.app/Contents/Resources/bin/docker
command -v docker >/dev/null 2>&1 && DOCKER=$(command -v docker)
if ! "$DOCKER" info >/dev/null 2>&1; then
    say "starting Docker Desktop (accept its terms window; skipping sign-in is fine)"
    open -a Docker
    for _ in $(seq 1 80); do "$DOCKER" info >/dev/null 2>&1 && break; sleep 3; done
    "$DOCKER" info >/dev/null 2>&1 || die "Docker isn't running yet. Finish Docker Desktop's first-run
setup, wait for 'Engine running', then re-run."
fi
say "docker: $("$DOCKER" version --format '{{.Server.Version}}')"

# 3. Clone the forks (submodules are pinned to their j613-bringup heads)
mkdir -p "$SRC"
if [ -d "$REPO/.git" ]; then
    say "updating $REPO"
    git -C "$REPO" fetch origin "$BRANCH"
    git -C "$REPO" checkout "$BRANCH"
    git -C "$REPO" pull --ff-only origin "$BRANCH"
    git -C "$REPO" submodule sync --recursive
else
    say "cloning (Mu pulls big submodules, give it a few minutes)"
    git clone -b "$BRANCH" "$FORK/windows-on-m1.git" "$REPO"
fi
git -C "$REPO" submodule update --init --recursive
say "windows-on-m1 $(git -C "$REPO" rev-parse --short HEAD) | m1n1 $(git -C "$REPO/m1n1_windows" rev-parse --short HEAD) | mu $(git -C "$REPO/mu" rev-parse --short HEAD)"

[ "$BUILD" = 1 ] || { say "setup done (no build requested)"; exit 0; }

# 4. First J613 build, entirely inside the Linux build container
LOG="$SRC/j613-build-$(date +%Y%m%d-%H%M%S).log"
say "building the container image (first time: ~10 min)"
"$DOCKER" build -t "$IMAGE" -f "$REPO/Dockerfile.build" "$REPO" 2>&1 | tee "$LOG"
[ "${PIPESTATUS[0]}" = 0 ] || die "container image build failed. Send back: $LOG"

say "building BOARD=j613 --debug uart, log: $LOG"
set +e
"$DOCKER" run --rm \
    -e BOARD=j613 \
    -e STANDALONE_IN_CONTAINER=1 \
    -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
    -v "$REPO:/work" -w /work \
    "$IMAGE" scripts/build-development.sh --debug uart 2>&1 | tee -a "$LOG"
STATUS=${PIPESTATUS[0]}
set -e

OUT="$REPO/dist/j613/debug-uart"
if [ "$STATUS" = 0 ] && [ -f "$OUT/m1n1.macho" ] && [ -f "$OUT/J613_EFI.fd" ]; then
    say "BUILD OK"
    ls -l "$OUT"
else
    printf '\n\033[1;33mBuild failed (exit %s). Expected on the first try. Send this file back:\n  %s\033[0m\n' "$STATUS" "$LOG"
    exit "$STATUS"
fi
