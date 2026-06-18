#!/usr/bin/env bash
# requires: gcc
# Per-target entry-scoped flags (issue #131): two bin targets in ONE package
# each carry their own `defines`/`cxxflags`, applied ONLY to that target's
# exclusive entry. A shared source compiled once must see NEITHER target's
# macro (compile-once stays neutral).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
mkdir -p proj/src
cd proj

# Shared source — globbed, compiled once, linked into both binaries.
# It must NOT see either per-target macro.
cat > src/common.cpp <<'EOF'
#if defined(ROLE_SERVER) || defined(ROLE_CLIENT)
#error "per-target macro leaked into a shared compile unit"
#endif
extern "C" int shared_answer(void) { return 7; }
EOF

# server entry: requires its own macro.
cat > src/server.cpp <<'EOF'
#ifndef ROLE_SERVER
#error "server target did not receive its per-target define"
#endif
extern "C" int shared_answer(void);
int main() { return (shared_answer() == 7 && ROLE_SERVER == 1) ? 0 : 1; }
EOF

# client entry: requires a different macro + a per-target cxxflag define.
cat > src/client.cpp <<'EOF'
#ifndef ROLE_CLIENT
#error "client target did not receive its per-target define"
#endif
#ifndef VIA_CXXFLAG
#error "client target did not receive its per-target cxxflag"
#endif
extern "C" int shared_answer(void);
int main() { return (shared_answer() == 7 && ROLE_CLIENT == 2) ? 0 : 1; }
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "proj"
version = "0.1.0"

[targets.server]
kind    = "bin"
main    = "src/server.cpp"
defines = ["ROLE_SERVER=1"]

[targets.client]
kind     = "bin"
main     = "src/client.cpp"
defines  = ["ROLE_CLIENT=2"]
cxxflags = ["-DVIA_CXXFLAG"]
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; echo "build failed"; exit 1; }

ninja_file="$(find target -name build.ninja | head -1)"
[[ -n "$ninja_file" ]] || { echo "no build.ninja"; exit 1; }
grep -q -- "-DROLE_SERVER=1"  "$ninja_file" || { echo "server define missing from ninja"; exit 1; }
grep -q -- "-DROLE_CLIENT=2"  "$ninja_file" || { echo "client define missing from ninja"; exit 1; }
grep -q -- "-DVIA_CXXFLAG"    "$ninja_file" || { echo "client cxxflag missing from ninja"; exit 1; }

# Both binaries built and each behaves per its own macro.
"$MCPP" run server > /dev/null 2>&1 || { echo "server run failed (wrong/missing macro)"; exit 1; }
"$MCPP" run client > /dev/null 2>&1 || { echo "client run failed (wrong/missing macro)"; exit 1; }

echo "OK"
