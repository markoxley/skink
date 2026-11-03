#!/usr/bin/env bash
# Copyright 2026 Mark Oxley. All rights reserved.
#
# Project: Skink — A self-hosted, statically-typed programming language
#          targeting LLVM IR, designed for systems programming with
#          modern ergonomics and a Go-inspired syntax.
#
# File:    build_pi_dist.sh
# Purpose: build_pi_dist.sh — Cross-build a Raspberry Pi (64-bit) Skink distribution tarball from a Raspberry Pi OS image
# Author:  Mark Oxley
# Created: 2026
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

#
# build_pi_dist.sh — Build skink-<version>-linux-arm64.tar.gz (Raspberry Pi OS
# 64-bit) entirely on the x86-64 host, no Pi or root access required.
#
# Pipeline:
#   1. Assemble an aarch64 sysroot in dist/pi-build/sysroot from:
#      - the rootfs of a Raspberry Pi OS 64-bit image (exact runtime libs),
#      - Debian bookworm-arm64 -dev packages fetched from deb.debian.org
#        (headers, crt1/crti/crtn startup objects, .so linker symlinks —
#        no Raspberry Pi OS image ships these).
#   2. Emit target-agnostic LLVM IR for the compiler and each tool with
#      the existing x86-64 ./skink.
#   3. Cross-link every IR file with
#      clang --target=aarch64-linux-gnu --sysroot=… -fuse-ld=lld.
#   4. Package the same layout release.sh produces, plus a .sha256 —
#      install.sh already maps aarch64 → arm64 asset names, so uploading
#      this archive to a GitHub release makes the curl installer work on
#      a Pi out of the box.
#
# Usage:
#   ./build_pi_dist.sh                    # auto-find image in ../skink-extras
#   ./build_pi_dist.sh --image /path/raspios.img[.xz]
#   ./build_pi_dist.sh --clean            # remove cached work dirs first
#   ./build_pi_dist.sh --no-qemu          # skip the qemu smoke test
#
# Requirements (host):
#   - ./skink x86-64 compiler binary (run `make` first if missing)
#   - clang with aarch64 target support, xz, fdisk, debugfs (e2fsprogs),
#     ar, tar, curl or wget
#   - a linker for aarch64: ld.lld or aarch64-linux-gnu-ld on PATH. If
#     absent, the script tries `dnf5 download lld lld-libs` and uses the
#     extracted binaries via LD_LIBRARY_PATH (no root needed).
#   - Optional: qemu-user-static for a "does it run" smoke test (also
#     fetched via dnf5 download when missing).
#
# Sysroot note: use a bookworm image, not trixie. A binary linked against
# bookworm's glibc/OpenSSL runs on both bookworm and trixie; the reverse
# is not true. The Lite image is all that is needed — desktop packages in
# the larger images contribute nothing to a sysroot.
#

set -euo pipefail

# ============================================================================
# Configuration
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
TOOLS_DIR="${WORKSPACE_DIR}/tools"
EXTRAS_DIR="${WORKSPACE_DIR}/skink-extras"
STD_DIR="${SCRIPT_DIR}/std"
RUNTIME_DIR="${STD_DIR}/runtime"

# Work/cache dir lives OUTSIDE dist/ — `make clean` rm -rf's dist/, and the
# sysroot + extracted image + host toolchain take minutes to rebuild.
WORK_DIR="${SCRIPT_DIR}/.pi-build"
SYSROOT="${WORK_DIR}/sysroot"
IR_DIR="${WORK_DIR}/ir"
DEB_DIR="${WORK_DIR}/debs"
HOSTTOOLS="${WORK_DIR}/hosttools"
IMG_DIR="${WORK_DIR}/img"
OUTPUT_DIR="${SCRIPT_DIR}/dist"

TARGET_TRIPLE="aarch64-linux-gnu"
TARGET_NAME="linux-arm64"
DEBIAN_MIRROR="https://deb.debian.org/debian"
# Prefer -updates versions (they most closely match a recent image); fall
# back to the base bookworm archive.
DEBIAN_DISTS=("bookworm-updates" "bookworm")

