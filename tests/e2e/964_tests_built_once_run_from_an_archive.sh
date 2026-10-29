#!/usr/bin/env bash
# requires: llvm python3
# 964 -- tests built once run from an archive (SPEC-015 §4; design 2026-10-29
# rev 2 §3.4).
#
# `mcpp test --no-run --archive <file>` builds the tests of every leg and
# writes them, with what each needs beside it, as one archive; `mcpp test
# --archive <file>` in a checkout of the project runs the tests this machine
# reaches and builds nothing. An ok image is the same bytes on every system,
# so a CI builds on one machine and runs on the others. On 2026.10.29.1
# `--archive` is an unknown option.
#
# Criteria:
#   A. `mcpp test --target openkal --no-run --archive t.tar.gz` writes the
#      archive; its mcpp-tests.json lists both legs, each with the test.
#   B. In a copy of the project without target/, `mcpp test --archive` runs
#      this machine's leg, reports the other not run, builds nothing and
#      exits 0 (`2` would be a leg it could not establish; the other leg is
#      not reached, which is a leg not run, and some test passed).
#   C. A test that fails in the archive fails the run (exit 1).
#   D. An archive none of whose legs this machine reaches runs nothing and
#      exits 1, saying so.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

case "$(uname -m)" in
    x86_64|amd64)  HOST_ARCH=x86_64;  OTHER=aarch64 ;;
    aarch64|arm64) HOST_ARCH=aarch64; OTHER=x86_64 ;;
    *) echo "SKIP: no ok-image row for $(uname -m)"; exit 0 ;;
esac
OT="$OTHER-openkal"

mkdir -p "$TMP/app/src" "$TMP/app/tests"
cd "$TMP/app"
printf '[package]\nname    = "app"\nversion = "0.1.0"\n' > mcpp.toml
printf 'import std;\nint main() { std::println("app"); }\n' > src/main.cpp
cat > tests/smoke.cpp <<'CPP'
import std;
int main() { std::vector<int> v{2, 1}; std::ranges::sort(v); return v.front() == 1 ? 0 : 1; }
CPP

# A
"$MCPP" test --target openkal --no-run --archive "$TMP/t.tar.gz" > a.log 2>&1 || fail "A: --no-run --archive" a.log
[ -s "$TMP/t.tar.gz" ] || fail "A: no archive written" a.log
mkdir -p "$TMP/x" && tar -xzf "$TMP/t.tar.gz" -C "$TMP/x"
python3 - "$TMP/x" <<'PY' || fail "A: the manifest" a.log
import json, pathlib, sys
m = next(pathlib.Path(sys.argv[1]).glob("*/mcpp-tests.json"))
d = json.load(open(m))
assert d["schema"] == 1 and len(d["legs"]) == 2, d
for leg in d["legs"]:
    names = [t["name"] for t in leg["tests"]]
    assert "smoke" in names, (leg["triple"], names)
    for t in leg["tests"]:
        assert (m.parent / t["program"]).is_file(), t["program"]
PY
echo "ok: the archive holds each leg's tests"

# B: a checkout with nothing built.
mkdir -p "$TMP/co" && cp -R "$TMP/app/src" "$TMP/app/tests" "$TMP/app/mcpp.toml" "$TMP/co/"
cd "$TMP/co"
"$MCPP" test --archive "$TMP/t.tar.gz" > b.log 2>&1 || fail "B: mcpp test --archive" b.log
grep -q "smoke ... ok" b.log || fail "B: this machine's leg did not pass" b.log
grep -q "not run here" b.log || fail "B: the other leg is not reported not run" b.log
# Nothing of the package is built here: no test program and no image of it.
[ -z "$(find target -path '*/.test-archive' -prune -o \( -name 'smoke*' -o -name 'app.oke' \) -print 2>/dev/null)" ] \
    || fail "B: the archive's tests were built again" b.log
echo "ok: an archive's tests run without a build"

# C: a failing test.
cd "$TMP/app"
printf 'int main() { return 3; }\n' > tests/broken.cpp
"$MCPP" test --target openkal --no-run --archive "$TMP/c.tar.gz" > c1.log 2>&1 || fail "C: archive with a failing test" c1.log
cd "$TMP/co"
rc=0; "$MCPP" test --archive "$TMP/c.tar.gz" > c2.log 2>&1 || rc=$?
[ "$rc" = 1 ] || fail "C: a failing archived test exited $rc, not 1" c2.log
grep -q "broken ... FAIL" c2.log || fail "C: the failure is not named" c2.log
echo "ok: an archived test that fails fails the run"

# D: only the other architecture.
cd "$TMP/app" && rm -f tests/broken.cpp
"$MCPP" test --target "$OT" --no-run --archive "$TMP/d.tar.gz" > d1.log 2>&1 || fail "D: archive of $OT" d1.log
cd "$TMP/co"
rc=0; "$MCPP" test --archive "$TMP/d.tar.gz" > d2.log 2>&1 || rc=$?
[ "$rc" = 1 ] || fail "D: an archive this machine reaches no leg of exited $rc, not 1" d2.log
grep -q "no test of .* ran on this machine" d2.log || fail "D: the failure does not say so" d2.log
echo "ok: an archive run that runs nothing fails"

echo "PASS: 964 tests built once run from an archive"
