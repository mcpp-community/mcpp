#!/usr/bin/env bash
# requires: gcc
# 638_pack_format_dispatch.sh — `mcpp pack --format <name>` dispatches to a
# package, and no distribution format lives in the engine.
#
# `tar` and `dir` answer the same question `msi` and `appimage` answer -- what
# shape does the output take -- so they are values of ONE flag, and everything
# past the two the engine owns comes from the resolved graph. What the engine
# adds is the mechanism: a staged tree an artifact action can consume, the rest
# of `[package]` in the build program, and the dispatch itself.
#
# The four properties this holds, each with the wrong answer it excludes:
#
#   1. DECLARE UNCONDITIONALLY, SUBMIT CONDITIONALLY. A build that asks for no
#      format still declares one, so `--format bogus` can name what IS
#      available. A member that declared only when asked works for its author
#      and makes the set unknowable for everyone else.
#   2. THE STAGED TREE IS REAL WHEN THE ACTION RUNS. Asserted from INSIDE the
#      action, by listing the tree into the output -- not by the action's exit
#      code, because a command that writes nothing and exits 0 is the measured
#      failure this whole mechanism exists to prevent.
#   3. A PLAIN BUILD HAS NO DISTRIBUTION EDGE, before or after a pack.
#   4. THE PLACEHOLDER REFUSES OUTSIDE A PACKAGING PASS, rather than expanding
#      to an empty string that the tool would accept.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ── The provider: one build program, both halves of the contract ───────────
mkdir -p app/src
cd app

cat > mcpp.toml <<'EOF'
[package]
name        = "app"
version     = "2.5.0"
description = "a program that ships"
license     = "Apache-2.0"
authors     = ["Ada <ada@example.org>"]
repo        = "https://example.org/app"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

cat > src/main.cpp <<'EOF'
#include <cstdio>
int main() { std::puts("app"); return 0; }
EOF

# The dist step. A separate script because an action's command is an argv with
# no shell assumed, which is the same reason 188_build_actions.sh writes one.
#
# It writes the package VERSION and the staged tree's top level into its
# output, so the assertions below read what the action actually saw rather than
# whether it exited 0.
cat > dist.sh <<'EOF'
#!/usr/bin/env bash
set -e
version="$1"; stage="$2"; out="$3"; minplat="$4"
{
  echo "version=$version"
  # #622 A11: MCPP_TARGET_MIN_PLATFORM_VERSION, unset for every other build
  # program below (they pass 3 args), so this line reads "min_platform_version="
  # for them too -- harmless, since the assertion below matches it by exact
  # line rather than by file content as a whole.
  echo "min_platform_version=$minplat"
  echo "staged:"
  ls -1 "$stage" | sort
} > "$out"
EOF
chmod +x dist.sh

cat > build.mcpp <<'EOF'
import mcpp;
#include <cstdio>
#include <string>
#include <string_view>
int main() {
    // Half one: unconditional. This is what makes the set knowable on a build
    // that asked for nothing.
    mcpp::provides_pack_format("zap");

    // Half two: conditional. A plain build must have no such edge.
    if (std::string_view(mcpp::pack_format()) != "zap") return 0;

    const std::string root = mcpp::manifest_dir();
    const std::string out  = std::string(mcpp::out_dir()) + "/app.zap";
    mcpp::action a;
    a.id          = "zap";
    a.role        = "artifact";
    a.description = "zap";
    a.arg((root + "/dist.sh").c_str())
     // The VERSION comes from `[package]`, not from an option the project
     // restates: a second copy drifts with nothing able to detect it.
     .arg(mcpp::package_version())
     .arg("${mcpp.stage_dir}")
     .arg(out.c_str())
     // #622 A11: the platform floor for this triple, empty on Linux. Read
     // here rather than restated, exactly as the version above is.
     .arg(mcpp::min_platform_version())
     .input("${mcpp.target_file:app}")
     .output(out.c_str())
     .submit();
    return 0;
}
EOF

MCPP="${MCPP:-mcpp}"

graph_dist() { sed -n '2p' "$1" | sed 's/.*;dist=//;s/;.*//'; }
find_graph() { find target -name build.ninja | head -1; }

