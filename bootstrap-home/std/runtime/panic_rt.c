// Copyright 2026 Mark Oxley. All rights reserved.
//
// Project: Skink — A self-hosted, statically-typed programming language
//          targeting LLVM IR, designed for systems programming with
//          modern ergonomics and a Go-inspired syntax.
//
// File:    std/runtime/panic_rt.c
// Purpose: Runtime support for the Skink panic() builtin.
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

// Runtime support for the Skink panic() builtin.
// Uses setjmp/longjmp for recoverable panic handling. A panic handler
// can be installed via Skink_panic_push_handler; if a panic occurs while
// a handler is active, control transfers to the handler instead of
// terminating the process. If no handler is installed, the process exits.

#include "skink_compat.h"
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <unistd.h>
#endif

#define MAX_PANIC_DEPTH 64

// panic_jmps stores the setjmp/longjmp recovery points for nested panic handlers.
// All panic state is thread-local (__thread): a panic in a spawned thread
// (e.g. a web connection handler) must unwind within that thread — a
// longjmp into a jmp_buf installed by another thread would be undefined
// behavior, and a shared depth/pending counter would corrupt under
// concurrent push/pop (comp_error.md #46).
static __thread jmp_buf panic_jmps[MAX_PANIC_DEPTH];
// panic_depth tracks the current number of active panic recovery handlers.
static __thread int panic_depth = 0;
// panic_msg_storage holds the message from the most recent panic.
static __thread char *panic_msg_storage = NULL;
// panic_pending is non-zero while a panic is being unwound.
static __thread int panic_pending = 0;

// Skink_panic_push_handler installs a recovery point. Returns 1 if a
// handler is now active, 0 if the stack is full.
int Skink_panic_push_handler(void *jmp_buf_ptr) {
  if (panic_depth >= MAX_PANIC_DEPTH) {
    return 0;
  }
  // Copy the jmp_buf into our stack
  memcpy(panic_jmps[panic_depth], jmp_buf_ptr, sizeof(jmp_buf));
  panic_depth++;
  return 1;
}

// Skink_panic_pop_handler removes the topmost recovery point.
void Skink_panic_pop_handler() {
  if (panic_depth > 0) {
    panic_depth--;
  }
}

// Skink_panic_get_msg returns the message from the most recent panic.
char *Skink_panic_get_msg() { return panic_msg_storage; }

// Skink_panic_pending returns nonzero while a panic is unwinding.
int Skink_panic_pending(void) { return panic_pending; }

// Skink_panic_recover implements recover(): called inside a deferred
// function while a panic is unwinding, it consumes the pending panic and
// returns its message. Outside a panic it returns an empty string.
char *Skink_panic_recover(void) {
  if (!panic_pending) {
    return (char *)"";
  }
  panic_pending = 0;
  char *m = panic_msg_storage;
  panic_msg_storage = NULL;
  return m ? m : (char *)"";
}

// Skink_panic_reraise continues an unrecovered panic to the next handler
// without re-printing the message. If no handler remains, the process exits.
void Skink_panic_reraise(void) {
  if (panic_depth > 0) {
    panic_depth--;
    longjmp(panic_jmps[panic_depth], 1);
  }
  if (panic_msg_storage) {
    fprintf(stderr, "panic: %s\n", panic_msg_storage);
  } else {
    fprintf(stderr, "panic\n");
  }
  _exit(1);
}

// Skink_panic triggers a panic with the given message, transferring control to the nearest handler or exiting.
void Skink_panic(char *msg);

// Skink_bounds_check verifies 0 <= lo <= hi <= len and panics otherwise.
// Used by slice expressions so out-of-bounds slicing fails cleanly
// instead of producing negative allocations or reading past the end.
void Skink_bounds_check(int lo, int hi, int len) {
  if (lo < 0 || hi < lo || hi > len) {
    Skink_panic((char *)"slice bounds out of range");
  }
}

// Skink_panic triggers a panic with the given message, transferring control to the nearest handler or exiting.
void Skink_panic(char *msg) {
  panic_pending = 1;
  // If a recovery handler is installed, longjmp to it. The message is
  // stored for recover() and printed only if the panic goes unhandled.
  if (panic_depth > 0) {
    panic_depth--;
    panic_msg_storage = msg;
    longjmp(panic_jmps[panic_depth], 1);
  }
  if (msg) {
    fprintf(stderr, "panic: %s\n", msg);
  } else {
    fprintf(stderr, "panic\n");
  }
  // No handler: exit with error code instead of abort() to avoid
  // generating core dumps and to allow cleanup via atexit handlers.
  _exit(1);
}

// Returned Skink strings must be RC-managed so the caller's release frees
// them — a plain malloc leaks under RC ownership (comp_error.md #104).
extern void *Skink_rc_alloc(int64_t payload_size);
// Skink_format_float formats a double as a string with the given precision.
char *Skink_format_float(double f, int prec) {
  char *buf = (char *)Skink_rc_alloc(64);
  if (!buf)
    return NULL;
  snprintf(buf, 64, "%.*f", prec, f);
  return buf;
}

// Skink_test_failnow prints a FailNow message and exits the process.
// This implements testing.FailNow() early-exit semantics: the test is
// marked as failed and execution stops immediately. Uses _exit instead
// of abort to avoid core dumps.
void Skink_test_failnow(char *msg) {
  if (msg) {
    fprintf(stderr, "FAIL_NOW: %s\n", msg);
  } else {
    fprintf(stderr, "FAIL_NOW\n");
  }
  _exit(1);
}
