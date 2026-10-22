#!/bin/bash
# Copyright 2026 Mark Oxley. All rights reserved.
#
# Project: Skink — A self-hosted, statically-typed programming language
#          targeting LLVM IR, designed for systems programming with
#          modern ergonomics and a Go-inspired syntax.
#
# File:    tests/harness_injection_test.sh
# Purpose: Regression test for VULN-23: the skink_test.sh and
#          skink_bench.sh harnesses interpolated each test file's
#          basename verbatim into generated Skink source
#          (`import "$base"`, `$base.$fn()`), so a hostile filename
#          injected arbitrary code into a compiled-and-run program.
#          Basenames are now restricted to the identifier grammar and
#          generated mains use PID-unique names.
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
REPO_DIR="$(dirname "$SCRIPT_DIR")"
SKINK_TEST_SH="$REPO_DIR/skink_test.sh"
SKINK_BENCH_SH="$REPO_DIR/skink_bench.sh"

SKINK="${SKINK_BIN:-$REPO_DIR/skink}"
SKINK_HOME="${SKINK_HOME:-$REPO_DIR}"
export SKINK SKINK_HOME

failures=0

check() {
    if [ "$2" -ne 0 ]; then
        failures=$((failures + 1))
        echo "FAIL: $1"
    fi
}

TMP="$(mktemp -d /tmp/skink_harness_inj.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

# A basename containing `"` closes the generated `import "$base"` line
# early — under the old harness the trailing text became live code.
# The payload avoids `/` (illegal in filenames) by trailing a harmless
# empty-string expression instead of a line comment, and builds its
# marker via concatenation so the literal "PWNED" never appears in the
# filename itself (the harness echoes unsafe basenames in SKIP lines).
EVIL_FILE="$TMP/evil\"; println(\"PW\" + \"NED\"); \"_test.skink"
cat > "$EVIL_FILE" <<'EOF'
module evil
import "std/testing"
pub fn TestNothing() {
    testing.AssertTrue(true, "ok")
}
EOF

# The injection check must run even without a working skink binary:
# the harness skips unsafe basenames before ever invoking it.
out="$(sh "$SKINK_TEST_SH" "$TMP" 2>&1)"
echo "$out" | grep -q "unsafe module name"
check "skink_test.sh did not reject unsafe basename" $?
# The injected code must never execute: PWNED must not appear.
if echo "$out" | grep -q "PWNED"; then
    failures=$((failures + 1))
    echo "FAIL: injected filename produced output (code executed)"
fi

# Same guard on the bench harness.
EVIL_BENCH="$TMP/evil\"; println(\"PW\" + \"NED\"); \"_bench.skink"
cat > "$EVIL_BENCH" <<'EOF'
module evil
pub fn BenchNothing() -> int {
    return 0
}
EOF
out="$(sh "$SKINK_BENCH_SH" "$TMP" 2>&1)"
echo "$out" | grep -q "unsafe module name"
check "skink_bench.sh did not reject unsafe basename" $?
if echo "$out" | grep -q "PWNED"; then
    failures=$((failures + 1))
    echo "FAIL: injected bench filename produced output (code executed)"
fi

# Positive control + leftover check, only when a compiler is available:
# a valid test file must still run, and no generated main may be left
# behind (fixed name -> PID-unique under VULN-23).
if [ -x "$SKINK" ]; then
    mkdir -p "$TMP/good"
    cat > "$TMP/good/ok_test.skink" <<'EOF'
module ok_test
import "std/testing"
pub fn TestOk() {
    testing.AssertTrue(true, "ok")
}
EOF
    out="$(sh "$SKINK_TEST_SH" "$TMP/good" 2>&1)"
    echo "$out" | grep -q "PASS"
    check "valid test file did not pass under harness" $?
    leftover="$(find "$TMP/good" -name '__skink_test_main*' | wc -l)"
    [ "$leftover" -eq 0 ]
    check "harness left generated main files behind" $?
else
    echo "note: no skink binary at $SKINK; skipping positive-control case"
fi

if [ "$failures" -gt 0 ]; then
    echo "harness_injection_test: $failures failure(s)"
    exit 1
fi
echo "harness_injection_test: all checks passed"
exit 0
