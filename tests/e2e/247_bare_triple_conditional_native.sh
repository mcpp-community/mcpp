#!/usr/bin/env bash
# requires:
# (no capability: the probe greps the MACRO NAME, never the `-D` prefix — MSVC
#  spells that `/D`, and the prefix was the only thing that tied this test to
#  one compiler family. The predicate it checks is compiler-independent, so the
#  test has to run everywhere the predicate does.)
# 247_bare_triple_conditional_native.sh — `[target.'<triple>'.build]` applies to
# a NATIVE build, not only to one with an explicit `--target`.
#
# It did not, and the failure shape is the dangerous one: CI passes `--target`
# and is green, the developer's plain `mcpp build` silently drops the section,
# and whatever it carried (a `-L`, a define, a source) goes missing somewhere
# far from the manifest. `cfg(linux)` matched the same build all along, so the
# two spellings of one statement disagreed.
#
# The probe asserts BOTH spellings under BOTH invocations. Asserting only the
# native case would pass against a build that applies every section always.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# The host triple, asked of mcpp rather than guessed.
#
# The first version pattern-matched `mcpp self env` for
# `<arch>-<os>-<env>` and fell back to `x86_64-linux-gnu`. macOS's canonical
# triple is `aarch64-macos` — TWO segments, no env — so the match failed, the
# fallback was used, and the test then asserted that a *Linux* section applied
# to a macOS build. It reported "the bare triple was inert" against a product
# that was behaving correctly.
#
# `target/<triple>/` is mcpp's own answer to the same question, so take it from
# there: build once with nothing conditional, read the directory name, then
# write the real manifest.
mkdir -p probe/src
echo 'int main() { return 0; }' > probe/src/main.cpp
cat > probe/mcpp.toml <<'EOF'
[package]
name    = "probe"
version = "0.1.0"
[targets.probe]
kind = "bin"
main = "src/main.cpp"
EOF
( cd probe && "$MCPP" build > discover.log 2>&1 ) \
    || { cat probe/discover.log; echo "discovery build failed"; exit 1; }
host_triple="$(ls probe/target | head -1)"
[[ -n "$host_triple" ]] || { ls -R probe/target; echo "no target/<triple> dir"; exit 1; }
echo "host triple: $host_triple"

cat > probe/mcpp.toml <<EOF
[package]
name    = "probe"
version = "0.1.0"
[targets.probe]
kind = "bin"
main = "src/main.cpp"

[target.'$host_triple'.build]
cxxflags = ["-DMCPP_BARE_TRIPLE=1"]

[target.'cfg(arch = "$(echo "$host_triple" | cut -d- -f1)")'.build]
cxxflags = ["-DMCPP_CFG_ALIAS=1"]
EOF

count() {   # $1 = macro name
    # The NAME, not `-D<name>`: the prefix is dialect-specific (`/D` on MSVC),
    # and what this test is about is whether the section applied at all.
    local nj; nj="$(find target -name build.ninja | head -1)"
    grep -c -- "$1" "$nj" || true
}

cd probe

# ── native: no --target ────────────────────────────────────────────────
rm -rf target
"$MCPP" build > native.log 2>&1 || { cat native.log; echo "native build failed"; exit 1; }
bare="$(count MCPP_BARE_TRIPLE)"
alias_="$(count MCPP_CFG_ALIAS)"
[[ "$alias_" -gt 0 ]] || { echo "cfg() did not apply on a native build"; exit 1; }
[[ "$bare"   -gt 0 ]] || {
    echo "FAIL: [target.'$host_triple'.build] was inert on a native build"
    echo "      (cfg() applied $alias_ times, the bare triple $bare)"
    exit 1; }

# ── explicit --target: unchanged ───────────────────────────────────────
rm -rf target
"$MCPP" build --target "$host_triple" > explicit.log 2>&1 \
    || { cat explicit.log; echo "explicit build failed"; exit 1; }
[[ "$(count MCPP_BARE_TRIPLE)" -gt 0 ]] || { echo "bare triple inert with --target"; exit 1; }
[[ "$(count MCPP_CFG_ALIAS)"   -gt 0 ]] || { echo "cfg() inert with --target"; exit 1; }

echo "PASS: a bare-triple conditional applies to native and explicit builds alike"
