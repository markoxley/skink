// Copyright 2026 Mark Oxley. All rights reserved.
//
// Project: Skink — A self-hosted, statically-typed programming language
//          targeting LLVM IR, designed for systems programming with
//          modern ergonomics and a Go-inspired syntax.
//
// File:    std/runtime/fs_rt.c
// Purpose: fs_rt.c — Platform-agnostic filesystem runtime for Skink.
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

// fs_rt.c — Platform-agnostic filesystem runtime for Skink.
//
// Provides wrapper functions that abstract platform-specific struct layouts
// (stat, dirent) so that Skink code doesn't need hardcoded offsets.

#include "skink_compat.h"

#ifdef _WIN32

// === Windows includes ===
#include <dirent.h>   // MinGW provides dirent.h
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <sys/stat.h>  // MinGW provides sys/stat.h
#include <io.h>        // _access, _close, _read, _write, _dup2
#include <process.h>   // _spawnvp, _cwait
#include <fcntl.h>     // _pipe, O_BINARY

#else

// === POSIX includes ===
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#endif

// SKINK_EXEC_OUT_MAX caps the bytes captured from a child's stdout by
// Skink_os_exec_with_output on both platforms: an unbounded child could
// otherwise stream gigabytes into memory (comp_error.md #153). When the
// cap is hit the pipe is closed (the child observes EPIPE/SIGPIPE, and
// SIGPIPE is ignored process-wide), the child is reaped, and the
// truncated buffer plus its exit status are returned.
#define SKINK_EXEC_OUT_MAX (64 * 1024 * 1024)

// realpath — resolve a path to its absolute, canonical form.
// On Windows, this wraps _fullpath from the C runtime.
#ifdef _WIN32
char *realpath(const char *path, char *resolved) {
  if (!path) return NULL;
  char *result = _fullpath(resolved, path, 4096);
  // Convert backslashes to forward slashes for consistency
  if (result) {
    for (char *p = result; *p; p++) {
      if (*p == '\\') *p = '/';
    }
  }
  return result;
}
#endif

// Skink_fs_stat — stat a path, returning fields via out-parameters.
// Returns 0 on success, -1 on error (errno is set).
int Skink_fs_stat(const char *path, int *mode, long long *size,
                  long long *mtime) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return -1;
  }
  if (mode)
    *mode = (int)st.st_mode;
  if (size)
    *size = (long long)st.st_size;
  if (mtime)
    *mtime = (long long)st.st_mtime;
  return 0;
}

// Skink_fs_isdir — returns 1 if path is a directory, 0 otherwise.
int Skink_fs_isdir(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  return S_ISDIR(st.st_mode) ? 1 : 0;
}

// Skink_fs_isfile — returns 1 if path is a regular file, 0 otherwise.
int Skink_fs_isfile(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  return S_ISREG(st.st_mode) ? 1 : 0;
}

// Skink_fs_exists — returns 1 if path exists, 0 otherwise.
int Skink_fs_exists(const char *path) {
  struct stat st;
  return (stat(path, &st) == 0) ? 1 : 0;
}

// Skink_fs_filesize — returns file size in bytes, or -1 on error.
long long Skink_fs_filesize(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return -1;
  return (long long)st.st_size;
}

// Skink_fs_mode — returns the st_mode (permission bits + type), or 0 on error.
int Skink_fs_mode(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  return (int)st.st_mode;
}

// Skink_fs_mtime — returns modification time, or -1 on error.
long long Skink_fs_mtime(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return -1;
  return (long long)st.st_mtime;
}

// === Directory iteration ===
//
// Skink uses an opaque iterator handle. The caller passes a buffer for
// the entry name (must be at least 256 bytes). Returns:
//   1 = entry read successfully (name is filled)
//   0 = end of directory
//  -1 = error

// Skink_fs_opendir — opens a directory for iteration. Returns handle or NULL.
void *Skink_fs_opendir(const char *path) { return opendir(path); }

