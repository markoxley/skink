// Copyright 2026 Mark Oxley. All rights reserved.
//
// Project: Skink — A self-hosted, statically-typed programming language
//          targeting LLVM IR, designed for systems programming with
//          modern ergonomics and a Go-inspired syntax.
//
// File:    std/runtime/conc_rt.c
// Purpose: C runtime support for conc rt.
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
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// Skink_panic is provided by panic_rt.c; channel operations panic on
// misuse (send-on-closed, double-close) matching Go semantics.
extern void Skink_panic(char *msg);
extern void *Skink_rc_alloc_typed(int64_t payload_size, void *dtor_ptr);
extern void Skink_rc_retain(void *ptr);
extern void Skink_rc_release(void *ptr);
#include <stdint.h>
#ifdef _WIN32
// close() for file descriptors provided by io.h via skink_compat.h
#else
#include <unistd.h>
#endif

// === Channel implementation ===

typedef struct ChanNode {
  void *data;
  struct ChanNode *next;
} ChanNode;

// Chan represents a thread-safe channel for goroutine-like communication.
// Channels are RC-allocated: the emitter treats them as i8* heap values,
// so every channel variable/field holds a reference and the destructor
// runs when the last reference is released.
typedef struct {
  int elem_size;
  int capacity;
  int closed;
  int live; // set once all sync primitives are initialized
  int recv_waiters; // receivers blocked in recv_cv or registered by select
  ChanNode *head;
  ChanNode *tail;
  int count;
  pthread_mutex_t mu;
  pthread_cond_t send_cv;
  pthread_cond_t recv_cv;
} Chan;

// chan_block_forever parks the calling thread permanently — the Go
// semantics for a send or receive on a nil channel. There is no object
// to wait on, so a sleep loop is the whole implementation. (Go detects
// the all-goroutines-asleep case and panics; Skink has no scheduler, so
// a nil-channel op simply hangs.)
static void chan_block_forever(void) {
  for (;;) {
    SKINK_USLEEP(1000000);
  }
}

// Skink_chan_destroy is the RC destructor for channels. It drains any
// still-queued nodes, releasing each element box (and the element's own
// reference when the element is a pointer — the send path retains heap
// elements for the buffer, so a never-received element's reference is
// owned by the queue). Skink_rc_release on a non-RC first word (e.g. an
// int element reinterpreted as a pointer) is a safe no-op via the RC
// registry check.
void Skink_chan_destroy(void *ch_ptr) {
  Chan *ch = (Chan *)ch_ptr;
  ChanNode *n = ch->head;
  while (n) {
    ChanNode *next = n->next;
    if (ch->elem_size >= (int)sizeof(void *) && n->data)
      Skink_rc_release(*(void **)n->data);
    free(n->data);
    free(n);
    n = next;
  }
  if (ch->live) {
    pthread_mutex_destroy(&ch->mu);
    pthread_cond_destroy(&ch->send_cv);
    pthread_cond_destroy(&ch->recv_cv);
  }
}

// Skink_chan_make creates a new channel with the given element size and capacity.
void *Skink_chan_make(int elem_size, int capacity) {
  Chan *ch = Skink_rc_alloc_typed(sizeof(Chan), (void *)Skink_chan_destroy);
  if (!ch)
    return NULL;
  ch->elem_size = elem_size;
  ch->capacity = capacity;
  if (pthread_mutex_init(&ch->mu, NULL) != 0) {
    Skink_rc_release(ch);
    return NULL;
  }
  if (pthread_cond_init(&ch->send_cv, NULL) != 0) {
    pthread_mutex_destroy(&ch->mu);
    Skink_rc_release(ch);
    return NULL;
  }
  if (pthread_cond_init(&ch->recv_cv, NULL) != 0) {
    pthread_cond_destroy(&ch->send_cv);
    pthread_mutex_destroy(&ch->mu);
    Skink_rc_release(ch);
    return NULL;
  }
  ch->live = 1;
  return ch;
}

