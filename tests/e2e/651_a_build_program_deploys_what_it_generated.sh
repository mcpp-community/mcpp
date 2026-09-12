#!/usr/bin/env bash
# requires: elf gcc
# 651_a_build_program_deploys_what_it_generated.sh -- `mcpp::deploy(from, to)`
# (#622 A4): a file a build program produced (an action's own declared
# output) or selected, placed beside the artifact at a path relative to the
# executable's directory. `[runtime] deploy` can only name a file that
# already exists in the package, package-root-relative, because its `from`
# refuses an absolute path -- so it cannot name what an action writes into
# MCPP_OUT_DIR one step later. This is the directive that closes that gap.
#
# What this holds, each with the wrong answer it excludes:
#
#   1. THE CACHE TAG IS NON-EMPTY, SO A REPLAY CARRIES THE ENTRY. Deleting
#      bin/ and rebuilding while the build.mcpp cache stays fresh (a cache
#      HIT, not a re-run) must still restore the deployed file -- the same
#      replay criterion `mcpp:warning=`/`mcpp:pack-format=` are held to.
#   2. `from` MAY BE ABSOLUTE. It is an action's own declared output, which
#      the manifest key's `deploy_path_problem("from", ..., false)` refuses.
#   3. THE COPY EDGE'S INPUT IS THE ACTION'S OUTPUT, so ninja orders them --
#      read from the emitted build.ninja, not inferred from a green build.
#   4. `to = "../x"` IS REFUSED, naming the directive and the package.
#   5. MCPP_TARGET_MIN_PLATFORM_VERSION (#622 A11) IS EMPTY ON LINUX.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME=$HOME/.mcpp

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"

# ── The provider: an action writes a file, the program deploys it ──────────
mkdir -p app/src
cd app

cat > mcpp.toml <<'TOML'
[package]
name    = "app"
version = "0.1.0"

[toolchain]
linux = "gcc@16.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
TOML

cat > src/main.cpp <<'EOF'
#include <cstdio>
int main() { std::puts("app"); return 0; }
EOF

# The action's command is an argv with no shell assumed, so a copy is a
# script rather than an inline shell pipeline.
cat > gen.sh <<'EOF'
#!/usr/bin/env bash
set -e
cp "$1" "$2"
EOF
chmod +x gen.sh

printf 'hello resource\n' > res.txt

cat > build.mcpp <<'EOF'
import mcpp;
#include <cstdio>
#include <string>
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string src  = root + "/res.txt";
    const std::string out  = std::string(mcpp::out_dir()) + "/gen/res.bin";

    // The generated file: role "source" because it is produced rather than
    // performed, though it is not itself compiled (no compilable extension) --
    // is_compilable_output leaves it out of the compile set and simply
    // declares it, exactly as protoc's companion .h does.
    mcpp::action a;
    a.id   = "gen-res";
    a.role = "source";
    a.arg((root + "/gen.sh").c_str()).arg(src.c_str()).arg(out.c_str())
     .input(src.c_str())
     .output(out.c_str())
     .submit();

    // The directive this test exists for: `from` is the action's own
    // absolute output, which `[runtime] deploy` could never name.
    mcpp::deploy(out.c_str(), "app.resources");

    // #622 A11, asserted from inside the program rather than inferred: empty
    // on Linux.
    if (FILE* f = std::fopen((std::string(mcpp::out_dir()) + "/minplat.txt").c_str(), "w")) {
        std::fputs(mcpp::min_platform_version(), f);
        std::fclose(f);
    }
    return 0;
}
EOF

MCPP="${MCPP:-mcpp}"
find_graph() { find target -name build.ninja | head -1; }

# ── 1. a plain build deploys the generated file beside the executable ──────
# `target/<triple>/<fingerprint>/bin/...`, never a bare `bin/` at the project
# root — found rather than hardcoded, the way 264_pack_library_is_relocatable
# and 174_cache_modes_and_commands already do.
"$MCPP" build > b1.log 2>&1 || fail "build failed" b1.log
DEPLOYED=$(find target -path '*/bin/app.resources/res.bin' | head -1)
[ -n "$DEPLOYED" ] || fail "the deployed file does not exist" b1.log
BINDIR=$(echo "$DEPLOYED" | sed 's#/app\.resources/res\.bin$##')
grep -qx "hello resource" "$DEPLOYED" \
  || fail "the deployed file does not carry the action's output" "$DEPLOYED"

