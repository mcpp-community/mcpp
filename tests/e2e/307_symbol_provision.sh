#!/usr/bin/env bash
# requires: elf
# 307_symbol_provision.sh — one library, one provider (issue #519).
#
# ⚠️ THE SILENT CASE IS THE IMPORTANT ONE, and it is why this test has two
# halves that differ by a single package.
#
# mcpp's own `kind = "shared"` mechanism produces the exact shape the
# diagnostic looks for: a shared dependency's link unit takes only ITS OWN
# objects, so a static package underneath it lands in the consumer's
# executable and the shared library binds back to it at run time. There is one
# copy of the code in the process and nothing is wrong. A check that reported
# "this image exports a symbol a library it loads binds to" would fire on
# every correct build of that shape, and the user could do nothing about it.
#
# The finding is the SECOND provider, not the export. So:
#
#   half 1   wrap.so (shared) → core (static)             must stay SILENT
#   half 2   ... plus alt.so, which defines the same name  must REPORT both
#
# Asserted against `resolution.json` rather than stdout: the message is free
# to improve, the recorded verdict is the contract.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p core/src wrap/src alt/src app/src

cat > core/mcpp.toml <<'EOF'
[package]
name    = "core"
version = "0.1.0"
[build]
c_standard = "c11"
sources    = ["src/*.c"]
[targets.core]
kind = "lib"
EOF
echo 'int shared_answer(void) { return 7; }' > core/src/core.c

# A shared library that CALLS the static package's symbol without defining it.
cat > wrap/mcpp.toml <<'EOF'
[package]
name    = "wrap"
version = "0.1.0"
[build]
c_standard = "c11"
sources    = ["src/*.c"]
[targets.wrap]
kind = "shared"
[dependencies]
core = { path = "../core" }
EOF
cat > wrap/src/wrap.c <<'EOF'
int shared_answer(void);
int wrap_call(void) { return shared_answer() + 1; }
EOF

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
[build]
c_standard = "c11"
[targets.app]
kind = "bin"
main = "src/main.c"
[dependencies]
wrap = { path = "../wrap" }
EOF
cat > app/src/main.c <<'EOF'
#include <stdio.h>
int wrap_call(void);
int main(void) { printf("%d\n", wrap_call()); return 0; }
EOF

verdict() {   # <resolution.json> <field>
    "$PYTHON" - "$1" "$2" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
entries = doc.get("runtime", {}).get("symbol_provision")
if not entries:
    print("MISSING"); raise SystemExit(0)
app = [e for e in entries if e["path"].endswith("app")]
if not app:
    print("NO-APP-ENTRY"); raise SystemExit(0)
print(app[0].get(sys.argv[2], "MISSING"))
PY
}

PYTHON="$(command -v python3 || command -v python || true)"
[[ -n "$PYTHON" ]] || { echo "skip: no python for JSON assertions"; exit 0; }

cd app

# ── half 1: the legitimate arrangement must stay silent ────────────────────
"$MCPP" build > silent.log 2>&1 || { cat silent.log; exit 1; }
res="$(find target -name resolution.json | head -1)"
[[ -n "$res" ]] || { echo "FAIL: no resolution.json"; exit 1; }

status="$(verdict "$res" status)"
[[ "$status" == "clean" ]] || {
    echo "FAIL: the legitimate shared→static arrangement reported '$status'"
    cat "$res"; exit 1; }

# It must have been MEASURED, not skipped: the export is real and the
# denominator is real. "0 findings" and "never looked" must not read alike.
exported="$(verdict "$res" exported)"
total="$(verdict "$res" dynamic_symbols)"
[[ "$exported" -ge 1 ]] || {
    echo "FAIL: expected the executable to export shared_answer, got $exported"
    exit 1; }
[[ "$total" -gt "$exported" ]] || {
    echo "FAIL: implausible denominator ($exported of $total)"; exit 1; }

grep -qi "also provided by" silent.log && {
    echo "FAIL: a correct build printed a conflict warning"; exit 1; }

# ── half 2: a second provider turns it into a finding ──────────────────────
cat > ../alt/mcpp.toml <<'EOF'
[package]
name    = "alt"
version = "0.1.0"
[build]
c_standard = "c11"
sources    = ["src/*.c"]
[targets.alt]
kind = "shared"
EOF
cat > ../alt/src/alt.c <<'EOF'
int shared_answer(void) { return 999; }
int alt_ping(void) { return shared_answer(); }
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
[build]
c_standard = "c11"
[targets.app]
kind = "bin"
main = "src/main.c"
[dependencies]
wrap = { path = "../wrap" }
alt  = { path = "../alt" }
EOF
cat > src/main.c <<'EOF'
#include <stdio.h>
int wrap_call(void);
int alt_ping(void);
int main(void) { printf("%d %d\n", wrap_call(), alt_ping()); return 0; }
EOF

"$MCPP" build > conflict.log 2>&1 || { cat conflict.log; exit 1; }

# ⚠️ `target/<triple>/` ACCUMULATES one directory per fingerprint, and adding a
# dependency changed the fingerprint — so half 1's directory is still there
# with its own `app` and its own clean verdict. Taking "the first" of either
# would answer about the build that is not under test. Everything below is
# keyed off the directory that contains the conflicting verdict.
found=0
conflict_dir=""
for r in $(find target -name resolution.json); do
    if [[ "$(verdict "$r" status)" == "conflict" ]]; then
        conflict_dir="$(dirname "$r")"
        "$PYTHON" - "$r" <<'PY' || exit 1
import json, sys
doc = json.load(open(sys.argv[1]))
app = [e for e in doc["runtime"]["symbol_provision"] if e["path"].endswith("app")][0]
conflicts = app["conflicts"]
names = {c["symbol"] for c in conflicts}
assert "shared_answer" in names, f"expected shared_answer, got {names}"
providers = {p for c in conflicts for p in c["also_provided_by"]}
assert any("libalt.so" in p for p in providers), \
    f"expected libalt.so among providers, got {providers}"
assert all(c["kind"] == "func" for c in conflicts), conflicts
PY
        found=1
    fi
done
[[ "$found" == 1 ]] || {
    echo "FAIL: two providers of shared_answer were not reported"
    cat conflict.log; exit 1; }

# The defect is real, not theoretical: alt's 999 is unreachable because the
# executable's merged copy wins for every caller in the process.
cbin="$conflict_dir/bin/app"
[[ -x "$cbin" ]] || { echo "FAIL: no executable in $conflict_dir"; exit 1; }
"$cbin" | grep -qx "8 7" || {
    echo "FAIL: expected '8 7' (the merged copy winning), got: $("$cbin")"; exit 1; }

# ── --strict turns the finding into a failure, in ONE place ────────────────
touch src/main.c
if "$MCPP" build --strict > strict.log 2>&1; then
    echo "FAIL: --strict did not fail on a reported conflict"; cat strict.log; exit 1
fi

echo "ok: silent on the legitimate arrangement, reported on a real conflict"
