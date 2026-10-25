#!/usr/bin/env bash
# requires:
# 857 -- a library that several members' build programs use is compiled once
# for all of them, and a library of a path dependency is never kept globally.
#
# #748 (B1), and mcpp#797. Every member of a workspace has a build program,
# and each uses the same library, which imports the engine interface. The
# programs of one build that share a host toolchain and a C++ standard are the
# members of one build programs' graph, below the workspace root, so the
# library is compiled once for them; a program of another standard is in
# another graph and never takes the others' BMI.
#
# The library is a path dependency, so its sources can change without its name
# and version changing, and it is kept where the project keeps everything that
# is its own: under the workspace's `target/`. The engine interface is
# identical for every project and is kept in the global cache.
#
# Criteria:
#   A. `m1` and `m2` agree on every flag: their programs are in one graph,
#      which compiles the library once.
#   B. `m3` states another standard. Its program is in another graph, with a
#      compile of the library of its own, at its own standard.
#   C. The graphs are under the workspace root's `target/.build-mcpp/host/`,
#      and not under any member's own `target/`.
#   D. With `--cache global`, nothing of the path dependency is written below
#      the global cache root; the engine interface is.
set -e
# The project-root copy of the compile database is opt-in since
# 2026.10.26.1 ([build] compile_commands); this test reads it.
export MCPP_COMPILE_COMMANDS=compile_commands.json
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/ws"
cd "$TMP/ws"

cat > mcpp.toml <<'EOF'
[workspace]
members = ["m1", "m2", "m3"]
EOF

# The rule package is a path dependency beside the members and is not one of
# them: it is built only for the build programs that import it.
mkdir -p rules/src
cat > rules/mcpp.toml <<'EOF'
[package]
name    = "rule857"
version = "0.1.0"

[targets.rule857]
kind = "lib"
EOF
cat > rules/src/rule857.cppm <<'EOF'
export module t857.rules;
import mcpp;
export int rule_value() { return 7; }
export void apply(const char* define) { mcpp::define(define); }
EOF

for m in m1 m2 m3; do
    mkdir -p $m/src
    {
        echo '[package]'
        echo "name    = \"$m\""
        echo 'version = "0.1.0"'
        [ "$m" = m3 ] && echo 'standard = "c++20"'
        echo
        echo '[build-dependencies]'
        echo 'rule857 = { path = "../rules", host-module = true }'
        echo
        echo "[targets.$m]"
        echo 'kind = "bin"'
        echo 'main = "src/main.cpp"'
    } > $m/mcpp.toml
    printf 'int main() { return 0; }\n' > $m/src/main.cpp
    cat > $m/build.mcpp <<EOF
import mcpp;
import t857.rules;
int main() {
    if (rule_value() != 7) return 1;
    apply("T857_${m}=1");
    return 0;
}
EOF
done

MCPP_VERBOSE=1 "$MCPP" build --workspace --cache global -j 4 > a.log 2>&1 \
    || fail "the workspace did not build" a.log
for m in m1 m2 m3; do
    grep -qE "^ *build\.mcpp $m .* ran " a.log || fail "the program of $m did not run" a.log
done

HOST=target/.build-mcpp/host
# The compiles of the library: one entry per graph in that graph's database.
entries() { cat $HOST/*/compile_commands.json 2>/dev/null | grep -cE '"file": ".*rules[/\\]+src[/\\]+rule857\.cppm"' || true; }

# A and B. Three programs, two standards: two graphs, two compiles.
graphs=$(ls $HOST/*/programs.json 2>/dev/null | wc -l | tr -d ' ')
[ "$graphs" = 2 ] || fail "A/B: $graphs build programs' graphs for three programs of two standards (want 2)" a.log
[ "$(entries)" = 2 ] || fail "A/B: the library was compiled $(entries) times for three programs of two standards (want 2)" a.log
for g in $HOST/*/programs.json; do
    if grep -q '"m3-' "$g"; then
        grep -q '"m1-' "$g" && fail "B: m3's program shares a graph with m1's" "$g"
    else
        { grep -q '"m1-' "$g" && grep -q '"m2-' "$g"; } || fail "A: m1 and m2 are not in one graph" "$g"
    fi
done
stds=$(cat $HOST/*/compile_commands.json | grep -o -- '-std=c++2[03]' | sort -u | wc -l | tr -d ' ')
[ "$stds" = 2 ] || fail "B: the two graphs compile at $stds distinct standards (want 2)"

# C. One place for the workspace, below the workspace root; none per member.
for m in m1 m2 m3; do
    [ ! -d "$m/target/.build-mcpp/host" ] \
        || fail "C: $m keeps its own build programs' graph beside the workspace's"
done

# D. The path dependency's module is not below the global cache root; the
# engine's interface is.
CACHE_ROOT="$("$MCPP" cache dir | head -1)"
if [ -d "$CACHE_ROOT/pkg" ] && grep -rl 't857.rules' "$CACHE_ROOT/pkg" --include=entry.json 2>/dev/null | grep -q .; then
    fail "D: a module of a path dependency was written below the global cache root"
fi
[ -n "$(find "$CACHE_ROOT/pkg/_engine" -name entry.json 2>/dev/null)" ] \
    || fail "D: the engine interface is not in the global cache" a.log

echo "PASS: 857_a_host_module_several_members_import_is_compiled_once"
