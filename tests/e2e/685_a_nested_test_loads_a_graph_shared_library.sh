#!/usr/bin/env bash
# requires: elf
# 685 -- a test program built from a subdirectory of `tests/` loads a shared
# library the dependency graph built.
#
# A consumer of a graph-built shared library searches its own directory
# (`$ORIGIN`), which holds the library for a program in `bin/`. A test from
# `tests/sub/` is linked to `bin/sub/`, and on 2026.9.14.1 the post-link
# closure check refused it ("libfw.so not found on the search path this
# artifact will actually use"), which failed the whole `mcpp test`. A consumer
# in another directory now also searches the relative path to the library's.
#
# Criteria:
#   1. `mcpp test` builds and passes a nested and a top-level test that both
#      call into the shared library;
#   2. the nested test's search path carries `$ORIGIN/..`;
#   3. negative direction: the program in `bin/` carries no relative entry,
#      so a consumer beside the library keeps its link line.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p fw/src app/src app/tests/sub
cat > fw/mcpp.toml <<'TOML'
[package]
namespace = "demo"
name      = "fw"
version   = "0.1.0"
[targets.fw]
kind = "shared"
TOML
printf 'export module fw;\nexport int fw_anchor() { return 41; }\n' > fw/src/fw.cppm
cat > app/mcpp.toml <<'TOML'
[package]
name    = "app"
version = "0.1.0"
[dependencies]
demo.fw = { path = "../fw" }
TOML
printf 'import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n' > app/src/main.cpp
cp app/src/main.cpp app/tests/sub/deep.cpp
cp app/src/main.cpp app/tests/top.cpp
cd app

# ── 1 ─────────────────────────────────────────────────────────────────────
"$MCPP" test > test.log 2>&1 || fail "1: mcpp test failed" test.log
grep -q "sub/deep ... ok" test.log || fail "1: the nested test did not pass" test.log
grep -q "top ... ok" test.log || fail "1: the top-level test did not pass" test.log

# ── 2 ─────────────────────────────────────────────────────────────────────
deep=$(find target -type f -name deep -path '*/bin/sub/*' | head -1)
[ -n "$deep" ] || fail "2: no bin/sub/deep was built" test.log
readelf -d "$deep" | grep -E 'R(UN)?PATH' | grep -qF '$ORIGIN/..' \
    || fail "2: the nested test does not search \$ORIGIN/.." test.log

# ── 3 ─────────────────────────────────────────────────────────────────────
"$MCPP" build > build.log 2>&1 || fail "3: mcpp build failed" build.log
prog=$(find target -type f -name app -path '*/bin/*' | head -1)
[ -n "$prog" ] || fail "3: no bin/app was built" build.log
if readelf -d "$prog" | grep -E 'R(UN)?PATH' | grep -qF '$ORIGIN/'; then
    fail "3: the program beside the library carries a relative search entry"
fi

echo "PASS: 685_a_nested_test_loads_a_graph_shared_library"