// Skink_chan_send sends a value into the channel. Capacity semantics:
//   capacity > 0 — bounded: the send blocks while the buffer is full.
//   capacity == 0 — rendezvous (Go unbuffered): the value is enqueued,
//     then the send blocks until a receiver has consumed it.
//   capacity < 0 — unbounded queue: the send never blocks (Skink
//     extension, e.g. make(chan<T>, -1)).
// Sending on a closed channel panics (Go semantics); the mutex is
// released before Skink_panic because it longjmps and never returns.
// Sending on a nil channel blocks forever.
void Skink_chan_send(void *ch_ptr, void *val) {
  if (!ch_ptr)
    chan_block_forever();
  Chan *ch = (Chan *)ch_ptr;
  int rendezvous = ch->capacity == 0;
  pthread_mutex_lock(&ch->mu);
  // Wait for a free slot: a full bounded buffer, or — for rendezvous —
  // an un-consumed previous handoff (queue depth stays <= 1).
  while (((ch->capacity > 0 && ch->count >= ch->capacity) ||
          (rendezvous && ch->count > 0)) &&
         !ch->closed) {
    pthread_cond_wait(&ch->send_cv, &ch->mu);
  }
  if (ch->closed) {
    pthread_mutex_unlock(&ch->mu);
    Skink_panic((char *)"send on closed channel");
    return;
  }
  ChanNode *node = malloc(sizeof(ChanNode));
  if (!node) {
    pthread_mutex_unlock(&ch->mu);
    return;
  }
  node->data = malloc(ch->elem_size);
  if (!node->data) {
    free(node);
    pthread_mutex_unlock(&ch->mu);
    return;
  }
  memcpy(node->data, val, ch->elem_size);
  node->next = NULL;
  if (ch->tail) {
    ch->tail->next = node;
    ch->tail = node;
  } else {
    ch->head = ch->tail = node;
  }
  ch->count++;
  pthread_cond_signal(&ch->recv_cv);
  if (rendezvous) {
    // The send completes only once a receiver has consumed the value.
    // A close during the wait abandons the undelivered value — panic as
    // a send on a closed channel; a consumed send completes normally
    // even if the channel has since closed.
    while (ch->count > 0 && !ch->closed) {
      pthread_cond_wait(&ch->send_cv, &ch->mu);
    }
    if (ch->count > 0) {
      pthread_mutex_unlock(&ch->mu);
      Skink_panic((char *)"send on closed channel");
      return;
    }
  }
  pthread_mutex_unlock(&ch->mu);
}

// Skink_chan_recv receives a value from the channel, blocking until data is available or the channel is closed.
// Receiving on a nil channel blocks forever.
void *Skink_chan_recv(void *ch_ptr) {
  if (!ch_ptr)
    chan_block_forever();
  Chan *ch = (Chan *)ch_ptr;
  pthread_mutex_lock(&ch->mu);
  while (ch->head == NULL && !ch->closed) {
    // Advertise a blocked receiver: select's rendezvous send-readiness
    // check requires recv_waiters > 0 to fire a cap-0 send case.
    ch->recv_waiters++;
    pthread_cond_wait(&ch->recv_cv, &ch->mu);
    ch->recv_waiters--;
  }
  if (ch->head == NULL) {
    pthread_mutex_unlock(&ch->mu);
    return NULL;
  }
  ChanNode *node = ch->head;
  void *data = node->data;
  ch->head = node->next;
  if (ch->head == NULL)
    ch->tail = NULL;
  ch->count--;
  free(node);
  pthread_cond_signal(&ch->send_cv);
  pthread_mutex_unlock(&ch->mu);
  return data;
}

// Skink_chan_close closes the channel, waking all blocked senders and
// receivers. Closing an already-closed channel panics (Go semantics);
// closing a nil channel panics too ("close of nil channel").
void Skink_chan_close(void *ch_ptr) {
  if (!ch_ptr) {
    Skink_panic((char *)"close of nil channel");
    return;
  }
  Chan *ch = (Chan *)ch_ptr;
  pthread_mutex_lock(&ch->mu);
  if (ch->closed) {
    pthread_mutex_unlock(&ch->mu);
    Skink_panic((char *)"close of closed channel");
    return;
  }
  ch->closed = 1;
  pthread_cond_broadcast(&ch->recv_cv);
  pthread_cond_broadcast(&ch->send_cv);
  pthread_mutex_unlock(&ch->mu);
}

// Skink_chan_len returns the number of buffered elements in the channel.
int Skink_chan_len(void *ch_ptr) {
  Chan *ch = (Chan *)ch_ptr;
  if (!ch)
    return 0;
  pthread_mutex_lock(&ch->mu);
  int n = ch->count;
  pthread_mutex_unlock(&ch->mu);
  return n;
}

