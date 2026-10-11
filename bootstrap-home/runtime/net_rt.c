// Copyright 2026 Mark Oxley. All rights reserved.
//
// Project: Skink — A self-hosted, statically-typed programming language
//          targeting LLVM IR, designed for systems programming with
//          modern ergonomics and a Go-inspired syntax.
//
// File:    std/runtime/net_rt.c
// Purpose: Runtime wrappers for POSIX network calls.
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

// Runtime wrappers for POSIX network calls (cross-platform: Linux/macOS/Windows).

#include "skink_compat.h"

#ifdef _WIN32

// === Windows (Winsock2) ===
#include <stdio.h>
#include <string.h>

// WSAStartup is called lazily by Skink_net_init() (defined in skink_compat.h).
// We use a constructor to ensure WSAStartup runs before any socket call.
static void __attribute__((constructor)) skink_net_auto_init(void) {
  Skink_net_init();
}

// Skink_close_fd — close a socket or file descriptor.
// On Windows, sockets must be closed with closesocket().
// _close is already provided by the MinGW C runtime (io.h) for file descriptors.
int Skink_close_fd(int fd) { return closesocket((SOCKET)fd); }

// Skink_set_socket_timeout sets receive and send timeouts on a socket.
// timeoutMs is the timeout in milliseconds; 0 disables timeout.
int Skink_set_socket_timeout(int fd, int timeoutMs) {
  if (timeoutMs <= 0)
    return 0;
  DWORD tv = (DWORD)timeoutMs;
  int rc1 = setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
  int rc2 = setsockopt((SOCKET)fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
  if (rc1 == SOCKET_ERROR || rc2 == SOCKET_ERROR)
    return -1;
  return 0;
}

// Skink_set_socket_timeouts sets independent receive and send timeouts.
// Each timeout is in milliseconds; <= 0 leaves that direction blocking.
int Skink_set_socket_timeouts(int fd, int recvMs, int sendMs) {
  DWORD tv;
  int rc = 0;
  if (recvMs > 0) {
    tv = (DWORD)recvMs;
    rc |= setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
  }
  if (sendMs > 0) {
    tv = (DWORD)sendMs;
    rc |= setsockopt((SOCKET)fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
  }
  return rc == SOCKET_ERROR ? -1 : 0;
}

// Skink_set_reuseaddr sets SO_REUSEADDR so a server can rebind its port
// immediately after restart while old connections linger in TIME_WAIT.
int Skink_set_reuseaddr(int fd) {
  int on = 1;
  return setsockopt((SOCKET)fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
}

// Skink_send_all loops send() until every byte is written or the socket
// fails. send() may return short counts when the kernel buffer fills, so
// callers that ignore the return value silently truncate responses
// (comp_error.md #47). Returns total bytes sent, or -1 on error before
// any byte was sent. Winsock has no MSG_NOSIGNAL; SIGPIPE does not exist.
int Skink_send_all(int fd, const char *buf, int len) {
  int sent = 0;
  while (sent < len) {
    int n = send((SOCKET)fd, buf + sent, len - sent, 0);
    if (n == SOCKET_ERROR) {
      if (WSAGetLastError() == WSAEINTR)
        continue;
      if (sent > 0)
        break;
      return -1;
    }
    if (n == 0)
      break;
    sent += n;
  }
  return sent;
}

// Skink_net_resolve_and_connect resolves a hostname (or IP) and connects
// to it on the given port with an optional timeout. Returns a socket fd
// on success, -1 on error. timeoutMs of 0 means no timeout (blocking).
int Skink_net_resolve_and_connect(const char *host, int port, int timeoutMs) {
  Skink_net_init();

  struct addrinfo hints;
  struct addrinfo *result = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;       // IPv4
  hints.ai_socktype = SOCK_STREAM; // TCP

  char portStr[16];
  snprintf(portStr, sizeof(portStr), "%d", port);

  int rc = getaddrinfo(host, portStr, &hints, &result);
  if (rc != 0 || !result) {
    return -1;
  }

  int fd = -1;
  struct addrinfo *rp;
  for (rp = result; rp != NULL; rp = rp->ai_next) {
    SOCKET sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sock == INVALID_SOCKET)
      continue;

    if (timeoutMs > 0) {
      // Set non-blocking mode
      u_long mode = 1;
      ioctlsocket(sock, FIONBIO, &mode);
      rc = connect(sock, rp->ai_addr, (int)rp->ai_addrlen);
      if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(sock);
        continue;
      }
      if (rc == SOCKET_ERROR) {
        // Wait for the socket to become writable
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(sock, &writefds);
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        rc = select(0, NULL, &writefds, NULL, &tv);
        if (rc <= 0) {
          closesocket(sock);
          continue;
        }
        // Check for connect error
        int err = 0;
        int errlen = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen);
        if (err != 0) {
          closesocket(sock);
          continue;
        }
      }
      // Restore blocking mode
      mode = 0;
      ioctlsocket(sock, FIONBIO, &mode);
    } else {
      rc = connect(sock, rp->ai_addr, (int)rp->ai_addrlen);
      if (rc == SOCKET_ERROR) {
        closesocket(sock);
        continue;
      }
    }
    fd = (int)sock;
    break; // Success
  }

  freeaddrinfo(result);
  return fd;
}

