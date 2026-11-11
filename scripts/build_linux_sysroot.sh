#!/usr/bin/env bash
#
# build_linux_sysroot.sh — assemble a minimal x86_64-linux sysroot that
# can be bundled into the Windows distribution so skink.exe on Windows
# can cross-compile Linux binaries:
#
#   skink.exe -target x86_64-linux hello.skink
#     -> clang --target=x86_64-unknown-linux-gnu --sysroot=<this> -fuse-ld=lld
#
# The script runs on the Linux *build* host (where the Windows dist is
# produced) and copies headers, CRT objects, linker scripts/DSOs, and the
# GCC runtime files from the host's sysroot.
#
# Usage:
#   scripts/build_linux_sysroot.sh <outdir>
#     -> writes <outdir>/x86_64-unknown-linux-gnu/{usr,lib,lib64}/...
#
# Zip-safety: the sysroot will be unzipped on Windows, where symlinks do
# not survive — everything is copied dereferenced (cp -L), and dev .so
# names are real files rather than links. Linker scripts (libc.so et al.)
# keep their absolute /lib64 paths; lld resolves them against --sysroot.

set -euo pipefail

OUT_DIR="${1:?usage: build_linux_sysroot.sh <outdir>}"
TRIPLE="x86_64-unknown-linux-gnu"
HOST_TRIPLE="x86_64-redhat-linux"   # overridden below when detected differently
SYSROOT="${OUT_DIR}/${TRIPLE}"

info()  { printf "\033[1;34m==>\033[0m %s\n" "$*"; }
warn()  { printf "\033[1;33mwarn:\033[0m %s\n" "$*"; }
fail()  { printf "\033[1;31merror:\033[0m %s\n" "$*" >&2; exit 1; }

# --- Detect host library layout ------------------------------------------------
# Fedora/RHEL: /usr/lib64 + /lib64;  Debian/Ubuntu: /usr/lib/x86_64-linux-gnu
LIBDIR="lib64"
if [[ -d /usr/lib/x86_64-linux-gnu ]]; then
    LIBDIR="lib/x86_64-linux-gnu"
    HOST_TRIPLE="x86_64-linux-gnu"
fi

GCC_DIR=""
for candidate in \
    /usr/lib/gcc/x86_64-redhat-linux/*/ \
    /usr/lib/gcc/x86_64-linux-gnu/*/ \
    /usr/lib/gcc/${HOST_TRIPLE}/*/; do
    if [[ -d "$candidate" ]] && [[ -f "${candidate}libgcc.a" ]]; then
        GCC_DIR="${candidate%/}"
        break
    fi
done
[[ -n "$GCC_DIR" ]] || fail "no GCC runtime dir found (need libgcc.a + crtbegin/crtend)"

info "Building Linux sysroot for ${TRIPLE}"
info "  host libdir:  /usr/${LIBDIR}"
info "  gcc dir:      ${GCC_DIR}"
info "  output:       ${SYSROOT}"

rm -rf "$SYSROOT"
mkdir -p "${SYSROOT}/usr/${LIBDIR}" "${SYSROOT}/${LIBDIR}" "${SYSROOT}/usr/lib/gcc"

# --- 1. Headers ----------------------------------------------------------------
info "Copying headers..."
cp -a /usr/include "${SYSROOT}/usr/" 2>/dev/null || {
    mkdir -p "${SYSROOT}/usr/include"
    cp -a /usr/include/. "${SYSROOT}/usr/include/"
}
# Debian multiarch headers live under /usr/include/<triple>
for d in /usr/include/x86_64-*/; do
    [[ -d "$d" ]] && cp -a "$d" "${SYSROOT}/usr/include/"
done

