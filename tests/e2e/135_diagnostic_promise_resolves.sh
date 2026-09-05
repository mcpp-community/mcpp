#!/usr/bin/env bash
# requires: llvm unix-shell
# A diagnostic that prints a copy-pasteable dependency line must print one that
# resolves.
#
# THIS TEST EXISTS BECAUSE THE SAME DEFECT SHIPPED TWICE.
#
# The freestanding `import std;` message ends in a `[dependencies]` block the
# reader is meant to paste. It has been wrong twice, in two different ways:
#
#   1. it named `mcpplibs.std.freestanding` before any such package existed —
#      pasting it failed at the very next command with "package not found";
#   2. it named version `0.1.0` after `0.2.0` superseded it in the index —
#      pasting it produced `E_NOT_FOUND: package 'compat.std-freestanding@0.1.0'
#      not found in the synced index`.
#
# Both times the repair was to edit the literal and add a comment saying the
# literal must be kept current. A comment cannot enforce a cross-repository
# invariant — this repository already learned that for its version pins, which
# is why `check_version_pins.sh` exists — and the second occurrence is the
# proof: the comment from the first repair was sitting right above the line
# that broke.
#
# THE TEST MUST NOT SPELL THE VERSION. Asserting `std-freestanding = "0.3.0"`
# would copy the literal into a second place and check that the two copies
# agree, which is true even when both are wrong. What is checked instead is the
# property that matters: whatever the diagnostic prints, RESOLVES.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new promise > /dev/null
cd promise
rm -f tests/*.cpp 2>/dev/null || true

cat > mcpp.toml <<'EOF'
[package]
name    = "promise"
version = "0.1.0"

[build]
target = "riscv64-none-elf"
EOF

# `import std;` on a freestanding target is the trigger.
cat > src/main.cpp <<'EOF'
import std;
extern "C" int main() { return 0; }
EOF

if "$MCPP" build > diag.log 2>&1; then
    cat diag.log
    echo "\`import std;\` on a freestanding target should have been rejected"
    exit 1
fi
grep -q 'is not available on' diag.log || {
    cat diag.log; echo "the freestanding import-std diagnostic did not appear"; exit 1; }

# ── Extract the advice, rather than restating it ─────────────────────────────
# The line is indented inside the message; take it verbatim, minus the padding.
ADVICE=$(grep -oE '[a-z0-9.-]+[[:space:]]*=[[:space:]]*"[^"]+"' diag.log | head -1)
[[ -n "$ADVICE" ]] || {
    cat diag.log
    echo "the diagnostic printed no dependency line to paste — if the advice was"
    echo "removed on purpose, remove this test with it"
    exit 1; }
echo "advice: $ADVICE"

# ── Paste it, exactly as a reader would ─────────────────────────────────────
cat > mcpp.toml <<EOF
[package]
name    = "promise"
version = "0.1.0"

[build]
target = "riscv64-none-elf"

[dependencies]
$ADVICE
EOF

cat > src/main.cpp <<'EOF'
import mcpplibs.std.freestanding;
extern "C" int main() { return static_cast<int>(std::array<int, 3>{}.size()); }
EOF

# The build is expected to get as far as the LINK. It may well fail there — this
# project has no board package, so there is no crt0 and nothing to start it —
# and that failure is not what this test is about. What must not happen is a
# RESOLUTION failure: that is what a stale name or a stale version produces, and
# it is what both historical occurrences looked like.
"$MCPP" build > paste.log 2>&1 || true

if grep -qE 'not found in the synced index|package not found|E_NOT_FOUND' paste.log; then
    cat paste.log
    echo
    echo "the diagnostic's copy-pasteable line does not resolve."
    echo "advice was: $ADVICE"
    echo "update the literal in prepare.cppm's freestanding import-std message"
    echo "to a version the index actually carries, in the same change that"
    echo "publishes it."
    exit 1
fi

# ...and the package must actually have entered the graph, so that a build which
# silently ignored the dependency cannot pass. One of the three verbs mcpp uses
# for a dependency it resolved has to name it.
grep -qE '(Downloading|Compiling|Cached).*std-freestanding' paste.log || {
    cat paste.log
    echo "the pasted dependency never entered the build graph"
    exit 1; }

echo "PASS: the import-std diagnostic's advice resolves and enters the graph"