# Debian arm64 packages layered over the image rootfs. Together these give
# clang+lld everything they need to compile and link: libc6-dev provides
# headers + crt objects + .so linker scripts, libgcc-12-dev provides
# crtbegin/crtend + libgcc.a, linux-libc-dev provides the kernel headers
# glibc's headers reference, and the rest are the -dev/-runtime pairs for
# the libraries the Skink driver links (-lssl -lcrypto -lz -lsqlite3).
WANT_DEBS=(
    libc6
    libc6-dev
    libc-dev-bin
    linux-libc-dev
    libgcc-s1
    libgcc-12-dev
    libssl3
    libssl-dev
    zlib1g
    zlib1g-dev
    libsqlite3-0
    libsqlite3-dev
)

# (moved below arg parsing — --version overrides the version.txt default)

IMAGE=""
CLEAN=0
USE_QEMU=1
# Version for the archive name; release.sh overrides this via --version so
# the arm64 archive always matches the native one in the same invocation.
VERSION_ARG=""

# ============================================================================
# Helpers
# ============================================================================

info()  { printf "\033[1;34m==>\033[0m %s\n" "$*"; }
warn()  { printf "\033[1;33mwarn:\033[0m %s\n" "$*"; }
error() { printf "\033[1;31merror:\033[0m %s\n" "$*" >&2; }
fail()  { error "$*"; exit 1; }

fetch() {
    # fetch <url> <output>
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$2" "$1"
    else
        fail "need curl or wget to download $1"
    fi
}

hosttools_ldpath() {
    # Library path covering the extracted usr/lib64 plus any versioned
    # llvm*/lib64 dirs Fedora's lld-libs/llvm-libs RPMs install into.
    local p="${HOSTTOOLS}/root/usr/lib64" d
    for d in "${HOSTTOOLS}/root/usr/lib64"/llvm*/lib64; do
        [[ -d "$d" ]] && p="${p}:${d}"
    done
    printf '%s' "$p"
}

extract_rpm() {
    # extract_rpm <rpm-file> <dest-root> — unpack an RPM payload without a
    # package manager. Goes through a temp file rather than a pipe: cpio
    # can close stdin early when entries already exist, which would
    # SIGPIPE rpm2cpio and trip `set -o pipefail`.
    local rpm="$1" dest="$2" tmp
    tmp="$(mktemp "${TMPDIR:-/tmp}/skink-rpm.XXXXXX.cpio")"
    rpm2cpio "$rpm" > "$tmp"
    (cd "$dest" && cpio -idm --quiet < "$tmp") || warn "partial extract of $(basename "$rpm")"
    rm -f "$tmp"
}

usage() {
    sed -n '/^# build_pi_dist.sh/,/^#$/p' "$0" | sed 's/^# \?//'
    exit 0
}

# ============================================================================
# Argument parsing
# ============================================================================

while [[ $# -gt 0 ]]; do
    case "$1" in
        --image)      IMAGE="$2"; shift 2 ;;
        --image=*)    IMAGE="${1#--image=}"; shift ;;
        --version)    VERSION_ARG="$2"; shift 2 ;;
        --version=*)  VERSION_ARG="${1#--version=}"; shift ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --output-dir=*) OUTPUT_DIR="${1#--output-dir=}"; shift ;;
        --clean)      CLEAN=1; shift ;;
        --no-qemu)    USE_QEMU=0; shift ;;
        --help|-h)    usage ;;
        *)           fail "unknown argument: $1 (try --help)" ;;
    esac
done

VERSION="${VERSION_ARG:-$(cat "${SCRIPT_DIR}/version.txt" 2>/dev/null || echo "0.0.0")}"
VERSION="${VERSION#v}"

# ============================================================================
# Phase 0: sanity checks
# ============================================================================

info "Checking host prerequisites..."

for tool in clang xz fdisk debugfs ar tar python3 file; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing required tool: $tool"
done

