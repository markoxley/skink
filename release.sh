#!/usr/bin/env bash
# Copyright 2026 Mark Oxley. All rights reserved.
#
# Project: Skink — A self-hosted, statically-typed programming language
#          targeting LLVM IR, designed for systems programming with
#          modern ergonomics and a Go-inspired syntax.
#
# File:    release.sh
# Purpose: release.sh — Build and package a Skink release archive for GitHub This script builds the skink compiler and skink-lsp, t...
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
# release.sh — Build, package, and publish a Skink release to GitHub
#
# This script builds the skink compiler, skink-lsp, skinkfmt, scales,
# skinkjs, and skinkcov, then packages them with the standard library and C runtime into a
# tarball. On Linux hosts it also produces the linux-arm64 (Raspberry Pi)
# archive via build_pi_dist.sh and the windows-x86_64 / windows-aarch64
# archives via build_windows.sh, then publishes everything to GitHub
# releases with the gh CLI — creating the v<version> release (and its git
# tag on the remote) or replacing the assets of an existing one.
#
# Version numbering lives HERE: this script bumps the patch component of
# version.txt unless an explicit version argument or --no-bump is given.
# The Makefile never advances version.txt — a plain `make` builds at
# whatever version the file already records.
#
# Usage:
#   ./release.sh                    # bump patch version, build, publish
#   ./release.sh 0.2.0              # explicit version (writes version.txt)
#   ./release.sh --no-bump          # build archives at current version.txt
#   ./release.sh --no-publish       # build archives only, no upload
#   ./release.sh --skip-windows     # skip the Windows archives
#   ./release.sh --cross linux-x86_64  # cross-compile target
#

set -euo pipefail

# ============================================================================
# Configuration
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKINK_HOME_DIR="${SCRIPT_DIR}/std"
LSP_DIR="${SCRIPT_DIR}/../tools/skink-lsp"
SCALES_DIR="${SCRIPT_DIR}/../tools/scales"
SKINKJS_DIR="${SCRIPT_DIR}/../tools/skinkjs"
SKINKCOV_DIR="${SCRIPT_DIR}/../tools/skinkcov"

# Version
VERSION=""
EXPLICIT_VERSION=0
if [[ $# -ge 1 ]] && [[ "$1" != --* ]]; then
    VERSION="$1"
    EXPLICIT_VERSION=1
    shift
else
    VERSION="$(cat "${SCRIPT_DIR}/version.txt" 2>/dev/null || echo "0.0.0")"
fi

# Strip leading 'v'
VERSION="${VERSION#v}"

# Output directory for archives
OUTPUT_DIR="${SCRIPT_DIR}/dist"

# Whether to publish the built archives to GitHub releases via the gh CLI.
# On by default (the "poor man's CI" flow: ./release.sh builds, packages,
# and publishes). Disable with --no-publish or PUBLISH=0. Skipped
# gracefully when gh is missing or not authenticated.
PUBLISH="${PUBLISH:-1}"
# --dry-run prints the gh commands without executing them.
DRY_RUN=0
# --no-bump keeps version.txt as-is (rebuilds of an existing release).
NO_BUMP="${NO_BUMP:-0}"
# --skip-windows skips the windows-x86_64/windows-aarch64 archives
# (e.g. on hosts without the MinGW/llvm-mingw cross toolchains).
SKIP_WINDOWS=0

# ============================================================================
# Helpers
# ============================================================================

info()  { printf "\033[1;34m==>\033[0m %s\n" "$*"; }
warn()  { printf "\033[1;33mwarn:\033[0m %s\n" "$*"; }
error() { printf "\033[1;31merror:\033[0m %s\n" "$*" >&2; }
fail()  { error "$*"; exit 1; }

# ============================================================================
# Detect platform
# ============================================================================

OS="$(uname -s)"
ARCH="$(uname -m)"

case "$OS" in
    Linux)  OS="linux" ;;
    Darwin) OS="darwin" ;;
    *)      fail "Unsupported OS: $OS" ;;
esac

