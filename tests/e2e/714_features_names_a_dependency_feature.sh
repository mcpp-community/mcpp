#!/usr/bin/env bash
# requires: gcc
# 714 -- `--features <dependency>/<feature>` opens a feature of a dependency, as
# the same token does inside `[features]` (#649 E8).
#
# The command line accepted root features only. With no `[features]` table the
# token became the macro `-DMCPP_FEATURE_SPIKE_FW_INSTALLER` and opened nothing;
# with the table it was reported as an undeclared feature of the root. A plain
# name without a `[features]` table keeps its documented meaning, a macro.
#
# Readings: without a `[features]` table the dependency's tool is built and no
# macro is made of the token; with the table the build is clean under
# `--strict`; a token naming no dependency is refused under `--strict`; and
# `mcpp why deps --features` reports the graph of the feature build.
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

mkdir -p fw/src fw/tool/src app/src
cat > fw/mcpp.toml <<'EOF'
[package]
name      = "fw"
namespace = "spike"
version   = "0.1.0"

[targets.fw]
kind = "lib"

[build]
sources = ["src/*.cpp"]

[features]
installer = []

[feature-deps.installer]
spike.fw-installer = { path = "tool", tools = ["fw-installer"], reexport = true }
EOF
echo 'int fw_answer() { return 42; }' > fw/src/fw.cpp
cat > fw/tool/mcpp.toml <<'EOF'
[package]
name      = "fw-installer"
namespace = "spike"
version   = "0.1.0"

[targets.fw-installer]
kind = "bin"
main = "src/main.cpp"
EOF
echo 'int main() { return 0; }' > fw/tool/src/main.cpp

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies]
spike.fw = { path = "../fw" }
EOF
cat > app/src/main.cpp <<'EOF'
int fw_answer();
int main() { return fw_answer() == 42 ? 0 : 1; }
EOF
cat > app/build.mcpp <<'EOF'
import std;
import mcpp;
int main() {
    const char* a = mcpp::dep_bin("spike.fw-installer", "fw-installer");
    mcpp::warning((std::string("DEPBIN tool=[") + (a ? a : "") + "]").c_str());
    return 0;
}
EOF
cd "$TMP/app"

# ── 1. no [features] table: the dependency's feature opens, no macro ─────
"$MCPP" build --features spike.fw/installer > b1.log 2>&1 || fail "the dependency feature build failed" b1.log
grep -q 'DEPBIN tool=\[[^]]' b1.log || fail "--features spike.fw/installer did not open the feature" b1.log
ninja_file=$(find target -name build.ninja | head -1)
grep -q 'MCPP_FEATURE_SPIKE_FW_INSTALLER' "$ninja_file" \
    && fail "the dependency token became a macro of the root" "$ninja_file"

# A plain undeclared name without a [features] table is documented as a macro.
rm -rf target
"$MCPP" build --strict --features nothing-here > b2.log 2>&1 \
    || fail "a plain name without a [features] table was refused" b2.log

# ── 2. a token naming no dependency is refused under --strict ───────────
if "$MCPP" build --strict --features nope/x > b3.log 2>&1; then
    fail "--features nope/x was accepted under --strict" b3.log
fi
grep -q "nope" b3.log || fail "the refusal does not name the token" b3.log

# ── 3. with a [features] table the token is not reported as undeclared ──
cat >> mcpp.toml <<'EOF'

[features]
windows-installer = []
EOF
rm -rf target
"$MCPP" build --strict --features spike.fw/installer > b4.log 2>&1 \
    || fail "the dependency token was refused beside a [features] table" b4.log
grep -q 'DEPBIN tool=\[[^]]' b4.log || fail "the feature did not open beside a [features] table" b4.log

# ── 4. `why deps` reads the graph of the feature build ──────────────────
"$MCPP" why deps --features spike.fw/installer > w1.log 2>&1 || fail "why deps --features failed" w1.log
grep -q 'spike.fw-installer' w1.log || fail "why deps does not show the feature's dependency" w1.log

echo "PASS: 714 --features opens a dependency's feature"
