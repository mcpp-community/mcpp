#!/usr/bin/env bash
# requires: gcc python3 fresh-sandbox
# 939 -- mcpp.lock is written optimistically (design 2026-10-26 §4.1): no lock
# is held while a build resolves its dependencies, so the write at the end
# compares what is there with what the run read when it started.
#
#   A  the first build records the index dependency in mcpp.lock;
#   B  a lock that changes while a build resolves (here: the project's own
#      build program rewrites it, between the read and the write) is not
#      overwritten; the build stops, naming [lockfile-concurrent] and the
#      remedy;
#   C  run again, the build resolves against the lock as it is now and
#      succeeds.
#
# The dependency is served by a project-local index and pre-extracted into the
# private xlings data directory (as 844), so the build touches no network.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/p" && cd "$TMP/p"

mkdir -p local-index/pkgs/c
cat > local-index/pkgs/c/cmdline.lua <<'EOF'
package = {
    spec = "1",
    namespace = "mcpplibs",
    name = "cmdline",
    description = "fixture for the optimistic lock write",
    licenses = {"MIT"},
    type = "package",
    xpm = { linux = { ["1.0.0"] = {
        url = "https://example.invalid/cmdline-1.0.0.tar.gz",
        sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
    } } },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/cmdline.cppm" },
        targets = { ["cmdline"] = { kind = "lib" } },
        deps = {},
    },
}
EOF
XP=".mcpp/.xlings/data/xpkgs/mcpplibs-x-cmdline/1.0.0"
mkdir -p "$XP/src"
cat > "$XP/src/cmdline.cppm" <<'EOF'
export module fixture.cmdline;
export int cmdline_value() { return 7; }
EOF
printf 'ok\n' > "$XP/.mcpp_ok"

mkdir -p src
cat > mcpp.toml <<'EOF'
[package]
name    = "p"
version = "0.1.0"

[indices]
default = { path = "local-index" }

[dependencies.mcpplibs]
cmdline = "1.0.0"
EOF
cat > src/main.cpp <<'EOF'
import fixture.cmdline;
int main() { return cmdline_value() == 7 ? 0 : 1; }
EOF

# A
"$MCPP" build > a.log 2>&1 || fail "A: the first build failed" a.log
grep -q 'cmdline' mcpp.lock || fail "A: mcpp.lock does not record the dependency" mcpp.lock

# B -- the build program runs after the lock is read and before it is written.
cat > build.mcpp <<'EOF'
#include <fstream>
int main() {
    std::ofstream f("mcpp.lock", std::ios::app);
    f << "# written by another run\n";
    return f ? 0 : 1;
}
EOF
set +e
"$MCPP" build > b.log 2>&1; rc=$?
set -e
[[ $rc -ne 0 ]] || fail "B: a lock changed meanwhile was overwritten (the build succeeded)" b.log mcpp.lock
grep -q 'lockfile-concurrent' b.log || fail "B: the refusal does not name lockfile-concurrent" b.log
grep -q 'run the command again' b.log || fail "B: the refusal names no remedy" b.log
grep -q '# written by another run' mcpp.lock || fail "B: the other run's lock was overwritten" mcpp.lock

# C
rm build.mcpp
"$MCPP" build > c.log 2>&1 || fail "C: the build after the concurrent write failed" c.log

echo "PASS: 939 a lock written meanwhile is not overwritten"
