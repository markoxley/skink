// Copyright 2026 Mark Oxley. All rights reserved.
//
// Skink cross-platform compatibility header.
//
// Provides abstraction macros and inline functions to allow the Skink C
// runtime to compile and run on both POSIX systems (Linux, macOS) and
// Windows (via MinGW-w64).
//
// Key abstractions:
//   - SKINK_CLOSE(fd)       — close a file descriptor or socket
//   - SKINK_USLEEP(us)      — sleep for N microseconds
//   - SKINK_INVALID_FD      — invalid file descriptor / socket value
//   - skink_sock_t          — portable socket type
//   - skink_socklen_t       — portable socklen_t type
//   - Skink_net_init()      — initialise networking (WSAStartup on Windows)
//   - Skink_net_cleanup()   — cleanup networking

#ifndef SKINK_COMPAT_H
#define SKINK_COMPAT_H

#ifdef _WIN32

// === Windows (MinGW-w64) ===

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <process.h>

// MinGW provides pthread.h via winpthreads — no change needed for pthread code.
// MinGW provides dirent.h — no change needed for dirent code.

// Socket type aliases
typedef SOCKET skink_sock_t;
typedef int skink_socklen_t;
#define SKINK_INVALID_FD (INVALID_SOCKET)
#define SKINK_SOCKET_ERROR SOCKET_ERROR

// Close a file descriptor or socket.
// On Windows, sockets must be closed with closesocket(), not close().
// We use a heuristic: Windows SOCKET values are typically large unsigned
// values (64-bit handles), while regular file descriptors are small
// non-negative integers. However, this is unreliable. Instead, we provide
// two macros: SKINK_CLOSE_FD for file descriptors and SKINK_CLOSE_SOCK
// for sockets.
#define SKINK_CLOSE_FD(fd) close(fd)
#define SKINK_CLOSE_SOCK(fd) closesocket(fd)

// usleep is not available on Windows. Use Sleep() (milliseconds).
// For sub-millisecond sleeps, use a busy-wait or Sleep(0) to yield.
static inline int skink_usleep(unsigned long us) {
  if (us == 0) {
    Sleep(0); // Yield time slice
    return 0;
  }
  if (us < 1000) {
    // Sub-millisecond: Sleep(0) to yield, since Windows Sleep granularity
    // is ~1ms. For very short sleeps, use a busy-wait loop.
    Sleep(0);
    return 0;
  }
  Sleep((DWORD)(us / 1000));
  return 0;
}
#define SKINK_USLEEP(us) skink_usleep(us)

// fcntl O_NONBLOCK equivalent on Windows uses ioctlsocket
#define SKINK_SET_NONBLOCK(fd) do { \
  u_long _mode = 1; \
  ioctlsocket((fd), FIONBIO, &_mode); \
} while (0)
#define SKINK_SET_BLOCK(fd) do { \
  u_long _mode = 0; \
  ioctlsocket((fd), FIONBIO, &_mode); \
} while (0)

// Access mode flags (Windows _access uses same values as POSIX for F_OK/R_OK/W_OK)
// Windows doesn't support X_OK meaningfully, but _access accepts it.
#define SKINK_ACCESS access

// mkdir: Windows _mkdir doesn't take a mode parameter
#define SKINK_MKDIR(path) _mkdir(path)
#define SKINK_MKDIR_MODE(path, mode) _mkdir(path)

// Pipe — Windows uses _pipe
#define SKINK_PIPE(fds) _pipe(fds, 0, 0)

// Read/write on pipes use _read/_write on Windows
#define SKINK_READ _read
#define SKINK_WRITE _write

// Fork/exec — Windows doesn't have fork. Use _spawnvp for simple exec.
// For exec with output capture, use CreateProcess with pipes.
// These are implemented in fs_rt.c with #ifdef guards.

// _exit is available on Windows
// _exit(1) works as expected

// Network initialisation
static inline int Skink_net_init(void) {
  WSADATA wsaData;
  static int initialised = 0;
  if (initialised) return 0;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    return -1;
  }
  initialised = 1;
  return 0;
}
static inline void Skink_net_cleanup(void) {
  WSACleanup();
}

// Errno equivalent for sockets on Windows
#define SKINK_SOCK_ERRNO WSAGetLastError()
#define SKINK_EINPROGRESS WSAEWOULDBLOCK

// SO_RCVTIMEO / SO_SNDTIMEO on Windows use DWORD (milliseconds) instead of struct timeval
// Handled in net_rt.c with #ifdef guards

#else

// === POSIX (Linux, macOS) ===

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

typedef int skink_sock_t;
typedef socklen_t skink_socklen_t;
#define SKINK_INVALID_FD (-1)
#define SKINK_SOCKET_ERROR (-1)

#define SKINK_CLOSE_FD(fd) close(fd)
#define SKINK_CLOSE_SOCK(fd) close(fd)

#define SKINK_USLEEP(us) usleep(us)

#define SKINK_SET_NONBLOCK(fd) do { \
  int _fl = fcntl((fd), F_GETFL, 0); \
  fcntl((fd), F_SETFL, _fl | O_NONBLOCK); \
} while (0)
#define SKINK_SET_BLOCK(fd) do { \
  int _fl = fcntl((fd), F_GETFL, 0); \
  fcntl((fd), F_SETFL, _fl & ~O_NONBLOCK); \
} while (0)

#define SKINK_ACCESS access
#define SKINK_MKDIR(path) mkdir((path), 0755)
#define SKINK_MKDIR_MODE(path, mode) mkdir((path), (mode_t)(mode))
#define SKINK_PIPE(fds) pipe(fds)
#define SKINK_READ read
#define SKINK_WRITE write

static inline int Skink_net_init(void) { return 0; }
static inline void Skink_net_cleanup(void) { /* no-op */ }

#define SKINK_SOCK_ERRNO errno
#define SKINK_EINPROGRESS EINPROGRESS

#endif // _WIN32

#endif // SKINK_COMPAT_H
