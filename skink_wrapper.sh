#!/bin/bash
# Copyright 2026 Mark Oxley. All rights reserved.
#
# Project: Skink — A self-hosted, statically-typed programming language
#          targeting LLVM IR, designed for systems programming with
#          modern ergonomics and a Go-inspired syntax.
#
# File:    skink_wrapper.sh
# Purpose: Wrapper script for the skink compiler that adds the template bridge file when linking.
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

# Wrapper script for the skink compiler that adds the template bridge file
# when linking.

# Extract -o argument

output=""
emit_ll=false
check_only=false
input=""

args=("$@")
i=0
while [ $i -lt ${#args[@]} ]; do
    case "${args[$i]}" in
        -o)
            i=$((i + 1))
            output="${args[$i]}"
            ;;
        -emit-ll)
            emit_ll=true
            ;;
        -check)
            check_only=true
            ;;
        *)
            input="${args[$i]}"
            ;;
    esac
    i=$((i + 1))
done

SKINK_REAL_BIN="${SKINK_REAL_BIN:-/usr/local/bin/skink}"
SKINK_HOME="${SKINK_HOME:-/usr/local/lib/skink}"

if [ "$check_only" = true ] || [ "$emit_ll" = true ]; then
    # Pass through directly for check or emit-ll
    exec "$SKINK_REAL_BIN" "$@"
fi

if [ -z "$output" ] || [ -z "$input" ]; then
    exec "$SKINK_REAL_BIN" "$@"
fi

# Compile to LLVM IR first
ll_file="/tmp/skink_wrapper_$$.ll"
err_file="/tmp/skink_wrapper_err_$$"
"$SKINK_REAL_BIN" -emit-ll "$input" > "$ll_file" 2>"$err_file"
rc=$?
if [ $rc -ne 0 ]; then
    cat "$err_file" >&2
    rm -f "$ll_file" "$err_file"
    exit $rc
fi
rm -f "$err_file"

# Link with runtime files and the template bridge
runtime_files=""
for f in "$SKINK_HOME"/runtime/*.c; do
    runtime_files="$runtime_files $f"
done
bridge_file="$SKINK_HOME/runtime/template_bridge.ll"

# Check if bridge file exists in SKINK_HOME, fall back to bootstrap-home
if [ ! -f "$bridge_file" ]; then
    bridge_file="$(dirname "$(dirname "$0")")/bootstrap-home/runtime/template_bridge.ll"
fi

if [ -f "$bridge_file" ]; then
    runtime_files="$runtime_files $bridge_file"
fi

clang -march=native -Wno-override-module -o "$output" "$ll_file" $runtime_files -lpthread -ldl -lm -lssl -lcrypto -lz -lsqlite3 2>&1
rc=$?
rm -f "$ll_file"
if [ $rc -ne 0 ]; then
    exit $rc
fi
echo "built $output"
exit 0
