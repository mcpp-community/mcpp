#!/usr/bin/env bash
# requires: unix-shell
# 851 -- a member that two members of different configurations use is
# compiled once in each configuration, and never served across them.
#
# A workspace is one graph per configuration (workspace design 2026-09-29
# §2, §3): a node is a (package, configuration) pair. e2e 833 builds a member
# of another standard beside the others, but that member uses no shared one;
# this states what happens to a member both configurations reach, and what
# does not split a package into two nodes.
#
# Criteria:
#   A. `lib` is used by `app23` (C++23, with lib's feature `extra`) and by
#      `app26` (`standard = "c++26"`, without it). `--workspace` gives two
#      build directories; each compiles lib's object once; each program
#      prints its own configuration's `__cplusplus` and feature.
#   B. In one configuration, a consumer's own `cxxflags` stay out of lib's
#      compile, and lib is compiled once for its two consumers there, with
#      the union of the features they ask for (docs/06, "Cargo-style,
#      additive").
#   C. `-p app23b` plans app23b's closure alone: lib is compiled without the
#      feature that only app23 asks for, in the same build directory.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
cd "$TMP"

mkdir -p lib/src app23/src app23b/src app26/src
cat > mcpp.toml <<'EOF'
[workspace]
members = ["lib", "app23", "app26"]
EOF
cat > lib/mcpp.toml <<'EOF'
[package]
name    = "lib"
version = "0.1.0"

[features]
extra = []

[targets.lib]
kind = "lib"
EOF
cat > lib/src/lib.cppm <<'EOF'
export module shared_lib;
export long std_v() { return __cplusplus; }
export int extra_v() {
#ifdef MCPP_FEATURE_EXTRA
    return 1;
#else
    return 0;
#endif
}
export int consumer_flag_v() {
#ifdef CONSUMER_ONLY_FLAG
    return 1;
#else
    return 0;
#endif
}
EOF
for a in app23 app23b app26; do
    printf '#include <cstdio>\nimport shared_lib;\nint main() { std::printf("%%ld %%d %%d\\n", std_v(), extra_v(), consumer_flag_v()); }\n' \
        > $a/src/main.cpp
done
cat > app23/mcpp.toml <<'EOF'
[package]
name    = "app23"
version = "0.1.0"

[dependencies]
lib = { path = "../lib", features = ["extra"] }

[targets.app23]
kind = "bin"
main = "src/main.cpp"
EOF
cat > app26/mcpp.toml <<'EOF'
[package]
name     = "app26"
version  = "0.1.0"
standard = "c++26"

[dependencies]
lib = { path = "../lib" }

[targets.app26]
kind = "bin"
main = "src/main.cpp"
EOF
cat > app23b/mcpp.toml <<'EOF'
[package]
name    = "app23b"
version = "0.1.0"

[dependencies]
lib = { path = "../lib" }

[build]
cxxflags = ["-DCONSUMER_ONLY_FLAG=1"]

[targets.app23b]
kind = "bin"
main = "src/main.cpp"
EOF

run() { "$(find target -path "*/bin/$1/$1" -type f | head -1)"; }
lib_objects() { awk -F'\t' '$4 ~ /(^|\/)lib\/src\/lib\.m\.o$/' "$1/.ninja_log" | wc -l | tr -d ' '; }

# ── A ──────────────────────────────────────────────────────────────────────
"$MCPP" build --workspace > a.log 2>&1 || fail "A: the workspace did not build" a.log
dirs=$(find target -name build.ninja -not -path '*/.build-mcpp/*' -exec dirname {} \; | sort)
[ "$(echo "$dirs" | wc -l | tr -d ' ')" = 2 ] || fail "A: expected two build directories, got: $dirs" a.log
for d in $dirs; do
    [ "$(lib_objects "$d")" = 1 ] || fail "A: lib's object was compiled $(lib_objects "$d") times in $d" "$d/.ninja_log"
done
read -r v23 x23 _ <<< "$(run app23)"
read -r v26 x26 _ <<< "$(run app26)"
[ "$x23" = 1 ] && [ "$x26" = 0 ] || fail "A: the features crossed configurations (app23 $x23, app26 $x26)" a.log
[ -n "$v23" ] && [ -n "$v26" ] && [ "$v26" -gt "$v23" ] \
    || fail "A: app26 does not link the lib compiled for its own standard ($v23 against $v26)" a.log
echo "ok: A, lib is compiled once per configuration and each program links its own ($v23, $v26)"

# ── B ──────────────────────────────────────────────────────────────────────
printf '[workspace]\nmembers = ["lib", "app23", "app23b", "app26"]\n' > mcpp.toml
"$MCPP" build --workspace > b.log 2>&1 || fail "B: the workspace did not build" b.log
d23=$(dirname "$(find target -path '*/bin/app23/app23' -type f | head -1)"); d23=${d23%/bin/app23}
[ "$(lib_objects "$d23")" = 1 ] || fail "B: lib's object was compiled more than once for its two consumers" "$d23/.ninja_log"
read -r _ xb fb <<< "$(run app23b)"
[ "$fb" = 0 ] || fail "B: a consumer's own cxxflags reached lib's compile" b.log
[ "$xb" = 1 ] || fail "B: lib was not compiled with the union of its consumers' features" b.log
echo "ok: B, one configuration compiles lib once, with its own flags and the union of the features"

# ── C ──────────────────────────────────────────────────────────────────────
"$MCPP" build -p app23b > c.log 2>&1 || fail "C: -p app23b did not build" c.log
read -r _ xc _ <<< "$(run app23b)"
[ "$xc" = 0 ] || fail "C: lib kept a feature app23b's closure does not ask for" c.log
[ "$(find target -name build.ninja -not -path '*/.build-mcpp/*' | wc -l | tr -d ' ')" = 2 ] || fail "C: -p made a third build directory" c.log
echo "ok: C, -p plans its own closure's features in the same build directory"

echo "PASS: 851_a_shared_member_is_compiled_once_per_configuration"
