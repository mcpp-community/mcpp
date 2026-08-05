#!/usr/bin/env bash
# requires: gcc
# 189_host_module_rules.sh — reusable build rules distributed as ORDINARY mcpp
# packages: `host-module = true` makes a dependency's module importable from the
# consumer's build.mcpp.
#
# This is the answer to "a rule should be written once, not copy-pasted into
# every package's build.mcpp" — without introducing a second language. xmake
# reaches for Lua rules and Bazel for Starlark; mcpp's whole premise is that
# the build is written in C++, so a rule is a C++ module in a versioned package
# and rides the package manager that already exists.
#
# The load-bearing implementation detail this test pins: the rule module is
# compiled in the SAME command as build.mcpp, with the same flags. A BMI is
# only usable by a compile that agrees with it on standard, dialect and
# compiler identity — building it separately would leave that to chance, and
# disagreement surfaces as `module X CRC mismatch` rather than a clear error.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ── the rule package ────────────────────────────────────────────────────────
# An ordinary mcpp library package whose lib root is the rule module.
mkdir -p rules/src
cat > rules/mcpp.toml <<'EOF'
[package]
name    = "rules"
version = "0.1.0"

[targets.rules]
kind = "lib"
EOF
cat > rules/src/rules.cppm <<'EOF'
module;
#include <cstdio>
export module rules;
export namespace rules {
// A rule: emits the directives its users would otherwise hand-write. Written
// ONCE here instead of copy-pasted into every consumer's build.mcpp.
inline void banner(const char* macro) {
    std::printf("mcpp:cxxflag=-D%s=1\n", macro);
}
inline void define_answer(int n) {
    std::printf("mcpp:cxxflag=-DRULE_ANSWER=%d\n", n);
}
}
EOF

# ── the consumer ────────────────────────────────────────────────────────────
mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
rules = { path = "../rules", host-module = true }
EOF
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
#ifndef RULE_ANSWER
#error "the rule package's directive never reached the compile"
#endif
#ifndef RULE_BANNER_OK
#error "the second rule directive never reached the compile"
#endif
int main() { std::printf("ANSWER=%d\n", RULE_ANSWER); }
EOF
cat > app/build.mcpp <<'EOF'
import mcpp;
import rules;      // a DEPENDENCY's module, compiled for the host
int main() {
    rules::banner("RULE_BANNER_OK");
    rules::define_answer(42);
}
EOF

cd app
"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "FAIL: build with a host rule module failed"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^ANSWER=' | tail -1)"
[[ "$out" == "ANSWER=42" ]] || {
    echo "FAIL: the rule package's directives did not reach the build: $out"; exit 1; }

# Editing the RULE must re-run build.mcpp: the rule's content is part of what
# the helper was compiled from, so a cached run would silently keep the old
# behaviour — the exact failure the declared-input cache exists to prevent.
sed -i 's/define_answer(int n)/define_answer(int n_)/; s/RULE_ANSWER=%d\\n", n)/RULE_ANSWER=%d\\n", n_ + 1)/' ../rules/src/rules.cppm
touch src/main.cpp
"$MCPP" build > b2.log 2>&1 || { cat b2.log; echo "FAIL: rebuild after editing the rule failed"; exit 1; }
grep -q "build.mcpp running" b2.log || {
    cat b2.log; echo "FAIL: editing the rule module did not re-run build.mcpp"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^ANSWER=' | tail -1)"
[[ "$out" == "ANSWER=43" ]] || {
    echo "FAIL: the edited rule did not take effect: $out"; exit 1; }

# ── a rule that uses the API it exists to wrap ──────────────────────────────
# The two properties below are what make the feature usable for a REAL rule
# rather than one that hand-prints directives, and 2026.8.5.1 had neither:
#
#   `import std;`   the rule was compiled before the std module was built, so
#                   it failed with `module 'std' not found`. The scan for
#                   `import std` read build.mcpp only, so a rule that needed it
#                   did not even trigger the build.
#   `import mcpp;`  registering the host module never removed the package from
#                   the consumer's ORDINARY graph, so the same .cppm was also
#                   compiled as a normal library — where `mcpp` does not exist.
cd "$TMP"
mv rules/src/rules.cppm rules/src/rules.cppm.bak
cat > rules/src/rules.cppm <<'EOF'
export module rules;
import std;    // must be usable: a rule is a normal C++23 module
import mcpp;   // must be usable: the typed wrapper is the whole point

export namespace rules {
inline void banner(const char* macro) { mcpp::define(macro); }
inline void define_answer(int n) {
    mcpp::cxxflag(std::format("-DRULE_ANSWER={}", n).c_str());
}
}
EOF
cd app && rm -rf target
"$MCPP" build > b4.log 2>&1 || {
    cat b4.log
    echo "FAIL: a rule using import std + import mcpp did not build"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^ANSWER=' | tail -1)"
[[ "$out" == "ANSWER=42" ]] || {
    echo "FAIL: the std/mcpp-based rule's directives did not land: $out"; exit 1; }

# The rule package must NOT be compiled into the consumer's target. It is
# build-time-only; an object under obj/rules/ means it was also treated as an
# ordinary library, which is what made `import mcpp;` fail in the first place.
if find target -path '*obj/rules/*' -name '*.o' | grep -q .; then
    find target -path '*obj/rules/*' -name '*.o'
    echo "FAIL: the host-module package was also built as a normal library"; exit 1
fi

cd "$TMP"
rm -f rules/src/rules.cppm
mv rules/src/rules.cppm.bak rules/src/rules.cppm

# A missing lib root must say so, not fail three edges later.
cd "$TMP"
mv rules/src/rules.cppm rules/src/elsewhere.cppm
cd app && rm -rf target
if "$MCPP" build > b3.log 2>&1; then
    cat b3.log; echo "FAIL: a host module with no interface unit was accepted"; exit 1
fi
grep -q "no interface unit" b3.log || {
    cat b3.log; echo "FAIL: unhelpful diagnostic for a missing rule interface"; exit 1; }

echo "OK"
