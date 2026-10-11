// Copyright 2026 Mark Oxley. All rights reserved.
//
// Project: Skink — A self-hosted, statically-typed programming language
//          targeting LLVM IR, designed for systems programming with
//          modern ergonomics and a Go-inspired syntax.
//
// File:    std/runtime/tls_rt.c
// Purpose: C runtime support for tls rt.
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

#include "skink_compat.h"
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
// close() for sockets is handled by closesocket() via skink_compat.h
// No unistd.h on Windows; SSL_set_fd works with Windows SOCKET values.
#define SKINK_MUTEX_T CRITICAL_SECTION
#define SKINK_MUTEX_INIT(m) InitializeCriticalSection(&(m))
#define SKINK_MUTEX_LOCK(m) EnterCriticalSection(&(m))
#define SKINK_MUTEX_UNLOCK(m) LeaveCriticalSection(&(m))
#else
#include <unistd.h>
#include <pthread.h>
#define SKINK_MUTEX_T pthread_mutex_t
#define SKINK_MUTEX_INIT(m) pthread_mutex_init(&(m), NULL)
#define SKINK_MUTEX_LOCK(m) pthread_mutex_lock(&(m))
#define SKINK_MUTEX_UNLOCK(m) pthread_mutex_unlock(&(m))
#endif

// === Server-side SNI registry (comp_error.md #67) ===
//
// A listener may serve several hostnames with different certificates.
// Skink_tls_ctx_add_cert loads a cert/key into a child SSL_CTX and
// records {parent ctx, hostname -> child ctx}; the servername callback
// installed by Skink_tls_ctx_create swaps the SSL's ctx when the
// client's SNI hostname matches.
typedef struct SniEntry {
  SSL_CTX *parent;
  char *host;
  SSL_CTX *child;
  struct SniEntry *next;
} SniEntry;

static SniEntry *sni_entries = NULL;
static SKINK_MUTEX_T sni_mu;
static int sni_mu_init_done = 0;

// sni_servername_cb is invoked during the TLS handshake when the client
// sends an SNI hostname. On a match the SSL object is rebound to the
// child ctx carrying that host's certificate.
static int sni_servername_cb(SSL *ssl, int *ad, void *arg) {
  (void)ad;
  (void)arg;
  const char *name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  if (!name)
    return SSL_TLSEXT_ERR_NOACK;
  SKINK_MUTEX_LOCK(sni_mu);
  SniEntry *e = sni_entries;
  SSL_CTX *cur = SSL_get_SSL_CTX(ssl);
  while (e) {
    if (e->parent == cur && strcasecmp(e->host, name) == 0) {
      SSL_set_SSL_CTX(ssl, e->child);
      break;
    }
    e = e->next;
  }
  SKINK_MUTEX_UNLOCK(sni_mu);
  // NOACK lets the handshake continue with the default cert when no
  // SNI name matches, rather than aborting the connection.
  return e ? SSL_TLSEXT_ERR_OK : SSL_TLSEXT_ERR_NOACK;
}

// alpn_select_cb picks "http/1.1" when the client offers it (the only
// protocol this stack speaks). Returning NOACK declines ALPN.
static int alpn_select_cb(SSL *ssl, const unsigned char **out,
                          unsigned char *outlen, const unsigned char *in,
                          unsigned int inlen, void *arg) {
  (void)ssl;
  (void)arg;
  static const unsigned char h1[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
  unsigned int i = 0;
  while (i < inlen) {
    unsigned int l = in[i];
    if (i + 1 + l <= inlen && l == 8 && memcmp(in + i + 1, "http/1.1", 8) == 0) {
      *out = h1 + 1;
      *outlen = 8;
      return SSL_TLSEXT_ERR_OK;
    }
    i += 1 + l;
  }
  return SSL_TLSEXT_ERR_NOACK;
}

// Skink_tls_ctx_create creates an SSL_CTX configured for server TLS.
void *Skink_tls_ctx_create() {
  const SSL_METHOD *method = TLS_server_method();
  SSL_CTX *ctx = SSL_CTX_new(method);
  if (!ctx) {
    return NULL;
  }
  if (!sni_mu_init_done) {
    SKINK_MUTEX_INIT(sni_mu);
    sni_mu_init_done = 1;
  }
  // Required for the server-side session cache / tickets (#67).
  SSL_CTX_set_session_id_context(ctx, (const unsigned char *)"skink", 5);
  SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, NULL);
  SSL_CTX_set_tlsext_servername_callback(ctx, sni_servername_cb);
  return ctx;
}

