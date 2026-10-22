#!/bin/sh
# Copyright 2026 Mark Oxley. All rights reserved.
#
# Project: Skink — A self-hosted, statically-typed programming language
#          targeting LLVM IR, designed for systems programming with
#          modern ergonomics and a Go-inspired syntax.
#
# File:    skink_bench.sh
# Purpose: Benchmark runner invoked by 'skink -b <target>'.
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

# Benchmark runner invoked by 'skink -b <target>'.
# Mirrors skink_test.sh but looks for *_bench*.skink files and Bench* functions.

SKINK="${SKINK:-./skink}"
SKINK_HOME="${SKINK_HOME:-./bootstrap-home}"
TARGET="$1"

run_one() {
    f="$1"
    dir=$(dirname "$f")
    base=$(basename "$f" .skink)

    # The basename is interpolated into generated Skink source
    # (import "$base" and $base.$fn()); anything outside the identifier
    # grammar would inject code into the compiled bench main (VULN-23).
    case "$base" in
        *[!A-Za-z0-9_]* | "" | [0-9]*)
            echo "SKIP $f (unsafe module name '$base')"
            return 0
            ;;
    esac
    main=$dir/__skink_bench_main_$$.skink
    bin=$dir/__skink_bench_bin_$$

    fns=$(grep -oE '^pub fn (Bench[A-Za-z0-9_]+)' "$f" | sed 's/pub fn //' | tr '\n' ' ')
    if [ -z "$fns" ]; then
        echo "SKIP $f (no benchmark functions)"
        return 0
    fi

    {
        echo 'module main'
        echo "import \"$base\""
        echo ''
        echo 'fn main() -> int {'
        for fn in $fns; do
            echo "    $base.$fn()"
        done
        echo '    return 0'
        echo '}'
    } > "$main"

    if SKINK_HOME="$SKINK_HOME" "$SKINK" -o "$bin" "$main"; then
        rm -f "$main"
        if "$bin"; then
            echo "PASS $f"
            rm -f "$bin"
            return 0
        else
            echo "FAIL $f (runtime)"
            rm -f "$bin"
            return 1
        fi
    else
        rm -f "$main"
        echo "FAIL $f (compile error)"
        return 1
    fi
}

if [ -f "$TARGET" ]; then
    case "$TARGET" in
        *_bench.skink)
            run_one "$TARGET"
            ;;
        *)
            bin="${TARGET%.*}_bin"
            if SKINK_HOME="$SKINK_HOME" "$SKINK" -o "$bin" "$TARGET"; then
                if "$bin"; then
                    echo "PASS $TARGET"
                    rm -f "$bin"
                    exit 0
                else
                    echo "FAIL $TARGET (runtime)"
                    rm -f "$bin"
                    exit 1
                fi
            else
                echo "FAIL $TARGET (compile error)"
                exit 1
            fi
            ;;
    esac
elif [ -d "$TARGET" ]; then
    files=$(find "$TARGET" -maxdepth 1 -name '*_bench.skink' | sort)
    passed=0
    for f in $files; do
        run_one "$f" || passed=1
    done
    exit $passed
else
    case "$TARGET" in
        *...)
            base="${TARGET%...}"
            if [ -z "$base" ]; then
                base="."
            fi
            if [ -d "$base" ]; then
                files=$(find "$base" -name '*_bench.skink' | sort)
                passed=0
                for f in $files; do
                    run_one "$f" || passed=1
                done
                exit $passed
            fi
            ;;
    esac
    echo "benchmark target not found: $TARGET"
    exit 1
fi
