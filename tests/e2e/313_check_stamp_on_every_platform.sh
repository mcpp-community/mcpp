#!/usr/bin/env bash
# 313_check_stamp_on_every_platform.sh — a `role = "check"` whose command never
# touches its stamp, on EVERY platform.
#
# ⚠️ WHY THIS EXISTS SEPARATELY FROM 188 AND 312.
#
# The engine writes a check's stamp so that an analyser -- which answers with an
# exit code and writes nothing -- can be a check without a wrapper script. The
# whole justification for doing it in the engine rather than in a script is that
# an action's command is an argv with no shell assumed, so the wrapper cannot be
# written portably: there is no `touch` on Windows.
#
# And every existing test of that mechanism declares `# requires: gcc`, so all
# of them SKIP on Windows. The feature's reason to exist was verified nowhere
# near the platform it exists for. This file carries no `# requires:` line at
# all, so it runs wherever the suite runs.
#
# To do that it must avoid a POSIX shell entirely, which the new mechanism makes
# possible: the check's command is `mcpp --version` -- an executable that is
# present by construction, exits 0, and writes no stamp.
set -e

source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p src
cat > mcpp.toml <<'EOF'
[package]
name    = "stampprobe"
version = "0.1.0"
EOF
cat > src/main.cpp <<'EOF'
#include <cstdio>
#ifndef CHECK_DECLARED
#error "the build program never ran"
#endif
int main() { std::printf("STAMP_PROBE_OK\n"); }
EOF

MCPP_HOST="$(host_path "$MCPP")"
cat > build.mcpp <<EOF
import std;
import mcpp;
int main() {
    mcpp::define("CHECK_DECLARED");
    mcpp::action a;
    a.id          = "probe";
    a.role        = "check";
    a.description = "a check whose command writes no stamp";
    // The engine itself: present on every platform this suite runs on, exits 0,
    // and touches nothing. That is exactly the shape of a real analyser.
    a.arg("$MCPP_HOST").arg("--version")
     .output((std::string(mcpp::out_dir()) + "/probe.stamp").c_str())
     .submit();
}
EOF

"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "FAIL: build failed"; exit 1; }
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == *STAMP_PROBE_OK* ]] || {
    echo "FAIL: the program did not run: '$out'"; exit 1; }

# THE criterion. Not the build's exit code: measured, ninja does not fail when a
# declared output goes unproduced -- it leaves the file absent and re-runs that
# edge on every build afterwards, so a build with no stamp mechanism at all
# stays green and merely redoes work.
mapfile -t stamps < <(find target -name 'probe.stamp')
(( ${#stamps[@]} == 1 )) || {
    printf '  %s\n' "${stamps[@]}"
    echo "FAIL: expected exactly one stamp written by the engine, found ${#stamps[@]}"
    exit 1; }

# And the other half: a satisfied edge is not re-run. Without the stamp this
# would repeat forever, which is the symptom the exit code hides.
"$MCPP" build > b2.log 2>&1 || { cat b2.log; echo "FAIL: no-op rebuild failed"; exit 1; }
if grep -q 'CHECK' b2.log; then
    cat b2.log
    echo "FAIL: the check re-ran with nothing changed — its stamp is not satisfying the edge"
    exit 1
fi

echo "OK"
