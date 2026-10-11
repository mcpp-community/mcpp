#!/usr/bin/env bash
# requires: unix-shell
# 861 -- compiling a workspace's build programs at the same time plans exactly
# what compiling them one after another plans.
#
# #748 (B2, criterion D of the issue). The programs are compiled at the same
# time and run in the order they always ran in; each run applies its directives
# to the plan, and the plan is what `build.ninja` states. So the plan, and with
# it the link line, must not depend on which compile finished first.
#
# Each program adds `-Wl,-rpath,/t861/<member>` to the link line, a flag whose
# order the link line keeps and that no build of this test reads. It is an ELF
# and Mach-O linker flag, which `lld-link` on Windows refuses; the property is
# the planner's and holds on every host, so the script runs where the flag is a
# flag (`unix-shell`: Linux and macOS).
#
# The workspace below has three libraries with a build program each, `core`, and
# `cli` and `gui` that depend on it, and an application that links all of them.
# Each program states a link flag, so the order the programs' directives were
# applied in is the order of those flags on the application's link line.
#
# Criteria:
#   A. The build at `-j 4` writes the `build.ninja` a build at `-j 1` writes,
#      byte for byte, and the cached directives of every program are the same.
#   B. The `ran` lines of the two builds are the same lines in the same order.
#   C. The link flags are on the link line in the order the programs ran in,
#      dependencies first, at both job counts.
set -e
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
members = ["core", "cli", "gui", "app"]
EOF
mkdir -p core/src cli/src gui/src app/src
cat > core/mcpp.toml <<'EOF'
[package]
name    = "core"
version = "0.1.0"

[targets.core]
kind = "lib"
EOF
printf 'export module t861_core;\nexport int core_v() { return 1; }\n' > core/src/core.cppm
for m in cli gui; do
    cat > $m/mcpp.toml <<EOF
[package]
name    = "$m"
version = "0.1.0"

[dependencies]
core = { path = "../core" }

[targets.$m]
kind = "lib"
EOF
    printf 'export module t861_%s;\nimport t861_core;\nexport int %s_v() { return core_v(); }\n' $m $m > $m/src/$m.cppm
done
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
cli = { path = "../cli" }
gui = { path = "../gui" }

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'import t861_cli;\nimport t861_gui;\nint main() { return cli_v() + gui_v() == 2 ? 0 : 1; }\n' > app/src/main.cpp
for m in core cli gui; do
    cat > $m/build.mcpp <<EOF
import mcpp;
int main() {
    mcpp::define("T861_$m=1");
    mcpp::link_flag("-Wl,-rpath,/t861/$m");
    return 0;
}
EOF
done

snapshot() {
    local out="$1"
    mkdir -p "$out"
    find target -name build.ninja -not -path '*/.build-mcpp/*' | sort > "$out/ninja.list"
    [ -s "$out/ninja.list" ] || fail "no build.ninja was written"
    local n=0
    while read -r f; do
        n=$((n + 1))
        cp "$f" "$out/build.ninja.$n"
    done < "$out/ninja.list"
    for m in core cli gui; do cp $m/target/.build-mcpp/build.mcpp.cache "$out/$m.cache"; done
}
# The names of the programs that ran, one per line, in the order the lines came.
ran_lines() { grep -E "^ *build\.mcpp [a-z]+ .*ran [0-9]" "$1" | sed -E 's/^ *build\.mcpp ([a-z]+) .*/\1/'; }

"$MCPP" build --workspace -j 1 > serial.log 2>&1 || fail "the build at -j 1 failed" serial.log
snapshot serial
rm -rf target */target
"$MCPP" build --workspace -j 4 > concurrent.log 2>&1 || fail "the build at -j 4 failed" concurrent.log
snapshot concurrent

# A
cmp -s serial/ninja.list concurrent/ninja.list || fail "A: the two builds wrote build.ninja in different places"
for f in serial/build.ninja.* serial/*.cache; do
    cmp -s "$f" "concurrent/$(basename "$f")" \
        || fail "A: $(basename "$f") differs between the build at -j 1 and the build at -j 4"
done

# B
[ "$(ran_lines serial.log | wc -l | tr -d ' ')" = 3 ] || fail "B: expected three programs to run" serial.log
[ "$(ran_lines serial.log)" = "$(ran_lines concurrent.log)" ] \
    || fail "B: the programs ran in another order at -j 4" serial.log concurrent.log

# C. core first, because cli and gui depend on it; and every program's link flag
# is on the application's link line, in one order.
[ "$(ran_lines serial.log | head -1)" = core ] || fail "C: core's program did not run first" serial.log
link_order() {
    grep -ohE -- "-Wl,-rpath,/t861/[a-z]+" "$1" | sed -E 's|.*/||' | awk '!seen[$0]++' | tr '\n' ' '
}
[ "$(link_order serial/build.ninja.1 | wc -w | tr -d ' ')" = 3 ] \
    || fail "C: the link line does not carry the three programs' link flags ($(link_order serial/build.ninja.1))"
[ "$(link_order serial/build.ninja.1)" = "$(link_order concurrent/build.ninja.1)" ] \
    || fail "C: the link flags are in another order at -j 4"

echo "PASS: 861_concurrent_compiles_plan_what_serial_compiles_plan"