// Skink_chan_select polls multiple channels for readiness and performs the first available send or receive.
// Nil-channel cases never become ready, so assigning nil to a channel
// variable disables its select case. Receive cases register as
// recv_waiters for the duration of the call so a rendezvous (cap 0) send
// — here or in another thread's select — can observe a waiter and pair.
int Skink_chan_select(int n, void **chs, int *is_send, void **vals,
                      void **out_val, int timeout_ms) {
  // Select: poll each channel for readiness.
  // timeout_ms == -1: block forever until a channel is ready
  // timeout_ms == 0:  poll once, return -1 if nothing ready (non-blocking)
  // timeout_ms > 0:   poll until timeout expires
  // For receive: check if data is available or channel is closed.
  // For send: readiness depends on capacity (see the poll loop below).
  for (int i = 0; i < n; i++) {
    if (!is_send[i] && chs[i]) {
      Chan *c = (Chan *)chs[i];
      pthread_mutex_lock(&c->mu);
      c->recv_waiters++;
      pthread_mutex_unlock(&c->mu);
    }
  }

  int ready_idx = -1;
  int elapsed = 0;
  while (ready_idx == -1) {
    for (int i = 0; i < n; i++) {
      Chan *ch = (Chan *)chs[i];
      if (!ch)
        continue; // nil channel case never fires
      if (is_send[i]) {
        // Send readiness by capacity: unbounded (cap < 0) is always
        // ready; bounded needs buffer room; rendezvous (cap 0) needs a
        // waiting receiver so the case does not "win" the scan and then
        // block inside Skink_chan_send while a later ready case starves.
        // Waiters registered by THIS select's own receive cases on the
        // same channel do not count — a select cannot rendezvous with
        // itself (the send would block forever holding the scan).
        // A closed channel still reports ready so the send panics (Go).
        int self_waiters = 0;
        for (int j = 0; j < n; j++) {
          if (!is_send[j] && chs[j] == chs[i])
            self_waiters++;
        }
        pthread_mutex_lock(&ch->mu);
        if (ch->closed || ch->capacity < 0 ||
            (ch->capacity > 0 && ch->count < ch->capacity) ||
            (ch->capacity == 0 && ch->count == 0 &&
             ch->recv_waiters > self_waiters)) {
          ready_idx = i;
        }
        pthread_mutex_unlock(&ch->mu);
        if (ready_idx == i)
          break;
      } else {
        pthread_mutex_lock(&ch->mu);
        if (ch->head != NULL || ch->closed) {
          ready_idx = i;
          pthread_mutex_unlock(&ch->mu);
          break;
        }
        pthread_mutex_unlock(&ch->mu);
      }
    }
    if (ready_idx != -1)
      break;
    // Non-blocking mode: return immediately if nothing ready
    if (timeout_ms == 0)
      break;
    // Timed mode: check if timeout has elapsed
    if (timeout_ms > 0 && elapsed >= timeout_ms * 10)
      break;
    // Sleep briefly before retrying (100 microseconds)
    SKINK_USLEEP(100);
    elapsed++;
  }

  // Unregister the receive interest registered on entry.
  for (int i = 0; i < n; i++) {
    if (!is_send[i] && chs[i]) {
      Chan *c = (Chan *)chs[i];
      pthread_mutex_lock(&c->mu);
      c->recv_waiters--;
      pthread_mutex_unlock(&c->mu);
    }
  }
  if (ready_idx == -1)
    return -1;

  Chan *ch = (Chan *)chs[ready_idx];
  if (is_send[ready_idx]) {
    Skink_chan_send(ch, vals[ready_idx]);
  } else {
    void *data = Skink_chan_recv(ch);
    if (out_val)
      *out_val = data;
  }
  return ready_idx;
}

// === Future implementation ===

typedef struct {
  void *value;
  int ready;
  int mu_ok;
  int cv_ok;
  pthread_mutex_t mu;
  pthread_cond_t cv;
} Future;

// Skink_future_dtor runs when the RC-managed future is released: destroys
// whichever synchronization primitives were successfully initialised before
// the block is freed.
static void Skink_future_dtor(void *payload) {
  Future *f = (Future *)payload;
  if (f->mu_ok)
    pthread_mutex_destroy(&f->mu);
  if (f->cv_ok)
    pthread_cond_destroy(&f->cv);
}

// Skink_future_make creates a new future for asynchronous result delivery.
// The future is RC-managed: the awaiting scope owns one reference and the
// spawned worker holds another (retained by Skink_spawn, released after
// Skink_future_set) so the handle stays valid until both sides are done.
void *Skink_future_make(void) {
  Future *f = Skink_rc_alloc_typed(sizeof(Future), (void *)Skink_future_dtor);
  if (!f)
    return NULL;
  f->mu_ok = pthread_mutex_init(&f->mu, NULL) == 0;
  f->cv_ok = f->mu_ok && pthread_cond_init(&f->cv, NULL) == 0;
  if (!f->mu_ok || !f->cv_ok) {
    // Release runs the destructor, which cleans up whichever primitive
    // initialised successfully, then frees the block.
    Skink_rc_release(f);
    return NULL;
  }
  return f;
}

// Skink_future_set stores a value in the future and wakes all waiting consumers.
void Skink_future_set(void *f_ptr, void *val) {
  Future *f = (Future *)f_ptr;
  pthread_mutex_lock(&f->mu);
  f->value = val;
  f->ready = 1;
  pthread_cond_broadcast(&f->cv);
  pthread_mutex_unlock(&f->mu);
}

