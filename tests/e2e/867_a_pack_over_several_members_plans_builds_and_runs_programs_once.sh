#!/usr/bin/env bash
# requires: unix-shell
# 867 -- `mcpp pack` over several members plans once, builds once, runs each build
# program once, and gives each member's program its own staged tree.
#
# mcpp#749 (K1, member selection design 2026-09-30). A project that ships several
# programs from one workspace ran `mcpp pack -p <member>` once per program. Each
# invocation planned the graph, ran the build programs again, including those of the
# members the programs share, and started the build again. `--workspace`, and a
# repeated `-p`, plan the members as one selection and stage each in a tree of its
# own.
#
# The fixture. `cli` and `gui` are programs, each providing the format `zap`
# through its own build program, and both use `core`, a library with a build
# program of its own that provides nothing. Every program appends one line to
# `runs.log` under its `OUT_DIR` when it runs, stating the request it answered
# and the staged tree it was told.
#
# Criteria (#749, A to D):
#   A. `mcpp pack --workspace --format zap` runs `core`'s program once in all, and
#      each provider's program once for each of the two passes a dispatched
#      format prepares: once when no format is asked, and once when it is.
#   B. Each member's packed tree has the same files with the same content as
#      `mcpp pack -p <member>` produces for it, and so does its distributable.
#   C. After `mcpp build --workspace` with the same profile, the pack compiles
#      nothing: no object is added to `.ninja_log`.
#   D. Each member's program reads its own `pack_stage_dir()`, and the tree it
#      names is the member's.
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
members = ["core", "cli", "gui"]
EOF

mkdir -p core/src
cat > core/mcpp.toml <<'EOF'
[package]
name    = "core"
version = "0.1.0"

[targets.core]
kind = "lib"
EOF
cat > core/src/core.cppm <<'EOF'
export module zcore;
export int core_answer() { return 42; }
EOF
cat > core/build.mcpp <<'EOF'
import mcpp;
#include <fstream>
#include <string>
int main() {
    std::ofstream log(std::string(mcpp::out_dir()) + "/runs.log", std::ios::app);
    log << "format=" << mcpp::pack_format() << " stage=" << mcpp::pack_stage_dir() << "\n";
    return 0;
}
EOF

# The distribution step: what the staged tree held, and whose it was.
cat > dist.sh <<'EOF'
#!/usr/bin/env bash
set -e
stage="$1"; out="$2"; member="$3"
{ echo "member=$member"; ( cd "$stage" && find . -type f | sort ); } > "$out"
EOF
chmod +x dist.sh

for m in cli gui; do
    mkdir -p $m/src
    cat > $m/mcpp.toml <<EOF
[package]
name    = "$m"
version = "0.1.0"

[dependencies]
core = { path = "../core" }

[targets.$m]
kind = "bin"
main = "src/main.cpp"
EOF
    cat > $m/src/main.cpp <<EOF
#include <cstdio>
import zcore;
int main() { std::printf("$m %d\n", core_answer()); return 0; }
EOF
    cat > $m/build.mcpp <<'EOF'
import mcpp;
#include <fstream>
#include <string>
#include <string_view>
int main() {
    const std::string out = mcpp::out_dir();
    // One line per run: the request it answered and the tree it was told.
    {
        std::ofstream log(out + "/runs.log", std::ios::app);
        log << "format=" << mcpp::pack_format() << " stage=" << mcpp::pack_stage_dir() << "\n";
    }
    // Half one, unconditional; half two, only when the format is asked for.
    mcpp::provides_pack_format("zap");
    if (std::string_view(mcpp::pack_format()) != "zap") return 0;
    {
        std::ofstream seen(out + "/seen-stage.txt", std::ios::trunc);
        seen << mcpp::pack_stage_dir() << "\n";
    }
    const std::string root = std::string(mcpp::manifest_dir()) + "/..";
    const std::string dist = out + "/" + mcpp::package_name() + ".zap";
    mcpp::action a;
    a.id   = "zap";
    a.role = "artifact";
    a.arg((root + "/dist.sh").c_str())
     .arg("${mcpp.stage_dir}")
     .arg(dist.c_str())
     .arg(mcpp::package_name())
     .input((std::string("${mcpp.target_file:") + mcpp::package_name() + "}").c_str())
     .output(dist.c_str())
     .submit();
    return 0;
}
EOF
done

