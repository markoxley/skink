#!/bin/bash
# Copyright 2026 Mark Oxley. All rights reserved.
#
# Project: Skink — A self-hosted, statically-typed programming language
#          targeting LLVM IR, designed for systems programming with
#          modern ergonomics and a Go-inspired syntax.
#
# File:    run_tests.sh
# Purpose: Comprehensive test runner for the self-hosted skink-pure compiler.
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

# Comprehensive test runner for the self-hosted skink-pure compiler.
# Compiles and runs each *_test.skink file in std/, reporting results.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKINK_HOME="$SCRIPT_DIR"
SKINK_BIN="${SKINK:-$SCRIPT_DIR/skink}"
export SKINK_HOME SKINK_BIN
RESULTS_DIR="/tmp/skink_test_results"
rm -rf "$RESULTS_DIR"
mkdir -p "$RESULTS_DIR"

# Sanitizer mode selection. VALGRIND=1 runs each test binary under valgrind;
# ASAN=1 compiles every binary (tests and book examples) with clang's
# AddressSanitizer + LeakSanitizer via SKINK_CLANG_FLAGS, which the compiler
# driver appends to its clang invocation. ASAN binaries report leaks/UAF by
# exiting non-zero, which the harness counts as a runtime failure.
if [ "${ASAN:-0}" = "1" ]; then
    export SKINK_CLANG_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g"
    export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}"
    echo "ASAN mode: compiling tests with -fsanitize=address (LeakSanitizer on)"
elif [ "${VALGRIND:-0}" = "1" ]; then
    echo "VALGRIND mode: running test binaries under valgrind"
fi

pass=0
fail=0
skip=0
compile_fail=0
total_tests=0
total_failures=0

# Get all test files from the chosen SKINK_HOME, scanning subdirectories.
# Match both the standard "<name>_test.skink" pattern and standalone tests
# such as "json_test_marshal_string.skink" (which carry _test in the middle).
test_files=$(find "$SKINK_HOME/std" -type f -name '*_test*.skink' ! -path '*/runtime/*' | sort)

