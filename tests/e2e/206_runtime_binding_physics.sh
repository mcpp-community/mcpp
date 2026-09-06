#!/usr/bin/env bash
# requires: elf gcc
# 206_runtime_binding_physics.sh — a selected RuntimeBinding must become the
# physical PT_INTERP/libc pair, and a host DSO linked under `allow_host_libs`
# must still BUILD (the declaration is honoured) while being reported honestly
# (the private loader does not consult /usr/lib). Proven mismatches fail at link
# completion; hot no-ops neither relink nor rewrite/re-probe the stored verdict.
set -euo pipefail

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

command -v readelf >/dev/null || fail "readelf is required"
command -v jq >/dev/null || fail "jq is required"
tinfo_link=$(find /lib /usr/lib -name 'libtinfo.so' -print -quit 2>/dev/null)
[[ -n "$tinfo_link" ]] || fail "host libtinfo development symlink is required"
tinfo_dir=$(dirname "$tinfo_link")

mkdir -p "$TMP/safe/src"
cat >"$TMP/safe/mcpp.toml" <<EOF
[package]
name = "runtime-physics-safe"
version = "0.1.0"

[build]
allow_host_libs = true
ldflags = ["-L$tinfo_dir", "-ltinfo"]
EOF
cat >"$TMP/safe/src/main.cpp" <<'EOF'
extern "C" int setupterm(const char*, int, int*);
int main() {
    int error = 0;
    // Keep libtinfo in DT_NEEDED; execution is not part of this link-physics
    // control because TERM/tty availability is unrelated to the closure.
    return setupterm(nullptr, 1, &error) == -99;
}
EOF

cd "$TMP/safe"
safe_out=$($MCPP build 2>&1) || { echo "$safe_out"; fail "safe host-DSO build"; }
output_dir=$(sed -n '2p' target/.build_cache)
[[ -d "$output_dir" ]] || fail "build cache did not name output directory"
artifact="$output_dir/bin/runtime-physics-safe"
verdict="$output_dir/.mcpp-runtime-verdicts.json"
[[ -x "$artifact" && -f "$verdict" ]] || fail "artifact/verdict missing"
readelf -d "$artifact" | grep -q 'libtinfo\.so' \
    || fail "safe control did not retain host libtinfo in DT_NEEDED"

# INCONCLUSIVE, not pass — and the change from `pass` is a correction, not a
# relaxation.
#
# This artifact links a HOST libtinfo while carrying a PRIVATE PT_INTERP, and
# measured with LD_DEBUG=libs the private loader's built-in default path is the
# glibc payload's own build-time prefix (`…/fromsource-x-glibc/2.39/lib`) — a
# directory that does not exist on this machine. /usr/lib is never consulted,
# and the binary exits 127 with "libtinfo.so.6: cannot open shared object file".
#
# It used to report `pass` because the closure model fell back to the HOST's
# default directories, where libtinfo of course is. The model was describing a
# loader the artifact does not use — the same defect that shipped a GL program
# as `validation: pass` while it exited 127. This file's own header note
# ("execution is not part of this link-physics control") is what let the false
# green stand: nothing ever ran it.
#
# `allow_host_libs` is still honoured, and that is the point of the state: the
# user declared they are reaching outside the sandbox, so mcpp REPORTS rather
# than blocks — resolution at run time (LD_LIBRARY_PATH, or installing where the
# private loader looks) is theirs to arrange.
# WHICH OF THE TWO VERDICTS IS CORRECT IS DECIDED BY THE MACHINE, SO THE TEST
# MEASURES IT RATHER THAN ASSUMING IT.
#
# The paragraph above says `inconclusive` because the private loader cannot
# reach a host `libtinfo`. That is true on a machine whose runtime search path
# does not contain one -- and false on a machine where it does. `xim:ncurses`
# is an ordinary ecosystem package, and a sub-OS that has it links
# `libtinfo.so.6` into the library view that IS on the artifact's RPATH. There
# the closure genuinely closes and `pass` is the right answer; asserting
# `inconclusive` would report a defect that is not there. (Measured
# 2026-09-06: green on every CI runner, red on a developer machine with
# ncurses provisioned. An earlier note in this file blamed a stale glibc
# payload, which was the wrong object.)
#
# So: read the artifact's own runtime search path, decide from it which verdict
# the model owes, and assert THAT. The test is then total -- it fails if the
# model says `pass` where nothing can be found, and equally if it says
# `inconclusive` where the library is sitting on the path.
soname=libtinfo.so.6
search=$(readelf -d "$artifact" | sed -n 's/.*R\(UN\)\?PATH.*\[\(.*\)\]/\2/p' | tr ':' '\n')
reachable=no
while read -r dir; do
    [[ -n "$dir" ]] || continue
    [[ -e "$dir/$soname" ]] && { reachable=yes; found="$dir/$soname"; break; }
