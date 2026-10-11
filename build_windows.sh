#!/usr/bin/env bash
# Copyright 2026 Mark Oxley. All rights reserved.
#
# build_windows.sh — Cross-compile the Skink compiler for Windows x86_64.
#
# This script:
#   1. Emits LLVM IR from skink.skink using the host (Linux) Skink compiler
#   2. Cross-compiles the IR + runtime C files to a Windows .exe using clang
#   3. Bundles the .exe with std/, runtime/, DLLs, and install.ps1 into a ZIP
#
# Requirements (Fedora):
#   sudo dnf install -y mingw64-gcc mingw64-headers mingw64-winpthreads \
#     mingw64-openssl mingw64-zlib mingw64-sqlite mingw64-libgcc
#
# Usage:
#   ./build_windows.sh [version]
#
# If version is not specified, it reads from version.txt.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Version
VERSION="${1:-$(cat version.txt 2>/dev/null || echo "0.0.0")}"
VERSION="${VERSION#v}"

# Configuration
TARGET="x86_64-w64-mingw32"
# MinGW sysroot: check system install first, then local extraction
MINGW_SYSROOT=""
for candidate in \
    "/usr/x86_64-w64-mingw32/sys-root/mingw" \
    "/tmp/mingw-sysroot/usr/x86_64-w64-mingw32/sys-root/mingw" \
    "$HOME/mingw-sysroot/usr/x86_64-w64-mingw32/sys-root/mingw"; do
    if [[ -d "$candidate" ]] && [[ -f "$candidate/lib/crt2.o" ]]; then
        MINGW_SYSROOT="$candidate"
        break
    fi
done
# GCC library path (for libgcc, libgcc_eh)
GCC_LIB=""
for candidate in \
    "/usr/lib/gcc/x86_64-w64-mingw32"/*/ \
    "/tmp/mingw-sysroot/usr/lib/gcc/x86_64-w64-mingw32"/*/; do
    if [[ -d "$candidate" ]] && [[ -f "${candidate}libgcc.a" ]]; then
        GCC_LIB="${candidate%/}"
        break
    fi
done
OUTPUT_DIR="${SCRIPT_DIR}/dist/windows"
STAGING_DIR="$(mktemp -d)"
trap 'rm -rf "$STAGING_DIR"' EXIT

# Helpers
info()  { printf "\033[1;34m==>\033[0m %s\n" "$*"; }
warn()  { printf "\033[1;33mwarn:\033[0m %s\n" "$*"; }
error() { printf "\033[1;31merror:\033[0m %s\n" "$*" >&2; }
fail()  { error "$*"; exit 1; }

info "Building Skink v${VERSION} for Windows x86_64 (${TARGET})"

# ============================================================================
# Step 1: Verify MinGW toolchain
# ============================================================================

info "Checking MinGW toolchain..."
if [[ ! -d "$MINGW_SYSROOT" ]]; then
    fail "MinGW sysroot not found at $MINGW_SYSROOT. Install with:
  sudo dnf install -y mingw64-gcc mingw64-headers mingw64-winpthreads mingw64-openssl mingw64-zlib mingw64-sqlite mingw64-libgcc"
fi

# Verify clang supports the target
if ! clang --target=$TARGET --version >/dev/null 2>&1; then
    fail "clang does not support target $TARGET"
fi

info "  MinGW sysroot: $MINGW_SYSROOT"
info "  clang target:  $TARGET"

# ============================================================================
# Step 2: Emit LLVM IR from skink.skink
# ============================================================================

info "Emitting LLVM IR from skink.skink..."
SKINK_HOME="${SCRIPT_DIR}/std" ./skink -emit-ll skink.skink > "${STAGING_DIR}/skink.ll" 2>/dev/null
[[ -s "${STAGING_DIR}/skink.ll" ]] || fail "Failed to emit LLVM IR"

LL_LINES=$(wc -l < "${STAGING_DIR}/skink.ll")
info "  Emitted ${LL_LINES} lines of LLVM IR"

# ============================================================================
# Step 3: Cross-compile to Windows .exe
# ============================================================================

info "Cross-compiling skink.exe..."

# Runtime C files to compile (same list as in skink.skink)
RUNTIME_FILES=""
for f in rc_rt.c fs_rt.c regex_rt.c sync_rt.c tensor_rt.c compress_rt.c \
         conc_rt.c db_rt.c reflect_rt.c rules_rt.c panic_rt.c net_rt.c \
         llm_rt.c mqtt_rt.c tls_rt.c; do
    RUNTIME_FILES="${RUNTIME_FILES} ${SCRIPT_DIR}/std/runtime/${f}"
done

# Template bridge LLVM IR
RUNTIME_FILES="${RUNTIME_FILES} ${SCRIPT_DIR}/std/runtime/template_bridge.ll"

