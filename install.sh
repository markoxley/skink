#!/usr/bin/env bash
# Copyright 2026 Mark Oxley. All rights reserved.
#
# Project: Skink — A self-hosted, statically-typed programming language
#          targeting LLVM IR, designed for systems programming with
#          modern ergonomics and a Go-inspired syntax.
#
# File:    install.sh
# Purpose: install.sh — Install the Skink compiler toolset from GitHub releases Usage: curl -fsSL https://raw.githubusercontent.com...
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
# install.sh — Install the Skink compiler toolset from GitHub releases
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/<repo>/main/install.sh | bash
#   curl -fsSL https://raw.githubusercontent.com/<repo>/main/install.sh | bash -s -- --version 0.2.0
#   curl -fsSL https://raw.githubusercontent.com/<repo>/main/install.sh | bash -s -- --prefix /opt/skink
#

set -euo pipefail

# ============================================================================
# Configuration (override with flags or environment variables)
# ============================================================================

# GitHub repo that hosts releases. Override with SKINK_REPO env var.
# Format: "owner/repo"
SKINK_REPO="${SKINK_REPO:-markoxley/skink}"

# Install prefix. Override with --prefix or SKINK_PREFIX env var.
SKINK_PREFIX="${SKINK_PREFIX:-$HOME/.local/skink}"

# Version to install. "latest" fetches the most recent release.
# Override with --version or SKINK_VERSION env var.
SKINK_VERSION="${SKINK_VERSION:-latest}"

# Whether to update shell config files. Set to 0 to skip.
UPDATE_SHELL_RC="${UPDATE_SHELL_RC:-1}"

# ============================================================================
# Helpers
# ============================================================================

info()  { printf "\033[1;34m==>\033[0m %s\n" "$*"; }
warn()  { printf "\033[1;33mwarn:\033[0m %s\n" "$*"; }
error() { printf "\033[1;31merror:\033[0m %s\n" "$*" >&2; }
fail()  { error "$*"; exit 1; }

# ============================================================================
# Parse arguments
# ============================================================================

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)
            SKINK_VERSION="$2"
            shift 2
            ;;
        --prefix)
            SKINK_PREFIX="$2"
            shift 2
            ;;
        --repo)
            SKINK_REPO="$2"
            shift 2
            ;;
        --no-shell-rc)
            UPDATE_SHELL_RC=0
            shift
            ;;
        --help|-h)
            cat <<EOF
Skink installer — installs the Skink compiler toolset from GitHub releases.

Usage:
  $0 [options]

Options:
  --version <ver>    Version to install (default: latest)
  --prefix <path>    Install prefix (default: ~/.local/skink)
  --repo <owner/repo>  GitHub repo hosting releases (default: markoxley/skink)
  --no-shell-rc      Skip updating shell config files
  --help             Show this help message

Environment variables:
  SKINK_REPO         Same as --repo
  SKINK_PREFIX       Same as --prefix
  SKINK_VERSION      Same as --version
  UPDATE_SHELL_RC    Set to 0 to skip shell config updates
EOF
            exit 0
            ;;
        *)
            fail "Unknown option: $1 (use --help for usage)"
            ;;
    esac
done

# ============================================================================
# Detect platform
# ============================================================================

OS="$(uname -s)"
ARCH="$(uname -m)"

case "$OS" in
    Linux)  OS="linux" ;;
    Darwin) OS="darwin" ;;
    *)      fail "Unsupported OS: $OS (only Linux and macOS are supported)" ;;
esac

case "$ARCH" in
    x86_64|amd64) ARCH="x86_64" ;;
    aarch64|arm64) ARCH="arm64" ;;
    *)             fail "Unsupported architecture: $ARCH (only x86_64 and arm64 are supported)" ;;
esac

info "Detected platform: ${OS}/${ARCH}"

# ============================================================================
# Resolve version
# ============================================================================

if [[ "$SKINK_VERSION" == "latest" ]]; then
    info "Fetching latest release version from GitHub..."
    SKINK_VERSION="$(curl -fsSL \
        "https://api.github.com/repos/${SKINK_REPO}/releases/latest" \
        | grep '"tag_name"' \
        | head -1 \
        | sed -E 's/.*"tag_name"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/')"
    [[ -z "$SKINK_VERSION" ]] && fail "Could not determine latest release version"