case "$ARCH" in
    x86_64|amd64) ARCH="x86_64" ;;
    aarch64|arm64) ARCH="arm64" ;;
    *)             fail "Unsupported architecture: $ARCH" ;;
esac

TARGET="${OS}-${ARCH}"

# Whether to also build the linux-arm64 (Raspberry Pi) archive via
# build_pi_dist.sh. Only meaningful on Linux hosts; skipped automatically
# elsewhere.
SKIP_ARM64=0
# Raspberry Pi OS image for the arm64 sysroot (empty = auto-find under
# ../skink-extras); passed through to build_pi_dist.sh --image.
PI_IMAGE=""

# Parse remaining args
while [[ $# -gt 0 ]]; do
    case "$1" in
        --cross)
            TARGET="$2"
            shift 2
            ;;
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --skip-arm64)
            SKIP_ARM64=1
            shift
            ;;
        --skip-windows)
            SKIP_WINDOWS=1
            shift
            ;;
        --no-bump)
            NO_BUMP=1
            shift
            ;;
        --no-publish)
            PUBLISH=0
            shift
            ;;
        --publish)
            PUBLISH=1
            shift
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        --pi-image)
            PI_IMAGE="$2"
            shift 2
            ;;
        --pi-image=*)
            PI_IMAGE="${1#--pi-image=}"
            shift
            ;;
        --help|-h)
            cat <<EOF
Skink release packager — builds and packages the toolset for distribution.

Builds the native archive plus, on Linux hosts, the linux-arm64 (Raspberry
Pi) cross-build via build_pi_dist.sh — both land in the output dir ready
for 'gh release create'.

Usage:
  $0 [version] [options]

Arguments:
  version            Version string (default: from version.txt)

