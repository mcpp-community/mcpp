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
#    ⚠️ The edit below adds a real `import`. Do NOT reduce it to `touch`: an
#    mtime-only change is exactly what a correct implementation is also
#    allowed to ignore, so a touch-based test can pass with the bug present.
#    ⚠️ And do NOT delete artifacts to force a rebuild: ninja then fails, the
#    failure is read as a stale-graph signature, and the fast path falls back
#    to a full prepare for the wrong reason — the assertion below would hold
#    while proving nothing.
#
#  * Part 2 — the key must reach the fingerprint.
#
#    `module_extensions` decides which units emit a BMI and which objects link
#    unconditionally, i.e. it is a build VARIANT. mcpp.toml's mtime alone only
#    protects the fast path inside one output dir; it does not stop a BMI
#    cache entry built under one classification from being served under
#    another. Changing the key must land in a different `target/<triple>/<fp>/`.
#
#    (Contrast `[build] build_program_timeout`, which is deliberately NOT
#    fingerprinted — it changes no edge, and folding it in would make raising a
#    timeout rebuild the whole project.)
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

fp_dir() { find target -name build.ninja -printf '%h\n' | head -1; }

# ── 0. First build, then confirm the fast path actually engages ────────────
"$MCPP" build > b0.log 2>&1 || { cat b0.log; echo "FAIL: first build"; exit 1; }
FP_BEFORE="$(fp_dir)"

"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "FAIL: no-change build"; exit 1; }
grep -q "Compiling" b1.log && {
    cat b1.log; echo "FAIL: fast path did not engage — the rest proves nothing"; exit 1; }
echo "  ok: fast path engages on a no-change build"

# ── 1. A NEW import inside the .ixx must invalidate the graph ──────────────
printf 'export module gshape.face;\nimport std;\nimport gshape.helper;\nexport auto face() -> int { return helper() + 1; }\n' > src/face.ixx

"$MCPP" build > b2.log 2>&1 || { cat b2.log; echo "FAIL: build after .ixx edit"; exit 1; }
grep -q "Compiling" b2.log || {
    cat b2.log
    echo "FAIL: editing a .ixx did not invalidate the fast path"
    echo "      (the freshness sweep is not classifying it as a graph-shape input)"
    exit 1; }
echo "  ok: a new import inside a .ixx forces a full prepare"

out="$("$MCPP" run 2>&1)"
[[ "$out" == *"42"* ]] || { echo "FAIL: expected 42 (41+1), got: $out"; exit 1; }
echo "  ok: the new dependency edge is real (41+1 = 42)"

# ── 2. Changing module_extensions must change the fingerprint ──────────────
cat > mcpp.toml <<'EOF'
[package]
name    = "gshape"
version = "0.1.0"

[build]
module_extensions = [".ixx", ".ccm"]
EOF

"$MCPP" build > b3.log 2>&1 || { cat b3.log; echo "FAIL: build after key change"; exit 1; }
FP_AFTER="$(fp_dir)"

[[ "$FP_BEFORE" != "$FP_AFTER" ]] || {
    echo "FAIL: module_extensions changed but the output dir did not"
    echo "      before=$FP_BEFORE after=$FP_AFTER"
    echo "      (the key is missing from the canonical compile-flags string)"
    exit 1; }
echo "  ok: the key is fingerprinted ($(basename "$FP_BEFORE") -> $(basename "$FP_AFTER"))"

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
