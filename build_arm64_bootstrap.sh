#!/usr/bin/env bash
# Copyright 2026 Mark Oxley. All rights reserved.
#
# Cross-build an aarch64 (arm64) bootstrap compiler for Skink WITHOUT
# touching the existing x86-64 bootstrap-home/ tree.
#
# The Skink compiler emits target-agnostic LLVM IR text. We use the
# existing x86-64 `./skink` (or bootstrap-home/skink) to emit that IR,
# then cross-link it with an aarch64-targeting clang + arm64 sysroot to
# produce a native arm64 bootstrap binary.
#
# The result is placed in ./bootstrap-home-arm64/ which mirrors the
# bootstrap-home/ layout (skink binary + std/ + runtime/) and can be
# copied directly to a Raspberry Pi 400 (or any aarch64 Linux box) and
# used as SKINK_HOME for `make`.
#
# Usage:
#   ./build_arm64_bootstrap.sh [--sysroot /path/to/arm64/sysroot]
#
# Requirements (host x86-64 machine):
#   - clang with aarch64 target support (default Fedora clang has it)
#   - lld (the LLVM linker) OR aarch64-linux-gnu-ld
#   - An arm64 sysroot containing: libc, libssl, libcrypto, libz,
#     libsqlite3, libpthread, libdl, libm
#
# Sysroot notes:
#   The sysroot is a directory tree that looks like the root of an arm64
#   Linux install, e.g.:
#     /path/to/sysroot/usr/lib/aarch64-linux-gnu/libssl.so
#     /path/to/sysroot/usr/lib/aarch64-linux-gnu/libc.so
#     /path/to/sysroot/usr/include/openssl/ssl.h
#   You can build one from a Raspberry Pi OS image:
#     mkdir -p sysroot && cd sysroot
#     # extract the rootfs of a Pi OS .img (loop-mount or use guestfish)
#     # then point --sysroot at it.
#   Or install cross packages and assemble one manually.

set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$repo_root"

# --- Configuration -----------------------------------------------------------

TARGET_TRIPLE="aarch64-linux-gnu"
ARM_DIR="bootstrap-home-arm64"
SYSROOT="${SYSROOT:-}"

# Allow --sysroot on the command line.
while [[ $# -gt 0 ]]; do
    case "$1" in
        --sysroot)
            SYSROOT="$2"
            shift 2
            ;;
        --sysroot=*)
            SYSROOT="${1#--sysroot=}"
            shift
            ;;
        --help|-h)
            sed -n '2,30p' "$0"
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

# --- Sanity checks -----------------------------------------------------------

echo "==> Checking for clang with aarch64 target..."
if ! command -v clang >/dev/null 2>&1; then
    echo "ERROR: clang not found. Install clang (e.g. 'dnf install clang' or 'apt install clang')." >&2
    exit 1
fi

if ! clang --print-targets 2>/dev/null | grep -qi "aarch64"; then
    echo "ERROR: this clang does not support the aarch64 target." >&2
    echo "       Install the full LLVM/clang package." >&2
    exit 1
fi

# Prefer lld; fall back to the GNU cross linker.
LINKER_FLAG=""
if command -v ld.lld >/dev/null 2>&1; then
    LINKER_FLAG="-fuse-ld=lld"
elif command -v aarch64-linux-gnu-ld >/dev/null 2>&1; then
    LINKER_FLAG="-fuse-ld=aarch64-linux-gnu-ld"
else
    echo "WARNING: neither ld.lld nor aarch64-linux-gnu-ld found." >&2
    echo "         Will try the default linker; it may fail." >&2
fi

SYSROOT_FLAG=""
if [[ -n "$SYSROOT" ]]; then
    if [[ ! -d "$SYSROOT" ]]; then
        echo "ERROR: sysroot not found: $SYSROOT" >&2
        exit 1
    fi
    SYSROOT_FLAG="--sysroot=$SYSROOT"
    echo "==> Using arm64 sysroot: $SYSROOT"
else
    echo "WARNING: no --sysroot given. clang will search the host (x86) library" >&2
    echo "         paths, which will NOT produce a working arm64 binary." >&2
    echo "         Pass --sysroot /path/to/arm64/sysroot for a real cross build." >&2
    echo "         Continuing anyway in case your toolchain is self-contained..." >&2
fi

# --- Locate a working x86 skink to emit IR ----------------------------------

SKINK_BIN=""
if [[ -x "$repo_root/skink" ]]; then
    SKINK_BIN="$repo_root/skink"
elif [[ -x "$repo_root/bootstrap-home/skink" ]]; then
    SKINK_BIN="$repo_root/bootstrap-home/skink"
else
    echo "ERROR: no x86 skink binary found to emit IR." >&2
    echo "       Run 'make' first to build ./skink." >&2
    exit 1