// Skink_fs_readdir — reads next directory entry name into buf.
// Returns 1 on success, 0 on end, -1 on error.
// Also sets *is_dir to 1 if the entry is a directory.
int Skink_fs_readdir(void *dir, char *buf, int buf_size, int *is_dir) {
  if (!dir || !buf || buf_size <= 0)
    return -1;
  struct dirent *entry = readdir((DIR *)dir);
  if (entry == NULL) {
    return 0;
  }
  // Skip "." and ".." entries
  while (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
    entry = readdir((DIR *)dir);
    if (entry == NULL) {
      return 0;
    }
  }
  strncpy(buf, entry->d_name, buf_size - 1);
  buf[buf_size - 1] = '\0';

  if (is_dir) {
#if defined(_DIRENT_HAVE_D_TYPE) || defined(DT_DIR)
    if (entry->d_type == DT_DIR) {
      *is_dir = 1;
    } else if (entry->d_type == DT_UNKNOWN) {
      // Fallback: use stat for filesystems that don't support d_type
      *is_dir = 0; // Caller can check separately if needed
    } else {
      *is_dir = 0;
    }
#else
    *is_dir = 0;
#endif
  }
  return 1;
}

// Skink_fs_closedir — closes a directory handle.
int Skink_fs_closedir(void *dir) { return closedir((DIR *)dir); }

// Skink_fs_mkdir — creates a directory with default permissions (0755).
int Skink_fs_mkdir(const char *path) { return SKINK_MKDIR(path); }

// Skink_fs_mkdir_mode — creates a directory with specified mode.
int Skink_fs_mkdir_mode(const char *path, int mode) {
  return SKINK_MKDIR_MODE(path, mode);
}

// Skink_fs_remove — removes a file or empty directory.
int Skink_fs_remove(const char *path) { return remove(path); }

// Skink_fs_rename — renames or moves a file/directory.
int Skink_fs_rename(const char *old_path, const char *new_path) {
  return rename(old_path, new_path);
}

// Skink_fs_access — checks accessibility. mode: 0=exists, 1=readable,
// 2=writable, 4=executable.
int Skink_fs_access(const char *path, int mode) {
  int amode = F_OK;
  if (mode & 1)
    amode |= R_OK;
  if (mode & 2)
    amode |= W_OK;
  if (mode & 4)
    amode |= X_OK;
  return SKINK_ACCESS(path, amode);
}

// === Safe process execution (no shell) ===
//
// These functions run programs without invoking a shell, preventing
// command injection attacks. On POSIX they use fork/exec/waitpid; on
// Windows they use _spawnvp / CreateProcess with pipes.

// skink_rc_copy_out copies captured child output into an RC-managed block so
// the returned Skink string is reclaimed by ordinary RC release instead of
// leaking as an untracked malloc (comp_error.md #104). Always frees the
// caller's buffer; returns NULL on allocation failure.
extern void *Skink_rc_alloc(int64_t payload_size);
static char *skink_rc_copy_out(char *buf, size_t len) {
  char *out = (char *)Skink_rc_alloc((int64_t)len + 1);
  if (out)
    memcpy(out, buf, len + 1); // includes the NUL terminator
  free(buf);
  return out;
}

#ifdef _WIN32

// --- Windows implementation ---

#include <process.h>
#include <fcntl.h>

// Skink_os_exec — execute a program with an argument array, without
// invoking a shell. argv is a NULL-terminated array of C strings.
// argv[0] is the program name (conventionally the path).
// Returns the child exit status on success, -1 on error.
int Skink_os_exec(const char **argv, int argc) {
  (void)argc;
  if (!argv || !argv[0]) {
    return -1;
  }
  // _P_WAIT blocks until the child exits and returns its exit code.
  int rc = _spawnvp(_P_WAIT, argv[0], (const char *const *)argv);
  if (rc == -1) {
    return -1;
  }
  return rc;
}