# ── 1. a plain build declares, and submits nothing ─────────────────────────
"$MCPP" build --release > b1.log 2>&1 || { cat b1.log; echo "FAIL: plain build failed"; exit 1; }
G=$(find_graph)
[ -n "$G" ] || { echo "FAIL: no build.ninja"; exit 1; }
[ "$(graph_dist "$G")" = "none" ] \
  || { sed -n '2p' "$G"; echo "FAIL: a plain build's graph is not dist=none"; exit 1; }
[ -z "$(find target -name 'app.zap' 2>/dev/null)" ] \
  || { echo "FAIL: a plain build produced a distributable"; exit 1; }

# ── 2. an unknown format is refused, BEFORE the build, naming what exists ──
set +e
"$MCPP" pack --format bogus > b2.log 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || { cat b2.log; echo "FAIL: --format bogus was accepted"; exit 1; }
grep -q "unknown --format 'bogus'" b2.log \
  || { cat b2.log; echo "FAIL: the refusal does not name the value"; exit 1; }
# THE LIST IS THE POINT. A refusal that named a fixed list would be the
# coupling this mechanism removes; this one names the graph's own answer.
grep -q "available in this build: tar, dir, zap" b2.log \
  || { cat b2.log; echo "FAIL: the refusal does not name what is available"; exit 1; }
# Nothing was compiled to find that out.
grep -q "Compiling app" b2.log \
  && { cat b2.log; echo "FAIL: the refusal arrived after a compile"; exit 1; }
# AND NOTHING REACHED STDOUT. This is the path a client hits when it probes an
# mcpp for a capability, so the machine-output contract
# (202_machine_output_contract.sh) requires an empty stdout, a non-empty stderr
# and exit 2. It is asserted here as well because the tension is local to this
# feature: the valid set is a property of the resolved graph, so the refusal
# cannot be decided until prepare has run -- and prepare narrates what it
# resolves. Deciding it later is what broke the contract once.
set +e
so=$("$MCPP" pack --format bogus 2>/dev/null); rc=$?
se=$("$MCPP" pack --format bogus 2>&1 >/dev/null)
set -e
[ -z "$so" ] || { echo "FAIL: the refusal wrote to stdout: $(echo "$so" | head -1)"; exit 1; }
[ -n "$se" ] || { echo "FAIL: the refusal said nothing on stderr"; exit 1; }
[ "$rc" = 2 ] || { echo "FAIL: the refusal exited $rc, expected 2"; exit 1; }

# ── 3. the dispatched format produces a file, and it saw the staged tree ───
"$MCPP" pack --format zap > b3.log 2>&1 || { cat b3.log; echo "FAIL: pack --format zap failed"; exit 1; }
Z=$(find target -name 'app.zap' | head -1)
[ -n "$Z" ] || { cat b3.log; echo "FAIL: --format zap produced nothing"; exit 1; }
# The engine handed the build program the rest of `[package]`.
grep -qx "version=2.5.0" "$Z" \
  || { cat "$Z"; echo "FAIL: the action was not told the package version"; exit 1; }
# #622 A11: MCPP_TARGET_MIN_PLATFORM_VERSION is empty on a Linux host/target,
# read through mcpp::min_platform_version() rather than restated.
grep -qx "min_platform_version=" "$Z" \
  || { cat "$Z"; echo "FAIL: MCPP_TARGET_MIN_PLATFORM_VERSION was not empty on Linux"; exit 1; }
# The action ran with a staged tree that already held the program. This is the
# assertion the ordering exists for: an artifact edge is scheduled by ninja and
# the tree is staged by mcpp after the link, so a single-pass design would run
# this against a directory that does not exist.
grep -qx "bin" "$Z" \
  || { cat "$Z"; echo "FAIL: the staged tree had no bin/ when the action ran"; exit 1; }
grep -q "Packed" b3.log \
  || { cat b3.log; echo "FAIL: the produced file was not reported"; exit 1; }
# The staged tree is described by a SIBLING of itself, never a member: a file
# inside it would ship inside every format that packages the directory.
[ -n "$(find target/dist -maxdepth 1 -name '*.stage-manifest' 2>/dev/null)" ] \
  || { echo "FAIL: no stage manifest beside the staged tree"; exit 1; }
[ -z "$(find target/dist -mindepth 2 -name '*.stage-manifest' 2>/dev/null)" ] \
  || { echo "FAIL: the stage manifest landed inside the staged tree"; exit 1; }

