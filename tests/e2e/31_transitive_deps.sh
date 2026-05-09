#!/usr/bin/env bash
# 31_transitive_deps.sh — transitive dependency walker:
#   * a path-dep that itself declares a path-dep is fully resolved
#     (consumer doesn't need to list the grandchild explicitly)
#   * the grandchild's [build].include_dirs propagate so its headers
#     are visible while compiling its parent.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"

# ── 1. Grandchild: a header-providing C lib whose `[build].include_dirs`
#       is what consumers care about. Plays the role of mbedtls in the
#       llmapi → tinyhttps → mbedtls chain.
mkdir -p "$TMP/grandchild" && cd "$TMP/grandchild"
"$MCPP" new gc > /dev/null
cd gc
mkdir -p include/gc
cat > include/gc/gc.h <<'EOF'
#pragma once
inline int gc_answer(void) { return 42; }
EOF
rm -f src/main.cpp
cat > src/gc.cppm <<'EOF'
export module gc;
EOF
cat > mcpp.toml <<'EOF'
[package]
name    = "gc"
version = "0.1.0"
[build]
include_dirs = ["include"]
[targets.gc]
kind = "lib"
EOF

# ── 2. Child: depends on grandchild via path; its own .cppm pulls
#       <gc/gc.h>, which can only work if gc's include_dirs reach the
#       child's compile rule (transitive include propagation).
mkdir -p "$TMP/child" && cd "$TMP/child"
"$MCPP" new ch > /dev/null
cd ch
rm -f src/main.cpp
cat > src/ch.cppm <<'EOF'
module;
#include <gc/gc.h>
export module ch;
export int ch_answer() { return gc_answer(); }
EOF
cat > mcpp.toml <<EOF
[package]
name    = "ch"
version = "0.1.0"
[targets.ch]
kind = "lib"

[dependencies]
gc = { path = "$TMP/grandchild/gc" }
EOF

# ── 3. Top: depends ONLY on child. Should still pull grandchild
#       transitively without an explicit declaration.
mkdir -p "$TMP/top" && cd "$TMP/top"
"$MCPP" new top > /dev/null
cd top
cat > src/main.cpp <<'EOF'
import std;
import ch;
int main() {
    std::println("answer={}", ch_answer());
    return ch_answer() == 42 ? 0 : 1;
}
EOF
cat > mcpp.toml <<EOF
[package]
name    = "top"
version = "0.1.0"

[dependencies]
ch = { path = "$TMP/child/ch" }
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; echo "transitive build failed"; exit 1; }
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "answer=42" ]] || { echo "unexpected output: $out"; exit 1; }

# ── 4. Same dep referenced through two parallel paths is allowed
#       (no version conflict — same path, same package).
mkdir -p "$TMP/top2" && cd "$TMP/top2"
"$MCPP" new top2 > /dev/null
cd top2
cat > src/main.cpp <<'EOF'
import std;
import ch;
int main() { std::println("answer={}", ch_answer()); return ch_answer() == 42 ? 0 : 1; }
EOF
cat > mcpp.toml <<EOF
[package]
name    = "top2"
version = "0.1.0"

[dependencies]
ch = { path = "$TMP/child/ch" }
gc = { path = "$TMP/grandchild/gc" }
EOF
"$MCPP" build > build-top2.log 2>&1 || { cat build-top2.log; echo "duplicate-but-consistent dep failed"; exit 1; }

echo "OK"
