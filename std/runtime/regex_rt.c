// Copyright 2026 Mark Oxley. All rights reserved.
//
// Project: Skink — A self-hosted, statically-typed programming language
//          targeting LLVM IR, designed for systems programming with
//          modern ergonomics and a Go-inspired syntax.
//
// File:    std/runtime/regex_rt.c
// Purpose: regex_rt.c — POSIX regex runtime for Skink.
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

// regex_rt.c — POSIX regex runtime for Skink.
//
// Provides wrapper functions around POSIX regex.h so Skink code can use
// real regular expressions without implementing a regex engine from scratch.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "win_regex.h"
#else
#include <regex.h>
#endif
#include <errno.h>

/* Skink_res_track / Skink_res_untrack (rc_rt.c) register native-resource
 * blocks so heap scanners treat them as reachable and the process-exit
 * sweep destroys them.  The regex_t pointer is surfaced to Skink as an
 * int64 handle inside a by-value Regex struct, so no single owner can free
 * it deterministically (comp_error.md #103). */
extern void Skink_res_track(void *ptr, void (*dtor)(void *));
extern void Skink_res_untrack(void *ptr);

/* skink_regex_dtor runs regfree when the exit sweep reclaims a still-
 * tracked compiled pattern. */
static void skink_regex_dtor(void *p) { regfree((regex_t *)p); free(p); }

// Skink_regex_compile — compiles a POSIX extended regex pattern.
// Returns a malloc'd regex_t* (as void*), or NULL on failure.
// error_buf is filled with the error message on failure (must be 256+ bytes).
void *Skink_regex_compile(const char *pattern, char *error_buf, int error_buf_size) {
    regex_t *re = (regex_t *)malloc(sizeof(regex_t));
    if (re == NULL) {
        strncpy(error_buf, "out of memory", error_buf_size - 1);
        error_buf[error_buf_size - 1] = '\0';
        return NULL;
    }
    int flags = REG_EXTENDED | REG_NEWLINE;
    int rc = regcomp(re, pattern, flags);
    if (rc != 0) {
        regerror(rc, re, error_buf, error_buf_size);
        regfree(re);
        free(re);
        return NULL;
    }
    error_buf[0] = '\0';
    Skink_res_track(re, skink_regex_dtor);
    return re;
}

// Skink_regex_free — frees a compiled regex.  Untracks the block first so
// the exit sweep cannot double-free it.
void Skink_regex_free(void *re) {
    if (re) {
        Skink_res_untrack(re);
        regfree((regex_t *)re);
        free(re);
    }
}

// Skink_regex_match — returns 1 if the entire text matches the pattern.
int Skink_regex_match(void *re, const char *text) {
    if (!re || !text) return 0;
    int rc = regexec((regex_t *)re, text, 0, NULL, 0);
    return (rc == 0) ? 1 : 0;
}

// Skink_regex_find — finds the first match. Fills start and end out-params.
// Returns 1 on match, 0 on no match.
int Skink_regex_find(void *re, const char *text, int *start, int *end) {
    if (!re || !text) return 0;
    regmatch_t match;
    int rc = regexec((regex_t *)re, text, 1, &match, 0);
    if (rc != 0) {
        return 0;
    }
    if (start) *start = (int)match.rm_so;
    if (end) *end = (int)match.rm_eo;
    return 1;
}

// Skink_regex_find_at — finds the first match starting at offset.
// Returns 1 on match, 0 on no match.
int Skink_regex_find_at(void *re, const char *text, int offset, int *start, int *end) {
    if (!re || !text) return 0;
    int text_len = (int)strlen(text);
    if (offset < 0 || offset > text_len) return 0;
    regmatch_t match;
    int rc = regexec((regex_t *)re, text + offset, 1, &match, 0);
    if (rc != 0) {
        return 0;
    }
    if (start) *start = (int)match.rm_so + offset;
    if (end) *end = (int)match.rm_eo + offset;
    return 1;
}

// Skink_regex_count_matches — counts non-overlapping matches in text.
int Skink_regex_count_matches(void *re, const char *text) {
    if (!re || !text) return 0;
    int count = 0;
    int offset = 0;
    int text_len = (int)strlen(text);
    while (offset <= text_len) {
        regmatch_t match;
        int rc = regexec((regex_t *)re, text + offset, 1, &match, 0);
        if (rc != 0) break;
        count++;
        if (match.rm_eo == 0) {
            // Empty match — advance by 1 to avoid infinite loop
            offset++;
        } else {
            offset += (int)match.rm_eo;
        }
    }
    return count;
}
