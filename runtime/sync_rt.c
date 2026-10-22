// Copyright 2026 Mark Oxley. All rights reserved.
//
// Project: Skink — A self-hosted, statically-typed programming language
//          targeting LLVM IR, designed for systems programming with
//          modern ergonomics and a Go-inspired syntax.
//
// File:    std/runtime/sync_rt.c
// Purpose: C runtime support for sync rt.
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

/*
 * Copyright 2026 Mark Oxley. All rights reserved.
 *
 * Project: Skink — A self-hosted, statically-typed programming language
 *          targeting LLVM IR, designed for systems programming with
 *          modern ergonomics and a Go-inspired syntax.
 *
 * File:    std/runtime/sync_rt.c
 * Purpose: C wrappers around POSIX pthread synchronization primitives
 *          (mutex, rwlock, condition variable) and GCC atomic built-ins,
 *          exposing real linker symbols that Skink code can call via
 *          extern fn declarations.
 * Author:  Mark Oxley
 * Created: 2026
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "skink_compat.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Mutex
 * ---------------------------------------------------------------------------
 */

/*
 * Guards lazy initialisation of zero-valued synchronisation handles so
 * that concurrent first use of the same handle allocates exactly one
 * underlying primitive.
 */
static pthread_mutex_t skink_lazy_init_mu = PTHREAD_MUTEX_INITIALIZER;

/*
 * Skink_res_track / Skink_res_untrack (rc_rt.c) register native-resource
 * blocks in the RC registry so heap scanners see them as reachable and the
 * process-exit sweep destroys them.  Sync primitives are surfaced to Skink
 * as int64 handles inside by-value structs, so no single owner exists to
 * free them deterministically (comp_error.md #103).
 */
extern void Skink_res_track(void *ptr, void (*dtor)(void *));
extern void Skink_res_untrack(void *ptr);

/*
 * skink_mutex_dtor / skink_rwlock_dtor / skink_cond_dtor destroy the native
 * primitive when the exit sweep frees a still-tracked block.
 */
static void skink_mutex_dtor(void *p) { pthread_mutex_destroy((pthread_mutex_t *)p); free(p); }
static void skink_rwlock_dtor(void *p) { pthread_rwlock_destroy((pthread_rwlock_t *)p); free(p); }
static void skink_cond_dtor(void *p) { pthread_cond_destroy((pthread_cond_t *)p); free(p); }

/*
 * Skink_mutex_alloc allocates and initialises a pthread_mutex_t on the heap.
 * Returns an opaque pointer that should be cast to int inside Skink code,
 * or NULL on allocation or initialisation failure.
 */
void *Skink_mutex_alloc(void) {
  pthread_mutex_t *mu = (pthread_mutex_t *)calloc(1, sizeof(pthread_mutex_t));
  if (!mu)
    return NULL;
  if (pthread_mutex_init(mu, NULL) != 0) {
    free(mu);
    return NULL;
  }
  Skink_res_track(mu, skink_mutex_dtor);
  return mu;
}

/*
 * Skink_mutex_ensure returns the mutex handle stored at *handle_ptr,
 * lazily allocating and storing one when the handle is still zero. This
 * makes a zero-valued Skink Mutex usable, matching its documented "the
 * zero value is unlocked" semantics. Allocation is serialised through a
 * process-wide guard so racing first uses cannot overwrite each other;
 * the loser of the race simply sees the winner's handle.
 */
void *Skink_mutex_ensure(int64_t *handle_ptr) {
  if (handle_ptr == NULL)
    return NULL;
  if (__atomic_load_n(handle_ptr, __ATOMIC_ACQUIRE) != 0)
    return (void *)(intptr_t)*handle_ptr;
  pthread_mutex_lock(&skink_lazy_init_mu);
  if (*handle_ptr == 0) {
    void *mu = Skink_mutex_alloc();
    if (mu != NULL)
      __atomic_store_n(handle_ptr, (int64_t)(intptr_t)mu, __ATOMIC_RELEASE);
  }
  pthread_mutex_unlock(&skink_lazy_init_mu);
  return (void *)(intptr_t)*handle_ptr;
}

/* Forward declarations — allocators are defined further down the file. */
void *Skink_rwlock_alloc(void);
void *Skink_cond_alloc(void);

/*
 * Skink_rwlock_ensure / Skink_cond_ensure are the rwlock and condition
 * variable counterparts of Skink_mutex_ensure: they lazily allocate the
 * underlying pthread primitive when the handle slot is still zero so a
 * zero-valued RWMutex/Cond (and structs embedding them, e.g.
 * waitgroup.WaitGroup) is usable without a constructor call
 * (comp_error.md #82).
 */
