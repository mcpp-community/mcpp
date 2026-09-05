#!/usr/bin/env bash
# requires: gcc
# 315_blocking_check_gates_compilation.sh — `blocking = true` on a check does
# what it has always been documented to do (mcpp#534).
#
# WHAT THIS DEFENDS. `blocking` was typed (`BuildAction::blocking`), emitted
# over the build-program protocol (`hostprogram.cppm`), parsed
# (`directives.cppm`), documented in both languages (`docs/07-build-mcpp.md`
# and its Chinese counterpart) and demonstrated in a shipped example
# (`examples/08-build-rules/rules-tidy`) — and read by nothing. The only
# order-only edge the ninja backend emitted was the staged-BMI one. So a check
# marked blocking ran alongside compilation exactly like a non-blocking one,
# and the flag was a no-op with a paper trail.
#
# ASSERTED ON THE ARTIFACT, NOT ON A LOG LINE. "the check failed" appears in
# the output either way; what distinguishes blocking from non-blocking is
# whether the object was PRODUCED. A grep over stderr would pass against the
# defect.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
mkdir -p "$TMP/app/src"
cd "$TMP/app"

cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
EOF
printf '#include <cstdio>\nint main(){ std::printf("ok\\n"); }\n' > src/main.cpp

# A check that fails, slowly. The sleep is what makes the difference
# observable: without the ordering edge the compile finishes long before the
# check does, so the object exists on disk when the build gives up.
cat > check.sh <<'EOF'
#!/usr/bin/env bash
sleep 2
echo "the check says no" >&2
exit 1
EOF
chmod +x check.sh

emit_build_mcpp() {   # $1 = "true" | "false"
    cat > build.mcpp <<EOF
#include <string>
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    mcpp::action a;
    a.id       = "gate";
    a.role     = "check";
    a.blocking = $1;
    a.arg((root + "/check.sh").c_str())
     .output("\${mcpp.out_dir}/gate.stamp")
     .submit();
}
EOF
}

# ── blocking = true: the object must NOT be produced ────────────────────────
emit_build_mcpp true
rm -rf target
if "$MCPP" build > b_blocking.log 2>&1; then
    cat b_blocking.log
    echo "FAIL: a failing blocking check let the build succeed"; exit 1
fi
if find target -name 'main*.o' | grep -q .; then
    echo "FAIL: a failing BLOCKING check did not stop the compile — the object exists:"
    find target -name 'main*.o'
    echo "      (this is the pre-fix behaviour: 'blocking' was parsed and never read)"
    exit 1
fi

# The edge that makes it a property rather than a race lost by the compiler.
nj=$(find target -name build.ninja | head -1)
[[ -n "$nj" ]] || { echo "FAIL: no build.ninja"; exit 1; }
grep -qE '^build mcpp-actions-app : phony.*gate\.stamp' "$nj" || {
    echo "FAIL: the blocking check's stamp is not in the package's action phony"
    grep -nE '^build mcpp-actions' "$nj" || echo "  (no phony at all)"
    exit 1; }
ordered=$(grep -cE '^build [^:]*main[^:]*\.o *:.*\|\|.*mcpp-actions-app' "$nj" || true)
[[ "$ordered" -ge 1 ]] || {
    echo "FAIL: the compile edge does not wait for the blocking check"; exit 1; }

# ── blocking = false: the CONTROL. Same check, same failure, compile runs ───
#
# Without this half, a build that refused to compile for any unrelated reason
# would pass the assertion above. What must differ between the two runs is
# exactly one line of build.mcpp.
emit_build_mcpp false
rm -rf target
"$MCPP" build > b_parallel.log 2>&1 || true   # still fails: the check still fails
if ! find target -name 'main*.o' | grep -q .; then
    cat b_parallel.log
    echo "FAIL: a NON-blocking check also stopped the compile — 'blocking' is"
    echo "      not distinguishing anything, it is just always on"
    exit 1
fi
nj2=$(find target -name build.ninja | head -1)
if grep -qE '^build mcpp-actions-app : phony.*gate\.stamp' "$nj2"; then
    echo "FAIL: a NON-blocking check joined the ordering phony"; exit 1
fi

echo "PASS: 315 (blocking gates the compile; non-blocking does not)"
