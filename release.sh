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
# release.sh — Build and package a Skink release archive for GitHub
#
# This script builds the skink compiler and skink-lsp, then packages them
# with the standard library and C runtime into a tarball ready for upload
# to GitHub releases.
#
# Usage:
#   ./release.sh                    # uses version from version.txt
#   ./release.sh 0.2.0              # explicit version
#   ./release.sh --cross linux-x86_64  # cross-compile target
#

set -euo pipefail

# ============================================================================
# Configuration
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKINK_HOME_DIR="${SCRIPT_DIR}/std"
LSP_DIR="${SCRIPT_DIR}/../skink-lsp"

# Version
VERSION=""
if [[ $# -ge 1 ]] && [[ "$1" != --* ]]; then
    VERSION="$1"
    shift
else
    VERSION="$(cat "${SCRIPT_DIR}/version.txt" 2>/dev/null || echo "0.0.0")"
fi

# Strip leading 'v'
VERSION="${VERSION#v}"

# Output directory for archives
OUTPUT_DIR="${SCRIPT_DIR}/dist"

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
        --help|-h)
            cat <<EOF
Skink release packager — builds and packages the toolset for distribution.

Usage:
  $0 [version] [options]

Arguments:
  version            Version string (default: from version.txt)

Options:
  --cross <target>   Cross-compile target (e.g. linux-x86_64, darwin-arm64)
  --output-dir <dir> Where to write the archive (default: ./dist)
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
if [[ -d "$LSP_DIR" ]]; then
    info "Building skink-lsp..."
    (
        cd "$LSP_DIR"
        make clean
        make build
    )
    LSP_BIN="${LSP_DIR}/skink-lsp"
    [[ -f "$LSP_BIN" ]] || fail "LSP build failed"
    info "Verifying LSP..."
    # LSP reads from stdin, just check it starts and exits on EOF
    echo "" | timeout 2 "$LSP_BIN" 2>/dev/null || true
else
    warn "skink-lsp directory not found at ${LSP_DIR}, skipping LSP"
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

# Copy standard library (includes runtime/ subdirectory with C files)
cp -r "${SKINK_HOME_DIR}" "${STAGING_DIR}/std"
# Remove test files from the distributed stdlib
find "${STAGING_DIR}/std" -name '*_test.skink' -delete
find "${STAGING_DIR}/std" -name '__skink_test_bin' -delete

# Create output dir and archive
mkdir -p "$OUTPUT_DIR"
ARCHIVE_PATH="${OUTPUT_DIR}/${ARCHIVE_NAME}"
tar -czf "$ARCHIVE_PATH" -C "$STAGING_DIR" .

info "Created: ${ARCHIVE_PATH}"
info "  Size: $(du -h "$ARCHIVE_PATH" | cut -f1)"
info "  Contents:"
tar -tzf "$ARCHIVE_PATH" | head -5
echo "  ..."
info "  Total files: $(tar -tzf "$ARCHIVE_PATH" | wc -l | tr -d ' ')"

# ============================================================================
# Generate SHA256 checksum
# ============================================================================

CHECKSUM_PATH="${ARCHIVE_PATH}.sha256"
sha256sum "$ARCHIVE_PATH" | awk '{print $1"  '"$(basename "$ARCHIVE_PATH")"'"}' > "$CHECKSUM_PATH"
info "Checksum: ${CHECKSUM_PATH}"

# ============================================================================
# Done
# ============================================================================

echo ""
info "Release v${VERSION} for ${TARGET} built successfully!"
echo ""
echo "  Archive:  ${ARCHIVE_PATH}"
echo "  Checksum: ${CHECKSUM_PATH}"
echo ""
echo "Upload to GitHub with:"
echo "  gh release create v${VERSION} \\"
echo "    ${ARCHIVE_PATH} \\"
echo "    ${CHECKSUM_PATH} \\"
echo "    --title \"Skink v${VERSION}\" \\"
echo "    --notes \"Release v${VERSION}\""
echo ""