void *Skink_rwlock_ensure(int64_t *handle_ptr) {
  if (handle_ptr == NULL)
    return NULL;
  if (__atomic_load_n(handle_ptr, __ATOMIC_ACQUIRE) != 0)
    return (void *)(intptr_t)*handle_ptr;
  pthread_mutex_lock(&skink_lazy_init_mu);
  if (*handle_ptr == 0) {
    void *rw = Skink_rwlock_alloc();
    if (rw != NULL)
      __atomic_store_n(handle_ptr, (int64_t)(intptr_t)rw, __ATOMIC_RELEASE);
  }
  pthread_mutex_unlock(&skink_lazy_init_mu);
  return (void *)(intptr_t)*handle_ptr;
}

void *Skink_cond_ensure(int64_t *handle_ptr) {
  if (handle_ptr == NULL)
    return NULL;
  if (__atomic_load_n(handle_ptr, __ATOMIC_ACQUIRE) != 0)
    return (void *)(intptr_t)*handle_ptr;
  pthread_mutex_lock(&skink_lazy_init_mu);
  if (*handle_ptr == 0) {
    void *cv = Skink_cond_alloc();
    if (cv != NULL)
      __atomic_store_n(handle_ptr, (int64_t)(intptr_t)cv, __ATOMIC_RELEASE);
  }
  pthread_mutex_unlock(&skink_lazy_init_mu);
  return (void *)(intptr_t)*handle_ptr;
}

/* Skink_mutex_lock acquires the mutex, blocking until it is available. */
void Skink_mutex_lock(void *mu) { pthread_mutex_lock((pthread_mutex_t *)mu); }

/*
 * Skink_mutex_trylock attempts to acquire the mutex without blocking.
 * Returns 0 on success, non-zero if the mutex is already held.
 */
int Skink_mutex_trylock(void *mu) {
  return pthread_mutex_trylock((pthread_mutex_t *)mu);
}

/* Skink_mutex_unlock releases the mutex. */
void Skink_mutex_unlock(void *mu) {
  pthread_mutex_unlock((pthread_mutex_t *)mu);
}

/* Skink_mutex_free destroys and frees a mutex allocated by Skink_mutex_alloc.
 * Untracks the block first so the exit sweep cannot double-destroy it. */
void Skink_mutex_free(void *mu) {
  if (!mu)
    return;
  Skink_res_untrack(mu);
  pthread_mutex_destroy((pthread_mutex_t *)mu);
  free(mu);
}

/* ---------------------------------------------------------------------------
 * Read-Write Lock (RWMutex)
 * ---------------------------------------------------------------------------
 */

/*
 * Skink_rwlock_alloc allocates and initialises a pthread_rwlock_t on the
 * heap.  Returns an opaque pointer or NULL on failure.
 */
void *Skink_rwlock_alloc(void) {
  pthread_rwlock_t *rw =
      (pthread_rwlock_t *)calloc(1, sizeof(pthread_rwlock_t));
  if (!rw)
    return NULL;
  if (pthread_rwlock_init(rw, NULL) != 0) {
    free(rw);
    return NULL;
  }
  Skink_res_track(rw, skink_rwlock_dtor);
  return rw;
}

/* Skink_rwlock_rdlock acquires a shared (read) lock, blocking if necessary. */
void Skink_rwlock_rdlock(void *rw) {
  pthread_rwlock_rdlock((pthread_rwlock_t *)rw);
}

/* Skink_rwlock_wrlock acquires an exclusive (write) lock, blocking if
 * necessary. */
void Skink_rwlock_wrlock(void *rw) {
  pthread_rwlock_wrlock((pthread_rwlock_t *)rw);
}

/* Skink_rwlock_unlock releases the held read or write lock. */
void Skink_rwlock_unlock(void *rw) {
  pthread_rwlock_unlock((pthread_rwlock_t *)rw);
}

/*
 * Skink_rwlock_tryrdlock attempts to acquire a shared (read) lock without
 * blocking.  Returns 0 on success, non-zero if the lock could not be acquired.
 */
int Skink_rwlock_tryrdlock(void *rw) {
  return pthread_rwlock_tryrdlock((pthread_rwlock_t *)rw);
}

/*
 * Skink_rwlock_trywrlock attempts to acquire an exclusive (write) lock
 * without blocking.  Returns 0 on success, non-zero if the lock could
 * not be acquired.
 */
int Skink_rwlock_trywrlock(void *rw) {
  return pthread_rwlock_trywrlock((pthread_rwlock_t *)rw);
}

/* Skink_rwlock_free destroys and frees a rwlock allocated by
 * Skink_rwlock_alloc. */
void Skink_rwlock_free(void *rw) {
  if (!rw)
    return;
  Skink_res_untrack(rw);
  pthread_rwlock_destroy((pthread_rwlock_t *)rw);
  free(rw);
}

/* ---------------------------------------------------------------------------
 * Condition Variable
 * ---------------------------------------------------------------------------
 */

/*
 * Skink_cond_alloc allocates and initialises a pthread_cond_t on the heap.
 * Returns an opaque pointer or NULL on failure.
 */
