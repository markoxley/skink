#!/usr/bin/env bash
# Copyright 2026 Mark Oxley. All rights reserved.
#
# build_windows.sh — Cross-compile the Skink compiler for Windows x86_64.
#
# This script:
#   1. Emits LLVM IR from skink.skink, scales.skink, and skinkjs.skink using the host Skink compiler
#   2. Cross-compiles the IR + runtime C files to Windows .exe binaries using clang
#   3. Bundles skink.exe, scales.exe, skinkjs.exe, std/, runtime/, DLLs, and install.ps1 into a ZIP
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
SCALES_DIR="${SCALES_DIR:-${SCRIPT_DIR}/../skink-scales}"
SKINKJS_DIR="${SKINKJS_DIR:-${SCRIPT_DIR}/../skinkjs}"
LSP_DIR="${LSP_DIR:-${SCRIPT_DIR}/../skink-lsp}"
cd "$SCRIPT_DIR"

# Compiler binary used for -emit-ll: prefer an installed skink (matches
# current sources — the in-repo ./skink can be a stale stage-0), fall
# back to the in-repo binary. Override with SKINK=/path/to/skink.
SKINK_BIN="${SKINK:-$(command -v skink || echo "${SCRIPT_DIR}/skink")}"

# Version and architecture
VERSION="${1:-$(cat version.txt 2>/dev/null || echo "0.0.0")}"
VERSION="${VERSION#v}"
ARCH="${2:-x86_64}"
case "$ARCH" in
    x86_64)        TARGET="x86_64-w64-mingw32" ;;
    aarch64|arm64) TARGET="aarch64-w64-mingw32"; ARCH="aarch64" ;;
    *) echo "ERROR: unknown arch '$ARCH' (expected x86_64 or aarch64)" >&2; exit 1 ;;
esac

# llvm-mingw root: required for aarch64. Host clang's MinGW driver
# injects -lgcc/-lgcc_eh/-lgcc_s which llvm-mingw does not ship — its
# prefixed clang is configured for compiler-rt + lld instead.
LLVM_MINGW_ROOT=""
for candidate in \
    "${SKINK_MINGW_ROOT:-}" \
    "${HOME}/llvm-mingw" \
    "/opt/llvm-mingw" \
    "/usr/local/llvm-mingw"; do
    if [[ -n "$candidate" ]] && [[ -d "${candidate}/aarch64-w64-mingw32" ]]; then
        LLVM_MINGW_ROOT="$candidate"
        break
    fi
done

# Compiler used for the target. x86_64 uses host clang (auto-detects
# the Fedora MinGW sysroot); aarch64 uses llvm-mingw's prefixed clang.
CLANG_BIN="clang"
if [[ "$ARCH" == "aarch64" ]]; then
    if [[ -n "$LLVM_MINGW_ROOT" ]] && [[ -x "${LLVM_MINGW_ROOT}/bin/aarch64-w64-mingw32-clang" ]]; then
        CLANG_BIN="${LLVM_MINGW_ROOT}/bin/aarch64-w64-mingw32-clang"
    else
        echo "ERROR: aarch64 build needs llvm-mingw (set SKINK_MINGW_ROOT):" >&2
        echo "  https://github.com/mstorsjo/llvm-mingw/releases" >&2
        exit 1
    fi
fi

# Configuration
# MinGW sysroot: check system install first, then local extraction
MINGW_SYSROOT=""
if [[ "$ARCH" == "aarch64" ]]; then
    MINGW_SYSROOT="${LLVM_MINGW_ROOT}/aarch64-w64-mingw32"
else
    for candidate in \
        "/usr/x86_64-w64-mingw32/sys-root/mingw" \
        "/tmp/mingw-sysroot/usr/x86_64-w64-mingw32/sys-root/mingw" \
        "$HOME/mingw-sysroot/usr/x86_64-w64-mingw32/sys-root/mingw"; do
        if [[ -d "$candidate" ]] && [[ -f "$candidate/lib/crt2.o" ]]; then
            MINGW_SYSROOT="$candidate"
            break
        fi
    done
fi
# GCC library path (for libgcc, libgcc_eh; llvm-mingw uses compiler-rt
# and needs none)
GCC_LIB=""
if [[ "$ARCH" == "x86_64" ]]; then
    for candidate in \
        "/usr/lib/gcc/x86_64-w64-mingw32"/*/ \
        "/tmp/mingw-sysroot/usr/lib/gcc/x86_64-w64-mingw32"/*/; do
        if [[ -d "$candidate" ]] && [[ -f "${candidate}libgcc.a" ]]; then
            GCC_LIB="${candidate%/}"
            break
        fi
    done