if ! clang --print-targets 2>/dev/null | grep -qi "aarch64"; then
    fail "this clang lacks the aarch64 target — install the full LLVM/clang package"
fi

X86_SKINK="${SCRIPT_DIR}/skink"
if [[ ! -x "$X86_SKINK" ]]; then
    fail "no x86-64 ./skink binary to emit IR — run \`make\` in ${SCRIPT_DIR} first"
fi
info "Host compiler: $("$X86_SKINK" -v 2>/dev/null | head -1 || echo "ok")"

if [[ "$CLEAN" -eq 1 ]]; then
    info "Removing cached work directory ${WORK_DIR}"
    rm -rf "$WORK_DIR"
fi

mkdir -p "$SYSROOT" "$IR_DIR" "$DEB_DIR" "$IMG_DIR" "$OUTPUT_DIR"

# ============================================================================
# Phase 1: Raspberry Pi OS image → rootfs partition
# ============================================================================
# The image is read without root: fdisk parses the DOS partition table,
# dd extracts the Linux partition, and debugfs (e2fsprogs) rdumps the
# directories we need straight out of the ext4 filesystem.

IMG_FILE="${IMG_DIR}/raspios.img"

if [[ ! -s "$IMG_FILE" ]]; then
    if [[ -z "$IMAGE" ]]; then
        # Auto-find the newest *.img or *.img.xz under skink-extras/.
        for cand in "$EXTRAS_DIR"/*.img.xz "$EXTRAS_DIR"/*.img "$SCRIPT_DIR"/*.img.xz "$SCRIPT_DIR"/*.img; do
            [[ -f "$cand" ]] && IMAGE="$cand"
        done
    fi
    [[ -n "$IMAGE" && -f "$IMAGE" ]] || \
        fail "no Raspberry Pi OS image found — pass --image /path/to/raspios.img[.xz]"
    info "Using image: $IMAGE"
    case "$IMAGE" in
        *.xz)  info "Decompressing image (this takes a moment)..."
               xz -dkc "$IMAGE" > "$IMG_FILE" ;;
        *.img) cp --reflink=auto "$IMAGE" "$IMG_FILE" ;;
        *)     fail "unrecognized image format: $IMAGE (want .img or .img.xz)" ;;
    esac
else
    info "Using cached decompressed image: $IMG_FILE"
fi

[[ -s "$IMG_FILE" ]] || fail "decompressed image is empty: $IMG_FILE"

# Partition table: take the LAST partition of type Linux (p1 is the FAT32
# boot partition on Raspberry Pi OS, p2 the ext4 rootfs).
read -r P_START P_END <<<"$(fdisk -l -o Device,Start,End,Type "$IMG_FILE" 2>/dev/null \
    | awk '$2 ~ /^[0-9]+$/ && $0 ~ /Linux/ {s=$2; e=$3} END{print s, e}')"
[[ -n "${P_START:-}" && -n "${P_END:-}" ]] || fail "could not find a Linux partition in $IMG_FILE"

# Sector size — fdisk reports it as "Units: sectors of 1 * N = M bytes".
SECTOR="$(fdisk -l "$IMG_FILE" 2>/dev/null \
    | sed -n 's/^Units: sectors of 1 \* \([0-9][0-9]*\) =.*/\1/p' | head -1)"
SECTOR="${SECTOR:-512}"
info "Rootfs partition: sectors ${P_START}..${P_END} (${SECTOR}-byte sectors)"

ROOTFS_IMG="${IMG_DIR}/rootfs.img"
if [[ ! -s "$ROOTFS_IMG" ]]; then
    info "Extracting rootfs partition (sparse)..."
    dd if="$IMG_FILE" of="$ROOTFS_IMG" bs=4M \
        iflag=skip_bytes,count_bytes conv=sparse \
        skip=$((P_START * SECTOR)) \
        count=$(( (P_END - P_START + 1) * SECTOR )) \
        status=none
else
    info "Using cached rootfs image: $ROOTFS_IMG"