void *Skink_cond_alloc(void) {
  pthread_cond_t *cv = (pthread_cond_t *)calloc(1, sizeof(pthread_cond_t));
  if (!cv)
    return NULL;
  if (pthread_cond_init(cv, NULL) != 0) {
    free(cv);
    return NULL;
  }
  Skink_res_track(cv, skink_cond_dtor);
  return cv;
}

/*
 * Skink_cond_wait atomically releases the mutex and blocks on the condition
 * variable.  Upon waking, the mutex is re-acquired before returning.
 */
void Skink_cond_wait(void *cv, void *mu) {
  pthread_cond_wait((pthread_cond_t *)cv, (pthread_mutex_t *)mu);
}

/* Skink_cond_signal wakes at least one thread waiting on the condition
 * variable. */
void Skink_cond_signal(void *cv) { pthread_cond_signal((pthread_cond_t *)cv); }

/* Skink_cond_broadcast wakes all threads currently waiting on the condition
 * variable. */
void Skink_cond_broadcast(void *cv) {
  pthread_cond_broadcast((pthread_cond_t *)cv);
}

/* Skink_cond_free destroys and frees a condition variable allocated by
 * Skink_cond_alloc. */
void Skink_cond_free(void *cv) {
  if (!cv)
    return;
  Skink_res_untrack(cv);
  pthread_cond_destroy((pthread_cond_t *)cv);
  free(cv);
}

/* ---------------------------------------------------------------------------
 * Atomic operations (GCC built-in wrappers)
 *
 * __atomic_* are compiler intrinsics; they do not exist as library symbols.
 * By wrapping them in small C functions we expose real linker symbols that
 * Skink can call via extern fn declarations.
 * ---------------------------------------------------------------------------
 */

/* Skink_atomic_load performs a sequentially-consistent load of *ptr. */
int Skink_atomic_load(int *ptr) {
  return __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
}

/* Skink_atomic_store performs a sequentially-consistent store of val into *ptr.
 */
void Skink_atomic_store(int *ptr, int val) {
  __atomic_store_n(ptr, val, __ATOMIC_SEQ_CST);
}

/*
 * Skink_atomic_add atomically adds delta to *ptr and returns the new value
 * using sequentially-consistent ordering.
 */
int Skink_atomic_add(int *ptr, int delta) {
  return __atomic_add_fetch(ptr, delta, __ATOMIC_SEQ_CST);
}

/*
 * Skink_atomic_sub atomically subtracts delta from *ptr and returns the new
 * value using sequentially-consistent ordering.
 */
int Skink_atomic_sub(int *ptr, int delta) {
  return __atomic_sub_fetch(ptr, delta, __ATOMIC_SEQ_CST);
}

/*
 * Skink_atomic_exchange atomically stores val into *ptr and returns the
 * previous value using sequentially-consistent ordering.
 */
int Skink_atomic_exchange(int *ptr, int val) {
  return __atomic_exchange_n(ptr, val, __ATOMIC_SEQ_CST);
}

/*
 * Skink_atomic_cas performs a compare-and-swap.  If *ptr equals *expected,
 * it is replaced with desired and *expected is left unchanged; otherwise
 * *expected is updated with the current value of *ptr.  Returns 1 (true)
 * if the swap succeeded, 0 (false) otherwise.
 */
int Skink_atomic_cas(int *ptr, int *expected, int desired) {
  return __atomic_compare_exchange_n(ptr, expected, desired, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* ---------------------------------------------------------------------------
 * PRNG seeding (VULN-27)
 * ---------------------------------------------------------------------------
 */

#ifndef _WIN32
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#endif

/* skink_do_seed performs the actual srand(): prefer OS entropy so two
 * processes started in the same second get different sequences; fall
 * back to time^pid^address entropy. libc rand() is process-global and
 * not cryptographically secure regardless — see std/rand docs.
 */
static void skink_do_seed(void) {
  unsigned int seed = 0;
#ifdef _WIN32
  /* rand_s is the MSVCRT cryptographic rand; _CRT_RAND_S must be defined
   * before <stdlib.h> — it may not be here, so declare it directly. */
  extern int rand_s(unsigned int *);
  if (rand_s(&seed) != 0)
    seed = 0;
#else
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    ssize_t n = read(fd, &seed, sizeof(seed));
    close(fd);
    if (n != (ssize_t)sizeof(seed))
      seed = 0;
  }
#endif
  if (seed == 0) {
    seed = (unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)&seed;
#ifndef _WIN32
    seed ^= (unsigned int)getpid() * 2654435761u;
#endif
  }
  srand(seed);
}

/*
 * Skink_rand_seed_once seeds the process-global libc rand() exactly
 * once, guarded by pthread_once so racing callers cannot reseed
 * mid-sequence (VULN-27). An explicit rand.Seed(s) call afterwards
 * still overrides for reproducible tests.
 */
void Skink_rand_seed_once(void) {
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  pthread_once(&once, skink_do_seed);
}