// skink_append_cmdline_arg appends one argument to a CreateProcess
// command line using MSVCRT quoting rules. Every argument is quoted
// (safe and simpler than conditional quoting); inside quotes,
// backslashes are doubled only when they precede a quote or close the
// quoted section, and embedded quotes are backslash-escaped. When buf
// is NULL the call only advances *pos (size-computation pass).
static void skink_append_cmdline_arg(char *buf, size_t *pos,
                                     const char *arg) {
  size_t p = *pos;
  if (buf)
    buf[p] = '"';
  p++;
  size_t backslashes = 0;
  for (const char *s = arg;; s++) {
    char c = *s;
    if (c == '\0' || c == '"') {
      // Double backslashes before a quote or the closing quote.
      for (size_t i = 0; i < backslashes; i++) {
        if (buf)
          buf[p] = '\\';
        p++;
      }
      backslashes = 0;
    }
    if (c == '\0')
      break;
    if (c == '\\') {
      backslashes++;
    } else if (c == '"') {
      if (buf) {
        buf[p] = '\\';
        buf[p + 1] = '"';
      }
      p += 2;
    } else {
      if (buf)
        buf[p] = c;
      p++;
    }
  }
  if (buf)
    buf[p] = '"';
  p++;
  if (buf)
    buf[p] = ' ';
  p++; // trailing space separator (harmless on the last arg)
  *pos = p;
}

// Skink_os_exec_with_output — execute a program with an argument array,
// capturing stdout. Returns the child exit status via *status_out and
// the captured stdout as an RC-managed string (released by RC ownership).
// On error returns NULL.
//
// Uses CreateProcess with the pipe write handle bound directly to the
// child's stdout via STARTUPINFO. The previous implementation redirected
// the PROCESS-global stdout fd with _dup2 — a data race that could
// divert unrelated threads' output into the pipe or crash (VULN-24).
char *Skink_os_exec_with_output(const char **argv, int argc, int *status_out) {
  if (!argv || !argv[0] || !status_out) {
    if (status_out)
      *status_out = -1;
    return NULL;
  }

  // Build the CreateProcess command line (it takes a string, not argv):
  // one sizing pass, one writing pass.
  size_t needed = 0;
  for (int i = 0; i < argc && argv[i]; i++)
    skink_append_cmdline_arg(NULL, &needed, argv[i]);
  char *cmdline = (char *)malloc(needed + 1);
  if (!cmdline) {
    *status_out = -1;
    return NULL;
  }
  size_t pos = 0;
  for (int i = 0; i < argc && argv[i]; i++)
    skink_append_cmdline_arg(cmdline, &pos, argv[i]);
  cmdline[pos] = '\0';

  // Create an inheritable pipe; keep the read end non-inheritable.
  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = NULL;
  sa.bInheritHandle = TRUE;
  HANDLE read_h = NULL, write_h = NULL;
  if (!CreatePipe(&read_h, &write_h, &sa, 0)) {
    free(cmdline);
    *status_out = -1;
    return NULL;
  }
  SetHandleInformation(read_h, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFO si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = write_h;
  si.hStdError = write_h;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  memset(&pi, 0, sizeof(pi));

  // Search PATH like _spawnvp did: lpApplicationName NULL + argv[0]
  // resolves through the standard CreateProcess search order.
  BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL,
                           &si, &pi);
  free(cmdline);
  CloseHandle(write_h);
  if (!ok) {
    CloseHandle(read_h);
    *status_out = -1;
    return NULL;
  }

  // Read child stdout from the pipe (capped at SKINK_EXEC_OUT_MAX so an
// unbounded child cannot exhaust memory — comp_error.md #153).
  size_t cap = 4096;
  size_t len = 0;
  char *buf = (char *)malloc(cap);
  if (!buf) {
    CloseHandle(read_h);
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    *status_out = -1;
    return NULL;
  }
  for (;;) {
    if (len >= SKINK_EXEC_OUT_MAX)
      break;
    if (len == cap) {
      size_t newcap = cap * 2;
      if (newcap > SKINK_EXEC_OUT_MAX)
        newcap = SKINK_EXEC_OUT_MAX;
      char *tmp = (char *)realloc(buf, newcap);
      if (!tmp) {
        free(buf);
        CloseHandle(read_h);
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        *status_out = -1;
        return NULL;
      }
      buf = tmp;
      cap = newcap;
    }
    DWORD n = 0;
    if (!ReadFile(read_h, buf + len, (DWORD)(cap - len), &n, NULL) ||
        n == 0)
      break;
    len += (size_t)n;
  }
  CloseHandle(read_h);

  // Null-terminate.
  if (len == cap && cap < SKINK_EXEC_OUT_MAX) {
    char *tmp = (char *)realloc(buf, cap + 1);
    if (!tmp) {
      free(buf);
      TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, INFINITE);
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
      *status_out = -1;
      return NULL;
    }
    buf = tmp;
  }
  buf[len] = '\0';

  // Wait for the child to exit and collect its status.
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  *status_out = (int)code;
  return skink_rc_copy_out(buf, len);
}