fi

# rdump a directory out of the ext4 image into the sysroot. debugfs's
# rdump semantics vary on whether the destination exists, so dump into a
# scratch dir and copy the payload across deterministically.
img_rdump() {
    local src="$1" dst="$2"
    local scratch="${IMG_DIR}/rdump_tmp"
    rm -rf "$scratch"; mkdir -p "$scratch"
    if ! debugfs -R "stat ${src}" "$ROOTFS_IMG" >/dev/null 2>&1; then
        warn "image has no ${src} — skipping"
        return 0
    fi
    debugfs -R "rdump ${src} ${scratch}" "$ROOTFS_IMG" >/dev/null 2>&1 \
        || fail "debugfs rdump failed for ${src}"
    mkdir -p "$(dirname "$dst")"
    local base; base="$(basename "$src")"
    if [[ -d "${scratch}/${base}" ]]; then
        cp -a "${scratch}/${base}" "$dst"
    else
        cp -a "$scratch" "$dst"
    fi
    rm -rf "$scratch"
}

if [[ ! -f "${WORK_DIR}/.sysroot-image-done" ]]; then
    info "Pulling runtime libraries out of the image rootfs..."
    img_rdump "/usr/lib/aarch64-linux-gnu" "${SYSROOT}/usr/lib/aarch64-linux-gnu"
    img_rdump "/usr/include" "${SYSROOT}/usr/include"
    img_rdump "/etc/ld.so.conf.d" "${SYSROOT}/etc/ld.so.conf.d"
    touch "${WORK_DIR}/.sysroot-image-done"
else
    info "Image extraction already done (cached)"
fi

# ============================================================================
# Phase 2: Debian bookworm-arm64 -dev packages → sysroot overlay
# ============================================================================
# Resolve exact .deb paths from the Packages index so the script keeps
# working as point releases roll forward. Debs are extracted AFTER the
# image so each dev package's matched runtime lib wins consistently.

if [[ ! -f "${WORK_DIR}/.sysroot-debs-done" ]]; then
    info "Resolving Debian bookworm-arm64 packages..."
    mkdir -p "$DEB_DIR"
    for dist in "${DEBIAN_DISTS[@]}"; do
        idx="${DEB_DIR}/Packages-${dist}.xz"
        [[ -s "$idx" ]] || fetch \
            "${DEBIAN_MIRROR}/dists/${dist}/main/binary-arm64/Packages.xz" "$idx"
    done

    deb_filename() {
        local pkg="$1" dist idx
        for dist in "${DEBIAN_DISTS[@]}"; do
            idx="${DEB_DIR}/Packages-${dist}.xz"
            [[ -s "$idx" ]] || continue
            local f
            f="$(xz -dc "$idx" | awk -v pkg="$pkg" '
                BEGIN { RS=""; FS="\n" }
                {
                    name=""; file=""
                    for (i = 1; i <= NF; i++) {
                        if ($i ~ /^Package: /)  name = substr($i, 10)
                        if ($i ~ /^Filename: /) file = substr($i, 11)
                    }
                    if (name == pkg) { print file; exit }
                }')"
            [[ -n "$f" ]] && { printf '%s' "$f"; return 0; }
        done
        return 1
    }

    extract_deb() {
        local deb="$1" member
        member="$(ar t "$deb" | grep -m1 '^data\.tar\.')"
        case "$member" in
            data.tar.xz)  ar p "$deb" "$member" | tar -xJf - -C "$SYSROOT" ;;
            data.tar.zst) ar p "$deb" "$member" | zstd -dc | tar -xf - -C "$SYSROOT" ;;
            data.tar.gz)  ar p "$deb" "$member" | tar -xzf - -C "$SYSROOT" ;;
            *) fail "unhandled deb payload '${member}' in ${deb}" ;;
        esac
    }

    for pkg in "${WANT_DEBS[@]}"; do
        rel="$(deb_filename "$pkg")" \
            || warn "package not found in bookworm indexes: ${pkg} (skipping)"
        [[ -n "${rel:-}" ]] || continue
        deb="${DEB_DIR}/$(basename "$rel")"
        [[ -s "$deb" ]] || { info "  fetch ${pkg}"; fetch "${DEBIAN_MIRROR}/${rel}" "$deb"; }
        extract_deb "$deb"
    done

    # Debian's merged-/usr layout: debs still ship files under /lib, while
    # clang's sysroot search paths live under /usr/lib. Fold lib/ into
    # usr/lib/ and leave a symlink so absolute paths embedded in linker
    # scripts (/lib/aarch64-linux-gnu/…) resolve inside the sysroot.
    if [[ -d "${SYSROOT}/lib" && ! -L "${SYSROOT}/lib" ]]; then
        info "Merging sysroot /lib into /usr/lib (usrmerge)"
        cp -a "${SYSROOT}/lib/." "${SYSROOT}/usr/lib/"
        rm -rf "${SYSROOT}/lib"
        ln -sfn usr/lib "${SYSROOT}/lib"
    fi
    touch "${WORK_DIR}/.sysroot-debs-done"
