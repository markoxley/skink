// Copyright 2026 Mark Oxley. All rights reserved.
//
// Project: Skink — A self-hosted, statically-typed programming language
//          targeting LLVM IR, designed for systems programming with
//          modern ergonomics and a Go-inspired syntax.
//
// File:    std/runtime/rules_rt.c
// Purpose: C runtime support for rules rt.
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
 * File:    std/runtime/rules_rt.c
 * Purpose: C runtime for the Skink ruleset engine, providing a background
 *          evaluation loop, thread-safe rule registration, and lifecycle
 *          management (start/stop/reset) using POSIX threads.
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
#include <string.h>
#ifdef _WIN32
// usleep replaced by SKINK_USLEEP via skink_compat.h
#else
#include <unistd.h>
#endif

/* Function pointer type for a rule callback that receives a self pointer. */
typedef void (*SkinkRuleFn)(void *);

/* A single rule entry stored inside a ruleset. */
typedef struct SkinkRuleEntry {
  void *self;        /* Opaque pointer to the rule's owning object */
  SkinkRuleFn eval;  /* Called on each evaluation tick */
  SkinkRuleFn start; /* Called once when the ruleset starts */
  SkinkRuleFn stop;  /* Called once when the ruleset stops */
  int priority;      /* Rule priority (higher = evaluated first, if sorted) */
} SkinkRuleEntry;

// SkinkRuleset is an opaque forward declaration of the ruleset engine struct.
typedef struct SkinkRuleset SkinkRuleset;

/* Forward declaration of the background evaluation loop entry point. */
void *Skink_ruleset_loop(void *arg);

// SkinkRuleset holds the rule array, thread state, and synchronization primitives for the ruleset engine.
struct SkinkRuleset {
  int running;               /* Non-zero when the ruleset is active */
  SkinkRuleEntry *rules;     /* Dynamically-sized array of rule entries */
  int count;                 /* Number of rules currently registered */
  int capacity;              /* Allocated capacity of the rules array */
  pthread_mutex_t mu;        /* Protects all mutable fields */
  pthread_cond_t stopped_cv; /* Signalled when the eval thread exits */
  int thread_active; /* Non-zero while the background thread is running */
};

/*
 * Skink_ruleset_create allocates and initialises a new ruleset with an
 * initial capacity of 8 rule slots.  Returns NULL on allocation or
 * synchronisation initialisation failure.
 */
SkinkRuleset *Skink_ruleset_create() {
  SkinkRuleset *rs = calloc(1, sizeof(SkinkRuleset));
  if (!rs)
    return NULL;
  rs->capacity = 8;
  rs->rules = malloc(sizeof(SkinkRuleEntry) * rs->capacity);
  if (!rs->rules) {
    free(rs);
    return NULL;
  }
  if (pthread_mutex_init(&rs->mu, NULL) != 0) {
    free(rs->rules);
    free(rs);
    return NULL;
  }
  if (pthread_cond_init(&rs->stopped_cv, NULL) != 0) {
    pthread_mutex_destroy(&rs->mu);
    free(rs->rules);
    free(rs);
    return NULL;
  }
  rs->thread_active = 0;
  return rs;
}

/*
 * Skink_ruleset_add_rule registers a new rule with the given callbacks and
 * priority.  The rules array is grown geometrically when full.  If the
 * ruleset is already running, the rule's start callback is invoked
 * immediately so the new rule is brought online.
 */
void Skink_ruleset_add_rule(SkinkRuleset *rs, void *self, SkinkRuleFn eval,
                            SkinkRuleFn start, SkinkRuleFn stop, int priority) {
  pthread_mutex_lock(&rs->mu);
  /* Grow the rules array if we have run out of capacity. */
  if (rs->count >= rs->capacity) {
    int new_cap = rs->capacity * 2;
    SkinkRuleEntry *new_rules =
        realloc(rs->rules, sizeof(SkinkRuleEntry) * new_cap);
    if (!new_rules) {
      pthread_mutex_unlock(&rs->mu);
      return;
    }
    rs->rules = new_rules;
    rs->capacity = new_cap;
  }
  SkinkRuleEntry *r = &rs->rules[rs->count];
  r->self = self;
  r->eval = eval;
  r->start = start;
  r->stop = stop;
  r->priority = priority;
  rs->count++;
  int was_running = rs->running;
  pthread_mutex_unlock(&rs->mu);
  /* If the ruleset is already running, start the newly added rule now.
   * The callback runs WITHOUT the lock held so a rule may safely call
   * back into the ruleset API without deadlocking. */
  if (was_running && start) {
    start(self);
  }
}

/*
 * Skink_ruleset_start activates the ruleset by invoking each rule's start
 * callback, then spawning a detached background thread that repeatedly
 * evaluates all rules.  If thread creation fails the running flag is
 * cleared so callers can detect the failure.
 */