#else

// --- POSIX implementation ---

#include <fcntl.h>
#include <sys/wait.h>

// skink_mark_inherited_cloexec marks every open fd above 2 (except
// except_fd — the pipe write end the child dup2s onto stdout) as
// close-on-exec so a spawned child never inherits the parent's
// descriptors — inherited fds leak into children and can be confused
// with the child's own streams (comp_error.md #153). Linux iterates
// /proc/self/fd; other POSIX platforms fall back to no-op (documented).
static void skink_mark_inherited_cloexec(int except_fd) {
#if defined(__linux__)
  DIR *d = opendir("/proc/self/fd");
  if (!d)
    return;
  int dfd = dirfd(d);
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (ent->d_name[0] == '.')
      continue;
    int fd = atoi(ent->d_name);
    if (fd <= 2 || fd == dfd || fd == except_fd)
      continue;
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0)
      fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  }
  closedir(d);
#else
  (void)except_fd;
#endif
}

// Skink_os_exec — execute a program with an argument array, without
// invoking a shell. argv is a NULL-terminated array of C strings.
// argv[0] is the program name (conventionally the path).
// Returns the child exit status on success, -1 on error.
int Skink_os_exec(const char **argv, int argc) {
  (void)argc;
  if (!argv || !argv[0]) {
    return -1;
  }
  pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    // Child: exec the program directly (no shell).
    execvp(argv[0], (char *const *)argv);
    // If execvp returns, it failed.
    _exit(127);
  }
  // Parent: wait for the child.
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return -1;
}

// Skink_os_exec_with_output — execute a program with an argument array,
// capturing stdout (capped at SKINK_EXEC_OUT_MAX). Returns the child
// exit status via *status_out and the captured stdout as an RC-managed
// string ("" if the child produced no output). On error returns NULL.
char *Skink_os_exec_with_output(const char **argv, int argc, int *status_out) {
  (void)argc;
  if (!argv || !argv[0] || !status_out) {
    if (status_out)
      *status_out = -1;
    return NULL;
  }
  int pipefd[2];
  if (pipe(pipefd) < 0) {
    *status_out = -1;
    return NULL;
  }
  // The read end must not leak into the child; the write end is dup2'd
  // onto stdout there, and the rest of the parent's descriptors are
  // marked close-on-exec so the child inherits nothing else.
  int pflags = fcntl(pipefd[0], F_GETFD);
  if (pflags >= 0)
    fcntl(pipefd[0], F_SETFD, pflags | FD_CLOEXEC);
  skink_mark_inherited_cloexec(pipefd[1]);
  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    *status_out = -1;
    return NULL;
  }
  if (pid == 0) {
    // Child: redirect stdout to the pipe, then exec.
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    execvp(argv[0], (char *const *)argv);
    _exit(127);
  }
  // Parent: read child stdout from the pipe.
  close(pipefd[1]);
  size_t cap = 4096;
  size_t len = 0;
  char *buf = (char *)malloc(cap);
  if (!buf) {
    close(pipefd[0]);
    waitpid(pid, NULL, 0); // reap the child to avoid a zombie
    *status_out = -1;
    return NULL;
  }
  for (;;) {
    if (len >= SKINK_EXEC_OUT_MAX)
      break;
    if (len == cap) {
      size_t newcap = cap * 2;
      if (newcap > SKINK_EXEC_OUT_MAX)
        newcap = SKINK_EXEC_OUT_MAX;
      char *tmp = (char *)realloc(buf, newcap);
      if (!tmp) {
        free(buf);
        close(pipefd[0]);
        waitpid(pid, NULL, 0); // reap the child to avoid a zombie
        *status_out = -1;
        return NULL;
      }
      buf = tmp;
      cap = newcap;
    }
    ssize_t n = read(pipefd[0], buf + len, cap - len);
    if (n <= 0)
      break;
    len += (size_t)n;
  }
  close(pipefd[0]);
  // Null-terminate.
  if (len == cap && cap < SKINK_EXEC_OUT_MAX) {
    char *tmp = (char *)realloc(buf, cap + 1);
    if (!tmp) {
      free(buf);
      waitpid(pid, NULL, 0); // reap the child to avoid a zombie
      *status_out = -1;
      return NULL;
    }
    buf = tmp;
  }
  buf[len] = '\0';

  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status)) {
    *status_out = WEXITSTATUS(status);
  } else {
    *status_out = -1;
  }
  return skink_rc_copy_out(buf, len);
}

