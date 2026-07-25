#!/usr/bin/env bash
# requires:
# 161_xpkg_name_form.sh — SPEC-001 §3.2: `package.name` is a SINGLE ATOMIC
# SEGMENT; all hierarchy belongs in `package.namespace`.
#
#     ✅ namespace = "chriskohlhoff", name = "asio"
#     ✅ namespace = "mcpplibs.capi", name = "lua"
#     ✅ namespace = "compat",        name = "compat.zlib"   (legacy FQN, kept working)
#     ❌ namespace = "mcpplibs",      name = "capi.lua"       (short name has a dot)
#
# Why the last one is rejected rather than reinterpreted: identity is the pair
# (namespace, name). A `name` carrying dots the namespace does not account for
# describes a package whose namespace nobody declared. mcpp used to split such a
# name on its LAST dot and silently invent `(mcpplibs.capi, lua)` — a namespace
# absent from the descriptor. It now refuses to guess.
#
# This test was the inverse before mcpp 0.0.106 (it asserted that the short form
# was the violation). See docs/spec/package-identity.md §3.2 and mcpp#278.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mk() {  # mk <file> <namespace> <name>
    cat > "$1" <<EOF
package = {
    spec = "1", namespace = "$2", name = "$3",
    xpm = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = { schema = "0.1", sources = { "*.cpp" } },
}
EOF
}

# ── 1. canonical short-name form is accepted ────────────────────────
mk short.lua "chriskohlhoff" "asio"
"$MCPP" xpkg parse short.lua > /dev/null

# ── 2. hierarchical namespace with an atomic short name ─────────────
mk nested.lua "mcpplibs.capi" "lua"
"$MCPP" xpkg parse nested.lua > /dev/null

# ── 3. legacy fully-qualified spelling still accepted ───────────────
# Descriptors written before SPEC-001 repeat the namespace inside `name`.
# They resolve to the same identity and stay installable, so they must not
# become errors — that would break every currently published package.
mk legacy.lua "compat" "compat.zlib"
"$MCPP" xpkg parse legacy.lua > /dev/null

# ── 4. non-atomic short name is rejected, with the fix spelled out ──
mk bad.lua "mcpplibs" "capi.lua"
if "$MCPP" xpkg parse bad.lua > /dev/null 2>&1; then
    echo "FAIL: a short name containing '.' must be rejected"; exit 1
fi
"$MCPP" xpkg parse bad.lua 2>&1 | tee bad.err
grep -q "single atomic segment" bad.err
# The diagnostic must name BOTH corrected fields, not just complain.
grep -q 'mcpplibs.capi' bad.err
grep -q '"lua"' bad.err

# ── 5. dotted name with no namespace at all is rejected ─────────────
# It would resolve into a namespace written down nowhere.
mk nons.lua "" "mcpplibs.capi.lua"
if "$MCPP" xpkg parse nons.lua > /dev/null 2>&1; then
    echo "FAIL: dotted name without a namespace must be rejected"; exit 1
fi

# ── 6. --json carries the violation machine-readably (index CI) ─────
if "$MCPP" xpkg parse --json bad.lua > bad.json 2>/dev/null; then
    echo "FAIL: --json must still exit non-zero on violation"; exit 1
fi
python3 - <<'PY'
import json
j = json.load(open("bad.json"))
assert "error" in j, j
assert "atomic" in j["error"], j
PY

# ── 7. runtime fail-fast: a descriptor that skipped index CI ────────
# A `[indices]` path index bypasses lint entirely, so the same predicate has
# to guard the install path — and must fire BEFORE any download.
mkdir -p idx/pkgs/d
cat > idx/pkgs/d/thing.lua <<'EOF'
package = {
    spec = "1", namespace = "demo", name = "sub.thing",
    xpm = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } },
            macosx = { ["1.0.0"] = { url = "u", sha256 = "h" } },
            windows = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = { schema = "0.1", sources = { "*.cpp" } },
}
EOF

mkdir -p app/src && cd app
cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[indices]
demo = { path = "../idx" }

[dependencies.demo]
"sub.thing" = "1.0.0"
EOF
echo 'int main() { return 0; }' > src/main.cpp

if "$MCPP" build > build.out 2>&1; then
    echo "FAIL: non-atomic short name must not build"; cat build.out; exit 1
fi
grep -q "single atomic segment" build.out || { cat build.out; exit 1; }
if grep -q "Downloading" build.out; then
    echo "FAIL: must fail before any download"; cat build.out; exit 1
fi

echo "PASS 161_xpkg_name_form"