void Skink_ruleset_start(SkinkRuleset *rs) {
  /* Copy the start callbacks under the lock, then invoke them after
   * releasing it so callbacks may safely call back into the API. */
  pthread_mutex_lock(&rs->mu);
  if (rs->running) {
    pthread_mutex_unlock(&rs->mu);
    return;
  }
  rs->running = 1;
  int n = rs->count;
  SkinkRuleEntry *snapshot = NULL;
  if (n > 0) {
    snapshot = malloc(sizeof(SkinkRuleEntry) * n);
    if (snapshot) {
      memcpy(snapshot, rs->rules, sizeof(SkinkRuleEntry) * n);
    } else {
      n = 0; /* proceed without invoking start callbacks */
    }
  }
  /* Mark thread_active BEFORE creating the thread: if the loop exits
   * quickly, the flag is already set so the exit path can clear it and
   * a concurrent stop() will not wait on a stale flag. */
  rs->thread_active = 1;
  pthread_mutex_unlock(&rs->mu);

  for (int i = 0; i < n; i++) {
    if (snapshot[i].start) {
      snapshot[i].start(snapshot[i].self);
    }
  }
  free(snapshot);

  /* Start evaluation loop in a background thread. */
  pthread_t tid;
  if (pthread_create(&tid, NULL, Skink_ruleset_loop, rs) != 0) {
    /* Failed to start thread — mark as not running and inactive. */
    pthread_mutex_lock(&rs->mu);
    rs->running = 0;
    rs->thread_active = 0;
    pthread_cond_signal(&rs->stopped_cv);
    pthread_mutex_unlock(&rs->mu);
    return;
  }
  pthread_detach(tid);
}

/*
 * Skink_ruleset_stop deactivates the ruleset by clearing the running flag,
 * invoking each rule's stop callback, and blocking until the background
 * evaluation thread has exited.
 */
void Skink_ruleset_stop(SkinkRuleset *rs) {
  /* Copy the stop callbacks under the lock, wait for the eval thread to
   * exit, then invoke callbacks lock-free so they cannot deadlock on the
   * ruleset API. */
  pthread_mutex_lock(&rs->mu);
  rs->running = 0;
  int n = rs->count;
  SkinkRuleEntry *snapshot = NULL;
  if (n > 0) {
    snapshot = malloc(sizeof(SkinkRuleEntry) * n);
    if (snapshot) {
      memcpy(snapshot, rs->rules, sizeof(SkinkRuleEntry) * n);
    } else {
      n = 0;
    }
  }
  /* Wait for the evaluation thread to exit before returning. */
  while (rs->thread_active) {
    pthread_cond_wait(&rs->stopped_cv, &rs->mu);
  }
  pthread_mutex_unlock(&rs->mu);

  for (int i = 0; i < n; i++) {
    if (snapshot[i].stop) {
      snapshot[i].stop(snapshot[i].self);
    }
  }
  free(snapshot);
}

/*
 * Skink_ruleset_reset stops the ruleset and clears all registered rules,
 * returning it to an empty state.  The ruleset can be repopulated and
 * restarted afterwards.
 */
void Skink_ruleset_reset(SkinkRuleset *rs) {
  /* Stop first: this clears running and joins the eval thread so no
   * callback can still be in flight when the rules are cleared. */
  Skink_ruleset_stop(rs);
  pthread_mutex_lock(&rs->mu);
  rs->count = 0;
  pthread_mutex_unlock(&rs->mu);
}

/*
 * Skink_ruleset_is_running returns 1 if the ruleset is currently active
 * (i.e. the background evaluation loop is running), 0 otherwise.
 */
int Skink_ruleset_is_running(SkinkRuleset *rs) {
  pthread_mutex_lock(&rs->mu);
  int r = rs->running;
  pthread_mutex_unlock(&rs->mu);
  return r;
}

/*
 * Skink_ruleset_evaluate runs all rule eval functions.  The mutex is held
 * during evaluation to prevent the rules array from being modified
 * concurrently by Skink_ruleset_add_rule.
 */
void Skink_ruleset_evaluate(SkinkRuleset *rs) {
  /* Snapshot the rule list under the lock, then invoke eval callbacks
   * without holding it: rules may re-enter the API (add_rule etc.)
   * without deadlocking the eval thread. */
  pthread_mutex_lock(&rs->mu);
  int n = rs->count;
  SkinkRuleEntry *snapshot = NULL;
  if (n > 0) {
    snapshot = malloc(sizeof(SkinkRuleEntry) * n);
    if (snapshot) {
      memcpy(snapshot, rs->rules, sizeof(SkinkRuleEntry) * n);
    } else {
      n = 0;
    }
  }
  pthread_mutex_unlock(&rs->mu);
  for (int i = 0; i < n; i++) {
    if (snapshot[i].eval) {
      snapshot[i].eval(snapshot[i].self);
    }
  }
  free(snapshot);
}

/*
 * Skink_ruleset_loop is the background thread entry point.  It repeatedly
 * evaluates all rules with a 1 ms sleep between ticks until the running
 * flag is cleared, then signals the stopped condition variable so that
 * Skink_ruleset_stop can return.
 */
void *Skink_ruleset_loop(void *arg) {
  SkinkRuleset *rs = (SkinkRuleset *)arg;
  while (1) {
    pthread_mutex_lock(&rs->mu);
    int running = rs->running;
    pthread_mutex_unlock(&rs->mu);
    if (!running)
      break;
    Skink_ruleset_evaluate(rs);
    SKINK_USLEEP(1000); /* 1 ms tick interval */
  }
  /* Signal that the thread has exited so stop() can return. */
  pthread_mutex_lock(&rs->mu);
  rs->thread_active = 0;
  pthread_cond_signal(&rs->stopped_cv);
  pthread_mutex_unlock(&rs->mu);
  return NULL;
}

/*
 * Skink_ruleset_destroy frees the ruleset and its resources.  The caller
 * must ensure the ruleset is stopped before calling this.
 */
void Skink_ruleset_destroy(SkinkRuleset *rs) {
  if (!rs)
    return;
  /* Defensive: ensure the ruleset is fully stopped (and the eval thread
   * joined) before freeing, even if the caller forgot to stop it. */
  Skink_ruleset_stop(rs);
  pthread_mutex_destroy(&rs->mu);
  pthread_cond_destroy(&rs->stopped_cv);
  free(rs->rules);
  free(rs);
}
