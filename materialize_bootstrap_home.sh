#!/usr/bin/env bash
# Copyright 2026 Mark Oxley. All rights reserved.
#
# Project: Skink — A self-hosted, statically-typed programming language
#          targeting LLVM IR, designed for systems programming with
#          modern ergonomics and a Go-inspired syntax.
#
# File:    materialize_bootstrap_home.sh
# Purpose: Shell script for materialize bootstrap home.
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

set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
skink_lang_root=${SKINK_LANG_ROOT:-"$repo_root/../skink-lang"}
target=${SKINK_HOME:-"$repo_root/bootstrap-home"}

copy_skink_tree() {
    local src_root=$1
    local dst_root=$2
    while IFS= read -r -d '' source; do
        rel_path=${source#"$src_root"/}
        dest="$dst_root/$rel_path"
        mkdir -p "$(dirname "$dest")"
        cp -L "$source" "$dest"
    done < <(find "$src_root" -type f -name '*.skink' -print0)
}

mkdir -p "$target/std" "$target/runtime"
copy_skink_tree "$skink_lang_root/std" "$target/std"
copy_skink_tree "$repo_root/std" "$target/std"

for source in "$skink_lang_root/compiler/runtime"/*.c; do
    cp -L "$source" "$target/runtime/$(basename "$source")"
done

if [[ -f "$repo_root/lib/testing.skink" ]]; then
    cp -L "$repo_root/lib/testing.skink" "$target/testing.skink"
fi

printf '%s\n' "$target"