# Cross-compile flags
CROSS_FLAGS=(
    --target=$TARGET
    -Wno-override-module
    -O2
    -I"$MINGW_SYSROOT/include"
    -I"${SCRIPT_DIR}/std/runtime"
    -L"$MINGW_SYSROOT/lib"
    -o "${STAGING_DIR}/skink.exe"
    "${STAGING_DIR}/skink.ll"
    ${RUNTIME_FILES}
    -lwinpthread
    -lws2_32
    -lssl
    -lcrypto
    -lz
    -lsqlite3
    -lbcrypt
)
if [[ -n "$GCC_LIB" ]]; then
    CROSS_FLAGS+=(-L"$GCC_LIB")
fi

clang "${CROSS_FLAGS[@]}" || fail "Cross-compilation failed"

# Verify the output
if ! file "${STAGING_DIR}/skink.exe" | grep -q "PE32+"; then
    fail "Output is not a Windows PE32+ executable: $(file "${STAGING_DIR}/skink.exe")"
fi

info "  Built: skink.exe ($(du -h "${STAGING_DIR}/skink.exe" | cut -f1))"

# ============================================================================
# Step 4: Bundle DLLs
# ============================================================================

info "Bundling Windows DLLs..."

# Find and copy required DLLs from the MinGW sysroot
DLLS=(
    libwinpthread-1.dll
    libssl-3-x64.dll
    libcrypto-3-x64.dll
    libsqlite3-0.dll
    zlib1.dll
    libgcc_s_seh-1.dll
    libstdc++-6.dll
)

for dll in "${DLLS[@]}"; do
    found=""
    # Search in sysroot bin and lib
    for dir in "$MINGW_SYSROOT/bin" "$MINGW_SYSROOT/lib" "/usr/x86_64-w64-mingw32/bin" "/tmp/mingw-sysroot/usr/x86_64-w64-mingw32/sys-root/mingw/bin"; do
        if [[ -f "$dir/$dll" ]]; then
            found="$dir/$dll"
            break
        fi
    done
    if [[ -n "$found" ]]; then
        cp "$found" "${STAGING_DIR}/"
        info "  Bundled: $dll"
    else
        warn "  DLL not found: $dll (may need to be installed separately on the target)"
    fi
done

# ============================================================================
# Step 5: Copy standard library and runtime
# ============================================================================

info "Copying standard library and runtime..."

# Copy std/ (Skink source modules)
cp -r "${SCRIPT_DIR}/std" "${STAGING_DIR}/std"
# Remove test files from the distributed stdlib
find "${STAGING_DIR}/std" -name '*_test.skink' -delete
find "${STAGING_DIR}/std" -name '__skink_test_bin' -delete
# Keep the runtime/ subdirectory with C files and the compat header
info "  std/: $(find "${STAGING_DIR}/std" -name '*.skink' | wc -l | tr -d ' ') modules"
info "  runtime/: $(find "${STAGING_DIR}/std/runtime" -name '*.c' | wc -l | tr -d ' ') C files"

# Copy version files
cp "${SCRIPT_DIR}/version.txt" "${STAGING_DIR}/" 2>/dev/null || true

# ============================================================================
# Step 6: Create install.ps1
# ============================================================================

info "Creating install.ps1..."
cat > "${STAGING_DIR}/install.ps1" << 'PSEOF'
# Skink installer for Windows
# Usage: powershell -ExecutionPolicy Bypass -File install.ps1 [-InstallDir <path>]
param(
    [string]$InstallDir = "$env:LOCALAPPDATA\Skink"
)

$ErrorActionPreference = "Stop"

Write-Host "==> Installing Skink to $InstallDir" -ForegroundColor Cyan

# Create install directories
$BinDir = Join-Path $InstallDir "bin"
$StdDir = Join-Path $InstallDir "std"
$RuntimeDir = Join-Path $StdDir "runtime"

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
New-Item -ItemType Directory -Force -Path $BinDir | Out-Null
New-Item -ItemType Directory -Force -Path $StdDir | Out-Null

# Copy skink.exe and DLLs to bin/
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Copy-Item "$ScriptDir\skink.exe" $BinDir -Force
Write-Host "  Installed: bin\skink.exe"

# Copy DLLs
Get-ChildItem "$ScriptDir\*.dll" | ForEach-Object {
    Copy-Item $_.FullName $BinDir -Force
    Write-Host "  Installed: bin\$($_.Name)"
}

# Copy std/ directory (includes runtime/)
if (Test-Path "$ScriptDir\std") {
    if (Test-Path $StdDir) {
        Remove-Item $StdDir -Recurse -Force
    }
    Copy-Item "$ScriptDir\std" $StdDir -Recurse -Force
    $moduleCount = (Get-ChildItem $StdDir -Filter "*.skink" -Recurse).Count
    Write-Host "  Installed: std\ ($moduleCount modules)"
}