#endif // _WIN32

// === Temporary files ===

// skink_mktemp_random mixes a pid/time/address/counter value so each
// candidate name is unpredictable without depending on rand state.
static unsigned long long skink_mktemp_random(int attempt) {
  unsigned long long r = (unsigned long long)time(NULL);
  r ^= (unsigned long long)(uintptr_t)&r >> 4;
#ifdef _WIN32
  r ^= (unsigned long long)_getpid() * 2654435761ULL;
#else
  r ^= (unsigned long long)getpid() * 2654435761ULL;
#endif
  r ^= (unsigned long long)attempt * 0x9E3779B97F4A7C15ULL;
  r ^= r >> 33;
  r *= 0xFF51AFD7ED558CCDULL;
  r ^= r >> 33;
  return r;
}

// Skink_fs_mktemp creates a uniquely-named file in the system temp
// directory and returns an RC-managed path string. Candidates are opened
// O_CREAT|O_EXCL|0600 so an attacker cannot pre-create or symlink the
// name — used by the compiler driver for its intermediate .ll file,
// which previously went to <outName>.ll in the working directory and
// could clobber a user file or collide predictably (VULN-01).
// Returns NULL on failure.
char *Skink_fs_mktemp(const char *prefix, const char *suffix) {
  if (!prefix)
    prefix = "skink";
  if (!suffix)
    suffix = "";
#ifdef _WIN32
  const char *tmpdir = getenv("TEMP");
  if (!tmpdir || !*tmpdir)
    tmpdir = getenv("TMP");
  if (!tmpdir || !*tmpdir)
    tmpdir = ".";
#else
  const char *tmpdir = getenv("TMPDIR");
  if (!tmpdir || !*tmpdir)
    tmpdir = "/tmp";
#endif
  size_t plen = strlen(tmpdir) + strlen(prefix) + 20 + strlen(suffix) + 3;
  for (int attempt = 0; attempt < 1000; attempt++) {
    char *path = (char *)malloc(plen);
    if (!path)
      return NULL;
    snprintf(path, plen, "%s/%s%llx%s", tmpdir, prefix,
             skink_mktemp_random(attempt), suffix);
#ifdef _WIN32
    int fd = _open(path, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
                   _S_IREAD | _S_IWRITE);
#else
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
#endif
    if (fd >= 0) {
#ifdef _WIN32
      _close(fd);
#else
      close(fd);
#endif
      return skink_rc_copy_out(path, strlen(path));
    }
    free(path);
    if (errno != EEXIST)
      return NULL;
  }
  return NULL;
}

// Skink_fs_mktemp_fopen creates a uniquely-named file in the system
// temp directory (O_CREAT|O_EXCL|0600) and immediately returns a FILE*
// opened for writing on that same descriptor — there is no close-and-
// reopen window for another process (same user) to swap the path for a
// symlink (comp_error.md #153). The created path is written into
// pathBuf (caller supplies at least 4096 bytes) so the caller can
// remove the file afterwards. Returns NULL on failure.
void *Skink_fs_mktemp_fopen(const char *prefix, const char *suffix,
                            char *pathBuf) {
  if (!prefix)
    prefix = "skink";
  if (!suffix)
    suffix = "";
#ifdef _WIN32
  const char *tmpdir = getenv("TEMP");
  if (!tmpdir || !*tmpdir)
    tmpdir = getenv("TMP");
  if (!tmpdir || !*tmpdir)
    tmpdir = ".";
#else
  const char *tmpdir = getenv("TMPDIR");
  if (!tmpdir || !*tmpdir)
    tmpdir = "/tmp";
#endif
  size_t plen = strlen(tmpdir) + strlen(prefix) + 20 + strlen(suffix) + 3;
  for (int attempt = 0; attempt < 1000; attempt++) {
    char *path = (char *)malloc(plen);
    if (!path)
      return NULL;
    snprintf(path, plen, "%s/%s%llx%s", tmpdir, prefix,
             skink_mktemp_random(attempt), suffix);
#ifdef _WIN32
    int fd = _open(path, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
                   _S_IREAD | _S_IWRITE);
#else
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
#endif
    if (fd >= 0) {
      FILE *fp = fdopen(fd, "w");
      if (!fp) {
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        free(path);
        return NULL;
      }
      if (pathBuf) {
        strncpy(pathBuf, path, 4095);
        pathBuf[4095] = '\0';
      }
      free(path);
      return fp;
    }
    free(path);
    if (errno != EEXIST)
      return NULL;
  }
  return NULL;
}

