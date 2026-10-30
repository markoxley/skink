// Copyright 2026 Mark Oxley. All rights reserved.
//
// Project: Skink — A self-hosted, statically-typed programming language
//          targeting LLVM IR, designed for systems programming with
//          modern ergonomics and a Go-inspired syntax.
//
// File:    std/runtime/sys_rt.c
// Purpose: sys_rt.c — Platform introspection runtime for Skink. Backs the
//          std/runtime module: identifies the host OS and CPU architecture
//          at compile time (preprocessor) and reports per-process values
//          (executable path, CPU count, page size) at run time.
// Author:  Mark Oxley
// Created: 2026
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// sys_rt.c — Platform introspection runtime for Skink.
//
// The OS/architecture strings are resolved by the preprocessor so a
// cross-compiled binary reports the target it was built for, not the
// machine that happens to run the compiler. Per-process values come from
// the C library / OS APIs so they reflect the running executable.

#include "skink_compat.h"

#ifdef _WIN32

// === Windows includes ===
#include <stdlib.h>
#include <string.h>
// windows.h comes in via skink_compat.h; GetModuleFileNameA, GetSystemInfo,
// and MAX_PATH are declared there.

#else

// === POSIX includes ===
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>   // _NSGetExecutablePath
#include <stdint.h>
#endif

#endif

// Skink_rt_os — name of the operating system this binary was compiled for.
// Values deliberately mirror the release target naming used by release.sh
// ("linux", "darwin", "windows") so a program can compare runtime.OS()
// against the archive suffixes.
const char *Skink_rt_os(void) {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "darwin";
#elif defined(__linux__)
  return "linux";
#elif defined(__FreeBSD__)
  return "freebsd";
#elif defined(__OpenBSD__)
  return "openbsd";
#elif defined(__NetBSD__)
  return "netbsd";
#elif defined(__unix__) || defined(__unix)
  return "unix";
#else
  return "unknown";
#endif
}

// Skink_rt_arch — name of the CPU architecture this binary was compiled for.
// Uses the same spelling as uname -m / release.sh ("x86_64", "arm64") so
// values compose with the rest of the toolchain.
const char *Skink_rt_arch(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
  return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__arm__) || defined(_M_ARM)
  return "arm";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#elif defined(__riscv) && (__riscv_xlen == 64)
  return "riscv64";
#elif defined(__powerpc64__) || defined(__ppc64__)
  return "ppc64";
#else
  return "unknown";
#endif
}

// Skink_rt_executable_path — absolute path of the running executable,
// returned as a fresh heap allocation (caller/RC owns it). Returns an
// allocated empty string when the platform cannot supply the path, so
// callers never see NULL.
char *Skink_rt_executable_path(void) {
#ifdef _WIN32
  // GetModuleFileNameA truncates at the buffer size, so grow until the
  // result fits (n < cap-1 means untruncated).
  DWORD cap = MAX_PATH;
  for (;;) {
    char *buf = (char *)malloc(cap);
    if (!buf) {
      return strdup("");
    }
    DWORD n = GetModuleFileNameA(NULL, buf, cap);
    if (n == 0) {
      free(buf);
      return strdup("");
    }
    if (n < cap - 1) {
      return buf;
    }
    free(buf);
    cap *= 2;
    if (cap > 65536) {
      return strdup("");
    }
  }
#elif defined(__APPLE__)
  // _NSGetExecutablePath reports the required size via bufsize when the
  // buffer is too small; the returned path may contain symlinks, so
  // canonicalise with realpath when possible.
  uint32_t cap = 1024;
  for (;;) {
    char *buf = (char *)malloc(cap);
    if (!buf) {
      return strdup("");
    }
    if (_NSGetExecutablePath(buf, &cap) == 0) {
      char *resolved = realpath(buf, NULL);
      if (resolved) {
        free(buf);
        return resolved;
      }
      return buf;
    }
    free(buf);
    if (cap > 65536) {
      return strdup("");
    }
  }
#else
  // Linux exposes the canonical executable path through /proc/self/exe;
  // the BSDs use /proc/curproc/file when procfs is mounted. readlink does
  // not NUL-terminate and truncates silently, so grow the buffer until the
  // result fits.
  size_t cap = 1024;
  for (;;) {
    char *buf = (char *)malloc(cap);
    if (!buf) {
      return strdup("");
    }
    ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    if (n < 0) {
      n = readlink("/proc/curproc/file", buf, cap - 1);
    }
#endif
    if (n < 0) {
      free(buf);
      return strdup("");
    }
    if ((size_t)n < cap - 1) {
      buf[n] = '\0';
      return buf;
    }
    free(buf);
    cap *= 2;
    if (cap > (1 << 20)) {
      return strdup("");
    }
  }
#endif
}

// Skink_rt_num_cpus — number of logical CPUs currently online. Always >= 1:
// a process cannot run on zero processors, so a failed query falls back to
// 1 rather than propagating an error value.
long Skink_rt_num_cpus(void) {
#ifdef _WIN32
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return (long)si.dwNumberOfProcessors;
#else
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? n : 1;
#endif
}

// Skink_rt_page_size — OS memory page size in bytes (typically 4096).
// Falls back to 4096 when sysconf fails.
long Skink_rt_page_size(void) {
#ifdef _WIN32
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return (long)si.dwPageSize;
#else
  long n = sysconf(_SC_PAGESIZE);
  return n > 0 ? n : 4096;
#endif
}