# ── 4. a plain build AFTER the pack still has no distribution edge ─────────
rm -f "$Z"
"$MCPP" build --release > b4.log 2>&1 || { cat b4.log; echo "FAIL: build after pack failed"; exit 1; }
[ "$(graph_dist "$(find_graph)")" = "none" ] \
  || { echo "FAIL: the graph still says dist= after a plain build"; exit 1; }
[ ! -f "$Z" ] \
  || { echo "FAIL: a plain build regenerated the distributable"; exit 1; }

# ── 5. the placeholder outside a packaging pass is refused ─────────────────
# The same action, ungated. An empty expansion would give the tool the build
# directory root, which exists -- so the mistake would produce a plausible
# artifact instead of a diagnostic.
cd "$TMP"
cp -r app ungated
cd ungated
cat > build.mcpp <<'EOF'
import mcpp;
#include <string>
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string out  = std::string(mcpp::out_dir()) + "/app.zap";
    mcpp::action a;
    a.id = "zap"; a.role = "artifact";
    a.arg((root + "/dist.sh").c_str()).arg("x").arg("${mcpp.stage_dir}").arg(out.c_str())
     .input("${mcpp.target_file:app}").output(out.c_str()).submit();
    return 0;
}
EOF
set +e
"$MCPP" build --release > b5.log 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || { cat b5.log; echo "FAIL: an ungated stage_dir built"; exit 1; }
grep -q "this build is not packaging" b5.log \
  || { cat b5.log; echo "FAIL: the refusal does not say why"; exit 1; }
grep -q "provides_pack_format" b5.log \
  || { cat b5.log; echo "FAIL: the refusal does not say what to do instead"; exit 1; }

# ── 6. a role other than artifact cannot reach the staged tree ─────────────
# GATED, so this is a role refusal and not the one above. An ungated action is
# already refused in the FIRST pass of `mcpp pack`, before there is a staged
# tree, which is why the two cases need different fixtures rather than a
# one-word edit: only a gated action gets far enough for its role to matter.
cd "$TMP"
cp -r app wrongrole
cd wrongrole
cat > build.mcpp <<'EOF'
import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("zap");
    if (std::string_view(mcpp::pack_format()) != "zap") return 0;
    const std::string root = mcpp::manifest_dir();
    const std::string out  = std::string(mcpp::out_dir()) + "/app.zap";
    mcpp::action a;
    a.id = "zap"; a.role = "source";
    a.arg((root + "/dist.sh").c_str()).arg("x").arg("${mcpp.stage_dir}").arg(out.c_str())
     .output(out.c_str()).submit();
    return 0;
}
EOF
set +e
"$MCPP" pack --format zap > b6.log 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || { cat b6.log; echo "FAIL: a source action reached the staged tree"; exit 1; }
grep -q 'other than "artifact"' b6.log \
  || { cat b6.log; echo "FAIL: the role refusal does not name the role"; exit 1; }

# ── 7. declared and never submitted is a refusal, not a success ────────────
# THE HALF A MEMBER AUTHOR IS MOST LIKELY TO GET WRONG. A gate that never
# opens leaves a pass that succeeds and produces no package, which reads as
# "packaging is not implemented yet" rather than as a defect in the member.
cd "$TMP"
cp -r app silent
cd silent
cat > build.mcpp <<'EOF'
import mcpp;
int main() { mcpp::provides_pack_format("zap"); return 0; }
EOF
set +e
"$MCPP" pack --format zap > b7.log 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || { cat b7.log; echo "FAIL: a format nothing claimed reported success"; exit 1; }
grep -q "no action claimed --format 'zap'" b7.log \
  || { cat b7.log; echo "FAIL: the refusal does not name the unclaimed format"; exit 1; }

