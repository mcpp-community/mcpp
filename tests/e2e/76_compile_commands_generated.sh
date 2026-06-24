#!/usr/bin/env bash
# requires:
# 76_compile_commands_generated.sh — `mcpp build` of a minimal project must
# emit a valid Clang compilation database (compile_commands.json) at the
# project root, on EVERY platform (Linux / macOS / Windows). IDE/clangd
# integration depends on it, and generation is unconditional in the ninja
# backend, so this guards the contract directly rather than as a side effect
# of the more specific CDB tests (47 prebuilt-module-path, 59 std flag).
#
# No `requires:` capability → runs on all three CI platforms.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new app > /dev/null
cd app
"$MCPP" build > /dev/null

cdb=compile_commands.json
[[ -f "$cdb" ]] || { echo "FAIL: no $cdb generated at project root"; exit 1; }
[[ -s "$cdb" ]] || { echo "FAIL: $cdb is empty"; exit 1; }

# Top-level must be a JSON array.
first="$(head -c 1 "$cdb")"
[[ "$first" == "[" ]] || {
    echo "FAIL: $cdb does not start with '[' (got '$first') — not a JSON array"
    cat "$cdb"; exit 1
}

# CDB-required keys + the clang command form (command OR arguments).
for key in '"directory"' '"file"'; do
    grep -q "$key" "$cdb" || { echo "FAIL: $cdb missing $key"; cat "$cdb"; exit 1; }
done
grep -qE '"command"|"arguments"' "$cdb" || {
    echo "FAIL: $cdb has neither \"command\" nor \"arguments\""; cat "$cdb"; exit 1
}

# The minimal project's source (src/main.cpp) must have an entry.
grep -q 'main\.cpp' "$cdb" || { echo "FAIL: $cdb has no entry for src/main.cpp"; cat "$cdb"; exit 1; }

# Deeper structural validation when a JSON parser is available (GitHub-hosted
# runners ship python3). Skips cleanly where it isn't, keeping the grep checks
# above as the portable baseline.
if command -v python3 >/dev/null 2>&1; then
    python3 - "$cdb" <<'PY' || exit 1
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8"))
assert isinstance(d, list) and d, "CDB must be a non-empty JSON array"
for e in d:
    assert "file" in e and "directory" in e, "entry missing file/directory: %r" % e
    assert ("command" in e) or ("arguments" in e), "entry missing command/arguments: %r" % e
print("  json validation OK (%d entries)" % len(d))
PY
fi

echo "OK"