// Skink_tls_ctx_add_cert registers an additional certificate under a
// hostname for SNI-based selection. The hostname copy is owned by the
// registry. Returns 0 on success.
int Skink_tls_ctx_add_cert(void *ctx, const char *hostname,
                           const char *certPath, const char *keyPath) {
  if (!ctx || !hostname || !certPath || !keyPath)
    return -1;
  SSL_CTX *child = SSL_CTX_new(TLS_server_method());
  if (!child)
    return -1;
  if (SSL_CTX_use_certificate_file(child, certPath, SSL_FILETYPE_PEM) <= 0 ||
      SSL_CTX_use_PrivateKey_file(child, keyPath, SSL_FILETYPE_PEM) <= 0 ||
      SSL_CTX_check_private_key(child) != 1) {
    SSL_CTX_free(child);
    return -1;
  }
  SniEntry *e = (SniEntry *)malloc(sizeof(SniEntry));
  if (!e) {
    SSL_CTX_free(child);
    return -1;
  }
  e->parent = (SSL_CTX *)ctx;
  e->host = strdup(hostname);
  e->child = child;
  SKINK_MUTEX_LOCK(sni_mu);
  e->next = sni_entries;
  sni_entries = e;
  SKINK_MUTEX_UNLOCK(sni_mu);
  return 0;
}

// Skink_tls_ctx_use_certificate_file loads the certificate PEM file.
int Skink_tls_ctx_use_certificate_file(void *ctx, const char *certPath) {
  if (!ctx || !certPath)
    return -1;
  if (SSL_CTX_use_certificate_file((SSL_CTX *)ctx, certPath,
                                   SSL_FILETYPE_PEM) <= 0) {
    return -1;
  }
  return 0;
}

// Skink_tls_ctx_use_private_key_file loads the private key PEM file.
int Skink_tls_ctx_use_private_key_file(void *ctx, const char *keyPath) {
  if (!ctx || !keyPath)
    return -1;
  if (SSL_CTX_use_PrivateKey_file((SSL_CTX *)ctx, keyPath, SSL_FILETYPE_PEM) <=
      0) {
    return -1;
  }
  return 0;
}

// Skink_tls_ctx_check_private_key verifies that the private key matches the
// cert.
int Skink_tls_ctx_check_private_key(void *ctx) {
  if (!ctx)
    return -1;
  if (SSL_CTX_check_private_key((SSL_CTX *)ctx) != 1) {
    return -1;
  }
  return 0;
}

// Skink_tls_accept wraps an accepted socket fd in an SSL session.
void *Skink_tls_accept(void *ctx, int fd) {
  if (!ctx)
    return NULL;
  SSL *ssl = SSL_new((SSL_CTX *)ctx);
  if (!ssl)
    return NULL;
  SSL_set_fd(ssl, fd);
  if (SSL_accept(ssl) <= 0) {
    SSL_free(ssl);
    return NULL;
  }
  return ssl;
}

// Skink_tls_read reads data from an SSL connection.
int Skink_tls_read(void *ssl, char *buf, int len) {
  if (!ssl || !buf || len <= 0)
    return -1;
  return SSL_read((SSL *)ssl, buf, len);
}

// Skink_tls_write writes data to an SSL connection.
int Skink_tls_write(void *ssl, const char *buf, int len) {
  if (!ssl || !buf || len <= 0)
    return -1;
  return SSL_write((SSL *)ssl, buf, len);
}

// Skink_tls_write_all loops SSL_write until every byte is written or the
// connection fails. SSL_write may return short counts, so a single call
// can silently truncate large responses (comp_error.md #47). Retries on
// WANT_READ/WANT_WRITE (blocking sockets normally don't produce these,
// but retrying is harmless). Returns total bytes sent, or -1 on error.
int Skink_tls_write_all(void *ssl, const char *buf, int len) {
  if (!ssl || !buf || len <= 0)
    return -1;
  int sent = 0;
  while (sent < len) {
    int n = SSL_write((SSL *)ssl, buf + sent, len - sent);
    if (n <= 0) {
      int e = SSL_get_error((SSL *)ssl, n);
      if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
        continue;
      if (sent > 0)
        break;
      return -1;
    }
    sent += n;
  }
  return sent;
}

// Skink_tls_ctx_free frees the SSL_CTX and any SNI child ctxs
// registered via Skink_tls_ctx_add_cert.
void Skink_tls_ctx_free(void *ctx) {
  if (!ctx)
    return;
  SKINK_MUTEX_LOCK(sni_mu);
  SniEntry **pp = &sni_entries;
  while (*pp) {
    SniEntry *e = *pp;
    if (e->parent == (SSL_CTX *)ctx) {
      *pp = e->next;
      SSL_CTX_free(e->child);
      free(e->host);
      free(e);
    } else {
      pp = &e->next;
    }
  }
  SKINK_MUTEX_UNLOCK(sni_mu);
  SSL_CTX_free((SSL_CTX *)ctx);
}

