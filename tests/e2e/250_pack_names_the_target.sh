#!/usr/bin/env bash
# requires:
# (no capability: a library package is claimed to work on every target, so this
#  test has to RUN on every platform. `# requires: gcc` would have skipped it on
#  macOS and Windows — Apple Clang is not the gcc capability — leaving the claim
#  unverified while the suite stayed green.)
# 250_pack_names_the_target.sh — `mcpp pack <name>` packs the target it was
# given, and refuses a name it cannot pack.
#
# The positional is what decides application-bundle vs library-package, so it
# has to reach BOTH pipelines. It did not reach the application one at first:
# a project with two `bin` targets accepted `mcpp pack app2` and bundled app1,
# which is the shape where the command succeeds and the answer is wrong.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p two/src
cat > two/src/alpha.cpp <<'EOF'
#include <cstdio>
int main() { std::printf("alpha\n"); return 0; }
EOF
cat > two/src/beta.cpp <<'EOF'
#include <cstdio>
int main() { std::printf("beta\n"); return 0; }
EOF
cat > two/mcpp.toml <<'EOF'
[package]
name    = "two"
version = "0.1.0"

[targets.alpha]
kind = "bin"
main = "src/alpha.cpp"

[targets.beta]
kind = "bin"
main = "src/beta.cpp"
EOF

cd two

# ── Mach-O: which target was CHOSEN is still checkable ─────────────────
#
# A program bundle cannot be produced on Mach-O (266), so "look inside the
# staging dir" is unavailable here. The refusal names the artifact it got to,
# which answers the same question: `pack beta` must reach beta and `pack alpha`
# must reach alpha. Without this branch the file would simply not run on macOS,
# and the defect it exists for — `mcpp pack app2` bundling app1 — is not a
# platform-specific one.
if [[ "$(uname -s)" == "Darwin" ]]; then
    for want in beta alpha; do
        other=$([[ "$want" == beta ]] && echo alpha || echo beta)
        if "$MCPP" pack "$want" --mode system > "$want.log" 2>&1; then
            cat "$want.log"; echo "FAIL: a Mach-O program bundle was produced"; exit 1
        fi
        grep -q "Mach-O program '$want'" "$want.log" || {
            cat "$want.log"
            echo "FAIL: 'mcpp pack $want' did not reach $want"; exit 1; }
        grep -q "Mach-O program '$other'" "$want.log" && {
            cat "$want.log"
            echo "FAIL: 'mcpp pack $want' reached $other instead"; exit 1; }
    done
    # An unknown name must still fail EARLIER and differently — otherwise the
    # two assertions above would pass for a build that refuses everything.
    if "$MCPP" pack nosuch --mode system > bad.log 2>&1; then
        cat bad.log; echo "FAIL: an unknown target name was accepted"; exit 1
    fi
    grep -q "no target named 'nosuch'" bad.log || {
        cat bad.log; echo "wrong refusal for an unknown name"; exit 1; }
    echo "PASS: mcpp pack reaches the target it is given"
    exit 0
fi

# ── the named program is the one that gets bundled ─────────────────────
"$MCPP" pack beta --mode system > beta.log 2>&1 || { cat beta.log; echo "pack beta failed"; exit 1; }
staged="$(find target/dist -maxdepth 1 -type d -name 'two-0.1.0*' | head -1)"
[[ -n "$staged" ]] || { cat beta.log; echo "no staging dir"; exit 1; }
find "$staged" -type f -name 'beta*' | grep -q . || {
    echo "FAIL: 'mcpp pack beta' did not bundle beta"; find "$staged" -type f; exit 1; }
find "$staged" -type f -name 'alpha*' | grep -q . && {
    echo "FAIL: 'mcpp pack beta' bundled alpha instead"; find "$staged" -type f; exit 1; }

# ── and the other one, to prove the first result was not the default ───
rm -rf target/dist
"$MCPP" pack alpha --mode system > alpha.log 2>&1 || { cat alpha.log; echo "pack alpha failed"; exit 1; }
staged="$(find target/dist -maxdepth 1 -type d -name 'two-0.1.0*' | head -1)"
find "$staged" -type f -name 'alpha*' | grep -q . || {
    echo "FAIL: 'mcpp pack alpha' did not bundle alpha"; find "$staged" -type f; exit 1; }

# ── an unknown name is refused, and says what there is ─────────────────
if "$MCPP" pack nosuch --mode system > bad.log 2>&1; then
    cat bad.log; echo "FAIL: an unknown target name was accepted"; exit 1
fi
grep -q "no target named 'nosuch'" bad.log || { cat bad.log; echo "wrong refusal"; exit 1; }
grep -q 'alpha' bad.log && grep -q 'beta' bad.log || {
    cat bad.log; echo "the refusal did not list the available targets"; exit 1; }

echo "PASS: mcpp pack packs the target it is given"