done <<<"$search"
echo "runtime search path of the artifact:"; sed 's/^/    /' <<<"$search"
echo "$soname reachable from it: $reachable${found:+ ($found)}"

status=$(jq -r '.artifacts["bin/runtime-physics-safe"].status' "$verdict")
if [[ "$reachable" == yes ]]; then
    # The declared host library is on the private loader's path after all.
    [[ "$status" == pass ]] \
        || { cat "$verdict"; fail "$soname is at $found, so the closure closes; the model says '$status'"; }
else
    # `allow_host_libs` is still honoured, and that is the point of the state:
    # the user declared they are reaching outside the sandbox, so mcpp REPORTS
    # rather than blocks -- resolution at run time (LD_LIBRARY_PATH, or
    # installing where the private loader looks) is theirs to arrange.
    [[ "$status" == inconclusive ]] \
        || { cat "$verdict"; fail "nothing on the search path provides $soname; the model says '$status'"; }
    jq -r '.artifacts["bin/runtime-physics-safe"].diagnostics[]' "$verdict" \
        | grep -q 'libtinfo\.so\.6' \
        || { cat "$verdict"; fail "the verdict does not name the library it could not resolve"; }
    jq -r '.artifacts["bin/runtime-physics-safe"].diagnostics[]' "$verdict" \
        | grep -q 'allow_host_libs' \
        || { cat "$verdict"; fail "the verdict does not say why it stopped short of a proof"; }
fi

artifact_before=$(stat -c '%y:%s' "$artifact")
verdict_before=$(stat -c '%y:%s' "$verdict")
noop_out=$($MCPP build 2>&1) || { echo "$noop_out"; fail "hot no-op build"; }
[[ $(stat -c '%y:%s' "$artifact") == "$artifact_before" ]] \
    || fail "hot no-op relinked artifact"
[[ $(stat -c '%y:%s' "$verdict") == "$verdict_before" ]] \
    || fail "hot no-op rewrote/re-probed verdict"

doctor_out=$($MCPP self doctor 2>&1 || true)
grep -q 'last runtime closure verdict' <<<"$doctor_out" \
    || { echo "$doctor_out"; fail "doctor omitted stored runtime verdict"; }
grep -q "runtime-physics-safe.*$status" <<<"$doctor_out" \
    || { echo "$doctor_out"; fail "doctor did not reuse the stored verdict ('$status')"; }

# Force form-X into a proven Rule-B mismatch: mcpp still supplies the selected
# private libc RUNPATH, while the final user flag replaces PT_INTERP with the
# host loader. The linker accepts this shape; mcpp must reject it before the
# user reaches a pre-main GLIBC_PRIVATE/version crash.
mkdir -p "$TMP/mismatch/src"
cat >"$TMP/mismatch/mcpp.toml" <<'EOF'
[package]
name = "runtime-physics-mismatch"
version = "0.1.0"

[build]
allow_host_libs = true
ldflags = ["-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2"]
EOF
echo 'int main() { return 0; }' >"$TMP/mismatch/src/main.cpp"
cd "$TMP/mismatch"
set +e
mismatch_out=$($MCPP build 2>&1)
mismatch_rc=$?
set -e
[[ $mismatch_rc -ne 0 ]] || { echo "$mismatch_out"; fail "Rule-B mismatch passed"; }
grep -q 'runtime closure validation failed' <<<"$mismatch_out" \
    || { echo "$mismatch_out"; fail "missing runtime validation headline"; }
grep -q 'rule B' <<<"$mismatch_out" \
    || { echo "$mismatch_out"; fail "missing Rule-B explanation"; }
grep -q 'ld-linux-x86-64.so.2' <<<"$mismatch_out" \
    || { echo "$mismatch_out"; fail "diagnostic omitted actual interpreter"; }
grep -q 'mcpp.toml' <<<"$mismatch_out" \
    || { echo "$mismatch_out"; fail "diagnostic omitted copyable remediation"; }

# A mismatch is not forgiven just because the second ninja drive is a no-op.
# Reuse the cached verdict without reparsing, but preserve the hard failure.
mismatch_verdict=$(find target -name '.mcpp-runtime-verdicts.json' -print -quit)
[[ -f "$mismatch_verdict" ]] || fail "mismatch verdict was not persisted"
mismatch_before=$(stat -c '%y:%s' "$mismatch_verdict")
set +e
mismatch_again=$($MCPP build 2>&1)
mismatch_again_rc=$?
set -e
[[ $mismatch_again_rc -ne 0 ]] \
    || { echo "$mismatch_again"; fail "cached Rule-B mismatch became green"; }
grep -q 'rule B' <<<"$mismatch_again" \
    || { echo "$mismatch_again"; fail "cached mismatch lost its explanation"; }
[[ $(stat -c '%y:%s' "$mismatch_verdict") == "$mismatch_before" ]] \
    || fail "cached mismatch was reparsed/rewritten"

echo "PASS: RuntimeBinding payload physics, host-DSO control and hot verdict cache"
