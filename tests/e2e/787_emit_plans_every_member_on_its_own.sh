#!/usr/bin/env bash
# requires: python3
# 787 -- `emit build-database` plans every selected workspace member on its
# own; one member's planning failure does not cost its siblings' sets
# (mcpp-community/mcpp#699 item 1, design 2026-09-26 §4.4, D1).
#
# Before this fix the member loop stopped at the first failure
# (src/cli/cmd_build.cppm), the same way `mcpp emit build-database --workspace`
# over `good` and `bad` used to report `MCPP_BUILD_DATABASE_PLAN_FAILED` with
# no `data` at all, discarding `good`'s sets along with `bad`'s. `mcpp build
# --workspace` never had this defect (continue-on-failure,
# src/cli/cmd_build.cppm cmd_build) -- this script is `emit`'s analogue.
#
# `bad`'s failure is a genuine PLANNING failure (an unresolvable dependency),
# deliberately NOT a build-program failure: a failing build.mcpp is its own,
# narrower case (E3, #699 item 2 -- see e2e 789), where the member is still
# described. Criteria:
#   A. `emit --workspace --format json` over `good` and `bad`: exit 1, `data`
#      present, its one set is `good/good`, `diagnostics` holds exactly one
#      `error` with `path` "bad/mcpp.toml" and a message naming `bad`, and
#      `watch` lists `bad/mcpp.toml` alongside `good`'s own inputs.
#   B. Without `--format`, the same run prints the document (not empty) and
#      still exits 1 -- the exit code is not conditioned on `--format`.
#   C. A workspace in which every member fails omits `data` altogether, with
#      one diagnostic per member.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
PY=python3

mkdir -p "$TMP/ws/good/src" "$TMP/ws/bad/src"
cd "$TMP/ws"
cat > mcpp.toml <<'EOF'
[workspace]
members = ["good", "bad"]
EOF
cat > good/mcpp.toml <<'EOF'
[package]
name    = "good"
version = "0.1.0"
EOF
echo 'int main() { return 0; }' > good/src/main.cpp
# `bad` names a `path` dependency that does not exist: a plan failure with
# nothing to do with build programs, the same shape #699's own report used
# for the member whose plan never reaches a build.mcpp at all.
cat > bad/mcpp.toml <<'EOF'
[package]
name    = "bad"
version = "0.1.0"

[dependencies]
missing = { path = "../does-not-exist" }
EOF
echo 'int main() { return 0; }' > bad/src/main.cpp

# ── A ──────────────────────────────────────────────────────────────────────
set +e
"$MCPP" emit build-database --workspace --format json > a.json 2> a.err
rc=$?
set -e
[ "$rc" = 1 ] || fail "A: exit status $rc, expected 1" a.err a.json
"$PY" - a.json <<'EOF' || fail "A: the envelope" a.json
import json, sys
e = json.load(open(sys.argv[1]))
d = e["data"]
sets = [s["name"] for s in d["database"]["sets"]]
assert sets == ["good/good"], sets
diags = e["diagnostics"]
assert len(diags) == 1, diags
diag = diags[0]
assert diag["code"] == "MCPP_BUILD_DATABASE_PLAN_FAILED", diag
assert diag["severity"] == "error", diag
assert diag["path"] == "bad/mcpp.toml", diag
assert "bad" in diag["message"], diag["message"]
assert "bad/mcpp.toml" in d["watch"], d["watch"]
assert any(w.startswith("good/") for w in d["watch"]), d["watch"]
EOF
echo "ok: A, one failed member's diagnostic and path, the other member's set kept"

# ── B ──────────────────────────────────────────────────────────────────────
set +e
"$MCPP" emit build-database --workspace > b.out 2> b.err
rc=$?
set -e
[ "$rc" = 1 ] || fail "B: bare invocation exit status $rc, expected 1" b.out b.err
[ -s b.out ] || fail "B: the bare document is empty" b.err
"$PY" -c 'import json,sys; d=json.load(open(sys.argv[1])); assert d["sets"][0]["name"]=="good/good", d["sets"]' b.out \
    || fail "B: the bare document's set" b.out
grep -q "bad" b.err || fail "B: the failed member's reason is not on stderr" b.err
echo "ok: B, the exit code is 1 without --format too, and the document still prints"

# ── C: every member fails ───────────────────────────────────────────────────
cat > good/mcpp.toml <<'EOF'
[package]
name    = "good"
version = "0.1.0"

[dependencies]
missing2 = { path = "../also-does-not-exist" }
EOF
set +e
"$MCPP" emit build-database --workspace --format json > c.json 2> c.err
rc=$?
set -e
[ "$rc" = 1 ] || fail "C: exit status $rc, expected 1" c.err c.json
"$PY" - c.json <<'EOF' || fail "C: the all-failed envelope" c.json
import json, sys
e = json.load(open(sys.argv[1]))
assert "data" not in e, e
diags = e["diagnostics"]
assert len(diags) == 2, diags
paths = sorted(d["path"] for d in diags)
assert paths == ["bad/mcpp.toml", "good/mcpp.toml"], paths
assert all(d["code"] == "MCPP_BUILD_DATABASE_PLAN_FAILED" and d["severity"] == "error"
           for d in diags), diags
EOF
echo "ok: C, a workspace in which every member fails omits data"

echo "PASS: 787_emit_plans_every_member_on_its_own"
