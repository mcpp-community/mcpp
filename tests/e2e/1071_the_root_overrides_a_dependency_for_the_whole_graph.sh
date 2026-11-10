#!/usr/bin/env bash
# requires: python3
# 1071 — the root overrides a dependency for the whole graph (design
# 2026-11-10 supplement, T2 and T3; docs/04 `override`).
#
#   A. Two requests no merge reconciles (a library pins mcpplibs.os 0.4.1, the
#      root asks for ^0.4.4) are refused, and the refusal names the line that
#      chooses one version for the whole graph.
#   B. With `override = true` on the root's declaration, every request takes
#      the root's version; the replaced request is named once ("Overrode"),
#      and mcpp.lock records the override.
#   C. `--locked` reads the override as part of the recorded resolution: the
#      same version reached without it is drift.
#   D. A library's `override` is ignored, and said to be.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
TMP_HOST="$(host_path "$TMP")"
trap 'rm -rf "$TMP"' EXIT
PY=python3; command -v python3 >/dev/null 2>&1 || PY=python
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f"; cat "$f"; done; exit 1; }

mkdir -p "$TMP/anchor/src" "$TMP/app/src"
cat > "$TMP/anchor/mcpp.toml" <<'EOF'
[package]
name    = "anchor"
version = "0.1.0"
[targets.anchor]
kind = "lib"

[dependencies]
mcpplibs.os = "0.4.1"
EOF
cat > "$TMP/anchor/src/anchor.cppm" <<'EOF'
export module anchor;
export int anchor_answer() { return 1; }
EOF
write_app() {   # write_app OS-DECLARATION
    cat > "$TMP/app/mcpp.toml" <<EOF
[package]
name    = "app"
version = "0.1.0"

[dependencies]
mcpplibs.os = $1
anchor = { path = "$TMP_HOST/anchor" }
EOF
}
cat > "$TMP/app/src/main.cpp" <<'EOF'
import anchor;
int main() { return anchor_answer() == 1 ? 0 : 1; }
EOF
locked_os() { $PY - <<'PY'
import re
text = open("mcpp.lock").read()
m = re.search(r'\[package\."mcpplibs\.os"\]([^\[]*)', text)
body = m.group(1) if m else ""
v = re.search(r'version\s*=\s*"([^"]+)"', body)
print((v.group(1) if v else "") + (" override" if re.search(r'^override\s*=\s*true', body, re.M) else ""))
PY
}
cd "$TMP/app"

# A
write_app '"^0.4.4"'
"$MCPP" build --configure-only > a.log 2>&1 && fail "A: two requests no merge reconciles were accepted" a.log
grep -q 'irreconcilable versions' a.log || fail "A: the refusal does not name the conflict" a.log
grep -q 'mcpplibs.os = { version = "[0-9.]*", override = true }' a.log || fail "A: the refusal does not name the override" a.log

# B
write_app '{ version = "0.4.4", override = true }'
"$MCPP" build --configure-only > b.log 2>&1 || fail "B: the override was refused" b.log
grep -qE 'Overrode +mcpplibs\.os 0\.4\.1 \(requested by [^)]*anchor[^)]*\) -> 0\.4\.4 \(\[dependencies\] of app\)' b.log \
    || fail "B: the replaced request is not named" b.log
[ "$(grep -c 'Overrode' b.log)" = 1 ] || fail "B: the replaced request is named more than once" b.log
[ "$(locked_os)" = "0.4.4 override" ] || fail "B: mcpp.lock does not record the override ($(locked_os))" mcpp.lock

# C
write_app '{ version = "0.4.4", override = true }'
"$MCPP" build --configure-only > c2.log 2>&1 || fail "C: the override was refused" c2.log
sed -i.bak 's/^override = true$//' mcpp.lock
"$MCPP" build --configure-only --locked > c3.log 2>&1 && fail "C: --locked accepted a lock without the override" c3.log
grep -q 'mcpplibs.os 0.4.4 -> 0.4.4 (override)' c3.log || fail "C: the drift does not name the override" c3.log

# D
sed -i.bak 's/^mcpplibs.os = "0.4.1"$/mcpplibs.os = { version = "0.4.1", override = true }/' "$TMP/anchor/mcpp.toml"
"$MCPP" build --configure-only > d.log 2>&1 || fail "D: the build failed" d.log
grep -q "\`override\` on 'mcpplibs.os' is ignored" d.log || fail "D: a library's override is not reported" d.log
sed -i.bak 's/^mcpplibs.os = { version = "0.4.1", override = true }$/mcpplibs.os = "0.4.1"/' "$TMP/anchor/mcpp.toml"

echo "PASS: 1071 the root overrides a dependency for the whole graph"
