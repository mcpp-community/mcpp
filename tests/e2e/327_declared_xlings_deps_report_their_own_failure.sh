#!/usr/bin/env bash
# 327_declared_xlings_deps_report_their_own_failure.sh — #540.
#
# #531 made `[xlings] deps` provision on first build. The call site tested
# `if (!r)` on `xlings::call`, which returns `expected<CallResult, string>` and
# is in the VALUE state whenever the child ran at all — a capability's own
# status arrives INSIDE CallResult, because xlings exits 0 once it has spoken
# the NDJSON protocol. So every failure xlings can report was read as success:
#
#     $ mcpp build          # deps = ["definitely-not-a-real-package"]
#     Provisioning [xlings] deps (definitely-not-a-real-package)
#         Finished dev [unoptimized + debuginfo] in 0.12s
#     $ cat .mcpp/.xlings-deps.stamp
#     definitely-not-a-real-package                 ← recorded as DONE
#
# #531's own comment says the defect it fixed was "the declaration looked
# accepted and did nothing — the worst shape a config key can have". Unread,
# the fix reproduced that shape and the stamp made it permanent.
#
# Two properties, and the second is the one a single run cannot show:
#   (1) a provisioning that fails, fails the build;
#   (2) it is NOT recorded, so the NEXT build tries again instead of
#       reporting success.
#
# Plus the auto-install gate `[toolchain]` has always had and this path did
# not — `MCPP_OFFLINE` and `MCPP_NO_AUTO_INSTALL`, naming the knob that fired.
# Those two legs need no network and no index, which is why they run first.
#
# requires: unix-shell
set -e

MCPP="${MCPP:-mcpp}"
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p proj/src
cat > proj/mcpp.toml <<'EOF'
[package]
name    = "provproj"
version = "0.1.0"

[xlings]
deps = ["definitely-not-a-real-package-540"]
EOF
echo 'int main() { return 0; }' > proj/src/main.cpp
cd proj

# ── (1) MCPP_NO_AUTO_INSTALL: refuse, and name THAT knob ────────────────────
# The knob that fired, not a fixed one. Telling someone who exported
# MCPP_NO_AUTO_INSTALL to drop --offline sends them looking for a variable they
# never set; the toolchain gate solved this years ago and this path is copying
# its shape, so the test holds the shape too.
set +e
MCPP_NO_AUTO_INSTALL=1 "$MCPP" build > noauto.log 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || { cat noauto.log; echo "FAIL: MCPP_NO_AUTO_INSTALL did not refuse"; exit 1; }
grep -q "MCPP_NO_AUTO_INSTALL" noauto.log || {
    cat noauto.log; echo "FAIL: the refusal does not name MCPP_NO_AUTO_INSTALL"; exit 1; }
grep -q "definitely-not-a-real-package-540" noauto.log || {
    cat noauto.log; echo "FAIL: the refusal does not name the package to install"; exit 1; }
grep -q "Provisioning" noauto.log && {
    cat noauto.log; echo "FAIL: it announced provisioning and then refused"; exit 1; }
echo "  ok  MCPP_NO_AUTO_INSTALL refuses, names the knob and the package"

# ── (2) MCPP_OFFLINE: same refusal, the other knob ──────────────────────────
set +e
MCPP_OFFLINE=1 "$MCPP" build > offline.log 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || { cat offline.log; echo "FAIL: MCPP_OFFLINE did not refuse"; exit 1; }
grep -q "MCPP_OFFLINE\|--offline" offline.log || {
    cat offline.log; echo "FAIL: the refusal does not name the offline knob"; exit 1; }
echo "  ok  MCPP_OFFLINE refuses and names the offline knob"

# ── (3) A declared package that cannot be provisioned fails the build ───────
# …and, the half a single run cannot show, is not recorded as done.
#
# THE DENOMINATOR IS TWO INVOCATIONS. The stamp is what made the original
# defect permanent, and a stamp written for a failure is invisible in the run
# that wrote it — the build that mattered already reported success. Only the
# SECOND build distinguishes "failed" from "failed and remembered as done".
#
# Asserted on behaviour rather than on the stamp's path: where the record lives
# is an implementation detail that has already moved once (project → registry),
# and a test keyed to the path would have passed through the move while the
# property it exists for was broken.
set +e
"$MCPP" build > first.log 2>&1
rc1=$?
"$MCPP" build > second.log 2>&1
rc2=$?
set -e
[ "$rc1" -ne 0 ] || { cat first.log; echo "FAIL: an unprovisionable [xlings] dep built successfully"; exit 1; }
[ "$rc2" -ne 0 ] || {
    cat second.log
    echo "FAIL: the SECOND build succeeded — the failed provisioning was recorded as done"
    exit 1; }
for f in first.log second.log; do
    grep -q "definitely-not-a-real-package-540" "$f" || {
        cat "$f"; echo "FAIL: $f does not name the package that could not be provisioned"; exit 1; }
done
echo "  ok  a failed provisioning fails the build, twice — it is not stamped as done"

# ── (4) relocating the record must not refuse a build that worked before ────
# The stamp moved from `<project>/.mcpp/.xlings-deps.stamp` into the registry,
# because it records a GLOBAL effect and lived in the project. Every project
# that had already provisioned therefore reads as un-provisioned on its first
# build after upgrading. Online that costs one round trip and self-corrects;
# with auto-install off, leg (1) above would refuse a build that worked
# yesterday, for packages that are in fact installed.
#
# THE LEGACY STAMP IS EVIDENCE ONLY HERE. The release that wrote it did not
# read the provisioning result, so it means "attempted", not "succeeded" —
# trusting it anywhere else would carry that defect across the upgrade that
# fixes it. This leg pins the one place it is allowed to count.
cd "$TMP"
mkdir -p legacy/src/.keep && rm -rf legacy/src/.keep
mkdir -p legacy/src legacy/.mcpp
cat > legacy/mcpp.toml <<'EOF'
[package]
name    = "legacyproj"
version = "0.1.0"

[xlings]
deps = ["definitely-not-a-real-package-540"]
EOF
echo 'int main() { return 0; }' > legacy/src/main.cpp
printf 'definitely-not-a-real-package-540\n' > legacy/.mcpp/.xlings-deps.stamp
cd legacy
set +e
MCPP_NO_AUTO_INSTALL=1 "$MCPP" build > legacy.log 2>&1
rc=$?
set -e
[ "$rc" -eq 0 ] || {
    cat legacy.log
    echo "FAIL: moving the stamp refused a build that a pre-upgrade mcpp allowed"
    exit 1; }
echo "  ok  a pre-upgrade stamp keeps an auto-install-off build working"

# …and it is NOT promoted into the registry: nothing verified anything, so the
# next build with the network available must still check.
cd "$TMP/proj"
set +e
MCPP_NO_AUTO_INSTALL=1 "$MCPP" build > again.log 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || {
    cat again.log
    echo "FAIL: a project with no legacy stamp was let through by another project's"
    exit 1; }
echo "  ok  the legacy stamp is not promoted, and does not leak between projects"

echo "PASS: 327 declared [xlings] deps report their own failure"
