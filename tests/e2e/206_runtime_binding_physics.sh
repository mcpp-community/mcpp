#!/usr/bin/env bash
# requires: elf gcc
# 206_runtime_binding_physics.sh — a selected RuntimeBinding must become the
# physical PT_INTERP/libc pair, and a host DSO whose GLIBC floor is satisfiable
# must remain a supported control. Proven mismatches fail at link completion;
# hot no-ops neither relink nor rewrite/re-probe the stored verdict.
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
[[ $(jq -r '.artifacts["bin/runtime-physics-safe"].status' "$verdict") == pass ]] \
    || { cat "$verdict"; fail "safe host-DSO closure did not pass"; }

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
grep -q 'runtime-physics-safe.*pass' <<<"$doctor_out" \
    || { echo "$doctor_out"; fail "doctor did not reuse passing verdict"; }

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