fi

# Third-party dep prefix (openssl/zlib/sqlite3) for arches without a
# system MinGW package set. aarch64 uses MSYS2 clangarm64 packages in
# the layout produced by scripts/build_deps_mingw.sh: include/, lib/
# (.dll.a import libs), bin/ (.dll). Fedora's mingw64-* packages only
# cover x86_64.
DEP_PREFIX=""
if [[ "$ARCH" == "aarch64" ]]; then
    for candidate in \
        "${SKINK_A64_DEPS:-}" \
        "${HOME}/skink-deps/clangarm64" \
        "/tmp/a64deps/clangarm64" \
        "/opt/skink-deps/clangarm64"; do
        if [[ -n "$candidate" ]] && [[ -d "${candidate}/lib" ]]; then
            DEP_PREFIX="$candidate"
            break
        fi
    done
    if [[ -z "$DEP_PREFIX" ]]; then
        echo "ERROR: aarch64 deps (openssl/zlib/sqlite3) not found. Fetch with:" >&2
        echo "  scripts/build_deps_mingw.sh aarch64" >&2
        exit 1
    fi
fi
OUTPUT_DIR="${SCRIPT_DIR}/dist/windows"
STAGING_DIR="$(mktemp -d)"
trap 'rm -rf "$STAGING_DIR"' EXIT

# Helpers
info()  { printf "\033[1;34m==>\033[0m %s\n" "$*"; }
warn()  { printf "\033[1;33mwarn:\033[0m %s\n" "$*"; }
error() { printf "\033[1;31merror:\033[0m %s\n" "$*" >&2; }
fail()  { error "$*"; exit 1; }

info "Building Skink v${VERSION} for Windows ${ARCH} (${TARGET})"

# ============================================================================
# Step 1: Verify MinGW toolchain
# ============================================================================

info "Checking MinGW toolchain..."
if [[ ! -d "$MINGW_SYSROOT" ]]; then
    fail "MinGW sysroot for $TARGET not found. Install with:
  sudo dnf install -y mingw64-gcc mingw64-headers mingw64-winpthreads mingw64-openssl mingw64-zlib mingw64-sqlite mingw64-libgcc"
fi

# Verify clang supports the target
if ! "$CLANG_BIN" --target=$TARGET --version >/dev/null 2>&1; then
    fail "$CLANG_BIN does not support target $TARGET"
fi

info "  MinGW sysroot: $MINGW_SYSROOT"
info "  clang:         $CLANG_BIN"
info "  clang target:  $TARGET"

# ============================================================================
# Step 2: Emit LLVM IR from skink.skink
# ============================================================================

info "Emitting LLVM IR from skink.skink (using ${SKINK_BIN})..."
SKINK_HOME="${SCRIPT_DIR}/std" "$SKINK_BIN" -emit-ll skink.skink > "${STAGING_DIR}/skink.ll" 2>/dev/null
[[ -s "${STAGING_DIR}/skink.ll" ]] || fail "Failed to emit LLVM IR"

LL_LINES=$(wc -l < "${STAGING_DIR}/skink.ll")
info "  Emitted ${LL_LINES} lines of LLVM IR"

# ============================================================================
# Step 3: Cross-compile to Windows .exe
# ============================================================================

info "Cross-compiling skink.exe..."

