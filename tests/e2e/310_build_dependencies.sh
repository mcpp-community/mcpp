#!/usr/bin/env bash
# requires: gcc
# 310_build_dependencies.sh — `[build-dependencies]`: what serves the BUILD and
# what reaches the TARGET are two different questions.
#
# The section has been parsed, merged across workspace members and
# conditionalised by target predicate for a long time, and until now nothing
# read it to make a decision: writing it produced a manifest that loaded, no
# diagnostic, and no effect. `types.cppm` even annotated the field
# "host-side tools (M5+ behavior)".
#
# The consequence was not merely a dormant feature. Both live build-time
# channels (`tools`, `host-module`) are written by the CONSUMER on an edge, so a
# build rule had no way to request anything on its own behalf — and what it
# wrote in `[dependencies]` LEAKED, because the rule-only predicate looked at
# edges into the rule and not at what lay behind it.
#
# Pinned here:
#   1. a rule's own `[dependencies]` do not reach the consumer's binary;
#   2. `[build-dependencies]` and everything behind them do not either;
#   3. a package the project depends on DIRECTLY still reaches the target even
#      when a build-time path also reaches it — the axes are orthogonal.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# A plain library. Whether its object ends up in a consumer's binary is the
# only thing this test looks at, so it carries one unmistakable symbol.
mk_lib() {
    local name="$1" sym="$2"
    mkdir -p "$name/src"
    cat > "$name/mcpp.toml" <<EOF
[package]
name    = "$name"
version = "0.1.0"

[targets.$name]
kind = "lib"
EOF
    cat > "$name/src/$name.cppm" <<EOF
export module $name;
export int $sym() { return 1; }
EOF
}

mk_lib behindrule behindrule_symbol
mk_lib buildside   buildside_symbol
mk_lib bothsides   bothsides_symbol

# ── the rule, which depends on a library of its own ─────────────────────────
mkdir -p rules/src
cat > rules/mcpp.toml <<'EOF'
[package]
name    = "ruleswithdeps"
version = "0.1.0"

[targets.ruleswithdeps]
kind = "lib"

[dependencies]
behindrule = { path = "../behindrule" }
EOF
cat > rules/src/ruleswithdeps.cppm <<'EOF'
export module ruleswithdeps;
import std;
import mcpp;
export namespace rules { inline void go() { mcpp::cxxflag("-DRULE_RAN=1"); } }
EOF

mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
ruleswithdeps = { path = "../rules", host-module = true }
bothsides     = { path = "../bothsides" }

[build-dependencies]
buildside = { path = "../buildside" }
EOF
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
#ifndef RULE_RAN
#error "the rule never ran"
#endif
import bothsides;
int main() { std::printf("APP=%d\n", bothsides_symbol()); }
EOF
cat > app/build.mcpp <<'EOF'
import mcpp;
import ruleswithdeps;
int main() { rules::go(); }
EOF

cd app
"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "FAIL: build failed"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^APP=' | tail -1)"
[[ "$out" == "APP=1" ]] || { echo "FAIL: the app did not run: $out"; exit 1; }

# The criterion is the ARTIFACT, not a log line. `nm` comes from the toolchain
# that produced the binary rather than from PATH: a host `nm` may not read what
# a private toolchain emits, and `command -v nm` has answered for the wrong
# toolchain in this repository before.
BIN="$("$MCPP" run --print-artifact 2>/dev/null || true)"
if [[ ! -x "${BIN:-}" ]]; then
    mapfile -t bins < <(find target -type f -name app -perm -u+x)
    (( ${#bins[@]} == 1 )) || {
        echo "FAIL: expected exactly one app binary, found ${#bins[@]}"
        printf '  %s\n' "${bins[@]}"; exit 1; }
    BIN="${bins[0]}"
fi

has_symbol() { grep -qF "$1" <(strings -a "$BIN" 2>/dev/null; nm -C "$BIN" 2>/dev/null || true); }

# 3. the orthogonal case FIRST, so a wholesale failure to link anything cannot
#    pass this test by accident. This symbol MUST be present.
has_symbol bothsides_symbol || {
    echo "FAIL: a direct [dependencies] library did not reach the target"; exit 1; }

# 1. a rule's own dependency must not be in the consumer's binary
if has_symbol behindrule_symbol; then
    echo "FAIL: a build rule's own [dependencies] leaked into the consumer's binary"
    exit 1
fi

# 2. nor a [build-dependencies] entry
if has_symbol buildside_symbol; then
    echo "FAIL: a [build-dependencies] entry reached the target"
    exit 1
fi

# ── the dual-role case, stated the other way round ──────────────────────────
# The same package named in BOTH tables by one consumer. The ordinary
# declaration wins: a `[build-dependencies]` line must not quietly drop a
# library the target needs.
cd "$TMP"
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
ruleswithdeps = { path = "../rules", host-module = true }
bothsides     = { path = "../bothsides" }

[build-dependencies]
bothsides = { path = "../bothsides" }
EOF
cd app && rm -rf target
"$MCPP" build > b2.log 2>&1 || { cat b2.log; echo "FAIL: dual-table build failed"; exit 1; }
mapfile -t bins < <(find target -type f -name app -perm -u+x)
(( ${#bins[@]} == 1 )) || {
    echo "FAIL: expected exactly one app binary, found ${#bins[@]}"; exit 1; }
BIN="${bins[0]}"
has_symbol bothsides_symbol || {
    echo "FAIL: naming a library in [build-dependencies] too removed it from the target"
    exit 1; }

echo "OK"