else
    info "Debian package overlay already done (cached)"
fi

# Sysroot sanity: everything the cross-link will actually need.
info "Verifying sysroot contents..."
for need in \
    usr/lib/aarch64-linux-gnu/libc.so.6 \
    usr/lib/aarch64-linux-gnu/Scrt1.o \
    usr/lib/aarch64-linux-gnu/libssl.so.3 \
    usr/lib/aarch64-linux-gnu/libz.so.1 \
    usr/lib/aarch64-linux-gnu/libsqlite3.so.0 \
    usr/include/stdio.h \
    usr/include/openssl/ssl.h \
    usr/include/zlib.h \
    usr/include/sqlite3.h \
    ; do
    [[ -e "${SYSROOT}/${need}" ]] || fail "sysroot missing ${need}"
done
CRT_BEGIN="$(find "${SYSROOT}/usr/lib/gcc" -name crtbegin.o 2>/dev/null | head -1)"
[[ -n "$CRT_BEGIN" ]] || warn "no crtbegin.o under sysroot gcc dir — link may fail"
info "Sysroot ready: ${SYSROOT}"

# ============================================================================
# Phase 3: an aarch64-capable linker
# ============================================================================

LINKER_FLAG=""
LLD_BIN=""

# Previously extracted lld — verify it still RUNS, not just exists. A host
# OS upgrade can remove the libLLVM it was built against (e.g. an fc44
# lld-libs needs libLLVM.so.22.1, gone after upgrading to fc45).
if [[ -x "${HOSTTOOLS}/root/usr/bin/ld.lld" ]]; then
    export PATH="${HOSTTOOLS}/root/usr/bin:${PATH}"
    export LD_LIBRARY_PATH="$(hosttools_ldpath):${LD_LIBRARY_PATH:-}"
    if ld.lld --version >/dev/null 2>&1; then
        LLD_BIN="${HOSTTOOLS}/root/usr/bin/ld.lld"
        info "Using cached lld: ${LLD_BIN}"
    else
        warn "cached lld no longer runs (host LLVM changed?) — refetching"
        rm -rf "${HOSTTOOLS}/root" "${HOSTTOOLS}/rpms"
        # Forget stale PATH lookups — bash's hash table would otherwise
        # still resolve ld.lld inside the just-deleted directory.
        hash -r
    fi
fi

if [[ -z "$LLD_BIN" ]] && command -v ld.lld >/dev/null 2>&1 \
        && ld.lld --version >/dev/null 2>&1; then
    LLD_BIN="$(command -v ld.lld)"
    info "Using system lld: ${LLD_BIN}"
fi

if [[ -z "$LLD_BIN" ]] && command -v aarch64-linux-gnu-ld >/dev/null 2>&1; then
    LINKER_FLAG="-fuse-ld=$(command -v aarch64-linux-gnu-ld)"
    info "Using aarch64-linux-gnu-ld"
fi