# Runtime C files to compile: all of std/runtime/*.c except ui_rt.c
# (SDL2-only; headless binaries exclude it — same selection as the
# Makefile's BOOTSTRAP_RT). Globbing prevents drift when runtime files
# are added or removed; the previous explicit list had silently dropped
# sys_rt.c and cov_rt.c.
RUNTIME_FILES=""
for f in "${SCRIPT_DIR}"/std/runtime/*.c; do
    case "$(basename "$f")" in
        ui_rt.c) continue ;;
    esac
    RUNTIME_FILES="${RUNTIME_FILES} ${f}"
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
if [[ -n "$DEP_PREFIX" ]]; then
    CROSS_FLAGS+=(-I"$DEP_PREFIX/include" -L"$DEP_PREFIX/lib")
fi

"$CLANG_BIN" "${CROSS_FLAGS[@]}" || fail "Cross-compilation failed"
rm -f "${STAGING_DIR}/skink.ll"

# Verify the output
if ! file "${STAGING_DIR}/skink.exe" | grep -q "PE32+"; then
    fail "Output is not a Windows PE32+ executable: $(file "${STAGING_DIR}/skink.exe")"
fi

info "  Built: skink.exe ($(du -h "${STAGING_DIR}/skink.exe" | cut -f1))"

# ============================================================================
# Step 3b-d: Emit LLVM IR and cross-compile the sibling tools
# ============================================================================

# build_tool_exe <source-dir> <src.skink> <out.exe> — emit IR in the
# tool's own directory (its imports resolve relative to cwd), then
# cross-compile with the same runtime/link recipe as skink.exe.
build_tool_exe() {
    local tool_dir="$1" src="$2" out="$3"
    local base="${out%.exe}"

    info "Emitting LLVM IR from ${src}..."
    (
        cd "$tool_dir"
        SKINK_HOME="${SCRIPT_DIR}/std" "$SKINK_BIN" -emit-ll "$src" > "${STAGING_DIR}/${base}.ll" 2>/dev/null
    )
    [[ -s "${STAGING_DIR}/${base}.ll" ]] || fail "Failed to emit LLVM IR for ${src}"

    info "Cross-compiling ${out}..."
    local flags=(
        --target=$TARGET
        -Wno-override-module
        -O2
        -I"$MINGW_SYSROOT/include"
        -I"${SCRIPT_DIR}/std/runtime"
        -L"$MINGW_SYSROOT/lib"
        -o "${STAGING_DIR}/${out}"
        "${STAGING_DIR}/${base}.ll"
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
        flags+=(-L"$GCC_LIB")
    fi
    if [[ -n "$DEP_PREFIX" ]]; then
        flags+=(-I"$DEP_PREFIX/include" -L"$DEP_PREFIX/lib")
    fi

    "$CLANG_BIN" "${flags[@]}" || fail "${out} cross-compilation failed"
    rm -f "${STAGING_DIR}/${base}.ll"

    if ! file "${STAGING_DIR}/${out}" | grep -q "PE32+"; then
        fail "${out} is not a Windows PE32+ executable: $(file "${STAGING_DIR}/${out}")"
    fi

    info "  Built: ${out} ($(du -h "${STAGING_DIR}/${out}" | cut -f1))"
}

if [[ -d "$SCALES_DIR" ]] && [[ -f "${SCALES_DIR}/scales.skink" ]]; then
    build_tool_exe "$SCALES_DIR" scales.skink scales.exe
else
    warn "scales directory not found at ${SCALES_DIR}, skipping scales.exe"
fi

if [[ -d "$SKINKJS_DIR" ]] && [[ -f "${SKINKJS_DIR}/skinkjs.skink" ]]; then
    build_tool_exe "$SKINKJS_DIR" skinkjs.skink skinkjs.exe
else
    warn "skinkjs directory not found at ${SKINKJS_DIR}, skipping skinkjs.exe"
fi

if [[ -d "$LSP_DIR" ]] && [[ -f "${LSP_DIR}/lsp.skink" ]]; then
    build_tool_exe "$LSP_DIR" lsp.skink skink-lsp.exe
else
    warn "skink-lsp directory not found at ${LSP_DIR}, skipping skink-lsp.exe"
fi

if [[ -d "$LSP_DIR" ]] && [[ -f "${LSP_DIR}/skinkfmt.skink" ]]; then
    build_tool_exe "$LSP_DIR" skinkfmt.skink skinkfmt.exe
else
    warn "skinkfmt source not found at ${LSP_DIR}, skipping skinkfmt.exe"
fi

# ============================================================================
# Step 4: Bundle DLLs
# ============================================================================

info "Bundling Windows DLLs..."

# Find and copy required DLLs. Names differ per arch: Fedora MinGW
# uses x64-suffixed OpenSSL DLLs, MSYS2 clangarm64 uses arm64 suffixes
# and ships libz under libz-1.dll.
DLL_DIRS=("$MINGW_SYSROOT/bin" "$MINGW_SYSROOT/lib")
if [[ "$ARCH" == "aarch64" ]]; then
    DLLS=(
        libwinpthread-1.dll
        libssl-3-arm64.dll
        libcrypto-3-arm64.dll
        libsqlite3-0.dll
        libz-1.dll
        libunwind.dll
    )
    DLL_DIRS+=("$DEP_PREFIX/bin")
else
    DLLS=(
        libwinpthread-1.dll
        libssl-3-x64.dll
        libcrypto-3-x64.dll
        libsqlite3-0.dll
        zlib1.dll
        libgcc_s_seh-1.dll
        libstdc++-6.dll
    )
    DLL_DIRS+=("/usr/x86_64-w64-mingw32/bin" "/tmp/mingw-sysroot/usr/x86_64-w64-mingw32/sys-root/mingw/bin")
fi

for dll in "${DLLS[@]}"; do
    found=""
    for dir in "${DLL_DIRS[@]}"; do
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

# Test/bench launchers for 'skink -t' / 'skink -b': the .sh harnesses
# plus .cmd wrappers that locate Git Bash on Windows.
for launcher in skink_test.sh skink_test.cmd skink_bench.sh skink_bench.cmd; do
    if [[ -f "${SCRIPT_DIR}/${launcher}" ]]; then
        cp "${SCRIPT_DIR}/${launcher}" "${STAGING_DIR}/"
        info "  Bundled: ${launcher}"
    fi
done

# ============================================================================
# Step 5b: Bundle Linux sysroot for Windows→Linux cross-compilation
# ============================================================================
#
# skink.exe -target x86_64-linux looks for SKINK_HOME/sysroots/
# x86_64-unknown-linux-gnu/. Staging it inside std/sysroots/ means the
# installer's std/ copy lands it at that path automatically.
if [[ -x "${SCRIPT_DIR}/scripts/build_linux_sysroot.sh" ]]; then
    info "Bundling Linux sysroot for Windows→Linux builds..."
    if "${SCRIPT_DIR}/scripts/build_linux_sysroot.sh" "${STAGING_DIR}/std/sysroots" >/dev/null 2>&1; then
        info "  sysroots/: $(du -sh "${STAGING_DIR}/std/sysroots" | cut -f1)"
    else
        warn "Linux sysroot build failed — -target x86_64-linux will need SKINK_SYSROOT"
    fi
else
    warn "scripts/build_linux_sysroot.sh not found — skipping sysroot bundle"
fi

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

# Copy skink.exe, scales.exe, and DLLs to bin/
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Copy-Item "$ScriptDir\skink.exe" $BinDir -Force
Write-Host "  Installed: bin\skink.exe"
if (Test-Path "$ScriptDir\scales.exe") {
    Copy-Item "$ScriptDir\scales.exe" $BinDir -Force
    Write-Host "  Installed: bin\scales.exe"
}
if (Test-Path "$ScriptDir\skinkjs.exe") {
    Copy-Item "$ScriptDir\skinkjs.exe" $BinDir -Force
    Write-Host "  Installed: bin\skinkjs.exe"
}
if (Test-Path "$ScriptDir\skink-lsp.exe") {
    Copy-Item "$ScriptDir\skink-lsp.exe" $BinDir -Force
    Write-Host "  Installed: bin\skink-lsp.exe"
}
if (Test-Path "$ScriptDir\skinkfmt.exe") {
    Copy-Item "$ScriptDir\skinkfmt.exe" $BinDir -Force
    Write-Host "  Installed: bin\skinkfmt.exe"
}

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

# Set environment variables for the current user. SKINK_HOME is written
# with forward slashes: the compiler's import resolver splits paths on
# "/" only, so a backslash-spelled root breaks std/ resolution on older
# builds (current builds normalize either spelling).
$StdDirFW = $StdDir -replace '\\', '/'
$envKey = [Environment]::GetEnvironmentVariable("SKINK_HOME", "User")
if ($envKey -ne $StdDirFW) {
    [Environment]::SetEnvironmentVariable("SKINK_HOME", $StdDirFW, "User")
    Write-Host "  Set SKINK_HOME = $StdDirFW"
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
if (Test-Path "$BinDir\scales.exe") {
    Write-Host "  Scales:  $BinDir\scales.exe"
}
if (Test-Path "$BinDir\skinkjs.exe") {
    Write-Host "  SkinkJS: $BinDir\skinkjs.exe"
}
if (Test-Path "$BinDir\skink-lsp.exe") {
    Write-Host "  LSP:     $BinDir\skink-lsp.exe"
}
if (Test-Path "$BinDir\skinkfmt.exe") {
    Write-Host "  Fmt:     $BinDir\skinkfmt.exe"
}
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
Skink Compiler v${VERSION} for Windows ${ARCH}
===============================================

Contents:
  skink.exe       — The Skink compiler binary
  scales.exe      — The Skink package manager and module resolver
  skinkjs.exe     — The Skink to ECMAScript/JavaScript transpiler
  skink-lsp.exe   — The Skink language server (LSP)
  skinkfmt.exe    — The Skink source formatter
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
ARCHIVE_NAME="skink-${VERSION}-windows-${ARCH}.zip"
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
info "Skink v${VERSION} for Windows ${ARCH} built successfully!"
echo ""
echo "  Archive:  $ARCHIVE_PATH"
echo "  Checksum: $CHECKSUM_PATH"
echo ""