fi

# Strip leading 'v' if present
SKINK_VERSION="${SKINK_VERSION#v}"

info "Installing Skink version ${SKINK_VERSION}"

# ============================================================================
# Download archive
# ============================================================================

ARCHIVE_NAME="skink-${SKINK_VERSION}-${OS}-${ARCH}.tar.gz"
DOWNLOAD_URL="https://github.com/${SKINK_REPO}/releases/download/v${SKINK_VERSION}/${ARCHIVE_NAME}"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

ARCHIVE_PATH="${TMPDIR}/${ARCHIVE_NAME}"

info "Downloading ${ARCHIVE_NAME}..."
info "  ${DOWNLOAD_URL}"

curl -fsSL -o "$ARCHIVE_PATH" "$DOWNLOAD_URL" \
    || fail "Download failed. Check that release v${SKINK_VERSION} exists for ${OS}/${ARCH}."

# Verify the SHA256 checksum published alongside the archive. A missing
# checksum file warns rather than fails so ad-hoc releases still install.
if command -v sha256sum >/dev/null 2>&1; then
    if curl -fsSL -o "${ARCHIVE_PATH}.sha256" "${DOWNLOAD_URL}.sha256"; then
        info "Verifying checksum..."
        (cd "$TMPDIR" && sha256sum -c "${ARCHIVE_NAME}.sha256") \
            || fail "Checksum verification failed for ${ARCHIVE_NAME}"
    else
        warn "No checksum file found for v${SKINK_VERSION}; skipping verification"
    fi
fi

# ============================================================================
# Install
# ============================================================================

info "Installing to ${SKINK_PREFIX}..."

mkdir -p "$SKINK_PREFIX"

# Extract into a temp staging dir, then move into place
STAGING="${TMPDIR}/staging"
mkdir -p "$STAGING"
tar -xzf "$ARCHIVE_PATH" -C "$STAGING"

# Expected archive layout:
#   skink              (compiler binary)
#   skink-lsp          (LSP binary)
#   skinkfmt           (source formatter binary)
#   skink_test.sh      (test runner invoked by `skink -t`)
#   skink_bench.sh     (benchmark runner invoked by `skink -b`)
#   std/               (standard library .skink files)
#   runtime/           (C runtime files)

# Install binaries
mkdir -p "${SKINK_PREFIX}/bin"
if [[ -f "${STAGING}/skink" ]]; then
    cp -f "${STAGING}/skink" "${SKINK_PREFIX}/bin/"
    chmod 755 "${SKINK_PREFIX}/bin/skink"
    info "  Installed: bin/skink"
else
    warn "  skink binary not found in archive"
fi

if [[ -f "${STAGING}/skink-lsp" ]]; then
    cp -f "${STAGING}/skink-lsp" "${SKINK_PREFIX}/bin/"
    chmod 755 "${SKINK_PREFIX}/bin/skink-lsp"
    info "  Installed: bin/skink-lsp"
else
    warn "  skink-lsp binary not found in archive (LSP support optional)"
fi

if [[ -f "${STAGING}/skinkfmt" ]]; then
    cp -f "${STAGING}/skinkfmt" "${SKINK_PREFIX}/bin/"
    chmod 755 "${SKINK_PREFIX}/bin/skinkfmt"
    info "  Installed: bin/skinkfmt"
fi

for harness in skink_test.sh skink_bench.sh; do
    if [[ -f "${STAGING}/${harness}" ]]; then
        cp -f "${STAGING}/${harness}" "${SKINK_PREFIX}/bin/"
        chmod 755 "${SKINK_PREFIX}/bin/${harness}"
        info "  Installed: bin/${harness}"
    fi
done

