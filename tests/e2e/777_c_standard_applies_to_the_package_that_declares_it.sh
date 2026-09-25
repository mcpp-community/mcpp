#!/usr/bin/env bash
# requires: gcc
# mcpp#695: `[build] c_standard` applies to the C units of the package that
# declares it, and a consumer's value never reaches a dependency.
#
# Before the fix the root's value was on the file-level `$cflags` line of every
# C compile in the graph, so a dependency's own declaration was parsed, hashed
# into its cache key and never applied. Each C file below states, in the
# preprocessor, the standard its package declares; a unit compiled at any other
# standard fails with `#error` and names what it received.
#
#   app    declares c99    its C unit must see __STDC_VERSION__ 199901L
#   cdep   declares gnu11  201112L, and no __STRICT_ANSI__ (a GNU dialect)
#   plain  declares none   201112L with __STRICT_ANSI__: the default, c11
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

fail() { [ -n "$2" ] && cat "$2"; echo "FAIL: $1"; exit 1; }

mkdir -p app/src cdep/src plain/src

cat > cdep/mcpp.toml <<'EOF'
[package]
name    = "cdep"
version = "0.1.0"

[build]
c_standard = "gnu11"

[targets.cdep]
kind = "lib"
EOF
cat > cdep/src/cdep.c <<'EOF'
#if __STDC_VERSION__ != 201112L
#error "cdep declares gnu11 and was compiled at another standard"
#endif
#ifdef __STRICT_ANSI__
#error "cdep declares gnu11 and was compiled at a strict standard"
#endif
int cdep_value(void) { return 11; }
EOF

cat > plain/mcpp.toml <<'EOF'
[package]
name    = "plain"
version = "0.1.0"

[targets.plain]
kind = "lib"
EOF
cat > plain/src/plain.c <<'EOF'
#if __STDC_VERSION__ != 201112L || !defined(__STRICT_ANSI__)
#error "plain declares no C standard and was not compiled at the default, c11"
#endif
int plain_value(void) { return 1; }
EOF

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[build]
c_standard = "c99"

[dependencies]
cdep  = { path = "../cdep" }
plain = { path = "../plain" }
EOF
cat > app/src/root.c <<'EOF'
#if __STDC_VERSION__ != 199901L
#error "app declares c99 and was compiled at another standard"
#endif
int root_value(void) { return 99; }
EOF
cat > app/src/main.cpp <<'EOF'
extern "C" int cdep_value();
extern "C" int plain_value();
extern "C" int root_value();
int main() { return cdep_value() == 11 && plain_value() == 1 && root_value() == 99 ? 0 : 1; }
EOF

cd app
"$MCPP" build > b.log 2>&1 || fail "each package's C units must compile at its own standard" b.log
"$MCPP" run > r.log 2>&1 || fail "the program did not run" r.log
echo "  ok: app at c99, cdep at gnu11, plain at c11"

# The compile database records the same per-unit standard the build used.
cdb=$(find . -name compile_commands.json | head -1)
[ -n "$cdb" ] || fail "no compile_commands.json" b.log
grep -q -- '-std=gnu11' "$cdb" || fail "the database does not carry cdep's standard" "$cdb"
grep -q -- '-std=c99' "$cdb" || fail "the database does not carry app's standard" "$cdb"
echo "  ok: compile_commands.json carries the same standards"

echo "PASS: c_standard applies to the package that declares it"