# Set environment variables for the current user
$envKey = [Environment]::GetEnvironmentVariable("SKINK_HOME", "User")
if ($envKey -ne $StdDir) {
    [Environment]::SetEnvironmentVariable("SKINK_HOME", $StdDir, "User")
    Write-Host "  Set SKINK_HOME = $StdDir"
}

# Add bin to PATH (user-level)
$currentPath = [Environment]::GetEnvironmentVariable("PATH", "User")
if ($currentPath -notlike "*$BinDir*") {
    $newPath = "$BinDir;$currentPath"
    [Environment]::SetEnvironmentVariable("PATH", $newPath, "User")
    Write-Host "  Added $BinDir to PATH"
}

Write-Host ""
Write-Host "Skink installed successfully!" -ForegroundColor Green
Write-Host ""
Write-Host "  Binary:  $BinDir\skink.exe"
Write-Host "  Stdlib:  $StdDir"
Write-Host "  Runtime: $RuntimeDir"
Write-Host ""
Write-Host "Note: You may need to open a new terminal for PATH changes to take effect."
Write-Host ""
Write-Host "Prerequisites: LLVM/Clang must be installed and on PATH for Skink to compile programs."
Write-Host "  Install from: https://releases.llvm.org/ or via winget: winget install LLVM.LLVM"
Write-Host ""
Write-Host "Verify with:"
Write-Host "  skink -v"
Write-Host ""
PSEOF

info "  Created: install.ps1"

# ============================================================================
# Step 7: Create README.txt
# ============================================================================

cat > "${STAGING_DIR}/README.txt" << READMEEOF
Skink Compiler v${VERSION} for Windows x86_64
===============================================

Contents:
  skink.exe       — The Skink compiler binary
  *.dll           — Required runtime DLLs (winpthread, OpenSSL, zlib, SQLite)
  std/            — Standard library (.skink source modules)
  std/runtime/    — C runtime source files (compiled into every Skink program)
  install.ps1     — PowerShell installer script
  README.txt      — This file

Installation:
  1. Extract this archive to a temporary location
  2. Open PowerShell and run:
       powershell -ExecutionPolicy Bypass -File install.ps1
  3. Or install to a custom directory:
       powershell -ExecutionPolicy Bypass -File install.ps1 -InstallDir "C:\Skink"

Prerequisites:
  - LLVM/Clang must be installed on the target system (used for linking)
    Install from https://releases.llvm.org/ or: winget install LLVM.LLVM

Usage:
  set SKINK_HOME=C:\Users\You\AppData\Local\Skink\std
  skink -o program.exe program.skink

Verify:
  skink -v

License: Apache License 2.0
Copyright 2026 Mark Oxley
READMEEOF

# ============================================================================
# Step 8: Create ZIP archive
# ============================================================================

info "Creating ZIP archive..."
mkdir -p "$OUTPUT_DIR"
ARCHIVE_NAME="skink-${VERSION}-windows-x86_64.zip"
ARCHIVE_PATH="${OUTPUT_DIR}/${ARCHIVE_NAME}"

# Use zip if available, otherwise use tar (which can create zip on some systems)
if command -v zip >/dev/null 2>&1; then
    (cd "$STAGING_DIR" && zip -r -q "$ARCHIVE_PATH" .)
else
    # Fallback: use python to create a zip
    python3 -c "
import zipfile, os, sys
with zipfile.ZipFile('$ARCHIVE_PATH', 'w', zipfile.ZIP_DEFLATED) as zf:
    for root, dirs, files in os.walk('$STAGING_DIR'):
        for f in files:
            filepath = os.path.join(root, f)
            arcname = os.path.relpath(filepath, '$STAGING_DIR')
            zf.write(filepath, arcname)
"
fi

info "  Created: $ARCHIVE_PATH"
info "  Size: $(du -h "$ARCHIVE_PATH" | cut -f1)"

# Generate SHA256 checksum
CHECKSUM_PATH="${ARCHIVE_PATH}.sha256"
sha256sum "$ARCHIVE_PATH" | awk '{print $1"  '"$(basename "$ARCHIVE_PATH")"'"}' > "$CHECKSUM_PATH"
info "  Checksum: $CHECKSUM_PATH"

# List contents
info "  Contents:"
if command -v zip >/dev/null 2>&1; then
    unzip -l "$ARCHIVE_PATH" | tail -5
else
    python3 -c "
import zipfile
with zipfile.ZipFile('$ARCHIVE_PATH') as zf:
    items = zf.namelist()
    print(f'  Total files: {len(items)}')
    for f in items[:5]:
        print(f'    {f}')
    if len(items) > 5:
        print(f'    ... and {len(items)-5} more')
"
fi

# ============================================================================
# Done
# ============================================================================

echo ""
info "Skink v${VERSION} for Windows x86_64 built successfully!"
echo ""
echo "  Archive:  $ARCHIVE_PATH"
echo "  Checksum: $CHECKSUM_PATH"
echo ""