// Skink_fs_mktemp_in is Skink_fs_mktemp with a caller-supplied
// directory (the LSP must place its check file beside the source so
// relative imports resolve; the system temp dir would break them).
// Same O_CREAT|O_EXCL|0600 contract. Returns an RC-managed path string.
char *Skink_fs_mktemp_in(const char *dir, const char *prefix,
                         const char *suffix) {
  if (!dir || !*dir)
    return NULL;
  if (!prefix)
    prefix = "skink";
  if (!suffix)
    suffix = "";
  size_t plen = strlen(dir) + strlen(prefix) + 20 + strlen(suffix) + 3;
  for (int attempt = 0; attempt < 1000; attempt++) {
    char *path = (char *)malloc(plen);
    if (!path)
      return NULL;
    snprintf(path, plen, "%s/%s%llx%s", dir, prefix,
             skink_mktemp_random(attempt), suffix);
#ifdef _WIN32
    int fd = _open(path, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
                   _S_IREAD | _S_IWRITE);
#else
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
#endif
    if (fd >= 0) {
#ifdef _WIN32
      _close(fd);
#else
      close(fd);
#endif
      return skink_rc_copy_out(path, strlen(path));
    }
    free(path);
    if (errno != EEXIST)
      return NULL;
  }
  return NULL;
}

// === Signal handling ===
//
// Skink_signal_handle registers a Skink function (passed as a C function
// pointer taking no arguments) as the handler for a POSIX signal. The
// trampoline table keeps one slot per signal number 0..63.

#ifndef _WIN32
#include <signal.h>

// skink_signal_handlers stores the registered Skink closure handle per
// signal number. A Skink fn() value is NOT a C function pointer: it is a
// pointer to a block whose first field is the code pointer, invoked as
// code(handle). Treating the handle itself as the callee executed heap
// data as code (VULN-05).
static void *skink_signal_handlers[64];

// skink_signal_tramp is the C signal handler trampoline that dispatches to the registered Skink callback.
static void skink_signal_tramp(int sig) {
  if (sig >= 0 && sig < 64 && skink_signal_handlers[sig]) {
    void *handle = skink_signal_handlers[sig];
    void (*code)(void *) = *(void (**)(void *))handle;
    // NOTE: a Skink handler that allocates (RC ops, slice growth,
    // string concat) is not async-signal-safe — handlers should only
    // set flags or do trivial work. Documented in std/os.
    code(handle);
  }
}

// Skink_signal_handle registers a Skink function (closure handle) as the
// handler for the given POSIX signal.
int Skink_signal_handle(int sig, void *fn) {
  if (sig < 0 || sig >= 64 || !fn) {
    return -1;
  }
  skink_signal_handlers[sig] = fn;
  return signal(sig, skink_signal_tramp) == SIG_ERR ? -1 : 0;
}

#else

// Skink_signal_handle is a stub that always fails on Windows (signal handling not yet supported).
int Skink_signal_handle(int sig, void *fn) {
  (void)sig;
  (void)fn;
  return -1; // not supported on Windows yet
}

#endif

// === Embedded filesystem ===
//
// `embed "dir"` declarations bake file contents into the compiled binary.
// The emitter emits @_skink_embed_init, which runs before main and calls
// Skink_embed_register once per embedded file. std/fs routes paths with an
// "int:" prefix to this table, so embedded data is readable through the
// ordinary fs API with no registration step in user code.

