#!/usr/bin/env bash
# 989 -- a native leg's tests run from an archive (SPEC-015 §4; design
# 2026-10-29 rev 2 §3.4).
#
# e2e 964 archives the legs of the openkal set, which run through the
# platform's runner and need llvm; a leg of the host's own system, whose test
# programs the system starts directly, was archived and run on no runner. On
# macOS such a program, unpacked from an archive written on Linux, was not
# started ("invalid argument", mcppls CI 2026-10-30). This test needs nothing
# but the host's toolchain, so every system's shards run it.
#
# Criteria:
#   A. `mcpp test --no-run --archive t.tar.gz` writes the host leg's tests.
#   B. In a copy of the project without target/, `mcpp test --archive` starts
#      them, passes, and builds nothing.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

# What the system says about an archived program it would not start.
evidence() {
    local program
    program=$(find "$TMP/co/target/.test-archive" -type f -name 'smoke*' ! -name '*.tsv' 2>/dev/null | head -1)
    [ -n "$program" ] || { echo "--- no unpacked program ---"; return; }
    echo "--- $program ---"
    file "$program" 2>&1 || true
    ls -l "$program" 2>&1 || true
    if [ "$(uname -s)" = Darwin ]; then
        ls -l@ "$program" 2>&1 || true
        xattr -l "$program" 2>&1 || true
        codesign -dvvv "$program" 2>&1 | head -20 || true
    fi
    echo "--- started directly ---"
    "$program"; echo "exit $?"
    cp "$program" "$TMP/copy" && "$TMP/copy"; echo "a copy: exit $?"
}

mkdir -p "$TMP/app/src" "$TMP/app/tests"
cd "$TMP/app"
printf '[package]\nname    = "app"\nversion = "0.1.0"\n' > mcpp.toml
printf 'import std;\nint main() { std::println("app"); }\n' > src/main.cpp
cat > tests/smoke.cpp <<'CPP'
import std;
int main() { std::vector<int> v{2, 1}; std::ranges::sort(v); return v.front() == 1 ? 0 : 1; }
CPP

# A
"$MCPP" test --no-run --archive "$TMP/t.tar.gz" > a.log 2>&1 || fail "A: --no-run --archive" a.log
[ -s "$TMP/t.tar.gz" ] || fail "A: no archive written" a.log
echo "ok: the host leg's tests are archived"

# B: a checkout with nothing built.
mkdir -p "$TMP/co" && cp -R "$TMP/app/src" "$TMP/app/tests" "$TMP/app/mcpp.toml" "$TMP/co/"
cd "$TMP/co"
if ! "$MCPP" test --archive "$TMP/t.tar.gz" > b.log 2>&1 || ! grep -q "smoke ... ok" b.log; then
    evidence > ev.log 2>&1
    fail "B: the host leg's archived test did not pass" b.log ev.log
fi
[ -z "$(find target -path '*/.test-archive' -prune -o -name 'smoke*' -print 2>/dev/null)" ] \
    || fail "B: the archive's tests were built again" b.log
echo "ok: the host leg's tests run from the archive"

echo "PASS: 989 a native leg's tests run from an archive"