// Skink_future_get blocks until the future's value is ready and returns it.
void *Skink_future_get(void *f_ptr) {
  Future *f = (Future *)f_ptr;
  pthread_mutex_lock(&f->mu);
  while (!f->ready) {
    pthread_cond_wait(&f->cv, &f->mu);
  }
  void *val = f->value;
  pthread_mutex_unlock(&f->mu);
  return val;
}

// === Spawn ===

typedef struct {
  void *(*fn)(void *);
  void *arg;
  void *future;
} SpawnArgs;

// spawn_trampoline calls the spawned function and stores its result in the associated future.
static void *spawn_trampoline(void *arg) {
  SpawnArgs *sa = (SpawnArgs *)arg;
  void *result = sa->fn(sa->arg);
  if (sa->future) {
    Skink_future_set(sa->future, result);
    // Drop the worker's reference; the awaiting scope owns the remaining one.
    Skink_rc_release(sa->future);
  }
  free(sa);
  return result;
}

// Skink_spawn launches a detached thread to execute fn(arg), storing the result in future.
void Skink_spawn(void *fn, void *arg, void *future) {
  pthread_t tid;
  SpawnArgs *sa = malloc(sizeof(SpawnArgs));
  if (!sa)
    return;
  sa->fn = (void *(*)(void *))fn;
  sa->arg = arg;
  sa->future = future;
  // The worker holds a reference on the RC-managed future for the whole run
  // so the handle outlives the awaiting scope if that scope exits early.
  if (future)
    Skink_rc_retain(future);
  if (pthread_create(&tid, NULL, spawn_trampoline, sa) != 0) {
    if (future)
      Skink_rc_release(future);
    free(sa);
    return;
  }
  pthread_detach(tid);
}

// === Web connection spawning ===
// Spawns a detached thread to handle a web connection. The handler function
// receives a server pointer and a socket file descriptor.

typedef struct {
  void (*fn)(void *, int);
  void *server;
  int fd;
} WebConnArgs;

// web_conn_trampoline dispatches a web connection to its handler function.
// The handler arrives as a Skink closure handle: an i8* block whose first
// field is the code pointer, invoked as code(handle, args...) — the
// handle is always passed as the hidden first (env) argument.
static void *web_conn_trampoline(void *arg) {
  WebConnArgs *wc = (WebConnArgs *)arg;
  void *code = *(void **)wc->fn;
  ((void (*)(void *, void *, int))code)(wc->fn, wc->server, wc->fd);
  free(wc);
  return NULL;
}

// Skink_web_spawn_conn spawns a detached thread to handle an incoming web connection.
void Skink_web_spawn_conn(void *fn, void *server, int fd) {
  WebConnArgs *wc = malloc(sizeof(WebConnArgs));
  if (!wc) {
    // Close the fd since we can't spawn a handler for it.
    SKINK_CLOSE_SOCK(fd);
    return;
  }
  wc->fn = (void (*)(void *, int))fn;
  wc->server = server;
  wc->fd = fd;
  pthread_t tid;
  if (pthread_create(&tid, NULL, web_conn_trampoline, wc) != 0) {
    SKINK_CLOSE_SOCK(fd);
    free(wc);
    return;
  }
  pthread_detach(tid);
}

// === Web TLS connection spawning ===
// Spawns a detached thread to handle a TLS web connection. The handler
// function receives a server pointer, a socket file descriptor, and a
// TLS context handle.

typedef struct {
  void (*fn)(void *, int, int64_t);
  void *server;
  int fd;
  int64_t ctx;
} WebTLSConnArgs;

// web_tls_conn_trampoline dispatches a TLS web connection to its handler
// function. The handler is a Skink closure handle — see web_conn_trampoline.
static void *web_tls_conn_trampoline(void *arg) {
  WebTLSConnArgs *wc = (WebTLSConnArgs *)arg;
  void *code = *(void **)wc->fn;
  ((void (*)(void *, void *, int, int64_t))code)(wc->fn, wc->server, wc->fd,
                                                wc->ctx);
  free(wc);
  return NULL;
}

// Skink_web_spawn_tls_conn spawns a detached thread to handle an incoming TLS web connection.
void Skink_web_spawn_tls_conn(void *fn, void *server, int fd, int64_t ctx) {
  WebTLSConnArgs *wc = malloc(sizeof(WebTLSConnArgs));
  if (!wc) {
    SKINK_CLOSE_SOCK(fd);
    return;
  }
  wc->fn = (void (*)(void *, int, int64_t))fn;
  wc->server = server;
  wc->fd = fd;
  wc->ctx = ctx;
  pthread_t tid;
  if (pthread_create(&tid, NULL, web_tls_conn_trampoline, wc) != 0) {
    SKINK_CLOSE_SOCK(fd);
    free(wc);
    return;
  }
  pthread_detach(tid);
}

// Memory helpers
void Skink_free(void *p) { free(p); }