# #622 A11: empty on Linux, and printed by mcpp:: itself rather than
# asserted about the environment from the shell, so this is what the
# PROGRAM saw, not what the harness happens to run under.
MINPLAT=$(find target -name minplat.txt | head -1)
[ -n "$MINPLAT" ] || fail "the program never wrote minplat.txt" b1.log
[ -z "$(cat "$MINPLAT")" ] \
  || fail "MCPP_TARGET_MIN_PLATFORM_VERSION was not empty on Linux" "$MINPLAT"

# ── 2. the copy edge's input is the action's declared output ───────────────
# Read from the emitted graph, not inferred from a green build: ninja must
# order the copy after the action because the copy's INPUT IS the action's
# OUTPUT, the same file, named the same way in both places.
G=$(find_graph)
[ -n "$G" ] || fail "no build.ninja" b1.log
ACTION_LINE=$(grep " : mcpp_action_" "$G" | grep "gen/res.bin" | head -1)
[ -n "$ACTION_LINE" ] || fail "no build.ninja action edge names gen/res.bin" "$G"
# The first whitespace-separated token after "build " is the action's own
# declared output path (absolute; TMPDIR here has no spaces to escape).
ACTION_OUT=$(echo "$ACTION_LINE" | sed -n 's/^build \([^ ]*\).*/\1/p')
[ -n "$ACTION_OUT" ] || fail "could not read the action's output path" "$G"
STAGE_LINE=$(grep "stage_file" "$G" | grep "app.resources/res.bin" | head -1)
[ -n "$STAGE_LINE" ] || fail "no stage_file edge for app.resources/res.bin" "$G"
echo "$STAGE_LINE" | grep -qF "$ACTION_OUT" \
  || fail "the copy edge's input is not the action's own output path" "$G"

# ── 3. touching the action's input changes the deployed file ───────────────
printf 'changed resource\n' > res.txt
touch src/main.cpp   # past the whole-project no-op fast path; see 139
"$MCPP" build > b2.log 2>&1 || fail "second build failed" b2.log
grep -qx "changed resource" "$DEPLOYED" \
  || fail "touching the action's input did not change the deployed file" "$DEPLOYED"

# ── 4. the replay criterion: bin/ deleted, rebuilt on a build.mcpp cache hit
rm -rf "$BINDIR"
touch src/main.cpp
"$MCPP" build > b3.log 2>&1 || fail "third build failed" b3.log
grep -q "up to date (cached)" b3.log \
  || fail "the third build re-ran build.mcpp; the replay path was not exercised" b3.log
[ -f "$DEPLOYED" ] \
  || fail "the deployed file was not restored on a build.mcpp cache hit" b3.log
grep -qx "changed resource" "$DEPLOYED" \
  || fail "the restored file does not carry the action's latest output" "$DEPLOYED"

# ── 5. `mcpp pack --format dir` stages the deployed file ────────────────────
"$MCPP" pack --format dir > b4.log 2>&1 || fail "pack --format dir failed" b4.log
STAGED=$(find target/dist -name res.bin | head -1)
[ -n "$STAGED" ] || fail "mcpp pack --format dir did not stage the deployed file" b4.log
case "$STAGED" in
  */bin/app.resources/res.bin) : ;;
  *) fail "the staged file is not at bin/app.resources/res.bin" b4.log ;;
esac

echo "PASS: deploy directive (a plain build, the graph edge, an input change, the replay, and pack)"

# ── 6. `to = "../x"` is refused, naming the directive and the package ──────
cd "$TMP"
cp -r app escapes
cd escapes
sed -i 's/name    = "app"/name    = "escapes"/' mcpp.toml
sed -i 's/mcpp::deploy(out.c_str(), "app.resources")/mcpp::deploy(out.c_str(), "..\/x")/' build.mcpp
set +e
"$MCPP" build > b5.log 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "a deploy 'to' escaping the executable directory was accepted" b5.log
grep -q "deploy" b5.log || fail "the refusal does not name the directive" b5.log
grep -q "escapes" b5.log || fail "the refusal does not name the package" b5.log
grep -q "component" b5.log || fail "the refusal does not name the path problem" b5.log

echo "PASS: 651_a_build_program_deploys_what_it_generated"
