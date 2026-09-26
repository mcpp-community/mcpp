#!/usr/bin/env bash
# requires: gcc python3
# 789 -- under `emit build-database`, a package whose build program fails is
# described without that program's directives, instead of costing its whole
# member (mcpp-community/mcpp#699 item 2, design 2026-09-26 §4.6, D3).
#
# Three packages, each its own single-package project (no workspace: the
# member-containment half of this rule is e2e 787's):
#   - `exits1`: build.mcpp compiles and exits 1.
#   - `nocompile`: build.mcpp does not compile at all.
#   - `libonly`: build.mcpp exits 1, and its only target is a library with no
#     sources of its own -- everything it would link comes from directives
#     the failed run never emitted, so THIS member fails as a whole (E1
#     applies, mcpp-community/mcpp#699 item 1) rather than being described
#     with an empty set.
# Criteria:
#   A. `exits1`: exit 1, `data` present with its source described, one error
#      `MCPP_BUILD_DATABASE_PROGRAM_FAILED` whose `path` is "build.mcpp", and
#      no directive the failed run printed before exiting reaches the unit's
#      arguments (build_program.cppm checks the exit code before parsing any
#      output at all, so nothing from a failed run is ever applied).
#   B. `nocompile`: the same shape; the message names a compiler diagnostic,
#      not a bare exit code.
#   C. `libonly`: `data` is absent and the one diagnostic is
#      `MCPP_BUILD_DATABASE_PLAN_FAILED` with `path` "mcpp.toml" -- the
#      missing directives left the target with nothing to link, which fails
#      the member under the ordinary rule, not under E3's own code.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
PY=python3

# ── A ──────────────────────────────────────────────────────────────────────
mkdir -p "$TMP/exits1/src" && cd "$TMP/exits1"
cat > mcpp.toml <<'EOF'
[package]
name    = "exits1"
version = "0.1.0"
EOF
echo 'int main() { return 0; }' > src/main.cpp
# Prints a directive before failing: build_program.cppm checks the exit code
# BEFORE it parses stdout for directives at all, so this must never reach the
# described unit's arguments.
cat > build.mcpp <<'EOF'
#include <cstdio>
int main() { std::printf("mcpp:cxxflag=-DSHOULD_NOT_APPEAR\n"); return 1; }
EOF
set +e
"$MCPP" emit build-database --format json > a.json 2> a.err
rc=$?
set -e
[ "$rc" = 1 ] || fail "A: exit status $rc, expected 1" a.err a.json
"$PY" - a.json <<'EOF' || fail "A: the envelope" a.json
import json, sys
e = json.load(open(sys.argv[1]))
d = e["data"]
sets = {s["name"]: s for s in d["database"]["sets"]}
assert "exits1" in sets, sets
unit = sets["exits1"]["translation-units"][0]
assert unit["source"].endswith("main.cpp"), unit
assert "-DSHOULD_NOT_APPEAR" not in unit["arguments"], unit["arguments"]
diags = e["diagnostics"]
assert len(diags) == 1, diags
diag = diags[0]
assert diag["code"] == "MCPP_BUILD_DATABASE_PROGRAM_FAILED", diag
assert diag["severity"] == "error", diag
assert diag["path"] == "build.mcpp", diag
EOF
echo "ok: A, the package is described past its program's failure, and nothing it printed leaked in"

# ── B ──────────────────────────────────────────────────────────────────────
mkdir -p "$TMP/nocompile/src" && cd "$TMP/nocompile"
cat > mcpp.toml <<'EOF'
[package]
name    = "nocompile"
version = "0.1.0"
EOF
echo 'int main() { return 0; }' > src/main.cpp
cat > build.mcpp <<'EOF'
int main() { this is not valid c++ }
EOF
set +e
"$MCPP" emit build-database --format json > b.json 2> b.err
rc=$?
set -e
[ "$rc" = 1 ] || fail "B: exit status $rc, expected 1" b.err b.json
"$PY" - b.json <<'EOF' || fail "B: the envelope" b.json
import json, sys
e = json.load(open(sys.argv[1]))
d = e["data"]
sets = {s["name"]: s for s in d["database"]["sets"]}
assert "nocompile" in sets, sets
diags = e["diagnostics"]
assert len(diags) == 1, diags
diag = diags[0]
assert diag["code"] == "MCPP_BUILD_DATABASE_PROGRAM_FAILED", diag
assert diag["path"] == "build.mcpp", diag
# Not a bare exit-code message: an actual compiler diagnostic reached it.
assert "error" in diag["message"], diag["message"]
EOF
echo "ok: B, a build program that does not compile is described the same way"

# ── C ──────────────────────────────────────────────────────────────────────
mkdir -p "$TMP/libonly" && cd "$TMP/libonly"
cat > mcpp.toml <<'EOF'
[package]
name      = "libonly"
version   = "0.1.0"
standard  = "c++23"

[targets.lib]
kind = "lib"
EOF
cat > build.mcpp <<'EOF'
int main() { return 1; }
EOF
set +e
"$MCPP" emit build-database --format json > c.json 2> c.err
rc=$?
set -e
[ "$rc" = 1 ] || fail "C: exit status $rc, expected 1" c.err c.json
"$PY" - c.json <<'EOF' || fail "C: the envelope" c.json
import json, sys
e = json.load(open(sys.argv[1]))
assert "data" not in e, e
diags = e["diagnostics"]
assert len(diags) == 1, diags
diag = diags[0]
assert diag["code"] == "MCPP_BUILD_DATABASE_PLAN_FAILED", diag
assert diag["path"] == "mcpp.toml", diag
EOF
echo "ok: C, missing directives that leave a target with nothing to link fail the whole member"

echo "PASS: 789_emit_describes_a_member_past_a_failed_build_program"