# A member's build-program output directory: one per configuration and
# package, below the workspace root (mcpp 2026.10.10.1+). Every command here
# builds the one configuration the pack asks for.
outdir() { ls -d target/.build-mcpp/out/*/"$1" 2>/dev/null | head -1; }
# The lines a member's program appended to its log, all of them.
runs() { cat "$(outdir "$1")/runs.log" 2>/dev/null; }
count() { runs "$1" | grep -c "$2" || true; }

stage_of() { ls -d "$1"/target/dist/"$1"-0.1.0-* | grep -v -e '\.stage-manifest$' -e '\.tar\.gz$' | head -1; }
objects() { awk -F'\t' '$4 ~ /\.o$/' "$1/.ninja_log" | wc -l | tr -d ' '; }

# ── A ────────────────────────────────────────────────────────────────────────
"$MCPP" pack --workspace --format zap > a.log 2>&1 || fail "A: the pack failed" a.log
# `core` is not a program, so it is not packed, and its program ran once in all:
# in the pass that asks for no format, and never again for a member whose answer
# does not depend on the request.
[ "$(count core '')" = 1 ] || fail "A: core's program ran $(count core '') times, not once" $(outdir core)/runs.log
for m in cli gui; do
    [ "$(count $m '')" = 2 ] || fail "A: $m's program ran $(count $m '') times, not once for each pass" $(outdir $m)/runs.log
    [ "$(count $m '^format= stage=$')" = 1 ] || fail "A: $m's first pass was told a request" $(outdir $m)/runs.log
    [ "$(count $m '^format=zap stage=/')" = 1 ] || fail "A: $m's second pass was not told its format and stage" $(outdir $m)/runs.log
done
grep -q "Packed .*cli.zap" a.log || fail "A: cli's distributable was not reported" a.log
grep -q "Packed .*gui.zap" a.log || fail "A: gui's distributable was not reported" a.log
echo "ok: A, one plan, one run of the shared member's program, one run of each provider's per pass"

# ── D ────────────────────────────────────────────────────────────────────────
for m in cli gui; do
    # The two spellings of one directory are compared as directories: a
    # temporary directory may be reached through a symbolic link.
    want="$(cd "$(stage_of $m)" && pwd -P)"
    saw="$(cat $(outdir $m)/seen-stage.txt)"
    [ -d "$saw" ] && [ "$(cd "$saw" && pwd -P)" = "$want" ] \
        || fail "D: $m's program read '$saw', and its tree is '$want'" a.log
    grep -qx "member=$m" $(outdir $m)/$m.zap || fail "D: $m's distributable was made for another member" $(outdir $m)/$m.zap
    grep -qx "./bin/$m" $(outdir $m)/$m.zap || fail "D: $m's tree did not hold $m's program" $(outdir $m)/$m.zap
done
[ "$(cat $(outdir cli)/seen-stage.txt)" != "$(cat $(outdir gui)/seen-stage.txt)" ] \
    || fail "D: two members were told one staged tree"
grep -q "gui" $(outdir cli)/cli.zap && fail "D: cli's tree holds gui's program" $(outdir cli)/cli.zap
echo "ok: D, each member's program reads its own staged tree"

# ── B ────────────────────────────────────────────────────────────────────────
mkdir multi single
for m in cli gui; do
    mkdir multi/$m single/$m
    cp -R "$(stage_of $m)" multi/$m/tree
    cp "$(stage_of $m).stage-manifest" multi/$m/stage-manifest
    cp $(outdir $m)/$m.zap multi/$m/dist
    "$MCPP" pack -p $m --format zap > b-$m.log 2>&1 || fail "B: mcpp pack -p $m failed" b-$m.log
    cp -R "$(stage_of $m)" single/$m/tree
    cp "$(stage_of $m).stage-manifest" single/$m/stage-manifest
    cp $(outdir $m)/$m.zap single/$m/dist
    diff -r multi/$m single/$m > b-$m.diff || fail "B: $m's packed tree differs from the one mcpp pack -p $m makes" b-$m.diff
done
echo "ok: B, each member's tree and distributable are those of mcpp pack -p <member>"

# ── C ────────────────────────────────────────────────────────────────────────
rm -rf target core/target cli/target gui/target
"$MCPP" build --workspace --release > c-build.log 2>&1 || fail "C: the workspace did not build" c-build.log
dirs=$(find target -name build.ninja -exec dirname {} \;)
[ "$(echo "$dirs" | wc -l | tr -d ' ')" = 1 ] || fail "C: expected one build directory, got: $dirs" c-build.log
before=$(objects "$dirs")
[ "$before" -gt 0 ] || fail "C: the build compiled nothing" "$dirs/.ninja_log"
"$MCPP" pack --workspace --format zap > c-pack.log 2>&1 || fail "C: the pack failed" c-pack.log
[ "$(find target -name build.ninja | wc -l | tr -d ' ')" = 1 ] || fail "C: the pack used another build directory" c-pack.log
after=$(objects "$dirs")
[ "$after" = "$before" ] || fail "C: the pack compiled $((after - before)) object(s) after the build" "$dirs/.ninja_log" c-pack.log
echo "ok: C, after a build of the workspace the pack compiles nothing"

echo "PASS: 867_a_pack_over_several_members_plans_builds_and_runs_programs_once"