# --- 2. CRT objects ------------------------------------------------------------
# Names vary in case and prefix: crt1.o/crti.o/crtn.o plus Scrt1.o (PIE —
# required, clang defaults to -pie), gcrt1.o/grcrt1.o/Mcrt1.o/rcrt1.o.
info "Copying CRT objects..."
cp -L "/usr/${LIBDIR}"/*crt*.o "${SYSROOT}/usr/${LIBDIR}/" || \
    fail "no CRT objects in /usr/${LIBDIR}"

# --- 3. GCC runtime (libgcc.a, libgcc_eh.a, crtbegin/crtend) --------------------
info "Copying GCC runtime..."
GCC_REL="${GCC_DIR#/usr/lib/gcc/}"
mkdir -p "${SYSROOT}/usr/lib/gcc/${GCC_REL}"
cp -a "${GCC_DIR}/." "${SYSROOT}/usr/lib/gcc/${GCC_REL}/"

# --- 4. Link-time libraries ----------------------------------------------------
# Skink links: -lpthread -ldl -lm -lssl -lcrypto -lz -lsqlite3
# glibc linker scripts (libc.so, libpthread.so on old glibc) are copied
# verbatim; ELF shared objects are copied dereferenced under their
# link-time name so no symlinks are required.
info "Copying link libraries..."

copy_dev_lib() {
    # $1 = library basename without 'lib' prefix or extension (e.g. "ssl")
    local name="$1"
    local devpath="/usr/${LIBDIR}/lib${name}.so"
    local soname
    if [[ -f "$devpath" ]]; then
        cp -L "$devpath" "${SYSROOT}/usr/${LIBDIR}/lib${name}.so"
        return 0
    fi
    # No dev file — find the runtime DSO (libfoo.so.N) and copy it under
    # the dev name; DT_SONAME inside still points at the runtime name.
    soname="$(find "/${LIBDIR}" "/usr/${LIBDIR}" -maxdepth 1 -name "lib${name}.so.*" -type f 2>/dev/null | sort -V | tail -1)"
    if [[ -n "$soname" ]]; then
        cp -L "$soname" "${SYSROOT}/usr/${LIBDIR}/lib${name}.so"
        return 0
    fi
    return 1
}

MISSING=""
for lib in pthread dl m rt util resolv ssl crypto z sqlite3; do
    if ! copy_dev_lib "$lib"; then
        MISSING="${MISSING} lib${lib}"
    fi
done
[[ -z "$MISSING" ]] || warn "missing link libraries:${MISSING} (install -devel/-dev packages)"

# libc.so and libc_nonshared.a: keep the linker script verbatim — its
# absolute GROUP paths resolve against the sysroot under lld.
if [[ -f "/usr/${LIBDIR}/libc.so" ]]; then
    cp "/usr/${LIBDIR}/libc.so" "${SYSROOT}/usr/${LIBDIR}/libc.so"
else
    cp -L "/${LIBDIR}/libc.so.6" "${SYSROOT}/usr/${LIBDIR}/libc.so"
fi
[[ -f "/usr/${LIBDIR}/libc_nonshared.a" ]] && \
    cp "/usr/${LIBDIR}/libc_nonshared.a" "${SYSROOT}/usr/${LIBDIR}/"

# --- 5. Runtime DSOs referenced by linker scripts -------------------------------
# libc.so's GROUP references /lib64/libc.so.6 and the dynamic loader;
# libgcc_s/libpthread stubs likewise resolve inside the sysroot.
info "Copying runtime DSOs..."
cp -L "/${LIBDIR}/libc.so.6" "${SYSROOT}/${LIBDIR}/"
cp -L "/${LIBDIR}/ld-linux-x86-64.so.2" "${SYSROOT}/${LIBDIR}/" || \
    cp -L "/lib/ld-musl-x86_64.so.1" "${SYSROOT}/${LIBDIR}/" 2>/dev/null || \
    warn "dynamic loader not found under /${LIBDIR}"
for so in libgcc_s.so.1 libpthread.so.0 libdl.so.2 libm.so.6 libmvec.so.1 \
          librt.so.1 libutil.so.1 libresolv.so.2; do
    for dir in "/${LIBDIR}" "/usr/${LIBDIR}"; do
        if [[ -f "${dir}/${so}" ]]; then
            cp -L "${dir}/${so}" "${SYSROOT}/${LIBDIR}/"
            break
        fi
    done
done

# --- 6. Summary -----------------------------------------------------------------
SIZE="$(du -sh "${SYSROOT}" | cut -f1)"
info "Done: ${SYSROOT} (${SIZE})"
info "Use with: skink -target x86_64-linux <file.skink>"
info "  (driver maps SKINK_SYSROOT -> --sysroot=${SYSROOT##*/} inside the dist)"