fi
echo "==> Using x86 skink for IR emission: $SKINK_BIN"

# --- Emit target-agnostic LLVM IR -------------------------------------------

IR_FILE="$repo_root/skink_arm64.ll"
echo "==> Emitting LLVM IR from skink.skink (target-agnostic)..."
SKINK_HOME="$repo_root/bootstrap-home" "$SKINK_BIN" -emit-ll skink.skink > "$IR_FILE"

if [[ ! -s "$IR_FILE" ]]; then
    echo "ERROR: IR emission produced an empty file." >&2
    exit 1
fi
echo "    IR written: $IR_FILE ($(wc -l < "$IR_FILE") lines)"

# --- Cross-link to arm64 ----------------------------------------------------

ARM_BIN="$repo_root/$ARM_DIR/skink"
mkdir -p "$repo_root/$ARM_DIR"

# Assemble the runtime source list (same set skink.skink links).
RT_DIR="$repo_root/bootstrap-home/runtime"
RT_SOURCES=(
    "$RT_DIR/rc_rt.c"
    "$RT_DIR/fs_rt.c"
    "$RT_DIR/regex_rt.c"
    "$RT_DIR/sync_rt.c"
    "$RT_DIR/tensor_rt.c"
    "$RT_DIR/compress_rt.c"
    "$RT_DIR/conc_rt.c"
    "$RT_DIR/db_rt.c"
    "$RT_DIR/reflect_rt.c"
    "$RT_DIR/rules_rt.c"
    "$RT_DIR/panic_rt.c"
    "$RT_DIR/net_rt.c"
    "$RT_DIR/llm_rt.c"
    "$RT_DIR/mqtt_rt.c"
    "$RT_DIR/tls_rt.c"
    "$RT_DIR/template_bridge.ll"
)

LIBS="-lpthread -ldl -lm -lssl -lcrypto -lz -lsqlite3"

echo "==> Cross-linking arm64 bootstrap binary..."
clang -Wno-override-module \
    --target="$TARGET_TRIPLE" \
    $SYSROOT_FLAG \
    $LINKER_FLAG \
    -o "$ARM_BIN" \
    "$IR_FILE" \
    "${RT_SOURCES[@]}" \
    $LIBS

if [[ ! -x "$ARM_BIN" ]]; then
    echo "ERROR: arm64 binary was not produced." >&2
    exit 1
fi

# Verify it's actually arm64.
echo "==> Verifying architecture..."
file "$ARM_BIN"

# --- Mirror std/ and runtime/ into the arm64 bootstrap home ------------------

echo "==> Populating $ARM_DIR/std and $ARM_DIR/runtime..."
mkdir -p "$repo_root/$ARM_DIR/std" "$repo_root/$ARM_DIR/runtime"

# Copy the standard library sources (these are target-agnostic .skink files).
cp -fL "$repo_root"/bootstrap-home/std/*.skink "$repo_root/$ARM_DIR/std/"
# Copy subdirectories (e.g. std/web/).
find "$repo_root/bootstrap-home/std" -mindepth 1 -type d -print0 | while IFS= read -r -d '' d; do
    rel="${d#$repo_root/bootstrap-home/}"
    mkdir -p "$repo_root/$ARM_DIR/$rel"
    cp -fL "$d"/*.skink "$repo_root/$ARM_DIR/$rel/" 2>/dev/null || true
done

# Copy the runtime C/LL sources (needed when the arm64 skink links programs).
cp -fL "$repo_root"/bootstrap-home/runtime/* "$repo_root/$ARM_DIR/runtime/"

# Copy the version module if present.
[[ -f "$repo_root/version.skink" ]] && cp -fL "$repo_root/version.skink" "$repo_root/$ARM_DIR/"

# --- Done -------------------------------------------------------------------

echo
echo "==> arm64 bootstrap home ready: $repo_root/$ARM_DIR"
echo
echo "Next steps:"
echo "  1. Copy the entire repo (or just $ARM_DIR/ + skink.skink) to the Pi:"
echo "       rsync -avz --exclude bootstrap-home --exclude '.git' \\"
echo "         $repo_root/ pi@<pi-host>:~/skink/"
echo "  2. On the Pi, install native deps:"
echo "       sudo apt update && sudo apt install clang make libssl-dev zlib1g-dev libsqlite3-dev"
echo "  3. On the Pi, build with the arm64 bootstrap as stage 0:"
echo "       cd ~/skink"
echo "       make STAGE0=./$ARM_DIR/skink BOOTSTRAP_HOME=./$ARM_DIR"
echo "     (or set SKINK_HOME and invoke the binary directly)"
echo
echo "The x86-64 bootstrap-home/ and ./skink are untouched."