#else

// === POSIX (Linux, macOS) ===
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

// skink_net_auto_init ignores SIGPIPE process-wide: OpenSSL's socket
// BIO uses plain write() and several code paths write without
// MSG_NOSIGNAL, so a peer disconnecting mid-write would otherwise kill
// the whole server process (comp_error.md #70). Winsock has no SIGPIPE.
static void __attribute__((constructor)) skink_net_auto_init(void) {
  signal(SIGPIPE, SIG_IGN);
}

// _close wraps the POSIX close() call for file descriptors.
int _close(int fd) { return close(fd); }

// Skink_close_fd — alias for _close on POSIX.
int Skink_close_fd(int fd) { return close(fd); }

// Skink_set_socket_timeout sets receive and send timeouts on a socket.
// timeoutMs is the timeout in milliseconds; 0 disables timeout.
int Skink_set_socket_timeout(int fd, int timeoutMs) {
  if (timeoutMs <= 0)
    return 0;
  struct timeval tv;
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  int rc1 = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  int rc2 = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if (rc1 < 0 || rc2 < 0)
    return -1;
  return 0;
}

// Skink_set_socket_timeouts sets independent receive and send timeouts.
// Each timeout is in milliseconds; <= 0 leaves that direction blocking
// (comp_error.md #48).
int Skink_set_socket_timeouts(int fd, int recvMs, int sendMs) {
  struct timeval tv;
  int rc = 0;
  if (recvMs > 0) {
    tv.tv_sec = recvMs / 1000;
    tv.tv_usec = (recvMs % 1000) * 1000;
    rc |= setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }
  if (sendMs > 0) {
    tv.tv_sec = sendMs / 1000;
    tv.tv_usec = (sendMs % 1000) * 1000;
    rc |= setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }
  return rc < 0 ? -1 : 0;
}

// Skink_set_reuseaddr sets SO_REUSEADDR so a server can rebind its port
// immediately after restart while old connections linger in TIME_WAIT
// (comp_error.md #50).
int Skink_set_reuseaddr(int fd) {
  int on = 1;
  return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
}

// Skink_send_all loops send() until every byte is written or the socket
// fails. send() may return short counts when the kernel buffer fills, so
// callers that ignore the return value silently truncate responses
// (comp_error.md #47). MSG_NOSIGNAL avoids SIGPIPE killing the process.
// Returns total bytes sent, or -1 on error before any byte was sent.
int Skink_send_all(int fd, const char *buf, int len) {
  int sent = 0;
  while (sent < len) {
    ssize_t n = send(fd, buf + sent, (size_t)(len - sent), MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (sent > 0)
        break;
      return -1;
    }
    if (n == 0)
      break;
    sent += (int)n;
  }
  return sent;
}

// Skink_net_resolve_and_connect resolves a hostname (or IP) and connects
// to it on the given port with an optional timeout. Returns a socket fd
// on success, -1 on error. timeoutMs of 0 means no timeout (blocking).
//
// NOTE: this is IPv4-only by design (AF_INET). IPv6 hosts fail to resolve.
// IPv6 support is a known limitation documented in the book.
int Skink_net_resolve_and_connect(const char *host, int port, int timeoutMs) {
  struct addrinfo hints;
  struct addrinfo *result = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;       // IPv4 only — see note above
  hints.ai_socktype = SOCK_STREAM; // TCP

  char portStr[16];
  snprintf(portStr, sizeof(portStr), "%d", port);

  int rc = getaddrinfo(host, portStr, &hints, &result);
  if (rc != 0 || !result) {
    return -1;
  }

  int fd = -1;
  struct addrinfo *rp;
  for (rp = result; rp != NULL; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0)
      continue;

    // If timeout is requested, set the socket to non-blocking, connect,
    // then use select to wait with a timeout.
    if (timeoutMs > 0) {
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        fd = -1;
        continue;
      }
      rc = connect(fd, rp->ai_addr, rp->ai_addrlen);
      if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        fd = -1;
        continue;
      }
      if (rc < 0) {
        // Wait for the socket to become writable
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(fd, &writefds);
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        rc = select(fd + 1, NULL, &writefds, NULL, &tv);
        if (rc <= 0) {
          // Timeout or error
          close(fd);
          fd = -1;
          continue;
        }
        // Check for connect error
        int err = 0;
        socklen_t errlen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 ||
            err != 0) {
          close(fd);
          fd = -1;
          continue;
        }
      }
      // Restore blocking mode
      fcntl(fd, F_SETFL, flags);
    } else {
      rc = connect(fd, rp->ai_addr, rp->ai_addrlen);
      if (rc < 0) {
        close(fd);
        fd = -1;
        continue;
      }
    }
    break; // Success
  }

  freeaddrinfo(result);
  return fd;
}

#endif // _WIN32
