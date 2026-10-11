#!/usr/bin/env bash
# requires: gcc
# 218_module_extensions_graph_shape.sh — a declared module extension has to
# behave like a module interface to the two mechanisms that decide WHETHER TO
# REBUILD, not just to the compiler.
#
#  * Part 1 — the freshness fast path must sweep it.
#
#    `sources_newer_than` asks "could the SHAPE of the graph have changed",
#    which is a different question from "did a file change" (ninja answers
#    that one). Its extension list used to be hand-written and did not include
#    `.ixx`, so adding an `import` to one changed nothing it could see: the
#    fast path replayed a stale graph, ninja recompiled the object because its
#    mtime moved, the dyndep edges stayed as they were, and NOTHING reported
#    anything. That is the worst failure mode in this area — silent, and it
#    surfaces later as an unrelated BMI error.
#
#    The edit below adds a real `import`. Do NOT reduce it to `touch`: an
#    mtime-only change is exactly what a correct implementation is also
#    allowed to ignore, so a touch-based test can pass with the bug present.
#    And do NOT delete artifacts to force a rebuild: ninja then fails, the
#    failure is read as a stale-graph signature, and the fast path falls back
#    to a full prepare for the wrong reason — the assertion below would hold
#    while proving nothing.
#
#  * Part 2 — the key must reach the graph.
#
#    `module_extensions` decides which units emit a BMI and which objects link
#    unconditionally. It is an attribute of the package that declares it, not
#    of the configuration (workspace design 2026-09-29 §3): the build
#    directory stays, and the graph written into it must classify the files
#    the key now names. A `.ccm` file the key starts naming becomes a module
#    interface with its own BMI edge, in the same directory.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
mkdir -p src

cat > mcpp.toml <<'EOF'
[package]
name    = "gshape"
version = "0.1.0"

[build]
module_extensions = [".ixx"]
EOF

printf 'export module gshape.helper;\nimport std;\nexport auto helper() -> int { return 41; }\n' > src/helper.cppm
printf 'export module gshape.face;\nimport std;\nexport auto face() -> int { return 1; }\n'      > src/face.ixx
printf 'import std;\nimport gshape.face;\nint main(){ std::println("{}", face()); }\n'           > src/main.cpp

# Ask mcpp for the fingerprint instead of inferring it from the build tree.
#
# The obvious `find target -name build.ninja -not -path '*/.build-mcpp/*' | head -1` is WRONG here and was
# flaky in exactly the way this test is meant to catch: after part 2 there are
# TWO output dirs, and `head -1` picks whichever `find` happened to walk first.
# It passed standalone and failed inside the suite. Assert on the value, not on
# a directory listing.
fingerprint() {
    "$MCPP" build --print-fingerprint 2>&1 | sed -n 's/^Fingerprint: //p' | head -1
}

# ── 0. First build, then confirm the fast path actually engages ────────────
"$MCPP" build > b0.log 2>&1 || { cat b0.log; echo "FAIL: first build"; exit 1; }
FP_BEFORE="$(fingerprint)"
[ -n "$FP_BEFORE" ] || { echo "FAIL: could not read the fingerprint"; exit 1; }

# Whether the last build planned: the full path writes build.ninja (or moves
# its time when the text is unchanged), and the fast path replays it untouched.
# A `Compiling` line no longer tells the two apart: it states that a package's
# steps ran, on either path (build progress design 2026-09-29).
planned_since() { find target -name build.ninja -not -path '*/.build-mcpp/*' -newer "$1" 2>/dev/null | grep -q .; }
touch .before-b1; sleep 1
"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "FAIL: no-change build"; exit 1; }
planned_since .before-b1 && {
    cat b1.log; echo "FAIL: fast path did not engage — the rest proves nothing"; exit 1; }
echo "  ok: fast path engages on a no-change build"

# ── 1. A NEW import inside the .ixx must invalidate the graph ──────────────
touch .before-b2; sleep 1
printf 'export module gshape.face;\nimport std;\nimport gshape.helper;\nexport auto face() -> int { return helper() + 1; }\n' > src/face.ixx

"$MCPP" build > b2.log 2>&1 || { cat b2.log; echo "FAIL: build after .ixx edit"; exit 1; }
planned_since .before-b2 || {
    cat b2.log
    echo "FAIL: editing a .ixx did not invalidate the fast path"
    echo "      (the freshness sweep is not classifying it as a graph-shape input)"
    exit 1; }
echo "  ok: a new import inside a .ixx forces a full prepare"

out="$("$MCPP" run 2>&1)"
[[ "$out" == *"42"* ]] || { echo "FAIL: expected 42 (41+1), got: $out"; exit 1; }
echo "  ok: the new dependency edge is real (41+1 = 42)"

# ── 2. Changing module_extensions must change the graph ────────────────────
printf 'export module gshape.extra;\nexport auto extra() -> int { return 2; }\n' > src/extra.ccm
cat > mcpp.toml <<'EOF'
[package]
name    = "gshape"
version = "0.1.0"

[build]
module_extensions = [".ixx", ".ccm"]
EOF

"$MCPP" build > b3.log 2>&1 || { cat b3.log; echo "FAIL: build after key change"; exit 1; }
FP_AFTER="$(fingerprint)"

[[ "$FP_BEFORE" == "$FP_AFTER" ]] || {
    echo "FAIL: module_extensions moved the build directory ($FP_BEFORE -> $FP_AFTER)"
    echo "      (a package attribute entered the configuration's name)"
    exit 1; }
ninja_file="$(find target -path "*/$FP_AFTER/build.ninja" | head -1)"
grep -q 'gshape\.extra' "$ninja_file" || {
    echo "FAIL: the .ccm named by module_extensions is not a module interface of the graph"
    exit 1; }
echo "  ok: the key reaches the graph in the same directory ($FP_AFTER)"

# ── 3. A dead entry is reported, not silently ignored ──────────────────────
#
# Otherwise a typo (".ixxx") is indistinguishable from "this project has none
# yet": the build succeeds and the key does nothing.
cat > mcpp.toml <<'EOF'
[package]
name    = "gshape"
version = "0.1.0"

[build]
module_extensions = [".ixx", ".nosuchext"]
EOF
"$MCPP" build > b4.log 2>&1 || { cat b4.log; echo "FAIL: build with a dead entry"; exit 1; }
grep -q "nosuchext" b4.log || {
    cat b4.log; echo "FAIL: a module_extensions entry matching nothing was not reported"; exit 1; }
echo "  ok: a dead module_extensions entry is reported"

# ── 4. A reserved extension is a hard error, not a warning ─────────────────
#
# Claiming `.c` would route C files to the C++ module rule and fail somewhere
# that names neither the file nor the key.
cat > mcpp.toml <<'EOF'
[package]
name    = "gshape"
version = "0.1.0"

[build]
module_extensions = [".c"]
EOF
if "$MCPP" build > b5.log 2>&1; then
    cat b5.log; echo "FAIL: [build] module_extensions = [\".c\"] was accepted"; exit 1
fi
grep -q "module_extensions" b5.log || {
    cat b5.log; echo "FAIL: the error does not name the offending key"; exit 1; }
echo "  ok: claiming a non-module extension is refused, and the error names the key"

echo "OK"
