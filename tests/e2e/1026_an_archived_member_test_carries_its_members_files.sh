#!/usr/bin/env bash
# requires: python3
# 1026 -- an archived member test carries its member's files (SPEC-015 §4).
#
# In a workspace, a member's programs and tests are placed in the member's own
# product directory with the files its `[runtime] deploy` names. `mcpp test
# --no-run --archive` copies each test with the files beside it; on
# 2026.11.7.1 the copy named the files where a plan of one package places
# them (`bin/<file>`), not the member's directory (`bin/<member>/<file>`), and
# the archive of a member with a deploy set was refused: "cannot copy … into
# the test archive".
#
# Criteria:
#   A. `mcpp test -p tool --no-run --archive` writes the archive, and the
#      member's deployed file is beside its test there.
#   B. In a checkout without target/, the archived test runs and finds the
#      file beside it.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

mkdir -p "$TMP/ws/app/src" "$TMP/ws/tool/src" "$TMP/ws/tool/tests" "$TMP/ws/tool/notices"
cd "$TMP/ws"
printf '[workspace]\nmembers = ["app", "tool"]\n' > mcpp.toml
printf '[package]\nname    = "app"\nversion = "0.1.0"\n' > app/mcpp.toml
printf 'int main() { return 0; }\n' > app/src/main.cpp
cat > tool/mcpp.toml <<'TOML'
[package]
name    = "tool"
version = "0.1.0"

[runtime]
deploy = [ { from = "notices/LICENSE", to = "licenses/tool" } ]
TOML
printf 'tool license\n' > tool/notices/LICENSE
printf 'int main() { return 0; }\n' > tool/src/main.cpp
cat > tool/tests/notice.cpp <<'CPP'
import std;
int main(int, char** argv) {
    auto here = std::filesystem::absolute(argv[0]).parent_path();
    std::ifstream in(here / "licenses" / "tool" / "LICENSE");
    std::string line;
    std::getline(in, line);
    return line == "tool license" ? 0 : 1;
}
CPP

check_archive() {   # $1 archive, $2 log
    rm -rf "$TMP/x" && mkdir -p "$TMP/x" && tar -xzf "$1" -C "$TMP/x"
    python3 - "$TMP/x" <<'PY' || fail "the archive does not carry the member's file" "$2"
import json, pathlib, sys
m = next(pathlib.Path(sys.argv[1]).glob("*/mcpp-tests.json"))
d = json.load(open(m))
tests = [t for leg in d["legs"] for t in leg["tests"] if t["name"] == "notice"]
assert tests, d
t = tests[0]
files = [f for f in t["files"] if f.endswith("licenses/tool/LICENSE")]
assert files, t
assert (m.parent / files[0]).read_text() == "tool license\n", files
PY
}

# A
"$MCPP" test -p tool --no-run --archive "$TMP/a.tar.gz" > a.log 2>&1 || fail "A: -p tool --no-run --archive" a.log
check_archive "$TMP/a.tar.gz" a.log
echo "ok: a member's archived test carries the member's file"

# B
mkdir -p "$TMP/co" && cp -R "$TMP/ws/mcpp.toml" "$TMP/ws/app" "$TMP/ws/tool" "$TMP/co/"
rm -rf "$TMP/co/target" "$TMP/co/app/target" "$TMP/co/tool/target"
cd "$TMP/co"
"$MCPP" test -p tool --archive "$TMP/a.tar.gz" > b.log 2>&1 || fail "B: mcpp test --archive" b.log
grep -q "notice ... ok" b.log || fail "B: the archived test did not find its file" b.log
echo "ok: the archived test finds the file beside it"

echo "PASS: 1026 an archived member test carries its member's files"
