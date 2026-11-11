#!/usr/bin/env bash
#
# build_deps_mingw.sh — fetch third-party dependency libraries for
# Windows targets that lack a system MinGW package set.
#
# Fedora's mingw64-{openssl,zlib,sqlite} packages cover x86_64 only.
# For aarch64-w64-mingw32 we pull prebuilt packages from the MSYS2
# clangarm64 repository (same ABI as llvm-mingw: UCRT + compiler-rt +
# LLD, .dll.a import libraries, no libgcc dependency).
#
# Usage:
#   scripts/build_deps_mingw.sh aarch64 [prefix]
#     -> populates <prefix>/{include,lib,bin}
#        default prefix: $HOME/skink-deps/clangarm64
#
# build_windows.sh aarch64 auto-detects the prefix via SKINK_A64_DEPS,
# $HOME/skink-deps/clangarm64, or /opt/skink-deps/clangarm64.

set -euo pipefail

ARCH="${1:-aarch64}"
case "$ARCH" in
    aarch64|arm64)
        MSYS2_REPO="https://repo.msys2.org/mingw/clangarm64"
        PKG_PREFIX="mingw-w64-clang-aarch64"
        ;;
    *) echo "ERROR: only aarch64 is supported (x86_64 uses system mingw64-* packages)" >&2; exit 1 ;;
esac

PREFIX="${2:-${HOME}/skink-deps/clangarm64}"
mkdir -p "$PREFIX"

info()  { printf "\033[1;34m==>\033[0m %s\n" "$*"; }
fail()  { printf "\033[1;31merror:\033[0m %s\n" "$*" >&2; exit 1; }
command -v curl >/dev/null || fail "curl required"
command -v unzstd >/dev/null || command -v zstd >/dev/null || fail "zstd required (dnf install zstd)"

# Package basenames (name-version-release before -any.pkg.tar.zst).
# Pinned versions; bump as needed.
PKGS=(
    openssl-3.6.5-1
    sqlite3-3.54.1-1
    zlib-1.4-2
)

info "Fetching aarch64-mingw dep packages from MSYS2 clangarm64"
info "  prefix: ${PREFIX}"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

for pkg in "${PKGS[@]}"; do
    name="${PKG_PREFIX}-${pkg}-any.pkg.tar.zst"
    info "  ${name}"
    curl -fsSL --retry 3 -o "${tmp}/pkg.tar.zst" "${MSYS2_REPO}/${name}" || \
        fail "download failed: ${name} (check version on ${MSYS2_REPO}/)"
    tar --zstd -xf "${tmp}/pkg.tar.zst" -C "$tmp"
    rm -f "${tmp}/pkg.tar.zst"
done

# Package payload lives under clangarm64/ — merge its include/, lib/,
# bin/ into the prefix.
for sub in include lib bin; do
    if [[ -d "${tmp}/clangarm64/${sub}" ]]; then
        mkdir -p "${PREFIX}/${sub}"
        cp -a "${tmp}/clangarm64/${sub}/." "${PREFIX}/${sub}/"
    fi
done

info "Installed dep files:"
for so in libssl.dll.a libcrypto.dll.a libsqlite3.dll.a libz.dll.a; do
    [[ -f "${PREFIX}/lib/${so}" ]] && echo "    lib/${so}"
done
for dll in libssl-3-arm64.dll libcrypto-3-arm64.dll libsqlite3-0.dll libz-1.dll; do
    [[ -f "${PREFIX}/bin/${dll}" ]] && echo "    bin/${dll}"
done

info "Done. build_windows.sh aarch64 auto-detects ${PREFIX}"
info "  (or set SKINK_A64_DEPS=${PREFIX})"
