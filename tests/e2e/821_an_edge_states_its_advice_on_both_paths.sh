#!/usr/bin/env bash
# requires: unix-shell
# 821_an_edge_states_its_advice_on_both_paths.sh -- SPEC-007 R4.5 (the
# 2026-09-28 design, WS3). A build edge that has something to say on success
# writes it to its advice file under the build directory, and mcpp reports it
# once after a successful build, by one function that the full build path and
# the fast path both call. Before 2026.9.28.2 an edge's output was shown only
# when the build failed or under `-v`, so what a successful edge had to say
# reached nobody; and a report attached to one of the two paths appears or not
# depending on whether build.ninja happened to be current.
#
# The edge here is a `check` action that states a note naming its input's
# contents. `place-dlls` is the engine's own writer of this channel, and it
# runs only for PE programs; the rule is the edge's, not the tool's.
#
#   A1  the full path reports the advice once, without -v;
#   A2  after the action's input changes, the build takes the fast path (on
#       Linux; it serves only ELF products, #400), the edge runs again, and
#       the advice is reported once, with the new text;
#   A3  a build in which the edge does not run reports nothing.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

mkdir -p p/src p/data
cd p
cat > mcpp.toml <<'EOF'
[package]
name    = "advice821"
version = "0.1.0"
EOF
printf 'int main() { return 0; }\n' > src/main.cpp
printf 'first\n' > data/probe.in
cat > build.mcpp <<'EOF'
import std;
import mcpp;
int main() {
    const std::string in  = std::string(mcpp::manifest_dir()) + "/data/probe.in";
    const std::string out = std::string(mcpp::out_dir()) + "/probe.stamp";
    mcpp::action a;
    a.id   = "advise";
    a.role = mcpp::roles::check;
    // The advice file of this edge, in the build directory the edge runs in.
    a.arg("sh").arg("-c")
     .arg("mkdir -p .mcpp-advice && printf 'note\\tprobe input reads %s\\n' \"$(cat \"$1\")\" > .mcpp-advice/probe.stamp.advice")
     .arg("sh").arg(in.c_str())
     .input(in.c_str())
     .output(out.c_str())
     .submit();
    return 0;
}
EOF

"$MCPP" build > b1.log 2>&1 || fail "the first build failed" b1.log
[[ "$(grep -c "probe input reads first" b1.log)" -eq 1 ]] \
    || fail "A1: the full path did not report the edge's advice exactly once" b1.log
echo "ok: A1 the full path reports the advice once"

# Whether the last build planned: the full path writes build.ninja (or moves
# its time when the text is unchanged), and the fast path replays it untouched.
# A `Compiling` line no longer tells the two apart: it states that a package's
# steps ran, on either path (build progress design 2026-09-29).
planned_since() { find target -name build.ninja -not -path '*/.build-mcpp/*' -newer "$1" 2>/dev/null | grep -q .; }
touch .before-b2
sleep 1   # a coarse file system clock must see the input as newer
printf 'second\n' > data/probe.in
"$MCPP" build > b2.log 2>&1 || fail "the second build failed" b2.log
# The fast path serves only ELF products (#400): on Linux the second build
# must take it, or A2 tests nothing new; elsewhere it declines, and A2 reads
# the full path again.
if planned_since .before-b2; then
    case "$(uname -s)" in
        Linux) fail "A2: the second build planned again, so the fast path was not exercised" b2.log ;;
        *)     echo "READING 821: the fast path declines on $(uname -s) (#400); A2 reads the full path" ;;
    esac
fi
[[ "$(grep -c "probe input reads second" b2.log)" -eq 1 ]] \
    || fail "A2: the second build did not report the edge's advice exactly once" b2.log
if grep -q "probe input reads first" b2.log; then
    fail "A2: the second build reported the previous run's advice" b2.log
fi
echo "ok: A2 the second build reports the advice of the edge that ran"

"$MCPP" build > b3.log 2>&1 || fail "the third build failed" b3.log
if grep -q "probe input reads" b3.log; then
    fail "A3: a build in which the edge did not run reported its advice" b3.log
fi
echo "ok: A3 a build without the edge reports nothing"

echo "PASS: 821_an_edge_states_its_advice_on_both_paths"