// SkinkEmbedEntry is one embedded file: its virtual path plus the byte
// blob. The blob is emitted as an LLVM constant and is NOT NUL-terminated —
// length is authoritative.
typedef struct {
  const char *path;
  const char *data;
  long long   len;
} SkinkEmbedEntry;

static SkinkEmbedEntry *skink_embed_entries = NULL;
static int skink_embed_count_v = 0;
static int skink_embed_cap_v = 0;

// Skink_embed_register adds an embedded file to the table. First
// registration of a path wins — later duplicates are ignored.
void Skink_embed_register(const char *path, const char *data, long long len) {
  if (!path || !data) {
    return;
  }
  for (int i = 0; i < skink_embed_count_v; i++) {
    if (strcmp(skink_embed_entries[i].path, path) == 0) {
      return;
    }
  }
  if (skink_embed_count_v >= skink_embed_cap_v) {
    int cap = skink_embed_cap_v ? skink_embed_cap_v * 2 : 32;
    SkinkEmbedEntry *p =
        (SkinkEmbedEntry *)realloc(skink_embed_entries, (size_t)cap * sizeof(SkinkEmbedEntry));
    if (!p) {
      return;
    }
    skink_embed_entries = p;
    skink_embed_cap_v = cap;
  }
  skink_embed_entries[skink_embed_count_v].path = path;
  skink_embed_entries[skink_embed_count_v].data = data;
  skink_embed_entries[skink_embed_count_v].len = len;
  skink_embed_count_v++;
}

// Skink_embed_ptr returns the embedded bytes for path, or NULL if absent.
const char *Skink_embed_ptr(const char *path) {
  if (!path) {
    return NULL;
  }
  for (int i = 0; i < skink_embed_count_v; i++) {
    if (strcmp(skink_embed_entries[i].path, path) == 0) {
      return skink_embed_entries[i].data;
    }
  }
  return NULL;
}

// Skink_embed_size returns the embedded byte count for path, or -1 if absent.
long long Skink_embed_size(const char *path) {
  if (!path) {
    return -1;
  }
  for (int i = 0; i < skink_embed_count_v; i++) {
    if (strcmp(skink_embed_entries[i].path, path) == 0) {
      return skink_embed_entries[i].len;
    }
  }
  return -1;
}

// Skink_embed_count returns the number of registered embedded files.
int Skink_embed_count(void) { return skink_embed_count_v; }

// Skink_embed_path returns the virtual path of the index-th embedded file,
// or NULL when index is out of range.
const char *Skink_embed_path(int index) {
  if (index < 0 || index >= skink_embed_count_v) {
    return NULL;
  }
  return skink_embed_entries[index].path;
}

// Skink_embed_isdir reports whether path names an embedded directory —
// i.e. any embedded file lives under path + "/". The empty string and
// "/" name the virtual root and report a directory whenever the table
// is non-empty.
int Skink_embed_isdir(const char *path) {
  if (!path) {
    return 0;
  }
  size_t n = strlen(path);
  if (n == 0 || (n == 1 && path[0] == '/')) {
    return skink_embed_count_v > 0 ? 1 : 0;
  }
  for (int i = 0; i < skink_embed_count_v; i++) {
    const char *p = skink_embed_entries[i].path;
    if (strncmp(p, path, n) == 0 && p[n] == '/') {
      return 1;
    }
  }
  return 0;
}

// Skink_fs_seek64/Skink_fs_tell64 provide 64-bit file positioning on all
// platforms: plain fseek/ftell use `long`, which is only 32-bit on Windows,
// so seeks and sizes past 2 GiB silently truncated (VULNERABILITIES.md
// ">2GiB file ops" limitation). Windows uses _fseeki64/_ftelli64; POSIX
// uses fseeko/ftello with off_t (64-bit under _FILE_OFFSET_BITS=64).
int Skink_fs_seek64(void *fp, int64_t offset, int whence) {
#ifdef _WIN32
  return _fseeki64((FILE *)fp, offset, whence);
#else
  return fseeko((FILE *)fp, (off_t)offset, whence);
#endif
}

int64_t Skink_fs_tell64(void *fp) {
#ifdef _WIN32
  return (int64_t)_ftelli64((FILE *)fp);
#else
  return (int64_t)ftello((FILE *)fp);
#endif
}