if [[ -z "$LLD_BIN" && -z "$LINKER_FLAG" ]]; then
    warn "no aarch64 linker on PATH — fetching lld via dnf5 download (unprivileged)"
    RPM_DIR="${HOSTTOOLS}/rpms"
    mkdir -p "$RPM_DIR" "${HOSTTOOLS}/root"
    if command -v dnf5 >/dev/null 2>&1; then
        DNF="dnf5"
    elif command -v dnf >/dev/null 2>&1; then
        DNF="dnf"
    else
        fail "no ld.lld/aarch64-linux-gnu-ld and no dnf to fetch them — install lld"
    fi
    (cd "$RPM_DIR" && "$DNF" download --destdir="$RPM_DIR" lld.x86_64 lld-libs.x86_64) \
        || fail "$DNF download failed — install lld or binutils-aarch64-linux-gnu manually"
    # llvm-libs carries the libLLVM.so.N that lld-libs links against —
    # extracting it too makes the toolchain self-contained and immune to
    # host LLVM version changes. Optional: if the package name differs on
    # this distro, the lld we extract just binds the system libLLVM.
    (cd "$RPM_DIR" && "$DNF" download --destdir="$RPM_DIR" llvm-libs.x86_64) \
        || warn "llvm-libs not fetched — extracted lld will bind the host libLLVM"
    for rpm in "$RPM_DIR"/*.rpm; do
        [[ -f "$rpm" ]] || continue
        case "$rpm" in *i686.rpm) continue ;; esac
        extract_rpm "$rpm" "${HOSTTOOLS}/root"
    done
    export PATH="${HOSTTOOLS}/root/usr/bin:${PATH}"
    export LD_LIBRARY_PATH="$(hosttools_ldpath):${LD_LIBRARY_PATH:-}"
    LLD_BIN="${HOSTTOOLS}/root/usr/bin/ld.lld"
    "$LLD_BIN" --version >/dev/null 2>&1 \
        || fail "extracted lld but ld.lld still not runnable"
    info "Using extracted lld: ${LLD_BIN}"
fi

if [[ -z "$LINKER_FLAG" ]]; then
    # clang 23 (Fedora 45) rejects the bare name in -fuse-ld=lld; the
    # absolute-path form is accepted across clang versions.
    LINKER_FLAG="-fuse-ld=${LLD_BIN}"
fi

# ============================================================================
# Phase 4: emit target-agnostic LLVM IR for every binary
# ============================================================================
# The compiler emits triple-less IR, so one x86-64 skink can produce IR
# for any target — the architecture is decided entirely at clang link
# time. Emitting IR for skink.skink takes ~8-10 minutes (see AGENTS.md);
# the tools are small.

emit_ir() {
    # emit_ir <name> <srcdir> <mainfile>
    local name="$1" dir="$2" main="$3" out="${IR_DIR}/$1.ll"
    if [[ ! -d "$dir" || ! -f "${dir}/${main}" ]]; then
        warn "${dir}/${main} not found — skipping ${name}"
        return 1
    fi
    if [[ -s "$out" ]]; then
        info "IR cached: ${name}"
        return 0
    fi
    info "Emitting IR: ${name} (${dir##*/}/${main})"
    (cd "$dir" && SKINK_HOME="$STD_DIR" "$X86_SKINK" -emit-ll "$main" > "$out")
    [[ -s "$out" ]] || fail "IR emission produced empty output for ${name}"
    return 0
}

declare -a BUILT_NAMES=()
emit_one() { emit_ir "$@" && BUILT_NAMES+=("$1") || true; }

emit_one "skink"     "${SCRIPT_DIR}"            "skink.skink"
emit_one "skink-lsp" "${TOOLS_DIR}/skink-lsp"   "lsp.skink"
emit_one "skinkfmt"  "${TOOLS_DIR}/skink-lsp"   "skinkfmt.skink"
emit_one "scales"    "${TOOLS_DIR}/scales"      "scales.skink"
emit_one "skinkjs"   "${TOOLS_DIR}/skinkjs"     "skinkjs.skink"
emit_one "skinkcov"  "${TOOLS_DIR}/skinkcov"    "skinkcov.skink"

