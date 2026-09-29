#!/usr/bin/env bash
# 839_a_workspaces_build_programs_are_named_ordered_and_cached.sh -- workspace
# design 2026-09-29 §15 and §17.1.
#
# Every member of the workspace below has a build program; cli and gui depend
# on core.
#
#   B1  each program's line names its package and states that it ran;
#   B2  a member's program runs after the programs of the members it depends
#       on (2026.9.29.4 ran them in discovery order: cli before core);
#   B3  after `--workspace`, a selection of one member, a second
#       `--workspace` and another selection reuse every program's result
#       (2026.9.29.4 reran them: a program's graph document listed every
#       requester of the plan, the virtual root included, so its cache key
#       followed the selection);
#   B4  `mcpp emit build-database` after the build reuses them too;
#   B5  a member another member depends on is named by its short name,
#       version and directory, whether it is selected or reached as cli's path
#       dependency under `-p cli` (build output design revision 3, §5.8).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

cat > mcpp.toml <<'EOF'
[workspace]
members = ["core", "cli", "gui"]
EOF
mkdir -p core/src cli/src gui/src
cat > core/mcpp.toml <<'EOF'
[package]
name = "core"
version = "0.1.0"

[targets.core]
kind = "lib"
EOF
printf 'export module bp_core;\nexport int core_v() { return 1; }\n' > core/src/core.cppm
for m in cli gui; do
    cat > $m/mcpp.toml <<EOF
[package]
name = "$m"
version = "0.1.0"

[dependencies]
core = { path = "../core" }

[targets.$m]
kind = "bin"
main = "src/main.cpp"
EOF
    printf 'import bp_core;\nint main() { return core_v() == 1 ? 0 : 1; }\n' > $m/src/main.cpp
done
for p in core cli gui; do
    printf 'import mcpp;\nint main() { mcpp::define("BP_%s=1"); return 0; }\n' $p > $p/build.mcpp
done

"$MCPP" build --workspace > b1.log 2>&1 || fail "the first build failed" b1.log

# B1
for p in core cli gui; do
    grep -qE "^ *build\.mcpp $p .* ran [0-9]" b1.log || fail "B1: no line states that $p's program ran" b1.log
done

# B2
first=$(grep -m1 -E "^ *build\.mcpp .* ran [0-9]" b1.log)
case "$first" in *"build.mcpp core "*) ;; *) fail "B2: core's program did not run first" b1.log ;; esac

# B3
# A reused program has a line under --verbose only (revision 3, §7.1).
"$MCPP" build -p cli -v > b2.log 2>&1 || fail "-p cli failed" b2.log
touch core/src/core.cppm
"$MCPP" build --workspace -v > b3.log 2>&1 || fail "the second --workspace failed" b3.log
touch core/src/core.cppm
"$MCPP" build -p gui -v > b4.log 2>&1 || fail "-p gui failed" b4.log
for f in b2.log b3.log b4.log; do
    ! grep -qE "^ *build\.mcpp .* ran [0-9]" $f || fail "B3: a program reran in $f" $f
    grep -qE "^ *build\.mcpp .* cached" $f || fail "B3: $f shows no program at all" $f
done

# B4
"$MCPP" emit build-database --format json -o db.json > e.log 2>&1 || fail "emit failed" e.log
! grep -qE "^ *build\.mcpp .* ran [0-9]" e.log || fail "B4: emit reran a program" e.log

# B5
grep -qxF "   Compiling core v0.1.0 (core)" b1.log || fail "B5: core is not named by its directory" b1.log
grep -qE "^ +(Compiling|Fresh) core v0\.1\.0 \(core\)$" b2.log || fail "B5: under -p cli, core is not named by its directory" b2.log
! grep -q "(path)" b1.log b2.log || fail "B5: a package is named by its source kind, not its directory" b1.log b2.log

echo "PASS: 839_a_workspaces_build_programs_are_named_ordered_and_cached"
