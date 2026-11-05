#!/bin/bash
# Set up an Intel macOS build host for the J613 (M3 MacBook Air 13") port and
# do the first BOARD=j613 debug-uart build of m1n1 + Project Mu.
#
#   bash mac-host-setup.sh            # setup + clone + build
#   bash mac-host-setup.sh --no-build # setup + clone only
#
# Touches only this Mac: Homebrew packages, Docker Desktop, and ~/src/windows-on-m1.
# It never talks to the Air, its disks, or any boot settings.
# Safe to re-run: every step skips what is already done.
set -euo pipefail

FORK=https://github.com/tttttianqiu-maker
BRANCH=j613-bringup
SRC="$HOME/src"
REPO="$SRC/windows-on-m1"
BUILD=1
[ "${1:-}" = --no-build ] && BUILD=0

say()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die()  { printf '\n\033[1;31mSTOP: %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = Darwin ] || die "run this on the Mac"
say "host: $(sw_vers -productVersion) $(uname -m)"

# 1. Xcode command line tools (Homebrew and git need them)
if ! xcode-select -p >/dev/null 2>&1; then
    xcode-select --install || true
    die "a Command Line Tools installer window opened. Finish it, then re-run this script."
fi

# 2. Homebrew (its installer asks for your Mac password itself)
if ! command -v brew >/dev/null 2>&1 && [ ! -x /usr/local/bin/brew ]; then
    say "installing Homebrew"
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
fi
eval "$(/usr/local/bin/brew shellenv 2>/dev/null || brew shellenv)"
if ! grep -q 'brew shellenv' "$HOME/.zprofile" 2>/dev/null; then
    echo 'eval "$(/usr/local/bin/brew shellenv)"' >> "$HOME/.zprofile"
fi

# 3. Toolchain
say "installing git, python 3.12, llvm, lld, rustup, dtc"
brew install git python@3.12 llvm lld rustup dtc
RUSTUP_BIN="$(brew --prefix rustup)/bin"
export PATH="$RUSTUP_BIN:$PATH"
rustup show active-toolchain >/dev/null 2>&1 || rustup default stable
rustup target add aarch64-unknown-none-softfloat
say "clang: $("$(brew --prefix llvm)/bin/clang" --version | head -1)"

# 4. Docker Desktop (the Mu firmware builds inside a Linux container)
if [ "$(sysctl -n kern.hv_support 2>/dev/null || echo 0)" != 1 ]; then
    die "this Mac reports no hardware virtualization (kern.hv_support=0). Docker Desktop
needs it: enable VT-x/VMX in the BIOS and make sure OpenCore isn't hiding it, then re-run."
fi
if [ ! -d /Applications/Docker.app ]; then
    say "installing Docker Desktop"
    brew install --cask docker-desktop
fi
if ! docker info >/dev/null 2>&1; then
    say "starting Docker Desktop (accept its terms window if one appears)"
    open -a Docker
    for _ in $(seq 1 60); do docker info >/dev/null 2>&1 && break; sleep 3; done
    docker info >/dev/null 2>&1 || die "Docker isn't running yet. Open Docker Desktop, finish its first-run
setup (accept terms, skip sign-in is fine), wait for 'Engine running', then re-run."
fi
say "docker: $(docker version --format '{{.Server.Version}}')"

# 5. Clone the forks (submodules are pinned to their j613-bringup heads)
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

# 6. First J613 build: Mu in Docker, m1n1 natively with Homebrew clang
LOG="$SRC/j613-build-$(date +%Y%m%d-%H%M%S).log"
say "building BOARD=j613 --debug uart, log: $LOG"
cd "$REPO"
set +e
BOARD=j613 \
STANDALONE_BUILD_CONTAINER=always \
ALLOW_UNVALIDATED_CLANG=1 \
MU_PYTHON="$(brew --prefix python@3.12)/bin/python3.12" \
    scripts/build-development.sh --debug uart 2>&1 | tee "$LOG"
STATUS=${PIPESTATUS[0]}
set -e

if [ "$STATUS" = 0 ] && [ -f dist/j613/debug-uart/m1n1.macho ] && [ -f dist/j613/debug-uart/J613_EFI.fd ]; then
    say "BUILD OK"
    ls -l dist/j613/debug-uart/
else
    printf '\n\033[1;33mBuild failed (exit %s). Expected on the first try. Send this file back:\n  %s\033[0m\n' "$STATUS" "$LOG"
    exit "$STATUS"
fi