for test_file in $test_files; do
    base=$(basename "$test_file" .skink)
    # Display label includes the path relative to std/ so subdirectory tests
    # (e.g. web/web_test) are distinguishable in the output.
    rel_to_std=$(echo "$test_file" | sed "s|^$SKINK_HOME/std/||")
    label="${rel_to_std%.skink}"
    # Filesystem-safe slug used for result file names (unique per path).
    slug=$(echo "$label" | tr '/' '_')

    # Detect the module declaration. Standalone tests declare `module main`
    # and provide their own `fn main()`; they are compiled and run directly.
    module_name=$(grep -m1 '^module ' "$test_file" | awk '{print $2}')

    if [ "$module_name" = "main" ]; then
        # Standalone test program: compile and run directly.
        compile_output=$(SKINK_HOME="$SKINK_HOME" timeout 120 "$SKINK_BIN" -o "$RESULTS_DIR/${slug}_bin" "$test_file" 2>&1)
        compile_rc=$?
        if [ $compile_rc -ne 0 ]; then
            compile_fail=$((compile_fail + 1))
            echo "FAIL $label (compile error)"
            echo "$compile_output" | head -5 | sed 's/^/  /'
            continue
        fi
        if [ "${VALGRIND:-0}" = "1" ]; then
            test_output=$(timeout 60 valgrind --error-exitcode=42 --leak-check=full --show-leak-kinds=definite,possible "$RESULTS_DIR/${slug}_bin" 2>&1)
        elif [ "${ASAN:-0}" = "1" ]; then
            # ASAN-instrumented binaries run slower; give them the same
            # headroom valgrind gets.
            test_output=$(timeout 60 "$RESULTS_DIR/${slug}_bin" 2>&1)
        else
            test_output=$(timeout 10 "$RESULTS_DIR/${slug}_bin" 2>&1)
        fi
        test_rc=$?
        total_tests=$((total_tests + 1))
        if [ $test_rc -eq 0 ]; then
            pass=$((pass + 1))
            echo "PASS $label (1 test)"
        else
            fail=$((fail + 1))
            total_failures=$((total_failures + 1))
            echo "FAIL $label (runtime failure)"
            echo "$test_output" | head -10 | sed 's/^/  /'
        fi
        continue
    fi

    # Module name is the file name without _test suffix and without path
    module_name="${base%_test}_test"

    # Get all Test* function names from the test file
    test_fns=$(grep -oP '^pub fn (Test\w+)' "$test_file" | sed 's/pub fn //')
    if [ -z "$test_fns" ]; then
        skip=$((skip + 1))
        echo "SKIP $label (no test functions)"
        continue
    fi

    # Generate test main
    main_file="$RESULTS_DIR/${slug}_main.skink"
    # Determine the import path for the test module, relative to std/.
    # e.g. std/web/web_test.skink -> "std/web/web_test"
    import_path="std/${rel_to_std%.skink}"

    {
        echo 'module main'
        echo 'import "std/testing"'
        echo "import \"${import_path}\""
        echo ''
        echo 'fn main() -> int {'
        echo '    passed := true'
        for fn in $test_fns; do
            echo '    {'
            echo '        testing.Reset()'
            echo "        ${module_name}.${fn}()"
            echo '        if testing.Failed() {'
            echo '            passed = false'
            echo '        }'
            echo '    }'
            total_tests=$((total_tests + 1))
        done
        echo '    if passed {'
        echo '        return 0'
        echo '    }'
        echo '    return 1'
        echo '}'
    } > "$main_file"

    # Compile the test
    compile_output=$(SKINK_HOME="$SKINK_HOME" timeout 120 "$SKINK_BIN" -o "$RESULTS_DIR/${slug}_bin" "$main_file" 2>&1)
    compile_rc=$?

    if [ $compile_rc -ne 0 ]; then
        compile_fail=$((compile_fail + 1))
        echo "FAIL $label (compile error)"
        echo "$compile_output" | head -5 | sed 's/^/  /'
        continue
    fi

    # Run the test (under valgrind if VALGRIND=1; ASAN binaries exit non-zero
    # on leak/error reports on their own).
    if [ "${VALGRIND:-0}" = "1" ]; then
        test_output=$(timeout 60 valgrind --error-exitcode=42 --leak-check=full --show-leak-kinds=definite,possible "$RESULTS_DIR/${slug}_bin" 2>&1)
    elif [ "${ASAN:-0}" = "1" ]; then
        test_output=$(timeout 60 "$RESULTS_DIR/${slug}_bin" 2>&1)
    else
        test_output=$(timeout 10 "$RESULTS_DIR/${slug}_bin" 2>&1)
    fi
    test_rc=$?

    if [ $test_rc -eq 0 ]; then
        pass=$((pass + 1))
        echo "PASS $label ($(echo "$test_fns" | wc -w) tests)"
    else
        fail=$((fail + 1))
        total_failures=$((total_failures + 1))
        echo "FAIL $label (runtime failures)"
        echo "$test_output" | head -10 | sed 's/^/  /'
    fi
done

echo ""
echo "============================================"
echo "TEST RESULTS"
echo "============================================"
echo "Test suites passed:     $pass"
echo "Test suites failed:     $fail"
echo "Test suites compile-fail: $compile_fail"
echo "Test suites skipped:    $skip"
echo "Total test functions:   $total_tests"
echo "============================================"

# Compile-test every code sample extracted from the book.
book_fail=0
if [ -f "$SCRIPT_DIR/book/test_examples.sh" ]; then
    echo ""
    echo "Book examples (book/example_code/):"
    bash "$SCRIPT_DIR/book/test_examples.sh" || book_fail=1
fi

if [ $fail -gt 0 ] || [ $compile_fail -gt 0 ] || [ $book_fail -gt 0 ]; then
    exit 1
fi
exit 0