[[ "${#BUILT_NAMES[@]}" -gt 0 ]] || fail "nothing was built"

# ============================================================================
# Phase 5: cross-link every binary
# ============================================================================
# Runtime set mirrors the driver (skink.skink): all runtime/*.c except
# ui_rt.c (std/ui only, needs SDL2 — out of scope for the Pi sysroot) and
# cov_rt.c (only linked for -cov builds), plus any *.ll shims.

RT_SOURCES=()
for f in "$RUNTIME_DIR"/*.c; do
    case "$(basename "$f")" in ui_rt.c|cov_rt.c) continue ;; esac
    RT_SOURCES+=("$f")
done
for f in "$RUNTIME_DIR"/*.ll; do
    [[ -f "$f" ]] && RT_SOURCES+=("$f")
done

LIBS=(-lpthread -ldl -lm -lssl -lcrypto -lz -lsqlite3)
BIN_DIR="${WORK_DIR}/bin"
mkdir -p "$BIN_DIR"

link_one() {
    local name="$1" ll="${IR_DIR}/$1.ll" out="${BIN_DIR}/$1"
    info "Cross-linking ${name} for ${TARGET_TRIPLE}..."
    clang -Wno-override-module \
        --target="$TARGET_TRIPLE" \
        --sysroot="$SYSROOT" \
        $LINKER_FLAG \
        -O2 \
        -o "$out" \
        "$ll" \
        "${RT_SOURCES[@]}" \
        "${LIBS[@]}"
    [[ -x "$out" ]] || fail "link did not produce ${out}"
    file "$out" | grep -q "aarch64\|ARM aarch64" \
        || fail "${out} is not an aarch64 binary"
}

for name in "${BUILT_NAMES[@]}"; do
    link_one "$name"
done

# ============================================================================
# Phase 6: smoke test under qemu (best effort)
# ============================================================================

QEMU=""
if [[ "$USE_QEMU" -eq 1 ]]; then
    for q in qemu-aarch64-static qemu-aarch64; do
        command -v "$q" >/dev/null 2>&1 && QEMU="$q" && break
    done
    # Fedora splits the static emulators per-target: qemu-user-static is a
    # meta-package, the aarch64 emulator binary is in
    # qemu-user-static-aarch64 as usr/bin/qemu-aarch64-static.
    for qc in "${HOSTTOOLS}/root/usr/bin/qemu-aarch64-static" \
              "${HOSTTOOLS}/root/usr/libexec/qemu-user-static/qemu-aarch64-static"; do
        [[ -x "$qc" ]] && QEMU="$qc" && break
    done
    if [[ -z "$QEMU" ]] && command -v dnf5 >/dev/null 2>&1; then
        info "Fetching qemu-user-static-aarch64 for the smoke test..."
        QRPM="${HOSTTOOLS}/qrpms"; mkdir -p "$QRPM"
        if (cd "$QRPM" && dnf5 download --destdir="$QRPM" qemu-user-static-aarch64.x86_64 >/dev/null 2>&1); then
            for rpm in "$QRPM"/*.rpm; do
                [[ -f "$rpm" ]] || continue
                case "$rpm" in *i686.rpm) continue ;; esac
                extract_rpm "$rpm" "${HOSTTOOLS}/root"
            done
            QEMU="${HOSTTOOLS}/root/usr/bin/qemu-aarch64-static"
            [[ -x "$QEMU" ]] || QEMU=""
        fi
    fi
fi

if [[ -n "$QEMU" ]]; then
    info "Smoke-testing binaries under qemu (sysroot=${SYSROOT})..."
    "$QEMU" -L "$SYSROOT" "${BIN_DIR}/skink" -v | head -2 \
        || warn "qemu run of skink -v failed"
    # Emitting IR for a hello-world exercises module resolution, the
    # checker, and the emitter end-to-end on aarch64 code.
    printf 'module main\nfn main() { println("pi ok") }\n' > "${WORK_DIR}/smoke.skink"
    if SKINK_HOME="$STD_DIR" "$QEMU" -L "$SYSROOT" \
            "${BIN_DIR}/skink" -emit-ll "${WORK_DIR}/smoke.skink" 2>/dev/null \
            | grep -q 'puts\|Skink_'; then
        info "qemu emit-ll smoke test passed"
    else
        warn "qemu emit-ll smoke test produced no IR"
    fi
    "$QEMU" -L "$SYSROOT" "${BIN_DIR}/scales" version 2>/dev/null | head -1 \
        || warn "qemu run of scales failed"
else
    warn "qemu-aarch64 not available — skipping run test (binaries verified as aarch64 ELF)"
fi

# ============================================================================
# Phase 7: package the tarball (same layout as release.sh)
# ============================================================================

ARCHIVE_NAME="skink-${VERSION}-${TARGET_NAME}.tar.gz"
STAGING_DIR="$(mktemp -d)"
trap 'rm -rf "$STAGING_DIR"' EXIT

info "Packaging ${ARCHIVE_NAME}..."
for name in "${BUILT_NAMES[@]}"; do
    cp "${BIN_DIR}/${name}" "${STAGING_DIR}/${name}"
    chmod 755 "${STAGING_DIR}/${name}"
done

# skink -t / -b resolve their harness scripts relative to argv[0], so they
# must sit beside the installed compiler binary.
for harness in skink_test.sh skink_bench.sh; do
    if [[ -f "${SCRIPT_DIR}/${harness}" ]]; then
        cp "${SCRIPT_DIR}/${harness}" "${STAGING_DIR}/${harness}"
        chmod 755 "${STAGING_DIR}/${harness}"
    fi
done

# Standard library, filtered exactly like release.sh.
cp -r "$STD_DIR" "${STAGING_DIR}/std"
find "${STAGING_DIR}/std" -name '*_test.skink' -delete
find "${STAGING_DIR}/std" -name '__skink_test_*' -delete
find "${STAGING_DIR}/std" -name '*.insyncdl' -delete
rm -rf "${STAGING_DIR}/std/fixture_libmain" "${STAGING_DIR}/std/diamond" \
       "${STAGING_DIR}/std/compilefail" "${STAGING_DIR}/std/regress_helper.skink"

ARCHIVE_PATH="${OUTPUT_DIR}/${ARCHIVE_NAME}"
tar -czf "$ARCHIVE_PATH" -C "$STAGING_DIR" .
sha256sum "$ARCHIVE_PATH" | awk '{print $1"  '"$(basename "$ARCHIVE_PATH")"'"}' \
    > "${ARCHIVE_PATH}.sha256"

# ============================================================================
# Done
# ============================================================================

echo ""
info "Pi distribution built: ${ARCHIVE_PATH}"
info "  Binaries: ${BUILT_NAMES[*]}"
info "  Size: $(du -h "$ARCHIVE_PATH" | cut -f1)"
echo ""
echo "On the Pi (64-bit Raspberry Pi OS):"
echo "  1. sudo apt update && sudo apt install clang make libssl-dev zlib1g-dev libsqlite3-dev git"
echo "     (add libsdl2-dev for std/ui programs)"
echo "  2. tar -xzf ${ARCHIVE_NAME} -C ~/.local/skink --strip-components=0  # or anywhere"
echo "  3. add the bin dir to PATH and set SKINK_HOME to the extracted std/ dir"
echo ""
echo "To ship it via the curl installer, upload to GitHub releases as:"
echo "  gh release create v${VERSION} ${ARCHIVE_PATH} ${ARCHIVE_PATH}.sha256 \\"
echo "    --title \"Skink v${VERSION}\" --notes \"Release v${VERSION}\""
echo "install.sh already maps aarch64 → ${TARGET_NAME}, so Pi users get this"
echo "archive automatically once it is attached to the release."
