// Copyright 2026 Mark Oxley. All rights reserved.
//
// Project: Skink — A self-hosted, statically-typed programming language
//          targeting LLVM IR, designed for systems programming with
//          modern ergonomics and a Go-inspired syntax.
//
// File:    tests/win_regex_selftest.c
// Purpose: Linux-runnable self-test for the Windows regex shim
//          (std/runtime/win_regex.h). Guards VULN-08:
//            - the group backtracking loop used to free() the compiled
//              group buffer and then keep matching through it
//              (use-after-free);
//            - an unclosed '(' made group_len underflow to -1, feeding
//              memcpy a (size_t)-1 length (giant OOB read/write).
//          The file lives outside std/runtime/ deliberately: a main()
//          in runtime/*.c would break every program link.
//          Run under ASan/UBSan for real UAF detection:
//            gcc -fsanitize=address,undefined tests/win_regex_selftest.c -o /tmp/wr_st
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

#include <stdio.h>
#include <string.h>

#include "../std/runtime/win_regex.h"

static int failures = 0;

// check asserts cond, printing msg on failure.
static void check(int cond, const char *msg) {
    if (!cond) {
        failures++;
        fprintf(stderr, "FAIL: %s\n", msg);
    }
}

// match runs regexec on pat against text, returning the regexec code.
static int match(const char *pat, const char *text) {
    regex_t re;
    int rc = regcomp(&re, pat, 0);
    if (rc != 0) {
        return rc;
    }
    rc = regexec(&re, text, 0, NULL, 0);
    regfree(&re);
    return rc;
}

int main(void) {
    // Unclosed groups must fail cleanly (pre-fix: memcpy(-1) underflow).
    check(match("a(", "a") == REG_NOMATCH, "unclosed '(' must not match");
    check(match("(ab", "ab") == REG_NOMATCH, "unclosed '(ab' must not match");
    check(match("a(b", "ab") == REG_NOMATCH, "trailing unclosed group must not match");
    check(match("(", "") == REG_NOMATCH, "lone '(' must not match");

    // Quantified groups drive the backtracking path that used to read
    // the freed group buffer (pre-fix: UAF; ASan catches it).
    check(match("(ab)+", "ababab") == 0, "(ab)+ should match ababab");
    check(match("(ab)+c", "ababc") == 0, "(ab)+c should match ababc");
    check(match("(ab)*c", "abababc") == 0, "(ab)*c should match");
    check(match("(ab){2,3}", "abab") == 0, "(ab){2,3} should match abab");
    check(match("(a?)+b", "aab") == 0, "(a?)+b should match aab");

    // Nested/edge patterns — must not crash regardless of result.
    match("((((a))))", "a");
    match("(a(b)c)+", "abcabc");
    match("()", "");
    match("(a)+b", "b");

    // Sanity: ordinary matching still works.
    check(match("abc", "abc") == 0, "literal match broken");
    check(match("abc", "abd") == REG_NOMATCH, "literal nomatch broken");
    check(match("a.c", "axc") == 0, "dot match broken");
    check(match("[0-9]+", "123") == 0, "class match broken");

    if (failures) {
        fprintf(stderr, "win_regex_selftest: %d failure(s)\n", failures);
        return 1;
    }
    printf("win_regex_selftest: all checks passed\n");
    return 0;
}
