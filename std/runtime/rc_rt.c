// Copyright 2026 Mark Oxley. All rights reserved.
//
// Project: Skink — A self-hosted, statically-typed programming language
//          targeting LLVM IR, designed for systems programming with
//          modern ergonomics and a Go-inspired syntax.
//
// File:    std/runtime/rc_rt.c
// Purpose: C runtime support for rc rt.
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
#include <stdint.h>
#if defined(_WIN32) || defined(__MINGW32__)
typedef int64_t atomic_int_fast64_t;
#define atomic_load(p) __atomic_load_n(p, __ATOMIC_SEQ_CST)
#define atomic_store(p, v) __atomic_store_n(p, v, __ATOMIC_SEQ_CST)
#define atomic_fetch_add(p, v) __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST)
#define atomic_fetch_sub(p, v) __atomic_fetch_sub(p, v, __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_weak(p, e, d)                                  \
  __atomic_compare_exchange_n(p, e, d, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#else
#include <stdatomic.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// === Skink Automatic Reference Counting (ARC) Runtime ===
//
// Every heap-allocated object has a 24-byte prefix:
//   [0..7]  atomic refcount (int64)
//   [8..15] magic number   (i64)  — sanity check, not used for safety
//   [16..23] user data size in bytes (i64)  ← also used as slice length
//   [24..]  payload
//
// The size field is at offset 16 so that `ptr - 8` (used by slices
// to store/retrieve their length) lands on it.
//
// All public functions work with the *payload* pointer (not the raw block).
// retain/release are safe to call on non-RC pointers (string literals,
// C-library strings, null, etc.) — they simply return early.
//
// === Limitation: No Cycle Collection ===
//
// This runtime uses pure reference counting without a cycle collector.
// This means that cyclic data structures (e.g., a struct that holds a
// pointer to itself, or two structs that reference each other) will
// never be freed, even when they become unreachable.
//
// This is a known limitation. To avoid memory leaks from cyclic references:
//
// 1. Break cycles manually before dropping the last external reference.
//    For example, set pointer fields to nil before letting a struct go
//    out of scope:
//      node.next = nil
//      node.prev = nil
//
// 2. Use weak references where appropriate (not yet implemented).
//
// 3. For graph-like data structures, use an arena allocator or a
//    custom memory pool that can be freed all at once.
//
// 4. Consider using indices (int) instead of pointers to reference
//    other nodes in a graph, storing all nodes in a flat array.
//
// A future version may implement a trial-deletion cycle collector
// or a backup mark-and-sweep garbage collector to handle cyclic
// structures automatically.
//
// To determine whether a pointer is RC-managed, we maintain a global
// hash table of all RC-allocated payload pointers.  This avoids reading
// memory before arbitrary pointers (which is undefined behavior for
// non-RC pointers like malloc blocks or string literals).
//
// The hash table is protected by a pthread rwlock for thread safety:
//   - retain/count: read lock (table lookup only)
//   - alloc: write lock (table insert)
//   - release: read lock (lookup) + atomic decrement, then write lock
//     (remove) only if refcount drops to zero
//
// When the refcount drops to zero, if a destructor is registered, it is
// called on the payload pointer before the block is freed.  The destructor
// is responsible for releasing any heap-allocated fields/sub-objects.

#define RC_PREFIX_SIZE 24
#define RC_MAGIC 0xDEADBEEFCAFEBABEULL

// SkinkDtor is a function pointer type for RC object destructors.
typedef void (*SkinkDtor)(void *);

// RCHeader is the 24-byte prefix prepended to every RC-allocated block.
typedef struct {
  atomic_int_fast64_t count;
  int64_t magic;
  int64_t size;
} RCHeader;

// === RC pointer registry ===
//
// A single open-addressing hash table tracks every RC-allocated payload
// pointer.  Each entry stores the pointer (key) and an optional destructor.
// A key of NULL means the slot is empty.
//
// `is_rc_ptr` is a table lookup — no memory before the pointer is read,
// making it safe to call on any pointer (malloc, string literals, stack,
// NULL, etc.).

#define RC_TABLE_INITIAL_CAP 1024
#define RC_TABLE_MAX_LOAD 0.7

// RCEntry represents one slot in the RC pointer registry hash table.
//
// kind: 0 = RC-managed block (refcount in the block's RCHeader),
//       1 = raw tracked allocation (Skink_raw_alloc/calloc),
//       2 = tracked external resource (exit sweep),
//       3 = slice view — the key is a pointer interior to view_base's
//           payload; size carries the view's element LENGTH and refs is
//           the view's own refcount (each reference to the view value).
typedef struct {
  void *key;      // payload pointer (NULL = empty slot)
  SkinkDtor dtor; // destructor (NULL = no destructor)
  int64_t size;
  int kind;
  void *view_base;             // kind 3: owning RC payload pointer
  atomic_int_fast64_t refs;    // kind 3: view refcount
} RCEntry;

// rc_table is the global hash table tracking all RC-allocated payload pointers.
static RCEntry *rc_table = NULL;
// rc_table_cap is the current capacity of the RC pointer registry hash table.
static size_t rc_table_cap = 0;
// rc_table_count is the number of entries currently in the RC pointer registry.
static size_t rc_table_count = 0;
// rc_lock protects the RC pointer registry hash table for thread-safe access.
static pthread_rwlock_t rc_lock = PTHREAD_RWLOCK_INITIALIZER;
// rc_live_bytes tracks the total bytes currently allocated by the RC runtime.
static atomic_int_fast64_t rc_live_bytes = 0;
// rc_peak_bytes tracks the maximum bytes ever allocated by the RC runtime.
static atomic_int_fast64_t rc_peak_bytes = 0;
// rc_live_objects tracks the number of RC-managed objects currently alive.
static atomic_int_fast64_t rc_live_objects = 0;

// rc_pending tracks blocks whose registry entry has been removed but whose
// memory has not yet been freed.  free() can only run after the registry
// lock is released (destructors recursively call Skink_rc_release, and the
// rwlock is not recursive), so there is a window between registry removal
// and free() where the block is allocated but has no tracked reference.
// A heap scan that pauses a thread inside that window — e.g. LeakSanitizer's
// stop-the-world pass racing a detached worker's teardown at process exit —
// would report every in-flight free as a leak.  A node holding the raw block
// base is pushed inside the same critical section that removes the registry
// entry and popped only after free() returns, so the block is never
// simultaneously allocated and unreferenced.
typedef struct RCPending {
  void *raw;
  struct RCPending *next;
} RCPending;
static RCPending *rc_pending = NULL;

// rc_pending_push links raw onto the pending-free list.
// Must be called with the write lock held.
static void rc_pending_push(void *raw) {
  RCPending *n = (RCPending *)malloc(sizeof(RCPending));
  if (!n)
    return; // failure only narrows leak-checker accuracy; the free still proceeds
  n->raw = raw;
  n->next = rc_pending;
  rc_pending = n;
}

// rc_pending_pop unlinks the node for raw after free() has completed.
// The node list stores raw bases purely for reachability; values are never
// dereferenced, so a stale pointer to a recycled chunk is harmless.
static void rc_pending_pop(void *raw) {
  pthread_rwlock_wrlock(&rc_lock);
  RCPending **pp = &rc_pending;
  while (*pp) {
    if ((*pp)->raw == raw) {
      RCPending *dead = *pp;
      *pp = dead->next;
      free(dead);
      break;
    }
    pp = &(*pp)->next;
  }
  pthread_rwlock_unlock(&rc_lock);
}

// rc_record_alloc updates the global allocation counters when a new RC block is allocated.
static void rc_record_alloc(int64_t size) {
  int64_t live = atomic_fetch_add(&rc_live_bytes, size) + size;
  atomic_fetch_add(&rc_live_objects, 1);
  int64_t peak = atomic_load(&rc_peak_bytes);
  while (live > peak &&
         !atomic_compare_exchange_weak(&rc_peak_bytes, &peak, live)) {
  }
}

// rc_record_free updates the global allocation counters when an RC block is freed.
static void rc_record_free(int64_t size) {
  atomic_fetch_sub(&rc_live_bytes, size);
  atomic_fetch_sub(&rc_live_objects, 1);
}

// rc_table_hash mixes pointer bits for hashing.
static inline size_t rc_table_hash(void *key, size_t cap) {
  uintptr_t v = (uintptr_t)key;
  v ^= v >> 16;
  v *= 0x45d9f3bULL;
  v ^= v >> 16;
  return (size_t)(v % cap);
}

// rc_table_init initializes the table if needed.
// Must be called with the write lock held.
static void rc_table_init(void) {
  if (rc_table == NULL) {
    rc_table_cap = RC_TABLE_INITIAL_CAP;
    rc_table = (RCEntry *)calloc(rc_table_cap, sizeof(RCEntry));
    rc_table_count = 0;
  }
}

// rc_table_grow doubles the table capacity.
// Must be called with the write lock held.
static void rc_table_grow(void) {
  size_t old_cap = rc_table_cap;
  RCEntry *old = rc_table;
  rc_table_cap = old_cap * 2;
  rc_table = (RCEntry *)calloc(rc_table_cap, sizeof(RCEntry));
  rc_table_count = 0;
  for (size_t i = 0; i < old_cap; i++) {
    if (old[i].key != NULL) {
      size_t idx = rc_table_hash(old[i].key, rc_table_cap);
      while (rc_table[idx].key != NULL) {
        idx = (idx + 1) % rc_table_cap;
      }
      rc_table[idx].key = old[i].key;
      rc_table[idx].dtor = old[i].dtor;
      rc_table[idx].size = old[i].size;
      rc_table[idx].kind = old[i].kind;
      rc_table[idx].view_base = old[i].view_base;
      atomic_store(&rc_table[idx].refs, atomic_load(&old[i].refs));
      rc_table_count++;
    }
  }
  free(old);
}

// rc_table_find returns a pointer to the entry for key, or NULL if not found.
// Must be called with at least a read lock held.
static RCEntry *rc_table_find(void *key) {
  if (rc_table == NULL)
    return NULL;
  size_t idx = rc_table_hash(key, rc_table_cap);
  while (rc_table[idx].key != NULL) {
    if (rc_table[idx].key == key)
      return &rc_table[idx];
    idx = (idx + 1) % rc_table_cap;
  }
  return NULL;
}

// rc_table_insert adds or updates an entry.
// Must be called with the write lock held.
static void rc_table_insert(void *key, SkinkDtor dtor, int64_t size, int kind) {
  rc_table_init();
  if ((double)rc_table_count / (double)rc_table_cap > RC_TABLE_MAX_LOAD) {
    rc_table_grow();
  }
  size_t idx = rc_table_hash(key, rc_table_cap);
  while (rc_table[idx].key != NULL) {
    if (rc_table[idx].key == key) {
      rc_table[idx].dtor = dtor;
      rc_table[idx].size = size;
      rc_table[idx].kind = kind;
      return;
    }
    idx = (idx + 1) % rc_table_cap;
  }
  rc_table[idx].key = key;
  rc_table[idx].dtor = dtor;
  rc_table[idx].size = size;
  rc_table[idx].kind = kind;
  rc_table_count++;
}

// rc_table_remove deletes an entry.  Safe to call if key is not present.
// Must be called with the write lock held.
static void rc_table_remove(void *key) {
  if (rc_table == NULL)
    return;
  size_t idx = rc_table_hash(key, rc_table_cap);
  while (rc_table[idx].key != NULL) {
    if (rc_table[idx].key == key) {
      rc_table[idx].key = NULL;
      rc_table[idx].dtor = NULL;
      rc_table[idx].size = 0;
      rc_table[idx].kind = 0;
      rc_table_count--;
      // Re-insert subsequent entries to maintain probe chain.
      size_t next = (idx + 1) % rc_table_cap;
      while (rc_table[next].key != NULL) {
        void *rekey = rc_table[next].key;
        SkinkDtor redtor = rc_table[next].dtor;
        int64_t resize = rc_table[next].size;
        int rekind = rc_table[next].kind;
        void *rebase = rc_table[next].view_base;
        int64_t rerefs = atomic_load(&rc_table[next].refs);
        rc_table[next].key = NULL;
        rc_table[next].dtor = NULL;
        rc_table[next].size = 0;
        rc_table[next].kind = 0;
        rc_table[next].view_base = NULL;
        atomic_store(&rc_table[next].refs, 0);
        rc_table_count--;
        size_t reidx = rc_table_hash(rekey, rc_table_cap);
        while (rc_table[reidx].key != NULL) {
          reidx = (reidx + 1) % rc_table_cap;
        }
        rc_table[reidx].key = rekey;
        rc_table[reidx].dtor = redtor;
        rc_table[reidx].size = resize;
        rc_table[reidx].kind = rekind;
        rc_table[reidx].view_base = rebase;
        atomic_store(&rc_table[reidx].refs, rerefs);
        rc_table_count++;
        next = (next + 1) % rc_table_cap;
      }
      return;
    }
    idx = (idx + 1) % rc_table_cap;
  }
}

// === Deferred reclamation (retire quarantine) ===
//
// Freed blocks are not handed to free() immediately.  Each dead block is
// appended to a bounded FIFO retire queue and is only actually freed when
// the queue exceeds its entry or byte bound (oldest evicted) or at exit.
// A thread that loaded a pointer just before another thread released it —
// e.g. copy() reading a map's backing array while a worker thread resizes
// the map — then reads stale-but-valid memory instead of touching a freed
// or re-purposed chunk (a reused chunk's RC size field reads as a bogus
// element count and turns the race into a heap overflow on the memcpy).
// The bound keeps memory overhead fixed; it does NOT make unsynchronized
// access data-correct — the reader can still observe torn contents.

#define RC_RETIRE_MAX_ENTRIES 4096
#define RC_RETIRE_MAX_BYTES (64 * 1024 * 1024)

typedef struct {
  void *raw;
  int64_t bytes;
} RCRetireEntry;

static RCRetireEntry rc_retire_q[RC_RETIRE_MAX_ENTRIES];
static int rc_retire_head = 0; // next eviction index
static int rc_retire_len = 0;
static int64_t rc_retire_bytes = 0;
static pthread_mutex_t rc_retire_lock = PTHREAD_MUTEX_INITIALIZER;

// rc_retire_free_one frees the oldest queued block.  Caller holds
// rc_retire_lock.
static void rc_retire_free_one(void) {
  RCRetireEntry e = rc_retire_q[rc_retire_head];
  rc_retire_head = (rc_retire_head + 1) % RC_RETIRE_MAX_ENTRIES;
  rc_retire_len--;
  rc_retire_bytes -= e.bytes;
  free(e.raw);
}

// rc_retire queues a dead block for deferred free().  bytes is the full
// allocation size including any RC header.
static void rc_retire(void *raw, int64_t bytes) {
  if (!raw)
    return;
  pthread_mutex_lock(&rc_retire_lock);
  if (rc_retire_len == RC_RETIRE_MAX_ENTRIES)
    rc_retire_free_one();
  int idx = (rc_retire_head + rc_retire_len) % RC_RETIRE_MAX_ENTRIES;
  rc_retire_q[idx].raw = raw;
  rc_retire_q[idx].bytes = bytes;
  rc_retire_len++;
  rc_retire_bytes += bytes;
  while (rc_retire_len > 0 && rc_retire_bytes > RC_RETIRE_MAX_BYTES)
    rc_retire_free_one();
  pthread_mutex_unlock(&rc_retire_lock);
  // Note: the queue is deliberately never drained at exit.  rc_retire_q is
  // a global, so queued blocks stay reachable to LeakSanitizer and are not
  // reported — while freeing them during exit could UAF a detached worker
  // thread that is itself mid-teardown when the handlers run.
}

// === External resource tracking (kind 2) ===
//
// Registry entries with kind==2 track raw heap blocks that own external
// resources — pthread mutexes/rwlocks/conds, POSIX regex_t, sqlite3
// handles, and similar opaque objects surfaced to Skink code as int64
// "handle" fields.  Handles are plain integers, invisible to reference
// counting, and the structs holding them are routinely copied by value, so
// there is no single owner that could destroy the resource deterministically
// (a Drop method would fire on return-by-value temporaries — see
// comp_error.md #103).  Registering the block keeps it reachable to heap
// scanners — silencing false leak reports for objects that are simply alive
// until process exit — and lets the atexit sweep destroy and free them.
// Explicit free paths (Skink_mutex_free, Skink_regex_free, Skink_db_close,
// ...) must call Skink_res_untrack before freeing.

// rc_sweep_installed guards one-time registration of the exit sweep.
// Accessed under rc_lock.
static int rc_sweep_installed = 0;

// rc_sweep_resources destroys and frees every tracked-resource (kind 2)
// block still registered when the process exits.  Removing the entry before
// destroying mirrors the release path: the pending-free list keeps the raw
// block reachable across the remove-to-free window so a stop-the-world heap
// scan can never observe an allocated-but-untracked block.
static void rc_sweep_resources(void) {
  for (;;) {
    void *key = NULL;
    SkinkDtor dtor = NULL;
    pthread_rwlock_wrlock(&rc_lock);
    if (rc_table != NULL) {
      for (size_t i = 0; i < rc_table_cap; i++) {
        if (rc_table[i].key != NULL && rc_table[i].kind == 2) {
          key = rc_table[i].key;
          dtor = rc_table[i].dtor;
          rc_pending_push(key);
          rc_table_remove(key);
          break;
        }
      }
    }
    pthread_rwlock_unlock(&rc_lock);
    if (key == NULL)
      break;
    // The dtor owns teardown entirely: some C APIs (sqlite3_close_v2,
    // sqlite3_finalize) free the tracked block themselves, while others
    // (regfree, pthread_*_destroy) leave the allocation to the caller.
    if (dtor)
      dtor(key);
    rc_pending_pop(key);
  }
}

// Skink_res_track registers ptr as a tracked external resource.  dtor runs
// on the block during the exit sweep (destroy-only; the sweep frees the
// block itself).
void Skink_res_track(void *ptr, SkinkDtor dtor) {
  if (!ptr)
    return;
  pthread_rwlock_wrlock(&rc_lock);
  rc_table_insert(ptr, dtor, 0, 2);
  if (!rc_sweep_installed) {
    rc_sweep_installed = 1;
    // atexit() performs no rc_lock access; safe inside the critical section.
    atexit(rc_sweep_resources);
  }
  pthread_rwlock_unlock(&rc_lock);
}

// Skink_res_untrack removes a tracked external resource so an explicit free
// path can destroy and free it without a stale registry key outliving the
// block.
void Skink_res_untrack(void *ptr) {
  if (!ptr)
    return;
  pthread_rwlock_wrlock(&rc_lock);
  rc_table_remove(ptr);
  pthread_rwlock_unlock(&rc_lock);
}

// is_rc_ptr returns 1 if ptr is an RC-managed payload pointer, 0 otherwise.
// Safe to call on any pointer (NULL, malloc, string literals, stack, etc.)
// — no memory before the pointer is read.
static int is_rc_ptr(void *ptr) {
  if (!ptr)
    return 0;
  pthread_rwlock_rdlock(&rc_lock);
  RCEntry *entry = rc_table_find(ptr);
  int found = entry != NULL && entry->kind == 0;
  pthread_rwlock_unlock(&rc_lock);
  return found;
}

// checked_rc_allocation_size validates payload size and computes the total allocation size.
static int checked_rc_allocation_size(int64_t payload_size, size_t *total_out) {
  if (payload_size < 0 || payload_size > INT64_MAX - RC_PREFIX_SIZE)
    return 0;
  // Allocate at least one payload byte even for a zero-size request: the
  // payload pointer for an empty block would otherwise equal the chunk's
  // end address, and one-past-the-end pointers are not recognized as
  // reachability roots by heap scanners.  LeakSanitizer reported live
  // empty-slice allocations (e.g. `make([]T, 0)`) as leaks because every
  // payload pointer — in the registry and on threads' stacks — pointed at
  // the chunk boundary rather than inside it.
  int64_t alloc_bytes = payload_size == 0 ? 1 : payload_size;
  if ((uint64_t)alloc_bytes > (uint64_t)SIZE_MAX - RC_PREFIX_SIZE)
    return 0;
  *total_out = (size_t)alloc_bytes + RC_PREFIX_SIZE;
  return 1;
}

// checked_byte_count validates count and element size, computing the total byte count safely.
static int checked_byte_count(int64_t count, int64_t size, size_t *total_out) {
  if (count < 0 || size < 0)
    return 0;
  if (size != 0 && (uint64_t)count > (uint64_t)SIZE_MAX / (uint64_t)size)
    return 0;
  uint64_t total = (uint64_t)count * (uint64_t)size;
  if (total > (uint64_t)INT64_MAX)
    return 0;
  *total_out = (size_t)total;
  return 1;
}

// Skink_rc_alloc allocates a block with no destructor.
void *Skink_rc_alloc(int64_t payload_size) {
  size_t total;
  if (!checked_rc_allocation_size(payload_size, &total))
    return NULL;
  void *raw = malloc(total);
  if (!raw)
    return NULL;
  memset(raw, 0, total);
  RCHeader *h = (RCHeader *)raw;
  atomic_store(&h->count, 1);
  h->magic = RC_MAGIC;
  h->size = payload_size;
  void *payload = (char *)raw + RC_PREFIX_SIZE;
  pthread_rwlock_wrlock(&rc_lock);
  rc_table_insert(payload, NULL, payload_size, 0);
  pthread_rwlock_unlock(&rc_lock);
  rc_record_alloc(payload_size);
  return payload;
}

// Skink_rc_alloc_typed allocates a block with an associated destructor.
// The destructor is called on the payload pointer when the refcount
// drops to zero, before the block is freed.  Pass NULL for no destructor.
void *Skink_rc_alloc_typed(int64_t payload_size, void *dtor_ptr) {
  size_t total;
  if (!checked_rc_allocation_size(payload_size, &total))
    return NULL;
  void *raw = malloc(total);
  if (!raw)
    return NULL;
  memset(raw, 0, total);
  RCHeader *h = (RCHeader *)raw;
  atomic_store(&h->count, 1);
  h->magic = RC_MAGIC;
  h->size = payload_size;
  void *payload = (char *)raw + RC_PREFIX_SIZE;
  pthread_rwlock_wrlock(&rc_lock);
  rc_table_insert(payload, (SkinkDtor)dtor_ptr, payload_size, 0);
  pthread_rwlock_unlock(&rc_lock);
  rc_record_alloc(payload_size);
  return payload;
}

// rc_release_locked decrements the refcount under the write lock and prepares the block for freeing.
// Returns 0 = not releasable, 1 = still referenced, 2 = block dead (free it),
// 3 = slice-view entry dead (release *viewBaseOut after unlocking — the
// recursive Skink_rc_release must not run under rc_lock).
static int rc_release_locked(void *ptr, SkinkDtor *dtorOut,
                             RCHeader **headerOut, int64_t *sizeOut,
                             void **viewBaseOut) {
  RCEntry *e = rc_table_find(ptr);
  if (e == NULL)
    return 0;
  if (e->kind == 3) {
    // Slice view: the entry itself is the refcounted object; the backing
    // array's reference is returned for release by the caller.
    int64_t r = atomic_fetch_sub(&e->refs, 1) - 1;
    if (r > 0)
      return 1;
    *viewBaseOut = e->view_base;
    rc_table_remove(ptr);
    return 3;
  }
  if (e->kind != 0)
    return 0;
  RCHeader *h = (RCHeader *)((char *)ptr - RC_PREFIX_SIZE);
  int64_t count = atomic_load(&h->count);
  if (count <= 0)
    return 1;
  if (count > 1) {
    atomic_fetch_sub(&h->count, 1);
    return 1;
  }
  atomic_store(&h->count, 0);
  *dtorOut = e->dtor;
  *headerOut = h;
  *sizeOut = e->size;
  // Keep the raw block rooted on the pending-free list until the caller's
  // free() completes (see rc_pending above).
  rc_pending_push(h);
  rc_table_remove(ptr);
  return 2;
}

// Skink_rc_retain increments the reference count of an RC-managed pointer.
// Slice-view entries (kind 3) bump the view's own refcount — the view holds
// a single reference on its backing array for the entry's whole lifetime.
void *Skink_rc_retain(void *ptr) {
  if (!ptr)
    return ptr;
  // Retain is a read-only table lookup + atomic increment. Use a read
  // lock so concurrent retains do not serialize unnecessarily.
  pthread_rwlock_rdlock(&rc_lock);
  RCEntry *e = rc_table_find(ptr);
  if (e != NULL && e->kind == 0) {
    RCHeader *h = (RCHeader *)((char *)ptr - RC_PREFIX_SIZE);
    if (atomic_load(&h->count) > 0) {
      atomic_fetch_add(&h->count, 1);
    }
  } else if (e != NULL && e->kind == 3) {
    atomic_fetch_add(&e->refs, 1);
  }
  pthread_rwlock_unlock(&rc_lock);
  return ptr;
}

// Skink_rc_release decrements the reference count and frees the block if it reaches zero.
void Skink_rc_release(void *ptr) {
  if (!ptr)
    return;
  SkinkDtor dtor = NULL;
  RCHeader *header = NULL;
  int64_t allocSize = 0;
  void *viewBase = NULL;
  pthread_rwlock_wrlock(&rc_lock);
  int status = rc_release_locked(ptr, &dtor, &header, &allocSize, &viewBase);
  pthread_rwlock_unlock(&rc_lock);
  if (status == 2) {
    if (dtor)
      dtor(ptr);
    rc_record_free(allocSize);
    rc_retire(header, allocSize + RC_PREFIX_SIZE);
    rc_pending_pop(header);
  } else if (status == 3) {
    // Last reference to a slice view dropped: release the view's hold on
    // its backing array. Done outside the lock — release is not reentrant
    // under rc_lock.
    Skink_rc_release(viewBase);
  }
}

// Generic array element destructor helpers.
//
// Slices whose backing arrays hold heap-reachable values (strings, slices,
// structs, maps, etc.) are allocated with an array destructor.  The destructor
// receives the slice payload pointer and calls elemDtor on each element's
// address.  For pointer elements the helper releases the stored pointer; for
// struct elements the struct's destructor trampoline is invoked.

void Skink_rc_release_ptr(void *p) {
  void **pp = (void **)p;
  if (pp)
    Skink_rc_release(*pp);
}

// Skink_template_box_dtor releases the data pointer held in a template
// fat-pointer box ({i8* data, i8* methodFns...}). Boxes are allocated by
// the emitter with Skink_rc_alloc_typed so template values can escape
// their creation frame (comp_error.md #95); releasing a box releases the
// boxed object reference it holds. Method slots are code addresses, not
// RC pointers — only field 0 is released.
void Skink_template_box_dtor(void *box) {
  void **bp = (void **)box;
  if (bp)
    Skink_rc_release(*bp);
}

// Skink_array_dtor_sized iterates over array elements and calls the element destructor on each.
void Skink_array_dtor_sized(void *arr, int64_t elemSize,
                            void (*elemDtor)(void *)) {
  if (!arr || elemSize <= 0 || !elemDtor)
    return;
  RCHeader *h = (RCHeader *)((char *)arr - RC_PREFIX_SIZE);
  // For slice backing arrays, the emitter stores the element COUNT at
  // payload-8 (overwriting the header size field), not the byte size.
  // Iterate that count directly — dividing by elemSize here would run the
  // element destructor on only count/elemSize elements, leaking the rest.
  int64_t count = h->size;
  if (count <= 0)
    return;
  char *p = (char *)arr;
  for (int64_t i = 0; i < count; i++) {
    elemDtor(p);
    p += elemSize;
  }
}

// Skink_slice_ensure guarantees that a uniquely-owned RC block has at least
// needBytes of payload capacity, growing geometrically (2x) when more room
// is required. The caller MUST have verified the block's refcount is 1
// (Skink_rc_count) — the storage may be resized in place via realloc, which
// would leave other owners dangling.
//
// Returns the payload pointer, which may differ from the input if realloc
// moved the block. Returns NULL if ptr is not a tracked RC block or if the
// allocation fails.
//
// This replaces the alloc-exactly-(len+1)-and-copy pattern previously used
// by every append(), which was O(N^2) in both copies and allocator churn:
// each new block was slightly larger than the freed hole so the allocator
// could not reuse it, driving RSS to tens of GB during self-compilation
// (comp_error.md #30). With geometric growth, append is amortized O(1) and
// RSS stays bounded near 2x live data.
void *Skink_slice_ensure(void *ptr, int64_t needBytes) {
  if (!ptr || needBytes < 0)
    return NULL;
  pthread_rwlock_wrlock(&rc_lock);
  RCEntry *e = rc_table_find(ptr);
  if (e == NULL || e->kind != 0) {
    pthread_rwlock_unlock(&rc_lock);
    return NULL;
  }
  if (e->size >= needBytes) {
    pthread_rwlock_unlock(&rc_lock);
    return ptr; /* existing capacity suffices — no grow needed */
  }
  int64_t oldBytes = e->size;
  int64_t newBytes = oldBytes * 2;
  if (newBytes < needBytes)
    newBytes = needBytes;
  SkinkDtor dtor = e->dtor;
  // Remove the payload key BEFORE realloc frees the old address: the table
  // must never retain a dangling key.  The pending-free node keeps the old
  // block reachable to heap scanners until realloc() frees it.
  void *oldraw = (char *)ptr - RC_PREFIX_SIZE;
  rc_pending_push(oldraw);
  rc_table_remove(ptr);
  pthread_rwlock_unlock(&rc_lock);

  void *newraw = realloc(oldraw, RC_PREFIX_SIZE + (size_t)newBytes);
  if (!newraw) {
    // Growth failed; the old block is untouched. Re-register it so it stays
    // tracked and freeable, then report failure to the caller.
    pthread_rwlock_wrlock(&rc_lock);
    rc_table_insert(ptr, dtor, oldBytes, 0);
    pthread_rwlock_unlock(&rc_lock);
    rc_pending_pop(oldraw);
    return NULL;
  }
  void *newpayload = (char *)newraw + RC_PREFIX_SIZE;
  if (newBytes > oldBytes) {
    memset((char *)newpayload + oldBytes, 0, (size_t)(newBytes - oldBytes));
  }
  RCHeader *h = (RCHeader *)newraw;
  // The header (count, magic, and the size field which carries the slice's
  // element count) was copied by realloc — reassert magic defensively but
  // leave h->size alone; the emitter writes the new element count itself.
  h->magic = RC_MAGIC;

  pthread_rwlock_wrlock(&rc_lock);
  rc_table_insert(newpayload, dtor, newBytes, 0);
  pthread_rwlock_unlock(&rc_lock);

  // realloc() freed the old block; drop its pending node.
  rc_pending_pop(oldraw);

  // Keep the global accounting honest: only the capacity delta is new.
  rc_record_alloc(newBytes - oldBytes);
  return newpayload;
}

// Skink_mem_alloc allocates an RC-managed block of the given size.
void *Skink_mem_alloc(int64_t size) { return Skink_rc_alloc(size); }

// Skink_raw_alloc allocates a plain (non-RC) heap block tracked by the registry.
void *Skink_raw_alloc(int64_t size) {
  if (size < 0 || (uint64_t)size > (uint64_t)SIZE_MAX)
    return NULL;
  void *ptr = malloc((size_t)size);
  if (!ptr)
    return NULL;
  pthread_rwlock_wrlock(&rc_lock);
  rc_table_insert(ptr, NULL, size, 1);
  pthread_rwlock_unlock(&rc_lock);
  return ptr;
}

// Skink_raw_calloc allocates a zeroed plain (non-RC) heap block tracked by the registry.
void *Skink_raw_calloc(int64_t count, int64_t size) {
  size_t total;
  if (!checked_byte_count(count, size, &total))
    return NULL;
  void *ptr = calloc((size_t)count, (size_t)size);
  if (!ptr)
    return NULL;
  pthread_rwlock_wrlock(&rc_lock);
  rc_table_insert(ptr, NULL, (int64_t)total, 1);
  pthread_rwlock_unlock(&rc_lock);
  return ptr;
}

// Skink_mem_calloc allocates a zeroed RC-managed block for count elements of the given size.
void *Skink_mem_calloc(int64_t count, int64_t size) {
  size_t total;
  if (!checked_byte_count(count, size, &total))
    return NULL;
  return Skink_rc_alloc((int64_t)total);
}

// Skink_panic is provided by panic_rt.c.
extern void Skink_panic(char *msg);

// Skink_rc_alloc_array allocates an RC-managed array of count elements of
// the given size, panicking on negative counts, multiplication overflow,
// or OOM rather than returning NULL. Used by emitters that would
// otherwise dereference an unchecked allocation (VULN-03).
void *Skink_rc_alloc_array(int64_t count, int64_t size) {
  size_t total;
  if (!checked_byte_count(count, size, &total)) {
    Skink_panic((char *)"allocation size overflow");
    return NULL;
  }
  void *p = Skink_rc_alloc((int64_t)total);
  if (!p) {
    Skink_panic((char *)"out of memory");
  }
  return p;
}

// Skink_rc_is_registered reports whether ptr is a live RC/registry block.
// reflect_rt.c uses this to recognize heap-allocated string values under
// type erasure instead of guessing from page mappings alone (VULN-17).
int Skink_rc_is_registered(const void *ptr) {
  if (!ptr)
    return 0;
  pthread_rwlock_rdlock(&rc_lock);
  RCEntry *e = rc_table_find((void *)ptr);
  pthread_rwlock_unlock(&rc_lock);
  return e != NULL ? 1 : 0;
}

// Skink_rc_registered_size returns the tracked allocation size in bytes
// for a registered kind-0/kind-1 block, or -1 for untracked pointers.
// reflect_rt.c bounds its erased-value string scan to this size so a
// registered non-string (a struct or other composite boxed into a
// generic container) can never be scanned past its own allocation into
// unmapped memory (comp_error.md #153).
int64_t Skink_rc_registered_size(const void *ptr) {
  if (!ptr)
    return -1;
  pthread_rwlock_rdlock(&rc_lock);
  RCEntry *e = rc_table_find((void *)ptr);
  int64_t sz = (e != NULL && (e->kind == 0 || e->kind == 1)) ? e->size : -1;
  pthread_rwlock_unlock(&rc_lock);
  return sz;
}

// Skink_mem_realloc resizes a block, copying existing data and releasing the old block.
void *Skink_mem_realloc(void *ptr, int64_t size) {
  if (size < 0 || (uint64_t)size > (uint64_t)SIZE_MAX)
    return NULL;
  if (!ptr)
    return Skink_raw_alloc(size);
  pthread_rwlock_rdlock(&rc_lock);
  RCEntry *entry = rc_table_find(ptr);
  int kind = entry != NULL ? entry->kind : -1;
  int64_t oldSize = entry != NULL ? entry->size : 0;
  pthread_rwlock_unlock(&rc_lock);
  if (kind < 0)
    return NULL;
  if (kind == 2)
    // Tracked external resources are managed by their explicit free paths
    // and the exit sweep — never realloc'd as data.
    return NULL;

  if (kind == 0) {
    // Preserve the destructor on the replacement: Skink_rc_alloc alone
    // would drop it, leaking the new block's elements (comp_error.md
    // #153). The old block's destructor runs via the release below.
    SkinkDtor dtor = NULL;
    pthread_rwlock_rdlock(&rc_lock);
    RCEntry *src = rc_table_find(ptr);
    if (src != NULL)
      dtor = src->dtor;
    pthread_rwlock_unlock(&rc_lock);
    void *replacement =
        dtor ? Skink_rc_alloc_typed(size, dtor) : Skink_rc_alloc(size);
    if (!replacement)
      return NULL;
    int64_t copySize = oldSize < size ? oldSize : size;
    if (copySize > 0)
      memcpy(replacement, ptr, (size_t)copySize);
    Skink_rc_release(ptr);
    return replacement;
  }

  void *replacement = malloc((size_t)size);
  if (!replacement)
    return NULL;
  int64_t copySize = oldSize < size ? oldSize : size;
  if (copySize > 0)
    memcpy(replacement, ptr, (size_t)copySize);
  pthread_rwlock_wrlock(&rc_lock);
  rc_pending_push(ptr);
  rc_table_remove(ptr);
  rc_table_insert(replacement, NULL, size, 1);
  pthread_rwlock_unlock(&rc_lock);
  rc_retire(ptr, oldSize);
  rc_pending_pop(ptr);
  return replacement;
}

// Skink_mem_free releases or frees a block, dispatching based on whether it is RC-managed or raw.
void Skink_mem_free(void *ptr) {
  if (!ptr)
    return;
  SkinkDtor dtor = NULL;
  RCHeader *header = NULL;
  int64_t allocSize = 0;
  int raw = 0;
  int status = 0;
  void *viewBase = NULL;
  pthread_rwlock_wrlock(&rc_lock);
  RCEntry *entry = rc_table_find(ptr);
  if (entry != NULL && entry->kind == 1) {
    allocSize = entry->size;
    rc_pending_push(ptr);
    rc_table_remove(ptr);
    raw = 1;
  } else if (entry != NULL && (entry->kind == 0 || entry->kind == 3)) {
    status = rc_release_locked(ptr, &dtor, &header, &allocSize, &viewBase);
  }
  pthread_rwlock_unlock(&rc_lock);
  if (raw) {
    rc_retire(ptr, allocSize);
    rc_pending_pop(ptr);
  } else if (status == 2) {
    if (dtor)
      dtor(ptr);
    rc_record_free(allocSize);
    rc_retire(header, allocSize + RC_PREFIX_SIZE);
    rc_pending_pop(header);
  } else if (status == 3) {
    Skink_rc_release(viewBase);
  }
}

// Skink_rc_count returns the current reference count of an RC-managed pointer.
int64_t Skink_rc_count(void *ptr) {
  if (!ptr)
    return 0;
  int64_t count = 0;
  pthread_rwlock_rdlock(&rc_lock);
  RCEntry *e = rc_table_find(ptr);
  if (e != NULL && e->kind == 0) {
    RCHeader *h = (RCHeader *)((char *)ptr - RC_PREFIX_SIZE);
    count = atomic_load(&h->count);
  } else if (e != NULL && e->kind == 3) {
    count = atomic_load(&e->refs);
  }
  pthread_rwlock_unlock(&rc_lock);
  return count;
}

// === Slice views ===
//
// Views are ordinary slice headers sharing a backing allocation — see
// "Slice headers" below. Kind-3 registry entries are legacy: nothing
// creates them any more, but the release/count paths still honour them so
// stale entries remain harmless.
//
// === Slice headers (fat-slice ABI, comp_error.md #114) ===
//
// A Skink slice value is a pointer to an SkinkSliceHdr — a small RC-managed
// record carrying the element base pointer, element length, element
// capacity, and a reference to the backing allocation. Every slice value
// flows through a header, so views are independent objects: s[0:k] and two
// same-offset views s[i:j]/s[i:k] each carry their own length, and a view
// keeps the backing alive via `base` regardless of the parent's fate.
// (The thin-pointer representation could not express same-base views —
// its length slot lived at payload-8 of the backing block, shared by all
// aliases with that pointer value.)
//
// The backing allocation is an ordinary RC payload (kind 0). Its payload-8
// slot stores the element CAPACITY for destructor scans (all allocated
// slots; unwritten slots are zeroed by rc_alloc and release to no-ops).
// Backing arrays are only reachable through headers — they are never slice
// values themselves.

// === Legacy thin-pointer slice views (kind 3 registry) ===
//
// The pre-fat-slice ABI represented a slice as a bare element pointer, so a
// non-zero-offset view s[i:j] needed a registry entry to carry its own
// length (the pointer could not). Bootstrap stage-0 binaries still emit
// calls to Skink_slice_view, so it must remain for bootstrap-home/runtime
// to keep building stage-1. New-ABI code never calls it — views are
// headers via Skink_slice_sub — but the kind-3 machinery also stays live in
// retain/release/count so stage-0-emitted code keeps working.
//
// s[0:k] is the exception that motivated the ABI change (comp_error.md
// #114): a view starting at offset 0 shares the base pointer and cannot
// carry a distinct length. Only headers can express that.

// Skink_slice_view registers view_ptr as a view of src (or its ultimate
// base when src is itself a view) with the given element length, and
// returns the pointer the caller should use as the slice value. NULL means
// "fall back to a copy" — the registry can only carry one length per
// interior pointer.
void *Skink_slice_view(void *src, void *view_ptr, int64_t len) {
  if (!src || !view_ptr)
    return NULL;
  void *base = src;
  pthread_rwlock_wrlock(&rc_lock);
  RCEntry *ve = rc_table_find(view_ptr);
  if (ve != NULL) {
    if (ve->kind == 3 && ve->size == len) {
      // Same interior pointer, same length — another owner.
      atomic_fetch_add(&ve->refs, 1);
      pthread_rwlock_unlock(&rc_lock);
      return view_ptr;
    }
    pthread_rwlock_unlock(&rc_lock);
    return NULL;
  }
  RCEntry *se = rc_table_find(src);
  if (se != NULL && se->kind == 3)
    base = se->view_base;
  rc_table_insert(view_ptr, NULL, len, 3);
  RCEntry *ne = rc_table_find(view_ptr);
  if (ne != NULL) {
    ne->view_base = base;
    atomic_store(&ne->refs, 1);
  }
  pthread_rwlock_unlock(&rc_lock);
  // Retain outside the table lock: Skink_rc_retain acquires rc_lock.
  Skink_rc_retain(base);
  return view_ptr;
}

// === Fat-slice headers (comp_error.md #114) ===
//
// Under the current ABI a slice value points to an RC-managed header
// instead of the elements themselves. The header carries the slice's own
// data/len/cap plus the backing allocation it holds a reference on, so any
// number of views — including same-offset views with different lengths —
// coexist with independent lengths and shared aliasing.

typedef struct {
  void   *data;  // first visible element (== base for full slices)
  int64_t len;   // element count visible through this slice
  int64_t cap;   // elements writable from data before a grow is needed
  void   *base;  // RC payload owning the backing store; NULL = foreign data
} SkinkSliceHdr;

// Skink_slicehdr_dtor releases the backing allocation when the last header
// referencing it dies.
void Skink_slicehdr_dtor(void *p) {
  SkinkSliceHdr *h = (SkinkSliceHdr *)p;
  if (h && h->base)
    Skink_rc_release(h->base);
}

// Skink_slice_hdr allocates a slice header describing (data, len, cap)
// over a freshly-allocated backing allocation `base`. Ownership of the
// caller's reference on `base` (refcount == 1 from Skink_rc_alloc*)
// transfers to the header and is released by Skink_slicehdr_dtor when the
// header dies. Pass NULL for foreign/borrowed storage such as argv — such
// a header releases nothing when it dies.
void *Skink_slice_hdr(void *data, int64_t len, int64_t cap, void *base) {
  SkinkSliceHdr *h = (SkinkSliceHdr *)Skink_rc_alloc_typed(
      (int64_t)sizeof(SkinkSliceHdr), Skink_slicehdr_dtor);
  if (!h) {
    if (base)
      Skink_rc_release(base);
    return NULL;
  }
  h->data = data;
  h->len = len;
  h->cap = cap;
  h->base = base;
  return h;
}

// Skink_slice_sub produces a view of an existing slice: a fresh header
// sharing the source's backing store. offBytes is the byte offset of the
// view's first element from src->data; len is the view's element count.
// Views carry cap = -1 so append() always copies a view into its own
// backing store rather than overwriting elements of the parent slice.
// Retains s->base so the new view header owns its own reference to the
// shared backing allocation. NULL src yields NULL.
void *Skink_slice_sub(void *src, int64_t offBytes, int64_t len, int64_t cap) {
  (void)cap;
  if (!src)
    return NULL;
  SkinkSliceHdr *s = (SkinkSliceHdr *)src;
  if (s->base)
    Skink_rc_retain(s->base);
  return Skink_slice_hdr((char *)s->data + offBytes, len, -1, s->base);
}

// Field readers — all NULL-safe so emitted code needs no nil check.
//
// These are dual-mode during the ABI transition: a slice value is a header
// iff its registry entry is a kind-0 block whose destructor is
// Skink_slicehdr_dtor (the only allocation made with it). Everything else
// follows the legacy thin-pointer rules, so programs emitted by stage-0
// binaries — raw backing pointers and kind-3 view pointers — keep working:
// len reads payload-8 (or the view registry), data/base are the pointer
// itself (or the view's owning base), cap is unknown (0 → callers copy).
static int rc_is_slice_hdr(void *p) {
  RCEntry *e = rc_table_find(p);
  return e != NULL && e->kind == 0 && e->dtor == Skink_slicehdr_dtor;
}

int64_t Skink_slice_len(void *ptr) {
  if (!ptr)
    return 0;
  int kind = -1;
  int64_t n = -1;
  pthread_rwlock_rdlock(&rc_lock);
  RCEntry *e = rc_table_find(ptr);
  if (e != NULL) {
    kind = e->kind;
    if (kind == 3)
      n = e->size;                       // legacy view: registry carries len
    else if (kind == 0 && e->dtor == Skink_slicehdr_dtor)
      n = ((SkinkSliceHdr *)ptr)->len;   // fat-slice header
  }
  pthread_rwlock_unlock(&rc_lock);
  if (n >= 0)
    return n;
  // Legacy raw backing array: count lives at payload-8. Only reachable
  // for registered kind-0 blocks — reading payload-8 of an untracked or
  // kind-1 pointer would under-read 8 bytes before an allocation the
  // caller may not own (comp_error.md #153). Untracked pointers yield 0.
  if (kind != 0)
    return 0;
  return *((int64_t *)((char *)ptr - 8));
}

int64_t Skink_slice_cap(void *h) {
  if (!h)
    return 0;
  pthread_rwlock_rdlock(&rc_lock);
  int isHdr = rc_is_slice_hdr(h);
  pthread_rwlock_unlock(&rc_lock);
  return isHdr ? ((SkinkSliceHdr *)h)->cap : 0;
}

// Skink_slice_data unwraps a header to its element base. Legacy callers
// (raw backing or kind-3 view pointers) get their pointer back unchanged —
// under the old ABI the value WAS the data pointer — which keeps
// runtime-shared C consumers like tensor_rt.c correct under both ABIs.
void *Skink_slice_data(void *h) {
  if (!h)
    return NULL;
  pthread_rwlock_rdlock(&rc_lock);
  int isHdr = rc_is_slice_hdr(h);
  pthread_rwlock_unlock(&rc_lock);
  return isHdr ? ((SkinkSliceHdr *)h)->data : h;
}

// Skink_slice_base returns the RC payload owning the backing store: the
// header's base, a kind-3 view's owning base, or the pointer itself for a
// legacy raw array. Untracked/foreign pointers return NULL.
void *Skink_slice_base(void *h) {
  if (!h)
    return NULL;
  void *base = NULL;
  pthread_rwlock_rdlock(&rc_lock);
  RCEntry *e = rc_table_find(h);
  if (e != NULL) {
    if (e->kind == 3)
      base = e->view_base;
    else if (e->kind == 0)
      base = (e->dtor == Skink_slicehdr_dtor) ? ((SkinkSliceHdr *)h)->base : h;
  }
  pthread_rwlock_unlock(&rc_lock);
  return base;
}

// Skink_arr_count reads the element-count slot at payload-8 of a raw
// backing array — the convention used by map/set internal arrays, which
// are plain allocations without a slice header.
int64_t Skink_arr_count(void *p) {
  return p ? *((int64_t *)((char *)p - 8)) : 0;
}

// Skink_str_len returns the byte length of a NUL-terminated string.
//
// Registered RC kind-0 blocks carry their allocation size at payload-8
// (the RCHeader size field), and strings are always allocated as exactly
// len+1 bytes (Concat/Substr/Repeat/Builder.String/snprintf-probe), so
// the length is O(1) — this keeps per-character string indexing in hot
// loops (lexer, parser) linear instead of re-running strlen per access.
// Untracked pointers (string literals, C strings, raw malloc'd text)
// fall back to strlen; NULL yields 0.
//
// Used by the emitter's string index bounds checks (comp_error.md #149):
// a Skink string index that previously skipped the bounds check (reads
// past the allocation — info leak; writes — heap corruption) now checks
// against this length. Slice values never reach this function: they are
// headers and use Skink_slice_len instead.
int64_t Skink_str_len(void *p) {
  if (!p)
    return 0;
  pthread_rwlock_rdlock(&rc_lock);
  RCEntry *e = rc_table_find(p);
  int64_t size = (e != NULL && e->kind == 0) ? e->size : -1;
  pthread_rwlock_unlock(&rc_lock);
  if (size >= 0) {
    if (size > 0)
      return size - 1;
    return 0;
  }
  return (int64_t)strlen((const char *)p);
}

// Skink_mem_write_byte stores one byte at ptr+idx WITHOUT the string
// bounds check. Internal stdlib API for building byte buffers that are
// not yet NUL-terminated: the caller guarantees ptr has at least idx+1
// allocated bytes (e.g. a freshly Skink_mem_alloc'd buffer whose
// terminator byte is written at index size-1, which the semantic string
// length — size-1 — would reject; comp_error.md #149). Never use on
// untrusted/arbitrary pointers.
void Skink_mem_write_byte(void *ptr, int64_t idx, int val) {
  if (!ptr || idx < 0)
    return;
  ((char *)ptr)[idx] = (char)val;
}

// Skink_mem_byte reads one byte at ptr+idx WITHOUT the string bounds
// check — counterpart of Skink_mem_write_byte for buffers that are not
// yet NUL-terminated (a strlen-based length on an uninitialized raw
// buffer is garbage, so indexed reads on them cannot be bounds-checked
// meaningfully; comp_error.md #149). Caller guarantees idx < allocated
// size.
int Skink_mem_byte(void *ptr, int64_t idx) {
  if (!ptr || idx < 0)
    return 0;
  return (int)((unsigned char *)ptr)[idx];
}

// Skink_rc_capacity returns the byte capacity recorded in the RC table for
// a kind-0 block — e.g. the grown size after Skink_slice_ensure reallocates
// a slice backing array. Untracked/foreign pointers yield 0.
int64_t Skink_rc_capacity(void *ptr) {
  if (!ptr)
    return 0;
  pthread_rwlock_rdlock(&rc_lock);
  RCEntry *e = rc_table_find(ptr);
  int64_t sz = (e != NULL && e->kind == 0) ? e->size : 0;
  pthread_rwlock_unlock(&rc_lock);
  return sz;
}

// Skink_slice_wrap_raw wraps foreign (non-RC) storage in a slice header —
// e.g. argv. The header's base is NULL so nothing is ever freed.
void *Skink_slice_wrap_raw(void *data, int64_t len, int64_t cap) {
  return Skink_slice_hdr(data, len, cap, NULL);
}

// Skink_str_from_slice materializes a byte slice ([]uint8/[]int8) as a
// NUL-terminated RC string: alloc len+1, copy h->data, append 0. A slice
// header is not itself a string — the data it wraps is not NUL-terminated
// in general — so string(b) must copy. NULL slice yields NULL.
void *Skink_str_from_slice(void *h) {
  if (!h)
    return NULL;
  int64_t n = Skink_slice_len(h);
  char *d = (char *)Skink_slice_data(h);
  char *s = (char *)Skink_rc_alloc(n + 1);
  if (!s)
    return NULL;
  if (n > 0 && d)
    memcpy(s, d, (size_t)n);
  s[n] = '\0';
  return s;
}

// Skink_rc_live_bytes returns the total bytes currently allocated by the RC runtime.
int64_t Skink_rc_live_bytes(void) { return atomic_load(&rc_live_bytes); }

// Skink_rc_peak_bytes returns the maximum bytes ever allocated by the RC runtime.
int64_t Skink_rc_peak_bytes(void) { return atomic_load(&rc_peak_bytes); }

// Skink_rc_live_objects returns the number of RC-managed objects currently alive.
int64_t Skink_rc_live_objects(void) { return atomic_load(&rc_live_objects); }

// === Debug-Mode Reference Cycle Detection ===
//
// Because rc_table records every live kind-0 RC payload pointer along with
// its byte size and destructor, we can reconstruct the exact directed
// reference graph among live composite RC objects (those with dtor != NULL,
// i.e. heap structs, boxed variables, slice headers, typed slice backing
// arrays, template boxes, and closure environments) and run Tarjan's
// strongly-connected-components (SCC) algorithm in O(V + E) time.
//
// Any SCC with > 1 vertex, or 1 vertex with a self-edge, is a live reference
// cycle that pure reference counting will never reclaim unless broken.

static int rc_find_vertex_idx(void **keys, int *bucket_head, int *bucket_next,
                              int mask, void *ptr) {
  if (!ptr)
    return -1;
  uint64_t h = ((uint64_t)(uintptr_t)ptr >> 3) * 0x9E3779B97F4A7C15ULL;
  int b = (int)(h & (uint64_t)mask);
  for (int idx = bucket_head[b]; idx != -1; idx = bucket_next[idx]) {
    if (keys[idx] == ptr)
      return idx;
  }
  return -1;
}

static int64_t rc_scan_cycles_impl(int report) {
  pthread_rwlock_rdlock(&rc_lock);
  if (!rc_table || rc_table_cap == 0) {
    pthread_rwlock_unlock(&rc_lock);
    if (report)
      fprintf(stderr, "[skink-rc] cycle detector: 0 cyclic objects found\n");
    return 0;
  }

  int n = 0;
  for (size_t i = 0; i < rc_table_cap; i++) {
    if (rc_table[i].key != NULL && rc_table[i].kind == 0 &&
        rc_table[i].dtor != NULL) {
      n++;
    }
  }
  if (n == 0) {
    pthread_rwlock_unlock(&rc_lock);
    if (report)
      fprintf(stderr, "[skink-rc] cycle detector: 0 cyclic objects found\n");
    return 0;
  }

  int hash_cap = 16;
  while (hash_cap < n * 2)
    hash_cap <<= 1;
  int hash_mask = hash_cap - 1;

  void **keys = (void **)calloc((size_t)n, sizeof(void *));
  int64_t *sizes = (int64_t *)calloc((size_t)n, sizeof(int64_t));
  SkinkDtor *dtors = (SkinkDtor *)calloc((size_t)n, sizeof(SkinkDtor));
  int64_t *rcs = (int64_t *)calloc((size_t)n, sizeof(int64_t));
  int *bucket_head = (int *)malloc((size_t)hash_cap * sizeof(int));
  int *bucket_next = (int *)malloc((size_t)n * sizeof(int));
  if (!keys || !sizes || !dtors || !rcs || !bucket_head || !bucket_next) {
    pthread_rwlock_unlock(&rc_lock);
    free(keys);
    free(sizes);
    free(dtors);
    free(rcs);
    free(bucket_head);
    free(bucket_next);
    return 0;
  }
  memset(bucket_head, -1, (size_t)hash_cap * sizeof(int));

  int idx = 0;
  for (size_t i = 0; i < rc_table_cap; i++) {
    if (rc_table[i].key != NULL && rc_table[i].kind == 0 &&
        rc_table[i].dtor != NULL) {
      void *k = rc_table[i].key;
      RCHeader *hdr = (RCHeader *)((char *)k - RC_PREFIX_SIZE);
      keys[idx] = k;
      sizes[idx] = rc_table[i].size;
      dtors[idx] = rc_table[i].dtor;
      rcs[idx] = atomic_load(&hdr->count);
      uint64_t h = ((uint64_t)(uintptr_t)k >> 3) * 0x9E3779B97F4A7C15ULL;
      int b = (int)(h & (uint64_t)hash_mask);
      bucket_next[idx] = bucket_head[b];
      bucket_head[b] = idx;
      idx++;
    }
  }

  int edge_cap = n * 4 + 16;
  int edge_cnt = 0;
  int *edge_head = (int *)malloc((size_t)n * sizeof(int));
  int *edge_to = (int *)malloc((size_t)edge_cap * sizeof(int));
  int *edge_next = (int *)malloc((size_t)edge_cap * sizeof(int));
  int64_t *in_deg = (int64_t *)calloc((size_t)n, sizeof(int64_t));
  int *self_loop = (int *)calloc((size_t)n, sizeof(int));
  memset(edge_head, -1, (size_t)n * sizeof(int));

  for (int u = 0; u < n; u++) {
    void *k = keys[u];
    int64_t sz = sizes[u];
    SkinkDtor dtor = dtors[u];
    if (dtor == Skink_slicehdr_dtor) {
      // Slice header: only `base` owns an RC reference (`data` is an alias).
      SkinkSliceHdr *sh = (SkinkSliceHdr *)k;
      int v = rc_find_vertex_idx(keys, bucket_head, bucket_next, hash_mask,
                                 sh->base);
      if (v >= 0) {
        if (edge_cnt >= edge_cap) {
          edge_cap *= 2;
          edge_to = (int *)realloc(edge_to, (size_t)edge_cap * sizeof(int));
          edge_next = (int *)realloc(edge_next, (size_t)edge_cap * sizeof(int));
        }
        edge_to[edge_cnt] = v;
        edge_next[edge_cnt] = edge_head[u];
        edge_head[u] = edge_cnt++;
        in_deg[v]++;
        if (u == v)
          self_loop[u] = 1;
      }
      continue;
    }
    if (dtor == Skink_template_box_dtor) {
      // Template box: only slot 0 is the boxed data pointer; remaining slots
      // are function pointers.
      sz = (int64_t)sizeof(void *);
    }
    int64_t words = sz / (int64_t)sizeof(void *);
    void **slots = (void **)k;
    for (int64_t w = 0; w < words; w++) {
      int v = rc_find_vertex_idx(keys, bucket_head, bucket_next, hash_mask,
                                 slots[w]);
      if (v >= 0) {
        if (edge_cnt >= edge_cap) {
          edge_cap *= 2;
          edge_to = (int *)realloc(edge_to, (size_t)edge_cap * sizeof(int));
          edge_next = (int *)realloc(edge_next, (size_t)edge_cap * sizeof(int));
        }
        edge_to[edge_cnt] = v;
        edge_next[edge_cnt] = edge_head[u];
        edge_head[u] = edge_cnt++;
        in_deg[v]++;
        if (u == v)
          self_loop[u] = 1;
      }
    }
  }
  pthread_rwlock_unlock(&rc_lock);

  // Iterative Tarjan SCC over (n, edge_head, edge_to, edge_next).
  int *disc = (int *)malloc((size_t)n * sizeof(int));
  int *low = (int *)malloc((size_t)n * sizeof(int));
  int *on_stack = (int *)calloc((size_t)n, sizeof(int));
  int *scc_stack = (int *)malloc((size_t)n * sizeof(int));
  int *dfs_node = (int *)malloc((size_t)n * sizeof(int));
  int *dfs_edge = (int *)malloc((size_t)n * sizeof(int));
  int *dfs_parent = (int *)malloc((size_t)n * sizeof(int));
  memset(disc, -1, (size_t)n * sizeof(int));

  int timer = 0;
  int scc_top = 0;
  int64_t cyclic_groups = 0;
  int64_t cyclic_objects = 0;
  int64_t cyclic_bytes = 0;

  for (int root = 0; root < n; root++) {
    if (disc[root] != -1)
      continue;
    int dtop = 0;
    disc[root] = low[root] = timer++;
    scc_stack[scc_top++] = root;
    on_stack[root] = 1;
    dfs_node[dtop] = root;
    dfs_edge[dtop] = edge_head[root];
    dfs_parent[dtop] = -1;
    dtop++;

    while (dtop > 0) {
      int u = dfs_node[dtop - 1];
      int e = dfs_edge[dtop - 1];
      if (e != -1) {
        dfs_edge[dtop - 1] = edge_next[e];
        int v = edge_to[e];
        if (disc[v] == -1) {
          disc[v] = low[v] = timer++;
          scc_stack[scc_top++] = v;
          on_stack[v] = 1;
          dfs_node[dtop] = v;
          dfs_edge[dtop] = edge_head[v];
          dfs_parent[dtop] = u;
          dtop++;
        } else if (on_stack[v]) {
          if (disc[v] < low[u])
            low[u] = disc[v];
        }
      } else {
        int p = dfs_parent[dtop - 1];
        dtop--;
        if (p != -1 && low[u] < low[p])
          low[p] = low[u];
        if (low[u] == disc[u]) {
          // Pop SCC rooted at u.
          int scc_start = scc_top;
          while (scc_top > 0) {
            int w = scc_stack[--scc_top];
            on_stack[w] = 0;
            if (w == u)
              break;
          }
          int scc_len = scc_start - scc_top;
          int is_cycle = (scc_len > 1) || (scc_len == 1 && self_loop[u]);
          if (is_cycle) {
            cyclic_groups++;
            cyclic_objects += scc_len;
            int64_t grp_bytes = 0;
            int all_internal = 1;
            for (int k = scc_top; k < scc_start; k++) {
              int m = scc_stack[k];
              grp_bytes += sizes[m];
              if (rcs[m] > in_deg[m])
                all_internal = 0;
            }
            cyclic_bytes += grp_bytes;
            if (report) {
              fprintf(stderr,
                      "  cycle #%lld: %d object(s), %lld bytes (%s)\n",
                      (long long)cyclic_groups, scc_len, (long long)grp_bytes,
                      all_internal ? "unreachable/leaked" : "externally reachable");
              for (int k = scc_top; k < scc_start; k++) {
                int m = scc_stack[k];
                fprintf(stderr,
                        "    obj %p (size=%lldB, rc=%lld, heap_in_refs=%lld)\n",
                        keys[m], (long long)sizes[m], (long long)rcs[m],
                        (long long)in_deg[m]);
              }
            }
          }
        }
      }
    }
  }

  if (report) {
    fprintf(stderr,
            "[skink-rc] cycle detector: %lld cyclic object(s) in %lld cycle group(s) (%lld bytes)\n",
            (long long)cyclic_objects, (long long)cyclic_groups,
            (long long)cyclic_bytes);
  }

  free(keys);
  free(sizes);
  free(dtors);
  free(rcs);
  free(bucket_head);
  free(bucket_next);
  free(edge_head);
  free(edge_to);
  free(edge_next);
  free(in_deg);
  free(self_loop);
  free(disc);
  free(low);
  free(on_stack);
  free(scc_stack);
  free(dfs_node);
  free(dfs_edge);
  free(dfs_parent);
  return cyclic_objects;
}

// Skink_rc_detect_cycles scans the live RC object graph and returns the
// number of objects currently participating in reference cycles.
int64_t Skink_rc_detect_cycles(void) { return rc_scan_cycles_impl(0); }

// Skink_rc_report_cycles scans the live RC object graph, prints a human-
// readable report of any reference cycles to stderr, and returns the number
// of cyclic objects found.
int64_t Skink_rc_report_cycles(void) { return rc_scan_cycles_impl(1); }

// Automatic exit-time cycle check when SKINK_RC_DETECT_CYCLES=1 is set.
// Setting SKINK_RC_STRICT_CYCLES=1 additionally exits with status 2 when
// cycles are detected at process exit.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((destructor)) static void rc_exit_cycle_check(void) {
  const char *env = getenv("SKINK_RC_DETECT_CYCLES");
  if (env && env[0] != '\0' && strcmp(env, "0") != 0) {
    int64_t cyc = Skink_rc_report_cycles();
    const char *strict = getenv("SKINK_RC_STRICT_CYCLES");
    if (cyc > 0 && strict && strict[0] != '\0' && strcmp(strict, "0") != 0) {
      _Exit(2);
    }
  }
}
#endif
