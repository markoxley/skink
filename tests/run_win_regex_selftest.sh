#!/bin/bash
# Copyright 2026 Mark Oxley. All rights reserved.
#
# Project: Skink — A self-hosted, statically-typed programming language
#          targeting LLVM IR, designed for systems programming with
#          modern ergonomics and a Go-inspired syntax.
#
# File:    tests/run_win_regex_selftest.sh
# Purpose: Build and run the win_regex.h C self-test (VULN-08 guard).
#          Prefers an ASan+UBSan build so the fixed use-after-free and
#          length-underflow paths are actually instrumented; falls back
#          to a plain build when the sanitizer runtime is unavailable.
# Author:  Mark Oxley
# Created: 2026
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$SCRIPT_DIR/win_regex_selftest.c"

CC="${CC:-cc}"
command -v "$CC" >/dev/null 2>&1 || CC=gcc
command -v "$CC" >/dev/null 2>&1 || CC=clang
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "SKIP win_regex_selftest (no C compiler)"
    exit 0
fi

BIN="$(mktemp /tmp/win_regex_selftest.XXXXXX)"
rm -f "$BIN"

# Try sanitizers first — the VULN-08 use-after-free is only observable
# under ASan; fall back to a plain binary if unsupported. Try each
# compiler in turn: gcc can lack libasan even when clang works.
sanitized=0
for cc_try in "$CC" clang gcc; do
    command -v "$cc_try" >/dev/null 2>&1 || continue
    if "$cc_try" -fsanitize=address,undefined -fno-omit-frame-pointer -g \
            -o "$BIN" "$SRC" 2>/dev/null; then
        sanitized=1
        break
    fi
done
if [ "$sanitized" -eq 0 ]; then
    if "$CC" -O1 -g -o "$BIN" "$SRC" 2>/dev/null; then
        echo "note: sanitizers unavailable, running uninstrumented"
    else
        echo "FAIL win_regex_selftest (compile error)"
        rm -f "$BIN"
        exit 1
    fi
fi

ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}" "$BIN"
rc=$?
rm -f "$BIN"
exit $rc
