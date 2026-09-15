#!/usr/bin/env bash
# requires: gcc
# 710 -- a feature forward is validated against every dependency table of its
# manifest (#647 E4.1).
#
# `injectForwards` applies a `[features]` forward to `[build-dependencies]`
# edges, while the validator used to look the key up in `[dependencies]` and
# `[dev-dependencies]` only. A two-level forward along build-dependency edges
# therefore opened the leaf's feature (its tool was built) and printed
# "not declared" for each level in the same run, and `--strict` refused the
# build whose forward had worked. A key declared only for another row is also
# declared: on this row the forward reaches no edge.
#
# Readings: the two-level forward builds the tool under `--strict` with no
# forward warning; a forward to a dependency declared for another row builds
# under `--strict`; a forward to a key no table declares still warns, and
# `--strict` refuses it.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

export MCPP_HOME="$TMP/mcpphome"
mkdir -p "$MCPP_HOME"
if [ -d "$HOME/.mcpp/registry" ]; then
    ln -s "$HOME/.mcpp/registry" "$MCPP_HOME/registry"
fi

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p kt/src leaf/src rules/src fw/src app/src win/src
cat > kt/mcpp.toml <<'EOF'
[package]
name      = "kt"
namespace = "spike"
version   = "0.1.0"

[targets.kt]
kind = "bin"
main = "src/main.cpp"
EOF
echo 'int main() { return 0; }' > kt/src/main.cpp

cat > leaf/mcpp.toml <<'EOF'
[package]
name      = "leaf"
namespace = "spike"
version   = "0.1.0"

[targets.leaf]
kind = "lib"

[build]
sources = ["src/*.cpp"]

[features]
kt = []

[feature-deps.kt]
spike.kt = { path = "../kt", tools = ["kt"] }
EOF
echo 'int leaf_answer() { return 1; }' > leaf/src/leaf.cpp

cat > rules/mcpp.toml <<'EOF'
[package]
name      = "rules"
namespace = "spike"
version   = "0.1.0"

[targets.rules]
kind = "lib"

[build]
sources = ["src/*.cpp"]

[build-dependencies]
spike.leaf = { path = "../leaf" }

[features]
kotlin = ["spike.leaf/kt"]
EOF
echo 'int rules_answer() { return 1; }' > rules/src/rules.cpp

cat > win/mcpp.toml <<'EOF'
[package]
name      = "win"
namespace = "spike"
version   = "0.1.0"

[targets.win]
kind = "lib"

[build]
sources = ["src/*.cpp"]

[features]
x = []
EOF
echo 'int win_answer() { return 1; }' > win/src/win.cpp

cat > fw/mcpp.toml <<'EOF'
[package]
name      = "fw"
namespace = "spike"
version   = "0.1.0"

[targets.fw]
kind = "lib"

[build]
sources = ["src/*.cpp"]

[build-dependencies]
spike.rules = { path = "../rules" }

[target.'cfg(os = "windows")'.dependencies]
spike.win = { path = "../win" }

[features]
kotlin = ["spike.rules/kotlin", "spike.win/x"]
EOF
echo 'int fw_answer() { return 42; }' > fw/src/fw.cpp

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies]
spike.fw = { path = "../fw", features = ["kotlin"] }
EOF
cat > app/src/main.cpp <<'EOF'
int fw_answer();
int main() { return fw_answer() == 42 ? 0 : 1; }
EOF

# ── 1. two levels along [build-dependencies], strict, no forward warning ──
cd "$TMP/app"
"$MCPP" build --strict > b1.log 2>&1 || fail "the two-level build-dependency forward was refused under --strict" b1.log
grep -q "forwards to dependency" b1.log \
    && fail "a forward along a build-dependency edge was reported as undeclared" b1.log
grep -q "Building host tool kt" b1.log \
    || fail "the forwarded feature did not reach the leaf (its tool was not built)" b1.log

# ── 2. a forward to a dependency declared only for another row ───────────
# `spike.win` is declared for Windows only (this script runs where gcc is the
# host compiler, which is never a Windows row). The forward names it, reaches
# no edge here, and must be accepted: the manifest is portable, not wrong.
grep -q "spike.win" b1.log && fail "a row-declared dependency's forward was reported" b1.log

# ── 3. a key no table declares is still named, and --strict refuses it ───
sed -i.bak 's|kotlin = \["spike.rules/kotlin", "spike.win/x"\]|kotlin = ["spike.rules/kotlin", "spike.nowhere/x"]|' "$TMP/fw/mcpp.toml"
rm -rf target
"$MCPP" build > b3.log 2>&1 || fail "an undeclared forward failed a non-strict build" b3.log
grep -q "forwards to dependency 'spike.nowhere'" b3.log \
    || fail "a forward to an undeclared key was not named" b3.log
if "$MCPP" build --strict > b4.log 2>&1; then
    fail "--strict accepted a forward to a key no table declares" b4.log
fi
grep -q "spike.nowhere" b4.log || fail "the --strict refusal does not name the key" b4.log

echo "PASS: 710 a forward is validated against every dependency table"
