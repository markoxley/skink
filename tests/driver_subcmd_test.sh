#!/usr/bin/env bash
# Regression test: `skink js` and `skink scales` dispatch to the sibling
# skinkjs/scales binaries with all trailing args forwarded. Guards the
# subcommand routing added in skink.skink — fails if the driver stops
# resolving siblings or drops args.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SKINK="${SKINK_BIN:-$ROOT/skink}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# sibling layout: skink + stub tools that echo their argv
ln -s "$SKINK" "$TMP/skink"
cat > "$TMP/skinkjs" <<'EOF'
#!/usr/bin/env bash
echo "SKINKJS:$*"
EOF
cat > "$TMP/scales" <<'EOF'
#!/usr/bin/env bash
echo "SCALES:$*"
EOF
cat > "$TMP/skinkfmt" <<'EOF'
#!/usr/bin/env bash
echo "SKINKFMT:$*"
EOF
cat > "$TMP/skinkcov" <<'EOF'
#!/usr/bin/env bash
echo "SKINKCOV:$*"
EOF
chmod +x "$TMP/skinkjs" "$TMP/scales" "$TMP/skinkfmt" "$TMP/skinkcov"

fail=0
check() {
    local name="$1" want="$2" got="$3"
    if [[ "$got" == "$want" ]]; then
        echo "PASS $name"
    else
        echo "FAIL $name: want [$want] got [$got]"
        fail=1
    fi
}

check "skink js args"     "SKINKJS:-o out.js app.skink" \
    "$(PATH="$TMP:$PATH" "$TMP/skink" js -o out.js app.skink)"
check "skink scales args" "SCALES:install foo@1.0" \
    "$(PATH="$TMP:$PATH" "$TMP/skink" scales install foo@1.0)"
check "skink fmt args"    "SKINKFMT:-w a.skink b.skink" \
    "$(PATH="$TMP:$PATH" "$TMP/skink" fmt -w a.skink b.skink)"
check "skink cover args"  "SKINKCOV:app.skinkcov -v" \
    "$(PATH="$TMP:$PATH" "$TMP/skink" cover app.skinkcov -v)"
check "skink cov alias"   "SKINKCOV:app.skinkcov" \
    "$(PATH="$TMP:$PATH" "$TMP/skink" cov app.skinkcov)"

# missing sibling must fail with a readable error, not a crash —
# run from a dir with only the skink symlink and no tools on PATH
mkdir "$TMP/lonely" && ln -s "$SKINK" "$TMP/lonely/skink"
missing="$(cd "$TMP/lonely" && PATH="/nonexistent" ./skink js 2>&1 || true)"
if [[ "$missing" == *"cannot find skinkjs"* ]]; then
    echo "PASS missing-tool diagnostic"
else
    echo "FAIL missing-tool diagnostic: got [$missing]"
    fail=1
fi

exit $fail
