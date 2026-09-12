#!/usr/bin/env bash
# 659_an_action_declaration_has_no_size_limit.sh -- `mcpp::action`'s list
# fields have no declared size limit (2026.9.13.1).
#
# Until this release the bundled `mcpp` module collected an action's inputs,
# outputs and command into fixed arrays (8192 bytes of serialised JSON for
# `inputs` and `outputs`, 16384 for `command`) and refused a declaration that
# did not fit with "arguments did not fit". The bound was in bytes, so a
# consumer's checkout depth decided whether a list of 44 resource files fit
# (HuxerUI#130 measured the margin at 45 bytes), and `outputs` is the one list
# an author cannot shorten: an output the program does not name cannot be
# built, and there is no depfile for outputs.
#
# This fixture declares one action whose serialised `inputs` and `outputs` each
# exceed the old bound by construction, and asserts that the whole declaration
# reaches the graph: `build.ninja` carries an edge with exactly N outputs and N
# inputs, N being this file's constant and not a number read back from the
# engine. The reverse leg -- the same fixture on 2026.9.12.4 is refused with
# the overflow diagnostic -- was run once before merge and is recorded in the
# pull request; CI cannot run two engines on one fixture.
#
# The command is the engine itself, which is present on every shard, exits 0
# and writes nothing, so the fixture runs everywhere the suite runs (no
# `# requires:` line, as 313 does). The outputs are therefore not produced;
# ninja does not fail on an unproduced output (313 states the measurement), and
# producing files is the command's business, covered by 188. What is under
# test here is the declaration channel.
#
# A second action carries a command past 16384 bytes on the hosts whose OS
# allows it. Windows is skipped for that leg only: ninja runs every command
# through `cmd /c` there, which caps a command line at 8191 characters, and
# that is the operating system's limit on the tool's own argv, not the
# engine's on the declaration (the design record's section 2.4).
set -e

source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

readonly N=200

mkdir -p app/src app/in
cd app

cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

cat > src/main.cpp <<'EOF'
#include <cstdio>
int main() { std::puts("LARGE_DECLARATION_OK"); }
EOF

# N input files, so that ninja finds every declared input on disk. Each path
# is padded to a fixed width with a long directory name, so the serialised
# list crosses the old bound regardless of where this fixture runs:
# N * (width + 3) > 8192 whenever width >= 40.
PAD="a-directory-name-long-enough-to-make-the-list-cross-the-old-bound"
mkdir -p "in/$PAD"
for i in $(seq 1 $N); do : > "in/$PAD/input-$i.txt"; done

ROOT_HOST=$(host_path "$PWD")
MCPP_HOST=$(host_path "$(cd "$(dirname "$MCPP")" && pwd)/$(basename "$MCPP")")

# The long-command leg: a program that accepts any argv and exits 0. Not on
# Windows (see the header).
TRUE_HOST=""
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) ;;
    *) TRUE_HOST=$(host_path "$(type -P true)") ;;
esac

cat > build.mcpp <<EOF
import std;
import mcpp;
int main() {
    const std::string root = "$ROOT_HOST";
    const std::string pad  = "$PAD";
    mcpp::action a;
    a.id          = "wide";
    a.role        = "source";
    a.description = "an action with $N inputs and $N outputs";
    a.arg("$MCPP_HOST").arg("--version");
    for (int i = 1; i <= $N; ++i) {
        a.input((root + "/in/" + pad + "/input-" + std::to_string(i) + ".txt").c_str());
        a.output((std::string(mcpp::out_dir()) + "/out/" + pad + "/output-" + std::to_string(i) + ".txt").c_str());
    }
    a.submit();

    const std::string t = "$TRUE_HOST";
    if (!t.empty()) {
        mcpp::action c;
        c.id          = "long-command";
        c.role        = "check";
        c.description = "a command past sixteen kilobytes";
        c.arg(t.c_str());
        // 300 arguments of 64 bytes: 19200 bytes of argv, past the old
        // 16384-byte bound of the command list.
        const std::string word(64, 'x');
        for (int i = 0; i < 300; ++i) c.arg(word.c_str());
        c.output((std::string(mcpp::out_dir()) + "/long-command.stamp").c_str());
        c.submit();
    }
}
EOF

"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "FAIL: a build with a wide action was refused"; exit 1; }
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == *LARGE_DECLARATION_OK* ]] || { echo "FAIL: the program did not run: '$out'"; exit 1; }

NINJA=$(find target -name build.ninja -print -quit)
[ -n "$NINJA" ] || { echo "FAIL: no build.ninja"; exit 1; }

# THE criterion: the edge in the graph names every declared output and every
# declared input. Counted against N, not against anything the engine reports.
# Only the action's own edge is read (`build <outputs>: mcpp_action_<k>
# <inputs>`): the engine also lists a source action's outputs a second time,
# as the inputs of the package's ordering phony, and counting the whole file
# would report 2N.
count_in_edge() {
    # $1 = the substring that identifies the file kind
    grep ': mcpp_action_' "$NINJA" | tr -s ' ' '\n' | grep -c "$1" || true
}
outs=$(count_in_edge "output-[0-9]*\.txt")
ins=$(count_in_edge "input-[0-9]*\.txt")
[ "$outs" = "$N" ] || { echo "FAIL: build.ninja names $outs of $N declared outputs"; exit 1; }
[ "$ins" = "$N" ] || { echo "FAIL: build.ninja names $ins of $N declared inputs"; exit 1; }
echo "ok: the edge carries $N outputs and $N inputs"

if [ -n "$TRUE_HOST" ]; then
    stamp=$(find target -name 'long-command.stamp' | wc -l | tr -d '[:space:]')
    [ "$stamp" = "1" ] || { cat b1.log; echo "FAIL: the long-command check did not run (stamps: $stamp)"; exit 1; }
    echo "ok: a command of 19200 bytes ran"
fi

# The overflow marker must be absent: the refusal path of the old bound is the
# only thing that ever wrote it, and the message it now carries is about
# allocation failure, which this build did not have.
if grep -q '"overflow":true' b1.log; then cat b1.log; echo "FAIL: the overflow marker appeared"; exit 1; fi

# Replay: an unrelated source is added, so the project fast path is off and
# the build program's cache record is replayed rather than re-run. The edge
# must come back complete from the record, not only from a live run.
cat > src/extra.cpp <<'EOF'
int extra() { return 1; }
EOF
"$MCPP" build > b2.log 2>&1 || { cat b2.log; echo "FAIL: rebuild after an unrelated source failed"; exit 1; }
NINJA=$(find target -name build.ninja -print -quit)
outs=$(count_in_edge "output-[0-9]*\.txt")
[ "$outs" = "$N" ] || { echo "FAIL: after a replay build.ninja names $outs of $N declared outputs"; exit 1; }
echo "ok: the replayed record carries all $N outputs"

echo "OK"