# ── 8. an artifact action that predates the request is not the package ─────
# THE CRITERION IS "WHAT THE REQUEST INTRODUCED", and this is what distinguishes
# it from "any artifact action". A codesign stamp or a size budget is also an
# artifact action and is present whether or not a format was asked for; naming
# one as the distributable would be a wrong answer that looks like a right one.
#
# It is also what an earlier revision got wrong from the other side: the check
# collected only actions naming ${mcpp.stage_dir}, which refused a member that
# packages ONE NAMED PROGRAM and never reads the tree -- the shape the design
# record recommends, after a bind path that resolved to nothing produced a
# valid, empty, 52 KB installer. So this fixture submits both shapes: an
# ungated stamp that must be ignored, and a gated action that names no staged
# tree at all and must still be reported.
cd "$TMP"
cp -r app twoshapes
cd twoshapes
cat > build.mcpp <<'EOF'
import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("zap");
    const std::string root = mcpp::manifest_dir();

    // Ungated: present in both passes, so it is not the distributable.
    const std::string stamp = std::string(mcpp::out_dir()) + "/size.stamp";
    mcpp::action s;
    s.id = "size-budget"; s.role = "artifact";
    s.arg((root + "/dist.sh").c_str()).arg("stamp").arg(root.c_str()).arg(stamp.c_str())
     .input("${mcpp.target_file:app}").output(stamp.c_str());
    s.submit();

    if (std::string_view(mcpp::pack_format()) != "zap") return 0;
    // Gated, and it names NO staged tree: the program arrives through
    // ${mcpp.target_file:...} exactly as an MSI's one File row does.
    const std::string out = std::string(mcpp::out_dir()) + "/app.zap";
    mcpp::action a;
    a.id = "zap"; a.role = "artifact";
    a.arg((root + "/dist.sh").c_str()).arg("named").arg(root.c_str()).arg(out.c_str())
     .input("${mcpp.target_file:app}").output(out.c_str());
    a.submit();
    return 0;
}
EOF
"$MCPP" pack --format zap > b8.log 2>&1 || { cat b8.log; echo "FAIL: a member that reads no staged tree was refused"; exit 1; }
grep -q "app.zap" b8.log \
  || { cat b8.log; echo "FAIL: the gated action was not reported as the package"; exit 1; }
grep -q "size.stamp" b8.log \
  && { cat b8.log; echo "FAIL: an action predating the request was reported as the package"; exit 1; }
# Both files exist -- the stamp was built, it was simply not the answer.
[ -n "$(find target -name 'size.stamp' 2>/dev/null)" ] \
  || { echo "FAIL: the ungated artifact action did not run at all"; exit 1; }

# ── 9. a format that names no staged tree survives a staging refusal ──────
# THE CASE macOS FOUND, HELD ON EVERY PLATFORM.
#
# `mcpp pack` refuses to bundle a Mach-O PROGRAM: the built-in closure walk is
# `LD_TRACE_LOADED_OBJECTS`, glibc's mechanism, and dyld ignores it and runs the
# program instead. That refusal is right about the built-in archive and says
# nothing about whether a `.app` bundler can work -- one that names a single
# program needs no closure walk at all. Before this, the refusal happened before
# the dispatch, so EVERY dispatched format was unreachable on that target.
#
# Staging is now a service to the provider rather than a precondition. This leg
# cannot reproduce the Mach-O refusal on Linux, so it holds the property the fix
# rests on instead: a provider that reads no staged tree is reported, and the
# reason travels far enough to reach the placeholder's refusal.
cd "$TMP"
cp -r app nostage
cd nostage
cat > build.mcpp <<'EOF'
import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("zap");
    if (std::string_view(mcpp::pack_format()) != "zap") return 0;
    // Reads NOTHING from the staged tree: the program arrives through
    // ${mcpp.target_file:...}, which is what an `.msi` of one program does.
    const std::string root = mcpp::manifest_dir();
    const std::string out  = std::string(mcpp::out_dir()) + "/app.zap";
    mcpp::action a;
    a.id = "zap"; a.role = "artifact";
    a.arg((root + "/dist.sh").c_str()).arg("named").arg(root.c_str()).arg(out.c_str())
     .input("${mcpp.target_file:app}").output(out.c_str());
    a.submit();
    return 0;
}
EOF
"$MCPP" pack --format zap > b9.log 2>&1 || { cat b9.log; echo "FAIL: a provider that reads no staged tree was refused"; exit 1; }
grep -q "app.zap" b9.log \
  || { cat b9.log; echo "FAIL: the provider was not reported as the package"; exit 1; }
# It really did not consume the tree: no stage manifest edge was added, so the
# action's inputs are the program alone.
grep -q "stage-manifest" b9.log \
  && { cat b9.log; echo "FAIL: a provider that names no tree gained a stage dependency"; exit 1; }
echo "ok: a provider that reads no staged tree is dispatched and reported"

echo "PASS: 638_pack_format_dispatch"