// === TLS Client ===

// A single shared client SSL_CTX (comp_error.md #67): sessions are
// scoped to their SSL_CTX, so a per-connection ctx — as used before —
// made session resumption impossible. Created lazily on first use.
static SSL_CTX *client_ctx = NULL;
static SKINK_MUTEX_T client_mu;
static int client_mu_init_done = 0;

// Session cache: key "host:port" -> SSL_SESSION*, bounded and mutex
// guarded. 64 entries is ample for an HTTP client.
#define SKINK_SESS_CACHE_SIZE 64
typedef struct {
  char *key;
  SSL_SESSION *sess;
} SessEntry;
static SessEntry sess_cache[SKINK_SESS_CACHE_SIZE];
static int sess_cache_count = 0;

// sess_lookup returns a held reference to a cached session for key, or
// NULL. Caller must SSL_SESSION_free it.
static SSL_SESSION *sess_lookup(const char *key) {
  SSL_SESSION *s = NULL;
  SKINK_MUTEX_LOCK(client_mu);
  for (int i = 0; i < sess_cache_count; i++) {
    if (strcmp(sess_cache[i].key, key) == 0) {
      s = sess_cache[i].sess;
      if (s)
        SSL_SESSION_up_ref(s);
      break;
    }
  }
  SKINK_MUTEX_UNLOCK(client_mu);
  return s;
}

// sess_store caches sess under key (taking its own reference),
// replacing any older session for the same key.
static void sess_store(const char *key, SSL_SESSION *sess) {
  if (!sess || !SSL_SESSION_is_resumable(sess)) {
    if (sess)
      SSL_SESSION_free(sess);
    return;
  }
  SKINK_MUTEX_LOCK(client_mu);
  int slot = -1;
  for (int i = 0; i < sess_cache_count; i++) {
    if (strcmp(sess_cache[i].key, key) == 0) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    if (sess_cache_count == SKINK_SESS_CACHE_SIZE) {
      // Evict the oldest entry.
      slot = 0;
    } else {
      slot = sess_cache_count++;
    }
  }
  if (sess_cache[slot].sess)
    SSL_SESSION_free(sess_cache[slot].sess);
  free(sess_cache[slot].key);
  sess_cache[slot].key = strdup(key);
  sess_cache[slot].sess = sess;
  SKINK_MUTEX_UNLOCK(client_mu);
}

// The "host:port" cache key is attached via SSL_set_app_data so the
// session can be harvested at close time — TLS 1.3 resumption tickets
// arrive post-handshake, so the session is captured in
// Skink_tls_close rather than at connect (#67).
static void client_ctx_init_locked(void) {
  client_ctx = SSL_CTX_new(TLS_client_method());
  if (!client_ctx)
    return;
  SSL_CTX_set_default_verify_paths(client_ctx);
  SSL_CTX_set_verify(client_ctx, SSL_VERIFY_PEER, NULL);
  SSL_CTX_set_session_cache_mode(client_ctx, SSL_SESS_CACHE_CLIENT);
}

// Skink_tls_client_connect_ex performs a client TLS handshake over fd.
// Sends SNI (SSL_set_tlsext_host_name), enables hostname verification
// when verify != 0, offers ALPN "http/1.1", and attempts session
// resumption from the shared cache keyed by hostname:port.
// Returns an SSL* on success, NULL on failure.
void *Skink_tls_client_connect_ex(int fd, const char *hostname, int port,
                                  int verify) {
  if (!client_mu_init_done) {
    SKINK_MUTEX_INIT(client_mu);
    client_mu_init_done = 1;
  }
  SKINK_MUTEX_LOCK(client_mu);
  if (!client_ctx)
    client_ctx_init_locked();
  SSL_CTX *ctx = client_ctx;
  SKINK_MUTEX_UNLOCK(client_mu);
  if (!ctx)
    return NULL;

  SSL *ssl = SSL_new(ctx);
  if (!ssl)
    return NULL;
  SSL_set_fd(ssl, fd);
  if (verify == 0) {
    // Test/dev path: accept self-signed or mismatched certs.
    SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL);
  }
  if (hostname && hostname[0]) {
    // Actually send the SNI extension (#67) — the old code only set
    // SSL_set1_host, which is verification metadata, not wire SNI.
    SSL_set_tlsext_host_name(ssl, hostname);
    if (verify != 0) {
      SSL_set1_host(ssl, hostname);
    }
  }
  // Advertise http/1.1 via ALPN (wire format: length-prefixed list).
  static const unsigned char alpn_h1[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
  SSL_set_alpn_protos(ssl, alpn_h1, sizeof(alpn_h1));

  // Session resumption: reuse a cached session for this host:port.
  char keybuf[300];
  snprintf(keybuf, sizeof(keybuf), "%s:%d", hostname ? hostname : "", port);
  SSL_SESSION *cached = sess_lookup(keybuf);
  if (cached) {
    SSL_set_session(ssl, cached);
    SSL_SESSION_free(cached);
  }
  SSL_set_app_data(ssl, strdup(keybuf));

  if (SSL_connect(ssl) <= 0) {
    free(SSL_get_app_data(ssl));
    SSL_free(ssl);
    return NULL;
  }
  if (verify != 0) {
    X509 *cert = SSL_get_peer_certificate(ssl);
    if (!cert) {
      // No certificate presented — fail closed.
      SSL_shutdown(ssl);
      free(SSL_get_app_data(ssl));
      SSL_free(ssl);
      return NULL;
    }
    X509_free(cert);
  }
  return ssl;
}

