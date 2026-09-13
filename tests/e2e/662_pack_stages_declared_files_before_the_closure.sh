#!/usr/bin/env bash
# requires: gcc
# 662_pack_stages_declared_files_before_the_closure.sh -- `mcpp pack` stages
# the program and its declared runtime files BEFORE it asks whether this
# host can walk the artifact's dependency closure, and records the outcome
# of that closure walk on the stage manifest.
#
# See .agents/docs/2026-09-13-630-what-a-framework-still-hits-in-the-engine.md
# §3. Before this, a program whose closure could not be walked (a Mach-O on
# any host, a non-PE artifact on a Windows host) was refused BEFORE anything
# was staged, so a dispatched format that names one program and never reads
# the closure (an `.app`/`.msi` bundler) had no tree to work from either.
# Reordering the steps gives every dispatched format a tree regardless, and
# leaves the two ELF-native formats (`tar`, `dir`) exactly as they were: the
# staged/archived tree IS the product for those two, so an unavailable
# closure remains the command failing.
#
# TWO LEGS ON THIS (LINUX) HOST, EACH WITH ITS OWN CLAIM:
#
#   A. `--format dir` of an ordinary ELF program: the reorder must not change
#      the ELF product. Asserted as an exact structure -- `bin/<program>`,
#      the declared runtime file, an empty `lib/` (this fixture bundles
#      nothing), and a stage manifest that says `closure = walked` -- rather
#      than "the command exited 0", which a wrong reorder also satisfies.
#   B. A DISPATCHED format (`--format zap`, in the shape 638 uses) whose
#      action reads `${mcpp.stage_dir}`: the tree it sees carries the
#      declared runtime file, and the manifest beside it says
#      `closure = walked` too -- on Linux the ELF closure is always walked,
#      so this leg's job is to prove the field REACHES a provider, not that
#      it ever reads "not-walked" here.
#
# THE "NOT-WALKED" CASE CANNOT BE PRODUCED ON LINUX. It needs a Mach-O
# program (refused on every host, by format) or a non-PE artifact packed
# from a Windows host -- neither is reachable from a `gcc`-only Linux runner.
# It is covered instead by:
#   - a unit test of the pure decision function
#     (`PackClosureUnavailableOutcome` in test_pack_modes.cpp) and of the
#     manifest writer/reader round-trip for both values
#     (`PackStageTree.*Closure*` in test_pack_stage_tree.cpp);
#   - 266_pack_refuses_a_macho_program.sh (`# requires: macos`), which holds
#     the negative direction end to end: `mcpp pack` (bare, i.e. `--format
#     tar`) of a Mach-O program still exits non-zero.
# The macOS half of THIS fixture -- a dispatched format on a Mach-O program
# staging without its closure, with `closure = not-walked` on the manifest
# and the reason readable by the provider -- is a follow-up for whoever runs
# this suite's iOS/macOS CI job; it is not asserted by any step here.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME=$HOME/.mcpp

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

# ── Leg A: `--format dir`, byte-for-byte the same ELF product ──────────────
cd "$TMP"
"$MCPP" new app > /dev/null
cd app
mkdir -p share
printf 'notes\n' > share/notes.txt
cat >> mcpp.toml <<'EOF'

[toolchain]
linux = "gcc@16.1.0"

[runtime]
deploy_files = ["share/notes.txt"]
EOF

"$MCPP" pack --format dir > dir.log 2>&1 || fail "mcpp pack --format dir failed" dir.log

tree=$(find target/dist -maxdepth 1 -type d -name 'app-0.1.0-*' | head -1)
[ -n "$tree" ] || fail "no staged directory under target/dist" dir.log
[ -x "$tree/bin/app" ] || fail "the staged tree has no executable bin/app" dir.log
[ -f "$tree/bin/notes.txt" ] || fail "the declared runtime file did not reach bin/" dir.log
grep -q 'notes' "$tree/bin/notes.txt" || fail "the deployed file's content is wrong" dir.log
# No third-party dependency was declared, so nothing should be bundled --
# the reorder must not manufacture a lib/ entry that was not there before it.
if [ -d "$tree/lib" ]; then
    n=$(find "$tree/lib" -mindepth 1 | wc -l)
    [ "$n" -eq 0 ] || { find "$tree/lib"; fail "lib/ has entries but nothing was declared" dir.log; }
fi
"$tree/bin/app" > run.log 2>&1 || fail "the staged program does not run" run.log dir.log

manifest="$(dirname "$tree")/$(basename "$tree").stage-manifest"
[ -f "$manifest" ] || fail "no stage manifest beside the staged tree" dir.log
[ "$(sed -n '1p' "$manifest")" = "closure = walked" ] \
    || fail "the manifest's first line is not 'closure = walked'" "$manifest" dir.log
grep -q '^reason = ' "$manifest" \
    && fail "a walked closure must carry no reason line" "$manifest"
grep -q ' bin/notes.txt$' "$manifest" || fail "the deploy file is not in the manifest" "$manifest"
echo "  leg A: --format dir stages bin/app + the deploy file, closure = walked"

# ── Leg B: a dispatched format reads the same tree + the same field ────────
cd "$TMP"
"$MCPP" new zapapp > /dev/null
cd zapapp
mkdir -p share
printf 'notes\n' > share/notes.txt
cat >> mcpp.toml <<'EOF'

[toolchain]
linux = "gcc@16.1.0"

[runtime]
deploy_files = ["share/notes.txt"]
EOF

cat > dist.sh <<'EOF'
#!/usr/bin/env bash
set -e
stage="$1"; manifest="$2"; out="$3"
{
  echo "staged:"
  ls -1 "$stage/bin" | sort
  echo "manifest-first-line:"
  sed -n '1p' "$manifest"
} > "$out"
EOF
chmod +x dist.sh

cat > build.mcpp <<'EOF'
import mcpp;
#include <string>
#include <string_view>
int main() {
    // Unconditional half: this build knows a "zap" format exists whether or
    // not one was requested (638 states why).
    mcpp::provides_pack_format("zap");
    if (std::string_view(mcpp::pack_format()) != "zap") return 0;

    const std::string root  = mcpp::manifest_dir();
    const std::string stage = std::string("${mcpp.stage_dir}");
    const std::string out   = std::string(mcpp::out_dir()) + "/app.zap";
    mcpp::action a;
    a.id          = "zap";
    a.role        = "artifact";
    a.description = "zap";
    a.arg((root + "/dist.sh").c_str())
     .arg(stage.c_str())
     .arg((stage + ".stage-manifest").c_str())
     .arg(out.c_str())
     .input("${mcpp.target_file:zapapp}")
     .output(out.c_str())
     .submit();
    return 0;
}
EOF

"$MCPP" pack --format zap > zap.log 2>&1 || fail "mcpp pack --format zap failed" zap.log
Z=$(find target -name 'app.zap' | head -1)
[ -n "$Z" ] || fail "--format zap produced nothing" zap.log

grep -qx "notes.txt" "$Z" \
    || fail "the action's own view of the staged tree lacks the deploy file" "$Z" zap.log
grep -qx "closure = walked" "$Z" \
    || fail "the manifest the action read does not say closure = walked" "$Z" zap.log
echo "  leg B: --format zap's action sees the deploy file and closure = walked"

echo "PASS: 662_pack_stages_declared_files_before_the_closure"