# Install standard library (includes runtime/ subdirectory with C files)
if [[ -d "${STAGING}/std" ]]; then
    rm -rf "${SKINK_PREFIX}/std"
    cp -r "${STAGING}/std" "${SKINK_PREFIX}/std"
    info "  Installed: std/ ($(find "${SKINK_PREFIX}/std" -name '*.skink' | wc -l | tr -d ' ') modules, $(find "${SKINK_PREFIX}/std/runtime" -name '*.c' 2>/dev/null | wc -l | tr -d ' ') runtime files)"
else
    warn "  std/ directory not found in archive"
fi

# ============================================================================
# Update shell config
# ============================================================================

SKINK_BIN_DIR="${SKINK_PREFIX}/bin"
SKINK_HOME_DIR="${SKINK_PREFIX}/std"

add_to_rc() {
    local rc_file="$1"
    local shell_name="$2"

    # Check if already configured
    if [[ -f "$rc_file" ]] && grep -q "SKINK_HOME" "$rc_file" 2>/dev/null; then
        info "Shell config already has Skink paths: ${rc_file}"
        return 0
    fi

    info "Updating ${rc_file}..."

    {
        echo ""
        echo "# Skink compiler toolset"
        echo "export SKINK_HOME=\"${SKINK_HOME_DIR}\""
        echo "export PATH=\"${SKINK_BIN_DIR}:\$PATH\""
    } >> "$rc_file"
}

if [[ "$UPDATE_SHELL_RC" -eq 1 ]]; then
    # Detect which shell config files exist and update them
    UPDATED_ANY=0

    # Bash
    if [[ -f "$HOME/.bashrc" ]]; then
        add_to_rc "$HOME/.bashrc" "bash"
        UPDATED_ANY=1
    fi
    if [[ -f "$HOME/.bash_profile" ]]; then
        add_to_rc "$HOME/.bash_profile" "bash"
        UPDATED_ANY=1
    fi

    # Zsh
    if [[ -f "$HOME/.zshrc" ]]; then
        add_to_rc "$HOME/.zshrc" "zsh"
        UPDATED_ANY=1
    fi

    # Fish
    FISH_CONFIG="$HOME/.config/fish/config.fish"
    if [[ -f "$FISH_CONFIG" ]]; then
        if ! grep -q "SKINK_HOME" "$FISH_CONFIG" 2>/dev/null; then
            info "Updating ${FISH_CONFIG}..."
            {
                echo ""
                echo "# Skink compiler toolset"
                echo "set -gx SKINK_HOME \"${SKINK_HOME_DIR}\""
                echo "set -gx PATH \"${SKINK_BIN_DIR}\" \$PATH"
            } >> "$FISH_CONFIG"
            UPDATED_ANY=1
        else
            info "Shell config already has Skink paths: ${FISH_CONFIG}"
        fi
    fi

    # Generic .profile (fallback)
    if [[ "$UPDATED_ANY" -eq 0 ]] && [[ -f "$HOME/.profile" ]]; then
        add_to_rc "$HOME/.profile" "sh"
        UPDATED_ANY=1
    fi

    if [[ "$UPDATED_ANY" -eq 0 ]]; then
        warn "No shell config file found (.bashrc, .zshrc, .profile, config.fish)."
        warn "Add these lines to your shell config manually:"
        echo ""
        echo "  export SKINK_HOME=\"${SKINK_HOME_DIR}\""
        echo "  export PATH=\"${SKINK_BIN_DIR}:\$PATH\""
        echo ""
    fi
fi

# ============================================================================
# Done
# ============================================================================

echo ""
info "Skink ${SKINK_VERSION} installed successfully!"
echo ""
echo "  Binary:     ${SKINK_BIN_DIR}/skink"
if [[ -f "${SKINK_BIN_DIR}/skink-lsp" ]]; then
    echo "  LSP:        ${SKINK_BIN_DIR}/skink-lsp"
fi
echo "  Stdlib:     ${SKINK_HOME_DIR}"
echo "  Runtime:    ${SKINK_HOME_DIR}/runtime"
echo ""

if [[ "$UPDATE_SHELL_RC" -eq 1 ]]; then
    echo "Restart your shell or run:"
    echo "  source ~/.bashrc   (or ~/.zshrc, etc.)"
    echo ""
fi

echo "Verify with:"
echo "  skink -v"
echo ""