Options:
  --cross <target>   Cross-compile target (e.g. linux-x86_64, darwin-arm64)
  --output-dir <dir> Where to write the archive (default: ./dist)
  --skip-arm64       Build only the native archive
  --skip-windows     Skip the windows-x86_64/windows-aarch64 archives
  --no-bump          Do not bump version.txt (rebuild current version)
  --no-publish       Build archives but do not upload to GitHub
  --publish          Upload to GitHub releases (default; needs gh auth)
  --dry-run          Print the gh commands instead of running them
  --pi-image <path>  Raspberry Pi OS image for the arm64 sysroot
                     (default: auto-find ../skink-extras/*.img[.xz])
  --help             Show this help message
EOF
            exit 0
            ;;
        *)
            fail "Unknown option: $1"
            ;;
    esac
done

# Split target into OS/ARCH
TARGET_OS="${TARGET%%-*}"
TARGET_ARCH="${TARGET##*-}"

# ============================================================================
# Git hygiene gate — must run BEFORE the version.txt bump (which dirties
# the tree intentionally). gh creates the release tag on the remote's
# pushed HEAD, so a release built from uncommitted or unpushed work
# ships code the tag does not contain.
#   1. Uncommitted changes           -> abort with the file list.
#   2. Clean tree, unpushed commits  -> push to the tracked upstream
#      first, then continue.
# ============================================================================
if command -v git >/dev/null 2>&1 && git -C "$SCRIPT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    if [[ -n "$(git -C "$SCRIPT_DIR" status --porcelain 2>/dev/null)" ]]; then
        error "working tree has uncommitted changes — refusing to release"
        git -C "$SCRIPT_DIR" status --short >&2 || true
        fail "commit or stash these changes first"
    fi

    UPSTREAM_REF="$(git -C "$SCRIPT_DIR" rev-parse --abbrev-ref '@{upstream}' 2>/dev/null || true)"
    if [[ -n "$UPSTREAM_REF" ]]; then
        UNPUSHED="$(git -C "$SCRIPT_DIR" log --oneline "${UPSTREAM_REF}..HEAD" 2>/dev/null || true)"
        if [[ -n "$UNPUSHED" ]]; then
            info "Local commits not on ${UPSTREAM_REF}:"
            echo "$UNPUSHED" | sed 's/^/  /'
            if [[ "$DRY_RUN" -eq 1 ]]; then
                info "[dry-run] git push"
            else
                info "Pushing to ${UPSTREAM_REF}..."
                git -C "$SCRIPT_DIR" push || fail "git push failed — resolve and re-run release.sh"
            fi
        fi
    else
        warn "no upstream tracking branch — 'gh release create' will tag the remote's HEAD,"
        warn "which may not include this release's code (push and set upstream to be safe)"
    fi
else
    warn "not inside a git work tree (or git unavailable) — skipping commit/push checks"
fi

# ============================================================================
# Version bump — release.sh is the single place version.txt advances.
# ============================================================================
# An explicit version argument is written back to version.txt so the built
# binaries report exactly the released version; otherwise the patch
# component is bumped (opt out with --no-bump). version.skink is
# regenerated from version.txt by `make build` below, so the compiler's
# -v output matches this release.
if [[ "$EXPLICIT_VERSION" -eq 1 ]]; then
    echo "$VERSION" > "${SCRIPT_DIR}/version.txt"
elif [[ "$NO_BUMP" -eq 0 ]]; then
    VERSION="$(awk -F. '{print $1"."$2"."($3+1)}' "${SCRIPT_DIR}/version.txt")"
    echo "$VERSION" > "${SCRIPT_DIR}/version.txt"
    info "Bumped version.txt to ${VERSION}"
fi

# Commit and push the version bump so the tag gh creates on the remote's
# pushed HEAD actually contains the released version. Only version.txt is
# committed — the hygiene gate above already proved the tree was clean
# before the bump, so staging the file alone is safe. --no-bump runs with
# an untouched version.txt are skipped.
if command -v git >/dev/null 2>&1 && git -C "$SCRIPT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1 \
    && [[ -n "$(git -C "$SCRIPT_DIR" status --porcelain -- version.txt 2>/dev/null)" ]]; then
    if [[ "$DRY_RUN" -eq 1 ]]; then
        info "[dry-run] git commit + push version.txt"
    else
        git -C "$SCRIPT_DIR" add version.txt \
            && git -C "$SCRIPT_DIR" commit -q -m "Bump version to ${VERSION}" \
            || fail "failed to commit version.txt bump"
        if git -C "$SCRIPT_DIR" rev-parse --abbrev-ref '@{upstream}' >/dev/null 2>&1; then
            info "Pushing version bump to $(git -C "$SCRIPT_DIR" rev-parse --abbrev-ref '@{upstream}')..."
            git -C "$SCRIPT_DIR" push || fail "git push failed — resolve and re-run release.sh"
        else
            warn "version bump committed locally but no upstream to push to — tag will not include it"
        fi
    fi
fi

info "Building Skink release v${VERSION} for ${TARGET}"

# ============================================================================
# Build compiler
# ============================================================================

info "Building skink compiler..."
(
    cd "$SCRIPT_DIR"
    make clean
    SKINK_HOME="$SKINK_HOME_DIR" make build
)
COMPILER_BIN="${SCRIPT_DIR}/skink"
[[ -f "$COMPILER_BIN" ]] || fail "Compiler build failed"

# Verify it runs
info "Verifying compiler..."
"$COMPILER_BIN" -v || fail "Compiler verification failed"

# ============================================================================
# Build LSP
# ============================================================================

LSP_BIN=""
FMT_BIN=""
if [[ -d "$LSP_DIR" ]]; then
    info "Building skink-lsp..."
    (
        cd "$LSP_DIR"
        make clean
        # Build against this repo's compiler and stdlib, not any ambient SKINK_HOME
        # (a stale /usr/local/lib/skink export would ship old sources).
        SKINK="$COMPILER_BIN" SKINK_HOME="$SKINK_HOME_DIR" make build
    )
    LSP_BIN="${LSP_DIR}/skink-lsp"
    [[ -f "$LSP_BIN" ]] || fail "LSP build failed"
    FMT_BIN="${LSP_DIR}/skinkfmt"
    [[ -f "$FMT_BIN" ]] || warn "skinkfmt not built, skipping formatter"
    info "Verifying LSP..."
    # LSP reads from stdin, just check it starts and exits on EOF
    echo "" | timeout 2 "$LSP_BIN" 2>/dev/null || true
else
    warn "skink-lsp directory not found at ${LSP_DIR}, skipping LSP"
fi

# ============================================================================
# Build scales
# ============================================================================

SCALES_BIN=""
if [[ -d "$SCALES_DIR" ]]; then
    info "Building scales..."
    (
        cd "$SCALES_DIR"
        make clean
        SKINK="$COMPILER_BIN" SKINK_HOME="$SKINK_HOME_DIR" make build
    )
    SCALES_BIN="${SCALES_DIR}/scales"
    [[ -f "$SCALES_BIN" ]] || fail "scales build failed"
    info "Verifying scales..."
    "$SCALES_BIN" -v || fail "scales verification failed"
else
    warn "scales directory not found at ${SCALES_DIR}, skipping scales"
fi

# ============================================================================
# Build skinkjs
# ============================================================================

SKINKJS_BIN=""
if [[ -d "$SKINKJS_DIR" ]]; then
    info "Building skinkjs..."
    (
        cd "$SKINKJS_DIR"
        make clean
        SKINK="$COMPILER_BIN" SKINK_HOME="$SKINK_HOME_DIR" make build
    )
    SKINKJS_BIN="${SKINKJS_DIR}/skinkjs"
    [[ -f "$SKINKJS_BIN" ]] || fail "skinkjs build failed"
    info "Verifying skinkjs..."
    "$SKINKJS_BIN" -v || fail "skinkjs verification failed"
else
    warn "skinkjs directory not found at ${SKINKJS_DIR}, skipping skinkjs"
fi

# ============================================================================
# Build skinkcov
# ============================================================================

SKINKCOV_BIN=""
if [[ -d "$SKINKCOV_DIR" ]]; then
    info "Building skinkcov..."
    (
        cd "$SKINKCOV_DIR"
        make clean
        SKINK="$COMPILER_BIN" SKINK_HOME="$SKINK_HOME_DIR" make build
    )
    SKINKCOV_BIN="${SKINKCOV_DIR}/skinkcov"
    [[ -f "$SKINKCOV_BIN" ]] || fail "skinkcov build failed"
    info "Verifying skinkcov..."
    # skinkcov has no version flag; running it with no args prints usage
    # and exits 1, so verify on the usage output instead.
    _cov_out="$("$SKINKCOV_BIN" 2>&1 || true)"
    echo "$_cov_out" | grep -q "usage:" || fail "skinkcov verification failed"
else
    warn "skinkcov directory not found at ${SKINKCOV_DIR}, skipping skinkcov"
fi

# ============================================================================
# Package archive
# ============================================================================

ARCHIVE_NAME="skink-${VERSION}-${TARGET}.tar.gz"
STAGING_DIR="$(mktemp -d)"
trap 'rm -rf "$STAGING_DIR"' EXIT

info "Packaging ${ARCHIVE_NAME}..."

# Copy binaries
cp "$COMPILER_BIN" "${STAGING_DIR}/skink"
chmod 755 "${STAGING_DIR}/skink"

if [[ -n "$LSP_BIN" ]] && [[ -f "$LSP_BIN" ]]; then
    cp "$LSP_BIN" "${STAGING_DIR}/skink-lsp"
    chmod 755 "${STAGING_DIR}/skink-lsp"
fi

if [[ -n "$FMT_BIN" ]] && [[ -f "$FMT_BIN" ]]; then
    cp "$FMT_BIN" "${STAGING_DIR}/skinkfmt"
    chmod 755 "${STAGING_DIR}/skinkfmt"
fi

if [[ -n "$SCALES_BIN" ]] && [[ -f "$SCALES_BIN" ]]; then
    cp "$SCALES_BIN" "${STAGING_DIR}/scales"
    chmod 755 "${STAGING_DIR}/scales"
fi

if [[ -n "$SKINKJS_BIN" ]] && [[ -f "$SKINKJS_BIN" ]]; then
    cp "$SKINKJS_BIN" "${STAGING_DIR}/skinkjs"
    chmod 755 "${STAGING_DIR}/skinkjs"
fi

if [[ -n "$SKINKCOV_BIN" ]] && [[ -f "$SKINKCOV_BIN" ]]; then
    cp "$SKINKCOV_BIN" "${STAGING_DIR}/skinkcov"
    chmod 755 "${STAGING_DIR}/skinkcov"
fi

# skink -t / -b resolve their harness scripts relative to argv[0], so they
# must sit beside the installed compiler binary.
for harness in skink_test.sh skink_bench.sh; do
    if [[ -f "${SCRIPT_DIR}/${harness}" ]]; then
        cp "${SCRIPT_DIR}/${harness}" "${STAGING_DIR}/${harness}"
        chmod 755 "${STAGING_DIR}/${harness}"
    fi
done

# Copy standard library (includes runtime/ subdirectory with C files)
cp -r "${SKINK_HOME_DIR}" "${STAGING_DIR}/std"
# Remove test files, test fixtures, and stray sync-tool temp files from
# the distributed stdlib
find "${STAGING_DIR}/std" -name '*_test.skink' -delete
find "${STAGING_DIR}/std" -name '__skink_test_*' -delete
find "${STAGING_DIR}/std" -name '*.insyncdl' -delete
rm -rf "${STAGING_DIR}/std/fixture_libmain" "${STAGING_DIR}/std/diamond" \
       "${STAGING_DIR}/std/compilefail" "${STAGING_DIR}/std/regress_helper.skink"

# Create output dir and archive
mkdir -p "$OUTPUT_DIR"
ARCHIVE_PATH="${OUTPUT_DIR}/${ARCHIVE_NAME}"
tar -czf "$ARCHIVE_PATH" -C "$STAGING_DIR" .

info "Created: ${ARCHIVE_PATH}"
info "  Size: $(du -h "$ARCHIVE_PATH" | cut -f1)"
info "  Contents:"
# tar|head can SIGPIPE under pipefail when the archive is large —
# capture the listing first, then display.
_archive_listing="$(tar -tzf "$ARCHIVE_PATH")"
echo "$_archive_listing" | head -5
echo "  ..."
info "  Total files: $(echo "$_archive_listing" | wc -l | tr -d ' ')"

# ============================================================================
# Generate SHA256 checksum
# ============================================================================

CHECKSUM_PATH="${ARCHIVE_PATH}.sha256"
sha256sum "$ARCHIVE_PATH" | awk '{print $1"  '"$(basename "$ARCHIVE_PATH")"'"}' > "$CHECKSUM_PATH"
info "Checksum: ${CHECKSUM_PATH}"

# ============================================================================
# Cross-build the linux-arm64 (Raspberry Pi) archive
# ============================================================================
# Delegates to build_pi_dist.sh, which reuses the just-built ./skink to emit
# target-agnostic LLVM IR and cross-links it against an aarch64 sysroot
# assembled from a Raspberry Pi OS image + Debian bookworm -dev packages.
# Linux hosts only — the image/sysroot tooling (fdisk, debugfs, ar) is
# Linux-specific.

ARM64_ARCHIVE_PATH=""
if [[ "$SKIP_ARM64" -eq 0 && "$OS" == "linux" && "$TARGET_OS" == "linux" ]]; then
    if [[ -f "${SCRIPT_DIR}/build_pi_dist.sh" ]]; then
        info "Building linux-arm64 (Raspberry Pi) archive..."
        ARM64_ARGS=(--version "$VERSION" --output-dir "$OUTPUT_DIR")
        [[ -n "$PI_IMAGE" ]] && ARM64_ARGS+=(--image "$PI_IMAGE")
        bash "${SCRIPT_DIR}/build_pi_dist.sh" "${ARM64_ARGS[@]}"
        ARM64_ARCHIVE_PATH="${OUTPUT_DIR}/skink-${VERSION}-linux-arm64.tar.gz"
        [[ -f "$ARM64_ARCHIVE_PATH" ]] \
            || fail "arm64 build did not produce ${ARM64_ARCHIVE_PATH}"
    else
        warn "build_pi_dist.sh not found — skipping linux-arm64 archive"
    fi
elif [[ "$SKIP_ARM64" -eq 1 ]]; then
    info "Skipping linux-arm64 archive (--skip-arm64)"
fi

# ============================================================================
# Cross-build the Windows archives (x86_64 + aarch64)
# ============================================================================
# Delegates to build_windows.sh, which cross-compiles the whole toolset
# (skink, scales, skink-lsp, skinkfmt) with MinGW/llvm-mingw, bundles the
# required DLLs, the .cmd/.sh test and bench launchers, the stdlib, and
# the Linux sysroot for Windows→Linux cross builds — emitting
# dist/windows/skink-<ver>-windows-<arch>.zip.

WIN_X64_ZIP=""
WIN_A64_ZIP=""
if [[ "$SKIP_WINDOWS" -eq 0 && "$OS" == "linux" ]]; then
    if [[ -f "${SCRIPT_DIR}/build_windows.sh" ]]; then
        for warch in x86_64 aarch64; do
            info "Building Windows ${warch} archive..."
            SKINK="$COMPILER_BIN" bash "${SCRIPT_DIR}/build_windows.sh" "$VERSION" "$warch"
        done
        WIN_X64_ZIP="${SCRIPT_DIR}/dist/windows/skink-${VERSION}-windows-x86_64.zip"
        WIN_A64_ZIP="${SCRIPT_DIR}/dist/windows/skink-${VERSION}-windows-aarch64.zip"
        [[ -f "$WIN_X64_ZIP" ]] \
            || fail "Windows x86_64 build did not produce ${WIN_X64_ZIP}"
        [[ -f "$WIN_A64_ZIP" ]] \
            || fail "Windows aarch64 build did not produce ${WIN_A64_ZIP}"
    else
        warn "build_windows.sh not found — skipping Windows archives"
    fi
elif [[ "$SKIP_WINDOWS" -eq 1 ]]; then
    info "Skipping Windows archives (--skip-windows)"
fi

# ============================================================================
# Publish to GitHub releases
# ============================================================================
# Enabled by default (--no-publish / PUBLISH=0 to opt out). Creates the
# v<version> release — gh auto-creates the tag on the remote's default
# branch HEAD — or, for a rebuild of an existing version, replaces the
# assets in place with `gh release upload --clobber`.

ARTIFACTS=("$ARCHIVE_PATH" "$CHECKSUM_PATH")
if [[ -n "$ARM64_ARCHIVE_PATH" ]]; then
    ARTIFACTS+=("$ARM64_ARCHIVE_PATH" "${ARM64_ARCHIVE_PATH}.sha256")
fi
if [[ -n "$WIN_X64_ZIP" ]]; then
    ARTIFACTS+=("$WIN_X64_ZIP" "${WIN_X64_ZIP}.sha256")
fi
if [[ -n "$WIN_A64_ZIP" ]]; then
    ARTIFACTS+=("$WIN_A64_ZIP" "${WIN_A64_ZIP}.sha256")
fi

TAG="v${VERSION}"
GH_REPO=""
PUBLISH_OK=0

if [[ "$PUBLISH" -eq 1 ]]; then
    if ! command -v gh >/dev/null 2>&1; then
        warn "gh CLI not found — skipping GitHub publish (install gh to enable)"
    elif ! gh auth status >/dev/null 2>&1; then
        warn "gh is not authenticated — skipping publish (run 'gh auth login')"
    else
        # Resolve owner/repo from this repo's own remote so the release
        # lands on the right project regardless of the invoking directory.
        GH_REPO="$(git -C "$SCRIPT_DIR" remote get-url origin 2>/dev/null \
            | sed -E 's#(git@github\.com:|https://github\.com/)##; s#\.git$##')"
        [[ -n "$GH_REPO" ]] || fail "could not determine GitHub repo from ${SCRIPT_DIR}'s origin remote"

        # gh creates the tag at the remote's pushed HEAD — warn loudly if
        # local work would not be part of the tagged code.
        if [[ -n "$(git -C "$SCRIPT_DIR" status --porcelain 2>/dev/null)" ]]; then
            warn "working tree has uncommitted changes — the ${TAG} tag will NOT include them"
        fi
        UPSTREAM="$(git -C "$SCRIPT_DIR" rev-parse --abbrev-ref '@{upstream}' 2>/dev/null || true)"
        if [[ -n "$UPSTREAM" ]] && \
           [[ -n "$(git -C "$SCRIPT_DIR" log --oneline "${UPSTREAM}..HEAD" 2>/dev/null)" ]]; then
            warn "local commits are not pushed — the ${TAG} tag will NOT include them"
        fi

        if [[ "$DRY_RUN" -eq 1 ]]; then
            info "[dry-run] gh release view ${TAG} -R ${GH_REPO}"
            info "[dry-run] gh release upload ${TAG} ${ARTIFACTS[*]} -R ${GH_REPO} --clobber"
            info "[dry-run] gh release create ${TAG} ${ARTIFACTS[*]} -R ${GH_REPO} --title \"Skink v${VERSION}\" --generate-notes"
            PUBLISH_OK=1
        elif gh release view "$TAG" -R "$GH_REPO" >/dev/null 2>&1; then
            info "Release ${TAG} exists — replacing assets..."
            gh release upload "$TAG" "${ARTIFACTS[@]}" -R "$GH_REPO" --clobber
            PUBLISH_OK=1
        else
            info "Creating release ${TAG} on ${GH_REPO}..."
            gh release create "$TAG" "${ARTIFACTS[@]}" -R "$GH_REPO" \
                --title "Skink v${VERSION}" --generate-notes \
            || gh release create "$TAG" "${ARTIFACTS[@]}" -R "$GH_REPO" \
                --title "Skink v${VERSION}" --notes "Release v${VERSION}"
            PUBLISH_OK=1
        fi
    fi
else
    info "Skipping GitHub publish (--no-publish)"
fi

# ============================================================================
# Done
# ============================================================================

echo ""
if [[ "$PUBLISH_OK" -eq 1 && "$DRY_RUN" -eq 0 ]]; then
    info "Release v${VERSION} built and published."
elif [[ "$PUBLISH_OK" -eq 1 ]]; then
    info "Release v${VERSION} built (publish dry-run only)."
else
    info "Release v${VERSION} built successfully!"
fi
echo ""
echo "  Archive:  ${ARCHIVE_PATH}"
echo "  Checksum: ${CHECKSUM_PATH}"
if [[ -n "$ARM64_ARCHIVE_PATH" ]]; then
echo "  Archive:  ${ARM64_ARCHIVE_PATH}  (linux-arm64 / Raspberry Pi)"
echo "  Checksum: ${ARM64_ARCHIVE_PATH}.sha256"
fi
if [[ -n "$WIN_X64_ZIP" ]]; then
echo "  Archive:  ${WIN_X64_ZIP}  (windows-x86_64)"
echo "  Checksum: ${WIN_X64_ZIP}.sha256"
fi
if [[ -n "$WIN_A64_ZIP" ]]; then
echo "  Archive:  ${WIN_A64_ZIP}  (windows-aarch64)"
echo "  Checksum: ${WIN_A64_ZIP}.sha256"
fi
echo ""
if [[ "$PUBLISH_OK" -eq 1 && "$DRY_RUN" -eq 0 ]]; then
    gh release view "$TAG" -R "$GH_REPO" --json url -q .url 2>/dev/null || true
elif [[ "$PUBLISH_OK" -ne 1 ]]; then
    echo "Publish manually with:"
    echo "  gh release create ${TAG} ${ARTIFACTS[*]} \\"
    echo "    --title \"Skink v${VERSION}\" --generate-notes"
fi
echo ""