// Skink_tls_client_connect preserves the original signature: verifying
// connection assuming port 443 for the session-cache key.
void *Skink_tls_client_connect(int fd, const char *hostname) {
  return Skink_tls_client_connect_ex(fd, hostname, 443, 1);
}

// Skink_tls_session_reused reports whether the connection resumed a
// cached session (test hook for #67).
int Skink_tls_session_reused(void *ssl) {
  if (!ssl)
    return 0;
  return SSL_session_reused((SSL *)ssl);
}

// skink_rc_strdup copies a C string into an RC-managed block so the
// returned Skink string is reclaimed by ordinary RC release instead of
// leaking as an untracked malloc (comp_error.md #104). NULL maps to "".
extern void *Skink_rc_alloc(int64_t payload_size);
static char *skink_rc_strdup(const char *s) {
  if (!s)
    s = "";
  size_t n = strlen(s) + 1;
  char *out = (char *)Skink_rc_alloc((int64_t)n);
  if (!out)
    return NULL;
  memcpy(out, s, n);
  return out;
}

// Skink_tls_get_alpn returns the ALPN protocol negotiated on the
// connection as an RC-managed string ("" if none).
char *Skink_tls_get_alpn(void *ssl) {
  if (!ssl)
    return skink_rc_strdup("");
  const unsigned char *p = NULL;
  unsigned int len = 0;
  SSL_get0_alpn_selected((SSL *)ssl, &p, &len);
  char *out = (char *)Skink_rc_alloc((int64_t)len + 1);
  if (!out)
    return NULL;
  memcpy(out, p, len);
  out[len] = 0;
  return out;
}

// Skink_tls_get_servername returns the SNI hostname the client sent on
// this server-side connection (RC-managed; "" if none).
char *Skink_tls_get_servername(void *ssl) {
  if (!ssl)
    return skink_rc_strdup("");
  const char *name = SSL_get_servername((SSL *)ssl, TLSEXT_NAMETYPE_host_name);
  return skink_rc_strdup(name);
}

// Skink_tls_peer_cert_cn returns the peer certificate's Common Name as
// an RC-managed string ("" if unavailable). Used by tests to verify SNI
// selected the right certificate.
char *Skink_tls_peer_cert_cn(void *ssl) {
  if (!ssl)
    return skink_rc_strdup("");
  X509 *cert = SSL_get_peer_certificate((SSL *)ssl);
  if (!cert)
    return skink_rc_strdup("");
  char buf[256];
  buf[0] = 0;
  X509_NAME *name = X509_get_subject_name(cert);
  if (name) {
    X509_NAME_get_text_by_NID(name, NID_commonName, buf, sizeof(buf));
  }
  X509_free(cert);
  return skink_rc_strdup(buf);
}

// Skink_tls_close frees the SSL session and shuts down the connection.
// Before teardown it stores a resumable session in the client cache —
// TLS 1.3 tickets arrive after the handshake during reads, so this is
// the earliest point a resumable session is guaranteed (#67).
void Skink_tls_close(void *ssl) {
  if (!ssl)
    return;
  char *key = (char *)SSL_get_app_data((SSL *)ssl);
  if (key) {
    sess_store(key, SSL_get1_session((SSL *)ssl));
    free(key);
    SSL_set_app_data((SSL *)ssl, NULL);
  }
  SSL_shutdown((SSL *)ssl);
  SSL_free((SSL *)ssl);
}

