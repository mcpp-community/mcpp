#!/usr/bin/env bash
# requires: unix-shell
# 331_xlings_workspace_one_table.sh — `[xlings.workspace]` is the one table.
#
# Design: .agents/docs/2026-09-03-xlings-workspace-as-the-one-table.md.
#
# NO NETWORK, and that is the whole technique. Every entry reaches the
# provisioning pass, and with auto-install off that pass refuses and PRINTS THE
# ADDRESSES IT WAS GOING TO ASK FOR. So the refusal is the assertion: it shows
# what an entry was assembled into, which is the thing the manifest change is
# about, without installing anything on any shard.
set -uo pipefail

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

fail() { echo "FAIL: $*"; exit 1; }
MCPP="${MCPP:?set MCPP to the mcpp binary under test}"

"$MCPP" new app >/dev/null 2>&1 || fail "mcpp new"
cd app
cp mcpp.toml mcpp.toml.base

# `MCPP_NO_AUTO_INSTALL` makes the provisioning pass refuse and name the
# addresses; `--offline` would do as well. Either way nothing is downloaded.
declare_and_build() {   # $1 = the [xlings] block
    cp mcpp.toml.base mcpp.toml
    printf '\n%s\n' "$1" >> mcpp.toml
    MCPP_NO_AUTO_INSTALL=1 "$MCPP" build 2>&1
}

# ── 1. an entry becomes an install address ─────────────────────────────────
out=$(declare_and_build '[xlings.workspace]
mcpp-e2e-absent = "1.0"')
grep -q "mcpp-e2e-absent@1.0" <<<"$out" \
    || fail "the address the entry assembles into is missing: $out"

# ── 2. the namespace may be written on either half, and means one entry ────
onValue=$(declare_and_build '[xlings.workspace]
mcpp-e2e-absent = "xim:1.0"')
onKey=$(declare_and_build '[xlings.workspace]
"xim:mcpp-e2e-absent" = "1.0"')
for o in "$onValue" "$onKey"; do
    grep -q "xim:mcpp-e2e-absent@1.0" <<<"$o" \
        || fail "namespaced address missing: $o"
done

# The unquoted key form is a TOML error, and the message must not be silence.
out=$(declare_and_build '[xlings.workspace]
xim:mcpp-e2e-absent = "1.0"')
grep -qi "error" <<<"$out" || fail "an unquoted namespaced key was accepted: $out"

# ── 3. `""` asks for presence only ─────────────────────────────────────────
out=$(declare_and_build '[xlings.workspace]
mcpp-e2e-absent = ""')
grep -q "mcpp-e2e-absent" <<<"$out" || fail "unconstrained entry missing: $out"
grep -q "mcpp-e2e-absent@" <<<"$out" && fail "unconstrained entry carries a version: $out"

# ── 4. `deps` is honoured and reported, with the line to write ─────────────
out=$(declare_and_build '[xlings]
deps = ["xim:mcpp-e2e-absent@1.0"]')
grep -q "xim:mcpp-e2e-absent@1.0" <<<"$out" || fail "deps entry not provisioned: $out"
grep -q "\[xlings.workspace\]" <<<"$out" || fail "no advisory naming the new table: $out"
# The recommended spelling, which is the namespace on the KEY: an author names
# a package and then says which version of it.
grep -q '"xim:mcpp-e2e-absent" = "1.0"' <<<"$out" \
    || fail "the advisory does not show the recommended line to write: $out"

# ── 5. one package in both tables, two versions, is refused ───────────────
out=$(declare_and_build '[xlings]
deps = ["mcpp-e2e-absent@2.0"]

[xlings.workspace]
mcpp-e2e-absent = "1.0"')
grep -q "named in both" <<<"$out" || fail "the conflict was accepted: $out"
grep -q "1.0" <<<"$out" || fail "the conflict message omits a version: $out"
grep -q "2.0" <<<"$out" || fail "the conflict message omits a version: $out"

# ── 6. `envs` is refused, naming the key ──────────────────────────────────
out=$(declare_and_build '[xlings.envs]
OPENBLAS_NUM_THREADS = "1"')
grep -q "\[xlings.envs\]" <<<"$out" || fail "envs was accepted: $out"

# ── 7. a project declaring none of it is untouched ────────────────────────
cp mcpp.toml.base mcpp.toml
out=$("$MCPP" build 2>&1) || fail "a project with no [xlings] section: $out"
grep -qi "xlings" <<<"$out" && fail "an undeclared project heard about xlings: $out"

echo "PASS: 331_xlings_workspace_one_table"
